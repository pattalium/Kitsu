use std::sync::Arc;
#[cfg(any(feature = "aws-private-ca", feature = "local-ca"))]
use std::time::Duration;
#[cfg(feature = "local-ca")]
use std::{
    collections::BTreeMap,
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::Mutex,
    time::Instant,
};

use async_trait::async_trait;
#[cfg(feature = "local-ca")]
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use chrono::{DateTime, Utc};
#[cfg(feature = "local-ca")]
use rcgen::{
    string::Ia5String, CertificateParams, CertificateRevocationListParams,
    CertificateSigningRequestParams, DistinguishedName, DnType, ExtendedKeyUsagePurpose, IsCa,
    Issuer, KeyIdMethod, KeyPair, KeyUsagePurpose, PublicKeyData, RevocationReason,
    RevokedCertParams, SanType, SerialNumber, PKCS_ECDSA_P256_SHA256,
};
#[cfg(feature = "local-ca")]
use serde::{Deserialize, Serialize};
#[cfg(feature = "local-ca")]
use x509_parser::{
    extensions::ParsedExtension,
    prelude::{FromDer, X509Certificate},
};
#[cfg(feature = "local-ca")]
use zeroize::Zeroizing;

use crate::{error::ApiError, pki::RawIssuedCertificate};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CertificateProfile {
    Companion,
    Gateway,
}

#[derive(Clone)]
pub struct IssueCertificateRequest {
    pub profile: CertificateProfile,
    pub csr_der: Vec<u8>,
    pub san_uri: String,
    /// Stable, non-secret, lowercase hexadecimal token. Providers must use it
    /// as their idempotency key where supported.
    pub idempotency_key: String,
}

#[derive(Clone, Debug)]
pub struct RevokedCertificate {
    pub serial_hex: String,
    pub revoked_at: DateTime<Utc>,
    pub provider_id: String,
}

#[async_trait]
pub trait CertificateIssuer: Send + Sync {
    async fn health(&self) -> Result<(), ApiError> {
        Ok(())
    }

    /// Start the provider job and return its durable opaque identifier. The
    /// caller persists this value before polling so a process restart resumes
    /// the same CA job instead of relying on a short idempotency window.
    async fn begin(&self, request: IssueCertificateRequest) -> Result<String, ApiError>;

    async fn finish(&self, provider_job_id: &str) -> Result<RawIssuedCertificate, ApiError>;

    /// Current signing CA delivered to a physical companion before its first
    /// enrollment claim. This certificate is public trust material, not a
    /// client credential.
    async fn enrollment_ca_certificate_der(&self) -> Result<Vec<u8>, ApiError>;

    /// Publish the complete revocation set. Cloud providers manage their own
    /// CRL configuration; the self-hosted provider atomically rewrites the
    /// local CRL bundle consumed by Envoy.
    async fn publish_crl(&self, _revoked: &[RevokedCertificate]) -> Result<(), ApiError> {
        Ok(())
    }
}

pub type DynCertificateIssuer = Arc<dyn CertificateIssuer>;

#[cfg(feature = "local-ca")]
struct LocalCaIdentity {
    id: String,
    issuer: Issuer<'static, KeyPair>,
    key_identifier_method: KeyIdMethod,
    chain_der: Vec<Vec<u8>>,
}

#[cfg(feature = "local-ca")]
#[derive(Default)]
struct LocalIoState {
    last_crl_digest: Option<[u8; 32]>,
    last_crl_generated: Option<Instant>,
}

#[cfg(feature = "local-ca")]
pub struct LocalCertificateIssuer {
    current_id: String,
    identities: BTreeMap<String, LocalCaIdentity>,
    certificate_validity_days: i64,
    job_dir: PathBuf,
    crl_file: PathBuf,
    io: Mutex<LocalIoState>,
}

#[cfg(feature = "local-ca")]
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct LocalIssuerJob {
    schema: u8,
    request_sha256_b64: String,
    leaf_der_b64: String,
    chain_der_b64: Vec<String>,
}

#[cfg(feature = "local-ca")]
impl LocalCertificateIssuer {
    pub fn load(
        current: &crate::config::LocalCaIdentityConfig,
        previous: &[crate::config::LocalCaIdentityConfig],
        certificate_validity_days: i64,
        job_dir: PathBuf,
        crl_file: PathBuf,
    ) -> anyhow::Result<Self> {
        ensure_directory(&job_dir)?;
        if let Some(parent) = crl_file.parent() {
            ensure_directory(parent)?;
        } else {
            anyhow::bail!("local CA CRL path must have a parent directory");
        }
        let mut identities = BTreeMap::new();
        let loaded = load_local_identity(current)?;
        identities.insert(loaded.id.clone(), loaded);
        for item in previous {
            let loaded = load_local_identity(item)?;
            if identities.insert(loaded.id.clone(), loaded).is_some() {
                anyhow::bail!("duplicate local CA identity ID");
            }
        }
        Ok(Self {
            current_id: current.id.clone(),
            identities,
            certificate_validity_days,
            job_dir,
            crl_file,
            io: Mutex::new(LocalIoState::default()),
        })
    }

