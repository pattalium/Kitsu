use std::{env, net::SocketAddr, path::PathBuf, str::FromStr, time::Duration};

use anyhow::{bail, Context};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use ipnet::IpNet;
use url::Url;
use zeroize::{Zeroize, ZeroizeOnDrop, Zeroizing};

pub struct SecretText(Zeroizing<String>);

impl SecretText {
    pub fn expose(&self) -> &str {
        self.0.as_str()
    }
}

impl Drop for SecretText {
    fn drop(&mut self) {
        self.0.zeroize();
    }
}

#[derive(Zeroize, ZeroizeOnDrop)]
pub struct BrowserStateKey(pub [u8; 32]);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProviderKind {
    Aws,
    Local,
}

impl FromStr for ProviderKind {
    type Err = anyhow::Error;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "aws" => Ok(Self::Aws),
            "local" => Ok(Self::Local),
            _ => bail!("provider must be either aws or local"),
        }
    }
}

#[derive(Clone)]
pub struct LocalKmsKeyConfig {
    pub id: String,
    pub path: PathBuf,
}

#[derive(Clone)]
pub struct LocalCaIdentityConfig {
    pub id: String,
    pub key_path: PathBuf,
    pub chain_path: PathBuf,
}

pub struct Config {
    pub public_base_url: Url,
    pub public_bind: SocketAddr,
    pub ops_bind: SocketAddr,
    pub database_url: SecretText,
    pub database_max_connections: u32,
    pub oidc_issuer: Url,
    pub oidc_internal_issuer: Url,
    pub oidc_api_audience: String,
    pub oidc_native_client_id: String,
    pub oidc_browser_client_id: String,
    pub oidc_browser_client_secret: SecretText,
    pub oidc_account_cleaner_client_id: String,
    pub oidc_account_cleaner_client_secret: SecretText,
    pub oidc_required_scope: String,
    pub browser_allowed_origins: Vec<Url>,
    pub browser_default_return_url: Url,
    pub browser_state_key: BrowserStateKey,
    pub browser_session_ttl: Duration,
    pub kms_provider: ProviderKind,
    pub kms_key_id: Option<String>,
    pub local_kms_current: Option<LocalKmsKeyConfig>,
    pub local_kms_previous: Vec<LocalKmsKeyConfig>,
    pub ca_provider: ProviderKind,
    pub private_ca_arn: Option<String>,
    pub private_ca_api_passthrough_template_arn: Option<String>,
    pub local_ca_current: Option<LocalCaIdentityConfig>,
    pub local_ca_previous: Vec<LocalCaIdentityConfig>,
    pub local_ca_job_dir: Option<PathBuf>,
    pub local_ca_crl_file: Option<PathBuf>,
    pub certificate_validity_days: i64,
    pub certificate_issue_timeout: Duration,
    pub trusted_mtls_proxy_cidrs: Vec<IpNet>,
    pub trusted_http_proxy_cidrs: Vec<IpNet>,
    pub http_client_ip_header: String,
    pub mtls_proxy_auth_token: SecretText,
    pub mtls_proxy_auth_header: String,
    pub mtls_xfcc_header: String,
    pub max_request_bytes: usize,
    pub enrollment_ttl: Duration,
    pub gateway_bootstrap_ttl: Duration,
    pub certificate_rotation_ttl: Duration,
    pub certificate_overlap: Duration,
    pub action_max_ttl: Duration,
    pub account_deletion_grace: Duration,
    pub event_retention_days: i32,
    pub action_retention_days: i32,
    pub audit_retention_days: i32,
    pub pet_pack_dir: PathBuf,
    pub log_filter: String,
}

