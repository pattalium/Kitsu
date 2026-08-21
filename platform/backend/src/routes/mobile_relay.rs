use axum::{
    extract::{ws::WebSocketUpgrade, Extension, Path, State},
    http::HeaderMap,
    response::Response,
    Json,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

use crate::{
    auth::{AuthMode, OwnerAuth},
    crypto::sha256,
    db::MobileRelayView,
    error::ApiError,
    routes::gateway::{self, ClaimRequest, ClaimResponse, EnvelopeAccepted},
    state::AppState,
    wire::DeviceEnvelope,
};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PutMobileRelayRequest {
    gateway_id: Uuid,
}

#[derive(Serialize)]
pub struct MobileRelayResponse {
    installation_id: Uuid,
    gateway_id: Uuid,
    created_at: DateTime<Utc>,
    ca_cert_der_b64: String,
}

pub async fn put_mobile_relay(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(installation_id): Path<Uuid>,
    Json(request): Json<PutMobileRelayRequest>,
) -> Result<Json<MobileRelayResponse>, ApiError> {
    let owner_id = native_owner_id(&auth)?;
    if installation_id.is_nil() || request.gateway_id.is_nil() {
        return Err(ApiError::Invalid("invalid mobile relay identity"));
    }
    state
        .db
        .check_rate_limit(
            "owner.mobile_relay.create",
            &sha256(owner_id.as_bytes()),
            20,
            3600,
        )
        .await?;
    let view = state
        .db
        .create_or_get_mobile_relay(owner_id, installation_id, request.gateway_id)
        .await?;
    Ok(Json(relay_response(&state, view)))
}

pub async fn get_mobile_relay(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(installation_id): Path<Uuid>,
) -> Result<Json<MobileRelayResponse>, ApiError> {
    let owner_id = native_owner_id(&auth)?;
    let view = state.db.mobile_relay(owner_id, installation_id).await?.view;
    Ok(Json(relay_response(&state, view)))
}

pub async fn claim_enrollment(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path((installation_id, enrollment_id)): Path<(Uuid, Uuid)>,
    Json(request): Json<ClaimRequest>,
) -> Result<Json<ClaimResponse>, ApiError> {
    let owner_id = native_owner_id(&auth)?;
    let relay = state.db.mobile_relay(owner_id, installation_id).await?;
    state
        .db
        .check_rate_limit(
            "mobile_relay.enrollment.claim",
            &relay_rate_subject(owner_id, installation_id),
            20,
            600,
        )
        .await?;
    gateway::claim_enrollment_for_gateway(&state, &relay.gateway, enrollment_id, request).await
}

pub async fn ingest_envelope(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(installation_id): Path<Uuid>,
    headers: HeaderMap,
    Json(envelope): Json<DeviceEnvelope>,
) -> Result<Json<EnvelopeAccepted>, ApiError> {
    let owner_id = native_owner_id(&auth)?;
    let spool_record_id = gateway::canonical_spool_record_id(&headers)?;
    let relay = state.db.mobile_relay(owner_id, installation_id).await?;
    let sequence = envelope.sequence.clone();
    gateway::process_envelope(&state, &relay.gateway, &envelope).await?;
    Ok(Json(EnvelopeAccepted {
        accepted: true,
        spool_record_id,
        sequence,
    }))
}

pub async fn session(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(installation_id): Path<Uuid>,
    upgrade: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    let owner_id = native_owner_id(&auth)?;
    let relay = state.db.mobile_relay(owner_id, installation_id).await?;
    state
        .db
        .check_rate_limit(
            "mobile_relay.session.open",
            &relay_rate_subject(owner_id, installation_id),
            120,
            60,
        )
        .await?;
    Ok(upgrade
        .max_message_size(64 * 1024)
        .on_upgrade(move |socket| gateway::gateway_socket(state, relay.gateway, socket)))
}

fn native_owner_id(auth: &OwnerAuth) -> Result<Uuid, ApiError> {
    if auth.mode != AuthMode::Bearer {
        return Err(ApiError::Forbidden);
    }
    Ok(auth.owner.id)
}

fn relay_rate_subject(owner_id: Uuid, installation_id: Uuid) -> [u8; 32] {
    let mut subject = [0_u8; 32];
    subject[..16].copy_from_slice(owner_id.as_bytes());
    subject[16..].copy_from_slice(installation_id.as_bytes());
    sha256(&subject)
}

pub(crate) fn relay_response(state: &AppState, view: MobileRelayView) -> MobileRelayResponse {
    MobileRelayResponse {
        installation_id: view.installation_id,
        gateway_id: view.gateway_id,
        created_at: view.created_at,
        ca_cert_der_b64: URL_SAFE_NO_PAD.encode(state.enrollment_ca_cert_der.as_slice()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::Owner;

    fn auth(mode: AuthMode) -> OwnerAuth {
        OwnerAuth {
            owner: Owner {
                id: Uuid::new_v4(),
                issuer: "https://issuer.example.invalid".to_owned(),
                subject: "owner".to_owned(),
            },
            mode,
            browser_session_id: None,
            csrf_digest: None,
            expires_at: None,
        }
    }

    #[test]
    fn relay_routes_require_native_bearer_and_strict_create_body() {
        assert!(native_owner_id(&auth(AuthMode::Bearer)).is_ok());
        assert!(matches!(
            native_owner_id(&auth(AuthMode::Browser)),
            Err(ApiError::Forbidden)
        ));
        assert!(
            serde_json::from_value::<PutMobileRelayRequest>(serde_json::json!({
                "gateway_id": Uuid::new_v4()
            }))
            .is_ok()
        );
        assert!(
            serde_json::from_value::<PutMobileRelayRequest>(serde_json::json!({
                "gateway_id": Uuid::new_v4(),
                "unexpected": true
            }))
            .is_err()
        );
        let response = serde_json::to_value(MobileRelayResponse {
            installation_id: Uuid::new_v4(),
            gateway_id: Uuid::new_v4(),
            created_at: Utc::now(),
            ca_cert_der_b64: "MAEA".to_owned(),
        })
        .unwrap();
        let fields = response.as_object().unwrap();
        assert_eq!(fields.len(), 4);
        assert!(fields.contains_key("installation_id"));
        assert!(fields.contains_key("gateway_id"));
        assert!(fields.contains_key("created_at"));
        assert_eq!(fields["ca_cert_der_b64"], "MAEA");
    }
}
