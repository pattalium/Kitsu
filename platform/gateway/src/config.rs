use std::{
    fs::{self, File},
    io::Read,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
    path::{Path, PathBuf},
    time::Duration,
};

use anyhow::{bail, Context};
use clap::{Parser, ValueEnum};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use url::Url;
use uuid::Uuid;

const MIN_SPOOL_BYTES: u64 = 8 * 1024 * 1024;
const MAX_FRAME_BYTES_LIMIT: usize = 512 * 1024;
const MAX_BOOTSTRAP_CONCURRENCY_LIMIT: usize = 128;
const MAX_STEADY_CONCURRENCY_LIMIT: usize = 4_096;
const PUBLIC_INSTANCE_MANIFEST_SCHEMA: &str = "kitsu.public-gateway-instance.v1";
const PUBLIC_INSTANCE_MANIFEST_MAX_BYTES: u64 = 16 * 1024;
const MAX_MANIFEST_PATH_BYTES: usize = 4_096;
const MAX_BACKEND_ORIGIN_BYTES: usize = 2_048;

#[derive(Debug, Clone, Copy, Default, Deserialize, Eq, PartialEq, Serialize, ValueEnum)]
#[serde(rename_all = "lowercase")]
pub enum DeploymentScope {
    #[default]
    Private,
    Public,
}

impl std::fmt::Display for DeploymentScope {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Private => formatter.write_str("private"),
            Self::Public => formatter.write_str("public"),
        }
    }
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct PublicInstanceManifest {
    schema: String,
    scope: DeploymentScope,
    gateway_id: Uuid,
    canonical_spool_root: String,
    backend_origin: String,
    server_certificate_sha256: String,
    device_ca_sha256: String,
    gateway_client_identity_sha256: String,
}

#[derive(Debug, Clone, Parser)]
#[command(name = "kitsu-gateway", version, about = "Kitsu home gateway")]
pub struct GatewayConfig {
    #[arg(long, env = "KITSU_GATEWAY_ID")]
    pub gateway_id: Uuid,

    #[arg(
        long,
        env = "KITSU_DEPLOYMENT_SCOPE",
        value_enum,
        default_value_t = DeploymentScope::Private
    )]
    pub deployment_scope: DeploymentScope,

    #[arg(long, env = "KITSU_LISTEN", default_value = "127.0.0.1:7443")]
    pub listen: SocketAddr,

    #[arg(long, env = "KITSU_BOOTSTRAP_LISTEN", default_value = "127.0.0.1:7442")]
    pub bootstrap_listen: SocketAddr,

    #[arg(long, env = "KITSU_ADMIN_LISTEN", default_value = "127.0.0.1:7444")]
    pub admin_listen: SocketAddr,

    #[arg(long, env = "KITSU_PUBLIC_NAME", default_value = "Kitsu Home Gateway")]
    pub public_name: String,

    #[arg(long, env = "KITSU_SERVER_CERT")]
    pub server_cert: PathBuf,

    #[arg(long, env = "KITSU_SERVER_KEY")]
    pub server_key: PathBuf,

    #[arg(long, env = "KITSU_DEVICE_CA")]
    pub device_ca: PathBuf,

    #[arg(long, env = "KITSU_BACKEND_URL")]
    pub backend_url: Url,

    #[arg(long, env = "KITSU_BACKEND_CA")]
    pub backend_ca: Option<PathBuf>,

    #[arg(long, env = "KITSU_GATEWAY_CLIENT_IDENTITY")]
    pub gateway_client_identity: PathBuf,

    #[arg(long, env = "KITSU_PUBLIC_INSTANCE_MANIFEST")]
    pub public_instance_manifest: Option<PathBuf>,

    #[arg(long, env = "KITSU_SPOOL_DIR", default_value = "./data/spool")]
    pub spool_dir: PathBuf,

    #[arg(long, env = "KITSU_SPOOL_MAX_BYTES", default_value_t = 512 * 1024 * 1024)]
    pub spool_max_bytes: u64,

    #[arg(long, env = "KITSU_SEGMENT_MAX_BYTES", default_value_t = 4 * 1024 * 1024)]
    pub segment_max_bytes: u64,

    #[arg(long, env = "KITSU_MAX_FRAME_BYTES", default_value_t = MAX_FRAME_BYTES_LIMIT)]
    pub max_frame_bytes: usize,

    #[arg(long, env = "KITSU_FRAME_TIMEOUT_SECONDS", default_value_t = 15)]
    pub frame_timeout_seconds: u64,

    #[arg(long, env = "KITSU_BOOTSTRAP_CONCURRENCY_LIMIT", default_value_t = 4)]
    pub bootstrap_concurrency_limit: usize,

    #[arg(long, env = "KITSU_STEADY_CONCURRENCY_LIMIT", default_value_t = 256)]
    pub steady_concurrency_limit: usize,

    #[arg(
        long,
        env = "KITSU_MDNS",
        default_value_t = true,
        action = clap::ArgAction::Set
    )]
    pub mdns: bool,
}