impl Config {
    pub fn load() -> anyhow::Result<Self> {
        let public_base_url = url("KITSU_PUBLIC_BASE_URL")?;
        if public_base_url.scheme() != "https"
            && public_base_url.host_str() != Some("localhost")
            && public_base_url.host_str() != Some("127.0.0.1")
        {
            bail!("KITSU_PUBLIC_BASE_URL must use https outside localhost");
        }
        if public_base_url.query().is_some() || public_base_url.fragment().is_some() {
            bail!("KITSU_PUBLIC_BASE_URL cannot contain query or fragment");
        }

        let oidc_issuer = url("KITSU_OIDC_ISSUER")?;
        if oidc_issuer.scheme() != "https" {
            bail!("KITSU_OIDC_ISSUER must use https");
        }
        if oidc_issuer.query().is_some() || oidc_issuer.fragment().is_some() {
            bail!("KITSU_OIDC_ISSUER cannot contain query or fragment");
        }
        let oidc_internal_issuer = env::var("KITSU_OIDC_INTERNAL_ISSUER")
            .ok()
            .filter(|value| !value.trim().is_empty())
            .map(|value| Url::parse(&value).context("invalid KITSU_OIDC_INTERNAL_ISSUER"))
            .transpose()?
            .unwrap_or_else(|| oidc_issuer.clone());
        if oidc_internal_issuer.scheme() == "http"
            && !matches!(
                oidc_internal_issuer.host_str(),
                Some("127.0.0.1" | "localhost" | "::1")
            )
        {
            bail!("an HTTP KITSU_OIDC_INTERNAL_ISSUER must use a loopback host");
        }
        if !matches!(oidc_internal_issuer.scheme(), "http" | "https") {
            bail!("KITSU_OIDC_INTERNAL_ISSUER must use http or https");
        }
        if oidc_internal_issuer.query().is_some() || oidc_internal_issuer.fragment().is_some() {
            bail!("KITSU_OIDC_INTERNAL_ISSUER cannot contain query or fragment");
        }

        let browser_allowed_origins = csv("KITSU_BROWSER_ALLOWED_ORIGINS")?
            .into_iter()
            .map(|value| exact_origin(&value))
            .collect::<anyhow::Result<Vec<_>>>()?;
        if browser_allowed_origins.is_empty() {
            bail!("at least one authenticated browser origin is required");
        }

        let browser_default_return_url = url("KITSU_BROWSER_DEFAULT_RETURN_URL")?;
        if !browser_allowed_origins.iter().any(|origin| {
            origin.origin().ascii_serialization()
                == browser_default_return_url.origin().ascii_serialization()
        }) {
            bail!("browser default return URL must use an allowed origin");
        }

        let state_key_encoded = secret_required("KITSU_BROWSER_STATE_KEY")?;
        let state_key_bytes = URL_SAFE_NO_PAD
            .decode(state_key_encoded.as_bytes())
            .context("KITSU_BROWSER_STATE_KEY must be unpadded base64url")?;
        let browser_state_key = BrowserStateKey(
            state_key_bytes
                .try_into()
                .map_err(|_| anyhow::anyhow!("KITSU_BROWSER_STATE_KEY must decode to 32 bytes"))?,
        );

        let trusted_mtls_proxy_cidrs = csv("KITSU_TRUSTED_MTLS_PROXY_CIDRS")?
            .into_iter()
            .map(|value| IpNet::from_str(&value).with_context(|| format!("invalid CIDR {value}")))
            .collect::<anyhow::Result<Vec<_>>>()?;
        if trusted_mtls_proxy_cidrs.is_empty() {
            bail!("at least one trusted mTLS proxy CIDR is required");
        }
        let trusted_http_proxy_cidrs = csv("KITSU_TRUSTED_HTTP_PROXY_CIDRS")?
            .into_iter()
            .map(|value| IpNet::from_str(&value).with_context(|| format!("invalid CIDR {value}")))
            .collect::<anyhow::Result<Vec<_>>>()?;
        if trusted_http_proxy_cidrs.is_empty() {
            bail!("at least one trusted HTTP proxy CIDR is required");
        }
        let mtls_proxy_auth_token = secret32("KITSU_MTLS_PROXY_AUTH_TOKEN")?;

        let certificate_rotation_ttl =
            Duration::from_secs(parse("KITSU_CERTIFICATE_ROTATION_TTL_SECONDS")?);
        let certificate_overlap = Duration::from_secs(parse("KITSU_CERTIFICATE_OVERLAP_SECONDS")?);
        if !(60..=3600).contains(&certificate_rotation_ttl.as_secs()) {
            bail!("certificate rotation TTL must be between 60 and 3600 seconds");
        }
        if !(60..=86400).contains(&certificate_overlap.as_secs()) {
            bail!("certificate overlap must be between 60 and 86400 seconds");
        }
        let certificate_validity_days: i64 = parse("KITSU_CERTIFICATE_VALIDITY_DAYS")?;
        if !(1..=365).contains(&certificate_validity_days) {
            bail!("certificate validity must be between 1 and 365 days");
        }
        let certificate_issue_timeout =
            Duration::from_secs(parse("KITSU_CERTIFICATE_ISSUE_TIMEOUT_SECONDS")?);
        // The route adds a 30-second KMS/commit margin.  Keep that total below
        // the 195-second database lease so a live issuer cannot overlap a
        // takeover, and leave recovery time inside AWS PCA's five-minute
        // idempotency window if the process dies before persisting the ARN.
        if !(5..=150).contains(&certificate_issue_timeout.as_secs()) {
            bail!("certificate issue timeout must be between 5 and 150 seconds");
        }
        let kms_provider = ProviderKind::from_str(&required("KITSU_KMS_PROVIDER")?)?;
        let (kms_key_id, local_kms_current, local_kms_previous) = match kms_provider {
            ProviderKind::Aws => (Some(required("KITSU_KMS_KEY_ID")?), None, Vec::new()),
            ProviderKind::Local => {
                let current = LocalKmsKeyConfig {
                    id: provider_id("KITSU_LOCAL_KMS_CURRENT_KEY_ID")?,
                    path: absolute_path("KITSU_LOCAL_KMS_CURRENT_KEY_FILE")?,
                };
                let previous = optional("KITSU_LOCAL_KMS_PREVIOUS_KEYS")
                    .map(|value| parse_local_kms_keys(&value))
                    .transpose()?
                    .unwrap_or_default();
                ensure_unique_provider_ids(
                    std::iter::once(current.id.as_str())
                        .chain(previous.iter().map(|item| item.id.as_str())),
                    "local KMS key",
                )?;
                (None, Some(current), previous)
            }
        };

        let ca_provider = ProviderKind::from_str(&required("KITSU_CA_PROVIDER")?)?;
        let (
            private_ca_arn,
            private_ca_api_passthrough_template_arn,
            local_ca_current,
            local_ca_previous,
            local_ca_job_dir,
            local_ca_crl_file,
        ) = match ca_provider {
            ProviderKind::Aws => {
                let template = required("KITSU_PRIVATE_CA_API_PASSTHROUGH_TEMPLATE_ARN")?;
                if !template.contains(":template/") || !template.contains("APIPassthrough") {
                    bail!("KITSU_PRIVATE_CA_API_PASSTHROUGH_TEMPLATE_ARN must select an API-passthrough template");
                }
                (
                    Some(required("KITSU_PRIVATE_CA_ARN")?),
                    Some(template),
                    None,
                    Vec::new(),
                    None,
                    None,
                )
            }
            ProviderKind::Local => {
                let current = LocalCaIdentityConfig {
                    id: provider_id("KITSU_LOCAL_CA_CURRENT_ID")?,
                    key_path: absolute_path("KITSU_LOCAL_CA_CURRENT_KEY_FILE")?,
                    chain_path: absolute_path("KITSU_LOCAL_CA_CURRENT_CHAIN_FILE")?,
                };
                let previous = optional("KITSU_LOCAL_CA_PREVIOUS_IDENTITIES")
                    .map(|value| parse_local_ca_identities(&value))
                    .transpose()?
                    .unwrap_or_default();
                ensure_unique_provider_ids(
                    std::iter::once(current.id.as_str())
                        .chain(previous.iter().map(|item| item.id.as_str())),
                    "local CA identity",
                )?;
                (
                    None,
                    None,
                    Some(current),
                    previous,
                    Some(absolute_path("KITSU_LOCAL_CA_JOB_DIR")?),
                    Some(absolute_path("KITSU_LOCAL_CA_CRL_FILE")?),
                )
            }
        };
        let enrollment_ttl = Duration::from_secs(parse("KITSU_ENROLLMENT_TTL_SECONDS")?);
        let gateway_bootstrap_ttl =
            Duration::from_secs(parse("KITSU_GATEWAY_BOOTSTRAP_TTL_SECONDS")?);
        if !(60..=3_600).contains(&enrollment_ttl.as_secs())
            || !(60..=3_600).contains(&gateway_bootstrap_ttl.as_secs())
        {
            bail!("enrollment and gateway bootstrap TTLs must be between 60 and 3600 seconds");
        }
        // A maximum-size 256 KiB device payload expands to 349,526 bytes in
        // unpadded base64url before the bounded JSON wrapper is added.
        let max_request_bytes: usize = parse("KITSU_MAX_REQUEST_BYTES")?;
        if !(352 * 1024..=1024 * 1024).contains(&max_request_bytes) {
            bail!("maximum request body must be between 352 KiB and 1 MiB");
        }
        let account_deletion_grace_hours: u64 = parse("KITSU_ACCOUNT_DELETION_GRACE_HOURS")?;
        if !(24..=720).contains(&account_deletion_grace_hours) {
            bail!("account deletion grace must be between 24 and 720 hours");
        }
        let event_retention_days: i32 = parse("KITSU_EVENT_RETENTION_DAYS")?;
        let action_retention_days: i32 = parse("KITSU_ACTION_RETENTION_DAYS")?;
        let audit_retention_days: i32 = parse("KITSU_AUDIT_RETENTION_DAYS")?;
        if !(30..=365).contains(&event_retention_days)
            || !(30..=365).contains(&action_retention_days)
            || !(90..=2555).contains(&audit_retention_days)
        {
            bail!("retention periods are outside production policy bounds");
        }

        Ok(Self {
            public_base_url,
            public_bind: parse("KITSU_PUBLIC_BIND")?,
            ops_bind: parse("KITSU_OPS_BIND")?,
            database_url: SecretText(Zeroizing::new(secret_required("KITSU_DATABASE_URL")?)),
            database_max_connections: parse("KITSU_DATABASE_MAX_CONNECTIONS")?,
            oidc_issuer,
            oidc_internal_issuer,
            oidc_api_audience: required("KITSU_OIDC_API_AUDIENCE")?,
            oidc_native_client_id: required("KITSU_OIDC_NATIVE_CLIENT_ID")?,
            oidc_browser_client_id: required("KITSU_OIDC_BROWSER_CLIENT_ID")?,
            oidc_browser_client_secret: SecretText(Zeroizing::new(secret_required(
                "KITSU_OIDC_BROWSER_CLIENT_SECRET",
            )?)),
            oidc_account_cleaner_client_id: required("KITSU_OIDC_ACCOUNT_CLEANER_CLIENT_ID")?,
            oidc_account_cleaner_client_secret: SecretText(Zeroizing::new(secret_required(
                "KITSU_OIDC_ACCOUNT_CLEANER_CLIENT_SECRET",
            )?)),
            oidc_required_scope: required("KITSU_OIDC_REQUIRED_SCOPE")?,
            browser_allowed_origins,
            browser_default_return_url,
            browser_state_key,
            browser_session_ttl: Duration::from_secs(
                parse::<u64>("KITSU_BROWSER_SESSION_HOURS")? * 3600,
            ),
            kms_provider,
            kms_key_id,
            local_kms_current,
            local_kms_previous,
            ca_provider,
            private_ca_arn,
            private_ca_api_passthrough_template_arn,
            local_ca_current,
            local_ca_previous,
            local_ca_job_dir,
            local_ca_crl_file,
            certificate_validity_days,
            certificate_issue_timeout,
            trusted_mtls_proxy_cidrs,
            trusted_http_proxy_cidrs,
            http_client_ip_header: header_name("KITSU_HTTP_CLIENT_IP_HEADER")?,
            mtls_proxy_auth_token,
            mtls_proxy_auth_header: header_name("KITSU_MTLS_PROXY_AUTH_HEADER")?,
            mtls_xfcc_header: header_name("KITSU_MTLS_XFCC_HEADER")?,
            max_request_bytes,
            enrollment_ttl,
            gateway_bootstrap_ttl,
            certificate_rotation_ttl,
            certificate_overlap,
            action_max_ttl: Duration::from_secs(parse("KITSU_ACTION_MAX_TTL_SECONDS")?),
            account_deletion_grace: Duration::from_secs(account_deletion_grace_hours * 3600),
            event_retention_days,
            action_retention_days,
            audit_retention_days,
            pet_pack_dir: absolute_path("KITSU_PET_PACK_DIR")?,
            log_filter: required("KITSU_LOG_FILTER")?,
        })
    }
}