    fn request_digest(request: &IssueCertificateRequest) -> [u8; 32] {
        let mut material = Vec::with_capacity(request.csr_der.len() + request.san_uri.len() + 64);
        material.extend_from_slice(b"KITSU-LOCAL-CA-REQUEST-1\0");
        material.push(match request.profile {
            CertificateProfile::Companion => 1,
            CertificateProfile::Gateway => 2,
        });
        material.extend_from_slice(&(request.csr_der.len() as u32).to_be_bytes());
        material.extend_from_slice(&request.csr_der);
        material.extend_from_slice(&(request.san_uri.len() as u32).to_be_bytes());
        material.extend_from_slice(request.san_uri.as_bytes());
        crate::crypto::sha256(&material)
    }

    fn provider_job_id(&self, idempotency_key: &str) -> String {
        format!("local-ca-v1:{}:{idempotency_key}", self.current_id)
    }

    fn job_path(&self, provider_job_id: &str) -> Result<PathBuf, ApiError> {
        let mut parts = provider_job_id.split(':');
        if parts.next() != Some("local-ca-v1")
            || parts
                .next()
                .is_none_or(|id| !self.identities.contains_key(id))
            || parts.next().is_none_or(|job| {
                job.len() < 5 || job.len() > 64 || !job.bytes().all(|byte| byte.is_ascii_hexdigit())
            })
            || parts.next().is_some()
        {
            return Err(ApiError::Invalid("invalid local certificate provider job"));
        }
        Ok(self.job_dir.join(format!(
            "{}.json",
            hex::encode(crate::crypto::sha256(provider_job_id.as_bytes()))
        )))
    }

