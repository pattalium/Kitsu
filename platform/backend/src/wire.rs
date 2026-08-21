//! Backend adapter for the root-owned language-neutral protocol. Keep field
//! names and validation aligned with `platform/protocol`; this module is not a
//! second wire-version authority.

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use uuid::Uuid;

use crate::error::ApiError;

pub const DEVICE_ENVELOPE_SCHEMA: &str = "kitsu.device-envelope.v1";
pub const REMOTE_ACTION_SCHEMA: &str = "kitsu.remote-action.v1";
pub const DEVICE_TRANSCRIPT_DOMAIN: &[u8] = b"KITSU-DEVICE-1\0";
pub const ACTION_TRANSCRIPT_DOMAIN: &[u8] = b"KITSU-ACTION-1\0";
pub const MAX_DEVICE_PAYLOAD_BYTES: usize = 256 * 1024;
pub const MAX_ACTION_PARAMETER_BYTES: usize = 16 * 1024;

/// JSON is only a transport wrapper. `payload_b64` is authenticated as exact
/// bytes and parsed after the HMAC and replay precondition succeed.
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct DeviceEnvelope {
    pub schema: String,
    #[serde(deserialize_with = "deserialize_canonical_uuid")]
    pub companion_id: Uuid,
    #[serde(deserialize_with = "deserialize_canonical_uuid")]
    pub gateway_id: Uuid,
    /// Canonical decimal, deliberately capped at i64::MAX for PostgreSQL
    /// BIGINT while encoded as u64be in the transcript.
    pub sequence: String,
    /// Canonical decimal signed integer. Zero means wall time unavailable.
    pub issued_epoch: String,
    /// Unpadded base64url, exactly 16 bytes.
    pub nonce_b64: String,
    #[serde(deserialize_with = "deserialize_canonical_uuid")]
    pub request_id: Uuid,
    pub key_version: u32,
    pub payload_type: String,
    /// Unpadded base64url exact UTF-8 JSON bytes.
    pub payload_b64: String,
    /// Unpadded base64url HMAC-SHA-256, exactly 32 bytes.
    pub signature_b64: String,
}

pub struct ValidatedEnvelope {
    pub sequence: i64,
    pub issued_epoch: i64,
    pub nonce: [u8; 16],
    pub signature: [u8; 32],
    pub payload: Vec<u8>,
}

