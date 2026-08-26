use std::net::SocketAddr;

use axum::{
    body::Body,
    extract::{ConnectInfo, State},
    http::{header, HeaderMap, HeaderValue, Response},
    Json,
};
use serde::Deserialize;

use crate::{
    client_ip::trusted_client_ip,
    crypto::{contact_source_digest, sha256},
    error::ApiError,
    pet_packs::{crc32, downloadable_catalog_entry},
    state::AppState,
};

const VERIFICATION_SCHEMA: &str = "kitsu.code-verification.v1";
const REDEMPTION_SCHEMA: &str = "kitsu.pet-pack-redemption.v1";
const CODE_ALPHABET: &str = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PetPackRedemptionRequest {
    schema: String,
    code: String,
    verification: DeviceCodeVerification,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct DeviceCodeVerification {
    schema: String,
    request_id: String,
    status: String,
    device_id: String,
    bound_device_id: String,
    code_id: String,
    pack_id: String,
    rarity: String,
}

pub async fn redeem(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(request): Json<PetPackRedemptionRequest>,
) -> Result<Response<Body>, ApiError> {
    if headers
        .get(header::ORIGIN)
        .and_then(|value| value.to_str().ok())
        != Some("https://k32.run")
    {
        return Err(ApiError::Forbidden);
    }

    let normalized_code = validate_request(&request)?;
    let verification = &request.verification;
    let pack_id = parse_hex_u32(&verification.pack_id).ok_or(ApiError::Forbidden)?;
    let code_id = parse_hex_u32(&verification.code_id).ok_or(ApiError::Forbidden)?;
    let catalog = downloadable_catalog_entry(pack_id).ok_or(ApiError::NotFound)?;
    if verification.rarity != catalog.rarity {
        return Err(ApiError::Forbidden);
    }
    let pack = state.pet_packs.get(pack_id).ok_or(ApiError::NotFound)?;

    let client_ip = trusted_client_ip(&state.config, remote, &headers)?;
    let source_digest =
        contact_source_digest(&state.config.browser_state_key.0, &client_ip.to_string());
    let code_digest = sha256(normalized_code.as_bytes());
    state
        .db
        .check_rate_limit("pet_pack.redeem.source", &source_digest, 20, 60)
        .await?;
    state
        .db
        .check_rate_limit("pet_pack.redeem.code", &code_digest, 10, 60)
        .await?;
    state
        .db
        .bind_pet_pack_unlock(
            &code_digest,
            code_id,
            &verification.device_id,
            pack_id,
            catalog.rarity,
        )
        .await?;

    let filename = format!("kitsu-{}.k868", catalog.slug.replace('_', "-"));
    let disposition = HeaderValue::from_str(&format!("attachment; filename=\"{filename}\""))
        .map_err(ApiError::internal)?;
    let pack_id_header =
        HeaderValue::from_str(&format!("{pack_id:08X}")).map_err(ApiError::internal)?;
    let digest_header =
        HeaderValue::from_str(&hex::encode(pack.sha256)).map_err(ApiError::internal)?;
    let mut response = Response::new(Body::from(pack.bytes.as_ref().to_vec()));
    response.headers_mut().insert(
        header::CONTENT_TYPE,
        HeaderValue::from_static("application/octet-stream"),
    );
    response
        .headers_mut()
        .insert(header::CONTENT_DISPOSITION, disposition);
    response.headers_mut().insert(
        header::CACHE_CONTROL,
        HeaderValue::from_static("private, no-store, max-age=0"),
    );
    response
        .headers_mut()
        .insert(header::PRAGMA, HeaderValue::from_static("no-cache"));
    response
        .headers_mut()
        .insert("x-kitsu-pack-id", pack_id_header);
    response
        .headers_mut()
        .insert("x-kitsu-pack-sha256", digest_header);
    Ok(response)
}

fn validate_request(request: &PetPackRedemptionRequest) -> Result<String, ApiError> {
    let verification = &request.verification;
    if request.schema != REDEMPTION_SCHEMA
        || verification.schema != VERIFICATION_SCHEMA
        || verification.status != "valid"
        || !valid_request_id(&verification.request_id)
        || !valid_hardware_uid(&verification.device_id)
        || verification.device_id != verification.bound_device_id
    {
        return Err(ApiError::Forbidden);
    }
    let normalized = normalize_code(&request.code).ok_or(ApiError::Forbidden)?;
    let supplied_code_id = parse_hex_u32(&verification.code_id).ok_or(ApiError::Forbidden)?;
    let computed_code_id = crc32(normalized.as_bytes()).max(1);
    if supplied_code_id == 0 || computed_code_id != supplied_code_id {
        return Err(ApiError::Forbidden);
    }
    Ok(normalized)
}

fn normalize_code(input: &str) -> Option<String> {
    if input.is_empty() || input.len() > 80 || !input.is_ascii() {
        return None;
    }
    let raw = input
        .bytes()
        .filter(|byte| !matches!(*byte, b'-' | b' ' | b'\t' | b'\r' | b'\n'))
        .map(|byte| byte.to_ascii_uppercase())
        .collect::<Vec<_>>();
    let characters = match raw.as_slice() {
        [b'K', b'8', rest @ ..] if rest.len() == 15 => rest,
        other if other.len() == 15 => other,
        _ => return None,
    };
    if !characters
        .iter()
        .all(|byte| CODE_ALPHABET.as_bytes().contains(byte))
    {
        return None;
    }
    let characters = std::str::from_utf8(characters).ok()?;
    Some(format!(
        "K8-{}-{}-{}",
        &characters[..5],
        &characters[5..10],
        &characters[10..15]
    ))
}

fn parse_hex_u32(value: &str) -> Option<u32> {
    if value.len() != 8 || !value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return None;
    }
    u32::from_str_radix(value, 16).ok()
}

