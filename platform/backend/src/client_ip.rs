use std::{
    net::{IpAddr, SocketAddr},
    str::FromStr,
};

use axum::http::HeaderMap;

use crate::{config::Config, error::ApiError};

/// Resolve a single client address from the immediately connected reverse
/// proxy. Forwarded chains are deliberately unsupported: nginx validates the
/// Cloudflare address and replaces this private header at each hop.
pub fn trusted_client_ip(
    config: &Config,
    remote: SocketAddr,
    headers: &HeaderMap,
) -> Result<IpAddr, ApiError> {
    let trusted = config
        .trusted_http_proxy_cidrs
        .iter()
        .any(|network| network.contains(&remote.ip()));
    resolve_client_ip(
        trusted,
        remote.ip(),
        headers,
        config.http_client_ip_header.as_str(),
    )
}

fn resolve_client_ip(
    trusted_proxy: bool,
    remote: IpAddr,
    headers: &HeaderMap,
    header_name: &str,
) -> Result<IpAddr, ApiError> {
    let Some(raw) = headers.get(header_name) else {
        return Ok(remote);
    };
    if !trusted_proxy {
        // An untrusted direct client cannot influence attribution.
        return Ok(remote);
    }
    let raw = raw
        .to_str()
        .map_err(|_| ApiError::Invalid("invalid client address"))?;
    if raw.trim() != raw || raw.contains(',') || raw.contains('%') || raw.len() > 45 {
        return Err(ApiError::Invalid("invalid client address"));
    }
    let parsed = IpAddr::from_str(raw).map_err(|_| ApiError::Invalid("invalid client address"))?;
    if parsed.is_unspecified() || parsed.is_multicast() {
        return Err(ApiError::Invalid("invalid client address"));
    }
    Ok(parsed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::http::HeaderValue;

    fn headers(value: &str) -> HeaderMap {
        let mut headers = HeaderMap::new();
        headers.insert("x-kitsu-client-ip", HeaderValue::from_str(value).unwrap());
        headers
    }

    #[test]
    fn untrusted_direct_caller_cannot_override_attribution() {
        let remote = "203.0.113.8".parse().unwrap();
        assert_eq!(
            resolve_client_ip(false, remote, &headers("198.51.100.9"), "x-kitsu-client-ip")
                .unwrap(),
            remote
        );
    }

    #[test]
    fn trusted_immediate_proxy_can_supply_one_valid_address() {
        assert_eq!(
            resolve_client_ip(
                true,
                "127.0.0.1".parse().unwrap(),
                &headers("2001:db8::5"),
                "x-kitsu-client-ip"
            )
            .unwrap(),
            "2001:db8::5".parse::<IpAddr>().unwrap()
        );
    }

    #[test]
    fn trusted_proxy_rejects_forwarded_chains_and_non_addresses() {
        for value in ["198.51.100.2, 203.0.113.4", "unknown", " 198.51.100.2"] {
            assert!(resolve_client_ip(
                true,
                "127.0.0.1".parse().unwrap(),
                &headers(value),
                "x-kitsu-client-ip"
            )
            .is_err());
        }
    }
}
