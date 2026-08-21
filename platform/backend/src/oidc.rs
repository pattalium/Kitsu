use std::{collections::HashSet, sync::Arc, time::Duration};

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use chrono::Utc;
use jsonwebtoken::{decode, decode_header, jwk::JwkSet, Algorithm, DecodingKey, Validation};
use serde::{Deserialize, Serialize};
use tokio::sync::RwLock;
use url::Url;
use uuid::Uuid;
use zeroize::Zeroizing;

use crate::{
    config::Config,
    crypto::{random_token, sha256},
    error::ApiError,
};

const JWKS_MAX_AGE: Duration = Duration::from_secs(15 * 60);

#[derive(Clone, Deserialize, Serialize)]
pub struct OidcDiscovery {
    pub issuer: String,
    pub authorization_endpoint: String,
    pub token_endpoint: String,
    pub jwks_uri: String,
}

#[derive(Clone)]
pub struct OidcPrincipal {
    pub issuer: String,
    pub subject: String,
    pub email: Option<String>,
    pub display_name: Option<String>,
}

#[derive(Deserialize)]
struct Claims {
    iss: String,
    sub: String,
    exp: i64,
    #[serde(default)]
    nbf: Option<i64>,
    #[serde(default)]
    aud: Audience,
    #[serde(default)]
    scope: Option<String>,
    #[serde(default)]
    scp: Vec<String>,
    #[serde(default)]
    nonce: Option<String>,
    #[serde(default)]
    email: Option<String>,
    #[serde(default)]
    name: Option<String>,
}

#[derive(Default, Deserialize)]
#[serde(untagged)]
enum Audience {
    One(String),
    Many(Vec<String>),
    #[default]
    Missing,
}

impl Audience {
    fn includes(&self, required: &str) -> bool {
        match self {
            Self::One(value) => value == required,
            Self::Many(values) => values.iter().any(|value| value == required),
            Self::Missing => false,
        }
    }
}

struct CachedJwks {
    value: JwkSet,
    fetched_at: std::time::Instant,
}

pub struct BrowserAuthorization {
    pub url: Url,
    pub verifier: Zeroizing<String>,
    pub nonce: Zeroizing<String>,
}

#[derive(Deserialize)]
pub struct TokenResponse {
    pub access_token: String,
    pub token_type: String,
    pub expires_in: u64,
    pub id_token: String,
}

#[derive(Deserialize)]
struct ClientCredentialsTokenResponse {
    access_token: String,
    token_type: String,
    expires_in: u64,
}

pub struct OidcClient {
    http: reqwest::Client,
    discovery: OidcDiscovery,
    internal_token_endpoint: String,
    internal_jwks_uri: String,
    issuer: String,
    api_audience: String,
    browser_client_id: String,
    browser_client_secret: Zeroizing<String>,
    account_cleaner_client_id: String,
    account_cleaner_client_secret: Zeroizing<String>,
    internal_admin_users: Url,
    required_scope: String,
    jwks: RwLock<CachedJwks>,
}

impl OidcClient {
    pub async fn discover(config: &Config) -> anyhow::Result<Arc<Self>> {
        let http = reqwest::Client::builder()
            .https_only(config.oidc_internal_issuer.scheme() == "https")
            .timeout(Duration::from_secs(10))
            .user_agent("kitsu-platform-backend/0.1")
            .build()?;
        let discovery_url = issuer_subpath(
            &config.oidc_internal_issuer,
            ".well-known/openid-configuration",
        )?;
        let discovery: OidcDiscovery = http
            .get(discovery_url)
            .send()
            .await?
            .error_for_status()?
            .json()
            .await?;
        // OpenID Connect defines the issuer identifier as an exact string.
        // In particular, silently trimming a trailing slash would accept a
        // different issuer namespace and can make discovery, tokens, and the
        // owner uniqueness key disagree.
        if discovery.issuer != config.oidc_issuer.as_str() {
            anyhow::bail!("OIDC discovery issuer does not match configured issuer");
        }
        for endpoint in [
            &discovery.authorization_endpoint,
            &discovery.token_endpoint,
            &discovery.jwks_uri,
        ] {
            let parsed = Url::parse(endpoint)?;
            if parsed.scheme() != "https" {
                anyhow::bail!("OIDC metadata endpoint is not HTTPS");
            }
        }
        let internal_token_endpoint = internal_endpoint(
            &discovery.token_endpoint,
            &config.oidc_issuer,
            &config.oidc_internal_issuer,
        )?;
        let internal_jwks_uri = internal_endpoint(
            &discovery.jwks_uri,
            &config.oidc_issuer,
            &config.oidc_internal_issuer,
        )?;
        let internal_admin_users = keycloak_admin_users_endpoint(&config.oidc_internal_issuer)?;
        let jwks = fetch_jwks(&http, internal_jwks_uri.as_str()).await?;
        Ok(Arc::new(Self {
            http,
            issuer: discovery.issuer.clone(),
            api_audience: config.oidc_api_audience.clone(),
            browser_client_id: config.oidc_browser_client_id.clone(),
            browser_client_secret: Zeroizing::new(
                config.oidc_browser_client_secret.expose().to_owned(),
            ),
            account_cleaner_client_id: config.oidc_account_cleaner_client_id.clone(),
            account_cleaner_client_secret: Zeroizing::new(
                config
                    .oidc_account_cleaner_client_secret
                    .expose()
                    .to_owned(),
            ),
            internal_admin_users,
            required_scope: config.oidc_required_scope.clone(),
            discovery,
            internal_token_endpoint: internal_token_endpoint.to_string(),
            internal_jwks_uri: internal_jwks_uri.to_string(),
            jwks: RwLock::new(CachedJwks {
                value: jwks,
                fetched_at: std::time::Instant::now(),
            }),
        }))
    }