fn required(name: &'static str) -> anyhow::Result<String> {
    let value = env::var(name).with_context(|| format!("missing {name}"))?;
    if value.trim().is_empty() {
        bail!("{name} cannot be empty");
    }
    Ok(value)
}

fn optional(name: &'static str) -> Option<String> {
    env::var(name)
        .ok()
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
}

fn secret_required(name: &'static str) -> anyhow::Result<String> {
    let direct = optional(name);
    let file_name = format!("{name}_FILE");
    let file = env::var(&file_name)
        .ok()
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty());
    match (direct, file) {
        (Some(_), Some(_)) => bail!("set only one of {name} or {file_name}"),
        (Some(value), None) => Ok(value),
        (None, Some(path)) => {
            let path = PathBuf::from(path);
            if !path.is_absolute() {
                bail!("{file_name} must be an absolute path");
            }
            let metadata = std::fs::symlink_metadata(&path)
                .with_context(|| format!("read credential configured by {file_name}"))?;
            if !metadata.is_file() || metadata.len() > 16 * 1024 {
                bail!("{file_name} must reference a small regular credential file");
            }
            let value = std::fs::read_to_string(&path)
                .with_context(|| format!("read credential configured by {file_name}"))?;
            let value = value.trim_end_matches(['\r', '\n']).to_owned();
            if value.trim().is_empty() {
                bail!("credential configured by {file_name} cannot be empty");
            }
            Ok(value)
        }
        (None, None) => bail!("missing {name} or {file_name}"),
    }
}