pub struct ValidatedRemoteAction {
    pub key_version: u32,
    pub nonce: [u8; 16],
    pub created_epoch: i64,
    pub expires_epoch: i64,
    pub params: Vec<u8>,
    pub signature: [u8; 32],
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum DevicePayload {
    EventBatch {
        events: Vec<DeviceEvent>,
    },
    PeerSnapshot {
        peers: Vec<PeerRecord>,
    },
    ActionResult {
        #[serde(deserialize_with = "deserialize_canonical_uuid")]
        action_id: Uuid,
        status: ActionResultStatus,
        completed_epoch: i64,
        result: serde_json::Map<String, Value>,
    },
    ActionAcceptance {
        #[serde(deserialize_with = "deserialize_canonical_uuid")]
        action_id: Uuid,
        accepted_epoch: i64,
    },
    Heartbeat {
        uptime_ms: u64,
        #[serde(default)]
        firmware_version: Option<String>,
    },
    GatewayHello {
        protocol_version: u8,
        gateway_version: String,
    },
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct DeviceEvent {
    #[serde(deserialize_with = "deserialize_canonical_uuid")]
    pub event_id: Uuid,
    pub event_type: String,
    pub observed: ObservationTime,
    pub body: serde_json::Map<String, Value>,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ObservationTime {
    pub epoch: Option<i64>,
    pub boot_id: u32,
    pub monotonic_ms: u32,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct LastHopSignal {
    pub scope: String,
    pub rssi_deci_dbm: i16,
    pub snr_deci_db: i16,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PeerRecord {
    pub public_key: String,
    pub role: PeerRole,
    pub name: String,
    pub seen_count: u32,
    pub last_seen: ObservationTime,
    pub signal: Option<LastHopSignal>,
    pub journal_sequence: u32,
}

#[derive(Clone, Copy, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PeerRole {
    Client,
    Repeater,
    Room,
    Sensor,
}

impl PeerRole {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Client => "client",
            Self::Repeater => "repeater",
            Self::Room => "room",
            Self::Sensor => "sensor",
        }
    }
}

#[derive(Clone, Copy, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ActionResultStatus {
    Succeeded,
    Failed,
    Rejected,
}

impl ActionResultStatus {
    pub fn as_db(self) -> &'static str {
        match self {
            Self::Succeeded => "succeeded",
            Self::Failed => "failed",
            Self::Rejected => "rejected",
        }
    }
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct CreateActionRequest {
    pub action_type: String,
    #[serde(default)]
    pub parameters: serde_json::Map<String, Value>,
    pub expires_in_seconds: u64,
}

/// End-to-end backend-authenticated command. A gateway routes this object as
/// opaque JSON and must not decode or rewrite `params_b64`.
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RemoteAction {
    pub schema: String,
    #[serde(deserialize_with = "deserialize_canonical_uuid")]
    pub action_id: Uuid,
    #[serde(deserialize_with = "deserialize_canonical_uuid")]
    pub companion_id: Uuid,
    pub key_version: u32,
    pub nonce_b64: String,
    pub action_type: String,
    pub created_epoch: String,
    pub expires_epoch: String,
    pub params_b64: String,
    pub signature_b64: String,
}

impl RemoteAction {
    pub fn validate_wrapper(&self) -> Result<ValidatedRemoteAction, ApiError> {
        if self.schema != REMOTE_ACTION_SCHEMA
            || self.action_id.is_nil()
            || self.companion_id.is_nil()
            || self.key_version == 0
            || !valid_protocol_name(&self.action_type)
        {
            return Err(ApiError::Invalid("unsupported remote action"));
        }
        let created_epoch = parse_canonical_i64(&self.created_epoch, false)?;
        let expires_epoch = parse_canonical_i64(&self.expires_epoch, false)?;
        if !(1_577_836_800..=4_102_444_800).contains(&created_epoch)
            || !(1_577_836_800..=4_102_444_800).contains(&expires_epoch)
            || expires_epoch <= created_epoch
            || expires_epoch - created_epoch > 86_400
        {
            return Err(ApiError::Invalid("remote action expiry is outside policy"));
        }
        let nonce = decode_exact::<16>(&self.nonce_b64, "invalid action nonce")?;
        let signature = decode_exact::<32>(&self.signature_b64, "invalid action signature")?;
        let params =
            decode_canonical_base64url(&self.params_b64, "invalid action parameter encoding")?;
        if params.is_empty() || params.len() > MAX_ACTION_PARAMETER_BYTES {
            return Err(ApiError::Invalid("action parameter size is outside policy"));
        }
        Ok(ValidatedRemoteAction {
            key_version: self.key_version,
            nonce,
            created_epoch,
            expires_epoch,
            params,
            signature,
        })
    }
}

impl DeviceEnvelope {
    pub fn validate_wrapper(&self) -> Result<ValidatedEnvelope, ApiError> {
        if self.schema != DEVICE_ENVELOPE_SCHEMA
            || self.companion_id.is_nil()
            || self.gateway_id.is_nil()
            || self.request_id.is_nil()
            || self.key_version == 0
        {
            return Err(ApiError::Invalid("unsupported device envelope"));
        }
        let sequence = parse_canonical_i64(&self.sequence, false)?;
        if sequence <= 0 {
            return Err(ApiError::Invalid("invalid sequence"));
        }
        let issued_epoch = parse_canonical_i64(&self.issued_epoch, true)?;
        if issued_epoch != 0 && !(1_577_836_800..=4_102_444_800).contains(&issued_epoch) {
            return Err(ApiError::Invalid("issued epoch is outside policy"));
        }
        if self.payload_type.is_empty()
            || self.payload_type.len() > 64
            || !valid_protocol_name(&self.payload_type)
        {
            return Err(ApiError::Invalid("invalid payload type"));
        }
        let nonce = decode_exact::<16>(&self.nonce_b64, "invalid nonce")?;
        let signature = decode_exact::<32>(&self.signature_b64, "invalid signature")?;
        let payload = decode_canonical_base64url(&self.payload_b64, "invalid payload encoding")?;
        if payload.is_empty() || payload.len() > MAX_DEVICE_PAYLOAD_BYTES {
            return Err(ApiError::Invalid("device payload size is outside policy"));
        }
        Ok(ValidatedEnvelope {
            sequence,
            issued_epoch,
            nonce,
            signature,
            payload,
        })
    }

