use std::net::SocketAddr;

use axum::{
    extract::{ConnectInfo, Form, State},
    http::{header, HeaderMap},
    response::Redirect,
    Json,
};
use serde::{Deserialize, Serialize};

use crate::{
    client_ip::trusted_client_ip, crypto::contact_source_digest, error::ApiError, state::AppState,
};

#[derive(Serialize)]
pub struct AuthConfiguration {
    issuer: String,
    authorization_endpoint: String,
    native_client_id: String,
    api_audience: String,
    required_scope: String,
    native_flow: &'static str,
    pkce_required: bool,
    browser_bff_start: &'static str,
}

pub async fn live() -> &'static str {
    "ok"
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PublicContactRequest {
    category: String,
    reply_contact: String,
    message: String,
    #[serde(default)]
    website: String,
}

pub async fn contact(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Form(request): Form<PublicContactRequest>,
) -> Result<Redirect, ApiError> {
    if headers
        .get(header::ORIGIN)
        .and_then(|value| value.to_str().ok())
        != Some("https://k32.run")
    {
        return Err(ApiError::Forbidden);
    }
    // Honeypot submissions receive the same browser response without creating
    // storage or revealing filtering behavior.
    if !request.website.is_empty() {
        return Ok(Redirect::to("https://k32.run/contact/?sent=1"));
    }
    if !matches!(
        request.category.as_str(),
        "security" | "privacy" | "abuse" | "service"
    ) || request.reply_contact.trim() != request.reply_contact
        || !(3..=320).contains(&request.reply_contact.len())
        || request.reply_contact.chars().any(char::is_control)
        || request.message.trim() != request.message
        || !(20..=4000).contains(&request.message.len())
        || request
            .message
            .chars()
            .any(|character| character.is_control() && !matches!(character, '\n' | '\t'))
    {
        return Err(ApiError::Invalid("invalid contact message"));
    }
    let address = trusted_client_ip(&state.config, remote, &headers)?;
    let source = contact_source_digest(&state.config.browser_state_key.0, &address.to_string());
    state
        .db
        .check_rate_limit("public.contact.create", &source, 5, 86_400)
        .await?;
    state
        .db
        .create_public_contact(
            &request.category,
            &request.reply_contact,
            &request.message,
            &source,
        )
        .await?;
    Ok(Redirect::to("https://k32.run/contact/?sent=1"))
}

pub async fn auth_config(State(state): State<AppState>) -> Json<AuthConfiguration> {
    let discovery = state.oidc.discovery();
    Json(AuthConfiguration {
        issuer: state.config.oidc_issuer.as_str().to_owned(),
        authorization_endpoint: discovery.authorization_endpoint.clone(),
        native_client_id: state.config.oidc_native_client_id.clone(),
        api_audience: state.config.oidc_api_audience.clone(),
        required_scope: state.config.oidc_required_scope.clone(),
        native_flow: "authorization_code",
        pkce_required: true,
        browser_bff_start: "/v1/browser/auth/start",
    })
}