fn absolute_path(name: &'static str) -> anyhow::Result<PathBuf> {
    let path = PathBuf::from(required(name)?);
    if !path.is_absolute() {
        bail!("{name} must be an absolute path");
    }
    Ok(path)
}

fn provider_id(name: &'static str) -> anyhow::Result<String> {
    validate_provider_id(&required(name)?).with_context(|| format!("invalid {name}"))
}

fn validate_provider_id(value: &str) -> anyhow::Result<String> {
    if value.is_empty()
        || value.len() > 48
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
    {
        bail!("provider ID must be 1-48 ASCII letters, digits, dots, underscores, or hyphens");
    }
    Ok(value.to_owned())
}

fn parse_local_kms_keys(value: &str) -> anyhow::Result<Vec<LocalKmsKeyConfig>> {
    value
        .split(',')
        .filter(|item| !item.trim().is_empty())
        .map(|item| {
            let mut fields = item.trim().split('|');
            let id = validate_provider_id(fields.next().unwrap_or_default())?;
            let path = PathBuf::from(
                fields
                    .next()
                    .context("local KMS previous-key entries must use id|absolute-key-path")?,
            );
            if fields.next().is_some() || !path.is_absolute() {
                bail!("local KMS previous-key entries must use id|absolute-key-path");
            }
            Ok(LocalKmsKeyConfig { id, path })
        })
        .collect()
}

