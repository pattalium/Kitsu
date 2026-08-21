use std::{
    fs::{self, OpenOptions},
    io::Write,
    net::IpAddr,
    path::{Path, PathBuf},
    time::Duration,
};

use anyhow::{bail, Context, Result};
use base64::{
    engine::general_purpose::{STANDARD, URL_SAFE_NO_PAD},
    Engine,
};
use clap::Parser;
use rcgen::{CertificateParams, KeyPair, PublicKeyData, PKCS_ECDSA_P256_SHA256};
use reqwest::{Certificate, Client, Url};
use serde::{Deserialize, Serialize};
use uuid::Uuid;
use x509_parser::{extensions::GeneralName, parse_x509_certificate};
use zeroize::{Zeroize, Zeroizing};

#[derive(Debug, Parser)]
#[command(about = "Claim a one-use Kitsu gateway bootstrap without exposing its private key")]
struct Args {
    /// Backend/API origin. Plain HTTP is accepted only for a loopback host.
    #[arg(long, env = "KITSU_BOOTSTRAP_BACKEND_URL")]
    backend_url: Url,

    /// Canonical UUID returned by POST /v1/gateway-bootstraps.
    #[arg(long, env = "KITSU_BOOTSTRAP_ID")]
    bootstrap_id: Uuid,

    /// Root-readable file containing only the 32-byte unpadded-base64url claim.
    #[arg(long, env = "KITSU_BOOTSTRAP_CLAIM_TOKEN_FILE")]
    claim_token_file: PathBuf,

    /// Directory that will receive the identity, CA bundle, and gateway ID.
    #[arg(long, env = "KITSU_BOOTSTRAP_OUTPUT_DIR")]
    output_dir: PathBuf,

    /// Optional PEM CA bundle for a private HTTPS backend origin.
    #[arg(long, env = "KITSU_BOOTSTRAP_SERVER_CA")]
    server_ca: Option<PathBuf>,

    /// Optional HTTP Host header for a loopback nginx origin.
    #[arg(long, env = "KITSU_BOOTSTRAP_HOST_HEADER")]
    host_header: Option<String>,
}

#[derive(Serialize)]
struct ClaimRequest<'a> {
    claim_token: &'a str,
    gateway_csr_der_b64: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ClaimResponse {
    gateway_id: Uuid,
    device_certificate_der_b64: String,
    device_certificate_chain_der_b64: Vec<String>,
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();
    validate_args(&args)?;
    ensure_output_directory(&args.output_dir)?;
    let identity_path = args.output_dir.join("gateway-client-identity.pem");
    if identity_path.exists() {
        bail!(
            "refusing to replace an existing gateway identity: {}",
            identity_path.display()
        );
    }

    let pending_key_path = args.output_dir.join(".bootstrap-pending-key.pem");
    let key = load_or_create_pending_key(&pending_key_path)?;
    let csr = CertificateParams::default()
        .serialize_request(&key)
        .context("create P-256 gateway CSR")?;
    let mut claim_token = read_claim_token(&args.claim_token_file)?;
    let endpoint = args
        .backend_url
        .join(&format!(
            "/v1/gateway-bootstraps/{}/claim",
            args.bootstrap_id
        ))
        .context("build gateway bootstrap endpoint")?;
    let client = client(&args)?;
    let mut request = client.post(endpoint).json(&ClaimRequest {
        claim_token: claim_token.as_str(),
        gateway_csr_der_b64: URL_SAFE_NO_PAD.encode(csr.der()),
    });
    if let Some(host) = &args.host_header {
        request = request.header(reqwest::header::HOST, host);
    }
    let response = request
        .send()
        .await
        .context("submit gateway bootstrap claim")?;
    claim_token.zeroize();
    if !response.status().is_success() {
        bail!(
            "gateway bootstrap was rejected with HTTP {}",
            response.status()
        );
    }
    if response
        .content_length()
        .is_some_and(|length| length > 64 * 1024)
    {
        bail!("gateway bootstrap response is oversized");
    }
    let bytes = response
        .bytes()
        .await
        .context("read gateway bootstrap response")?;
    if bytes.len() > 64 * 1024 {
        bail!("gateway bootstrap response is oversized");
    }
    let response: ClaimResponse =
        serde_json::from_slice(&bytes).context("parse gateway bootstrap response")?;
    let leaf = decode_canonical(&response.device_certificate_der_b64, "gateway certificate")?;
    let chain = response
        .device_certificate_chain_der_b64
        .iter()
        .map(|value| decode_canonical(value, "gateway certificate chain"))
        .collect::<Result<Vec<_>>>()?;
    validate_response(response.gateway_id, &leaf, &chain, &key)?;

    let mut identity = Zeroizing::new(key.serialize_pem().into_bytes());
    identity.extend_from_slice(pem_block("CERTIFICATE", &leaf).as_bytes());
    for certificate in &chain {
        identity.extend_from_slice(pem_block("CERTIFICATE", certificate).as_bytes());
    }
    atomic_write(&identity_path, identity.as_slice(), 0o600)?;
    identity.zeroize();
    let ca_path = args.output_dir.join("gateway-client-ca.pem");
    let ca_bundle = chain
        .iter()
        .map(|certificate| pem_block("CERTIFICATE", certificate))
        .collect::<String>();
    atomic_write(&ca_path, ca_bundle.as_bytes(), 0o640)?;
    let gateway_id_path = args.output_dir.join("gateway-id");
    atomic_write(
        &gateway_id_path,
        format!("{}\n", response.gateway_id).as_bytes(),
        0o640,
    )?;
    fs::remove_file(&pending_key_path).context("remove consumed pending gateway key")?;

    println!("gateway bootstrap completed: {}", response.gateway_id);
    println!("identity: {}", identity_path.display());
    println!("client CA: {}", ca_path.display());
    Ok(())
}

