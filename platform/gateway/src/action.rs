use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::Deserialize;
use uuid::Uuid;

pub const REMOTE_ACTION_SCHEMA: &str = "kitsu.remote-action.v1";
pub const MAX_ACTION_TYPE_BYTES: usize = 64;
pub const MAX_ACTION_PARAMS_BYTES: usize = 16 * 1024;

/// The gateway validates only the public routing envelope. It deliberately
/// cannot verify or recreate the HMAC because the companion secret exists only
/// on the Heltec and in the backend's KMS-protected store.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RemoteActionEnvelope {
    schema: String,
    action_id: String,
    companion_id: String,
    key_version: u32,
    nonce_b64: String,
    action_type: String,
    created_epoch: String,
    expires_epoch: String,
    params_b64: String,
    signature_b64: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ValidatedRemoteAction {
    pub action_id: Uuid,
    pub companion_id: Uuid,
    pub key_version: u32,
    pub action_type: String,
    pub created_epoch: i64,
    pub expires_epoch: i64,
    pub params_len: usize,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum ActionEnvelopeError {
    #[error("invalid action JSON")]
    Json,
    #[error("unsupported action schema")]
    Schema,
    #[error("invalid action identity")]
    Identity,
    #[error("invalid action key version")]
    KeyVersion,
    #[error("invalid action nonce or signature")]
    Proof,
    #[error("invalid action type")]
    ActionType,
    #[error("invalid action lifetime")]
    Lifetime,
    #[error("invalid action parameters")]
    Params,
}

impl ValidatedRemoteAction {
    pub fn parse(bytes: &[u8]) -> Result<Self, ActionEnvelopeError> {
        let envelope: RemoteActionEnvelope =
            serde_json::from_slice(bytes).map_err(|_| ActionEnvelopeError::Json)?;
        if envelope.schema != REMOTE_ACTION_SCHEMA {
            return Err(ActionEnvelopeError::Schema);
        }
        let action_id =
            parse_canonical_uuid(&envelope.action_id).ok_or(ActionEnvelopeError::Identity)?;
        let companion_id =
            parse_canonical_uuid(&envelope.companion_id).ok_or(ActionEnvelopeError::Identity)?;
        if envelope.key_version == 0 {
            return Err(ActionEnvelopeError::KeyVersion);
        }
        if decode_exact(&envelope.nonce_b64, 16).is_none()
            || decode_exact(&envelope.signature_b64, 32).is_none()
        {
            return Err(ActionEnvelopeError::Proof);
        }
        if !valid_protocol_name(&envelope.action_type) {
            return Err(ActionEnvelopeError::ActionType);
        }
        let created_epoch = parse_canonical_positive_i64(&envelope.created_epoch)
            .ok_or(ActionEnvelopeError::Lifetime)?;
        let expires_epoch = parse_canonical_positive_i64(&envelope.expires_epoch)
            .ok_or(ActionEnvelopeError::Lifetime)?;
        if expires_epoch <= created_epoch {
            return Err(ActionEnvelopeError::Lifetime);
        }
        let params = URL_SAFE_NO_PAD
            .decode(envelope.params_b64.as_bytes())
            .map_err(|_| ActionEnvelopeError::Params)?;
        if params.is_empty()
            || params.len() > MAX_ACTION_PARAMS_BYTES
            || std::str::from_utf8(&params).is_err()
        {
            return Err(ActionEnvelopeError::Params);
        }
        Ok(Self {
            action_id,
            companion_id,
            key_version: envelope.key_version,
            action_type: envelope.action_type,
            created_epoch,
            expires_epoch,
            params_len: params.len(),
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

fn decode_exact(value: &str, expected: usize) -> Option<Vec<u8>> {
    let decoded = URL_SAFE_NO_PAD.decode(value.as_bytes()).ok()?;
    (decoded.len() == expected).then_some(decoded)
}

fn valid_protocol_name(value: &str) -> bool {
    if value.is_empty() || value.len() > MAX_ACTION_TYPE_BYTES {
        return false;
    }
    let mut bytes = value.bytes();
    matches!(bytes.next(), Some(b'a'..=b'z'))
        && bytes.all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'_' | b'.' | b'-')
        })
}

fn parse_canonical_positive_i64(value: &str) -> Option<i64> {
    if value.is_empty()
        || value.starts_with('0')
        || !value.bytes().all(|byte| byte.is_ascii_digit())
    {
        return None;
    }
    value.parse::<i64>().ok().filter(|number| *number > 0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn action() -> Vec<u8> {
        serde_json::to_vec(&json!({
            "schema": REMOTE_ACTION_SCHEMA,
            "action_id": "11111111-2222-4333-8444-555555555555",
            "companion_id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "key_version": 1,
            "nonce_b64": URL_SAFE_NO_PAD.encode([7_u8; 16]),
            "action_type": "companion.pet",
            "created_epoch": "1786924800",
            "expires_epoch": "1786924830",
            "params_b64": URL_SAFE_NO_PAD.encode(b"{}"),
            "signature_b64": URL_SAFE_NO_PAD.encode([9_u8; 32])
        }))
        .unwrap()
    }

    #[test]
    fn accepts_the_frozen_action_wrapper_without_knowing_the_secret() {
        let parsed = ValidatedRemoteAction::parse(&action()).unwrap();
        assert_eq!(parsed.action_type, "companion.pet");
        assert_eq!(parsed.params_len, 2);
        assert_eq!(parsed.expires_epoch - parsed.created_epoch, 30);
    }

    #[test]
    fn rejects_unknown_fields_and_noncanonical_lifetime() {
        let unknown = String::from_utf8(action())
            .unwrap()
            .replace("\"schema\":", "\"unexpected\":1,\"schema\":");
        assert_eq!(
            ValidatedRemoteAction::parse(unknown.as_bytes()).unwrap_err(),
            ActionEnvelopeError::Json
        );
        let noncanonical = String::from_utf8(action())
            .unwrap()
            .replace("\"1786924800\"", "\"01786924800\"");
        assert_eq!(
            ValidatedRemoteAction::parse(noncanonical.as_bytes()).unwrap_err(),
            ActionEnvelopeError::Lifetime
        );
    }

    #[test]
    fn rejects_bad_proof_and_oversize_params() {
        let bad_proof = String::from_utf8(action())
            .unwrap()
            .replace(&URL_SAFE_NO_PAD.encode([9_u8; 32]), "AA");
        assert_eq!(
            ValidatedRemoteAction::parse(bad_proof.as_bytes()).unwrap_err(),
            ActionEnvelopeError::Proof
        );

        let mut value: serde_json::Value = serde_json::from_slice(&action()).unwrap();
        value["params_b64"] =
            serde_json::Value::String(
                URL_SAFE_NO_PAD.encode(vec![b'x'; MAX_ACTION_PARAMS_BYTES + 1]),
            );
        assert_eq!(
            ValidatedRemoteAction::parse(&serde_json::to_vec(&value).unwrap()).unwrap_err(),
            ActionEnvelopeError::Params
        );
    }

    #[test]
    fn rejects_nil_or_noncanonical_action_identities() {
        let nil = String::from_utf8(action()).unwrap().replace(
            "11111111-2222-4333-8444-555555555555",
            "00000000-0000-0000-0000-000000000000",
        );
        assert_eq!(
            ValidatedRemoteAction::parse(nil.as_bytes()).unwrap_err(),
            ActionEnvelopeError::Identity
        );
        let uppercase = String::from_utf8(action()).unwrap().replace(
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE",
        );
        assert_eq!(
            ValidatedRemoteAction::parse(uppercase.as_bytes()).unwrap_err(),
            ActionEnvelopeError::Identity
        );
    }
}
