use std::net::SocketAddr;

use axum::{
    extract::{ws::WebSocketUpgrade, ConnectInfo, Path, State},
    http::{header, HeaderMap, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    Json,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::{Deserialize, Serialize};
use uuid::Uuid;
use zeroize::Zeroizing;

use crate::{
    client_ip::trusted_client_ip,
    crypto::{device_relay_source_digest, random_token, sha256, sha256_text},
    db::EnrollmentView,
    error::ApiError,
    routes::{
        gateway::{self, ClaimRequest, EnvelopeAccepted},
        mobile_relay::{relay_response, MobileRelayResponse},
    },
    state::AppState,
    wire::DeviceEnvelope,
};

const RELAY_AUTH_SCHEME: &str = "KitsuRelay ";

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PutDeviceRelayRequest {
    gateway_id: Uuid,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateDeviceRelayEnrollmentRequest {
    hardware_uid: String,
    display_name: String,
}

#[derive(Serialize)]
pub struct CreateDeviceRelayEnrollmentResponse {
    enrollment: EnrollmentView,
    /// Returned exactly once. The service stores only SHA-256.
    claim_token: String,
}

pub async fn put_device_relay(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    Path(installation_id): Path<Uuid>,
    headers: HeaderMap,
    Json(request): Json<PutDeviceRelayRequest>,
) -> Result<Json<MobileRelayResponse>, ApiError> {
    let credential_digest = relay_credential_digest(&headers)?;
    match state
        .db
        .device_relay(installation_id, &credential_digest)
        .await
    {
        Ok(existing) => {
            if existing.relay.view.gateway_id != request.gateway_id {
                return Err(ApiError::Conflict(
                    "device relay is bound to another gateway",
                ));
            }
            return Ok(Json(relay_response(&state, existing.relay.view)));
        }
        Err(ApiError::Unauthorized) => {}
        Err(error) => return Err(error),
    }
    let source_address = trusted_client_ip(&state.config, remote, &headers)?;
    let source_digest = device_relay_source_digest(
        &state.config.browser_state_key.0,
        source_address.to_string().as_str(),
    );
    // This is the one account-free row-creation boundary. Attribute it to the
    // trusted immediate-proxy address before attempting any inserts.
    state
        .db
        .check_rate_limit("device_relay.create.source", &source_digest, 10, 3600)
        .await?;
    state
        .db
        .check_rate_limit(
            "device_relay.create.credential",
            &credential_digest,
            20,
            3600,
        )
        .await?;
    let relay = state
        .db
        .create_or_get_device_relay(installation_id, request.gateway_id, &credential_digest)
        .await?;
    Ok(Json(relay_response(&state, relay.relay.view)))
}

pub async fn get_device_relay(
    State(state): State<AppState>,
    Path(installation_id): Path<Uuid>,
    headers: HeaderMap,
) -> Result<Json<MobileRelayResponse>, ApiError> {
    let credential_digest = relay_credential_digest(&headers)?;
    let relay = state
        .db
        .device_relay(installation_id, &credential_digest)
        .await?;
    Ok(Json(relay_response(&state, relay.relay.view)))
}

pub async fn create_enrollment(
    State(state): State<AppState>,
    Path(installation_id): Path<Uuid>,
    headers: HeaderMap,
    Json(request): Json<CreateDeviceRelayEnrollmentRequest>,
) -> Result<Response, ApiError> {
    let credential_digest = relay_credential_digest(&headers)?;
    state
        .db
        .check_rate_limit(
            "device_relay.enrollment.create",
            &credential_digest,
            10,
            3600,
        )
        .await?;
    let claim_token = random_token(32);
    let enrollment = state
        .db
        .create_device_relay_enrollment(
            installation_id,
            &credential_digest,
            &request.hardware_uid,
            &request.display_name,
            &sha256_text(claim_token.as_str()),
            state.config.enrollment_ttl,
        )
        .await?;
    Ok(one_time_secret_response(
        StatusCode::CREATED,
        CreateDeviceRelayEnrollmentResponse {
            enrollment,
            claim_token: claim_token.to_string(),
        },
    ))
}

pub async fn claim_enrollment(
    State(state): State<AppState>,
    Path((installation_id, enrollment_id)): Path<(Uuid, Uuid)>,
    headers: HeaderMap,
    Json(request): Json<ClaimRequest>,
) -> Result<Response, ApiError> {
    let credential_digest = relay_credential_digest(&headers)?;
    // Authenticate and rate-limit before the shared handler parses the
    // caller-controlled P-256 material.
    let relay = state
        .db
        .device_relay(installation_id, &credential_digest)
        .await?;
    state
        .db
        .check_rate_limit("device_relay.enrollment.claim", &credential_digest, 20, 600)
        .await?;
    let Json(response) =
        gateway::claim_enrollment_for_gateway(&state, &relay.relay.gateway, enrollment_id, request)
            .await?;
    // The shared claim is replay-safe. Repeating the exact completed claim
    // also repairs a crash between certificate commit and this activation.
    state
        .db
        .activate_device_relay(installation_id, enrollment_id, &credential_digest)
        .await?;
    Ok(one_time_secret_response(StatusCode::OK, response))
}

pub async fn ingest_envelope(
    State(state): State<AppState>,
    Path(installation_id): Path<Uuid>,
    headers: HeaderMap,
    Json(envelope): Json<DeviceEnvelope>,
) -> Result<Json<EnvelopeAccepted>, ApiError> {
    let credential_digest = relay_credential_digest(&headers)?;
    let relay = state
        .db
        .device_relay(installation_id, &credential_digest)
        .await?;
    if !relay.activated {
        return Err(ApiError::Forbidden);
    }
    let spool_record_id = gateway::canonical_spool_record_id(&headers)?;
    let sequence = envelope.sequence.clone();
    gateway::process_envelope(&state, &relay.relay.gateway, &envelope).await?;
    Ok(Json(EnvelopeAccepted {
        accepted: true,
        spool_record_id,
        sequence,
    }))
}

pub async fn session(
    State(state): State<AppState>,
    Path(installation_id): Path<Uuid>,
    headers: HeaderMap,
    upgrade: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    let credential_digest = relay_credential_digest(&headers)?;
    let relay = state
        .db
        .device_relay(installation_id, &credential_digest)
        .await?;
    if !relay.activated {
        return Err(ApiError::Forbidden);
    }
    state
        .db
        .check_rate_limit("device_relay.session.open", &credential_digest, 120, 60)
        .await?;
    Ok(upgrade
        .max_message_size(64 * 1024)
        .on_upgrade(move |socket| gateway::gateway_socket(state, relay.relay.gateway, socket)))
}

fn relay_credential_digest(headers: &HeaderMap) -> Result<[u8; 32], ApiError> {
    let value = headers
        .get(header::AUTHORIZATION)
        .and_then(|value| value.to_str().ok())
        .ok_or(ApiError::Unauthorized)?;
    let token = value
        .strip_prefix(RELAY_AUTH_SCHEME)
        .filter(|token| token.len() == 43)
        .ok_or(ApiError::Unauthorized)?;
    let decoded = Zeroizing::new(
        URL_SAFE_NO_PAD
            .decode(token)
            .map_err(|_| ApiError::Unauthorized)?,
    );
    if decoded.len() != 32 || URL_SAFE_NO_PAD.encode(decoded.as_slice()) != token {
        return Err(ApiError::Unauthorized);
    }
    Ok(sha256(decoded.as_slice()))
}

fn one_time_secret_response<T: Serialize>(status: StatusCode, body: T) -> Response {
    let mut response = (status, Json(body)).into_response();
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    response
}

#[cfg(test)]
mod tests {
    use super::*;

    fn authorization(value: &str) -> HeaderMap {
        let mut headers = HeaderMap::new();
        headers.insert(header::AUTHORIZATION, HeaderValue::from_str(value).unwrap());
        headers
    }

    #[test]
    fn relay_credential_is_exact_canonical_base64url() {
        let raw = [0x5a_u8; 32];
        let token = URL_SAFE_NO_PAD.encode(raw);
        let headers = authorization(&format!("{RELAY_AUTH_SCHEME}{token}"));
        assert_eq!(relay_credential_digest(&headers).unwrap(), sha256(&raw));

        let malformed = [
            String::new(),
            format!("Bearer {token}"),
            format!("{RELAY_AUTH_SCHEME}{token}="),
            format!("{RELAY_AUTH_SCHEME} {token}"),
            format!("{RELAY_AUTH_SCHEME}{}", URL_SAFE_NO_PAD.encode([0_u8; 31])),
        ];
        assert!(relay_credential_digest(&HeaderMap::new()).is_err());
        for value in malformed {
            assert!(matches!(
                relay_credential_digest(&authorization(&value)),
                Err(ApiError::Unauthorized)
            ));
        }
    }

    #[test]
    fn create_bodies_reject_unknown_fields() {
        assert!(
            serde_json::from_value::<PutDeviceRelayRequest>(serde_json::json!({
                "gateway_id": Uuid::new_v4()
            }))
            .is_ok()
        );
        assert!(
            serde_json::from_value::<PutDeviceRelayRequest>(serde_json::json!({
                "gateway_id": Uuid::new_v4(),
                "extra": true
            }))
            .is_err()
        );
        assert!(
            serde_json::from_value::<CreateDeviceRelayEnrollmentRequest>(serde_json::json!({
                "hardware_uid": "device-1234",
                "display_name": "Kitsu"
            }))
            .is_ok()
        );
        assert!(
            serde_json::from_value::<CreateDeviceRelayEnrollmentRequest>(serde_json::json!({
                "hardware_uid": "device-1234",
                "display_name": "Kitsu",
                "extra": true
            }))
            .is_err()
        );
    }
}
