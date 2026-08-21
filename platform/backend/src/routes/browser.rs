use std::{net::SocketAddr, time::Duration};

use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        ConnectInfo, Extension, Query, State,
    },
    http::{header, HeaderMap, HeaderValue, StatusCode},
    response::{IntoResponse, Redirect, Response},
    Json,
};
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use subtle::ConstantTimeEq;
use url::Url;
use uuid::Uuid;
use zeroize::{Zeroize, ZeroizeOnDrop, Zeroizing};

use crate::{
    auth::{
        clear_csrf_cookie, clear_oauth_state_cookie, clear_session_cookie, cookie, csrf_cookie,
        oauth_state_cookie, session_cookie, OwnerAuth, CSRF_COOKIE, OAUTH_STATE_COOKIE,
    },
    client_ip::trusted_client_ip,
    crypto::{
        decrypt_browser_state, encrypt_browser_state, random_token, sha256_text, EncryptedBytes,
    },
    error::ApiError,
    state::AppState,
};

#[derive(Deserialize)]
pub struct StartQuery {
    return_url: Option<String>,
}

#[derive(Deserialize)]
pub struct CallbackQuery {
    code: String,
    state: String,
}

#[derive(Serialize, Deserialize, Zeroize, ZeroizeOnDrop)]
struct StoredBrowserSecret {
    verifier: String,
    nonce: String,
}

#[derive(Serialize)]
pub struct BrowserSessionView {
    authenticated: bool,
    owner_id: Uuid,
    csrf_cookie_name: &'static str,
    csrf_token: String,
    expires_at: chrono::DateTime<chrono::Utc>,
}

#[derive(Serialize)]
pub struct WsTicketView {
    ticket: String,
    expires_in_seconds: u8,
}

#[derive(Deserialize)]
pub struct WsQuery {
    ticket: String,
}

pub async fn auth_start(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Query(query): Query<StartQuery>,
) -> Result<Response, ApiError> {
    let client_ip = trusted_client_ip(&state.config, remote, &headers)?;
    state
        .db
        .check_rate_limit(
            "browser.auth.start",
            &crate::crypto::sha256(client_ip.to_string().as_bytes()),
            30,
            60,
        )
        .await?;
    let return_url = allowed_return_url(
        &state,
        query
            .return_url
            .as_deref()
            .unwrap_or(state.config.browser_default_return_url.as_str()),
    )?;
    let state_token = random_token(32);
    let state_cookie_token = random_token(32);
    let redirect_uri = state
        .config
        .public_base_url
        .join("/v1/browser/auth/callback")
        .map_err(ApiError::internal)?;
    let authorization = state
        .oidc
        .browser_authorization(state_token.as_str(), &redirect_uri)?;
    let attempt_id = Uuid::new_v4();
    let stored = StoredBrowserSecret {
        verifier: authorization.verifier.to_string(),
        nonce: authorization.nonce.to_string(),
    };
    let stored_bytes = Zeroizing::new(serde_json::to_vec(&stored).map_err(ApiError::internal)?);
    let encrypted = encrypt_browser_state(
        &state.config.browser_state_key.0,
        attempt_id,
        stored_bytes.as_slice(),
    )?;
    state
        .db
        .create_browser_oauth_attempt(
            attempt_id,
            &sha256_text(state_token.as_str()),
            &sha256_text(state_cookie_token.as_str()),
            &sha256_text(authorization.nonce.as_str()),
            &encrypted.nonce,
            &encrypted.ciphertext,
            return_url.as_str(),
            chrono::Utc::now() + chrono::TimeDelta::minutes(10),
        )
        .await?;
    let mut response = Redirect::temporary(authorization.url.as_str()).into_response();
    response.headers_mut().insert(
        header::SET_COOKIE,
        HeaderValue::from_str(&oauth_state_cookie(state_cookie_token.as_str()))
            .map_err(ApiError::internal)?,
    );
    mark_no_store(&mut response);
    Ok(response)
}

