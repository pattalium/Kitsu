//! Enrollment cryptography and certificate validation.
//!
//! The device proof and HPKE formats in this module are wire contracts.  They
//! intentionally operate on fixed binary transcripts; JSON is only transport.

use aes_gcm::{
    aead::{Aead, KeyInit, Payload},
    Aes256Gcm, Nonce,
};
use chrono::{DateTime, Utc};
use hmac::{Hmac, Mac};
use ring::{
    agreement,
    rand::SystemRandom,
    signature::{UnparsedPublicKey, ECDSA_P256_SHA256_FIXED},
};
use sha2::Sha256;
use uuid::Uuid;
use x509_parser::{
    extensions::{GeneralName, ParsedExtension},
    prelude::{FromDer, X509Certificate, X509CertificationRequest},
};
use zeroize::{Zeroize, Zeroizing};

use crate::{crypto::sha256, error::ApiError};

pub const DEVICE_PROOF_DOMAIN: &[u8] = b"KITSU-ENROLL-DEVICE-1\0";
pub const ENROLL_SECRET_DOMAIN: &[u8] = b"KITSU-ENROLL-SECRET-1\0";
pub const HPKE_SUITE_NAME: &str = "DHKEM(P-256,HKDF-SHA256)/HKDF-SHA256/AES-256-GCM";
const KEM_SUITE_ID: &[u8] = b"KEM\x00\x10";
const HPKE_SUITE_ID: &[u8] = b"HPKE\x00\x10\x00\x01\x00\x02";
const HPKE_VERSION_LABEL: &[u8] = b"HPKE-v1";

#[derive(Clone)]
pub struct ValidatedCsr {
    pub der: Vec<u8>,
    pub public_key: [u8; 65],
    pub spki_sha256: [u8; 32],
}

#[derive(Clone)]
pub struct RawIssuedCertificate {
    pub leaf_der: Vec<u8>,
    pub chain_der: Vec<Vec<u8>>,
    /// Opaque provider identifier (for example the AWS Private CA certificate
    /// ARN).  It is audit metadata, never an authorization input.
    pub provider_id: String,
}

#[derive(Clone)]
pub struct ValidatedCertificate {
    pub leaf_der: Vec<u8>,
    pub chain_der: Vec<Vec<u8>>,
    pub provider_id: String,
    pub serial_hex: String,
    pub fingerprint_sha256: [u8; 32],
    pub subject_public_key_sha256: [u8; 32],
    pub san_uri: String,
    pub valid_after: DateTime<Utc>,
    pub valid_until: DateTime<Utc>,
}

pub struct SealedSecret {
    pub enc: [u8; 65],
    pub ciphertext: Zeroizing<Vec<u8>>,
}

pub fn validate_p256_csr(der: &[u8]) -> Result<ValidatedCsr, ApiError> {
    if der.is_empty() || der.len() > 4_096 {
        return Err(ApiError::Invalid("invalid PKCS#10 CSR"));
    }
    let (remaining, csr) = X509CertificationRequest::from_der(der)
        .map_err(|_| ApiError::Invalid("invalid PKCS#10 CSR"))?;
    if !remaining.is_empty()
        || csr.as_raw().len() != der.len()
        || csr.certification_request_info.version.0 != 0
        || csr.signature_algorithm.algorithm.to_id_string() != "1.2.840.10045.4.3.2"
    {
        return Err(ApiError::Invalid("invalid P-256 PKCS#10 CSR"));
    }
    let spki = &csr.certification_request_info.subject_pki;
    if spki.algorithm.algorithm.to_id_string() != "1.2.840.10045.2.1"
        || spki
            .algorithm
            .parameters
            .as_ref()
            .and_then(|value| value.as_oid().ok())
            .map(|oid| oid.to_id_string())
            .as_deref()
            != Some("1.2.840.10045.3.1.7")
        || spki.subject_public_key.unused_bits != 0
        || spki.subject_public_key.data.len() != 65
        || spki.subject_public_key.data.first() != Some(&0x04)
    {
        return Err(ApiError::Invalid("CSR key must be P-256"));
    }
    csr.verify_signature()
        .map_err(|_| ApiError::Invalid("invalid CSR signature"))?;

    let public_key: [u8; 65] = spki
        .subject_public_key
        .data
        .as_ref()
        .try_into()
        .map_err(|_| ApiError::Invalid("CSR key must be P-256"))?;
    // Older Kitsu firmware redundantly requests digitalSignature. The service
    // still owns the certificate profile; reject every other requested value.
    if let Some(mut items) = csr.requested_extensions() {
        let allowed = match (items.next(), items.next()) {
            (None, None) => true,
            (Some(ParsedExtension::KeyUsage(usage)), None) => usage.flags == 1,
            _ => false,
        };
        if !allowed {
            return Err(ApiError::Invalid("CSR extensions are not accepted"));
        }
    }
    Ok(ValidatedCsr {
        der: der.to_vec(),
        public_key,
        spki_sha256: sha256(spki.raw),
    })
}

