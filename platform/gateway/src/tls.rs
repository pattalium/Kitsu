use std::{fs::File, io::BufReader, path::Path, sync::Arc};

use anyhow::{bail, Context};
use rustls::{
    pki_types::{CertificateDer, PrivateKeyDer},
    server::WebPkiClientVerifier,
    ClientConfig, RootCertStore, ServerConfig,
};
use sha2::{Digest, Sha256};
use tokio_rustls::TlsAcceptor;
use uuid::Uuid;
use x509_parser::{extensions::GeneralName, parse_x509_certificate};

const COMPANION_SAN_PREFIX: &str = "urn:kitsu:companion:";

pub fn build_device_acceptor(
    cert_path: &Path,
    key_path: &Path,
    device_ca_path: &Path,
) -> anyhow::Result<TlsAcceptor> {
    let certs = load_certificates(cert_path)?;
    let key = load_private_key(key_path)?;
    let mut roots = RootCertStore::empty();
    for cert in load_certificates(device_ca_path)? {
        roots.add(cert).context("add device CA certificate")?;
    }
    if roots.is_empty() {
        bail!("device CA bundle is empty");
    }

    let verifier = WebPkiClientVerifier::builder(Arc::new(roots))
        .build()
        .context("build device certificate verifier")?;
    let mut config = ServerConfig::builder()
        .with_client_cert_verifier(verifier)
        .with_single_cert(certs, key)
        .context("build gateway TLS configuration")?;
    config.alpn_protocols = vec![b"kitsu-lan/1".to_vec()];
    Ok(TlsAcceptor::from(Arc::new(config)))
}

/// Enrollment uses a separate server-authenticated TLS listener because a
/// fresh device does not yet possess its client certificate. The phone
/// provisions the gateway CA/SPKI over authenticated BLE; firmware pins that
/// identity before sending the one-use claim. Steady device traffic never
/// uses this acceptor.
pub fn build_bootstrap_acceptor(cert_path: &Path, key_path: &Path) -> anyhow::Result<TlsAcceptor> {
    let certs = load_certificates(cert_path)?;
    let key = load_private_key(key_path)?;
    let mut config = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, key)
        .context("build gateway bootstrap TLS configuration")?;
    config.alpn_protocols = vec![b"kitsu-bootstrap/1".to_vec()];
    Ok(TlsAcceptor::from(Arc::new(config)))
}

pub fn build_backend_client_config(
    identity_pem_path: &Path,
    backend_ca_path: Option<&Path>,
) -> anyhow::Result<Arc<ClientConfig>> {
    let certs = load_certificates(identity_pem_path)?;
    let key = load_private_key(identity_pem_path)?;
    let mut roots = RootCertStore::empty();
    if let Some(ca_path) = backend_ca_path {
        for cert in load_certificates(ca_path)? {
            roots.add(cert).context("add backend CA certificate")?;
        }
    } else {
        roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
    }
    if roots.is_empty() {
        bail!("backend CA root store is empty");
    }
    let mut config = ClientConfig::builder()
        .with_root_certificates(roots)
        .with_client_auth_cert(certs, key)
        .context("build gateway backend mTLS configuration")?;
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(Arc::new(config))
}

pub fn certificate_sha256(cert: &CertificateDer<'_>) -> String {
    hex::encode(Sha256::digest(cert.as_ref()))
}

/// Returns the sole canonical Kitsu companion UUID bound into the verified
/// client certificate. Routing never trusts the companion ID asserted by an
/// application envelope alone.
pub fn certificate_companion_id(cert_der: &CertificateDer<'_>) -> anyhow::Result<Uuid> {
    let (remaining, certificate) = parse_x509_certificate(cert_der.as_ref())
        .map_err(|_| anyhow::anyhow!("parse device certificate DER"))?;
    if !remaining.is_empty() {
        bail!("device certificate contains trailing DER bytes");
    }
    let san = certificate
        .subject_alternative_name()
        .map_err(|_| anyhow::anyhow!("parse device certificate SAN"))?
        .context("device certificate has no subjectAltName")?;
    let mut companion = None;
    for name in &san.value.general_names {
        let GeneralName::URI(uri) = name else {
            continue;
        };
        let Some(raw_uuid) = uri.strip_prefix(COMPANION_SAN_PREFIX) else {
            continue;
        };
        let parsed = parse_canonical_uuid(raw_uuid)
            .with_context(|| format!("invalid Kitsu companion SAN URI: {uri}"))?;
        if companion.replace(parsed).is_some() {
            bail!("device certificate has multiple Kitsu companion SAN URIs");
        }
    }
    companion.context("device certificate has no Kitsu companion SAN URI")
}

fn parse_canonical_uuid(value: &str) -> anyhow::Result<Uuid> {
    if value.len() != 36 || value.bytes().any(|byte| byte.is_ascii_uppercase()) {
        bail!("companion UUID is not canonical lowercase hyphenated text");
    }
    let parsed = Uuid::parse_str(value).context("parse companion UUID")?;
    if parsed.hyphenated().to_string() != value || parsed.is_nil() {
        bail!("companion UUID is not canonical or is nil");
    }
    Ok(parsed)
}

pub fn load_certificates(path: &Path) -> anyhow::Result<Vec<CertificateDer<'static>>> {
    let file = File::open(path).with_context(|| format!("open certificate {}", path.display()))?;
    let mut reader = BufReader::new(file);
    let certs = rustls_pemfile::certs(&mut reader)
        .collect::<Result<Vec<_>, _>>()
        .with_context(|| format!("parse certificate {}", path.display()))?;
    if certs.is_empty() {
        bail!("certificate file is empty: {}", path.display());
    }
    Ok(certs)
}

fn load_private_key(path: &Path) -> anyhow::Result<PrivateKeyDer<'static>> {
    let file = File::open(path).with_context(|| format!("open private key {}", path.display()))?;
    let mut reader = BufReader::new(file);
    rustls_pemfile::private_key(&mut reader)
        .with_context(|| format!("parse private key {}", path.display()))?
        .ok_or_else(|| anyhow::anyhow!("private key file is empty: {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_only_canonical_non_nil_companion_uris() {
        let expected = Uuid::parse_str("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee").unwrap();
        assert_eq!(
            parse_canonical_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee").unwrap(),
            expected
        );
        assert!(parse_canonical_uuid("AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE").is_err());
        assert!(parse_canonical_uuid("00000000-0000-0000-0000-000000000000").is_err());
        assert!(parse_canonical_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeee").is_err());
    }
}