    fn load_job(
        &self,
        provider_job_id: &str,
        expected_digest: Option<&[u8; 32]>,
    ) -> Result<Option<RawIssuedCertificate>, ApiError> {
        let path = self.job_path(provider_job_id)?;
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(error) => {
                tracing::error!(error = %error, "read local CA issuance job failed");
                return Err(ApiError::Unavailable);
            }
        };
        if bytes.len() > 256 * 1024 {
            return Err(ApiError::Unavailable);
        }
        let job: LocalIssuerJob =
            serde_json::from_slice(&bytes).map_err(|_| ApiError::Unavailable)?;
        if job.schema != 1
            || expected_digest.is_some_and(|expected| {
                URL_SAFE_NO_PAD
                    .decode(&job.request_sha256_b64)
                    .ok()
                    .as_deref()
                    != Some(expected.as_slice())
            })
        {
            return Err(ApiError::Conflict("certificate provider job changed"));
        }
        let leaf_der = decode_job_bytes(&job.leaf_der_b64)?;
        let chain_der = job
            .chain_der_b64
            .iter()
            .map(|value| decode_job_bytes(value))
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Some(RawIssuedCertificate {
            leaf_der,
            chain_der,
            provider_id: provider_job_id.to_owned(),
        }))
    }

    fn issue_and_store(
        &self,
        request: &IssueCertificateRequest,
        request_digest: &[u8; 32],
        provider_job_id: &str,
    ) -> Result<(), ApiError> {
        let identity = self
            .identities
            .get(&self.current_id)
            .ok_or(ApiError::Unavailable)?;
        let mut csr = CertificateSigningRequestParams::from_der(&request.csr_der.as_slice().into())
            .map_err(|_| ApiError::Invalid("invalid P-256 PKCS#10 CSR"))?;
        if csr.public_key.algorithm() != &PKCS_ECDSA_P256_SHA256 {
            return Err(ApiError::Invalid("CSR key must be P-256"));
        }
        let now = time::OffsetDateTime::now_utc();
        let mut params = CertificateParams::default();
        params.not_before = now - time::Duration::minutes(5);
        params.not_after = now + time::Duration::days(self.certificate_validity_days);
        let mut serial_material = Vec::with_capacity(provider_job_id.len() + request_digest.len());
        serial_material.extend_from_slice(b"KITSU-LOCAL-CA-SERIAL-1\0");
        serial_material.extend_from_slice(provider_job_id.as_bytes());
        serial_material.extend_from_slice(request_digest);
        let mut serial = crate::crypto::sha256(&serial_material);
        serial[0] &= 0x7f;
        if serial[..20].iter().all(|byte| *byte == 0) {
            serial[19] = 1;
        }
        params.serial_number = Some(SerialNumber::from_slice(&serial[..20]));
        params.subject_alt_names = vec![SanType::URI(
            Ia5String::try_from(request.san_uri.as_str())
                .map_err(|_| ApiError::Invalid("invalid certificate SAN"))?,
        )];
        let identity_text = request
            .san_uri
            .rsplit(':')
            .next()
            .ok_or(ApiError::Invalid("invalid certificate SAN"))?;
        let mut subject = DistinguishedName::new();
        subject.push(DnType::OrganizationName, "Kitsu");
        subject.push(
            DnType::OrganizationalUnitName,
            match request.profile {
                CertificateProfile::Companion => "Kitsu companion",
                CertificateProfile::Gateway => "Kitsu gateway",
            },
        );
        subject.push(DnType::CommonName, identity_text);
        params.distinguished_name = subject;
        params.is_ca = IsCa::ExplicitNoCa;
        params.key_usages = vec![KeyUsagePurpose::DigitalSignature];
        params.extended_key_usages = vec![ExtendedKeyUsagePurpose::ClientAuth];
        params.use_authority_key_identifier_extension = true;
        csr.params = params;
        let certificate = csr.signed_by(&identity.issuer).map_err(|error| {
            tracing::error!(error = %error, "local CA certificate signing failed");
            ApiError::Unavailable
        })?;
        let job = LocalIssuerJob {
            schema: 1,
            request_sha256_b64: URL_SAFE_NO_PAD.encode(request_digest),
            leaf_der_b64: URL_SAFE_NO_PAD.encode(certificate.der()),
            chain_der_b64: identity
                .chain_der
                .iter()
                .map(|certificate| URL_SAFE_NO_PAD.encode(certificate))
                .collect(),
        };
        let encoded = serde_json::to_vec(&job).map_err(ApiError::internal)?;
        atomic_write(&self.job_path(provider_job_id)?, &encoded).map_err(|error| {
            tracing::error!(error = %error, "persist local CA issuance job failed");
            ApiError::Unavailable
        })
    }

    fn publish_local_crl(&self, revoked: &[RevokedCertificate]) -> Result<(), ApiError> {
        let mut canonical = revoked.to_vec();
        canonical.sort_by(|left, right| {
            (&left.provider_id, &left.serial_hex, left.revoked_at).cmp(&(
                &right.provider_id,
                &right.serial_hex,
                right.revoked_at,
            ))
        });
        let mut digest_material = Vec::new();
        for item in &canonical {
            digest_material.extend_from_slice(item.provider_id.as_bytes());
            digest_material.push(0);
            digest_material.extend_from_slice(item.serial_hex.as_bytes());
            digest_material.push(0);
            digest_material.extend_from_slice(&item.revoked_at.timestamp().to_be_bytes());
        }
        let digest = crate::crypto::sha256(&digest_material);
        let mut state = self.io.lock().map_err(|_| ApiError::Unavailable)?;
        if self.crl_file.is_file()
            && state.last_crl_digest == Some(digest)
            && state
                .last_crl_generated
                .is_some_and(|instant| instant.elapsed() < Duration::from_secs(12 * 60 * 60))
        {
            return Ok(());
        }

        let mut grouped = BTreeMap::<String, Vec<RevokedCertParams>>::new();
        for identity in self.identities.keys() {
            grouped.insert(identity.clone(), Vec::new());
        }
        for item in canonical {
            let Some(identity_id) = local_ca_id(&item.provider_id) else {
                // AWS PCA and any future external issuer maintain their own CRLs.
                continue;
            };
            let entries = grouped.get_mut(identity_id).ok_or_else(|| {
                tracing::error!(ca_id = %identity_id, "revoked certificate belongs to an unavailable local CA identity");
                ApiError::Unavailable
            })?;
            let serial = hex::decode(&item.serial_hex).map_err(|_| ApiError::Unavailable)?;
            let revocation_time =
                time::OffsetDateTime::from_unix_timestamp(item.revoked_at.timestamp())
                    .map_err(|_| ApiError::Unavailable)?;
            entries.push(RevokedCertParams {
                serial_number: SerialNumber::from_slice(&serial),
                revocation_time,
                reason_code: Some(RevocationReason::Unspecified),
                invalidity_date: None,
            });
        }

        let now = time::OffsetDateTime::now_utc();
        let mut bundle = String::new();
        for (identity_id, entries) in grouped {
            let identity = self
                .identities
                .get(&identity_id)
                .ok_or(ApiError::Unavailable)?;
            let number = next_crl_number(&self.job_dir, &identity_id).map_err(|error| {
                tracing::error!(error = %error, "advance local CA CRL number failed");
                ApiError::Unavailable
            })?;
            let crl = CertificateRevocationListParams {
                this_update: now - time::Duration::minutes(5),
                next_update: now + time::Duration::hours(48),
                crl_number: SerialNumber::from(number),
                issuing_distribution_point: None,
                revoked_certs: entries,
                // Match the actual issuer certificate's SKI. An externally
                // generated CA commonly uses RFC 5280's legacy SHA-1 SKI;
                // inventing rcgen's SHA-256 key ID here makes OpenSSL/Envoy
                // unable to associate this otherwise-valid CRL with the CA.
                key_identifier_method: identity.key_identifier_method.clone(),
            }
            .signed_by(&identity.issuer)
            .map_err(|error| {
                tracing::error!(error = %error, "sign local CA CRL failed");
                ApiError::Unavailable
            })?;
            bundle.push_str(&crl.pem().map_err(|_| ApiError::Unavailable)?);
        }
        atomic_write(&self.crl_file, bundle.as_bytes()).map_err(|error| {
            tracing::error!(error = %error, "publish local CA CRL bundle failed");
            ApiError::Unavailable
        })?;
        state.last_crl_digest = Some(digest);
        state.last_crl_generated = Some(Instant::now());
        Ok(())
    }
}