pub fn device_proof_transcript(
    enrollment_id: Uuid,
    hardware_uid: &str,
    csr_der: &[u8],
    hpke_recipient: &[u8; 65],
    nonce: &[u8; 16],
) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    device_proof_transcript_from_csr_sha256(
        enrollment_id,
        hardware_uid,
        &sha256(csr_der),
        hpke_recipient,
        nonce,
    )
}

fn device_proof_transcript_from_csr_sha256(
    enrollment_id: Uuid,
    hardware_uid: &str,
    csr_sha256: &[u8; 32],
    hpke_recipient: &[u8; 65],
    nonce: &[u8; 16],
) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    if enrollment_id.is_nil() {
        return Err(ApiError::Invalid("invalid enrollment ID"));
    }
    let hardware = hardware_uid.as_bytes();
    let hardware_len =
        u16::try_from(hardware.len()).map_err(|_| ApiError::Invalid("hardware UID is too long"))?;
    let mut transcript = Zeroizing::new(Vec::with_capacity(
        DEVICE_PROOF_DOMAIN.len() + 16 + 2 + hardware.len() + 32 + 65 + 16,
    ));
    transcript.extend_from_slice(DEVICE_PROOF_DOMAIN);
    transcript.extend_from_slice(enrollment_id.as_bytes());
    transcript.extend_from_slice(&hardware_len.to_be_bytes());
    transcript.extend_from_slice(hardware);
    transcript.extend_from_slice(csr_sha256);
    transcript.extend_from_slice(hpke_recipient);
    transcript.extend_from_slice(nonce);
    Ok(transcript)
}

pub fn verify_device_proof(
    csr: &ValidatedCsr,
    transcript: &[u8],
    proof_p1363: &[u8; 64],
) -> Result<(), ApiError> {
    UnparsedPublicKey::new(&ECDSA_P256_SHA256_FIXED, csr.public_key)
        .verify(transcript, proof_p1363)
        .map_err(|_| ApiError::Unauthorized)
}

/// Validate the exact uncompressed SEC1 P-256 recipient point before a claim
/// is bound or any certificate-provider work begins. Ring performs public-key
/// validation as part of agreement, so a throwaway local scalar is sufficient
/// and no derived bytes need to escape this function.
pub fn validate_hpke_recipient(recipient_public_key: &[u8; 65]) -> Result<(), ApiError> {
    if recipient_public_key.first() != Some(&0x04) {
        return Err(ApiError::Invalid("invalid HPKE recipient key"));
    }
    let ephemeral =
        agreement::EphemeralPrivateKey::generate(&agreement::ECDH_P256, &SystemRandom::new())
            .map_err(|_| ApiError::Unavailable)?;
    let peer = agreement::UnparsedPublicKey::new(&agreement::ECDH_P256, recipient_public_key);
    agreement::agree_ephemeral(ephemeral, &peer, |_| ())
        .map_err(|_| ApiError::Invalid("invalid HPKE recipient key"))
}