    pub fn parse_payload(&self, bytes: &[u8]) -> Result<DevicePayload, ApiError> {
        let payload: DevicePayload = serde_json::from_slice(bytes)
            .map_err(|_| ApiError::Invalid("device payload is not valid protocol JSON"))?;
        let actual_type = match &payload {
            DevicePayload::EventBatch { .. } => "event_batch",
            DevicePayload::PeerSnapshot { .. } => "peer_snapshot",
            DevicePayload::ActionResult { .. } => "action_result",
            DevicePayload::ActionAcceptance { .. } => "action_acceptance",
            DevicePayload::Heartbeat { .. } => "heartbeat",
            DevicePayload::GatewayHello { .. } => "gateway_hello",
        };
        if self.payload_type != actual_type {
            return Err(ApiError::Invalid(
                "signed payload type does not match JSON payload",
            ));
        }
        validate_payload(&payload)?;
        Ok(payload)
    }
}

pub fn validate_action(request: &CreateActionRequest, max_ttl: u64) -> Result<(), ApiError> {
    if !matches!(
        request.action_type.as_str(),
        "companion.pet"
            | "companion.feed"
            | "companion.play"
            | "companion.listen_once"
            | "sync.pull"
            | "clock.set"
            | "mesh.introduce"
            | "message.send"
    ) {
        return Err(ApiError::Invalid("unsupported action type"));
    }
    if !(5..=max_ttl).contains(&request.expires_in_seconds) {
        return Err(ApiError::Invalid("action expiry is outside policy"));
    }
    validate_action_parameters(&request.action_type, &request.parameters)?;
    let encoded = serde_jcs::to_vec(&request.parameters).map_err(ApiError::internal)?;
    if encoded.len() > MAX_ACTION_PARAMETER_BYTES {
        return Err(ApiError::Invalid("action parameters are too large"));
    }
    Ok(())
}

fn validate_action_parameters(
    action_type: &str,
    parameters: &serde_json::Map<String, Value>,
) -> Result<(), ApiError> {
    match action_type {
        "companion.pet" | "companion.feed" | "companion.play" | "sync.pull" => {
            require_keys(parameters, &[], &[])?;
        }
        "companion.listen_once" => {
            require_keys(parameters, &["duration_ms"], &[])?;
            let duration = parameters["duration_ms"]
                .as_u64()
                .ok_or(ApiError::Invalid("listen duration must be an integer"))?;
            if !(1_000..=60_000).contains(&duration) {
                return Err(ApiError::Invalid("listen duration must be 1s..60s"));
            }
        }
        "clock.set" => {
            require_keys(parameters, &["epoch"], &[])?;
            let epoch = parameters["epoch"]
                .as_i64()
                .ok_or(ApiError::Invalid("clock epoch must be an integer"))?;
            if !(1_577_836_800..=4_102_444_800).contains(&epoch) {
                return Err(ApiError::Invalid("clock epoch is outside policy"));
            }
        }
        "mesh.introduce" => {
            require_keys(parameters, &["scope"], &[])?;
            if !matches!(parameters["scope"].as_str(), Some("nearby" | "mesh")) {
                return Err(ApiError::Invalid("introduce scope must be nearby or mesh"));
            }
        }
        "message.send" => {
            require_keys(parameters, &["route", "target", "text"], &[])?;
            if !matches!(parameters["route"].as_str(), Some("direct" | "channel")) {
                return Err(ApiError::Invalid("message route must be direct or channel"));
            }
            let target = parameters["target"]
                .as_str()
                .ok_or(ApiError::Invalid("message target must be text"))?;
            if target.is_empty() || target.len() > 128 || target.chars().any(char::is_control) {
                return Err(ApiError::Invalid("message target is outside policy"));
            }
            let text = parameters["text"]
                .as_str()
                .ok_or(ApiError::Invalid("message text must be text"))?;
            if text.is_empty() || text.len() > 128 || text.contains('\0') {
                return Err(ApiError::Invalid("message text must be 1..128 UTF-8 bytes"));
            }
        }
        _ => return Err(ApiError::Invalid("unsupported action type")),
    }
    Ok(())
}

fn require_keys(
    parameters: &serde_json::Map<String, Value>,
    required: &[&str],
    optional: &[&str],
) -> Result<(), ApiError> {
    if required.iter().any(|key| !parameters.contains_key(*key))
        || parameters
            .keys()
            .any(|key| !required.contains(&key.as_str()) && !optional.contains(&key.as_str()))
    {
        return Err(ApiError::Invalid(
            "action parameters do not match the action schema",
        ));
    }
    Ok(())
}

fn validate_payload(payload: &DevicePayload) -> Result<(), ApiError> {
    match payload {
        DevicePayload::EventBatch { events } => {
            if events.is_empty() || events.len() > 64 {
                return Err(ApiError::Invalid("event batch must contain 1..64 events"));
            }
            for event in events {
                validate_event(event)?;
            }
        }
        DevicePayload::PeerSnapshot { peers } => {
            if peers.len() > 64 {
                return Err(ApiError::Invalid("peer snapshot exceeds 64 records"));
            }
            for peer in peers {
                validate_peer(peer)?;
            }
        }
        DevicePayload::ActionResult {
            completed_epoch,
            result,
            ..
        } => {
            if !(1_577_836_800..=4_102_444_800).contains(completed_epoch) || result.len() > 32 {
                return Err(ApiError::Invalid("invalid action result"));
            }
        }
        DevicePayload::ActionAcceptance { accepted_epoch, .. } => {
            if !(1_577_836_800..=4_102_444_800).contains(accepted_epoch) {
                return Err(ApiError::Invalid("invalid action acceptance"));
            }
        }
        DevicePayload::Heartbeat {
            firmware_version, ..
        } => {
            if firmware_version
                .as_ref()
                .is_some_and(|value| value.len() > 64)
            {
                return Err(ApiError::Invalid("firmware version is too long"));
            }
        }
        DevicePayload::GatewayHello {
            protocol_version,
            gateway_version,
        } => {
            if *protocol_version != 1 || gateway_version.is_empty() || gateway_version.len() > 64 {
                return Err(ApiError::Invalid("invalid gateway hello"));
            }
        }
    }
    Ok(())
}

fn validate_event(event: &DeviceEvent) -> Result<(), ApiError> {
    if !valid_protocol_name(&event.event_type) || event.body.len() > 64 {
        return Err(ApiError::Invalid("invalid device event"));
    }
    validate_observed(&event.observed)?;
    if event.event_type == "companion.snapshot" {
        validate_companion_snapshot(&event.body)?;
    }
    Ok(())
}

fn validate_companion_snapshot(body: &Map<String, Value>) -> Result<(), ApiError> {
    require_keys(
        body,
        &[
            "schema",
            "firmware_version",
            "remote_connectivity_allowed",
            "wifi",
            "gateway",
            "channels",
        ],
        &[],
    )?;
    if body["schema"].as_str() != Some("kitsu.companion-snapshot.v1") {
        return Err(ApiError::Invalid("invalid companion snapshot schema"));
    }
    let firmware = body["firmware_version"]
        .as_str()
        .ok_or(ApiError::Invalid("invalid firmware version"))?;
    if firmware.is_empty() || firmware.len() > 64 || firmware.chars().any(char::is_control) {
        return Err(ApiError::Invalid("invalid firmware version"));
    }
    if !body["remote_connectivity_allowed"].is_boolean() {
        return Err(ApiError::Invalid("invalid remote connectivity state"));
    }
    let wifi = body["wifi"]
        .as_object()
        .ok_or(ApiError::Invalid("invalid Wi-Fi snapshot"))?;
    require_keys(wifi, &["configured", "state"], &[])?;
    if !wifi["configured"].is_boolean() || !bounded_state(&wifi["state"]) {
        return Err(ApiError::Invalid("invalid Wi-Fi snapshot"));
    }
    let gateway = body["gateway"]
        .as_object()
        .ok_or(ApiError::Invalid("invalid gateway snapshot"))?;
    require_keys(gateway, &["configured", "enrolled", "lan_state"], &[])?;
    if !gateway["configured"].is_boolean()
        || !gateway["enrolled"].is_boolean()
        || !bounded_state(&gateway["lan_state"])
    {
        return Err(ApiError::Invalid("invalid gateway snapshot"));
    }
    let channels = body["channels"]
        .as_array()
        .filter(|items| items.len() == 4)
        .ok_or(ApiError::Invalid("invalid channel snapshot"))?;
    let mut seen = [false; 4];
    for channel in channels {
        let channel = channel
            .as_object()
            .ok_or(ApiError::Invalid("invalid channel snapshot"))?;
        require_keys(
            channel,
            &["slot", "configured", "max_utf8_bytes"],
            &["name"],
        )?;
        let slot = channel["slot"]
            .as_u64()
            .and_then(|slot| usize::try_from(slot).ok())
            .filter(|slot| *slot < seen.len())
            .ok_or(ApiError::Invalid("invalid channel slot"))?;
        if seen[slot]
            || !channel["configured"].is_boolean()
            || channel["max_utf8_bytes"].as_u64() != Some(128)
        {
            return Err(ApiError::Invalid("invalid channel snapshot"));
        }
        seen[slot] = true;
        match (channel["configured"].as_bool(), channel.get("name")) {
            (Some(true), Some(Value::String(name)))
                if !name.is_empty() && name.len() <= 32 && !name.chars().any(char::is_control) => {}
            (Some(false), None) => {}
            _ => return Err(ApiError::Invalid("invalid channel name")),
        }
    }
    Ok(())
}

fn bounded_state(value: &Value) -> bool {
    value.as_str().is_some_and(|text| {
        !text.is_empty() && text.len() <= 64 && !text.chars().any(char::is_control)
    })
}

fn validate_peer(peer: &PeerRecord) -> Result<(), ApiError> {
    if peer.public_key.len() != 64
        || !peer
            .public_key
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'A'..=b'F').contains(&byte))
        || peer.name.len() > 32
        || peer.name.chars().any(char::is_control)
        || peer.seen_count == 0
        || peer.journal_sequence == 0
    {
        return Err(ApiError::Invalid("invalid peer record"));
    }
    validate_observed(&peer.last_seen)?;
    if let Some(signal) = &peer.signal {
        if signal.scope != "last_hop"
            || !(-2000..=0).contains(&signal.rssi_deci_dbm)
            || !(-500..=500).contains(&signal.snr_deci_db)
        {
            return Err(ApiError::Invalid("invalid last-hop signal"));
        }
    }
    Ok(())
}

