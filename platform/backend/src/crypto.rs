use aes_gcm::{
    aead::{Aead, KeyInit, Payload},
    Aes256Gcm, Nonce,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use hmac::{Hmac, Mac};
use rand::{rngs::OsRng, RngCore};
use serde::Serialize;
use sha2::{Digest, Sha256};
use uuid::Uuid;
use zeroize::{Zeroize, Zeroizing};

use crate::{
    error::ApiError,
    wire::{
        DeviceEnvelope, RemoteAction, ValidatedEnvelope, ValidatedRemoteAction,
        ACTION_TRANSCRIPT_DOMAIN, DEVICE_TRANSCRIPT_DOMAIN, REMOTE_ACTION_SCHEMA,
    },
};

type HmacSha256 = Hmac<Sha256>;

const OIDC_SUBJECT_DIGEST_DOMAIN: &[u8] = b"kitsu.oidc-subject-tombstone.v1\0";

pub struct EncryptedBytes {
    pub nonce: [u8; 12],
    pub ciphertext: Vec<u8>,
}

/// Stable, one-way identifier for a precise OpenID Connect issuer/subject
/// pair. Length-prefixing prevents ambiguous concatenations.
pub fn oidc_subject_digest(issuer: &str, subject: &str) -> [u8; 32] {
    let mut hash = Sha256::new();
    hash.update(OIDC_SUBJECT_DIGEST_DOMAIN);
    hash.update((issuer.len() as u64).to_be_bytes());
    hash.update(issuer.as_bytes());
    hash.update((subject.len() as u64).to_be_bytes());
    hash.update(subject.as_bytes());
    hash.finalize().into()
}

pub fn contact_source_digest(key: &[u8; 32], address: &str) -> [u8; 32] {
    let mut mac = <HmacSha256 as Mac>::new_from_slice(key).expect("HMAC accepts a 32-byte key");
    mac.update(b"kitsu.public-contact-source.v1\0");
    mac.update(address.as_bytes());
    mac.finalize().into_bytes().into()
}

pub fn verify_device_hmac(
    envelope: &DeviceEnvelope,
    validated: &ValidatedEnvelope,
    secret: &[u8; 32],
) -> Result<(), ApiError> {
    let transcript = device_transcript(envelope, validated)?;
    let mut mac = <HmacSha256 as Mac>::new_from_slice(secret)
        .map_err(|_| ApiError::Invalid("invalid companion key"))?;
    mac.update(&transcript);
    mac.verify_slice(&validated.signature)
        .map_err(|_| ApiError::Unauthorized)
}

pub fn device_transcript(
    envelope: &DeviceEnvelope,
    validated: &ValidatedEnvelope,
) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    let payload_type = envelope.payload_type.as_bytes();
    let payload_type_len = u16::try_from(payload_type.len())
        .map_err(|_| ApiError::Invalid("payload type is too long"))?;
    let payload_len = u32::try_from(validated.payload.len())
        .map_err(|_| ApiError::Invalid("payload is too large"))?;

    let mut transcript = Zeroizing::new(Vec::with_capacity(
        DEVICE_TRANSCRIPT_DOMAIN.len()
            + 16
            + 16
            + 8
            + 8
            + 16
            + 16
            + 4
            + 2
            + payload_type.len()
            + 4
            + validated.payload.len(),
    ));
    transcript.extend_from_slice(DEVICE_TRANSCRIPT_DOMAIN);
    transcript.extend_from_slice(envelope.companion_id.as_bytes());
    transcript.extend_from_slice(envelope.gateway_id.as_bytes());
    transcript.extend_from_slice(&(validated.sequence as u64).to_be_bytes());
    transcript.extend_from_slice(&validated.issued_epoch.to_be_bytes());
    transcript.extend_from_slice(&validated.nonce);
    transcript.extend_from_slice(envelope.request_id.as_bytes());
    transcript.extend_from_slice(&envelope.key_version.to_be_bytes());
    transcript.extend_from_slice(&payload_type_len.to_be_bytes());
    transcript.extend_from_slice(payload_type);
    transcript.extend_from_slice(&payload_len.to_be_bytes());
    transcript.extend_from_slice(&validated.payload);
    Ok(transcript)
}

