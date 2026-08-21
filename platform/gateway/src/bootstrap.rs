use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

pub const BOOTSTRAP_PROTOCOL_VERSION: u16 = 1;
pub const MAX_BOOTSTRAP_FRAME_BYTES: usize = 24 * 1024;
pub const MAX_DEVICE_CSR_BYTES: usize = 4_096;
pub const SEALED_SECRET_SUITE: &str = "DHKEM(P-256,HKDF-SHA256)/HKDF-SHA256/AES-256-GCM";

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct BootstrapFrame {
    v: u16,
    r#type: String,
    enrollment_id: String,
    request_b64: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct DeviceClaimRequest {
    claim_token: String,
    hardware_uid: String,
    device_csr_der_b64: String,
    hpke_recipient_b64: String,
    device_nonce_b64: String,
    device_proof_b64: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct DeviceClaimResponse {
    companion_id: String,
    gateway_id: String,
    key_version: u32,
    device_certificate_der_b64: String,
    device_certificate_chain_der_b64: Vec<String>,
    sealed_secret: SealedSecret,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct SealedSecret {
    suite: String,
    enc_b64: String,
    ciphertext_b64: String,
}

#[derive(Debug, Serialize)]
pub struct BootstrapSuccess {
    pub v: u16,
    pub r#type: &'static str,
    pub ok: bool,
    pub enrollment_id: String,
    pub response_b64: String,
}

#[derive(Debug, Serialize)]
pub struct BootstrapFailure {
    pub v: u16,
    pub r#type: &'static str,
    pub ok: bool,
    pub error: &'static str,
}

#[derive(Debug)]
pub struct ValidatedBootstrapRequest {
    pub enrollment_id: Uuid,
    pub claim_json: Vec<u8>,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum BootstrapError {
    #[error("invalid bootstrap JSON")]
    Json,
    #[error("unsupported bootstrap protocol")]
    Protocol,
    #[error("invalid enrollment identity")]
    Identity,
    #[error("invalid device claim")]
    Claim,
    #[error("invalid sealed enrollment response")]
    Response,
}

impl ValidatedBootstrapRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self, BootstrapError> {
        let frame: BootstrapFrame =
            serde_json::from_slice(bytes).map_err(|_| BootstrapError::Json)?;
        if frame.v != BOOTSTRAP_PROTOCOL_VERSION || frame.r#type != "device_enrollment" {
            return Err(BootstrapError::Protocol);
        }
        let enrollment_id = canonical_uuid(&frame.enrollment_id).ok_or(BootstrapError::Identity)?;
        let claim_json = decode_canonical(&frame.request_b64).ok_or(BootstrapError::Claim)?;
        if claim_json.is_empty() || claim_json.len() > 16 * 1024 {
            return Err(BootstrapError::Claim);
        }
        let claim: DeviceClaimRequest =
            serde_json::from_slice(&claim_json).map_err(|_| BootstrapError::Claim)?;
        validate_claim(&claim)?;
        Ok(Self {
            enrollment_id,
            claim_json,
        })
    }
}

pub fn validate_claim_response(bytes: &[u8], expected_gateway: Uuid) -> Result<(), BootstrapError> {
    let response: DeviceClaimResponse =
        serde_json::from_slice(bytes).map_err(|_| BootstrapError::Response)?;
    canonical_uuid(&response.companion_id).ok_or(BootstrapError::Response)?;
    if canonical_uuid(&response.gateway_id) != Some(expected_gateway) || response.key_version == 0 {
        return Err(BootstrapError::Response);
    }
    let leaf =
        decode_canonical(&response.device_certificate_der_b64).ok_or(BootstrapError::Response)?;
    if leaf.is_empty() || leaf.len() > 8 * 1024 {
        return Err(BootstrapError::Response);
    }
    if response.device_certificate_chain_der_b64.is_empty()
        || response.device_certificate_chain_der_b64.len() > 8
        || response
            .device_certificate_chain_der_b64
            .iter()
            .any(|encoded| {
                decode_canonical(encoded).is_none_or(|certificate| {
                    certificate.is_empty() || certificate.len() > 8 * 1024
                })
            })
    {
        return Err(BootstrapError::Response);
    }
    if response.sealed_secret.suite != SEALED_SECRET_SUITE
        || decode_canonical(&response.sealed_secret.enc_b64)
            .is_none_or(|enc| enc.len() != 65 || enc[0] != 0x04)
        || decode_canonical(&response.sealed_secret.ciphertext_b64)
            .is_none_or(|ciphertext| ciphertext.len() != 48)
    {
        return Err(BootstrapError::Response);
    }
    Ok(())
}

pub fn success(enrollment_id: Uuid, response: &[u8]) -> BootstrapSuccess {
    BootstrapSuccess {
        v: BOOTSTRAP_PROTOCOL_VERSION,
        r#type: "device_enrollment_result",
        ok: true,
        enrollment_id: enrollment_id.to_string(),
        response_b64: URL_SAFE_NO_PAD.encode(response),
    }
}

pub fn failure(error: &'static str) -> BootstrapFailure {
    BootstrapFailure {
        v: BOOTSTRAP_PROTOCOL_VERSION,
        r#type: "device_enrollment_result",
        ok: false,
        error,
    }
}

fn validate_claim(claim: &DeviceClaimRequest) -> Result<(), BootstrapError> {
    if decode_canonical(&claim.claim_token).is_none_or(|token| token.len() != 32)
        || claim.hardware_uid.len() < 4
        || claim.hardware_uid.len() > 128
        || claim.hardware_uid.chars().any(char::is_control)
    {
        return Err(BootstrapError::Claim);
    }
    let csr = decode_canonical(&claim.device_csr_der_b64).ok_or(BootstrapError::Claim)?;
    if csr.is_empty() || csr.len() > MAX_DEVICE_CSR_BYTES {
        return Err(BootstrapError::Claim);
    }
    if decode_canonical(&claim.hpke_recipient_b64)
        .is_none_or(|key| key.len() != 65 || key[0] != 0x04)
        || decode_canonical(&claim.device_nonce_b64).is_none_or(|nonce| nonce.len() != 16)
        || decode_canonical(&claim.device_proof_b64).is_none_or(|proof| proof.len() != 64)
    {
        return Err(BootstrapError::Claim);
    }
    Ok(())
}

fn canonical_uuid(value: &str) -> Option<Uuid> {
    if value.len() != 36 || value.bytes().any(|byte| byte.is_ascii_uppercase()) {
        return None;
    }
    let parsed = Uuid::parse_str(value).ok()?;
    (parsed.hyphenated().to_string() == value && !parsed.is_nil()).then_some(parsed)
}

fn decode_canonical(value: &str) -> Option<Vec<u8>> {
    if value.is_empty() || value.contains('=') {
        return None;
    }
    let decoded = URL_SAFE_NO_PAD.decode(value.as_bytes()).ok()?;
    (URL_SAFE_NO_PAD.encode(&decoded) == value).then_some(decoded)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn claim() -> serde_json::Value {
        json!({
            "claim_token": URL_SAFE_NO_PAD.encode([0_u8; 32]),
            "hardware_uid": "KITSU868-TEST-0001",
            "device_csr_der_b64": URL_SAFE_NO_PAD.encode([1_u8; 64]),
            "hpke_recipient_b64": URL_SAFE_NO_PAD.encode(
                std::iter::once(4_u8).chain([2_u8; 64]).collect::<Vec<_>>()
            ),
            "device_nonce_b64": URL_SAFE_NO_PAD.encode([3_u8; 16]),
            "device_proof_b64": URL_SAFE_NO_PAD.encode([4_u8; 64])
        })
    }

    fn frame(enrollment: Uuid) -> Vec<u8> {
        serde_json::to_vec(&json!({
            "v": 1,
            "type": "device_enrollment",
            "enrollment_id": enrollment,
            "request_b64": URL_SAFE_NO_PAD.encode(serde_json::to_vec(&claim()).unwrap())
        }))
        .unwrap()
    }

    #[test]
    fn accepts_only_a_strict_bounded_device_claim() {
        let id = Uuid::new_v4();
        let parsed = ValidatedBootstrapRequest::parse(&frame(id)).unwrap();
        assert_eq!(parsed.enrollment_id, id);
        assert_eq!(
            serde_json::from_slice::<serde_json::Value>(&parsed.claim_json).unwrap(),
            claim()
        );

        let unknown = String::from_utf8(frame(id))
            .unwrap()
            .replace("\"v\":1", "\"extra\":true,\"v\":1");
        assert_eq!(
            ValidatedBootstrapRequest::parse(unknown.as_bytes()).unwrap_err(),
            BootstrapError::Json
        );
    }

    #[test]
    fn validates_the_sealed_response_and_rejects_plaintext_fields() {
        let gateway = Uuid::new_v4();
        let response = json!({
            "companion_id": Uuid::new_v4(),
            "gateway_id": gateway,
            "key_version": 1,
            "device_certificate_der_b64": URL_SAFE_NO_PAD.encode([5_u8; 128]),
            "device_certificate_chain_der_b64": [URL_SAFE_NO_PAD.encode([6_u8; 128])],
            "sealed_secret": {
                "suite": SEALED_SECRET_SUITE,
                "enc_b64": URL_SAFE_NO_PAD.encode(
                    std::iter::once(4_u8).chain([7_u8; 64]).collect::<Vec<_>>()
                ),
                "ciphertext_b64": URL_SAFE_NO_PAD.encode([8_u8; 48])
            }
        });
        let encoded = serde_json::to_vec(&response).unwrap();
        assert_eq!(validate_claim_response(&encoded, gateway), Ok(()));

        let mut plaintext = response;
        plaintext["companion_secret_b64"] = json!(URL_SAFE_NO_PAD.encode([9_u8; 32]));
        assert_eq!(
            validate_claim_response(&serde_json::to_vec(&plaintext).unwrap(), gateway),
            Err(BootstrapError::Response)
        );
    }
}