fn validate_observed(observed: &ObservationTime) -> Result<(), ApiError> {
    if observed
        .epoch
        .is_some_and(|epoch| !(1_577_836_800..=4_102_444_800).contains(&epoch))
    {
        return Err(ApiError::Invalid("invalid observation epoch"));
    }
    Ok(())
}

fn valid_protocol_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value.bytes().enumerate().all(|(index, byte)| {
            if index == 0 {
                byte.is_ascii_lowercase()
            } else {
                byte.is_ascii_lowercase()
                    || byte.is_ascii_digit()
                    || matches!(byte, b'_' | b'.' | b'-')
            }
        })
}

fn deserialize_canonical_uuid<'de, D>(deserializer: D) -> Result<Uuid, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let text = String::deserialize(deserializer)?;
    let uuid = Uuid::parse_str(&text).map_err(serde::de::Error::custom)?;
    if uuid.is_nil() || uuid.hyphenated().to_string() != text {
        return Err(serde::de::Error::custom(
            "UUID must be non-nil canonical lowercase hyphenated text",
        ));
    }
    Ok(uuid)
}

fn parse_canonical_i64(value: &str, allow_zero: bool) -> Result<i64, ApiError> {
    if value.is_empty()
        || value.len() > 19
        || value.starts_with('+')
        || (value.len() > 1 && value.starts_with('0'))
        || value.starts_with('-')
        || !value.bytes().all(|byte| byte.is_ascii_digit())
    {
        return Err(ApiError::Invalid("invalid canonical integer"));
    }
    let parsed = value
        .parse::<i64>()
        .map_err(|_| ApiError::Invalid("integer outside supported range"))?;
    if !allow_zero && parsed == 0 {
        return Err(ApiError::Invalid("zero is not allowed"));
    }
    Ok(parsed)
}