impl GatewayConfig {
    pub fn validate(&self) -> anyhow::Result<()> {
        if self.gateway_id.is_nil() {
            bail!("gateway ID must not be nil");
        }
        if self.bootstrap_listen == self.listen
            || self.bootstrap_listen == self.admin_listen
            || self.listen == self.admin_listen
        {
            bail!("bootstrap, steady-device, and admin listeners must be distinct");
        }
        if self.public_name.trim().is_empty()
            || self.public_name.len() > 63
            || self.public_name.chars().any(char::is_control)
        {
            bail!("public name must contain 1..63 bytes");
        }
        if self.backend_url.scheme() != "https" {
            bail!("backend base URL must use HTTPS");
        }
        if self.backend_url.username() != "" || self.backend_url.password().is_some() {
            bail!("backend URL must not contain credentials");
        }
        if self.backend_url.path() != "/"
            || self.backend_url.query().is_some()
            || self.backend_url.fragment().is_some()
        {
            bail!("backend URL must be an HTTPS origin without a path, query, or fragment");
        }
        if self.spool_max_bytes < MIN_SPOOL_BYTES {
            bail!("spool limit must be at least {MIN_SPOOL_BYTES} bytes");
        }
        if self.segment_max_bytes < 64 * 1024 || self.segment_max_bytes > self.spool_max_bytes {
            bail!("segment limit must be between 64 KiB and the spool limit");
        }
        if self.max_frame_bytes == 0 || self.max_frame_bytes > MAX_FRAME_BYTES_LIMIT {
            bail!("frame limit must be between 1 and {MAX_FRAME_BYTES_LIMIT} bytes");
        }
        if self.frame_timeout_seconds == 0 || self.frame_timeout_seconds > 120 {
            bail!("frame timeout must be between 1 and 120 seconds");
        }
        self.validate_runtime_policy()?;

        for (label, path) in [
            ("server certificate", &self.server_cert),
            ("server private key", &self.server_key),
            ("device CA", &self.device_ca),
            ("gateway client identity", &self.gateway_client_identity),
        ] {
            if !path.is_file() {
                bail!("{label} does not exist: {}", path.display());
            }
        }
        if let Some(path) = &self.backend_ca {
            if !path.is_file() {
                bail!("backend CA does not exist: {}", path.display());
            }
        }

        if self.deployment_scope == DeploymentScope::Private {
            fs::create_dir_all(&self.spool_dir)
                .with_context(|| format!("create spool directory {}", self.spool_dir.display()))?;
        }
        let spool_label = if self.deployment_scope == DeploymentScope::Public {
            "pre-provisioned public spool directory"
        } else {
            "spool directory"
        };
        let spool_metadata = fs::symlink_metadata(&self.spool_dir)
            .with_context(|| format!("inspect {spool_label} {}", self.spool_dir.display()))?;
        if spool_metadata.file_type().is_symlink() || !spool_metadata.is_dir() {
            bail!(
                "spool path must be a real directory, not a symlink or another file type: {}",
                self.spool_dir.display()
            );
        }
        let canonical_spool_root = fs::canonicalize(&self.spool_dir).with_context(|| {
            format!("canonicalize spool directory {}", self.spool_dir.display())
        })?;
        if self.deployment_scope == DeploymentScope::Public {
            self.validate_public_instance_manifest(&canonical_spool_root)?;
        }
        Ok(())
    }

