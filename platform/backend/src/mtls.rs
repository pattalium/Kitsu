use std::net::SocketAddr;

use axum::http::HeaderMap;
use chrono::{DateTime, Utc};
use ipnet::IpNet;
use serde::Deserialize;
use subtle::ConstantTimeEq;
use uuid::Uuid;
use x509_parser::{
    extensions::GeneralName,
    prelude::{FromDer, X509Certificate},
};

use crate::{crypto::sha256, error::ApiError};

#[derive(Clone)]
pub struct MtlsIdentity {
    pub certificate_sha256: [u8; 32],
    pub uri_san: String,
    pub not_before: DateTime<Utc>,
    pub not_after: DateTime<Utc>,
}

pub struct MtlsHeaders<'a> {
    pub proxy_auth: &'a str,
    pub xfcc: &'a str,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct XfccEntry {
    #[serde(default)]
    by: Vec<String>,
    hash: String,
    cert: String,
    uri: Vec<String>,
}

/// Authenticate one Envoy JSON-format XFCC record produced with
/// `forward_client_cert_details: SANITIZE_SET` on a verified mTLS connection.
/// The proxy source and its independent 256-bit credential are checked before
/// any forwarded certificate material is parsed.
pub fn extract_mtls_identity(
    remote: SocketAddr,
    headers: &HeaderMap,
    trusted_proxies: &[IpNet],
    expected_proxy_auth_token: &str,
    names: MtlsHeaders<'_>,
) -> Result<MtlsIdentity, ApiError> {
    if !trusted_proxies
        .iter()
        .any(|network| network.contains(&remote.ip()))
    {
        return Err(ApiError::Forbidden);
    }
    let supplied_proxy_auth = one_header(headers, names.proxy_auth)?;
    if supplied_proxy_auth.len() > 128
        || !constant_time_equal(
            &sha256(supplied_proxy_auth.as_bytes()),
            &sha256(expected_proxy_auth_token.as_bytes()),
        )
    {
        return Err(ApiError::Forbidden);
    }

    let xfcc = one_header(headers, names.xfcc)?;
    if xfcc.is_empty() || xfcc.len() > 96 * 1024 {
        return Err(ApiError::Unauthorized);
    }
    let entries: Vec<XfccEntry> = serde_json::from_str(xfcc).map_err(|_| ApiError::Unauthorized)?;
    let [entry] = entries.as_slice() else {
        return Err(ApiError::Unauthorized);
    };
    if entry.by.len() > 4 || entry.by.iter().any(|value| value.len() > 512) {
        return Err(ApiError::Unauthorized);
    }
    if entry.hash.len() != 64
        || !entry
            .hash
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(ApiError::Unauthorized);
    }

    let mut pem_items = x509_parser::pem::Pem::iter_from_buffer(entry.cert.as_bytes());
    let pem = pem_items
        .next()
        .ok_or(ApiError::Unauthorized)?
        .map_err(|_| ApiError::Unauthorized)?;
    if pem.label != "CERTIFICATE"
        || pem.contents.is_empty()
        || pem.contents.len() > 64 * 1024
        || pem_items.next().is_some()
    {
        return Err(ApiError::Unauthorized);
    }
    let (remaining, certificate) =
        X509Certificate::from_der(&pem.contents).map_err(|_| ApiError::Unauthorized)?;
    if !remaining.is_empty() {
        return Err(ApiError::Unauthorized);
    }

    let certificate_sha256 = sha256(&pem.contents);
    let forwarded_hash = hex::decode(&entry.hash).map_err(|_| ApiError::Unauthorized)?;
    if !constant_time_equal(&certificate_sha256, &forwarded_hash) {
        return Err(ApiError::Unauthorized);
    }
    let san = certificate
        .subject_alternative_name()
        .map_err(|_| ApiError::Unauthorized)?
        .ok_or(ApiError::Unauthorized)?;
    let uri_san = match san.value.general_names.as_slice() {
        [GeneralName::URI(uri)] => *uri,
        _ => return Err(ApiError::Unauthorized),
    };
    validate_kitsu_uri_san(uri_san)?;
    if entry.uri.as_slice() != [uri_san] {
        return Err(ApiError::Unauthorized);
    }

    let not_before = DateTime::from_timestamp(certificate.validity().not_before.timestamp(), 0)
        .ok_or(ApiError::Unauthorized)?;
    let not_after = DateTime::from_timestamp(certificate.validity().not_after.timestamp(), 0)
        .ok_or(ApiError::Unauthorized)?;
    let now = Utc::now();
    if not_after <= not_before || now < not_before || now >= not_after {
        return Err(ApiError::Unauthorized);
    }
    Ok(MtlsIdentity {
        certificate_sha256,
        uri_san: uri_san.to_owned(),
        not_before,
        not_after,
    })
}

fn one_header<'a>(headers: &'a HeaderMap, name: &str) -> Result<&'a str, ApiError> {
    let mut values = headers.get_all(name).iter();
    let value = values.next().ok_or(ApiError::Unauthorized)?;
    if values.next().is_some() {
        return Err(ApiError::Unauthorized);
    }
    value.to_str().map_err(|_| ApiError::Unauthorized)
}

