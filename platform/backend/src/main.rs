use std::{sync::Arc, time::Duration};

use anyhow::Context;
use metrics_exporter_prometheus::PrometheusBuilder;
use tokio::net::TcpListener;
use tracing_subscriber::{layer::SubscriberExt, util::SubscriberInitExt, EnvFilter};
use uuid::Uuid;

use kitsu_platform_backend::{
    config::{Config, ProviderKind},
    db::Database,
    issuer::DynCertificateIssuer,
    kms::DynKms,
    oidc::OidcClient,
    persistence::postgres::PgListener,
    routes,
    state::{AppState, ConnectionHubs},
};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let config = Arc::new(Config::load()?);
    tracing_subscriber::registry()
        .with(EnvFilter::new(&config.log_filter))
        .with(tracing_subscriber::fmt::layer().json())
        .init();

    let metrics = PrometheusBuilder::new().install_recorder()?;
    let db = Database::connect(
        config.database_url.expose(),
        config.database_max_connections,
    )
    .await
    .context("connect/migrate PostgreSQL")?;
    let oidc = OidcClient::discover(&config)
        .await
        .context("discover OIDC issuer")?;

    let kms = build_kms(&config).await?;
    let certificate_issuer = build_certificate_issuer(&config).await?;
    let enrollment_ca_cert_der = certificate_issuer
        .enrollment_ca_certificate_der()
        .await
        .context("load enrollment CA certificate")?;
    if enrollment_ca_cert_der.is_empty() || enrollment_ca_cert_der.len() > 8 * 1024 {
        anyhow::bail!("enrollment CA certificate has invalid size");
    }
    let state = AppState {
        config: config.clone(),
        db,
        oidc,
        kms,
        certificate_issuer,
        enrollment_ca_cert_der: Arc::new(enrollment_ca_cert_der),
        instance_id: Uuid::new_v4(),
        hubs: Arc::new(ConnectionHubs::new()),
        metrics,
    };

    refresh_crl(&state)
        .await
        .context("publish initial certificate revocation list")?;
    tokio::spawn(action_notification_loop(state.clone()));
    tokio::spawn(crl_refresh_loop(state.clone()));
    tokio::spawn(retention_loop(state.clone()));
    let public_listener = TcpListener::bind(config.public_bind).await?;
    let ops_listener = TcpListener::bind(config.ops_bind).await?;
    tracing::info!(
        public = %config.public_bind,
        operations = %config.ops_bind,
        instance_id = %state.instance_id,
        "Kitsu backend listening"
    );

    let shutdown = shutdown_signal();
    let public = axum::serve(
        public_listener,
        routes::public_router(state.clone())
            .into_make_service_with_connect_info::<std::net::SocketAddr>(),
    )
    .with_graceful_shutdown(shutdown);
    let operations = axum::serve(ops_listener, routes::ops_router(state).into_make_service());

    tokio::select! {
        result = public => result?,
        result = operations => result?,
    }
    Ok(())
}

async fn build_kms(config: &Config) -> anyhow::Result<DynKms> {
    match config.kms_provider {
        ProviderKind::Aws => {
            #[cfg(feature = "aws-kms")]
            {
                let key_id = config
                    .kms_key_id
                    .clone()
                    .context("AWS KMS provider is missing its key ID")?;
                Ok(Arc::new(
                    kitsu_platform_backend::kms::AwsKmsProvider::new(key_id).await,
                ))
            }
            #[cfg(not(feature = "aws-kms"))]
            anyhow::bail!("backend was built without the aws-kms feature")
        }
        ProviderKind::Local => {
            #[cfg(feature = "local-kms")]
            {
                let current = config
                    .local_kms_current
                    .as_ref()
                    .context("local KMS provider is missing its current key")?;
                Ok(Arc::new(
                    kitsu_platform_backend::kms::LocalKmsProvider::load(
                        current,
                        &config.local_kms_previous,
                    )?,
                ))
            }
            #[cfg(not(feature = "local-kms"))]
            anyhow::bail!("backend was built without the local-kms feature")
        }
    }
}

async fn build_certificate_issuer(config: &Config) -> anyhow::Result<DynCertificateIssuer> {
    match config.ca_provider {
        ProviderKind::Aws => {
            #[cfg(feature = "aws-private-ca")]
            {
                Ok(Arc::new(
                    kitsu_platform_backend::issuer::AwsPrivateCaIssuer::new(
                        config
                            .private_ca_arn
                            .clone()
                            .context("AWS Private CA provider is missing its CA ARN")?,
                        config
                            .private_ca_api_passthrough_template_arn
                            .clone()
                            .context("AWS Private CA provider is missing its template ARN")?,
                        config.certificate_validity_days,
                        config.certificate_issue_timeout,
                    )
                    .await,
                ))
            }
            #[cfg(not(feature = "aws-private-ca"))]
            anyhow::bail!("backend was built without the aws-private-ca feature")
        }
        ProviderKind::Local => {
            #[cfg(feature = "local-ca")]
            {
                Ok(Arc::new(
                    kitsu_platform_backend::issuer::LocalCertificateIssuer::load(
                        config
                            .local_ca_current
                            .as_ref()
                            .context("local CA provider is missing its current identity")?,
                        &config.local_ca_previous,
                        config.certificate_validity_days,
                        config
                            .local_ca_job_dir
                            .clone()
                            .context("local CA provider is missing its job directory")?,
                        config
                            .local_ca_crl_file
                            .clone()
                            .context("local CA provider is missing its CRL path")?,
                    )?,
                ))
            }
            #[cfg(not(feature = "local-ca"))]
            anyhow::bail!("backend was built without the local-ca feature")
        }
    }
}