    pub fn discovery(&self) -> &OidcDiscovery {
        &self.discovery
    }

    pub async fn health(&self) -> Result<(), ApiError> {
        let jwks = fetch_jwks(&self.http, &self.internal_jwks_uri)
            .await
            .map_err(|_| ApiError::Unavailable)?;
        if jwks.keys.is_empty() {
            return Err(ApiError::Unavailable);
        }
        Ok(())
    }

    pub async fn verify_access_token(&self, token: &str) -> Result<OidcPrincipal, ApiError> {
        self.verify_jwt(token, &self.api_audience, None, true).await
    }

    /// Permanently remove one exact Keycloak identity. The dedicated client is
    /// granted only `manage-users` in the Kitsu realm, and the public nginx
    /// origin never exposes `/admin`.
    pub async fn delete_identity(&self, issuer: &str, subject: &str) -> Result<(), ApiError> {
        if issuer != self.issuer || Uuid::parse_str(subject).is_err() {
            return Err(ApiError::Invalid("invalid account identity"));
        }
        let response = self
            .http
            .post(&self.internal_token_endpoint)
            .form(&[
                ("grant_type", "client_credentials"),
                ("client_id", self.account_cleaner_client_id.as_str()),
                ("client_secret", self.account_cleaner_client_secret.as_str()),
            ])
            .send()
            .await
            .map_err(|_| ApiError::Unavailable)?;
        if !response.status().is_success() {
            return Err(ApiError::Unavailable);
        }
        let token: ClientCredentialsTokenResponse =
            response.json().await.map_err(|_| ApiError::Unavailable)?;
        if !token.token_type.eq_ignore_ascii_case("bearer")
            || token.access_token.is_empty()
            || token.expires_in == 0
        {
            return Err(ApiError::Unavailable);
        }
        let endpoint = self
            .internal_admin_users
            .join(subject)
            .map_err(ApiError::internal)?;
        let response = self
            .http
            .delete(endpoint)
            .bearer_auth(token.access_token)
            .send()
            .await
            .map_err(|_| ApiError::Unavailable)?;
        if response.status().is_success() || response.status() == reqwest::StatusCode::NOT_FOUND {
            Ok(())
        } else {
            Err(ApiError::Unavailable)
        }
    }

    pub async fn verify_browser_id_token(
        &self,
        token: &str,
        nonce: &str,
    ) -> Result<OidcPrincipal, ApiError> {
        self.verify_jwt(token, &self.browser_client_id, Some(nonce), false)
            .await
    }

    pub fn browser_authorization(
        &self,
        state: &str,
        redirect_uri: &Url,
    ) -> Result<BrowserAuthorization, ApiError> {
        let verifier = random_token(32);
        let nonce = random_token(32);
        let challenge = URL_SAFE_NO_PAD.encode(sha256(verifier.as_bytes()));
        let mut url =
            Url::parse(&self.discovery.authorization_endpoint).map_err(ApiError::internal)?;
        url.query_pairs_mut()
            .append_pair("response_type", "code")
            .append_pair("client_id", &self.browser_client_id)
            .append_pair("redirect_uri", redirect_uri.as_str())
            .append_pair("scope", "openid profile email")
            .append_pair("state", state)
            .append_pair("nonce", nonce.as_str())
            .append_pair("code_challenge", &challenge)
            .append_pair("code_challenge_method", "S256");
        Ok(BrowserAuthorization {
            url,
            verifier,
            nonce,
        })
    }