#[cfg(feature = "local-ca")]
#[async_trait]
impl CertificateIssuer for LocalCertificateIssuer {
    async fn health(&self) -> Result<(), ApiError> {
        if !self.identities.contains_key(&self.current_id) {
            return Err(ApiError::Unavailable);
        }
        let metadata = std::fs::metadata(&self.crl_file).map_err(|_| ApiError::Unavailable)?;
        if !metadata.is_file() || metadata.len() == 0 {
            return Err(ApiError::Unavailable);
        }
        drop(self.io.lock().map_err(|_| ApiError::Unavailable)?);
        Ok(())
    }

    async fn begin(&self, request: IssueCertificateRequest) -> Result<String, ApiError> {
        validate_issue_request(&request)?;
        let provider_job_id = self.provider_job_id(&request.idempotency_key);
        let request_digest = Self::request_digest(&request);
        let _guard = self.io.lock().map_err(|_| ApiError::Unavailable)?;
        if self
            .load_job(&provider_job_id, Some(&request_digest))?
            .is_none()
        {
            self.issue_and_store(&request, &request_digest, &provider_job_id)?;
        }
        Ok(provider_job_id)
    }

    async fn finish(&self, provider_job_id: &str) -> Result<RawIssuedCertificate, ApiError> {
        self.load_job(provider_job_id, None)?
            .ok_or(ApiError::Unavailable)
    }

    async fn enrollment_ca_certificate_der(&self) -> Result<Vec<u8>, ApiError> {
        self.identities
            .get(&self.current_id)
            .and_then(|identity| identity.chain_der.first())
            .cloned()
            .ok_or(ApiError::Unavailable)
    }

    async fn publish_crl(&self, revoked: &[RevokedCertificate]) -> Result<(), ApiError> {
        self.publish_local_crl(revoked)
    }
}

#[cfg(feature = "local-ca")]
fn validate_issue_request(request: &IssueCertificateRequest) -> Result<(), ApiError> {
    if request.csr_der.is_empty()
        || request.csr_der.len() > 4_096
        || request.san_uri.len() > 255
        || request.idempotency_key.len() < 5
        || request.idempotency_key.len() > 36
        || !request
            .idempotency_key
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric())
    {
        return Err(ApiError::Invalid("invalid certificate issuance request"));
    }
    let prefix = match request.profile {
        CertificateProfile::Companion => "urn:kitsu:companion:",
        CertificateProfile::Gateway => "urn:kitsu:gateway:",
    };
    let raw_id = request
        .san_uri
        .strip_prefix(prefix)
        .ok_or(ApiError::Invalid("invalid certificate SAN"))?;
    let id =
        uuid::Uuid::parse_str(raw_id).map_err(|_| ApiError::Invalid("invalid certificate SAN"))?;
    if id.is_nil() || id.hyphenated().to_string() != raw_id {
        return Err(ApiError::Invalid("invalid certificate SAN"));
    }
    Ok(())
}