#[allow(clippy::too_many_arguments)]
pub fn sign_remote_action(
    action_id: Uuid,
    companion_id: Uuid,
    key_version: u32,
    action_type: &str,
    created_epoch: i64,
    expires_epoch: i64,
    params: &[u8],
    secret: &[u8; 32],
) -> Result<RemoteAction, ApiError> {
    sign_remote_action_with_nonce(
        action_id,
        companion_id,
        key_version,
        random_array(),
        action_type,
        created_epoch,
        expires_epoch,
        params,
        secret,
    )
}

#[allow(clippy::too_many_arguments)]
pub fn sign_remote_action_with_nonce(
    action_id: Uuid,
    companion_id: Uuid,
    key_version: u32,
    nonce: [u8; 16],
    action_type: &str,
    created_epoch: i64,
    expires_epoch: i64,
    params: &[u8],
    secret: &[u8; 32],
) -> Result<RemoteAction, ApiError> {
    let mut action = RemoteAction {
        schema: REMOTE_ACTION_SCHEMA.to_owned(),
        action_id,
        companion_id,
        key_version,
        nonce_b64: URL_SAFE_NO_PAD.encode(nonce),
        action_type: action_type.to_owned(),
        created_epoch: created_epoch.to_string(),
        expires_epoch: expires_epoch.to_string(),
        params_b64: URL_SAFE_NO_PAD.encode(params),
        signature_b64: URL_SAFE_NO_PAD.encode([0_u8; 32]),
    };
    let validated = action.validate_wrapper()?;
    let transcript = action_transcript(&action, &validated)?;
    let mut mac = <HmacSha256 as Mac>::new_from_slice(secret)
        .map_err(|_| ApiError::Invalid("invalid companion key"))?;
    mac.update(&transcript);
    action.signature_b64 = URL_SAFE_NO_PAD.encode(mac.finalize().into_bytes());
    Ok(action)
}

pub fn verify_remote_action_hmac(
    action: &RemoteAction,
    validated: &ValidatedRemoteAction,
    secret: &[u8; 32],
) -> Result<(), ApiError> {
    let transcript = action_transcript(action, validated)?;
    let mut mac = <HmacSha256 as Mac>::new_from_slice(secret)
        .map_err(|_| ApiError::Invalid("invalid companion key"))?;
    mac.update(&transcript);
    mac.verify_slice(&validated.signature)
        .map_err(|_| ApiError::Unauthorized)
}

pub fn action_transcript(
    action: &RemoteAction,
    validated: &ValidatedRemoteAction,
) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    let action_type = action.action_type.as_bytes();
    let action_type_len = u16::try_from(action_type.len())
        .map_err(|_| ApiError::Invalid("action type is too long"))?;
    let params_len = u32::try_from(validated.params.len())
        .map_err(|_| ApiError::Invalid("action parameters are too large"))?;
    let mut transcript = Zeroizing::new(Vec::with_capacity(
        ACTION_TRANSCRIPT_DOMAIN.len()
            + 16
            + 16
            + 4
            + 16
            + 8
            + 8
            + 2
            + action_type.len()
            + 4
            + validated.params.len(),
    ));
    transcript.extend_from_slice(ACTION_TRANSCRIPT_DOMAIN);
    transcript.extend_from_slice(action.action_id.as_bytes());
    transcript.extend_from_slice(action.companion_id.as_bytes());
    transcript.extend_from_slice(&validated.key_version.to_be_bytes());
    transcript.extend_from_slice(&validated.nonce);
    transcript.extend_from_slice(&validated.created_epoch.to_be_bytes());
    transcript.extend_from_slice(&validated.expires_epoch.to_be_bytes());
    transcript.extend_from_slice(&action_type_len.to_be_bytes());
    transcript.extend_from_slice(action_type);
    transcript.extend_from_slice(&params_len.to_be_bytes());
    transcript.extend_from_slice(&validated.params);
    Ok(transcript)
}

pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

pub fn sha256_text(value: &str) -> [u8; 32] {
    sha256(value.as_bytes())
}

pub fn canonical_request_hash<T: Serialize>(value: &T) -> Result<[u8; 32], ApiError> {
    let canonical = serde_jcs::to_vec(value).map_err(ApiError::internal)?;
    Ok(sha256(&canonical))
}

