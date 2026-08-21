pub mod bootstrap;
pub mod browser;
pub mod gateway;
pub mod mobile_relay;
pub mod ops;
pub mod owner;
pub mod public;

use axum::{
    body::Body,
    http::{header, HeaderName, HeaderValue, Method, Request},
    middleware,
    routing::{get, post},
    Router,
};
use tower_http::{
    cors::{AllowOrigin, CorsLayer},
    limit::RequestBodyLimitLayer,
    request_id::{MakeRequestUuid, PropagateRequestIdLayer, SetRequestIdLayer},
    sensitive_headers::{SetSensitiveRequestHeadersLayer, SetSensitiveResponseHeadersLayer},
    set_header::SetResponseHeaderLayer,
    trace::TraceLayer,
};

use crate::{auth, state::AppState};

pub fn public_router(state: AppState) -> Router {
    let owner = Router::new()
        .route("/v1/browser/session", get(browser::session))
        .route("/v1/browser/logout", post(browser::logout_with_state))
        .route("/v1/browser/ws-ticket", post(browser::ws_ticket))
        .route(
            "/v1/account/client-attribution",
            get(owner::client_attribution),
        )
        .route("/v1/enrollments", post(owner::create_enrollment))
        .route(
            "/v1/gateway-bootstraps",
            post(owner::create_gateway_bootstrap),
        )
        .route(
            "/v1/gateways/{id}/certificate-rotations",
            post(owner::create_certificate_rotation),
        )
        .route("/v1/gateways", get(owner::list_gateways))
        .route(
            "/v1/mobile-relays/{installation_id}",
            get(mobile_relay::get_mobile_relay).put(mobile_relay::put_mobile_relay),
        )
        .route(
            "/v1/mobile-relays/{installation_id}/enrollments/{enrollment_id}/claim",
            post(mobile_relay::claim_enrollment),
        )
        .route(
            "/v1/mobile-relays/{installation_id}/envelopes",
            post(mobile_relay::ingest_envelope),
        )
        .route(
            "/v1/mobile-relays/{installation_id}/session",
            get(mobile_relay::session),
        )
        .route(
            "/v1/gateways/{gateway_id}/certificates/{certificate_id}/revoke",
            post(owner::revoke_gateway_certificate),
        )
        .route("/v1/companions", get(owner::list_companions))
        .route("/v1/contact-messages", get(owner::list_public_contacts))
        .route(
            "/v1/contact-messages/{id}/resolve",
            post(owner::resolve_public_contact),
        )
        .route(
            "/v1/account/deletion",
            get(owner::account_deletion).delete(owner::request_account_deletion),
        )
        .route(
            "/v1/account/deletion/cancel",
            post(owner::cancel_account_deletion),
        )
        .route("/v1/companions/{id}/snapshot", get(owner::snapshot))
        .route("/v1/companions/{id}/peers", get(owner::peers))
        .route("/v1/companions/{id}/channels", get(owner::channels))
        .route("/v1/companions/{id}/events", get(owner::events))
        .route(
            "/v1/companions/{id}/actions",
            get(owner::list_actions).post(owner::create_action),
        )
        .route_layer(middleware::from_fn_with_state(
            state.clone(),
            auth::require_owner,
        ));

    let allowed_origins = state
        .config
        .browser_allowed_origins
        .iter()
        .map(|origin| {
            HeaderValue::from_str(&origin.origin().ascii_serialization())
                .expect("validated browser origin")
        })
        .collect::<Vec<_>>();
    let cors = CorsLayer::new()
        .allow_origin(AllowOrigin::list(allowed_origins))
        .allow_credentials(true)
        .allow_methods([
            Method::GET,
            Method::HEAD,
            Method::POST,
            Method::PUT,
            Method::PATCH,
            Method::DELETE,
            Method::OPTIONS,
        ])
        .allow_headers([
            header::AUTHORIZATION,
            header::CONTENT_TYPE,
            header::IF_NONE_MATCH,
            HeaderName::from_static("x-csrf-token"),
            HeaderName::from_static("idempotency-key"),
        ])
        .expose_headers([header::ETAG]);
    let sensitive = vec![
        header::AUTHORIZATION,
        header::COOKIE,
        HeaderName::from_bytes(state.config.mtls_proxy_auth_header.as_bytes())
            .expect("validated proxy-auth header"),
        HeaderName::from_bytes(state.config.mtls_xfcc_header.as_bytes())
            .expect("validated XFCC header"),
    ];

    Router::new()
        .route("/health/live", get(public::live))
        .route("/v1/auth/config", get(public::auth_config))
        .route("/v1/contact", post(public::contact))
        .route("/v1/browser/auth/start", get(browser::auth_start))
        .route("/v1/browser/auth/callback", get(browser::auth_callback))
        .route("/v1/browser/ws", get(browser::websocket))
        .route(
            "/v1/gateway-bootstraps/{id}/claim",
            post(bootstrap::claim_gateway_bootstrap),
        )
        .route(
            "/v1/gateway/enrollments/{id}/claim",
            post(gateway::claim_enrollment),
        )
        .route(
            "/v1/gateway/certificate-rotations/{id}/activate",
            post(gateway::activate_certificate_rotation),
        )
        .route("/v1/gateway/envelopes", post(gateway::ingest_envelope))
        .route("/v1/gateway/session", get(gateway::session))
        .route(
            "/v1/gateway/catalog",
            axum::routing::put(gateway::put_catalog),
        )
        .merge(owner)
        .layer(SetResponseHeaderLayer::if_not_present(
            header::X_CONTENT_TYPE_OPTIONS,
            HeaderValue::from_static("nosniff"),
        ))
        .layer(SetResponseHeaderLayer::if_not_present(
            header::REFERRER_POLICY,
            HeaderValue::from_static("no-referrer"),
        ))
        .layer(SetSensitiveResponseHeadersLayer::new([header::SET_COOKIE]))
        .layer(SetSensitiveRequestHeadersLayer::new(sensitive))
        .layer(PropagateRequestIdLayer::x_request_id())
        .layer(SetRequestIdLayer::x_request_id(MakeRequestUuid))
        .layer(
            TraceLayer::new_for_http().make_span_with(|request: &Request<Body>| {
                tracing::info_span!(
                    "http.request",
                    method = %request.method(),
                    path = request.uri().path(),
                    request_id = request
                        .headers()
                        .get("x-request-id")
                        .and_then(|value| value.to_str().ok())
                        .unwrap_or("")
                )
            }),
        )
        .layer(RequestBodyLimitLayer::new(state.config.max_request_bytes))
        .layer(cors)
        .with_state(state)
}

pub fn ops_router(state: AppState) -> Router {
    Router::new()
        .route("/health/ready", get(ops::ready))
        .route("/metrics", get(ops::metrics))
        .with_state(state)
}