pub fn enrollment_secret_context(
    enrollment_id: Uuid,
    companion_id: Uuid,
    gateway_id: Uuid,
    key_version: u32,
) -> Result<[u8; 74], ApiError> {
    if enrollment_id.is_nil() || companion_id.is_nil() || gateway_id.is_nil() || key_version == 0 {
        return Err(ApiError::Invalid("invalid enrollment context"));
    }
    debug_assert_eq!(ENROLL_SECRET_DOMAIN.len(), 22);
    let mut context = [0_u8; 74];
    let mut cursor = 0;
    for part in [
        ENROLL_SECRET_DOMAIN,
        enrollment_id.as_bytes(),
        companion_id.as_bytes(),
        gateway_id.as_bytes(),
        key_version.to_be_bytes().as_slice(),
    ] {
        context[cursor..cursor + part.len()].copy_from_slice(part);
        cursor += part.len();
    }
    debug_assert_eq!(cursor, context.len());
    Ok(context)
}

/// RFC 9180 base-mode seal with suite KEM 0x0010, KDF 0x0001, AEAD 0x0002.
pub fn seal_enrollment_secret(
    recipient_public_key: &[u8; 65],
    context: &[u8; 74],
    secret: &[u8; 32],
) -> Result<SealedSecret, ApiError> {
    let rng = SystemRandom::new();
    let ephemeral = agreement::EphemeralPrivateKey::generate(&agreement::ECDH_P256, &rng)
        .map_err(|_| ApiError::Unavailable)?;
    let public = ephemeral
        .compute_public_key()
        .map_err(|_| ApiError::Unavailable)?;
    let enc: [u8; 65] = public
        .as_ref()
        .try_into()
        .map_err(|_| ApiError::Unavailable)?;
    let peer = agreement::UnparsedPublicKey::new(&agreement::ECDH_P256, recipient_public_key);
    agreement::agree_ephemeral(ephemeral, &peer, |dh| {
        seal_enrollment_secret_from_dh(&enc, recipient_public_key, dh, context, secret)
    })
    .map_err(|_| ApiError::Invalid("invalid HPKE recipient key"))?
}

fn seal_enrollment_secret_from_dh(
    enc: &[u8; 65],
    recipient_public_key: &[u8; 65],
    dh: &[u8],
    context: &[u8; 74],
    secret_plaintext: &[u8; 32],
) -> Result<SealedSecret, ApiError> {
    if dh.len() != 32 {
        return Err(ApiError::Unavailable);
    }
    let mut kem_context = Zeroizing::new(Vec::with_capacity(130));
    kem_context.extend_from_slice(enc);
    kem_context.extend_from_slice(recipient_public_key);
    let eae_prk = labeled_extract(KEM_SUITE_ID, &[], b"eae_prk", dh)?;
    let shared_secret = labeled_expand(
        KEM_SUITE_ID,
        &eae_prk[..],
        b"shared_secret",
        &kem_context,
        32,
    )?;

    let psk_id_hash = labeled_extract(HPKE_SUITE_ID, &[], b"psk_id_hash", &[])?;
    let info_hash = labeled_extract(HPKE_SUITE_ID, &[], b"info_hash", context)?;
    let mut schedule_context = Zeroizing::new(Vec::with_capacity(65));
    schedule_context.push(0); // mode_base
    schedule_context.extend_from_slice(&psk_id_hash[..]);
    schedule_context.extend_from_slice(&info_hash[..]);
    let schedule_secret = labeled_extract(HPKE_SUITE_ID, &shared_secret, b"secret", &[])?;
    let mut key = labeled_expand(
        HPKE_SUITE_ID,
        &schedule_secret[..],
        b"key",
        &schedule_context,
        32,
    )?;
    let mut nonce = labeled_expand(
        HPKE_SUITE_ID,
        &schedule_secret[..],
        b"base_nonce",
        &schedule_context,
        12,
    )?;
    let cipher = Aes256Gcm::new_from_slice(&key).map_err(|_| ApiError::Unavailable)?;
    let ciphertext = cipher
        .encrypt(
            Nonce::from_slice(&nonce),
            Payload {
                msg: secret_plaintext,
                aad: context,
            },
        )
        .map_err(|_| ApiError::Unavailable)?;
    key.zeroize();
    nonce.zeroize();
    Ok(SealedSecret {
        enc: *enc,
        ciphertext: Zeroizing::new(ciphertext),
    })
}