pub async fn auth_callback(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Query(query): Query<CallbackQuery>,
) -> Result<Response, ApiError> {
    let client_ip = trusted_client_ip(&state.config, remote, &headers)?;
    state
        .db
        .check_rate_limit(
            "browser.auth.callback",
            &crate::crypto::sha256(client_ip.to_string().as_bytes()),
            60,
            600,
        )
        .await?;
    if query.code.len() > 4096 || query.state.len() > 256 {
        return Err(ApiError::Unauthorized);
    }
    let cookie_token = cookie(&headers, OAUTH_STATE_COOKIE).ok_or(ApiError::Unauthorized)?;
    let attempt = state
        .db
        .consume_browser_oauth_attempt(&sha256_text(&query.state), &sha256_text(&cookie_token))
        .await?;
    let encrypted = EncryptedBytes {
        nonce: attempt.pkce_nonce,
        ciphertext: attempt.pkce_ciphertext,
    };
    let stored = decrypt_browser_state(&state.config.browser_state_key.0, attempt.id, &encrypted)?;
    let secret: StoredBrowserSecret =
        serde_json::from_slice(&stored).map_err(|_| ApiError::Unauthorized)?;
    if !bool::from(sha256_text(&secret.nonce).ct_eq(&attempt.nonce_digest)) {
        return Err(ApiError::Unauthorized);
    }
    let redirect_uri = state
        .config
        .public_base_url
        .join("/v1/browser/auth/callback")
        .map_err(ApiError::internal)?;
    let tokens = state
        .oidc
        .exchange_browser_code(&query.code, &secret.verifier, &redirect_uri)
        .await?;
    let principal = state
        .oidc
        .verify_browser_id_token(&tokens.id_token, &secret.nonce)
        .await?;
    let owner = state.db.upsert_owner(&principal).await?;
    let session_token = random_token(32);
    let csrf_token = random_token(32);
    let ttl = state.config.browser_session_ttl;
    let expires_at = chrono::Utc::now()
        + chrono::TimeDelta::seconds(i64::try_from(ttl.as_secs()).map_err(ApiError::internal)?);
    let user_agent_digest = headers
        .get(header::USER_AGENT)
        .and_then(|value| value.to_str().ok())
        .map(sha256_text);
    state
        .db
        .create_browser_session(
            owner.id,
            &sha256_text(session_token.as_str()),
            &sha256_text(csrf_token.as_str()),
            expires_at,
            user_agent_digest.as_ref(),
        )
        .await?;

    let mut response = Redirect::to(&attempt.return_url).into_response();
    let max_age = ttl.as_secs();
    response.headers_mut().append(
        header::SET_COOKIE,
        HeaderValue::from_str(&session_cookie(session_token.as_str(), max_age))
            .map_err(ApiError::internal)?,
    );
    response.headers_mut().append(
        header::SET_COOKIE,
        HeaderValue::from_str(&csrf_cookie(csrf_token.as_str(), max_age))
            .map_err(ApiError::internal)?,
    );
    response.headers_mut().append(
        header::SET_COOKIE,
        HeaderValue::from_str(&clear_oauth_state_cookie()).map_err(ApiError::internal)?,
    );
    mark_no_store(&mut response);
    Ok(response)
}

pub async fn session(
    Extension(auth): Extension<OwnerAuth>,
    headers: HeaderMap,
) -> Result<Response, ApiError> {
    auth.browser_session_id.ok_or(ApiError::Unauthorized)?;
    let csrf_token = cookie(&headers, CSRF_COOKIE).ok_or(ApiError::Forbidden)?;
    if csrf_token.len() > 256 {
        return Err(ApiError::Forbidden);
    }
    let actual = sha256_text(&csrf_token);
    let expected = auth.csrf_digest.ok_or(ApiError::Forbidden)?;
    if !bool::from(actual.ct_eq(&expected)) {
        return Err(ApiError::Forbidden);
    }
    let mut response = Json(BrowserSessionView {
        authenticated: true,
        owner_id: auth.owner.id,
        csrf_cookie_name: CSRF_COOKIE,
        csrf_token,
        expires_at: auth.expires_at.ok_or(ApiError::Unauthorized)?,
    })
    .into_response();
    mark_no_store(&mut response);
    Ok(response)
}