fn parse_local_ca_identities(value: &str) -> anyhow::Result<Vec<LocalCaIdentityConfig>> {
    value
        .split(',')
        .filter(|item| !item.trim().is_empty())
        .map(|item| {
            let mut fields = item.trim().split('|');
            let id = validate_provider_id(fields.next().unwrap_or_default())?;
            let key_path = PathBuf::from(fields.next().context(
                "local CA previous-identity entries must use id|absolute-key-path|absolute-chain-path",
            )?);
            let chain_path = PathBuf::from(fields.next().context(
                "local CA previous-identity entries must use id|absolute-key-path|absolute-chain-path",
            )?);
            if fields.next().is_some() || !key_path.is_absolute() || !chain_path.is_absolute() {
                bail!("local CA previous-identity entries must use id|absolute-key-path|absolute-chain-path");
            }
            Ok(LocalCaIdentityConfig {
                id,
                key_path,
                chain_path,
            })
        })
        .collect()
}

fn ensure_unique_provider_ids<'a>(
    values: impl Iterator<Item = &'a str>,
    label: &str,
) -> anyhow::Result<()> {
    let mut seen = std::collections::HashSet::new();
    for value in values {
        if !seen.insert(value) {
            bail!("duplicate {label} ID: {value}");
        }
    }
    Ok(())
}