fn labeled_extract(
    suite_id: &[u8],
    salt: &[u8],
    label: &[u8],
    input: &[u8],
) -> Result<Zeroizing<[u8; 32]>, ApiError> {
    let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(salt).map_err(|_| ApiError::Unavailable)?;
    mac.update(HPKE_VERSION_LABEL);
    mac.update(suite_id);
    mac.update(label);
    mac.update(input);
    Ok(Zeroizing::new(mac.finalize().into_bytes().into()))
}

fn labeled_expand(
    suite_id: &[u8],
    prk: &[u8],
    label: &[u8],
    info: &[u8],
    length: usize,
) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    let length_u16 = u16::try_from(length).map_err(|_| ApiError::Unavailable)?;
    let mut labeled_info = Zeroizing::new(Vec::with_capacity(
        2 + HPKE_VERSION_LABEL.len() + suite_id.len() + label.len() + info.len(),
    ));
    labeled_info.extend_from_slice(&length_u16.to_be_bytes());
    labeled_info.extend_from_slice(HPKE_VERSION_LABEL);
    labeled_info.extend_from_slice(suite_id);
    labeled_info.extend_from_slice(label);
    labeled_info.extend_from_slice(info);
    hkdf_expand(prk, &labeled_info, length)
}

fn hkdf_expand(prk: &[u8], info: &[u8], length: usize) -> Result<Zeroizing<Vec<u8>>, ApiError> {
    if length > 255 * 32 {
        return Err(ApiError::Unavailable);
    }
    let mut output = Zeroizing::new(Vec::with_capacity(length));
    let mut previous = Zeroizing::new(Vec::new());
    let blocks = length.div_ceil(32);
    for index in 1..=blocks {
        let mut mac =
            <Hmac<Sha256> as Mac>::new_from_slice(prk).map_err(|_| ApiError::Unavailable)?;
        mac.update(&previous);
        mac.update(info);
        mac.update(&[u8::try_from(index).map_err(|_| ApiError::Unavailable)?]);
        previous = Zeroizing::new(mac.finalize().into_bytes().to_vec());
        output.extend_from_slice(&previous);
    }
    output.truncate(length);
    Ok(output)
}