pub fn random_token(bytes: usize) -> Zeroizing<String> {
    let mut raw = Zeroizing::new(vec![0_u8; bytes]);
    OsRng.fill_bytes(&mut raw);
    Zeroizing::new(URL_SAFE_NO_PAD.encode(raw.as_slice()))
}

pub fn random_array<const N: usize>() -> [u8; N] {
    let mut value = [0_u8; N];
    OsRng.fill_bytes(&mut value);
    value
}

pub fn encrypt_companion_secret(
    companion_id: Uuid,
    key_version: u32,
    dek: &[u8; 32],
    secret: &[u8; 32],
) -> Result<EncryptedBytes, ApiError> {
    encrypt_bytes(dek, secret, &companion_aad(companion_id, key_version))
}

pub fn decrypt_companion_secret(
    companion_id: Uuid,
    key_version: u32,
    dek: &[u8; 32],
    encrypted: &EncryptedBytes,
) -> Result<Zeroizing<[u8; 32]>, ApiError> {
    let mut plaintext = decrypt_bytes(dek, encrypted, &companion_aad(companion_id, key_version))?;
    if plaintext.len() != 32 {
        plaintext.zeroize();
        return Err(ApiError::Unavailable);
    }
    let mut secret = Zeroizing::new([0_u8; 32]);
    secret.copy_from_slice(&plaintext);
    plaintext.zeroize();
    Ok(secret)
}

pub fn encrypt_browser_state(
    key: &[u8; 32],
    attempt_id: Uuid,
    verifier: &[u8],
) -> Result<EncryptedBytes, ApiError> {
    encrypt_bytes(key, verifier, browser_aad(attempt_id).as_bytes())
}

pub fn decrypt_browser_state(
    key: &[u8; 32],
    attempt_id: Uuid,
    encrypted: &EncryptedBytes,
) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    decrypt_bytes(key, encrypted, browser_aad(attempt_id).as_bytes())
}

fn encrypt_bytes(key: &[u8; 32], bytes: &[u8], aad: &[u8]) -> Result<EncryptedBytes, ApiError> {
    let cipher = Aes256Gcm::new_from_slice(key).map_err(|_| ApiError::Unavailable)?;
    let nonce = random_array::<12>();
    let ciphertext = cipher
        .encrypt(Nonce::from_slice(&nonce), Payload { msg: bytes, aad })
        .map_err(|_| ApiError::Unavailable)?;
    Ok(EncryptedBytes { nonce, ciphertext })
}

fn decrypt_bytes(
    key: &[u8; 32],
    encrypted: &EncryptedBytes,
    aad: &[u8],
) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    let cipher = Aes256Gcm::new_from_slice(key).map_err(|_| ApiError::Unavailable)?;
    let plaintext = cipher
        .decrypt(
            Nonce::from_slice(&encrypted.nonce),
            Payload {
                msg: encrypted.ciphertext.as_slice(),
                aad,
            },
        )
        .map_err(|_| ApiError::Unavailable)?;
    Ok(Zeroizing::new(plaintext))
}

fn companion_aad(companion_id: Uuid, key_version: u32) -> Vec<u8> {
    let mut aad = Vec::with_capacity(16 + 4 + 16);
    aad.extend_from_slice(b"KITSU-SECRET-1\0");
    aad.extend_from_slice(companion_id.as_bytes());
    aad.extend_from_slice(&key_version.to_be_bytes());
    aad
}