    pub async fn exchange_browser_code(
        &self,
        code: &str,
        verifier: &str,
        redirect_uri: &Url,
    ) -> Result<TokenResponse, ApiError> {
        let response = self
            .http
            .post(&self.internal_token_endpoint)
            .form(&[
                ("grant_type", "authorization_code"),
                ("client_id", self.browser_client_id.as_str()),
                ("client_secret", self.browser_client_secret.as_str()),
                ("code", code),
                ("code_verifier", verifier),
                ("redirect_uri", redirect_uri.as_str()),
            ])
            .send()
            .await
            .map_err(|_| ApiError::Unavailable)?;
        if !response.status().is_success() {
            return Err(ApiError::Unauthorized);
        }
        let tokens: TokenResponse = response.json().await.map_err(|_| ApiError::Unavailable)?;
        if !tokens.token_type.eq_ignore_ascii_case("bearer")
            || tokens.access_token.is_empty()
            || tokens.id_token.is_empty()
            || tokens.expires_in == 0
        {
            return Err(ApiError::Unauthorized);
        }
        Ok(tokens)
    }

    async fn verify_jwt(
        &self,
        token: &str,
        audience: &str,
        expected_nonce: Option<&str>,
        require_scope: bool,
    ) -> Result<OidcPrincipal, ApiError> {
        if token.len() > 16 * 1024 {
            return Err(ApiError::Unauthorized);
        }
        let header = decode_header(token).map_err(|_| ApiError::Unauthorized)?;
        if !matches!(header.alg, Algorithm::RS256 | Algorithm::ES256) {
            return Err(ApiError::Unauthorized);
        }
        let kid = header.kid.ok_or(ApiError::Unauthorized)?;

        let mut jwk = self.find_jwk(&kid).await;
        if jwk.is_none() {
            self.refresh_jwks().await?;
            jwk = self.find_jwk(&kid).await;
        }
        let jwk = jwk.ok_or(ApiError::Unauthorized)?;
        let key = DecodingKey::from_jwk(&jwk).map_err(|_| ApiError::Unauthorized)?;
        let mut validation = Validation::new(header.alg);
        validation.set_issuer(&[self.issuer.as_str()]);
        validation.set_audience(&[audience]);
        // Pin verification to the algorithm declared by this token after the
        // explicit allowlist check above. Do not leave a multi-algorithm
        // verifier configured when one concrete JWK/signature is expected.
        validation.algorithms = vec![header.alg];
        validation.leeway = 30;
        let claims = decode::<Claims>(token, &key, &validation)
            .map_err(|_| ApiError::Unauthorized)?
            .claims;

        let now = Utc::now().timestamp();
        if claims.iss != self.issuer
            || claims.sub.is_empty()
            || claims.sub.len() > 255
            || claims.exp <= now
            || claims.nbf.is_some_and(|nbf| nbf > now + 30)
            || !claims.aud.includes(audience)
            || expected_nonce.is_some_and(|expected| claims.nonce.as_deref() != Some(expected))
        {
            return Err(ApiError::Unauthorized);
        }
        if require_scope {
            let mut scopes: HashSet<&str> = claims
                .scope
                .as_deref()
                .unwrap_or_default()
                .split_ascii_whitespace()
                .collect();
            scopes.extend(claims.scp.iter().map(String::as_str));
            if !scopes.contains(self.required_scope.as_str()) {
                return Err(ApiError::Forbidden);
            }
        }
        Ok(OidcPrincipal {
            issuer: self.issuer.clone(),
            subject: claims.sub,
            email: claims.email.filter(|value| value.len() <= 320),
            display_name: claims.name.filter(|value| value.len() <= 160),
        })
    }

    async fn find_jwk(&self, kid: &str) -> Option<jsonwebtoken::jwk::Jwk> {
        let cache = self.jwks.read().await;
        if cache.fetched_at.elapsed() > JWKS_MAX_AGE {
            drop(cache);
            let _ = self.refresh_jwks().await;
            let refreshed = self.jwks.read().await;
            return refreshed.value.find(kid).cloned();
        }
        cache.value.find(kid).cloned()
    }

