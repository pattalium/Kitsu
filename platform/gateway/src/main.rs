use std::{
    collections::HashMap,
    fs,
    net::IpAddr,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use anyhow::{Context, Result};
use axum::{extract::State, http::StatusCode, response::IntoResponse, routing::get, Json, Router};
use clap::Parser;
use futures_util::{SinkExt, StreamExt};
use kitsu_home_gateway::{
    action::ValidatedRemoteAction,
    bootstrap::{
        failure as bootstrap_failure, success as bootstrap_success, validate_claim_response,
        ValidatedBootstrapRequest, MAX_BOOTSTRAP_FRAME_BYTES,
    },
    config::{DeploymentScope, GatewayConfig},
    device_hub::{DeviceCommand, DeviceHub, RouteOutcome},
    envelope::DeviceEnvelope,
    framing::{read_frame, write_frame, FrameError},
    spool::{Spool, SpoolIdentity},
    tls::{
        build_backend_client_config, build_bootstrap_acceptor, build_device_acceptor,
        certificate_companion_id, certificate_sha256,
    },
    GATEWAY_PROTOCOL_VERSION,
};
use mdns_sd::{ServiceDaemon, ServiceInfo};
use reqwest::{Certificate, Identity};
use serde::{Deserialize, Serialize};
use tokio::{
    net::{TcpListener, TcpStream},
    sync::{mpsc, watch, OwnedSemaphorePermit, Semaphore},
};
use tokio_rustls::TlsAcceptor;
use tokio_tungstenite::{
    connect_async_tls_with_config,
    tungstenite::{protocol::WebSocketConfig, Message},
    Connector,
};
use tracing::{info, warn};
use uuid::Uuid;

const CAPACITY_WARNING_INTERVAL: Duration = Duration::from_secs(5);

#[derive(Default)]
struct CapacityWarningLimiter {
    last_warning: Option<Instant>,
    suppressed: u64,
}

impl CapacityWarningLimiter {
    fn observe(&mut self, now: Instant) -> Option<u64> {
        if self
            .last_warning
            .is_none_or(|previous| now.duration_since(previous) >= CAPACITY_WARNING_INTERVAL)
        {
            let suppressed = self.suppressed;
            self.last_warning = Some(now);
            self.suppressed = 0;
            Some(suppressed)
        } else {
            self.suppressed = self.suppressed.saturating_add(1);
            None
        }
    }
}

#[derive(Clone)]
struct AppState {
    gateway_id: Uuid,
    deployment_scope: DeploymentScope,
    spool: Arc<Mutex<Spool>>,
    upload_online: watch::Receiver<bool>,
    action_session_online: watch::Receiver<bool>,
    devices: DeviceHub,
}

#[derive(Debug, Serialize)]
struct GatewayAck {
    v: u16,
    r#type: &'static str,
    spool_record_id: String,
    device_sequence: String,
}

#[derive(Debug, Deserialize)]
struct BackendAck {
    accepted: bool,
    spool_record_id: String,
    sequence: String,
}

#[derive(Debug, Serialize)]
struct HealthResponse {
    status: &'static str,
    protocol: u16,
    gateway_id: Uuid,
    deployment_scope: DeploymentScope,
    backend_online: bool,
    backend_upload_online: bool,
    backend_action_session_online: bool,
    spool_pending: u64,
    spool_bytes: u64,
    devices_online: usize,
}

#[tokio::main]
async fn main() -> Result<()> {
    // reqwest/tungstenite may also compile rustls' ring backend into this
    // process. Select the gateway's pinned aws-lc-rs provider explicitly so a
    // dual-feature dependency graph can never turn startup into a panic.
    install_crypto_provider()?;
    tracing_subscriber::fmt()
        .json()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .with_target(false)
        .init();

    let config = GatewayConfig::parse();
    config.validate()?;
    let acceptor =
        build_device_acceptor(&config.server_cert, &config.server_key, &config.device_ca)?;
    let bootstrap_acceptor = build_bootstrap_acceptor(&config.server_cert, &config.server_key)?;
    let spool = Arc::new(Mutex::new(Spool::open(
        &config.spool_dir,
        config.segment_max_bytes,
        config.spool_max_bytes,
        config.max_frame_bytes,
        SpoolIdentity {
            deployment_scope: config.deployment_scope,
            gateway_id: config.gateway_id,
        },
    )?));
    let (upload_tx, upload_rx) = watch::channel(false);
    let (action_tx, action_rx) = watch::channel(false);
    let devices = DeviceHub::new();
    let state = AppState {
        gateway_id: config.gateway_id,
        deployment_scope: config.deployment_scope,
        spool: spool.clone(),
        upload_online: upload_rx,
        action_session_online: action_rx,
        devices: devices.clone(),
    };

    let mdns = if config.mdns {
        Some(register_mdns(&config)?)
    } else {
        None
    };
    let admin = tokio::spawn(run_admin(config.admin_listen, state.clone()));
    let forwarder = tokio::spawn(run_forwarder(config.clone(), spool.clone(), upload_tx));
    let action_session = tokio::spawn(run_action_supervisor(
        config.clone(),
        devices.clone(),
        action_tx,
    ));
    let device_listener = tokio::spawn(run_device_listener(
        config.clone(),
        acceptor,
        spool,
        devices,
    ));
    let bootstrap_listener =
        tokio::spawn(run_bootstrap_listener(config.clone(), bootstrap_acceptor));

    info!(
        gateway_id = %config.gateway_id,
        deployment_scope = %config.deployment_scope,
        listen = %config.listen,
        bootstrap_concurrency_limit = config.bootstrap_concurrency_limit,
        steady_concurrency_limit = config.steady_concurrency_limit,
        "Kitsu gateway started"
    );
    tokio::select! {
        result = admin => result.context("admin task join")??,
        result = forwarder => result.context("forwarder task join")??,
        result = action_session => result.context("action-session task join")??,
        result = device_listener => result.context("device task join")??,
        result = bootstrap_listener => result.context("bootstrap task join")??,
        _ = tokio::signal::ctrl_c() => info!("shutdown requested"),
    }

    if let Some(daemon) = mdns {
        let _ = daemon.shutdown();
    }
    Ok(())
}

fn install_crypto_provider() -> Result<()> {
    rustls::crypto::aws_lc_rs::default_provider()
        .install_default()
        .map_err(|_| anyhow::anyhow!("install the gateway rustls crypto provider"))
}

async fn run_bootstrap_listener(config: GatewayConfig, acceptor: TlsAcceptor) -> Result<()> {
    let listener = TcpListener::bind(config.bootstrap_listen)
        .await
        .with_context(|| format!("bind bootstrap listener {}", config.bootstrap_listen))?;
    let backend = backend_client(&config)?;
    let concurrency = Arc::new(Semaphore::new(config.bootstrap_concurrency_limit));
    let mut capacity_warnings = CapacityWarningLimiter::default();
    loop {
        let (stream, peer) = listener.accept().await?;
        let Some(permit) = try_acquire_connection(&concurrency) else {
            if let Some(suppressed) = capacity_warnings.observe(Instant::now()) {
                warn!(peer = %peer, suppressed, "bootstrap connections rejected by concurrency limit");
            }
            continue;
        };
        let config = config.clone();
        let acceptor = acceptor.clone();
        let backend = backend.clone();
        tokio::spawn(async move {
            let _permit = permit;
            if let Err(error) =
                handle_bootstrap(stream, peer.ip(), &config, acceptor, &backend).await
            {
                warn!(peer = %peer, error = %error, "bootstrap session ended");
            }
        });
    }
}

async fn handle_bootstrap(
    stream: TcpStream,
    peer_ip: IpAddr,
    config: &GatewayConfig,
    acceptor: TlsAcceptor,
    backend: &reqwest::Client,
) -> Result<()> {
    let mut stream = tokio::time::timeout(Duration::from_secs(10), acceptor.accept(stream))
        .await
        .context("bootstrap TLS handshake timeout")?
        .context("bootstrap server-authenticated TLS handshake")?;
    let body = match read_frame(
        &mut stream,
        MAX_BOOTSTRAP_FRAME_BYTES,
        config.frame_timeout(),
    )
    .await
    {
        Ok(body) => body,
        Err(error) => {
            write_bootstrap_error(&mut stream, "invalid_request").await?;
            return Err(error.into());
        }
    };
    let request = match ValidatedBootstrapRequest::parse(&body) {
        Ok(request) => request,
        Err(error) => {
            write_bootstrap_error(&mut stream, "invalid_request").await?;
            return Err(error.into());
        }
    };
    let endpoint = config
        .backend_url
        .join(&format!(
            "/v1/gateway/enrollments/{}/claim",
            request.enrollment_id
        ))
        .context("build backend device-enrollment URL")?;
    let response = match backend
        .post(endpoint)
        .header(reqwest::header::CONTENT_TYPE, "application/json")
        .body(request.claim_json)
        .send()
        .await
    {
        Ok(response) => response,
        Err(error) => {
            write_bootstrap_error(&mut stream, "backend_unavailable").await?;
            return Err(error.into());
        }
    };
    if !response.status().is_success() {
        let error = if response.status() == reqwest::StatusCode::SERVICE_UNAVAILABLE {
            "issuer_unavailable"
        } else {
            "claim_rejected"
        };
        write_bootstrap_error(&mut stream, error).await?;
        return Ok(());
    }
    let response_bytes = read_bounded_response(response, 64 * 1024).await?;
    if validate_claim_response(&response_bytes, config.gateway_id).is_err() {
        write_bootstrap_error(&mut stream, "invalid_backend_response").await?;
        anyhow::bail!("backend returned an invalid sealed enrollment response");
    }
    let framed = serde_json::to_vec(&bootstrap_success(request.enrollment_id, &response_bytes))?;
    write_frame(&mut stream, &framed).await?;
    info!(peer_ip = %peer_ip, enrollment_id = %request.enrollment_id, "sealed device enrollment relayed");
    Ok(())
}

async fn write_bootstrap_error<S>(stream: &mut S, code: &'static str) -> Result<()>
where
    S: tokio::io::AsyncWrite + Unpin,
{
    let body = serde_json::to_vec(&bootstrap_failure(code))?;
    write_frame(stream, &body).await?;
    Ok(())
}

async fn read_bounded_response(mut response: reqwest::Response, limit: usize) -> Result<Vec<u8>> {
    if response
        .content_length()
        .is_some_and(|length| length > limit as u64)
    {
        anyhow::bail!("backend response exceeds enrollment limit");
    }
    let mut bytes = Vec::new();
    while let Some(chunk) = response
        .chunk()
        .await
        .context("read backend enrollment response")?
    {
        if bytes.len().saturating_add(chunk.len()) > limit {
            anyhow::bail!("backend response exceeds enrollment limit");
        }
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

async fn run_admin(address: std::net::SocketAddr, state: AppState) -> Result<()> {
    let app = Router::new()
        .route("/health/live", get(health))
        .with_state(state);
    let listener = TcpListener::bind(address)
        .await
        .with_context(|| format!("bind admin listener {address}"))?;
    axum::serve(listener, app)
        .await
        .context("serve admin listener")
}

async fn health(State(state): State<AppState>) -> impl IntoResponse {
    let devices_online = state.devices.online_count().await;
    let stats = match state.spool.lock() {
        Ok(spool) => spool.stats(),
        Err(_) => {
            return (
                StatusCode::SERVICE_UNAVAILABLE,
                Json(HealthResponse {
                    status: "degraded",
                    protocol: GATEWAY_PROTOCOL_VERSION,
                    gateway_id: state.gateway_id,
                    deployment_scope: state.deployment_scope,
                    backend_online: false,
                    backend_upload_online: false,
                    backend_action_session_online: false,
                    spool_pending: 0,
                    spool_bytes: 0,
                    devices_online,
                }),
            )
        }
    };
    let upload_online = *state.upload_online.borrow();
    let action_session_online = *state.action_session_online.borrow();
    (
        StatusCode::OK,
        Json(HealthResponse {
            status: "ok",
            protocol: GATEWAY_PROTOCOL_VERSION,
            gateway_id: state.gateway_id,
            deployment_scope: state.deployment_scope,
            backend_online: upload_online && action_session_online,
            backend_upload_online: upload_online,
            backend_action_session_online: action_session_online,
            spool_pending: stats.pending_records,
            spool_bytes: stats.total_bytes,
            devices_online,
        }),
    )
}

async fn run_device_listener(
    config: GatewayConfig,
    acceptor: TlsAcceptor,
    spool: Arc<Mutex<Spool>>,
    devices: DeviceHub,
) -> Result<()> {
    let listener = TcpListener::bind(config.listen)
        .await
        .with_context(|| format!("bind device listener {}", config.listen))?;
    let concurrency = Arc::new(Semaphore::new(config.steady_concurrency_limit));
    let mut capacity_warnings = CapacityWarningLimiter::default();
    loop {
        let (stream, peer) = listener.accept().await?;
        let Some(permit) = try_acquire_connection(&concurrency) else {
            if let Some(suppressed) = capacity_warnings.observe(Instant::now()) {
                warn!(peer = %peer, suppressed, "device connections rejected by concurrency limit");
            }
            continue;
        };
        let config = config.clone();
        let acceptor = acceptor.clone();
        let spool = spool.clone();
        let devices = devices.clone();
        tokio::spawn(async move {
            let _permit = permit;
            if let Err(error) =
                handle_device(stream, peer.ip(), config, acceptor, spool, devices).await
            {
                warn!(peer = %peer, error = %error, "device session ended");
            }
        });
    }
}

fn try_acquire_connection(concurrency: &Arc<Semaphore>) -> Option<OwnedSemaphorePermit> {
    concurrency.clone().try_acquire_owned().ok()
}

async fn handle_device(
    stream: TcpStream,
    peer_ip: IpAddr,
    config: GatewayConfig,
    acceptor: TlsAcceptor,
    spool: Arc<Mutex<Spool>>,
    devices: DeviceHub,
) -> Result<()> {
    let mut stream = tokio::time::timeout(Duration::from_secs(10), acceptor.accept(stream))
        .await
        .context("TLS handshake timeout")?
        .context("mutual TLS handshake")?;
    let (_, connection) = stream.get_ref();
    let peer_cert = connection
        .peer_certificates()
        .and_then(|certs| certs.first())
        .context("missing verified device certificate")?;
    let fingerprint = certificate_sha256(peer_cert);
    let certificate_companion = certificate_companion_id(peer_cert)
        .context("device certificate is not bound to a Kitsu companion")?;
    info!(peer_ip = %peer_ip, cert_sha256 = %fingerprint, companion_id = %certificate_companion, "device connected");

    let (command_sender, command_receiver) = mpsc::channel(32);
    let session_id = devices
        .register(certificate_companion, command_sender)
        .await;
    let result = run_device_session(
        &mut stream,
        certificate_companion,
        &config,
        &spool,
        command_receiver,
    )
    .await;
    devices.unregister(certificate_companion, session_id).await;
    result
}

async fn run_device_session<S>(
    stream: &mut S,
    certificate_companion: Uuid,
    config: &GatewayConfig,
    spool: &Arc<Mutex<Spool>>,
    commands: mpsc::Receiver<DeviceCommand>,
) -> Result<()>
where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin,
{
    let (reader, writer) = tokio::io::split(stream);
    let (ack_sender, ack_receiver) = mpsc::channel(32);
    tokio::select! {
        result = run_device_reader(
            reader,
            certificate_companion,
            config,
            spool,
            ack_sender,
        ) => result,
        result = run_device_writer(
            writer,
            certificate_companion,
            commands,
            ack_receiver,
        ) => result,
    }
}

async fn run_device_reader<R>(
    mut reader: R,
    certificate_companion: Uuid,
    config: &GatewayConfig,
    spool: &Arc<Mutex<Spool>>,
    ack_sender: mpsc::Sender<Vec<u8>>,
) -> Result<()>
where
    R: tokio::io::AsyncRead + Unpin,
{
    loop {
        let body =
            match read_frame(&mut reader, config.max_frame_bytes, config.frame_timeout()).await {
                Ok(body) => body,
                Err(FrameError::Closed) => return Ok(()),
                Err(error) => return Err(error.into()),
            };
        let validated = DeviceEnvelope::parse_and_validate(&body, config.gateway_id)
            .context("validate device envelope")?;
        if validated.companion_id != certificate_companion {
            anyhow::bail!("device envelope companion does not match certificate SAN");
        }
        let record_id = {
            let mut spool = spool
                .lock()
                .map_err(|_| anyhow::anyhow!("spool mutex poisoned"))?;
            spool
                .append(&body)
                .context("durably append device envelope")?
        };
        let ack = serde_json::to_vec(&GatewayAck {
            v: GATEWAY_PROTOCOL_VERSION,
            r#type: "gateway_ack",
            spool_record_id: record_id.to_string(),
            device_sequence: validated.sequence.to_string(),
        })?;
        ack_sender
            .send(ack)
            .await
            .map_err(|_| anyhow::anyhow!("device writer stopped before durable ACK"))?;
        info!(record_id, companion_id = %validated.companion_id, sequence = validated.sequence, payload_type = %validated.payload_type, "device envelope spooled");
    }
}

async fn run_device_writer<W>(
    mut writer: W,
    certificate_companion: Uuid,
    mut commands: mpsc::Receiver<DeviceCommand>,
    mut acks: mpsc::Receiver<Vec<u8>>,
) -> Result<()>
where
    W: tokio::io::AsyncWrite + Unpin,
{
    loop {
        tokio::select! {
            biased;
            ack = acks.recv() => {
                let Some(ack) = ack else { return Ok(()); };
                write_frame(&mut writer, &ack).await?;
            }
            command = commands.recv() => {
                let Some(command) = command else { return Ok(()); };
                // The signed remote-action JSON is framed byte-for-byte. The
                // gateway does not decode params, reserialize, or authenticate
                // it on the companion's behalf.
                write_frame(&mut writer, command.bytes.as_ref()).await?;
                info!(action_id = %command.action_id, companion_id = %certificate_companion, "remote action routed to device session");
            }
        }
    }
}

async fn run_action_supervisor(
    config: GatewayConfig,
    devices: DeviceHub,
    online: watch::Sender<bool>,
) -> Result<()> {
    let endpoint = config.backend_session_url()?;
    let tls = build_backend_client_config(
        &config.gateway_client_identity,
        config.backend_ca.as_deref(),
    )?;
    let mut retry_delay = Duration::from_secs(1);

    loop {
        let connected_at = tokio::time::Instant::now();
        let result =
            run_action_session(&endpoint, tls.clone(), devices.clone(), online.clone()).await;
        let _ = online.send(false);

        match result {
            Ok(()) => warn!("backend action session closed; reconnecting"),
            Err(error) => warn!(error = %error, "backend action session failed; reconnecting"),
        }

        if connected_at.elapsed() >= Duration::from_secs(30) {
            retry_delay = Duration::from_secs(1);
        }
        tokio::time::sleep(retry_delay).await;
        retry_delay = (retry_delay * 2).min(Duration::from_secs(60));
    }
}

async fn run_action_session(
    endpoint: &url::Url,
    tls: Arc<rustls::ClientConfig>,
    devices: DeviceHub,
    online: watch::Sender<bool>,
) -> Result<()> {
    let websocket_config = WebSocketConfig::default()
        .read_buffer_size(8 * 1024)
        .write_buffer_size(8 * 1024)
        .max_write_buffer_size(128 * 1024)
        .max_message_size(Some(64 * 1024))
        .max_frame_size(Some(64 * 1024));
    let (mut socket, response) = tokio::time::timeout(
        Duration::from_secs(15),
        connect_async_tls_with_config(
            endpoint.as_str(),
            Some(websocket_config),
            true,
            Some(Connector::Rustls(tls)),
        ),
    )
    .await
    .context("backend action-session connection timeout")?
    .context("connect backend action-session WebSocket")?;

    let _ = online.send(true);
    info!(status = %response.status(), endpoint = %endpoint, "backend action session connected");

    while let Some(message) = socket.next().await {
        match message.context("read backend action-session frame")? {
            Message::Text(text) => {
                let bytes: Arc<[u8]> = Arc::from(text.as_bytes());
                let action = ValidatedRemoteAction::parse(bytes.as_ref())
                    .context("validate backend remote action")?;
                let outcome = devices
                    .route(action.companion_id, action.action_id, bytes)
                    .await;
                match outcome {
                    RouteOutcome::Queued => info!(
                        action_id = %action.action_id,
                        companion_id = %action.companion_id,
                        action_type = %action.action_type,
                        "remote action queued for device"
                    ),
                    RouteOutcome::DuplicateSuppressed => info!(
                        action_id = %action.action_id,
                        companion_id = %action.companion_id,
                        "duplicate remote action suppressed"
                    ),
                    RouteOutcome::Offline => warn!(
                        action_id = %action.action_id,
                        companion_id = %action.companion_id,
                        "remote action retained by backend; companion is offline"
                    ),
                    RouteOutcome::Backpressured => warn!(
                        action_id = %action.action_id,
                        companion_id = %action.companion_id,
                        "remote action retained by backend; device queue is full"
                    ),
                    RouteOutcome::ConflictingDuplicate => {
                        anyhow::bail!(
                            "backend reused action ID {} with different signed bytes",
                            action.action_id
                        );
                    }
                }
            }
            Message::Ping(payload) => {
                socket
                    .send(Message::Pong(payload))
                    .await
                    .context("reply to backend WebSocket ping")?;
            }
            Message::Pong(_) => {}
            Message::Close(frame) => {
                info!(?frame, "backend closed action session");
                return Ok(());
            }
            Message::Binary(_) => {
                anyhow::bail!("backend action session sent an unsupported binary frame");
            }
            Message::Frame(_) => {
                anyhow::bail!("backend action session exposed an unexpected raw frame");
            }
        }
    }
    Ok(())
}

async fn run_forwarder(
    config: GatewayConfig,
    spool: Arc<Mutex<Spool>>,
    backend_online: watch::Sender<bool>,
) -> Result<()> {
    let client = backend_client(&config)?;
    let endpoint = config
        .backend_url
        .join("/v1/gateway/envelopes")
        .context("build backend envelope URL")?;
    let mut delay = Duration::from_secs(1);
    loop {
        let batch = {
            let spool = spool
                .lock()
                .map_err(|_| anyhow::anyhow!("spool mutex poisoned"))?;
            spool.read_pending(32, 2 * 1024 * 1024)?
        };
        if batch.is_empty() {
            // An empty local WAL says nothing about backend reachability. Keep
            // the last observed state until an actual backend request succeeds
            // or fails; otherwise a disconnected gateway reports a false
            // healthy state whenever it has nothing queued.
            tokio::time::sleep(Duration::from_secs(1)).await;
            continue;
        }

        let mut made_progress = false;
        for record in batch {
            let response = client
                .post(endpoint.clone())
                .header("x-kitsu-spool-record-id", record.id.to_string())
                .header(reqwest::header::CONTENT_TYPE, "application/json")
                .body(record.payload)
                .send()
                .await;
            let response = match response {
                Ok(response) => response,
                Err(error) => {
                    let _ = backend_online.send(false);
                    warn!(error = %error, "backend unavailable; WAL retained");
                    break;
                }
            };
            if !response.status().is_success() {
                let _ = backend_online.send(false);
                warn!(status = %response.status(), record_id = record.id, "backend rejected envelope; WAL retained");
                break;
            }
            let ack: BackendAck = match response.json().await {
                Ok(ack) => ack,
                Err(error) => {
                    let _ = backend_online.send(false);
                    warn!(error = %error, record_id = record.id, "backend acknowledgement malformed; WAL retained");
                    break;
                }
            };
            if !ack.accepted
                || ack.spool_record_id != record.id.to_string()
                || ack.sequence.is_empty()
            {
                let _ = backend_online.send(false);
                warn!(
                    record_id = record.id,
                    "backend acknowledgement did not correlate; WAL retained"
                );
                break;
            }
            {
                let mut spool = spool
                    .lock()
                    .map_err(|_| anyhow::anyhow!("spool mutex poisoned"))?;
                spool.acknowledge_through(record.id)?;
            }
            made_progress = true;
            let _ = backend_online.send(true);
        }

        if made_progress {
            delay = Duration::from_secs(1);
        } else {
            tokio::time::sleep(delay).await;
            delay = (delay * 2).min(Duration::from_secs(60));
        }
    }
}

fn backend_client(config: &GatewayConfig) -> Result<reqwest::Client> {
    let identity_pem =
        fs::read(&config.gateway_client_identity).context("read gateway client identity")?;
    let identity = Identity::from_pem(&identity_pem).context("parse gateway client identity")?;
    let mut builder = reqwest::Client::builder()
        .identity(identity)
        .https_only(true)
        .http2_adaptive_window(true)
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_secs(30))
        .user_agent(concat!("kitsu-home-gateway/", env!("CARGO_PKG_VERSION")));
    if let Some(path) = &config.backend_ca {
        let ca = Certificate::from_pem(&fs::read(path).context("read backend CA")?)
            .context("parse backend CA")?;
        builder = builder.add_root_certificate(ca);
    }
    builder.build().context("build backend HTTP client")
}

fn register_mdns(config: &GatewayConfig) -> Result<ServiceDaemon> {
    let daemon = ServiceDaemon::new().context("start mDNS daemon")?;
    let service_type = "_kitsu-gw._tcp.local.";
    let hostname = format!(
        "kitsu-{}.local.",
        &config.gateway_id.simple().to_string()[..12]
    );
    let properties: HashMap<String, String> = [
        ("proto".into(), GATEWAY_PROTOCOL_VERSION.to_string()),
        ("gateway_id".into(), config.gateway_id.to_string()),
        ("secure".into(), "mtls".into()),
        (
            "bootstrap_port".into(),
            config.bootstrap_listen.port().to_string(),
        ),
    ]
    .into_iter()
    .collect();
    let service = ServiceInfo::new(
        service_type,
        &config.public_name,
        &hostname,
        "",
        config.listen.port(),
        properties,
    )
    .context("construct mDNS service")?;
    daemon.register(service).context("register mDNS service")?;
    Ok(daemon)
}

#[cfg(test)]
mod startup_tests {
    use std::sync::Arc;

    use tokio::sync::Semaphore;

    #[test]
    fn installs_one_process_level_crypto_provider_before_tls_builders() {
        super::install_crypto_provider().unwrap();
        assert!(rustls::crypto::CryptoProvider::get_default().is_some());
    }

    #[test]
    fn connection_admission_fails_closed_at_capacity() {
        let concurrency = Arc::new(Semaphore::new(1));
        let permit = super::try_acquire_connection(&concurrency).unwrap();

        assert!(super::try_acquire_connection(&concurrency).is_none());

        drop(permit);
        assert!(super::try_acquire_connection(&concurrency).is_some());
    }

    #[test]
    fn capacity_warning_limiter_coalesces_rejection_floods() {
        let start = std::time::Instant::now();
        let mut limiter = super::CapacityWarningLimiter::default();

        assert_eq!(limiter.observe(start), Some(0));
        assert_eq!(
            limiter.observe(start + std::time::Duration::from_secs(1)),
            None
        );
        assert_eq!(
            limiter.observe(start + std::time::Duration::from_secs(2)),
            None
        );
        assert_eq!(
            limiter.observe(start + super::CAPACITY_WARNING_INTERVAL),
            Some(2)
        );
    }

    #[test]
    fn health_response_exposes_the_selected_deployment_scope() {
        let response = super::HealthResponse {
            status: "ok",
            protocol: 1,
            gateway_id: uuid::Uuid::nil(),
            deployment_scope: kitsu_home_gateway::config::DeploymentScope::Public,
            backend_online: false,
            backend_upload_online: false,
            backend_action_session_online: false,
            spool_pending: 0,
            spool_bytes: 0,
            devices_online: 0,
        };

        let encoded = serde_json::to_value(response).unwrap();
        assert_eq!(encoded["deployment_scope"], "public");
    }
}
