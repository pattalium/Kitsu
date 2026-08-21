use std::{
    net::SocketAddr,
    sync::atomic::{AtomicBool, Ordering},
    time::Duration,
};

use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        ConnectInfo, Path, State,
    },
    http::{HeaderMap, StatusCode},
    response::Response,
    Json,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use uuid::Uuid;
use x509_parser::parse_x509_certificate;

use crate::{
    crypto::{
        decrypt_companion_secret, device_transcript, encrypt_companion_secret, random_array,
        sha256, sha256_text, verify_device_hmac,
    },
    db::{
        ActivatedCertificateView, BeginEnrollmentIssuance, EnrollmentIssuanceResult, Gateway,
        IngestOutcome, NewSecretRecord, ReservedEnrollment,
    },
    error::ApiError,
    issuer::{CertificateProfile, IssueCertificateRequest},
    mtls::{extract_mtls_identity, MtlsHeaders, MtlsIdentity},
    pki::{
        device_proof_transcript, enrollment_secret_context, seal_enrollment_secret,
        validate_hpke_recipient, validate_issued_certificate, validate_p256_csr,
        verify_device_proof, HPKE_SUITE_NAME,
    },
    state::AppState,
    wire::{DeviceEnvelope, RemoteAction},
};

const PROVIDER_BEGIN_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClaimRequest {
    claim_token: String,
    hardware_uid: String,
    device_csr_der_b64: String,
    hpke_recipient_b64: String,
    device_nonce_b64: String,
    device_proof_b64: String,
}

#[derive(Serialize)]
pub struct EnvelopeAccepted {
    pub(crate) accepted: bool,
    pub(crate) spool_record_id: String,
    pub(crate) sequence: String,
}

#[derive(Serialize)]
pub struct ClaimResponse {
    companion_id: Uuid,
    gateway_id: Uuid,
    key_version: u32,
    device_certificate_der_b64: String,
    device_certificate_chain_der_b64: Vec<String>,
    sealed_secret: SealedSecretResponse,
}

