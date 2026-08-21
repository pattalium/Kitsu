use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::Deserialize;
use uuid::Uuid;

pub const MAX_PAYLOAD_TYPE_BYTES: usize = 64;
pub const MAX_BACKEND_PAYLOAD_BYTES: usize = 256 * 1024;
pub const MAX_SEQUENCE: u64 = i64::MAX as u64;
pub const MIN_KNOWN_EPOCH: i64 = 1_577_836_800;
pub const MAX_KNOWN_EPOCH: i64 = 4_102_444_800;

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DeviceEnvelope {
    pub schema: String,
    pub companion_id: String,
    pub gateway_id: String,
    pub sequence: String,
    pub issued_epoch: String,
    pub nonce_b64: String,
    pub request_id: String,
    pub key_version: u32,
    pub payload_type: String,
    pub payload_b64: String,
    pub signature_b64: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ValidatedEnvelope {
    pub companion_id: Uuid,
    pub gateway_id: Uuid,
    pub sequence: u64,
    pub issued_epoch: i64,
    pub payload_type: String,
    pub payload_len: usize,
    pub key_version: u32,
    pub request_id: Uuid,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum EnvelopeError {
    #[error("invalid JSON envelope")]
    Json,
    #[error("unsupported schema or algorithm")]
    Unsupported,
    #[error("invalid envelope identity")]
    Identity,
    #[error("gateway identifier does not match this gateway")]
    WrongGateway,
    #[error("invalid sequence")]
    Sequence,
    #[error("invalid issued epoch")]
    Epoch,
    #[error("invalid payload type")]
    PayloadType,
    #[error("invalid payload encoding or size")]
    Payload,
    #[error("invalid proof fields")]
    Proof,
}

impl DeviceEnvelope {
    pub fn parse_and_validate(
        bytes: &[u8],
        expected_gateway: Uuid,
    ) -> Result<ValidatedEnvelope, EnvelopeError> {
        let envelope: DeviceEnvelope =
            serde_json::from_slice(bytes).map_err(|_| EnvelopeError::Json)?;
        if envelope.schema != "kitsu.device-envelope.v1" {
            return Err(EnvelopeError::Unsupported);
        }
        let companion_id =
            parse_canonical_uuid(&envelope.companion_id).ok_or(EnvelopeError::Identity)?;
        let gateway_id =
            parse_canonical_uuid(&envelope.gateway_id).ok_or(EnvelopeError::Identity)?;
        let request_id =
            parse_canonical_uuid(&envelope.request_id).ok_or(EnvelopeError::Identity)?;
        if gateway_id != expected_gateway {
            return Err(EnvelopeError::WrongGateway);
        }
        let sequence = parse_canonical_u64(&envelope.sequence).ok_or(EnvelopeError::Sequence)?;
        if sequence == 0 || sequence > MAX_SEQUENCE {
            return Err(EnvelopeError::Sequence);
        }
        let issued_epoch =
            parse_canonical_i64(&envelope.issued_epoch).ok_or(EnvelopeError::Epoch)?;
        if issued_epoch != 0 && !(MIN_KNOWN_EPOCH..=MAX_KNOWN_EPOCH).contains(&issued_epoch) {
            return Err(EnvelopeError::Epoch);
        }
        if !valid_payload_type(&envelope.payload_type) {
            return Err(EnvelopeError::PayloadType);
        }
        let payload = URL_SAFE_NO_PAD
            .decode(envelope.payload_b64.as_bytes())
            .map_err(|_| EnvelopeError::Payload)?;
        if payload.is_empty()
            || payload.len() > MAX_BACKEND_PAYLOAD_BYTES
            || std::str::from_utf8(&payload).is_err()
        {
            return Err(EnvelopeError::Payload);
        }
        if envelope.key_version == 0
            || URL_SAFE_NO_PAD
                .decode(envelope.nonce_b64.as_bytes())
                .map_err(|_| EnvelopeError::Proof)?
                .len()
                != 16
            || URL_SAFE_NO_PAD
                .decode(envelope.signature_b64.as_bytes())
                .map_err(|_| EnvelopeError::Proof)?
                .len()
                != 32
        {
            return Err(EnvelopeError::Proof);
        }

        Ok(ValidatedEnvelope {
            companion_id,
            gateway_id,
            sequence,
            issued_epoch,
            payload_type: envelope.payload_type,
            payload_len: payload.len(),
            key_version: envelope.key_version,
            request_id,
        })
    }
}

fn parse_canonical_uuid(value: &str) -> Option<Uuid> {
    if value.len() != 36 || value.bytes().any(|byte| byte.is_ascii_uppercase()) {
        return None;
    }
    let parsed = Uuid::parse_str(value).ok()?;
    (parsed.hyphenated().to_string() == value && !parsed.is_nil()).then_some(parsed)
}

fn parse_canonical_u64(value: &str) -> Option<u64> {
    if value.is_empty()
        || (value.len() > 1 && value.starts_with('0'))
        || !value.bytes().all(|b| b.is_ascii_digit())
    {
        return None;
    }
    value.parse().ok()
}

fn parse_canonical_i64(value: &str) -> Option<i64> {
    if value == "0" {
        return Some(0);
    }
    if value.is_empty()
        || value.starts_with('-')
        || value.starts_with('0')
        || !value.bytes().all(|b| b.is_ascii_digit())
    {
        return None;
    }
    value.parse().ok()
}

fn valid_payload_type(value: &str) -> bool {
    if value.is_empty() || value.len() > MAX_PAYLOAD_TYPE_BYTES {
        return false;
    }
    let mut bytes = value.bytes();
    matches!(bytes.next(), Some(b'a'..=b'z'))
        && bytes.all(|b| {
            b.is_ascii_lowercase() || b.is_ascii_digit() || matches!(b, b'_' | b'.' | b'-')
        })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn envelope(gateway: Uuid) -> Vec<u8> {
        serde_json::to_vec(&json!({
            "schema": "kitsu.device-envelope.v1",
            "companion_id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "gateway_id": gateway,
            "sequence": "1",
            "issued_epoch": "0",
            "nonce_b64": URL_SAFE_NO_PAD.encode([7_u8; 16]),
            "request_id": "11111111-2222-4333-8444-555555555555",
            "key_version": 1,
            "payload_type": "heartbeat",
            "payload_b64": URL_SAFE_NO_PAD.encode(b"{}"),
            "signature_b64": URL_SAFE_NO_PAD.encode([9_u8; 32])
        }))
        .unwrap()
    }

    #[test]
    fn validates_bounded_outer_envelope_without_needing_secret() {
        let gateway = Uuid::new_v4();
        let parsed = DeviceEnvelope::parse_and_validate(&envelope(gateway), gateway).unwrap();
        assert_eq!(parsed.sequence, 1);
        assert_eq!(parsed.payload_len, 2);
    }

    #[test]
    fn rejects_wrong_gateway_and_noncanonical_sequence() {
        let gateway = Uuid::new_v4();
        assert_eq!(
            DeviceEnvelope::parse_and_validate(&envelope(gateway), Uuid::new_v4()).unwrap_err(),
            EnvelopeError::WrongGateway
        );
        let altered = String::from_utf8(envelope(gateway))
            .unwrap()
            .replace("\"sequence\":\"1\"", "\"sequence\":\"01\"");
        assert_eq!(
            DeviceEnvelope::parse_and_validate(altered.as_bytes(), gateway).unwrap_err(),
            EnvelopeError::Sequence
        );
    }

    #[test]
    fn rejects_nil_and_noncanonical_uuid_text() {
        let gateway = Uuid::new_v4();
        let nil = String::from_utf8(envelope(gateway)).unwrap().replace(
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "00000000-0000-0000-0000-000000000000",
        );
        assert_eq!(
            DeviceEnvelope::parse_and_validate(nil.as_bytes(), gateway).unwrap_err(),
            EnvelopeError::Identity
        );
        let uppercase = String::from_utf8(envelope(gateway)).unwrap().replace(
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE",
        );
        assert_eq!(
            DeviceEnvelope::parse_and_validate(uppercase.as_bytes(), gateway).unwrap_err(),
            EnvelopeError::Identity
        );
    }
}