    async fn refresh_jwks(&self) -> Result<(), ApiError> {
        let value = fetch_jwks(&self.http, &self.internal_jwks_uri)
            .await
            .map_err(|_| ApiError::Unavailable)?;
        *self.jwks.write().await = CachedJwks {
            value,
            fetched_at: std::time::Instant::now(),
        };
        Ok(())
    }
}

async fn fetch_jwks(http: &reqwest::Client, uri: &str) -> anyhow::Result<JwkSet> {
    Ok(http
        .get(uri)
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?)
}

fn issuer_subpath(issuer: &Url, suffix: &str) -> anyhow::Result<Url> {
    if suffix.is_empty() || suffix.starts_with('/') || suffix.contains("..") {
        anyhow::bail!("invalid OIDC issuer subpath");
    }
    let base = Url::parse(&format!("{}/", issuer.as_str().trim_end_matches('/')))?;
    Ok(base.join(suffix)?)
}

fn internal_endpoint(external: &str, issuer: &Url, internal: &Url) -> anyhow::Result<Url> {
    let external = Url::parse(external)?;
    let public_prefix = format!("{}/", issuer.as_str().trim_end_matches('/'));
    let suffix = external
        .as_str()
        .strip_prefix(&public_prefix)
        .ok_or_else(|| {
            anyhow::anyhow!("OIDC metadata endpoint is outside the configured issuer")
        })?;
    if suffix.is_empty() || external.fragment().is_some() {
        anyhow::bail!("invalid OIDC metadata endpoint");
    }
    let internal_prefix = Url::parse(&format!("{}/", internal.as_str().trim_end_matches('/')))?;
    Ok(internal_prefix.join(suffix)?)
}

fn keycloak_admin_users_endpoint(internal_issuer: &Url) -> anyhow::Result<Url> {
    let segments = internal_issuer
        .path_segments()
        .ok_or_else(|| anyhow::anyhow!("OIDC internal issuer cannot be a base URL"))?
        .filter(|segment| !segment.is_empty())
        .collect::<Vec<_>>();
    if segments.len() != 2 || segments[0] != "realms" || segments[1].is_empty() {
        anyhow::bail!("KITSU_OIDC_INTERNAL_ISSUER must name one exact Keycloak realm");
    }
    let mut endpoint = internal_issuer.clone();
    endpoint.set_path(&format!("/admin/realms/{}/users/", segments[1]));
    endpoint.set_query(None);
    endpoint.set_fragment(None);
    Ok(endpoint)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rewrites_only_endpoints_below_the_exact_public_issuer() {
        let public = Url::parse("https://auth.k32.run/realms/kitsu").unwrap();
        let internal = Url::parse("http://127.0.0.1:8789/realms/kitsu").unwrap();
        assert_eq!(
            internal_endpoint(
                "https://auth.k32.run/realms/kitsu/protocol/openid-connect/certs",
                &public,
                &internal,
            )
            .unwrap()
            .as_str(),
            "http://127.0.0.1:8789/realms/kitsu/protocol/openid-connect/certs"
        );
        assert!(internal_endpoint(
            "https://auth.k32.run/realms/other/protocol/openid-connect/certs",
            &public,
            &internal,
        )
        .is_err());
    }

    #[test]
    fn discovery_path_preserves_the_issuer_realm_segment() {
        let issuer = Url::parse("http://127.0.0.1:8789/realms/kitsu").unwrap();
        assert_eq!(
            issuer_subpath(&issuer, ".well-known/openid-configuration")
                .unwrap()
                .as_str(),
            "http://127.0.0.1:8789/realms/kitsu/.well-known/openid-configuration"
        );
    }

    #[test]
    fn issuer_identifiers_are_not_trailing_slash_equivalent() {
        let configured = Url::parse("https://auth.k32.run/realms/kitsu").unwrap();
        assert_ne!(configured.as_str(), "https://auth.k32.run/realms/kitsu/");
    }

    #[test]
    fn account_deletion_admin_endpoint_is_internal_and_realm_scoped() {
        let issuer = Url::parse("http://127.0.0.1:8789/realms/kitsu").unwrap();
        assert_eq!(
            keycloak_admin_users_endpoint(&issuer).unwrap().as_str(),
            "http://127.0.0.1:8789/admin/realms/kitsu/users/"
        );
        assert!(
            keycloak_admin_users_endpoint(&Url::parse("http://127.0.0.1:8789/").unwrap()).is_err()
        );
    }
}