#[derive(Serialize)]
pub struct SealedSecretResponse {
    suite: &'static str,
    enc_b64: String,
    ciphertext_b64: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ActivateCertificateRequest {
    claim_token: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PutCatalogRequest {
    display_name: String,
    host: String,
    bootstrap_port: u16,
    port: u16,
    server_name: String,
    ca_cert_der_b64: String,
    spki_sha256_b64: String,
}

pub async fn put_catalog(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(request): Json<PutCatalogRequest>,
) -> Result<StatusCode, ApiError> {
    validate_catalog_name(&request.display_name)?;
    validate_endpoint_host(&request.host)?;
    validate_dns_name(&request.server_name, "invalid gateway server name")?;
    if request.bootstrap_port == 0 || request.port == 0 || request.bootstrap_port == request.port {
        return Err(ApiError::Invalid("invalid gateway listener ports"));
    }
    let ca_cert_der =
        decode_canonical_base64url(&request.ca_cert_der_b64, "invalid gateway CA certificate")?;
    validate_catalog_ca(&ca_cert_der)?;
    let spki_sha256: [u8; 32] =
        decode_canonical_base64url(&request.spki_sha256_b64, "invalid gateway SPKI digest")?
            .try_into()
            .map_err(|_| ApiError::Invalid("invalid gateway SPKI digest"))?;

    let mtls = mtls_identity(&state, remote, &headers)?;
    let certificate_subject = sha256(&mtls.certificate_sha256);
    state
        .db
        .check_rate_limit("gateway.catalog.update", &certificate_subject, 60, 3600)
        .await?;
    let gateway = state.db.gateway_by_mtls(&mtls).await?;
    state
        .db
        .upsert_gateway_catalog(
            &gateway,
            &request.display_name,
            &request.host,
            request.bootstrap_port,
            request.port,
            &request.server_name,
            &ca_cert_der,
            &spki_sha256,
        )
        .await?;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn activate_certificate_rotation(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(rotation_id): Path<Uuid>,
    Json(request): Json<ActivateCertificateRequest>,
) -> Result<Json<ActivatedCertificateView>, ApiError> {
    if request.claim_token.is_empty() || request.claim_token.len() > 256 {
        return Err(ApiError::Invalid("invalid certificate rotation claim"));
    }
    let mtls = mtls_identity(&state, remote, &headers)?;
    let certificate_subject = sha256(&mtls.certificate_sha256);
    state
        .db
        .check_rate_limit("gateway.certificate.rotate", &certificate_subject, 20, 600)
        .await?;
    let activated = state
        .db
        .activate_certificate_rotation(rotation_id, &sha256_text(&request.claim_token), &mtls)
        .await?;
    Ok(Json(activated))
}

pub async fn claim_enrollment(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(enrollment_id): Path<Uuid>,
    Json(request): Json<ClaimRequest>,
) -> Result<Json<ClaimResponse>, ApiError> {
    // Authenticate and rate-limit the forwarding gateway before doing any
    // untrusted P-256 parsing or signature verification.
    let mtls = mtls_identity(&state, remote, &headers)?;
    let rate_subject = sha256(&mtls.certificate_sha256);
    state
        .db
        .check_rate_limit("gateway.enrollment.claim", &rate_subject, 20, 600)
        .await?;
    let gateway = state.db.gateway_by_mtls(&mtls).await?;
    claim_enrollment_for_gateway(&state, &gateway, enrollment_id, request).await
}

pub(crate) async fn claim_enrollment_for_gateway(
    state: &AppState,
    gateway: &Gateway,
    enrollment_id: Uuid,
    request: ClaimRequest,
) -> Result<Json<ClaimResponse>, ApiError> {
    if request.claim_token.is_empty()
        || request.claim_token.len() > 256
        || request.hardware_uid.len() < 4
        || request.hardware_uid.len() > 128
        || request.hardware_uid.chars().any(char::is_control)
        || request.device_csr_der_b64.len() > 5_464
        || request.hpke_recipient_b64.len() != 87
        || request.device_nonce_b64.len() != 22
        || request.device_proof_b64.len() != 86
    {
        return Err(ApiError::Invalid("invalid enrollment claim"));
    }
    if enrollment_id.is_nil() {
        return Err(ApiError::Invalid("invalid enrollment ID"));
    }

    let csr_der = decode_canonical_base64url(&request.device_csr_der_b64, "invalid device CSR")?;
    let csr = validate_p256_csr(&csr_der)?;
    let hpke_recipient =
        decode_canonical_base64url(&request.hpke_recipient_b64, "invalid HPKE recipient key")?;
    if hpke_recipient.len() != 65 || hpke_recipient.first() != Some(&0x04) {
        return Err(ApiError::Invalid("invalid HPKE recipient key"));
    }
    let hpke_recipient: [u8; 65] = hpke_recipient
        .try_into()
        .map_err(|_| ApiError::Invalid("invalid HPKE recipient key"))?;
    validate_hpke_recipient(&hpke_recipient)?;
    let device_nonce: [u8; 16] =
        decode_canonical_base64url(&request.device_nonce_b64, "invalid device nonce")?
            .try_into()
            .map_err(|_| ApiError::Invalid("invalid device nonce"))?;
    let device_proof: [u8; 64] =
        decode_canonical_base64url(&request.device_proof_b64, "invalid device proof")?
            .try_into()
            .map_err(|_| ApiError::Invalid("invalid device proof"))?;
    let proof_transcript = device_proof_transcript(
        enrollment_id,
        &request.hardware_uid,
        &csr_der,
        &hpke_recipient,
        &device_nonce,
    )?;
    verify_device_proof(&csr, &proof_transcript, &device_proof)?;
    let mut request_material = Vec::with_capacity(proof_transcript.len() + device_proof.len());
    request_material.extend_from_slice(&proof_transcript);
    request_material.extend_from_slice(&device_proof);
    let request_sha256 = sha256(&request_material);

    let token_digest = sha256_text(&request.claim_token);
    match state
        .db
        .begin_enrollment_issuance(
            enrollment_id,
            &token_digest,
            &request_sha256,
            &request.hardware_uid,
            gateway,
        )
        .await?
    {
        BeginEnrollmentIssuance::Completed(result) => Ok(Json(claim_response(result))),
        BeginEnrollmentIssuance::Issue(reserved) => {
            let provider_boundary = AtomicBool::new(reserved.provider_job_id.is_some());
            let result = tokio::time::timeout(
                state.config.certificate_issue_timeout + Duration::from_secs(30),
                issue_enrollment(
                    state,
                    &reserved,
                    &csr,
                    &hpke_recipient,
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
                        .release_enrollment_issuance(reserved.id, reserved.issuance_id)
                        .await
                } else {
                    state
                        .db
                        .abort_enrollment_issuance(reserved.id, reserved.issuance_id)
                        .await
                };
                if let Err(error) = cleanup {
                    tracing::error!(error = %error, "failed to release enrollment issuance lease");
                }
            }
            result.map(|result| Json(claim_response(result)))
        }
    }
}

async fn issue_enrollment(
    state: &AppState,
    reserved: &ReservedEnrollment,
    csr: &crate::pki::ValidatedCsr,
    hpke_recipient: &[u8; 65],
    request_sha256: &[u8; 32],
    provider_boundary: &AtomicBool,
) -> Result<EnrollmentIssuanceResult, ApiError> {
    let san_uri = format!("urn:kitsu:companion:{}", reserved.companion_id);
    let provider_job_id = if let Some(provider_job_id) = &reserved.provider_job_id {
        provider_job_id.clone()
    } else {
        state.db.mark_enrollment_provider_attempt(reserved).await?;
        provider_boundary.store(true, Ordering::Release);
        let provider_job_id = tokio::time::timeout(
            PROVIDER_BEGIN_TIMEOUT,
            state.certificate_issuer.begin(IssueCertificateRequest {
                profile: CertificateProfile::Companion,
                csr_der: csr.der.clone(),
                san_uri: san_uri.clone(),
                idempotency_key: hex::encode(&request_sha256[..16]),
            }),
        )
        .await
        .map_err(|_| ApiError::Unavailable)??;
        state
            .db
            .record_enrollment_provider_job(reserved, &provider_job_id)
            .await?;
        provider_job_id
    };
    let raw_certificate = state.certificate_issuer.finish(&provider_job_id).await?;
    let certificate =
        validate_issued_certificate(raw_certificate, csr, &san_uri, chrono::Utc::now())?;
    // CA issuance is independent of the companion secret.  Do all KMS and
    // HPKE work only after the provider ARN is durable so a crash-recovery
    // retry cannot spend its bounded idempotency window on unrelated work.
    let data_key = state
        .kms
        .generate_data_key(reserved.companion_id, reserved.key_version)
        .await?;
    let secret = zeroize::Zeroizing::new(random_array::<32>());
    let encrypted = encrypt_companion_secret(
        reserved.companion_id,
        reserved.key_version,
        &data_key.plaintext,
        &secret,
    )?;
    let secret_record = NewSecretRecord {
        companion_id: reserved.companion_id,
        key_version: reserved.key_version,
        kms_key_id: data_key.kms_key_id,
        wrapped_dek: data_key.wrapped,
        encrypted,
    };
    let context = enrollment_secret_context(
        reserved.id,
        reserved.companion_id,
        reserved.gateway_id,
        reserved.key_version,
    )?;
    let sealed = seal_enrollment_secret(hpke_recipient, &context, &secret)?;
    state
        .db
        .complete_enrollment_issuance(reserved, &secret_record, &certificate, &sealed)
        .await
}

fn claim_response(result: EnrollmentIssuanceResult) -> ClaimResponse {
    ClaimResponse {
        companion_id: result.companion_id,
        gateway_id: result.gateway_id,
        key_version: result.key_version,
        device_certificate_der_b64: URL_SAFE_NO_PAD.encode(result.certificate_der),
        device_certificate_chain_der_b64: result
            .certificate_chain_der
            .into_iter()
            .map(|certificate| URL_SAFE_NO_PAD.encode(certificate))
            .collect(),
        sealed_secret: SealedSecretResponse {
            suite: HPKE_SUITE_NAME,
            enc_b64: URL_SAFE_NO_PAD.encode(result.hpke_enc),
            ciphertext_b64: URL_SAFE_NO_PAD.encode(result.hpke_ciphertext),
        },
    }
}

pub async fn ingest_envelope(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(envelope): Json<DeviceEnvelope>,
) -> Result<Json<EnvelopeAccepted>, ApiError> {
    let spool_record_id = canonical_spool_record_id(&headers)?;
    let mtls = mtls_identity(&state, remote, &headers)?;
    let gateway = state.db.gateway_by_mtls(&mtls).await?;
    let sequence = envelope.sequence.clone();
    process_envelope(&state, &gateway, &envelope).await?;
    // Do not alter this response without coordinating the gateway WAL parser.
    Ok(Json(EnvelopeAccepted {
        accepted: true,
        spool_record_id,
        sequence,
    }))
}

pub async fn session(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    upgrade: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    let mtls = mtls_identity(&state, remote, &headers)?;
    let gateway = state.db.gateway_by_mtls(&mtls).await?;
    Ok(upgrade
        .max_message_size(64 * 1024)
        .on_upgrade(move |socket| gateway_socket(state, gateway, socket)))
}

pub(crate) async fn process_envelope(
    state: &AppState,
    gateway: &Gateway,
    envelope: &DeviceEnvelope,
) -> Result<IngestOutcome, ApiError> {
    let validated = envelope.validate_wrapper()?;
    let rate_subject = sha256(envelope.companion_id.as_bytes());
    state
        .db
        .check_rate_limit("device.envelope", &rate_subject, 1200, 60)
        .await?;
    let stored = state.db.secret_for_envelope(gateway, envelope).await?;
    let dek = state
        .kms
        .decrypt_data_key(
            stored.companion_id,
            stored.key_version,
            &stored.kms_key_id,
            &stored.wrapped_dek,
        )
        .await?;
    let secret = decrypt_companion_secret(
        stored.companion_id,
        stored.key_version,
        &dek,
        &stored.encrypted,
    )?;
    verify_device_hmac(envelope, &validated, &secret)?;
    let transcript = device_transcript(envelope, &validated)?;
    let transcript_hash = sha256(&transcript);
    state
        .db
        .ingest_verified_envelope(gateway, envelope, &validated, &transcript_hash)
        .await
}

pub(crate) async fn gateway_socket(state: AppState, gateway: Gateway, socket: WebSocket) {
    let (connection_id, mut queued) = state.hubs.register_gateway(gateway.id).await;
    metrics::gauge!("kitsu_gateway_sessions", "gateway_id" => gateway.id.to_string())
        .increment(1.0);
    let (mut sender, mut receiver) = socket.split();
    let mut poll = tokio::time::interval(Duration::from_secs(10));
    let mut ping = tokio::time::interval(Duration::from_secs(30));

    if send_pending(&state, &gateway, &mut sender).await.is_err() {
        state
            .hubs
            .unregister_gateway(gateway.id, connection_id)
            .await;
        return;
    }
    loop {
        tokio::select! {
            Some(action) = queued.recv() => {
                if send_action(&state, &gateway, action, &mut sender).await.is_err() { break; }
            }
            _ = poll.tick() => {
                if send_pending(&state, &gateway, &mut sender).await.is_err() { break; }
            }
            _ = ping.tick() => {
                if sender.send(Message::Ping(Vec::new().into())).await.is_err() { break; }
            }
            incoming = receiver.next() => {
                match incoming {
                    Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                    Some(Ok(Message::Ping(value))) => {
                        if sender.send(Message::Pong(value)).await.is_err() { break; }
                    }
                    // Signed device traffic uses POST /envelopes so its exact
                    // WAL correlation contract remains simple and durable.
                    Some(Ok(Message::Text(_))) | Some(Ok(Message::Binary(_))) => {
                        let _ = sender.send(Message::Close(None)).await;
                        break;
                    }
                    Some(Ok(_)) => {}
                }
            }
        }
    }
    state
        .hubs
        .unregister_gateway(gateway.id, connection_id)
        .await;
    metrics::gauge!("kitsu_gateway_sessions", "gateway_id" => gateway.id.to_string())
        .decrement(1.0);
}

async fn send_pending<S>(
    state: &AppState,
    gateway: &Gateway,
    sender: &mut S,
) -> Result<(), ApiError>
where
    S: futures_util::Sink<Message> + Unpin,
{
    for action in state.db.pending_actions(gateway).await? {
        send_action(state, gateway, action, sender).await?;
    }
    Ok(())
}

async fn send_action<S>(
    state: &AppState,
    gateway: &Gateway,
    action: RemoteAction,
    sender: &mut S,
) -> Result<(), ApiError>
where
    S: futures_util::Sink<Message> + Unpin,
{
    let action_id = action.action_id;
    let text = serde_json::to_string(&action).map_err(ApiError::internal)?;
    let accepted = sender.send(Message::Text(text.into())).await.is_ok();
    state
        .db
        .record_action_delivery_attempt(gateway, action_id, state.instance_id, accepted)
        .await?;
    if !accepted {
        return Err(ApiError::Unavailable);
    }
    Ok(())
}

fn mtls_identity(
    state: &AppState,
    remote: SocketAddr,
    headers: &HeaderMap,
) -> Result<MtlsIdentity, ApiError> {
    extract_mtls_identity(
        remote,
        headers,
        &state.config.trusted_mtls_proxy_cidrs,
        state.config.mtls_proxy_auth_token.expose(),
        MtlsHeaders {
            proxy_auth: &state.config.mtls_proxy_auth_header,
            xfcc: &state.config.mtls_xfcc_header,
        },
    )
}

pub(crate) fn canonical_spool_record_id(headers: &HeaderMap) -> Result<String, ApiError> {
    let mut values = headers.get_all("x-kitsu-spool-record-id").iter();
    let value = values
        .next()
        .and_then(|value| value.to_str().ok())
        .ok_or(ApiError::Invalid("X-Kitsu-Spool-Record-Id is required"))?;
    if values.next().is_some() {
        return Err(ApiError::Invalid(
            "X-Kitsu-Spool-Record-Id must appear exactly once",
        ));
    }
    if value.is_empty()
        || value.len() > 20
        || (value.len() > 1 && value.starts_with('0'))
        || !value.bytes().all(|byte| byte.is_ascii_digit())
        || value.parse::<u64>().is_err()
    {
        return Err(ApiError::Invalid("invalid spool record ID"));
    }
    Ok(value.to_owned())
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

fn validate_catalog_name(value: &str) -> Result<(), ApiError> {
    if value.is_empty()
        || value.len() > 80
        || value.trim() != value
        || value.chars().any(char::is_control)
    {
        return Err(ApiError::Invalid("invalid gateway display name"));
    }
    Ok(())
}

fn validate_dns_name(value: &str, message: &'static str) -> Result<(), ApiError> {
    if value.is_empty()
        || value.len() > 253
        || !value.is_ascii()
        || value.bytes().any(|byte| byte.is_ascii_uppercase())
    {
        return Err(ApiError::Invalid(message));
    }
    let valid = value.split('.').all(|label| {
        !label.is_empty()
            && label.len() <= 63
            && label
                .as_bytes()
                .first()
                .is_some_and(u8::is_ascii_alphanumeric)
            && label
                .as_bytes()
                .last()
                .is_some_and(u8::is_ascii_alphanumeric)
            && label
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    }) && value.bytes().any(|byte| byte.is_ascii_alphabetic());
    if !valid {
        return Err(ApiError::Invalid(message));
    }
    Ok(())
}

fn validate_endpoint_host(value: &str) -> Result<(), ApiError> {
    if let Ok(address) = value.parse::<std::net::IpAddr>() {
        if address.to_string() == value {
            return Ok(());
        }
        return Err(ApiError::Invalid("invalid gateway host"));
    }
    validate_dns_name(value, "invalid gateway host")
}

fn validate_catalog_ca(der: &[u8]) -> Result<(), ApiError> {
    if der.is_empty() || der.len() > 8_192 {
        return Err(ApiError::Invalid("invalid gateway CA certificate"));
    }
    let (remaining, certificate) = parse_x509_certificate(der)
        .map_err(|_| ApiError::Invalid("invalid gateway CA certificate"))?;
    let basic = certificate
        .basic_constraints()
        .map_err(|_| ApiError::Invalid("invalid gateway CA certificate"))?
        .ok_or(ApiError::Invalid("invalid gateway CA certificate"))?;
    let usage = certificate
        .key_usage()
        .map_err(|_| ApiError::Invalid("invalid gateway CA certificate"))?
        .ok_or(ApiError::Invalid("invalid gateway CA certificate"))?;
    if !remaining.is_empty()
        || !basic.value.ca
        || !usage.value.key_cert_sign()
        || certificate.subject() != certificate.issuer()
        || certificate.verify_signature(None).is_err()
    {
        return Err(ApiError::Invalid("invalid gateway CA certificate"));
    }
    Ok(())
}

#[cfg(test)]
mod catalog_tests {
    use super::*;

    #[test]
    fn catalog_dns_names_are_canonical() {
        assert!(validate_dns_name("gateway.example", "bad").is_ok());
        assert!(validate_dns_name("gateway-device.k32.internal", "bad").is_ok());
        for invalid in [
            "GATEWAY.example",
            "https://gateway.example",
            "gateway.example:7443",
            "-gateway.example",
            "gateway-.example",
            "gateway..example",
            "gateway.example.",
        ] {
            assert!(validate_dns_name(invalid, "bad").is_err(), "{invalid}");
        }
    }

    #[test]
    fn catalog_endpoint_hosts_accept_canonical_ip_literals() {
        for valid in ["192.0.2.10", "2001:db8::1", "gateway.example"] {
            assert!(validate_endpoint_host(valid).is_ok(), "{valid}");
        }
        for invalid in ["192.168.001.1", "2001:0db8::1", "[2001:db8::1]"] {
            assert!(validate_endpoint_host(invalid).is_err(), "{invalid}");
        }
        assert!(validate_dns_name("192.0.2.10", "bad").is_err());
        assert!(validate_dns_name("2001:db8::1", "bad").is_err());
    }

    #[test]
    fn catalog_display_name_is_bounded_and_clean() {
        assert!(validate_catalog_name("Kitsu Home Gateway").is_ok());
        assert!(validate_catalog_name("").is_err());
        assert!(validate_catalog_name(" padded").is_err());
        assert!(validate_catalog_name("line\nbreak").is_err());
        assert!(validate_catalog_name(&"x".repeat(81)).is_err());
    }

    #[test]
    fn bootstrap_and_steady_listeners_are_distinct() {
        let valid = PutCatalogRequest {
            display_name: "Kitsu Home Gateway".to_owned(),
            host: "gateway.example".to_owned(),
            bootstrap_port: 7442,
            port: 7443,
            server_name: "gateway.example".to_owned(),
            ca_cert_der_b64: "unused".to_owned(),
            spki_sha256_b64: "unused".to_owned(),
        };
        assert_ne!(valid.bootstrap_port, valid.port);
    }
}