fn parse<T>(name: &'static str) -> anyhow::Result<T>
where
    T: FromStr,
    T::Err: std::error::Error + Send + Sync + 'static,
{
    required(name)?
        .parse::<T>()
        .with_context(|| format!("invalid {name}"))
}

fn csv(name: &'static str) -> anyhow::Result<Vec<String>> {
    Ok(required(name)?
        .split(',')
        .map(str::trim)
        .filter(|item| !item.is_empty())
        .map(ToOwned::to_owned)
        .collect())
}

fn url(name: &'static str) -> anyhow::Result<Url> {
    Url::parse(&required(name)?).with_context(|| format!("invalid {name}"))
}

fn exact_origin(value: &str) -> anyhow::Result<Url> {
    let parsed = Url::parse(value).context("invalid browser origin")?;
    if !matches!(parsed.scheme(), "http" | "https")
        || parsed.path() != "/"
        || parsed.query().is_some()
        || parsed.fragment().is_some()
        || parsed.username() != ""
        || parsed.password().is_some()
    {
        bail!("browser origins must contain only scheme, host, and optional port");
    }
    if parsed.scheme() != "https" && parsed.host_str() != Some("localhost") {
        bail!("browser origins must use https outside localhost");
    }
    Ok(parsed)
}

fn header_name(name: &'static str) -> anyhow::Result<String> {
    let value = required(name)?.to_ascii_lowercase();
    http::HeaderName::from_bytes(value.as_bytes())
        .with_context(|| format!("invalid header name in {name}"))?;
    Ok(value)
}

fn secret32(name: &'static str) -> anyhow::Result<SecretText> {
    let encoded = secret_required(name)?;
    let decoded = URL_SAFE_NO_PAD
        .decode(encoded.as_bytes())
        .with_context(|| format!("{name} must be unpadded base64url"))?;
    if decoded.len() != 32 || URL_SAFE_NO_PAD.encode(&decoded) != encoded {
        bail!("{name} must be canonical unpadded base64url encoding of 32 bytes");
    }
    Ok(SecretText(Zeroizing::new(encoded)))
}