fn valid_request_id(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || matches!(byte, b'a'..=b'f'))
}

fn valid_hardware_uid(value: &str) -> bool {
    value.len() == 6
        && value.starts_with("KT")
        && value[2..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || matches!(byte, b'A'..=b'F'))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(code: &str, code_id: u32) -> PetPackRedemptionRequest {
        PetPackRedemptionRequest {
            schema: REDEMPTION_SCHEMA.to_owned(),
            code: code.to_owned(),
            verification: DeviceCodeVerification {
                schema: VERIFICATION_SCHEMA.to_owned(),
                request_id: "0123456789abcdef0123456789abcdef".to_owned(),
                status: "valid".to_owned(),
                device_id: "KT12AF".to_owned(),
                bound_device_id: "KT12AF".to_owned(),
                code_id: format!("{code_id:08X}"),
                pack_id: "5CAC86A3".to_owned(),
                rarity: "common".to_owned(),
            },
        }
    }

    #[test]
    fn accepts_only_the_device_code_crc_and_binding() {
        let normalized = "K8-01234-56789-ABCDE";
        let code_id = crc32(normalized.as_bytes());
        let accepted = request("k8 01234 56789 abcde", code_id);
        assert_eq!(validate_request(&accepted).unwrap(), normalized);

        let mut wrong_device = request(normalized, code_id);
        wrong_device.verification.bound_device_id = "KTFFFF".to_owned();
        assert!(validate_request(&wrong_device).is_err());

        let wrong_code = request(normalized, code_id ^ 1);
        assert!(validate_request(&wrong_code).is_err());
    }

    #[test]
    fn rejects_non_crockford_and_arbitrary_length_codes() {
        assert!(normalize_code("K8-OOOOO-OOOOO-OOOOO").is_none());
        assert!(normalize_code("ABCDEFGH").is_none());
        assert!(normalize_code("K8-01234-56789-ABCDE").is_some());
    }
}