fn decode_exact<const N: usize>(encoded: &str, message: &'static str) -> Result<[u8; N], ApiError> {
    let decoded = decode_canonical_base64url(encoded, message)?;
    decoded.try_into().map_err(|_| ApiError::Invalid(message))
}

fn decode_canonical_base64url(encoded: &str, message: &'static str) -> Result<Vec<u8>, ApiError> {
    let decoded = URL_SAFE_NO_PAD
        .decode(encoded.as_bytes())
        .map_err(|_| ApiError::Invalid(message))?;
    if URL_SAFE_NO_PAD.encode(&decoded) != encoded {
        return Err(ApiError::Invalid(message));
    }
    Ok(decoded)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(action_type: &str, parameters: Value) -> CreateActionRequest {
        CreateActionRequest {
            action_type: action_type.to_owned(),
            parameters: parameters.as_object().unwrap().clone(),
            expires_in_seconds: 60,
        }
    }

    #[test]
    fn care_actions_have_strict_parameter_schemas() {
        for action_type in ["companion.pet", "companion.feed", "companion.play"] {
            assert!(validate_action(&request(action_type, serde_json::json!({})), 3_600).is_ok());
            assert!(validate_action(
                &request(action_type, serde_json::json!({"unexpected": true})),
                3_600
            )
            .is_err());
        }

        assert!(validate_action(
            &request(
                "companion.listen_once",
                serde_json::json!({"duration_ms": 1_000})
            ),
            3_600
        )
        .is_ok());
        assert!(validate_action(
            &request(
                "companion.listen_once",
                serde_json::json!({"duration_ms": 60_000})
            ),
            3_600
        )
        .is_ok());
        for invalid in [999_u64, 60_001] {
            assert!(validate_action(
                &request(
                    "companion.listen_once",
                    serde_json::json!({"duration_ms": invalid})
                ),
                3_600
            )
            .is_err());
        }
    }

    #[test]
    fn message_and_introduction_parameters_are_bounded_and_closed() {
        assert!(validate_action(
            &request(
                "message.send",
                serde_json::json!({"route":"direct","target":"AABB","text":"hello"})
            ),
            3_600
        )
        .is_ok());
        assert!(validate_action(
            &request(
                "message.send",
                serde_json::json!({"route":"direct","target":"AABB","text":"hello","extra":1})
            ),
            3_600
        )
        .is_err());
        assert!(validate_action(
            &request(
                "message.send",
                serde_json::json!({"route":"broadcast","target":"AABB","text":"hello"})
            ),
            3_600
        )
        .is_err());
        assert!(validate_action(
            &request("mesh.introduce", serde_json::json!({"scope":"nearby"})),
            3_600
        )
        .is_ok());
        assert!(validate_action(
            &request("mesh.introduce", serde_json::json!({"scope":"global"})),
            3_600
        )
        .is_err());
    }

    #[test]
    fn wire_bytes_require_canonical_unpadded_base64url() {
        let mut action = RemoteAction {
            schema: REMOTE_ACTION_SCHEMA.to_owned(),
            action_id: Uuid::from_u128(1),
            companion_id: Uuid::from_u128(2),
            key_version: 1,
            nonce_b64: URL_SAFE_NO_PAD.encode([0_u8; 16]),
            action_type: "companion.pet".to_owned(),
            created_epoch: "1800000000".to_owned(),
            expires_epoch: "1800000060".to_owned(),
            params_b64: URL_SAFE_NO_PAD.encode(b"{}"),
            signature_b64: URL_SAFE_NO_PAD.encode([0_u8; 32]),
        };
        assert!(action.validate_wrapper().is_ok());
        action.params_b64.push('=');
        assert!(action.validate_wrapper().is_err());
    }

    #[test]
    fn wire_uuid_text_is_canonical_lowercase_and_non_nil() {
        let action = serde_json::json!({
            "schema": REMOTE_ACTION_SCHEMA,
            "action_id": "00112233-4455-6677-8899-aabbccddeeff",
            "companion_id": "10213243-5465-7687-98a9-bacbdcedfe0f",
            "key_version": 1,
            "nonce_b64": URL_SAFE_NO_PAD.encode([0_u8; 16]),
            "action_type": "companion.pet",
            "created_epoch": "1800000000",
            "expires_epoch": "1800000060",
            "params_b64": URL_SAFE_NO_PAD.encode(b"{}"),
            "signature_b64": URL_SAFE_NO_PAD.encode([0_u8; 32])
        });
        assert!(serde_json::from_value::<RemoteAction>(action.clone()).is_ok());
        let mut uppercase = action.clone();
        uppercase["action_id"] = Value::String("00112233-4455-6677-8899-AABBCCDDEEFF".to_owned());
        assert!(serde_json::from_value::<RemoteAction>(uppercase).is_err());
        let mut compact = action.clone();
        compact["action_id"] = Value::String("00112233445566778899aabbccddeeff".to_owned());
        assert!(serde_json::from_value::<RemoteAction>(compact).is_err());
        let mut nil = action;
        nil["companion_id"] = Value::String(Uuid::nil().to_string());
        assert!(serde_json::from_value::<RemoteAction>(nil).is_err());
    }
}