#[cfg(feature = "local-ca")]
fn load_local_identity(
    config: &crate::config::LocalCaIdentityConfig,
) -> anyhow::Result<LocalCaIdentity> {
    let key_bytes = Zeroizing::new(
        fs::read(&config.key_path)
            .map_err(|error| anyhow::anyhow!("read local CA credential: {error}"))?,
    );
    if key_bytes.len() > 16 * 1024 {
        anyhow::bail!("local CA credential is oversized");
    }
    let key_pem = std::str::from_utf8(key_bytes.as_slice())
        .map_err(|_| anyhow::anyhow!("local CA credential is not UTF-8 PEM"))?;
    let key =
        KeyPair::from_pem(key_pem).map_err(|_| anyhow::anyhow!("invalid local CA key PEM"))?;
    if key.algorithm() != &PKCS_ECDSA_P256_SHA256 {
        anyhow::bail!("local CA signing key must be P-256");
    }

    let chain_pem = fs::read_to_string(&config.chain_path)
        .map_err(|error| anyhow::anyhow!("read local CA chain: {error}"))?;
    if chain_pem.len() > 512 * 1024 {
        anyhow::bail!("local CA chain is oversized");
    }
    let chain_der = crate::pki::parse_pem_certificates(&chain_pem)
        .map_err(|_| anyhow::anyhow!("invalid local CA chain PEM"))?;
    if chain_der.is_empty() || chain_der.len() > 8 {
        anyhow::bail!("local CA chain must contain 1-8 certificates");
    }
    let mut parsed = Vec::with_capacity(chain_der.len());
    for der in &chain_der {
        let (remaining, certificate) = X509Certificate::from_der(der)
            .map_err(|_| anyhow::anyhow!("invalid local CA certificate DER"))?;
        if !remaining.is_empty() {
            anyhow::bail!("local CA certificate contains trailing DER bytes");
        }
        parsed.push(certificate);
    }
    let signing_ca = &parsed[0];
    let basic = signing_ca
        .basic_constraints()
        .map_err(|_| anyhow::anyhow!("invalid local CA basic constraints"))?
        .ok_or_else(|| anyhow::anyhow!("local CA certificate lacks basic constraints"))?;
    let usage = signing_ca
        .key_usage()
        .map_err(|_| anyhow::anyhow!("invalid local CA key usage"))?
        .ok_or_else(|| anyhow::anyhow!("local CA certificate lacks key usage"))?;
    if !basic.value.ca || !usage.value.key_cert_sign() || !usage.value.crl_sign() {
        anyhow::bail!("local CA certificate must permit certificate and CRL signing");
    }
    if signing_ca.tbs_certificate.subject_pki.raw != key.subject_public_key_info() {
        anyhow::bail!("local CA certificate does not match the signing key");
    }
    let key_identifier_method = signing_ca
        .iter_extensions()
        .find_map(|extension| match extension.parsed_extension() {
            ParsedExtension::SubjectKeyIdentifier(identifier) => {
                Some(KeyIdMethod::PreSpecified(identifier.0.to_vec()))
            }
            _ => None,
        })
        .unwrap_or(KeyIdMethod::Sha256);
    for pair in parsed.windows(2) {
        pair[0]
            .verify_signature(Some(pair[1].public_key()))
            .map_err(|_| anyhow::anyhow!("local CA chain signature verification failed"))?;
    }
    let issuer = Issuer::from_ca_cert_der(&chain_der[0].as_slice().into(), key)
        .map_err(|_| anyhow::anyhow!("local CA issuer construction failed"))?;
    Ok(LocalCaIdentity {
        id: config.id.clone(),
        issuer,
        key_identifier_method,
        chain_der,
    })
}

#[cfg(feature = "local-ca")]
fn ensure_directory(path: &Path) -> anyhow::Result<()> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            anyhow::bail!(
                "local CA state path must be a real directory: {}",
                path.display()
            )
        }
        Ok(_) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir_all(path)?;
            let metadata = fs::symlink_metadata(path)?;
            if metadata.file_type().is_symlink() || !metadata.is_dir() {
                anyhow::bail!(
                    "local CA state path must be a real directory: {}",
                    path.display()
                );
            }
            Ok(())
        }
        Err(error) => Err(error.into()),
    }
}

#[cfg(feature = "local-ca")]
fn atomic_write(path: &Path, value: &[u8]) -> std::io::Result<()> {
    let parent = path.parent().ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::InvalidInput, "path has no parent")
    })?;
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or_else(|| {
            std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "path has no UTF-8 filename",
            )
        })?;
    let temporary = parent.join(format!(".{file_name}.{}.tmp", uuid::Uuid::new_v4()));
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            file.set_permissions(fs::Permissions::from_mode(0o640))?;
        }
        file.write_all(value)?;
        file.sync_all()?;
        fs::rename(&temporary, path)?;
        #[cfg(unix)]
        OpenOptions::new().read(true).open(parent)?.sync_all()?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

#[cfg(feature = "local-ca")]
fn decode_job_bytes(value: &str) -> Result<Vec<u8>, ApiError> {
    if value.is_empty() || value.len() > 128 * 1024 {
        return Err(ApiError::Unavailable);
    }
    let decoded = URL_SAFE_NO_PAD
        .decode(value.as_bytes())
        .map_err(|_| ApiError::Unavailable)?;
    if URL_SAFE_NO_PAD.encode(&decoded) != value {
        return Err(ApiError::Unavailable);
    }
    Ok(decoded)
}

#[cfg(feature = "local-ca")]
fn local_ca_id(provider_job_id: &str) -> Option<&str> {
    let mut parts = provider_job_id.split(':');
    (parts.next()? == "local-ca-v1").then_some(())?;
    let identity = parts.next()?;
    parts.next()?;
    parts.next().is_none().then_some(identity)
}