fn validate_kitsu_uri_san(value: &str) -> Result<(), ApiError> {
    let id_text = value
        .strip_prefix("urn:kitsu:gateway:")
        .or_else(|| value.strip_prefix("urn:kitsu:companion:"))
        .ok_or(ApiError::Unauthorized)?;
    let id = Uuid::parse_str(id_text).map_err(|_| ApiError::Unauthorized)?;
    if id.is_nil() || id.hyphenated().to_string() != id_text {
        return Err(ApiError::Unauthorized);
    }
    Ok(())
}

fn constant_time_equal(left: &[u8], right: &[u8]) -> bool {
    bool::from(left.ct_eq(right))
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::http::HeaderValue;
    use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
    use serde_json::json;
    use std::str::FromStr;

    const LEAF: &str = "MIIBzzCCAXWgAwIBAgIBAjAKBggqhkjOPQQDAjArMQ4wDAYDVQQKDAVLaXRzdTEZMBcGA1UEAwwQS2l0c3UgRml4dHVyZSBDQTAeFw0yNTAxMDEwMDAwMDBaFw0zNTAxMDEwMDAwMDBaMDsxDjAMBgNVBAoMBUtpdHN1MRAwDgYDVQQLDAdGaXh0dXJlMRcwFQYDVQQDDA5maXh0dXJlLWRldmljZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABIPT6GMP6QrBN_FsX_cIUl-4_PKfJcvD8qUl1nkM4Y-JV7KKmdZZlr9VfB0zvAZaJbfUcFmQd35nkGPelbqp5z2jejB4MAwGA1UdEwEB_wQCMAAwDgYDVR0PAQH_BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMCMEMGA1UdEQQ8MDqGOHVybjpraXRzdTpjb21wYW5pb246MTAyMTMyNDMtNTQ2NS03Njg3LTk4YTktYmFjYmRjZWRmZTBmMAoGCCqGSM49BAMCA0gAMEUCIQClQEfdBLO_rAg5zU546VqTt5e1TVsjFFs-g_U_XAaVCAIgEyMoSgPPmupb__yb7dINDxFh4OLzUOryhdiguEddJwQ";
    const URI: &str = "urn:kitsu:companion:10213243-5465-7687-98a9-bacbdcedfe0f";

    fn fixture_headers() -> HeaderMap {
        use base64::engine::general_purpose::STANDARD;

        let der = URL_SAFE_NO_PAD.decode(LEAF).unwrap();
        let pem = format!(
            "-----BEGIN CERTIFICATE-----\n{}\n-----END CERTIFICATE-----\n",
            STANDARD.encode(&der)
        );
        let xfcc = json!([{
            "hash": hex::encode(sha256(&der)),
            "cert": pem,
            "uri": [URI]
        }]);
        let mut headers = HeaderMap::new();
        headers.insert("x-proxy-auth", HeaderValue::from_static("fixture-secret"));
        headers.insert(
            "x-forwarded-client-cert",
            HeaderValue::from_str(&xfcc.to_string()).unwrap(),
        );
        headers
    }

    fn extract(remote: &str, headers: &HeaderMap) -> Result<MtlsIdentity, ApiError> {
        extract_mtls_identity(
            remote.parse().unwrap(),
            headers,
            &[IpNet::from_str("10.0.0.0/8").unwrap()],
            "fixture-secret",
            MtlsHeaders {
                proxy_auth: "x-proxy-auth",
                xfcc: "x-forwarded-client-cert",
            },
        )
    }

    #[test]
    fn rejects_spoofed_headers_from_untrusted_source() {
        assert!(matches!(
            extract("203.0.113.4:1234", &fixture_headers()),
            Err(ApiError::Forbidden)
        ));
    }

    #[test]
    fn derives_identity_from_one_sanitized_envoy_xfcc_record() {
        let headers = fixture_headers();
        let identity = extract("10.0.0.7:1234", &headers).unwrap();
        assert_eq!(identity.uri_san, URI);
        assert_eq!(
            identity.certificate_sha256,
            sha256(&URL_SAFE_NO_PAD.decode(LEAF).unwrap())
        );

        let mut forged = headers.clone();
        forged.insert("x-proxy-auth", HeaderValue::from_static("wrong-secret"));
        assert!(matches!(
            extract("10.0.0.7:1234", &forged),
            Err(ApiError::Forbidden)
        ));
    }

    #[test]
    fn rejects_xfcc_hash_or_uri_that_disagrees_with_leaf() {
        let mut headers = fixture_headers();
        let value = headers
            .get("x-forwarded-client-cert")
            .unwrap()
            .to_str()
            .unwrap();
        let mut parsed: serde_json::Value = serde_json::from_str(value).unwrap();
        parsed[0]["hash"] = serde_json::Value::String("00".repeat(32));
        headers.insert(
            "x-forwarded-client-cert",
            HeaderValue::from_str(&parsed.to_string()).unwrap(),
        );
        assert!(matches!(
            extract("10.0.0.7:1234", &headers),
            Err(ApiError::Unauthorized)
        ));

        assert!(
            validate_kitsu_uri_san("urn:kitsu:gateway:00112233-4455-6677-8899-AABBCCDDEEFF")
                .is_err()
        );
        assert!(
            validate_kitsu_uri_san("urn:kitsu:gateway:00000000-0000-0000-0000-000000000000")
                .is_err()
        );
    }
}
