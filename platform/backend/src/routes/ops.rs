use axum::{extract::State, http::StatusCode, response::IntoResponse};

use crate::state::AppState;

pub async fn ready(State(state): State<AppState>) -> impl IntoResponse {
    let result = tokio::time::timeout(std::time::Duration::from_secs(8), async {
        let (database, oidc, kms, issuer) = tokio::join!(
            async { state.db.ready().await },
            state.oidc.health(),
            state.kms.health(),
            state.certificate_issuer.health(),
        );
        database && oidc.is_ok() && kms.is_ok() && issuer.is_ok()
    })
    .await
    .unwrap_or(false);
    metrics::gauge!("kitsu_backend_ready").set(if result { 1.0 } else { 0.0 });
    if result {
        (StatusCode::OK, "ready")
    } else {
        tracing::warn!("backend aggregate readiness check failed");
        (StatusCode::SERVICE_UNAVAILABLE, "not ready")
    }
}

pub async fn metrics(State(state): State<AppState>) -> String {
    state.metrics.render()
}
