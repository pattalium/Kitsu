use std::{
    net::SocketAddr,
    sync::atomic::{AtomicBool, Ordering},
};

use axum::{
    extract::{ConnectInfo, Path, State},
    http::HeaderMap,
    Json,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

use crate::{
    client_ip::trusted_client_ip,
    crypto::{sha256, sha256_text},
    db::{BeginGatewayBootstrap, GatewayBootstrapResult, ReservedGatewayBootstrap},
    error::ApiError,
    issuer::{CertificateProfile, IssueCertificateRequest},
    pki::{validate_issued_certificate, validate_p256_csr, ValidatedCsr},
    state::AppState,
};

const PROVIDER_BEGIN_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClaimGatewayBootstrapRequest {
    claim_token: String,
    gateway_csr_der_b64: String,
}

#[derive(Serialize)]
pub struct ClaimGatewayBootstrapResponse {
    gateway_id: Uuid,
    device_certificate_der_b64: String,
    device_certificate_chain_der_b64: Vec<String>,
}

pub async fn claim_gateway_bootstrap(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(bootstrap_id): Path<Uuid>,
    Json(request): Json<ClaimGatewayBootstrapRequest>,
) -> Result<Json<ClaimGatewayBootstrapResponse>, ApiError> {
    if bootstrap_id.is_nil() || request.claim_token.is_empty() || request.claim_token.len() > 256 {
        return Err(ApiError::Invalid("invalid gateway bootstrap claim"));
    }
    if request.gateway_csr_der_b64.len() > 5_464 {
        return Err(ApiError::Invalid("invalid gateway CSR"));
    }
    let client_ip = trusted_client_ip(&state.config, remote, &headers)?;
    let remote_subject = sha256(client_ip.to_string().as_bytes());
    state
        .db
        .check_rate_limit("gateway.bootstrap.claim", &remote_subject, 20, 600)
        .await?;
    let csr_der = decode_canonical_base64url(&request.gateway_csr_der_b64, "invalid gateway CSR")?;
    let csr = validate_p256_csr(&csr_der)?;
    let request_sha256 = sha256(&csr_der);
    match state
        .db
        .begin_gateway_bootstrap(
            bootstrap_id,
            &sha256_text(&request.claim_token),
            &request_sha256,
        )
        .await?
    {
        BeginGatewayBootstrap::Completed(result) => Ok(Json(response(result))),
        BeginGatewayBootstrap::Issue(reserved) => {
            let provider_boundary = AtomicBool::new(reserved.provider_job_id.is_some());
            let result = tokio::time::timeout(
                state.config.certificate_issue_timeout + std::time::Duration::from_secs(30),
                issue_gateway_certificate(
                    &state,
                    &reserved,
                    &csr,
                    &request_sha256,
                    &provider_boundary,
                ),
            )
            .await
            .map_err(|_| ApiError::Unavailable)
            .and_then(|result| result);
            if result.is_err() {
                let cleanup = if provider_boundary.load(Ordering::Acquire) {
                    state
                        .db
                        .release_gateway_bootstrap(reserved.id, reserved.issuance_id)
                        .await
                } else {
                    state
                        .db
                        .abort_gateway_bootstrap(reserved.id, reserved.issuance_id)
                        .await
                };
                if let Err(error) = cleanup {
                    tracing::error!(error = %error, "failed to release gateway bootstrap lease");
                }
            }
            result.map(|result| Json(response(result)))
        }
    }
}

async fn issue_gateway_certificate(
    state: &AppState,
    reserved: &ReservedGatewayBootstrap,
    csr: &ValidatedCsr,
    request_sha256: &[u8; 32],
    provider_boundary: &AtomicBool,
) -> Result<GatewayBootstrapResult, ApiError> {
    let san_uri = format!("urn:kitsu:gateway:{}", reserved.gateway_id);
    // AWS Private CA idempotency is scoped to an account/region, not to this
    // bootstrap row. Include the bootstrap identity so reusing one CSR for two
    // gateways can never return a certificate carrying the first gateway SAN.
    let mut issuer_request = [0_u8; 48];
    issuer_request[..16].copy_from_slice(reserved.id.as_bytes());
    issuer_request[16..].copy_from_slice(request_sha256);
    let issuer_request_sha256 = sha256(&issuer_request);
    let provider_job_id = if let Some(provider_job_id) = &reserved.provider_job_id {
        provider_job_id.clone()
    } else {
        state.db.mark_gateway_provider_attempt(reserved).await?;
        provider_boundary.store(true, Ordering::Release);
        let provider_job_id = tokio::time::timeout(
            PROVIDER_BEGIN_TIMEOUT,
            state.certificate_issuer.begin(IssueCertificateRequest {
                profile: CertificateProfile::Gateway,
                csr_der: csr.der.clone(),
                san_uri: san_uri.clone(),
                idempotency_key: hex::encode(&issuer_request_sha256[..16]),
            }),
        )
        .await
        .map_err(|_| ApiError::Unavailable)??;
        state
            .db
            .record_gateway_provider_job(reserved, &provider_job_id)
            .await?;
        provider_job_id
    };
    let raw_certificate = state.certificate_issuer.finish(&provider_job_id).await?;
    let certificate =
        validate_issued_certificate(raw_certificate, csr, &san_uri, chrono::Utc::now())?;
    state
        .db
        .complete_gateway_bootstrap(reserved, &certificate)
        .await
}

fn response(result: GatewayBootstrapResult) -> ClaimGatewayBootstrapResponse {
    ClaimGatewayBootstrapResponse {
        gateway_id: result.gateway_id,
        device_certificate_der_b64: URL_SAFE_NO_PAD.encode(result.certificate_der),
        device_certificate_chain_der_b64: result
            .certificate_chain_der
            .into_iter()
            .map(|certificate| URL_SAFE_NO_PAD.encode(certificate))
            .collect(),
    }
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