fn browser_aad(attempt_id: Uuid) -> String {
    format!("KITSU-BROWSER-STATE-1:{attempt_id}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::DeviceEnvelope;

    #[test]
    fn transcript_has_fixed_network_order_layout() {
        let envelope = DeviceEnvelope {
            schema: "kitsu.device-envelope.v1".into(),
            companion_id: Uuid::parse_str("00112233-4455-6677-8899-aabbccddeeff").unwrap(),
            gateway_id: Uuid::parse_str("ffeeddcc-bbaa-9988-7766-554433221100").unwrap(),
            sequence: "42".into(),
            issued_epoch: "0".into(),
            nonce_b64: URL_SAFE_NO_PAD.encode([0x11; 16]),
            request_id: Uuid::parse_str("10213243-5465-7687-98a9-bacbdcedfe0f").unwrap(),
            key_version: 1,
            payload_type: "heartbeat".into(),
            payload_b64: URL_SAFE_NO_PAD.encode(br#"{"type":"heartbeat","uptime_ms":7}"#),
            signature_b64: URL_SAFE_NO_PAD.encode([0_u8; 32]),
        };
        let validated = envelope.validate_wrapper().unwrap();
        let transcript = device_transcript(&envelope, &validated).unwrap();
        assert!(transcript.starts_with(b"KITSU-DEVICE-1\0\x00\x11\x22\x33"));
        assert_eq!(&transcript[47..55], &42_u64.to_be_bytes());
    }

    #[test]
    fn companion_ciphertext_is_bound_to_identity_and_version() {
        let dek = [7_u8; 32];
        let secret = [9_u8; 32];
        let companion = Uuid::new_v4();
        let encrypted = encrypt_companion_secret(companion, 1, &dek, &secret).unwrap();
        assert_eq!(encrypted.ciphertext.len(), 48);
        let decoded = decrypt_companion_secret(companion, 1, &dek, &encrypted).unwrap();
        assert_eq!(&*decoded, &secret);
        assert!(decrypt_companion_secret(companion, 2, &dek, &encrypted).is_err());
        assert!(decrypt_companion_secret(Uuid::new_v4(), 1, &dek, &encrypted).is_err());
    }

    #[test]
    fn remote_action_signature_vector_uses_rfc4122_uuid_order() {
        let action_id = Uuid::parse_str("00112233-4455-6677-8899-aabbccddeeff").unwrap();
        let companion_id = Uuid::parse_str("ffeeddcc-bbaa-9988-7766-554433221100").unwrap();
        let nonce: [u8; 16] = core::array::from_fn(|index| index as u8);
        let secret: [u8; 32] = core::array::from_fn(|index| index as u8);
        let params = br#"{"gesture":"ear-scratch"}"#;
        let action = sign_remote_action_with_nonce(
            action_id,
            companion_id,
            0x0102_0304,
            nonce,
            "companion.pet",
            1_800_000_000,
            1_800_000_060,
            params,
            &secret,
        )
        .unwrap();
        assert_eq!(
            action.signature_b64,
            "Ba24Hq65ANHWNZ3ZDYkfhVQ1KorRFzfLgmxPEBTNdQ4"
        );
        let validated = action.validate_wrapper().unwrap();
        verify_remote_action_hmac(&action, &validated, &secret).unwrap();
        let transcript = action_transcript(&action, &validated).unwrap();
        assert_eq!(&transcript[15..31], action_id.as_bytes());
        assert_eq!(&transcript[31..47], companion_id.as_bytes());
        assert_eq!(transcript.len(), 127);
    }

    #[test]
    fn remote_action_signature_is_sensitive_to_exact_parameter_bytes() {
        let secret = [0x5a; 32];
        let mut action = sign_remote_action_with_nonce(
            Uuid::from_u128(2),
            Uuid::from_u128(1),
            1,
            [7; 16],
            "companion.pet",
            1_800_000_000,
            1_800_000_060,
            br#"{}"#,
            &secret,
        )
        .unwrap();
        action.params_b64 = URL_SAFE_NO_PAD.encode(br#"{ }"#);
        let changed = action.validate_wrapper().unwrap();
        assert!(verify_remote_action_hmac(&action, &changed, &secret).is_err());
    }

    #[test]
    fn remote_action_rejects_invalid_expiry_and_retries_byte_identically() {
        let args = (
            Uuid::from_u128(2),
            Uuid::from_u128(3),
            4,
            [9; 16],
            "companion.feed",
            1_800_000_000,
            1_800_000_030,
            br#"{}"#.as_slice(),
            [0x33; 32],
        );
        let first = sign_remote_action_with_nonce(
            args.0, args.1, args.2, args.3, args.4, args.5, args.6, args.7, &args.8,
        )
        .unwrap();
        let retry = sign_remote_action_with_nonce(
            args.0, args.1, args.2, args.3, args.4, args.5, args.6, args.7, &args.8,
        )
        .unwrap();
        assert_eq!(
            serde_json::to_vec(&first).unwrap(),
            serde_json::to_vec(&retry).unwrap()
        );

        let mut expired = first;
        expired.expires_epoch = expired.created_epoch.clone();
        assert!(expired.validate_wrapper().is_err());
        expired.expires_epoch = "1800086401".into();
        assert!(expired.validate_wrapper().is_err());
    }
}