#[cfg(feature = "local-ca")]
fn next_crl_number(job_dir: &Path, identity_id: &str) -> std::io::Result<u64> {
    let path = job_dir.join(format!("crl-{identity_id}.number"));
    let previous = match fs::read_to_string(&path) {
        Ok(value) => value.trim().parse::<u64>().map_err(|_| {
            std::io::Error::new(std::io::ErrorKind::InvalidData, "invalid CRL number")
        })?,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => 0,
        Err(error) => return Err(error),
    };
    let next = previous.checked_add(1).ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::InvalidData, "CRL number exhausted")
    })?;
    atomic_write(&path, next.to_string().as_bytes())?;
    Ok(next)
}

#[cfg(feature = "aws-private-ca")]
pub struct AwsPrivateCaIssuer {
    client: aws_sdk_acmpca::Client,
    certificate_authority_arn: String,
    api_passthrough_template_arn: String,
    certificate_validity_days: i64,
    issue_timeout: Duration,
}

#[cfg(feature = "aws-private-ca")]
impl AwsPrivateCaIssuer {
    pub async fn new(
        certificate_authority_arn: String,
        api_passthrough_template_arn: String,
        certificate_validity_days: i64,
        issue_timeout: Duration,
    ) -> Self {
        let config = aws_config::load_defaults(aws_config::BehaviorVersion::latest()).await;
        Self {
            client: aws_sdk_acmpca::Client::new(&config),
            certificate_authority_arn,
            api_passthrough_template_arn,
            certificate_validity_days,
            issue_timeout,
        }
    }
}

#[cfg(feature = "aws-private-ca")]
#[async_trait]
impl CertificateIssuer for AwsPrivateCaIssuer {
    async fn health(&self) -> Result<(), ApiError> {
        self.client
            .describe_certificate_authority()
            .certificate_authority_arn(&self.certificate_authority_arn)
            .send()
            .await
            .map(|_| ())
            .map_err(|error| {
                tracing::error!(error = %error, "private CA readiness check failed");
                ApiError::Unavailable
            })
    }

    async fn begin(&self, request: IssueCertificateRequest) -> Result<String, ApiError> {
        use aws_sdk_acmpca::{
            primitives::Blob,
            types::{
                ApiPassthrough, Asn1Subject, ExtendedKeyUsage, ExtendedKeyUsageType, Extensions,
                GeneralName, KeyUsage, SigningAlgorithm, Validity, ValidityPeriodType,
            },
        };

        if request.csr_der.is_empty()
            || request.csr_der.len() > 4_096
            || request.san_uri.len() > 255
            || request.idempotency_key.len() < 5
            || request.idempotency_key.len() > 36
            || !request
                .idempotency_key
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric())
        {
            return Err(ApiError::Invalid("invalid certificate issuance request"));
        }
        let identity = request
            .san_uri
            .rsplit(':')
            .next()
            .ok_or(ApiError::Invalid("invalid certificate SAN"))?;
        let subject_kind = match request.profile {
            CertificateProfile::Companion => "Kitsu companion",
            CertificateProfile::Gateway => "Kitsu gateway",
        };
        let extensions = Extensions::builder()
            .subject_alternative_names(
                GeneralName::builder()
                    .uniform_resource_identifier(&request.san_uri)
                    .build(),
            )
            .key_usage(KeyUsage::builder().digital_signature(true).build())
            .extended_key_usage(
                ExtendedKeyUsage::builder()
                    .extended_key_usage_type(ExtendedKeyUsageType::ClientAuth)
                    .build(),
            )
            .build();
        let passthrough = ApiPassthrough::builder()
            .subject(
                Asn1Subject::builder()
                    .organization("Kitsu")
                    .organizational_unit(subject_kind)
                    .common_name(identity)
                    .build(),
            )
            .extensions(extensions)
            .build();
        let validity = Validity::builder()
            .value(self.certificate_validity_days)
            .r#type(ValidityPeriodType::Days)
            .build()
            .map_err(ApiError::internal)?;
        let issued = self
            .client
            .issue_certificate()
            .certificate_authority_arn(&self.certificate_authority_arn)
            .csr(Blob::new(request.csr_der))
            .signing_algorithm(SigningAlgorithm::Sha256Withecdsa)
            .template_arn(&self.api_passthrough_template_arn)
            .api_passthrough(passthrough)
            .validity(validity)
            .idempotency_token(request.idempotency_key)
            .send()
            .await
            .map_err(|error| {
                tracing::error!(error = %error, "AWS Private CA IssueCertificate failed");
                ApiError::Unavailable
            })?;
        Ok(issued
            .certificate_arn()
            .ok_or(ApiError::Unavailable)?
            .to_owned())
    }

    async fn finish(&self, provider_job_id: &str) -> Result<RawIssuedCertificate, ApiError> {
        if provider_job_id.is_empty()
            || provider_job_id.len() > 2_048
            || !provider_job_id.starts_with("arn:")
        {
            return Err(ApiError::Invalid("invalid certificate provider job"));
        }

        let deadline = tokio::time::Instant::now() + self.issue_timeout;
        loop {
            match self
                .client
                .get_certificate()
                .certificate_authority_arn(&self.certificate_authority_arn)
                .certificate_arn(provider_job_id)
                .send()
                .await
            {
                Ok(output) => {
                    let leaf_pem = output.certificate().ok_or(ApiError::Unavailable)?;
                    let chain_pem = output.certificate_chain().ok_or(ApiError::Unavailable)?;
                    let mut leaf = crate::pki::parse_pem_certificates(leaf_pem)?;
                    if leaf.len() != 1 {
                        return Err(ApiError::Unavailable);
                    }
                    let chain = crate::pki::parse_pem_certificates(chain_pem)?;
                    return Ok(RawIssuedCertificate {
                        leaf_der: leaf.remove(0),
                        chain_der: chain,
                        provider_id: provider_job_id.to_owned(),
                    });
                }
                Err(error) if tokio::time::Instant::now() < deadline => {
                    // AWS Private CA reports RequestInProgress while signing.
                    // Other service failures are retried only inside the tight,
                    // configured issuance deadline; no request material is logged.
                    tracing::warn!(error = %error, "AWS Private CA certificate not ready");
                    tokio::time::sleep(Duration::from_secs(1)).await;
                }
                Err(error) => {
                    tracing::error!(error = %error, "AWS Private CA GetCertificate failed");
                    return Err(ApiError::Unavailable);
                }
            }
        }
    }

    async fn enrollment_ca_certificate_der(&self) -> Result<Vec<u8>, ApiError> {
        let output = self
            .client
            .get_certificate_authority_certificate()
            .certificate_authority_arn(&self.certificate_authority_arn)
            .send()
            .await
            .map_err(|error| {
                tracing::error!(error = %error, "AWS Private CA certificate lookup failed");
                ApiError::Unavailable
            })?;
        let mut certificates =
            crate::pki::parse_pem_certificates(output.certificate().ok_or(ApiError::Unavailable)?)?;
        if certificates.len() != 1 {
            return Err(ApiError::Unavailable);
        }
        Ok(certificates.remove(0))
    }
}