async fn refresh_crl(state: &AppState) -> anyhow::Result<()> {
    let revoked = state
        .db
        .revoked_certificates()
        .await
        .map_err(|_| anyhow::anyhow!("load certificate revocations"))?;
    state
        .certificate_issuer
        .publish_crl(&revoked)
        .await
        .map_err(|_| anyhow::anyhow!("publish certificate revocations"))
}

async fn crl_refresh_loop(state: AppState) {
    let mut interval = tokio::time::interval(Duration::from_secs(60));
    interval.tick().await;
    loop {
        interval.tick().await;
        if let Err(error) = refresh_crl(&state).await {
            tracing::error!(error = %error, "certificate revocation refresh failed");
        }
    }
}

async fn retention_loop(state: AppState) {
    let mut interval = tokio::time::interval(Duration::from_secs(3600));
    loop {
        interval.tick().await;
        if let Err(error) = state
            .db
            .apply_retention(
                state.config.event_retention_days,
                state.config.action_retention_days,
                state.config.audit_retention_days,
            )
            .await
        {
            tracing::error!(error = %error, "retention pass failed");
            continue;
        }
        let due = match state.db.prepare_due_account_deletions().await {
            Ok(due) => due,
            Err(error) => {
                tracing::error!(error = %error, "account deletion preparation failed");
                continue;
            }
        };
        let mut completed = 0_u64;
        for deletion in due {
            let result = async {
                if !deletion.identity_revoked {
                    state
                        .oidc
                        .delete_identity(&deletion.issuer, &deletion.subject)
                        .await?;
                    state
                        .db
                        .mark_account_identity_revoked(deletion.owner_id)
                        .await?;
                }
                state.db.process_account_deletion(deletion.owner_id).await
            }
            .await;
            match result {
                Ok(()) => completed += 1,
                Err(error) => tracing::error!(
                    owner_id = %deletion.owner_id,
                    error = %error,
                    "scheduled account deletion failed and will retry"
                ),
            }
        }
        if completed > 0 {
            tracing::info!(completed, "completed scheduled account deletions");
            if let Err(error) = refresh_crl(&state).await {
                tracing::error!(error = %error, "CRL refresh after account deletion failed");
            }
        }
    }
}

async fn action_notification_loop(state: AppState) {
    loop {
        let listener = PgListener::connect_with(state.db.pool()).await;
        let mut listener = match listener {
            Ok(listener) => listener,
            Err(error) => {
                tracing::error!(error = %error, "failed to connect PostgreSQL notification listener");
                tokio::time::sleep(Duration::from_secs(5)).await;
                continue;
            }
        };
        if let Err(error) = listener.listen("kitsu_remote_actions").await {
            tracing::error!(error = %error, "failed to LISTEN for remote actions");
            tokio::time::sleep(Duration::from_secs(5)).await;
            continue;
        }
        loop {
            let notification = match listener.recv().await {
                Ok(notification) => notification,
                Err(error) => {
                    tracing::warn!(error = %error, "remote-action LISTEN connection lost");
                    break;
                }
            };
            let Ok(companion_id) = Uuid::parse_str(notification.payload()) else {
                tracing::warn!("ignored malformed remote-action notification");
                continue;
            };
            let Ok(Some(gateway_id)) = state.db.gateway_for_companion(companion_id).await else {
                continue;
            };
            let Ok(Some(gateway)) = state.db.gateway_by_id(gateway_id).await else {
                continue;
            };
            let Ok(actions) = state.db.pending_actions(&gateway).await else {
                continue;
            };
            for action in actions {
                let _ = state.hubs.send_gateway(gateway_id, action).await;
            }
        }
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
}

async fn shutdown_signal() {
    let ctrl_c = async {
        tokio::signal::ctrl_c()
            .await
            .expect("install Ctrl-C handler");
    };
    #[cfg(unix)]
    let terminate = async {
        tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("install SIGTERM handler")
            .recv()
            .await;
    };
    #[cfg(not(unix))]
    let terminate = std::future::pending::<()>();
    tokio::select! {
        _ = ctrl_c => {},
        _ = terminate => {},
    }
    tracing::info!("shutdown requested");
}