pub fn validate_issued_certificate(
    issued: RawIssuedCertificate,
    csr: &ValidatedCsr,
    expected_san_uri: &str,
    now: DateTime<Utc>,
) -> Result<ValidatedCertificate, ApiError> {
    if issued.leaf_der.is_empty()
        || issued.leaf_der.len() > 64 * 1024
        || issued.chain_der.is_empty()
        || issued.chain_der.len() > 8
        || issued
            .chain_der
            .iter()
            .any(|item| item.is_empty() || item.len() > 64 * 1024)
    {
        return Err(ApiError::Unavailable);
    }
    let (remaining, certificate) =
        X509Certificate::from_der(&issued.leaf_der).map_err(|_| ApiError::Unavailable)?;
    if !remaining.is_empty() {
        return Err(ApiError::Unavailable);
    }
    let spki = &certificate.tbs_certificate.subject_pki;
    if spki.algorithm.algorithm.to_id_string() != "1.2.840.10045.2.1"
        || spki
            .algorithm
            .parameters
            .as_ref()
            .and_then(|value| value.as_oid().ok())
            .map(|oid| oid.to_id_string())
            .as_deref()
            != Some("1.2.840.10045.3.1.7")
        || spki.subject_public_key.data.as_ref() != csr.public_key
    {
        return Err(ApiError::Unavailable);
    }
    let san = certificate
        .subject_alternative_name()
        .map_err(|_| ApiError::Unavailable)?
        .ok_or(ApiError::Unavailable)?;
    if san.value.general_names.as_slice() != [GeneralName::URI(expected_san_uri)] {
        return Err(ApiError::Unavailable);
    }
    let basic_constraints = certificate
        .basic_constraints()
        .map_err(|_| ApiError::Unavailable)?
        .ok_or(ApiError::Unavailable)?;
    if basic_constraints.value.ca {
        return Err(ApiError::Unavailable);
    }
    let key_usage = certificate
        .key_usage()
        .map_err(|_| ApiError::Unavailable)?
        .ok_or(ApiError::Unavailable)?;
    if !key_usage.value.digital_signature()
        || key_usage.value.key_cert_sign()
        || key_usage.value.crl_sign()
    {
        return Err(ApiError::Unavailable);
    }
    let extended = certificate
        .extended_key_usage()
        .map_err(|_| ApiError::Unavailable)?
        .ok_or(ApiError::Unavailable)?;
    if !extended.value.client_auth
        || extended.value.any
        || extended.value.server_auth
        || extended.value.code_signing
        || extended.value.email_protection
        || extended.value.time_stamping
        || extended.value.ocsp_signing
        || !extended.value.other.is_empty()
    {
        return Err(ApiError::Unavailable);
    }
    let valid_after = DateTime::from_timestamp(certificate.validity().not_before.timestamp(), 0)
        .ok_or(ApiError::Unavailable)?;
    let valid_until = DateTime::from_timestamp(certificate.validity().not_after.timestamp(), 0)
        .ok_or(ApiError::Unavailable)?;
    if valid_after > now + chrono::TimeDelta::minutes(5) || valid_until <= now {
        return Err(ApiError::Unavailable);
    }

    // Verify the returned leaf against the first issuer and each supplied
    // issuer against the next.  Trust-anchor selection remains the mTLS proxy's
    // responsibility; this prevents persisting malformed/mismatched PCA output.
    let mut parsed_chain = Vec::with_capacity(issued.chain_der.len());
    for item in &issued.chain_der {
        let (remaining, parsed) =
            X509Certificate::from_der(item).map_err(|_| ApiError::Unavailable)?;
        if !remaining.is_empty() {
            return Err(ApiError::Unavailable);
        }
        parsed_chain.push(parsed);
    }
    certificate
        .verify_signature(Some(parsed_chain[0].public_key()))
        .map_err(|_| ApiError::Unavailable)?;
    for pair in parsed_chain.windows(2) {
        pair[0]
            .verify_signature(Some(pair[1].public_key()))
            .map_err(|_| ApiError::Unavailable)?;
    }

    Ok(ValidatedCertificate {
        serial_hex: hex::encode(certificate.raw_serial()),
        fingerprint_sha256: sha256(&issued.leaf_der),
        subject_public_key_sha256: sha256(spki.raw),
        san_uri: expected_san_uri.to_owned(),
        valid_after,
        valid_until,
        leaf_der: issued.leaf_der,
        chain_der: issued.chain_der,
        provider_id: issued.provider_id,
    })
}

pub fn parse_pem_certificates(pem: &str) -> Result<Vec<Vec<u8>>, ApiError> {
    let mut certificates = Vec::new();
    for parsed in x509_parser::pem::Pem::iter_from_buffer(pem.as_bytes()) {
        let parsed = parsed.map_err(|_| ApiError::Unavailable)?;
        if parsed.label != "CERTIFICATE" {
            return Err(ApiError::Unavailable);
        }
        certificates.push(parsed.contents);
    }
    if certificates.is_empty() {
        return Err(ApiError::Unavailable);
    }
    Ok(certificates)
}