/// Deterministic provider used by protocol and database integration tests.  It
/// returns one immutable, pre-issued fixture only when every request field is
/// byte-for-byte identical to the configured expectation.
#[cfg(any(test, feature = "test-ca"))]
pub struct DeterministicTestIssuer {
    expected_profile: CertificateProfile,
    expected_csr_sha256: [u8; 32],
    expected_san_uri: String,
    issued: RawIssuedCertificate,
}

#[cfg(any(test, feature = "test-ca"))]
impl DeterministicTestIssuer {
    pub fn new(
        expected_profile: CertificateProfile,
        expected_csr_sha256: [u8; 32],
        expected_san_uri: String,
        issued: RawIssuedCertificate,
    ) -> Self {
        Self {
            expected_profile,
            expected_csr_sha256,
            expected_san_uri,
            issued,
        }
    }
}

#[cfg(any(test, feature = "test-ca"))]
#[async_trait]
impl CertificateIssuer for DeterministicTestIssuer {
    async fn begin(&self, request: IssueCertificateRequest) -> Result<String, ApiError> {
        if request.profile != self.expected_profile
            || crate::crypto::sha256(&request.csr_der) != self.expected_csr_sha256
            || request.san_uri != self.expected_san_uri
            || request.idempotency_key.is_empty()
        {
            return Err(ApiError::Invalid("unexpected deterministic CA request"));
        }
        Ok(self.issued.provider_id.clone())
    }

    async fn finish(&self, provider_job_id: &str) -> Result<RawIssuedCertificate, ApiError> {
        if provider_job_id != self.issued.provider_id {
            return Err(ApiError::Invalid("unexpected deterministic CA job"));
        }
        Ok(self.issued.clone())
    }