pub async fn logout_with_state(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
) -> Result<Response, ApiError> {
    let session_id = auth.browser_session_id.ok_or(ApiError::Unauthorized)?;
    state.db.revoke_browser_session(session_id).await?;
    let mut response = StatusCode::NO_CONTENT.into_response();
    response.headers_mut().append(
        header::SET_COOKIE,
        HeaderValue::from_str(&clear_session_cookie()).map_err(ApiError::internal)?,
    );
    response.headers_mut().append(
        header::SET_COOKIE,
        HeaderValue::from_str(&clear_csrf_cookie()).map_err(ApiError::internal)?,
    );
    mark_no_store(&mut response);
    Ok(response)
}

pub async fn ws_ticket(
    State(state): State<AppState>,
    Extension(auth): Extension<OwnerAuth>,
) -> Result<Response, ApiError> {
    let session_id = auth.browser_session_id.ok_or(ApiError::Unauthorized)?;
    let ticket = random_token(32);
    state
        .db
        .create_ws_ticket(session_id, &sha256_text(ticket.as_str()))
        .await?;
    let mut response = Json(WsTicketView {
        ticket: ticket.to_string(),
        expires_in_seconds: 30,
    })
    .into_response();
    mark_no_store(&mut response);
    Ok(response)
}

pub async fn websocket(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<WsQuery>,
    upgrade: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    let origin = headers
        .get(header::ORIGIN)
        .and_then(|value| value.to_str().ok())
        .ok_or(ApiError::Forbidden)?;
    if !state
        .config
        .browser_allowed_origins
        .iter()
        .any(|allowed| allowed.origin().ascii_serialization() == origin)
        || query.ticket.len() > 256
    {
        return Err(ApiError::Forbidden);
    }
    let owner = state
        .db
        .consume_ws_ticket(&sha256_text(&query.ticket))
        .await?;
    Ok(upgrade
        .max_message_size(64 * 1024)
        .on_upgrade(move |socket| owner_socket(state, owner.id, socket)))
}

async fn owner_socket(state: AppState, owner_id: Uuid, socket: WebSocket) {
    let (connection_id, mut events) = state.hubs.register_owner(owner_id).await;
    let (mut sender, mut receiver) = socket.split();
    let mut heartbeat = tokio::time::interval(Duration::from_secs(30));
    loop {
        tokio::select! {
            Some(event) = events.recv() => {
                if sender.send(Message::Text(event.to_string().into())).await.is_err() {
                    break;
                }
            }
            _ = heartbeat.tick() => {
                if sender.send(Message::Ping(Vec::new().into())).await.is_err() {
                    break;
                }
            }
            incoming = receiver.next() => {
                match incoming {
                    Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                    Some(Ok(Message::Ping(value))) => {
                        if sender.send(Message::Pong(value)).await.is_err() { break; }
                    }
                    // Browser commands intentionally use CSRF-protected REST.
                    Some(Ok(Message::Text(_))) | Some(Ok(Message::Binary(_))) => {
                        let _ = sender.send(Message::Close(None)).await;
                        break;
                    }
                    Some(Ok(_)) => {}
                }
            }
        }
    }
    state.hubs.unregister_owner(owner_id, connection_id).await;
}

fn allowed_return_url(state: &AppState, raw: &str) -> Result<Url, ApiError> {
    let parsed = Url::parse(raw).map_err(|_| ApiError::Invalid("invalid return URL"))?;
    if parsed.username() != ""
        || parsed.password().is_some()
        || parsed.fragment().is_some()
        || !state.config.browser_allowed_origins.iter().any(|allowed| {
            allowed.origin().ascii_serialization() == parsed.origin().ascii_serialization()
        })
    {
        return Err(ApiError::Invalid("return URL origin is not allowed"));
    }
    Ok(parsed)
}

fn mark_no_store(response: &mut Response) {
    response.headers_mut().insert(
        header::CACHE_CONTROL,
        HeaderValue::from_static("no-store, max-age=0"),
    );
}