    fn validate_runtime_policy(&self) -> anyhow::Result<()> {
        validate_concurrency_limit(
            "bootstrap",
            self.bootstrap_concurrency_limit,
            MAX_BOOTSTRAP_CONCURRENCY_LIMIT,
        )?;
        validate_concurrency_limit(
            "steady-device",
            self.steady_concurrency_limit,
            MAX_STEADY_CONCURRENCY_LIMIT,
        )?;

        if !self.admin_listen.ip().is_loopback() {
            bail!("admin listener must use a loopback address");
        }
        match self.deployment_scope {
            DeploymentScope::Private => {
                if !is_private_listener_ip(self.listen.ip())
                    || !is_private_listener_ip(self.bootstrap_listen.ip())
                {
                    bail!(
                        "private deployment listeners must use explicit LAN, link-local, or loopback addresses; wildcard and globally routable addresses are forbidden"
                    );
                }
                if self.public_instance_manifest.is_some() {
                    bail!("a public instance manifest is valid only in public deployment scope");
                }
            }
            DeploymentScope::Public => {
                if self.mdns {
                    bail!("public deployment scope requires mDNS to be disabled");
                }
                if self.public_instance_manifest.is_none() {
                    bail!("public deployment scope requires a public instance manifest");
                }
            }
        }
        Ok(())
    }

    fn validate_public_instance_manifest(&self, canonical_spool_root: &Path) -> anyhow::Result<()> {
        let path = self
            .public_instance_manifest
            .as_deref()
            .context("public deployment scope requires a public instance manifest")?;
        let bytes = read_bounded_regular_file(
            path,
            PUBLIC_INSTANCE_MANIFEST_MAX_BYTES,
            "public instance manifest",
        )?;
        let manifest: PublicInstanceManifest =
            serde_json::from_slice(&bytes).context("parse strict public instance manifest JSON")?;

        if manifest.schema != PUBLIC_INSTANCE_MANIFEST_SCHEMA {
            bail!("public instance manifest schema must be {PUBLIC_INSTANCE_MANIFEST_SCHEMA}");
        }
        if manifest.scope != DeploymentScope::Public {
            bail!("public instance manifest scope must be public");
        }
        if manifest.gateway_id != self.gateway_id {
            bail!("public instance manifest gateway ID does not match configuration");
        }
        if manifest.canonical_spool_root.is_empty()
            || manifest.canonical_spool_root.len() > MAX_MANIFEST_PATH_BYTES
        {
            bail!("public instance manifest spool root is outside its size bound");
        }
        let declared_spool_root = PathBuf::from(&manifest.canonical_spool_root);
        if !declared_spool_root.is_absolute() {
            bail!("public instance manifest spool root must be an absolute canonical path");
        }
        let canonical_declared_spool_root = fs::canonicalize(&declared_spool_root)
            .context("canonicalize public instance manifest spool root")?;
        if declared_spool_root != canonical_declared_spool_root {
            bail!("public instance manifest spool root is not canonical");
        }
        if canonical_declared_spool_root != canonical_spool_root {
            bail!("public instance manifest spool root does not match configuration");
        }
        if manifest.backend_origin.is_empty()
            || manifest.backend_origin.len() > MAX_BACKEND_ORIGIN_BYTES
        {
            bail!("public instance manifest backend origin is outside its size bound");
        }
        if manifest.backend_origin != self.backend_url.as_str() {
            bail!("public instance manifest backend origin does not match configuration");
        }

        validate_manifest_fingerprint(
            "server certificate",
            &manifest.server_certificate_sha256,
            &self.server_cert,
        )?;
        validate_manifest_fingerprint("device CA", &manifest.device_ca_sha256, &self.device_ca)?;
        validate_manifest_fingerprint(
            "gateway client identity",
            &manifest.gateway_client_identity_sha256,
            &self.gateway_client_identity,
        )?;
        Ok(())
    }

    pub fn frame_timeout(&self) -> Duration {
        Duration::from_secs(self.frame_timeout_seconds)
    }

    pub fn backend_session_url(&self) -> anyhow::Result<Url> {
        let mut url = self
            .backend_url
            .join("/v1/gateway/session")
            .context("build backend gateway-session URL")?;
        url.set_scheme("wss")
            .map_err(|_| anyhow::anyhow!("convert backend session URL to WSS"))?;
        Ok(url)
    }
}

fn validate_concurrency_limit(label: &str, value: usize, maximum: usize) -> anyhow::Result<()> {
    if value == 0 || value > maximum {
        bail!("{label} concurrency limit must be between 1 and {maximum}");
    }
    Ok(())
}

fn is_private_listener_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => is_private_ipv4(ip),
        IpAddr::V6(ip) => is_private_ipv6(ip),
    }
}

fn is_private_ipv4(ip: Ipv4Addr) -> bool {
    ip.is_private() || ip.is_loopback() || ip.is_link_local()
}

