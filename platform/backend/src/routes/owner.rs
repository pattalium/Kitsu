use axum::{
    extract::{ConnectInfo, Extension, Path, Query, State},
    http::{header, HeaderMap, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    Json,
};
use chrono::Utc;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::net::SocketAddr;
use uuid::Uuid;

use crate::{
    auth::OwnerAuth,
    client_ip::trusted_client_ip,
    crypto::{
        canonical_request_hash, decrypt_companion_secret, random_token, sha256, sha256_text,
        sign_remote_action,
    },
    db::{
        AccountDeletionView, ActionView, CertificateRotationView, CompanionListItem,
        EnrollmentView, GatewayBootstrapView, GatewayCatalogView, PublicContactView,
    },
    error::ApiError,
    state::AppState,
    wire::{validate_action, CreateActionRequest},
};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateEnrollmentRequest {
    hardware_uid: String,
    display_name: String,
}

#[derive(Serialize)]
pub struct CreateEnrollmentResponse {
    enrollment: EnrollmentView,
    /// Returned exactly once. The service stores only SHA-256.
    claim_token: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateGatewayBootstrapRequest {
    display_name: String,
}

#[derive(Serialize)]
pub struct CreateGatewayBootstrapResponse {
    bootstrap: GatewayBootstrapView,
    /// Returned exactly once. The service stores only SHA-256.
    claim_token: String,
}

#[derive(Serialize)]
pub struct CreateCertificateRotationResponse {
    rotation: CertificateRotationView,
    /// Returned once; only its digest is durable.
    claim_token: String,
}

#[derive(Serialize)]
pub struct CertificateRevokedResponse {
    revoked: bool,
    gateway_id: Uuid,
    certificate_id: Uuid,
}

#[derive(Deserialize)]
pub struct CursorQuery {
    after: Option<String>,
    limit: Option<i64>,
}

#[derive(Serialize)]
pub struct ListEnvelope<T> {
    items: T,
}

#[derive(Serialize)]
pub struct ChannelView {
    /// MeshCore channel slot. The firmware supports exactly slots 0 through 3.
    slot: u8,
    name: Option<String>,
    configured: Option<bool>,
    max_utf8_bytes: u16,
}

#[derive(Serialize)]
pub struct ClientAttributionView {
    address: String,
    provenance: &'static str,
}

pub async fn client_attribution(
    State(state): State<AppState>,
    Extension(_auth): Extension<OwnerAuth>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
) -> Result<Json<ClientAttributionView>, ApiError> {
    let address = trusted_client_ip(&state.config, remote, &headers)?;
    Ok(Json(ClientAttributionView {
        address: address.to_string(),
        provenance: "trusted_immediate_proxy",
    }))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RequestAccountDeletion {
    confirmation: String,
}

pub async fn account_deletion(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
) -> Result<Json<Option<AccountDeletionView>>, ApiError> {
    Ok(Json(state.db.account_deletion(auth.owner.id).await?))
}

pub async fn request_account_deletion(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Json(request): Json<RequestAccountDeletion>,
) -> Result<Response, ApiError> {
    if request.confirmation != "DELETE MY KITSU ACCOUNT" {
        return Err(ApiError::Invalid(
            "account deletion confirmation is incorrect",
        ));
    }
    let subject = sha256(auth.owner.id.as_bytes());
    state
        .db
        .check_rate_limit("owner.account.delete", &subject, 3, 86_400)
        .await?;
    let view = state
        .db
        .request_account_deletion(auth.owner.id, state.config.account_deletion_grace)
        .await?;
    Ok(one_time_secret_response(StatusCode::ACCEPTED, view))
}

pub async fn cancel_account_deletion(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
) -> Result<Json<AccountDeletionView>, ApiError> {
    Ok(Json(state.db.cancel_account_deletion(auth.owner.id).await?))
}

pub async fn list_companions(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
) -> Result<Json<ListEnvelope<Vec<CompanionListItem>>>, ApiError> {
    Ok(Json(ListEnvelope {
        items: state.db.list_companions(auth.owner.id).await?,
    }))
}

pub async fn list_public_contacts(
    State(state): State<AppState>,
    Extension(_auth): Extension<OwnerAuth>,
    Query(query): Query<CursorQuery>,
) -> Result<Json<ListEnvelope<Vec<PublicContactView>>>, ApiError> {
    Ok(Json(ListEnvelope {
        items: state
            .db
            .list_public_contacts(query.limit.unwrap_or(100))
            .await?,
    }))
}

pub async fn resolve_public_contact(
    State(state): State<AppState>,
    Extension(_auth): Extension<OwnerAuth>,
    Path(id): Path<Uuid>,
) -> Result<Json<PublicContactView>, ApiError> {
    Ok(Json(state.db.resolve_public_contact(id).await?))
}

pub async fn list_gateways(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
) -> Result<Json<ListEnvelope<Vec<GatewayCatalogView>>>, ApiError> {
    Ok(Json(ListEnvelope {
        items: state.db.list_gateway_catalog(auth.owner.id).await?,
    }))
}

pub async fn create_enrollment(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Json(request): Json<CreateEnrollmentRequest>,
) -> Result<Response, ApiError> {
    let owner_subject = sha256(auth.owner.id.as_bytes());
    state
        .db
        .check_rate_limit("owner.enrollment.create", &owner_subject, 10, 3600)
        .await?;
    let claim_token = random_token(32);
    let enrollment = state
        .db
        .create_enrollment(
            auth.owner.id,
            &request.hardware_uid,
            &request.display_name,
            &sha256_text(claim_token.as_str()),
            state.config.enrollment_ttl,
        )
        .await?;
    Ok(one_time_secret_response(
        StatusCode::CREATED,
        CreateEnrollmentResponse {
            enrollment,
            claim_token: claim_token.to_string(),
        },
    ))
}

pub async fn create_gateway_bootstrap(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Json(request): Json<CreateGatewayBootstrapRequest>,
) -> Result<Response, ApiError> {
    let owner_subject = sha256(auth.owner.id.as_bytes());
    state
        .db
        .check_rate_limit("owner.gateway.bootstrap.create", &owner_subject, 10, 3600)
        .await?;
    let claim_token = random_token(32);
    let bootstrap = state
        .db
        .create_gateway_bootstrap(
            auth.owner.id,
            &request.display_name,
            &sha256_text(&claim_token),
            state.config.gateway_bootstrap_ttl,
        )
        .await?;
    Ok(one_time_secret_response(
        StatusCode::CREATED,
        CreateGatewayBootstrapResponse {
            bootstrap,
            claim_token: claim_token.to_string(),
        },
    ))
}

pub async fn create_certificate_rotation(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(gateway_id): Path<Uuid>,
) -> Result<Response, ApiError> {
    let owner_subject = sha256(auth.owner.id.as_bytes());
    state
        .db
        .check_rate_limit("owner.gateway.certificate.rotate", &owner_subject, 10, 3600)
        .await?;
    let claim_token = random_token(32);
    let rotation = state
        .db
        .create_certificate_rotation(
            auth.owner.id,
            gateway_id,
            &sha256_text(claim_token.as_str()),
            state.config.certificate_rotation_ttl,
            state.config.certificate_overlap,
        )
        .await?;
    Ok(one_time_secret_response(
        StatusCode::CREATED,
        CreateCertificateRotationResponse {
            rotation,
            claim_token: claim_token.to_string(),
        },
    ))
}

fn one_time_secret_response<T: Serialize>(status: StatusCode, body: T) -> Response {
    let mut response = (status, Json(body)).into_response();
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    response
}

pub async fn revoke_gateway_certificate(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path((gateway_id, certificate_id)): Path<(Uuid, Uuid)>,
) -> Result<Json<CertificateRevokedResponse>, ApiError> {
    state
        .db
        .revoke_gateway_certificate(auth.owner.id, gateway_id, certificate_id)
        .await?;
    let revoked = state.db.revoked_certificates().await?;
    state.certificate_issuer.publish_crl(&revoked).await?;
    Ok(Json(CertificateRevokedResponse {
        revoked: true,
        gateway_id,
        certificate_id,
    }))
}

pub async fn snapshot(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(companion_id): Path<Uuid>,
    headers: HeaderMap,
) -> Result<Response, ApiError> {
    let projection = state.db.snapshot(auth.owner.id, companion_id).await?;
    let canonical = serde_jcs::to_vec(&projection).map_err(ApiError::internal)?;
    let etag = format!("\"{}\"", hex::encode(sha256(&canonical)));
    if headers
        .get(header::IF_NONE_MATCH)
        .and_then(|value| value.to_str().ok())
        .is_some_and(|candidate| candidate.split(',').any(|item| item.trim() == etag))
    {
        let mut response = StatusCode::NOT_MODIFIED.into_response();
        snapshot_headers(response.headers_mut(), &etag)?;
        return Ok(response);
    }
    let mut response = Json(projection).into_response();
    snapshot_headers(response.headers_mut(), &etag)?;
    Ok(response)
}

pub async fn peers(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(companion_id): Path<Uuid>,
) -> Result<Json<ListEnvelope<Vec<Value>>>, ApiError> {
    Ok(Json(ListEnvelope {
        items: state.db.list_peers(auth.owner.id, companion_id).await?,
    }))
}

pub async fn channels(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(companion_id): Path<Uuid>,
) -> Result<Json<ListEnvelope<Vec<ChannelView>>>, ApiError> {
    // Ownership is checked against PostgreSQL even though channel metadata is
    // currently the firmware's fixed four-slot contract. This avoids turning
    // the endpoint into a companion-existence oracle.
    let snapshot = state
        .db
        .latest_device_snapshot(auth.owner.id, companion_id)
        .await?;
    let reported = snapshot
        .as_ref()
        .and_then(Value::as_object)
        .filter(|body| {
            body.get("schema").and_then(Value::as_str) == Some("kitsu.companion-snapshot.v1")
        })
        .and_then(|body| body.get("channels"))
        .and_then(Value::as_array);
    Ok(Json(ListEnvelope {
        items: (0_u8..=3)
            .map(|slot| {
                let channel = reported.and_then(|items| {
                    items.iter().find(|item| {
                        item.get("slot").and_then(Value::as_u64) == Some(u64::from(slot))
                    })
                });
                let configured = channel
                    .and_then(|item| item.get("configured"))
                    .and_then(Value::as_bool);
                let name = channel
                    .filter(|_| configured == Some(true))
                    .and_then(|item| item.get("name"))
                    .and_then(Value::as_str)
                    .filter(|name| {
                        !name.is_empty() && name.len() <= 32 && !name.chars().any(char::is_control)
                    })
                    .map(ToOwned::to_owned);
                ChannelView {
                    slot,
                    name,
                    configured,
                    max_utf8_bytes: 128,
                }
            })
            .collect(),
    }))
}

pub async fn events(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(companion_id): Path<Uuid>,
    Query(query): Query<CursorQuery>,
) -> Result<Json<ListEnvelope<Vec<Value>>>, ApiError> {
    let after = parse_cursor(query.after.as_deref())?;
    Ok(Json(ListEnvelope {
        items: state
            .db
            .list_events(
                auth.owner.id,
                companion_id,
                after,
                query.limit.unwrap_or(100),
            )
            .await?,
    }))
}

pub async fn list_actions(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(companion_id): Path<Uuid>,
    Query(query): Query<CursorQuery>,
) -> Result<Json<ListEnvelope<Vec<ActionView>>>, ApiError> {
    Ok(Json(ListEnvelope {
        items: state
            .db
            .list_actions(auth.owner.id, companion_id, query.limit.unwrap_or(100))
            .await?,
    }))
}

pub async fn create_action(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
    Path(companion_id): Path<Uuid>,
    headers: HeaderMap,
    Json(request): Json<CreateActionRequest>,
) -> Result<(StatusCode, Json<ActionView>), ApiError> {
    let idempotency_key = headers
        .get("idempotency-key")
        .and_then(|value| value.to_str().ok())
        .filter(|value| {
            (8..=128).contains(&value.len())
                && value
                    .bytes()
                    .all(|byte| byte.is_ascii_graphic() && byte != b'"' && byte != b'\\')
        })
        .ok_or(ApiError::Invalid(
            "a valid Idempotency-Key header is required",
        ))?;
    validate_action(&request, state.config.action_max_ttl.as_secs())?;
    let subject = sha256(auth.owner.id.as_bytes());
    state
        .db
        .check_rate_limit("owner.action.create", &subject, 120, 60)
        .await?;
    let request_hash = canonical_request_hash(&json!({
        "companion_id": companion_id,
        "request": &request
    }))?;
    let secret_record = state
        .db
        .action_secret_for_owner(auth.owner.id, companion_id)
        .await?;
    let dek = state
        .kms
        .decrypt_data_key(
            secret_record.companion_id,
            secret_record.key_version,
            &secret_record.kms_key_id,
            &secret_record.wrapped_dek,
        )
        .await?;
    let companion_secret = decrypt_companion_secret(
        secret_record.companion_id,
        secret_record.key_version,
        &dek,
        &secret_record.encrypted,
    )?;
    let params = serde_jcs::to_vec(&request.parameters).map_err(ApiError::internal)?;
    let created_epoch = Utc::now().timestamp();
    let expires_epoch = created_epoch
        .checked_add(i64::try_from(request.expires_in_seconds).map_err(ApiError::internal)?)
        .ok_or(ApiError::Invalid("action expiry is outside policy"))?;
    let wire = sign_remote_action(
        Uuid::new_v4(),
        companion_id,
        secret_record.key_version,
        &request.action_type,
        created_epoch,
        expires_epoch,
        &params,
        &companion_secret,
    )?;
    let created = state
        .db
        .create_action(
            auth.owner.id,
            companion_id,
            idempotency_key,
            &request_hash,
            &request,
            &wire,
        )
        .await?;
    let action = created.view;

    if action.status == "queued" {
        if let Some(gateway_id) = state.db.gateway_for_companion(companion_id).await? {
            let _ = state.hubs.send_gateway(gateway_id, created.wire).await;
        }
    }
    state
        .hubs
        .broadcast_owner(
            auth.owner.id,
            json!({
                "type": "action.changed",
                "companion_id": companion_id,
                "action_id": action.id,
                "status": action.status,
                "server_epoch": Utc::now().timestamp()
            }),
        )
        .await;
    Ok((StatusCode::ACCEPTED, Json(action)))
}

fn parse_cursor(raw: Option<&str>) -> Result<i64, ApiError> {
    let Some(raw) = raw else { return Ok(0) };
    if raw.is_empty()
        || raw.len() > 19
        || (raw.len() > 1 && raw.starts_with('0'))
        || !raw.bytes().all(|byte| byte.is_ascii_digit())
    {
        return Err(ApiError::Invalid("invalid cursor"));
    }
    raw.parse::<i64>()
        .map_err(|_| ApiError::Invalid("invalid cursor"))
}

fn snapshot_headers(headers: &mut HeaderMap, etag: &str) -> Result<(), ApiError> {
    headers.insert(
        header::ETAG,
        HeaderValue::from_str(etag).map_err(ApiError::internal)?,
    );
    headers.insert(
        header::CACHE_CONTROL,
        HeaderValue::from_static("private, max-age=0, must-revalidate"),
    );
    headers.insert(
        header::VARY,
        HeaderValue::from_static("Authorization, Cookie"),
    );
    Ok(())
}