    async fn enrollment_ca_certificate_der(&self) -> Result<Vec<u8>, ApiError> {
        self.issued
            .chain_der
            .first()
            .cloned()
            .ok_or(ApiError::Unavailable)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn deterministic_provider_resumes_the_same_immutable_job() {
        let csr = b"fixture-csr".to_vec();
        let san = "urn:kitsu:gateway:00112233-4455-6677-8899-aabbccddeeff";
        let issued = RawIssuedCertificate {
            leaf_der: vec![1, 2, 3],
            chain_der: vec![vec![4, 5, 6]],
            provider_id: "fixture-ca:job-1".to_owned(),
        };
        let provider = DeterministicTestIssuer::new(
            CertificateProfile::Gateway,
            crate::crypto::sha256(&csr),
            san.to_owned(),
            issued.clone(),
        );
        let job = provider
            .begin(IssueCertificateRequest {
                profile: CertificateProfile::Gateway,
                csr_der: csr,
                san_uri: san.to_owned(),
                idempotency_key: "0011223344556677".to_owned(),
            })
            .await
            .unwrap();
        assert_eq!(job, issued.provider_id);
        let first = provider.finish(&job).await.unwrap();
        let retry = provider.finish(&job).await.unwrap();
        assert_eq!(first.leaf_der, retry.leaf_der);
        assert_eq!(first.chain_der, retry.chain_der);
        assert!(provider.finish("fixture-ca:other-job").await.is_err());
    }

    #[cfg(feature = "local-ca")]
    #[tokio::test]
    async fn local_provider_persists_exact_p256_jobs_and_publishes_a_crl() {
        use rcgen::BasicConstraints;
        use x509_parser::{pem::parse_x509_pem, revocation_list::CertificateRevocationList};

        let directory = tempfile::tempdir().unwrap();
        let key_path = directory.path().join("ca-key.pem");
        let chain_path = directory.path().join("ca-chain.pem");
        let job_dir = directory.path().join("jobs");
        let crl_path = directory.path().join("gateway-client-crl.pem");

        let ca_key = KeyPair::generate_for(&PKCS_ECDSA_P256_SHA256).unwrap();
        let mut ca_params = CertificateParams::default();
        ca_params
            .distinguished_name
            .push(DnType::OrganizationName, "Kitsu test CA");
        ca_params.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);
        ca_params.key_usages = vec![
            KeyUsagePurpose::DigitalSignature,
            KeyUsagePurpose::KeyCertSign,
            KeyUsagePurpose::CrlSign,
        ];
        let expected_key_identifier = vec![0x5a; 20];
        ca_params.key_identifier_method =
            KeyIdMethod::PreSpecified(expected_key_identifier.clone());
        let ca_certificate = ca_params.self_signed(&ca_key).unwrap();
        fs::write(&key_path, ca_key.serialize_pem()).unwrap();
        fs::write(&chain_path, ca_certificate.pem()).unwrap();

        let identity = crate::config::LocalCaIdentityConfig {
            id: "test-v1".to_owned(),
            key_path,
            chain_path,
        };
        let issuer =
            LocalCertificateIssuer::load(&identity, &[], 30, job_dir.clone(), crl_path.clone())
                .unwrap();
        let device_key = KeyPair::generate_for(&PKCS_ECDSA_P256_SHA256).unwrap();
        let csr = CertificateParams::default()
            .serialize_request(&device_key)
            .unwrap();
        let companion_id = uuid::Uuid::new_v4();
        let request = IssueCertificateRequest {
            profile: CertificateProfile::Companion,
            csr_der: csr.der().to_vec(),
            san_uri: format!("urn:kitsu:companion:{companion_id}"),
            idempotency_key: "00112233445566778899aabbccddeeff".to_owned(),
        };
        let job = issuer.begin(request.clone()).await.unwrap();
        assert_eq!(issuer.begin(request).await.unwrap(), job);
        assert_eq!(
            fs::read_dir(&job_dir)
                .unwrap()
                .filter_map(Result::ok)
                .filter(|item| item.path().extension().is_some_and(|value| value == "json"))
                .count(),
            1
        );
        let issued = issuer.finish(&job).await.unwrap();
        let validated_csr = crate::pki::validate_p256_csr(csr.der()).unwrap();
        crate::pki::validate_issued_certificate(
            issued,
            &validated_csr,
            &format!("urn:kitsu:companion:{companion_id}"),
            Utc::now(),
        )
        .unwrap();

        issuer.publish_crl(&[]).await.unwrap();
        let crl = fs::read_to_string(crl_path).unwrap();
        assert!(crl.starts_with("-----BEGIN X509 CRL-----"));
        assert!(!crl.contains("PRIVATE KEY"));
        let (_, crl_pem) = parse_x509_pem(crl.as_bytes()).unwrap();
        let (_, parsed_crl) = CertificateRevocationList::from_der(&crl_pem.contents).unwrap();
        let actual_key_identifier = parsed_crl
            .extensions()
            .iter()
            .find_map(|extension| match extension.parsed_extension() {
                ParsedExtension::AuthorityKeyIdentifier(identifier) => identifier
                    .key_identifier
                    .as_ref()
                    .map(|value| value.0.to_vec()),
                _ => None,
            })
            .unwrap();
        assert_eq!(actual_key_identifier, expected_key_identifier);
    }
}