fn validate_args(args: &Args) -> Result<()> {
    if args.bootstrap_id.is_nil() {
        bail!("bootstrap ID cannot be nil");
    }
    if args.backend_url.query().is_some() || args.backend_url.fragment().is_some() {
        bail!("backend URL cannot contain a query or fragment");
    }
    match args.backend_url.scheme() {
        "https" => {}
        "http"
            if args
                .backend_url
                .host_str()
                .and_then(|host| host.parse::<IpAddr>().ok())
                .is_some_and(|address| address.is_loopback())
                || args.backend_url.host_str() == Some("localhost") => {}
        _ => bail!("backend URL must use HTTPS, or HTTP on a loopback host"),
    }
    if let Some(host) = &args.host_header {
        reqwest::header::HeaderValue::from_str(host).context("invalid bootstrap Host header")?;
        if host.chars().any(char::is_whitespace) {
            bail!("invalid bootstrap Host header");
        }
    }
    Ok(())
}

fn client(args: &Args) -> Result<Client> {
    let mut builder = Client::builder()
        .no_proxy()
        .https_only(args.backend_url.scheme() == "https")
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_secs(210))
        .user_agent("kitsu-gateway-bootstrap/0.1");
    if let Some(path) = &args.server_ca {
        let bytes = fs::read(path).context("read bootstrap server CA")?;
        if bytes.len() > 512 * 1024 {
            bail!("bootstrap server CA bundle is oversized");
        }
        builder = builder.add_root_certificate(
            Certificate::from_pem(&bytes).context("parse bootstrap server CA")?,
        );
    }
    builder.build().context("build bootstrap HTTP client")
}

fn ensure_output_directory(path: &Path) -> Result<()> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            bail!("bootstrap output path must be a real directory")
        }
        Ok(_) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir_all(path).context("create bootstrap output directory")?;
            Ok(())
        }
        Err(error) => Err(error).context("inspect bootstrap output directory"),
    }
}

fn load_or_create_pending_key(path: &Path) -> Result<KeyPair> {
    match fs::read(path) {
        Ok(bytes) => {
            if bytes.len() > 16 * 1024 {
                bail!("pending gateway key is oversized");
            }
            let text = std::str::from_utf8(&bytes).context("pending gateway key is not PEM")?;
            let key = KeyPair::from_pem(text).context("parse pending gateway key")?;
            if key.algorithm() != &PKCS_ECDSA_P256_SHA256 {
                bail!("pending gateway key is not P-256");
            }
            Ok(key)
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            let key = KeyPair::generate_for(&PKCS_ECDSA_P256_SHA256)
                .context("generate P-256 gateway key")?;
            let mut pem = Zeroizing::new(key.serialize_pem().into_bytes());
            atomic_write(path, pem.as_slice(), 0o600)?;
            pem.zeroize();
            Ok(key)
        }
        Err(error) => Err(error).context("read pending gateway key"),
    }
}

fn read_claim_token(path: &Path) -> Result<Zeroizing<String>> {
    let metadata = fs::symlink_metadata(path).context("inspect gateway claim token")?;
    if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > 128 {
        bail!("gateway claim token must be a small regular file");
    }
    let token = fs::read_to_string(path).context("read gateway claim token")?;
    let token = token.trim();
    let decoded = URL_SAFE_NO_PAD
        .decode(token.as_bytes())
        .context("gateway claim token is not base64url")?;
    if decoded.len() != 32 || URL_SAFE_NO_PAD.encode(decoded) != token {
        bail!("gateway claim token must be canonical base64url for 32 bytes");
    }
    Ok(Zeroizing::new(token.to_owned()))
}