#[cfg(test)]
mod tests {
    use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};

    use super::*;

    fn decode<const N: usize>(value: &str) -> [u8; N] {
        URL_SAFE_NO_PAD.decode(value).unwrap().try_into().unwrap()
    }

    #[test]
    fn frozen_hpke_vector_matches_exactly() {
        let enrollment = Uuid::parse_str("00112233-4455-6677-8899-aabbccddeeff").unwrap();
        let companion = Uuid::parse_str("10213243-5465-7687-98a9-bacbdcedfe0f").unwrap();
        let gateway = Uuid::parse_str("f0e0d0c0-b0a0-9080-7060-504030201000").unwrap();
        let recipient = decode::<65>(
            "BHpZMYCGDEA3yDwSdJhFyO4UJN0pf63LiV41glXSx9KyqMolWA8mJv5XkGL_G5n_kcJKDaBvsytb4gFIySSfVlA",
        );
        let enc = decode::<65>(
            "BMZVnUFt-1avcU8UbZF8JKv4GLL7EhYEEpZJhIIwotJYsqbYLcbGc0zwkv-qn8AS8Q9wCNOVKgjVeX6F_qul2Xc",
        );
        let dh: [u8; 32] =
            hex::decode("3bf93df2bbc3bbfc82a8afc49bf6025f85a9619bb95617a7f63f72c67306a77c")
                .unwrap()
                .try_into()
                .unwrap();
        let secret: [u8; 32] =
            hex::decode("404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f")
                .unwrap()
                .try_into()
                .unwrap();
        let context = enrollment_secret_context(enrollment, companion, gateway, 1).unwrap();
        assert_eq!(
            URL_SAFE_NO_PAD.encode(context),
            "S0lUU1UtRU5ST0xMLVNFQ1JFVC0xAAARIjNEVWZ3iJmqu8zd7v8QITJDVGV2h5ipusvc7f4P8ODQwLCgkIBwYFBAMCAQAAAAAAE"
        );
        let sealed =
            seal_enrollment_secret_from_dh(&enc, &recipient, &dh, &context, &secret).unwrap();
        assert_eq!(sealed.enc, enc);
        assert_eq!(
            URL_SAFE_NO_PAD.encode(sealed.ciphertext.as_slice()),
            "Tg3SqAwA9XovN-ZG2uw_EuMMh4wlq3ZH-hSygkNjM-yWlSDm8FWsVBG77lmlIBdo"
        );
    }

    #[test]
    fn frozen_device_proof_transcript_digest_matches() {
        let enrollment = Uuid::parse_str("00112233-4455-6677-8899-aabbccddeeff").unwrap();
        let recipient = decode::<65>(
            "BHpZMYCGDEA3yDwSdJhFyO4UJN0pf63LiV41glXSx9KyqMolWA8mJv5XkGL_G5n_kcJKDaBvsytb4gFIySSfVlA",
        );
        let nonce = decode::<16>("gIGCg4SFhoeIiYqLjI2Ojw");
        // The published cross-component fixture freezes a CSR digest (rather
        // than a full CSR), so exercise the same production transcript builder
        // immediately after its SHA-256 boundary.
        let csr_sha256: [u8; 32] =
            hex::decode("d1584bafd97b7bcea1aaa537abefaa4051796b9c10fe6d54183e77b16caec69d")
                .unwrap()
                .try_into()
                .unwrap();
        let transcript = device_proof_transcript_from_csr_sha256(
            enrollment,
            "KITSU868-TEST-0001",
            &csr_sha256,
            &recipient,
            &nonce,
        )
        .unwrap();
        assert_eq!(
            hex::encode(sha256(&transcript)),
            "473a8019d38ff7f3b392ba3a08caf5fcee76b2110ce9ad628c979893506f155c"
        );
        let signing_public_key = decode::<65>(
            "BL-X0O4YZqrG-Agm663ELz2B4ba48pj10-vnVCt8tIOn3o8U3d6aoudlsuYOzmDPobCVaDqPb2Kvi6zuf036GOo",
        );
        let proof = decode::<64>(
            "UePEVL2fin23PqTWwuYaMKtUYqdMshkOYAjvFxWLe_VO4-32S0YBiPbFrhxr2yqxw9iq9a52NbmsZjJCmiQiPA",
        );
        let fixture_csr = ValidatedCsr {
            der: Vec::new(),
            public_key: signing_public_key,
            spki_sha256: [0_u8; 32],
        };
        verify_device_proof(&fixture_csr, &transcript, &proof).unwrap();
        let mut changed = transcript.to_vec();
        *changed.last_mut().unwrap() ^= 1;
        assert!(verify_device_proof(&fixture_csr, &changed, &proof).is_err());
    }

    #[test]
    fn hpke_context_rejects_nil_or_zero_version() {
        let valid = Uuid::new_v4();
        assert!(enrollment_secret_context(Uuid::nil(), valid, valid, 1).is_err());
        assert!(enrollment_secret_context(valid, valid, valid, 0).is_err());
    }

    #[test]
    fn hpke_recipient_is_validated_before_provider_work() {
        let valid = decode::<65>(
            "BHpZMYCGDEA3yDwSdJhFyO4UJN0pf63LiV41glXSx9KyqMolWA8mJv5XkGL_G5n_kcJKDaBvsytb4gFIySSfVlA",
        );
        validate_hpke_recipient(&valid).unwrap();
        let mut invalid = [0_u8; 65];
        invalid[0] = 0x04;
        assert!(validate_hpke_recipient(&invalid).is_err());
    }

    #[test]
    fn csr_and_issued_leaf_are_verified_end_to_end() {
        const CSR: &str = "MIH3MIGdAgEAMDsxDjAMBgNVBAoMBUtpdHN1MRAwDgYDVQQLDAdGaXh0dXJlMRcwFQYDVQQDDA5maXh0dXJlLWRldmljZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABIPT6GMP6QrBN_FsX_cIUl-4_PKfJcvD8qUl1nkM4Y-JV7KKmdZZlr9VfB0zvAZaJbfUcFmQd35nkGPelbqp5z2gADAKBggqhkjOPQQDAgNJADBGAiEAhiKmUeaZZiN_s-AVGqQuF5lHuh9z-HY3lDpT-tRSHQ0CIQD6gwpkDgKinUBBEuICJ4t7skskzGK1obL9IwMMpa5edw";
        const LEAF: &str = "MIIBzzCCAXWgAwIBAgIBAjAKBggqhkjOPQQDAjArMQ4wDAYDVQQKDAVLaXRzdTEZMBcGA1UEAwwQS2l0c3UgRml4dHVyZSBDQTAeFw0yNTAxMDEwMDAwMDBaFw0zNTAxMDEwMDAwMDBaMDsxDjAMBgNVBAoMBUtpdHN1MRAwDgYDVQQLDAdGaXh0dXJlMRcwFQYDVQQDDA5maXh0dXJlLWRldmljZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABIPT6GMP6QrBN_FsX_cIUl-4_PKfJcvD8qUl1nkM4Y-JV7KKmdZZlr9VfB0zvAZaJbfUcFmQd35nkGPelbqp5z2jejB4MAwGA1UdEwEB_wQCMAAwDgYDVR0PAQH_BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMCMEMGA1UdEQQ8MDqGOHVybjpraXRzdTpjb21wYW5pb246MTAyMTMyNDMtNTQ2NS03Njg3LTk4YTktYmFjYmRjZWRmZTBmMAoGCCqGSM49BAMCA0gAMEUCIQClQEfdBLO_rAg5zU546VqTt5e1TVsjFFs-g_U_XAaVCAIgEyMoSgPPmupb__yb7dINDxFh4OLzUOryhdiguEddJwQ";
        const CA: &str = "MIIBijCCATCgAwIBAgIBATAKBggqhkjOPQQDAjArMQ4wDAYDVQQKDAVLaXRzdTEZMBcGA1UEAwwQS2l0c3UgRml4dHVyZSBDQTAeFw0yNTAxMDEwMDAwMDBaFw0zNTAxMDEwMDAwMDBaMCsxDjAMBgNVBAoMBUtpdHN1MRkwFwYDVQQDDBBLaXRzdSBGaXh0dXJlIENBMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEVNMdTE9rFptHUEqD0NjAySYNjiO21qRpOGyxiBYyUoxvU-RkCg8p_iUcZckaJFTyacBnVw4ll6DOIalSnIt_cKNFMEMwEgYDVR0TAQH_BAgwBgEB_wIBADAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFO7oIIdwNziJAhO5aqzJIMgUEWfVMAoGCCqGSM49BAMCA0gAMEUCIFBtIRHdumQs1YiAy4Ie-a0wYlsgXKpgw2HxmvL4FkNbAiEA_VEgaRleRy4qN0rgY5kuLRnjw5vsJ0FD8Kj8DfKgkQE";
        let csr_der = URL_SAFE_NO_PAD.decode(CSR).unwrap();
        let csr = validate_p256_csr(&csr_der).unwrap();
        assert_eq!(
            hex::encode(sha256(&csr.der)),
            "0541420a9812aff88202cc8712464c9cf4c3d5b943a57830c82ec20833994cdb"
        );
        // Exercise the public production builder with actual DER as well as
        // the published cross-component vector's frozen CSR-hash boundary.
        // This expected digest was independently derived from the transcript
        // bytes and prevents a drift in the wrapper's CSR hashing step.
        let production_transcript = device_proof_transcript(
            Uuid::parse_str("00112233-4455-6677-8899-aabbccddeeff").unwrap(),
            "KITSU868-TEST-0001",
            &csr_der,
            &decode::<65>(
                "BHpZMYCGDEA3yDwSdJhFyO4UJN0pf63LiV41glXSx9KyqMolWA8mJv5XkGL_G5n_kcJKDaBvsytb4gFIySSfVlA",
            ),
            &decode::<16>("gIGCg4SFhoeIiYqLjI2Ojw"),
        )
        .unwrap();
        assert_eq!(
            hex::encode(sha256(&production_transcript)),
            "6cc7b9d8439e0efef93931f21137df0cf67055a4afb9ce194e9b9e73ab02358f"
        );
        let san = "urn:kitsu:companion:10213243-5465-7687-98a9-bacbdcedfe0f";
        let validated = validate_issued_certificate(
            RawIssuedCertificate {
                leaf_der: URL_SAFE_NO_PAD.decode(LEAF).unwrap(),
                chain_der: vec![URL_SAFE_NO_PAD.decode(CA).unwrap()],
                provider_id: "fixture-ca:2".to_owned(),
            },
            &csr,
            san,
            DateTime::parse_from_rfc3339("2026-08-17T00:00:00Z")
                .unwrap()
                .to_utc(),
        )
        .unwrap();
        assert_eq!(validated.san_uri, san);
        assert_eq!(validated.serial_hex, "02");

        let raw = RawIssuedCertificate {
            leaf_der: URL_SAFE_NO_PAD.decode(LEAF).unwrap(),
            chain_der: vec![URL_SAFE_NO_PAD.decode(CA).unwrap()],
            provider_id: "fixture-ca:2".to_owned(),
        };
        assert!(validate_issued_certificate(
            raw,
            &csr,
            "urn:kitsu:companion:00000000-0000-0000-0000-000000000001",
            Utc::now(),
        )
        .is_err());
        let mut corrupted = csr_der;
        *corrupted.last_mut().unwrap() ^= 1;
        assert!(validate_p256_csr(&corrupted).is_err());
    }
}
