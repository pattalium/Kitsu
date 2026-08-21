use axum::{
    extract::{Request, State},
    http::{header, HeaderMap, Method},
    middleware::Next,
    response::Response,
};
use chrono::{DateTime, Utc};
use subtle::ConstantTimeEq;
use uuid::Uuid;

use crate::{
    crypto::sha256_text,
    db::{BrowserSession, Owner},
    error::ApiError,
    state::AppState,
};

pub const SESSION_COOKIE: &str = "__Host-kitsu_session";
pub const OAUTH_STATE_COOKIE: &str = "__Host-kitsu_oauth_state";
pub const CSRF_COOKIE: &str = "__Host-kitsu_csrf";

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum AuthMode {
    Bearer,
    Browser,
}

#[derive(Clone)]
pub struct OwnerAuth {
    pub owner: Owner,
    pub mode: AuthMode,
    pub browser_session_id: Option<Uuid>,
    pub csrf_digest: Option<[u8; 32]>,
    pub expires_at: Option<DateTime<Utc>>,
}

pub async fn require_owner(
    State(state): State<AppState>,
    mut request: Request,
    next: Next,
) -> Result<Response, ApiError> {
    let auth = authenticate(&state, request.headers()).await?;
    validate_csrf_if_needed(&state, &request, &auth)?;
    request.extensions_mut().insert(auth);
    Ok(next.run(request).await)
}

fn validate_csrf_if_needed(
    state: &AppState,
    request: &Request,
    auth: &OwnerAuth,
) -> Result<(), ApiError> {
    if auth.mode == AuthMode::Browser && is_mutation(request.method()) {
        let origin = request
            .headers()
            .get(header::ORIGIN)
            .and_then(|value| value.to_str().ok())
            .ok_or(ApiError::Forbidden)?;
        if !state
            .config
            .browser_allowed_origins
            .iter()
            .any(|allowed| allowed.origin().ascii_serialization() == origin)
        {
            return Err(ApiError::Forbidden);
        }
        let supplied = request
            .headers()
            .get("x-csrf-token")
            .and_then(|value| value.to_str().ok())
            .ok_or(ApiError::Forbidden)?;
        if supplied.len() > 256 {
            return Err(ApiError::Forbidden);
        }
        let actual = sha256_text(supplied);
        let expected = auth.csrf_digest.ok_or(ApiError::Forbidden)?;
        if !bool::from(actual.ct_eq(&expected)) {
            return Err(ApiError::Forbidden);
        }
    }
    Ok(())
}

pub fn cookie(headers: &HeaderMap, name: &str) -> Option<String> {
    headers
        .get(header::COOKIE)?
        .to_str()
        .ok()?
        .split(';')
        .filter_map(|pair| pair.trim().split_once('='))
        .find_map(|(candidate, value)| (candidate == name).then(|| value.to_owned()))
}

pub fn session_cookie(token: &str, max_age_seconds: u64) -> String {
    format!(
        "{SESSION_COOKIE}={token}; Path=/; Max-Age={max_age_seconds}; Secure; HttpOnly; SameSite=Lax"
    )
}

pub fn clear_session_cookie() -> String {
    format!("{SESSION_COOKIE}=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Lax")
}

pub fn csrf_cookie(token: &str, max_age_seconds: u64) -> String {
    format!("{CSRF_COOKIE}={token}; Path=/; Max-Age={max_age_seconds}; Secure; SameSite=Strict")
}

pub fn clear_csrf_cookie() -> String {
    format!("{CSRF_COOKIE}=; Path=/; Max-Age=0; Secure; SameSite=Strict")
}

pub fn oauth_state_cookie(token: &str) -> String {
    format!(
        "{OAUTH_STATE_COOKIE}={token}; Path=/v1/browser/auth/callback; Max-Age=600; Secure; HttpOnly; SameSite=Lax"
    )
}

pub fn clear_oauth_state_cookie() -> String {
    format!(
        "{OAUTH_STATE_COOKIE}=; Path=/v1/browser/auth/callback; Max-Age=0; Secure; HttpOnly; SameSite=Lax"
    )
}

async fn authenticate(state: &AppState, headers: &HeaderMap) -> Result<OwnerAuth, ApiError> {
    if let Some(value) = headers.get(header::AUTHORIZATION) {
        let value = value.to_str().map_err(|_| ApiError::Unauthorized)?;
        let token = value
            .strip_prefix("Bearer ")
            .filter(|token| !token.is_empty())
            .ok_or(ApiError::Unauthorized)?;
        let principal = state.oidc.verify_access_token(token).await?;
        let owner = state.db.upsert_owner(&principal).await?;
        return Ok(OwnerAuth {
            owner,
            mode: AuthMode::Bearer,
            browser_session_id: None,
            csrf_digest: None,
            expires_at: None,
        });
    }

    let token = cookie(headers, SESSION_COOKIE).ok_or(ApiError::Unauthorized)?;
    if token.len() > 256 {
        return Err(ApiError::Unauthorized);
    }
    let BrowserSession {
        id,
        owner,
        csrf_digest,
        expires_at,
        ..
    } = state.db.browser_session(&sha256_text(&token)).await?;
    Ok(OwnerAuth {
        owner,
        mode: AuthMode::Browser,
        browser_session_id: Some(id),
        csrf_digest: Some(csrf_digest),
        expires_at: Some(expires_at),
    })
}

fn is_mutation(method: &Method) -> bool {
    !matches!(*method, Method::GET | Method::HEAD | Method::OPTIONS)
}