fn is_private_ipv6(ip: Ipv6Addr) -> bool {
    let first = ip.octets()[0];
    let first_segment = ip.segments()[0];
    ip.is_loopback() || first & 0xfe == 0xfc || first_segment & 0xffc0 == 0xfe80
}

fn read_bounded_regular_file(path: &Path, maximum: u64, label: &str) -> anyhow::Result<Vec<u8>> {
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("inspect {label} {}", path.display()))?;
    if metadata.file_type().is_symlink() {
        bail!("{label} must not be a symlink: {}", path.display());
    }
    if !metadata.is_file() {
        bail!("{label} must be a regular file: {}", path.display());
    }
    if metadata.len() > maximum {
        bail!("{label} exceeds the {maximum}-byte limit");
    }

    let file = File::open(path).with_context(|| format!("open {label} {}", path.display()))?;
    if !file
        .metadata()
        .with_context(|| format!("inspect open {label} {}", path.display()))?
        .is_file()
    {
        bail!("{label} changed to a non-regular file while opening");
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.take(maximum + 1)
        .read_to_end(&mut bytes)
        .with_context(|| format!("read {label} {}", path.display()))?;
    if bytes.len() as u64 > maximum {
        bail!("{label} exceeds the {maximum}-byte limit");
    }
    Ok(bytes)
}

fn validate_manifest_fingerprint(label: &str, declared: &str, path: &Path) -> anyhow::Result<()> {
    if declared.len() != 64
        || !declared
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        bail!("public instance manifest {label} SHA-256 must be 64 lowercase hex characters");
    }
    let actual = sha256_file(path).with_context(|| format!("hash {label} file"))?;
    if declared != actual {
        bail!("public instance manifest {label} SHA-256 does not match configuration");
    }
    Ok(())
}