fn decode_canonical(value: &str, label: &str) -> Result<Vec<u8>> {
    if value.is_empty() || value.len() > 128 * 1024 {
        bail!("invalid {label}");
    }
    let decoded = URL_SAFE_NO_PAD
        .decode(value.as_bytes())
        .with_context(|| format!("invalid {label}"))?;
    if URL_SAFE_NO_PAD.encode(&decoded) != value {
        bail!("invalid {label}");
    }
    Ok(decoded)
}

fn validate_response(
    gateway_id: Uuid,
    leaf: &[u8],
    chain: &[Vec<u8>],
    key: &KeyPair,
) -> Result<()> {
    if gateway_id.is_nil()
        || leaf.is_empty()
        || leaf.len() > 64 * 1024
        || chain.is_empty()
        || chain.len() > 8
        || chain
            .iter()
            .any(|certificate| certificate.is_empty() || certificate.len() > 64 * 1024)
    {
        bail!("invalid gateway bootstrap certificate response");
    }
    let (remaining, certificate) =
        parse_x509_certificate(leaf).map_err(|_| anyhow::anyhow!("invalid gateway certificate"))?;
    if !remaining.is_empty()
        || certificate.tbs_certificate.subject_pki.raw != key.subject_public_key_info()
    {
        bail!("gateway certificate does not match the retained private key");
    }
    let expected_san = format!("urn:kitsu:gateway:{gateway_id}");
    let san = certificate
        .subject_alternative_name()
        .context("parse gateway certificate SAN")?
        .context("gateway certificate has no SAN")?;
    if san.value.general_names.as_slice() != [GeneralName::URI(&expected_san)] {
        bail!("gateway certificate has the wrong SAN");
    }
    let basic = certificate
        .basic_constraints()
        .context("parse gateway certificate constraints")?
        .context("gateway certificate lacks basic constraints")?;
    let usage = certificate
        .key_usage()
        .context("parse gateway certificate key usage")?
        .context("gateway certificate lacks key usage")?;
    let extended = certificate
        .extended_key_usage()
        .context("parse gateway certificate extended usage")?
        .context("gateway certificate lacks extended usage")?;
    if basic.value.ca
        || !usage.value.digital_signature()
        || usage.value.key_cert_sign()
        || usage.value.crl_sign()
        || !extended.value.client_auth
        || extended.value.any
        || extended.value.server_auth
    {
        bail!("gateway certificate has an invalid client-auth profile");
    }
    let parsed_chain = chain
        .iter()
        .map(|item| {
            let (remaining, certificate) = parse_x509_certificate(item)
                .map_err(|_| anyhow::anyhow!("invalid gateway certificate chain"))?;
            if !remaining.is_empty() {
                bail!("gateway certificate chain contains trailing data");
            }
            Ok(certificate)
        })
        .collect::<Result<Vec<_>>>()?;
    certificate
        .verify_signature(Some(parsed_chain[0].public_key()))
        .context("gateway certificate signature is invalid")?;
    for pair in parsed_chain.windows(2) {
        pair[0]
            .verify_signature(Some(pair[1].public_key()))
            .context("gateway certificate chain signature is invalid")?;
    }
    Ok(())
}

fn pem_block(label: &str, der: &[u8]) -> String {
    let encoded = STANDARD.encode(der);
    let mut output = format!("-----BEGIN {label}-----\n");
    for chunk in encoded.as_bytes().chunks(64) {
        output.push_str(std::str::from_utf8(chunk).expect("base64 is ASCII"));
        output.push('\n');
    }
    output.push_str(&format!("-----END {label}-----\n"));
    output
}

fn atomic_write(path: &Path, value: &[u8], unix_mode: u32) -> Result<()> {
    let parent = path.parent().context("output path has no parent")?;
    let name = path
        .file_name()
        .and_then(|name| name.to_str())
        .context("output filename is invalid")?;
    let temporary = parent.join(format!(".{name}.{}.tmp", Uuid::new_v4()));
    let result = (|| -> Result<()> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .context("create temporary bootstrap output")?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            file.set_permissions(fs::Permissions::from_mode(unix_mode))
                .context("set bootstrap output permissions")?;
        }
        #[cfg(not(unix))]
        let _ = unix_mode;
        file.write_all(value).context("write bootstrap output")?;
        file.sync_all().context("sync bootstrap output")?;
        fs::rename(&temporary, path).context("publish bootstrap output")?;
        #[cfg(unix)]
        OpenOptions::new()
            .read(true)
            .open(parent)
            .context("open bootstrap output directory")?
            .sync_all()
            .context("sync bootstrap output directory")?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}