fn sha256_file(path: &Path) -> anyhow::Result<String> {
    let mut file = File::open(path).with_context(|| format!("open {}", path.display()))?;
    let mut digest = Sha256::new();
    let mut buffer = [0_u8; 16 * 1024];
    loop {
        let read = file
            .read(&mut buffer)
            .with_context(|| format!("read {}", path.display()))?;
        if read == 0 {
            break;
        }
        digest.update(&buffer[..read]);
    }
    Ok(hex::encode(digest.finalize()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parsed_config() -> GatewayConfig {
        GatewayConfig::try_parse_from([
            "kitsu-home-gateway",
            "--gateway-id",
            "018f47ef-48cc-7c70-84d9-166c787a3e21",
            "--server-cert",
            "server-cert.pem",
            "--server-key",
            "server-key.pem",
            "--device-ca",
            "device-ca.pem",
            "--backend-url",
            "https://api.example.test",
            "--gateway-client-identity",
            "gateway-client-identity.pem",
        ])
        .unwrap()
    }

    fn public_fixture() -> (tempfile::TempDir, GatewayConfig) {
        let temp = tempfile::tempdir().unwrap();
        let mut config = parsed_config();
        config.deployment_scope = DeploymentScope::Public;
        config.mdns = false;
        config.listen = "0.0.0.0:7443".parse().unwrap();
        config.bootstrap_listen = "0.0.0.0:7442".parse().unwrap();
        config.server_cert = temp.path().join("public-server-cert.pem");
        config.server_key = temp.path().join("public-server-key.pem");
        config.device_ca = temp.path().join("public-device-ca.pem");
        config.gateway_client_identity = temp.path().join("public-client-identity.pem");
        config.spool_dir = temp.path().join("public-spool");
        config.public_instance_manifest = Some(temp.path().join("public-instance.json"));
        fs::write(&config.server_cert, b"public server certificate\n").unwrap();
        fs::write(&config.server_key, b"public server key\n").unwrap();
        fs::write(&config.device_ca, b"public device CA\n").unwrap();
        fs::write(
            &config.gateway_client_identity,
            b"public gateway client identity\n",
        )
        .unwrap();
        fs::create_dir(&config.spool_dir).unwrap();
        write_matching_manifest(&config);
        (temp, config)
    }

    fn matching_manifest(config: &GatewayConfig) -> PublicInstanceManifest {
        PublicInstanceManifest {
            schema: PUBLIC_INSTANCE_MANIFEST_SCHEMA.to_owned(),
            scope: DeploymentScope::Public,
            gateway_id: config.gateway_id,
            canonical_spool_root: fs::canonicalize(&config.spool_dir)
                .unwrap()
                .to_string_lossy()
                .into_owned(),
            backend_origin: config.backend_url.as_str().to_owned(),
            server_certificate_sha256: sha256_file(&config.server_cert).unwrap(),
            device_ca_sha256: sha256_file(&config.device_ca).unwrap(),
            gateway_client_identity_sha256: sha256_file(&config.gateway_client_identity).unwrap(),
        }
    }

    fn write_matching_manifest(config: &GatewayConfig) {
        let bytes = serde_json::to_vec_pretty(&matching_manifest(config)).unwrap();
        fs::write(config.public_instance_manifest.as_ref().unwrap(), bytes).unwrap();
    }

    #[test]
    fn deployment_scope_defaults_to_private_with_bounded_limits() {
        let config = parsed_config();

        assert_eq!(config.deployment_scope, DeploymentScope::Private);
        assert_eq!(config.listen, "127.0.0.1:7443".parse().unwrap());
        assert_eq!(config.bootstrap_listen, "127.0.0.1:7442".parse().unwrap());
        assert_eq!(config.bootstrap_concurrency_limit, 4);
        assert_eq!(config.steady_concurrency_limit, 256);
        assert!(config.mdns);
        config.validate_runtime_policy().unwrap();
    }

    #[test]
    fn public_scope_requires_mdns_off_and_loopback_admin() {
        let mut config = parsed_config();
        config.deployment_scope = DeploymentScope::Public;

        assert_eq!(
            config.validate_runtime_policy().unwrap_err().to_string(),
            "public deployment scope requires mDNS to be disabled"
        );

        config.mdns = false;
        config.admin_listen = "0.0.0.0:7444".parse().unwrap();
        assert_eq!(
            config.validate_runtime_policy().unwrap_err().to_string(),
            "admin listener must use a loopback address"
        );

        config.admin_listen = "[::1]:7444".parse().unwrap();
        config.public_instance_manifest = Some("public-instance.json".into());
        config.validate_runtime_policy().unwrap();
    }

    #[test]
    fn private_scope_also_requires_loopback_admin() {
        let mut config = parsed_config();
        config.admin_listen = "192.0.2.10:7444".parse().unwrap();

        assert_eq!(
            config.validate_runtime_policy().unwrap_err().to_string(),
            "admin listener must use a loopback address"
        );
    }

    #[test]
    fn deployment_scope_has_a_stable_health_value() {
        assert_eq!(
            serde_json::to_string(&DeploymentScope::Private).unwrap(),
            "\"private\""
        );
        assert_eq!(
            serde_json::to_string(&DeploymentScope::Public).unwrap(),
            "\"public\""
        );
    }

    #[test]
    fn public_scope_is_an_explicit_cli_value() {
        let mut args = vec![
            "kitsu-home-gateway",
            "--gateway-id",
            "018f47ef-48cc-7c70-84d9-166c787a3e21",
            "--server-cert",
            "server-cert.pem",
            "--server-key",
            "server-key.pem",
            "--device-ca",
            "device-ca.pem",
            "--backend-url",
            "https://api.example.test",
            "--gateway-client-identity",
            "gateway-client-identity.pem",
        ];
        args.extend([
            "--deployment-scope",
            "public",
            "--mdns=false",
            "--public-instance-manifest",
            "public-instance.json",
        ]);

        let config = GatewayConfig::try_parse_from(args).unwrap();

        assert_eq!(config.deployment_scope, DeploymentScope::Public);
        assert!(!config.mdns);
        config.validate_runtime_policy().unwrap();
    }

    #[test]
    fn private_scope_accepts_only_explicit_lan_link_local_or_loopback_addresses() {
        let mut config = parsed_config();
        for ip in [
            "10.1.2.3",
            "172.16.10.20",
            "192.168.50.4",
            "127.0.0.1",
            "169.254.10.20",
            "fc00::1234",
            "fd12:3456::1",
            "fe80::1234",
            "::1",
        ] {
            let ip: IpAddr = ip.parse().unwrap();
            config.listen = SocketAddr::new(ip, 7443);
            config.bootstrap_listen = SocketAddr::new(ip, 7442);
            config.validate_runtime_policy().unwrap();
        }

        for ip in [
            "0.0.0.0",
            "::",
            "8.8.8.8",
            "192.0.2.10",
            "224.0.0.1",
            "2606:4700:4700::1111",
            "ff02::1",
        ] {
            let ip: IpAddr = ip.parse().unwrap();
            config.listen = SocketAddr::new(ip, 7443);
            config.bootstrap_listen = SocketAddr::new(ip, 7442);
            assert!(config
                .validate_runtime_policy()
                .unwrap_err()
                .to_string()
                .contains("private deployment listeners"));
        }
    }

    #[test]
    fn copied_private_configuration_cannot_become_public_by_changing_only_scope_and_mdns() {
        let mut config = parsed_config();
        config.deployment_scope = DeploymentScope::Public;
        config.mdns = false;

        assert_eq!(
            config.validate_runtime_policy().unwrap_err().to_string(),
            "public deployment scope requires a public instance manifest"
        );
    }

    #[test]
    fn matching_strict_public_instance_manifest_validates() {
        let (_temp, config) = public_fixture();
        config.validate().unwrap();
    }

    #[test]
    fn public_spool_root_must_be_preprovisioned() {
        let (_temp, config) = public_fixture();
        fs::remove_dir(&config.spool_dir).unwrap();

        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("inspect pre-provisioned public spool directory"));
    }

    #[test]
    fn public_instance_manifest_rejects_unknown_fields_and_identity_mismatches() {
        let (_temp, config) = public_fixture();
        let path = config.public_instance_manifest.as_ref().unwrap();
        let mut value = serde_json::to_value(matching_manifest(&config)).unwrap();
        value
            .as_object_mut()
            .unwrap()
            .insert("unexpected".to_owned(), serde_json::json!(true));
        fs::write(path, serde_json::to_vec(&value).unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("parse strict public instance manifest"));

        let mut manifest = matching_manifest(&config);
        manifest.gateway_id = Uuid::new_v4();
        fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("gateway ID does not match"));

        let mut manifest = matching_manifest(&config);
        manifest.backend_origin = "https://different.example.test/".to_owned();
        fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("backend origin does not match"));

        let mut manifest = matching_manifest(&config);
        manifest.server_certificate_sha256 = "0".repeat(64);
        fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("server certificate SHA-256 does not match"));
    }

    #[test]
    fn public_instance_manifest_rejects_scope_spool_and_fingerprint_shape_mismatches() {
        let (temp, config) = public_fixture();
        let path = config.public_instance_manifest.as_ref().unwrap();
        let mut manifest = matching_manifest(&config);
        manifest.scope = DeploymentScope::Private;
        fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("scope must be public"));

        let other_spool = temp.path().join("other-spool");
        fs::create_dir(&other_spool).unwrap();
        let mut manifest = matching_manifest(&config);
        manifest.canonical_spool_root = fs::canonicalize(other_spool)
            .unwrap()
            .to_string_lossy()
            .into_owned();
        fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("spool root does not match"));

        let mut manifest = matching_manifest(&config);
        manifest.device_ca_sha256 = "ABC".to_owned();
        fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("64 lowercase hex"));
    }

    #[test]
    fn public_instance_manifest_rejects_nonregular_and_oversized_files() {
        let (temp, mut config) = public_fixture();
        config.public_instance_manifest = Some(temp.path().join("manifest-directory"));
        fs::create_dir(config.public_instance_manifest.as_ref().unwrap()).unwrap();
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("must be a regular file"));

        let oversized = temp.path().join("oversized.json");
        fs::write(
            &oversized,
            vec![b' '; PUBLIC_INSTANCE_MANIFEST_MAX_BYTES as usize + 1],
        )
        .unwrap();
        config.public_instance_manifest = Some(oversized);
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("exceeds the 16384-byte limit"));
    }

    #[cfg(unix)]
    #[test]
    fn public_instance_manifest_rejects_symlinks() {
        use std::os::unix::fs::symlink;

        let (temp, mut config) = public_fixture();
        let target = config.public_instance_manifest.take().unwrap();
        let link = temp.path().join("manifest-link.json");
        symlink(target, &link).unwrap();
        config.public_instance_manifest = Some(link);
        assert!(config
            .validate()
            .unwrap_err()
            .to_string()
            .contains("must not be a symlink"));
    }

    #[test]
    fn concurrency_limits_reject_zero_and_excessive_values() {
        let mut config = parsed_config();
        config.bootstrap_concurrency_limit = 0;
        assert!(config
            .validate_runtime_policy()
            .unwrap_err()
            .to_string()
            .contains("bootstrap concurrency limit"));

        config.bootstrap_concurrency_limit = 4;
        config.steady_concurrency_limit = MAX_STEADY_CONCURRENCY_LIMIT + 1;
        assert!(config
            .validate_runtime_policy()
            .unwrap_err()
            .to_string()
            .contains("steady-device concurrency limit"));
    }
}
