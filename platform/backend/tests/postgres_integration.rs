//! PostgreSQL 16 integration invariants.
//!
//! The test is intentionally gated by `DATABASE_URL`.  CI supplies an isolated
//! PostgreSQL 16 service; a normal offline/unit-test run skips without touching
//! any database.  Each run creates a random schema and sets `search_path`, so it
//! never mutates application tables in another schema.

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use chrono::{TimeDelta, Utc};
use kitsu_platform_backend::{
    crypto::{device_transcript, sha256, sign_remote_action_with_nonce, EncryptedBytes},
    db::{
        BeginEnrollmentIssuance, BeginGatewayBootstrap, Database, Gateway, IngestOutcome,
        NewSecretRecord,
    },
    error::ApiError,
    mtls::MtlsIdentity,
    oidc::OidcPrincipal,
    persistence::{query, query_scalar, Row},
    pki::{SealedSecret, ValidatedCertificate},
    wire::{CreateActionRequest, DeviceEnvelope, DEVICE_ENVELOPE_SCHEMA},
};
use serde_json::{json, Map, Value};
use sqlx_postgres::PgPoolOptions;
use uuid::Uuid;
use zeroize::Zeroizing;

#[tokio::test]
async fn postgres16_durability_and_concurrency_invariants() {
    let Ok(database_url) = std::env::var("DATABASE_URL") else {
        eprintln!("DATABASE_URL is unset; skipping PostgreSQL integration test");
        return;
    };
    let admin = PgPoolOptions::new()
        .max_connections(2)
        .connect(&database_url)
        .await
        .expect("connect PostgreSQL integration service");
    let version: i32 = query_scalar::<_, String>("SHOW server_version_num")
        .fetch_one(&admin)
        .await
        .expect("read PostgreSQL version")
        .parse()
        .expect("numeric PostgreSQL server_version_num");
    assert!(
        version >= 160_000,
        "integration suite requires PostgreSQL 16+"
    );

    let schema = format!("kitsu_test_{}", Uuid::new_v4().simple());
    assert!(
        schema.starts_with("kitsu_test_")
            && schema
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_')
    );
    query(&format!("CREATE SCHEMA \"{schema}\""))
        .execute(&admin)
        .await
        .expect("create isolated test schema");
    let mut scoped_url = url::Url::parse(&database_url).expect("DATABASE_URL is a URL");
    scoped_url
        .query_pairs_mut()
        .append_pair("options", &format!("-csearch_path={schema}"));

    let db = Database::connect(scoped_url.as_str(), 8)
        .await
        .expect("apply migrations");
    let migration_count: i64 = query_scalar("SELECT count(*) FROM _sqlx_migrations")
        .fetch_one(db.pool())
        .await
        .unwrap();
    assert_eq!(migration_count, 10);

    let owner = db
        .upsert_owner(&OidcPrincipal {
            issuer: "https://issuer.integration.invalid".to_owned(),
            subject: format!("owner-{}", Uuid::new_v4()),
            email: None,
            display_name: Some("Integration owner".to_owned()),
        })
        .await
        .unwrap();
    let other_owner = db
        .upsert_owner(&OidcPrincipal {
            issuer: "https://issuer.integration.invalid".to_owned(),
            subject: format!("other-{}", Uuid::new_v4()),
            email: None,
            display_name: None,
        })
        .await
        .unwrap();

    // A native installation can atomically create and persist a logical
    // gateway without a PC gateway certificate. The installation is
    // immutable and never visible or reusable across owners.
    let relay_installation_id = Uuid::new_v4();
    let relay_gateway_id = Uuid::new_v4();
    let relay = db
        .create_or_get_mobile_relay(owner.id, relay_installation_id, relay_gateway_id)
        .await
        .unwrap();
    assert_eq!(relay.installation_id, relay_installation_id);
    assert_eq!(relay.gateway_id, relay_gateway_id);
    let relay_replay = db
        .create_or_get_mobile_relay(owner.id, relay_installation_id, relay_gateway_id)
        .await
        .unwrap();
    assert_eq!(relay_replay.created_at, relay.created_at);
    let loaded_relay = db
        .mobile_relay(owner.id, relay_installation_id)
        .await
        .unwrap();
    assert_eq!(loaded_relay.gateway.id, relay_gateway_id);
    assert!(loaded_relay.gateway.certificate_id.is_none());
    assert!(db
        .gateway_by_id(relay_gateway_id)
        .await
        .unwrap()
        .unwrap()
        .certificate_id
        .is_none());
    assert!(matches!(
        db.mobile_relay(other_owner.id, relay_installation_id).await,
        Err(ApiError::NotFound)
    ));
    assert!(matches!(
        db.create_or_get_mobile_relay(other_owner.id, relay_installation_id, relay_gateway_id)
            .await,
        Err(ApiError::NotFound)
    ));
    assert!(matches!(
        db.create_or_get_mobile_relay(owner.id, relay_installation_id, Uuid::new_v4())
            .await,
        Err(ApiError::Conflict(_))
    ));

    // An account-free installation receives a separate, pending relay
    // principal. Exact retries preserve it, while wrong credentials and
    // immutable-gateway rebinds are rejected without granting owner access.
    let device_relay_installation_id = Uuid::new_v4();
    let device_relay_gateway_id = Uuid::new_v4();
    let device_relay_credential = sha256(b"integration device relay credential");
    let device_relay = db
        .create_or_get_device_relay(
            device_relay_installation_id,
            device_relay_gateway_id,
            &device_relay_credential,
        )
        .await
        .unwrap();
    assert!(!device_relay.activated);
    let device_relay_replay = db
        .create_or_get_device_relay(
            device_relay_installation_id,
            device_relay_gateway_id,
            &device_relay_credential,
        )
        .await
        .unwrap();
    assert_eq!(
        device_relay_replay.relay.view.created_at,
        device_relay.relay.view.created_at
    );
    assert!(matches!(
        db.device_relay(
            device_relay_installation_id,
            &sha256(b"wrong integration device relay credential")
        )
        .await,
        Err(ApiError::Unauthorized)
    ));
    assert!(matches!(
        db.create_device_relay_enrollment(
            device_relay_installation_id,
            &sha256(b"wrong integration device relay credential"),
            "x",
            "",
            &sha256(b"untrusted enrollment token"),
            std::time::Duration::from_secs(600),
        )
        .await,
        Err(ApiError::Unauthorized)
    ));
    assert!(matches!(
        db.create_or_get_device_relay(
            device_relay_installation_id,
            Uuid::new_v4(),
            &device_relay_credential
        )
        .await,
        Err(ApiError::Conflict(_))
    ));
    assert!(matches!(
        db.mobile_relay(owner.id, device_relay_installation_id)
            .await,
        Err(ApiError::NotFound)
    ));

    // A pending principal has one live enrollment slot. An ambiguous CA
    // attempt beyond the provider replay window releases that slot so the
    // documented replacement-claim recovery remains possible.
    let device_enrollment = db
        .create_device_relay_enrollment(
            device_relay_installation_id,
            &device_relay_credential,
            &format!("device-relay-{}", Uuid::new_v4()),
            "Device relay companion",
            &sha256(b"device relay enrollment token"),
            std::time::Duration::from_secs(600),
        )
        .await
        .unwrap();
    assert!(matches!(
        db.create_device_relay_enrollment(
            device_relay_installation_id,
            &device_relay_credential,
            &format!("device-relay-{}", Uuid::new_v4()),
            "Second pending companion",
            &sha256(b"second pending device relay enrollment token"),
            std::time::Duration::from_secs(600),
        )
        .await,
        Err(ApiError::Conflict(_))
    ));
    query(
        r#"
        UPDATE enrollment_challenges SET status='issuing',
          claim_request_sha256=$2,reserved_companion_id=$3,
          provider_started_at=clock_timestamp()-interval '5 minutes',
          provider_ambiguous=TRUE
        WHERE id=$1
        "#,
    )
    .bind(device_enrollment.id)
    .bind(sha256(b"ambiguous device relay claim").as_slice())
    .bind(Uuid::new_v4())
    .execute(db.pool())
    .await
    .unwrap();
    db.create_device_relay_enrollment(
        device_relay_installation_id,
        &device_relay_credential,
        &format!("device-relay-{}", Uuid::new_v4()),
        "Replacement companion",
        &sha256(b"replacement device relay enrollment token"),
        std::time::Duration::from_secs(600),
    )
    .await
    .unwrap();

    // The retention worker removes only old, never-activated synthetic
    // identities. This one has no enrollment and is asserted after the
    // existing retention pass below.
    let expired_device_relay_installation_id = Uuid::new_v4();
    let expired_device_relay_gateway_id = Uuid::new_v4();
    let expired_device_relay_credential = sha256(b"expired device relay credential");
    db.create_or_get_device_relay(
        expired_device_relay_installation_id,
        expired_device_relay_gateway_id,
        &expired_device_relay_credential,
    )
    .await
    .unwrap();
    query(
        "UPDATE mobile_relay_credentials SET created_at=clock_timestamp()-interval '2 days' WHERE installation_id=$1",
    )
    .bind(expired_device_relay_installation_id)
    .execute(db.pool())
    .await
    .unwrap();

    // Exercise the one-use, owner-authorized gateway bootstrap all the way to
    // durable certificate identity, including exact replay and request binding.
    let bootstrap_token = sha256(b"integration gateway bootstrap token");
    let bootstrap_request = sha256(b"integration gateway CSR");
    let bootstrap = db
        .create_gateway_bootstrap(
            owner.id,
            "Integration gateway",
            &bootstrap_token,
            std::time::Duration::from_secs(600),
        )
        .await
        .unwrap();
    let mut reserved_gateway = match db
        .begin_gateway_bootstrap(bootstrap.id, &bootstrap_token, &bootstrap_request)
        .await
        .unwrap()
    {
        BeginGatewayBootstrap::Issue(reserved) => reserved,
        BeginGatewayBootstrap::Completed(_) => panic!("new bootstrap was already completed"),
    };
    let gateway_id = reserved_gateway.gateway_id;
    db.mark_gateway_provider_attempt(&reserved_gateway)
        .await
        .unwrap();
    db.record_gateway_provider_job(&reserved_gateway, "integration-ca:21")
        .await
        .unwrap();
    // A claim that crossed the CA boundary while valid remains resumable after
    // its bearer-token deadline; otherwise an already-issued certificate can
    // be orphaned solely because provider polling was slow.
    query(
        "UPDATE gateway_bootstraps SET created_at=clock_timestamp()-interval '2 seconds', expires_at=clock_timestamp()-interval '1 second' WHERE id=$1",
    )
    .bind(bootstrap.id)
    .execute(db.pool())
    .await
    .unwrap();
    db.release_gateway_bootstrap(reserved_gateway.id, reserved_gateway.issuance_id)
        .await
        .unwrap();
    reserved_gateway = match db
        .begin_gateway_bootstrap(bootstrap.id, &bootstrap_token, &bootstrap_request)
        .await
        .unwrap()
    {
        BeginGatewayBootstrap::Issue(reserved) => reserved,
        BeginGatewayBootstrap::Completed(_) => panic!("released CA job completed unexpectedly"),
    };
    assert_eq!(
        reserved_gateway.provider_job_id.as_deref(),
        Some("integration-ca:21")
    );
    let gateway_san = format!("urn:kitsu:gateway:{gateway_id}");
    let gateway_certificate = fixture_certificate(&gateway_san, 0x21);
    let gateway_fingerprint = gateway_certificate.fingerprint_sha256;
    let gateway_valid_after = gateway_certificate.valid_after;
    let gateway_valid_until = gateway_certificate.valid_until;
    let completed_gateway = db
        .complete_gateway_bootstrap(&reserved_gateway, &gateway_certificate)
        .await
        .unwrap();
    assert_eq!(completed_gateway.gateway_id, gateway_id);
    let (gateway_replay_one, gateway_replay_two) = tokio::join!(
        db.begin_gateway_bootstrap(bootstrap.id, &bootstrap_token, &bootstrap_request),
        db.begin_gateway_bootstrap(bootstrap.id, &bootstrap_token, &bootstrap_request)
    );
    for replay in [gateway_replay_one.unwrap(), gateway_replay_two.unwrap()] {
        match replay {
            BeginGatewayBootstrap::Completed(result) => {
                assert_eq!(result.certificate_der, completed_gateway.certificate_der)
            }
            BeginGatewayBootstrap::Issue(_) => panic!("completed bootstrap was issued twice"),
        }
    }
    assert!(matches!(
        db.begin_gateway_bootstrap(bootstrap.id, &bootstrap_token, &sha256(b"different CSR"))
            .await,
        Err(ApiError::Conflict(_))
    ));

    // A provider call interrupted before its ARN is observed is quarantined
    // instead of being reissued after AWS's five-minute idempotency window.
    let ambiguous_token = sha256(b"ambiguous gateway bootstrap token");
    let ambiguous_request = sha256(b"ambiguous gateway CSR");
    let ambiguous = db
        .create_gateway_bootstrap(
            owner.id,
            "Ambiguous gateway",
            &ambiguous_token,
            std::time::Duration::from_secs(600),
        )
        .await
        .unwrap();
    let ambiguous_reserved = match db
        .begin_gateway_bootstrap(ambiguous.id, &ambiguous_token, &ambiguous_request)
        .await
        .unwrap()
    {
        BeginGatewayBootstrap::Issue(reserved) => reserved,
        BeginGatewayBootstrap::Completed(_) => panic!("new ambiguous bootstrap completed"),
    };
    db.mark_gateway_provider_attempt(&ambiguous_reserved)
        .await
        .unwrap();
    db.release_gateway_bootstrap(ambiguous.id, ambiguous_reserved.issuance_id)
        .await
        .unwrap();
    let ambiguous_retry = match db
        .begin_gateway_bootstrap(ambiguous.id, &ambiguous_token, &ambiguous_request)
        .await
        .unwrap()
    {
        BeginGatewayBootstrap::Issue(reserved) => reserved,
        BeginGatewayBootstrap::Completed(_) => panic!("ambiguous provider call completed"),
    };
    let ambiguous_row =
        query("SELECT status::text,provider_ambiguous FROM gateway_bootstraps WHERE id=$1")
            .bind(ambiguous.id)
            .fetch_one(db.pool())
            .await
            .unwrap();
    assert_eq!(ambiguous_row.get::<String, _>("status"), "issuing");
    assert!(ambiguous_row.get::<bool, _>("provider_ambiguous"));
    query(
        "UPDATE gateway_bootstraps SET provider_started_at=clock_timestamp()-interval '5 minutes' WHERE id=$1",
    )
    .bind(ambiguous.id)
    .execute(db.pool())
    .await
    .unwrap();
    db.release_gateway_bootstrap(ambiguous.id, ambiguous_retry.issuance_id)
        .await
        .unwrap();
    assert!(matches!(
        db.begin_gateway_bootstrap(ambiguous.id, &ambiguous_token, &ambiguous_request)
            .await,
        Err(ApiError::ReplacementRequired)
    ));

    let gateway_mtls = MtlsIdentity {
        certificate_sha256: gateway_fingerprint,
        uri_san: gateway_san,
        not_before: gateway_valid_after,
        not_after: gateway_valid_until,
    };
    let gateway = db.gateway_by_mtls(&gateway_mtls).await.unwrap();
    assert_eq!(gateway.id, gateway_id);

    // LAN discovery metadata is owner-scoped and bound to the exact active
    // certificate which authenticated the write. Only public connection
    // material is retained and returned.
    let catalog_ca = vec![0x30, 0x00];
    let catalog_spki = sha256(b"integration gateway server SPKI");
    db.upsert_gateway_catalog(
        &gateway,
        "Integration gateway",
        "gateway.integration.local",
        7442,
        7443,
        "gateway.integration.local",
        &catalog_ca,
        &catalog_spki,
    )
    .await
    .unwrap();
    let catalog = db.list_gateway_catalog(owner.id).await.unwrap();
    assert_eq!(catalog.len(), 1);
    assert_eq!(catalog[0].gateway_id, gateway_id);
    assert_eq!(catalog[0].host, "gateway.integration.local");
    assert_eq!(catalog[0].bootstrap_port, 7442);
    assert_eq!(catalog[0].port, 7443);
    assert_eq!(catalog[0].state, "active");
    assert_eq!(
        catalog[0].ca_cert_der_b64,
        URL_SAFE_NO_PAD.encode(&catalog_ca)
    );
    assert_eq!(
        catalog[0].spki_sha256_b64,
        URL_SAFE_NO_PAD.encode(catalog_spki)
    );
    assert!(db
        .list_gateway_catalog(other_owner.id)
        .await
        .unwrap()
        .is_empty());
    let forged_gateway = Gateway {
        certificate_id: Some(Uuid::new_v4()),
        ..gateway.clone()
    };
    assert!(matches!(
        db.upsert_gateway_catalog(
            &forged_gateway,
            "Forged gateway",
            "forged.integration.local",
            7442,
            7443,
            "forged.integration.local",
            &catalog_ca,
            &catalog_spki,
        )
        .await,
        Err(ApiError::Unauthorized)
    ));

    // Complete physical companion enrollment atomically. A gateway owned by a
    // different principal cannot consume the owner's challenge.
    let hardware_uid = format!("KITSU868-INTEGRATION-{gateway_id}");
    let enrollment_token = sha256(b"integration device enrollment token");
    let enrollment_request = sha256(b"integration signed device claim");
    let enrollment = db
        .create_enrollment(
            owner.id,
            &hardware_uid,
            "Integration Kitsu",
            &enrollment_token,
            std::time::Duration::from_secs(600),
        )
        .await
        .unwrap();
    let wrong_owner_gateway = Gateway {
        owner_id: other_owner.id,
        ..gateway.clone()
    };
    assert!(matches!(
        db.begin_enrollment_issuance(
            enrollment.id,
            &enrollment_token,
            &enrollment_request,
            &hardware_uid,
            &wrong_owner_gateway,
        )
        .await,
        Err(ApiError::Forbidden)
    ));
    let mut reserved_enrollment = match db
        .begin_enrollment_issuance(
            enrollment.id,
            &enrollment_token,
            &enrollment_request,
            &hardware_uid,
            &gateway,
        )
        .await
        .unwrap()
    {
        BeginEnrollmentIssuance::Issue(reserved) => reserved,
        BeginEnrollmentIssuance::Completed(_) => panic!("new enrollment was already completed"),
    };
    let companion_id = reserved_enrollment.companion_id;
    db.mark_enrollment_provider_attempt(&reserved_enrollment)
        .await
        .unwrap();
    db.record_enrollment_provider_job(&reserved_enrollment, "integration-ca:41")
        .await
        .unwrap();
    query(
        "UPDATE enrollment_challenges SET created_at=clock_timestamp()-interval '2 seconds', expires_at=clock_timestamp()-interval '1 second' WHERE id=$1",
    )
    .bind(enrollment.id)
    .execute(db.pool())
    .await
    .unwrap();
    db.release_enrollment_issuance(enrollment.id, reserved_enrollment.issuance_id)
        .await
        .unwrap();
    reserved_enrollment = match db
        .begin_enrollment_issuance(
            enrollment.id,
            &enrollment_token,
            &enrollment_request,
            &hardware_uid,
            &gateway,
        )
        .await
        .unwrap()
    {
        BeginEnrollmentIssuance::Issue(reserved) => reserved,
        BeginEnrollmentIssuance::Completed(_) => {
            panic!("released enrollment completed unexpectedly")
        }
    };
    assert_eq!(
        reserved_enrollment.provider_job_id.as_deref(),
        Some("integration-ca:41")
    );
    let secret = NewSecretRecord {
        companion_id,
        key_version: 1,
        kms_key_id: "integration-kms".to_owned(),
        wrapped_dek: vec![1_u8; 48],
        encrypted: EncryptedBytes {
            nonce: [2_u8; 12],
            ciphertext: vec![3_u8; 48],
        },
    };
    let companion_certificate =
        fixture_certificate(&format!("urn:kitsu:companion:{companion_id}"), 0x41);
    let sealed = SealedSecret {
        enc: [4_u8; 65],
        ciphertext: Zeroizing::new(vec![5_u8; 48]),
    };
    let completed_enrollment = db
        .complete_enrollment_issuance(
            &reserved_enrollment,
            &secret,
            &companion_certificate,
            &sealed,
        )
        .await
        .unwrap();
    let enrollment_replay = db
        .begin_enrollment_issuance(
            enrollment.id,
            &enrollment_token,
            &enrollment_request,
            &hardware_uid,
            &gateway,
        )
        .await
        .unwrap();
    match enrollment_replay {
        BeginEnrollmentIssuance::Completed(result) => {
            assert_eq!(result.hpke_enc, completed_enrollment.hpke_enc);
            assert_eq!(result.hpke_ciphertext, completed_enrollment.hpke_ciphertext);
            assert_eq!(result.certificate_der, completed_enrollment.certificate_der);
        }
        BeginEnrollmentIssuance::Issue(_) => panic!("completed enrollment was issued twice"),
    }
    assert!(matches!(
        db.begin_enrollment_issuance(
            enrollment.id,
            &enrollment_token,
            &sha256(b"different signed claim"),
            &hardware_uid,
            &gateway,
        )
        .await,
        Err(ApiError::Conflict(_))
    ));

    // Ownership is checked by every projection/list query, not inferred from a
    // caller-supplied companion UUID.
    assert!(matches!(
        db.snapshot(other_owner.id, companion_id).await,
        Err(ApiError::NotFound)
    ));

    let envelope = DeviceEnvelope {
        schema: DEVICE_ENVELOPE_SCHEMA.to_owned(),
        companion_id,
        gateway_id,
        sequence: "1".to_owned(),
        issued_epoch: "0".to_owned(),
        nonce_b64: URL_SAFE_NO_PAD.encode([4_u8; 16]),
        request_id: Uuid::new_v4(),
        key_version: 1,
        payload_type: "heartbeat".to_owned(),
        payload_b64: URL_SAFE_NO_PAD.encode(br#"{"type":"heartbeat","uptime_ms":7}"#),
        signature_b64: URL_SAFE_NO_PAD.encode([5_u8; 32]),
    };
    let validated_for_hash = envelope.validate_wrapper().unwrap();
    let transcript_hash = sha256(&device_transcript(&envelope, &validated_for_hash).unwrap());
    let ingest = |database: Database, gateway: Gateway, envelope: DeviceEnvelope| async move {
        let validated = envelope.validate_wrapper().unwrap();
        database
            .ingest_verified_envelope(&gateway, &envelope, &validated, &transcript_hash)
            .await
    };
    let (first, second) = tokio::join!(
        ingest(db.clone(), gateway.clone(), envelope.clone()),
        ingest(db.clone(), gateway.clone(), envelope.clone())
    );
    let accepted = [first.unwrap(), second.unwrap()]
        .into_iter()
        .filter(|value| *value == IngestOutcome::Accepted)
        .count();
    assert_eq!(accepted, 1);
    let request_count: i64 = query_scalar("SELECT count(*) FROM device_requests")
        .fetch_one(db.pool())
        .await
        .unwrap();
    assert_eq!(request_count, 1);
    let last_sequence: i64 =
        query_scalar("SELECT last_sequence FROM device_sequences WHERE companion_id=$1")
            .bind(companion_id)
            .fetch_one(db.pool())
            .await
            .unwrap();
    assert_eq!(last_sequence, 1);

    // Device-reported connectivity and channel metadata are accepted only as
    // exact, authenticated protocol snapshots. The owner projection must
    // preserve that truth without inventing channel names or false states.
    let snapshot_body = json!({
        "schema": "kitsu.companion-snapshot.v1",
        "firmware_version": "integration-fw-1",
        "remote_connectivity_allowed": true,
        "wifi": {"configured": true, "state": "connected"},
        "gateway": {"configured": true, "enrolled": true, "lan_state": "connected"},
        "channels": [
            {"slot": 0, "configured": true, "max_utf8_bytes": 128, "name": "Primary"},
            {"slot": 1, "configured": false, "max_utf8_bytes": 128},
            {"slot": 2, "configured": true, "max_utf8_bytes": 128, "name": "Friends"},
            {"slot": 3, "configured": false, "max_utf8_bytes": 128}
        ]
    });
    ingest_protocol_payload(
        &db,
        &gateway,
        companion_id,
        gateway_id,
        2,
        "event_batch",
        json!({
            "type": "event_batch",
            "events": [{
                "event_id": Uuid::new_v4(),
                "event_type": "companion.snapshot",
                "observed": {"epoch": 1_800_000_000_i64, "boot_id": 7, "monotonic_ms": 42},
                "body": snapshot_body
            }]
        }),
    )
    .await;
    let latest = db
        .latest_device_snapshot(owner.id, companion_id)
        .await
        .unwrap()
        .unwrap();
    assert_eq!(latest["channels"][0]["name"], "Primary");
    assert_eq!(latest["channels"][1]["configured"], false);
    assert!(latest["channels"][1].get("name").is_none());
    let projected = db.snapshot(owner.id, companion_id).await.unwrap();
    assert_eq!(projected.connectivity["online"], true);
    assert_eq!(
        projected.connectivity["provenance"],
        "gateway_mtls_device_hmac"
    );
    assert_eq!(projected.connectivity["gateway_id"], gateway_id.to_string());
    assert!(projected.connectivity["last_seen_at"].is_string());
    assert_eq!(projected.connectivity["wifi_configured"], true);
    assert_eq!(projected.connectivity["wifi_state"], "connected");
    assert_eq!(projected.connectivity["gateway_configured"], true);
    assert_eq!(projected.connectivity["gateway_enrolled"], true);
    assert_eq!(projected.connectivity["gateway_lan_state"], "connected");
    assert_eq!(projected.connectivity["remote_connectivity_allowed"], true);

    let peer_key = [0xAB_u8; 32];
    ingest_protocol_payload(
        &db,
        &gateway,
        companion_id,
        gateway_id,
        3,
        "peer_snapshot",
        json!({
            "type": "peer_snapshot",
            "peers": [{
                "public_key": hex::encode_upper(peer_key),
                "role": "client",
                "name": "Integration peer",
                "seen_count": 1,
                "last_seen": {"epoch": 1_800_000_001_i64, "boot_id": 7, "monotonic_ms": 43},
                "signal": {"scope": "last_hop", "rssi_deci_dbm": -700, "snr_deci_db": 80},
                "journal_sequence": 1
            }]
        }),
    )
    .await;
    let peers = db.list_peers(owner.id, companion_id).await.unwrap();
    assert_eq!(peers.len(), 1);
    assert_eq!(peers[0]["public_key_b64"], URL_SAFE_NO_PAD.encode(peer_key));
    assert_eq!(peers[0]["public_key_hex"], hex::encode_upper(peer_key));
    assert!(peers[0].get("public_key").is_none());
    assert_eq!(
        URL_SAFE_NO_PAD
            .decode(peers[0]["public_key_b64"].as_str().unwrap())
            .unwrap()
            .len(),
        32
    );

    // Migration 0008 is exercised through the real persistence methods, and
    // migration 0006's retention exception removes only data past policy.
    let contact_id = db
        .create_public_contact(
            "security",
            "integration-contact",
            "A sufficiently detailed integration security report.",
            &sha256(b"integration source address"),
        )
        .await
        .unwrap();
    assert_eq!(db.list_public_contacts(10).await.unwrap().len(), 1);
    let resolved = db.resolve_public_contact(contact_id).await.unwrap();
    assert_eq!(resolved.status, "resolved");
    query(
        "UPDATE public_contact_messages SET resolved_at=clock_timestamp()-interval '31 days' WHERE id=$1",
    )
    .bind(contact_id)
    .execute(db.pool())
    .await
    .unwrap();
    db.apply_retention(90, 90, 365).await.unwrap();
    let retained_contact_count: i64 =
        query_scalar("SELECT count(*) FROM public_contact_messages WHERE id=$1")
            .bind(contact_id)
            .fetch_one(db.pool())
            .await
            .unwrap();
    assert_eq!(retained_contact_count, 0);
    assert!(matches!(
        db.device_relay(
            expired_device_relay_installation_id,
            &expired_device_relay_credential
        )
        .await,
        Err(ApiError::Unauthorized)
    ));
    // Existing OIDC-owned relay rows are outside the synthetic issuer and
    // survive the same cleanup pass.
    assert_eq!(
        db.mobile_relay(owner.id, relay_installation_id)
            .await
            .unwrap()
            .gateway
            .id,
        relay_gateway_id
    );

    // Migration 0007 makes deletion crash-resumable: tombstone first, retry
    // the external IdP phase, persist the IdP result, then erase local keys.
    // A re-request can never move `deleting` back to `pending`.
    let deletion_principal = OidcPrincipal {
        issuer: "https://issuer.integration.invalid".to_owned(),
        subject: Uuid::new_v4().to_string(),
        email: Some("delete@example.invalid".to_owned()),
        display_name: Some("Delete integration owner".to_owned()),
    };
    let deletion_owner = db.upsert_owner(&deletion_principal).await.unwrap();
    db.request_account_deletion(deletion_owner.id, std::time::Duration::from_secs(60))
        .await
        .unwrap();
    query(
        "UPDATE account_deletion_requests SET requested_at=clock_timestamp()-interval '2 seconds', execute_after=clock_timestamp()-interval '1 second' WHERE owner_id=$1",
    )
    .bind(deletion_owner.id)
    .execute(db.pool())
    .await
    .unwrap();
    let due = db.prepare_due_account_deletions().await.unwrap();
    let due = due
        .into_iter()
        .find(|item| item.owner_id == deletion_owner.id)
        .unwrap();
    assert!(!due.identity_revoked);
    assert!(matches!(
        db.upsert_owner(&deletion_principal).await,
        Err(ApiError::Forbidden)
    ));
    assert!(matches!(
        db.request_account_deletion(deletion_owner.id, std::time::Duration::from_secs(60))
            .await,
        Err(ApiError::Conflict(_))
    ));
    // A worker retry before the IdP result remains an idempotent external
    // deletion attempt; after the result is persisted it resumes local erase.
    let retry = db.prepare_due_account_deletions().await.unwrap();
    assert!(retry
        .iter()
        .any(|item| item.owner_id == deletion_owner.id && !item.identity_revoked));
    db.mark_account_identity_revoked(deletion_owner.id)
        .await
        .unwrap();
    let resume = db.prepare_due_account_deletions().await.unwrap();
    assert!(resume
        .iter()
        .any(|item| item.owner_id == deletion_owner.id && item.identity_revoked));
    db.process_account_deletion(deletion_owner.id)
        .await
        .unwrap();
    assert_eq!(
        db.account_deletion(deletion_owner.id)
            .await
            .unwrap()
            .unwrap()
            .status,
        "completed"
    );
    assert!(matches!(
        db.upsert_owner(&deletion_principal).await,
        Err(ApiError::Forbidden)
    ));
    let tombstones: i64 = query_scalar("SELECT count(*) FROM deleted_oidc_subjects")
        .fetch_one(db.pool())
        .await
        .unwrap();
    assert_eq!(tombstones, 1);

    let action_request = CreateActionRequest {
        action_type: "companion.pet".to_owned(),
        parameters: Map::new(),
        expires_in_seconds: 60,
    };
    let action = sign_remote_action_with_nonce(
        Uuid::new_v4(),
        companion_id,
        1,
        [8_u8; 16],
        "companion.pet",
        1_800_000_000,
        1_800_000_060,
        b"{}",
        &[9_u8; 32],
    )
    .unwrap();
    let action_hash = sha256(b"integration action request");
    let (one, two) = tokio::join!(
        db.create_action(
            owner.id,
            companion_id,
            "same-key",
            &action_hash,
            &action_request,
            &action
        ),
        db.create_action(
            owner.id,
            companion_id,
            "same-key",
            &action_hash,
            &action_request,
            &action
        )
    );
    let one = one.unwrap();
    let two = two.unwrap();
    assert_eq!(one.view.id, two.view.id);
    assert_ne!(one.inserted, two.inserted);
    assert_eq!(
        serde_json::to_vec(&one.wire).unwrap(),
        serde_json::to_vec(&two.wire).unwrap()
    );
    let action_id = one.view.id;

    let audit_id: i64 = query(
        "INSERT INTO audit_log (actor_type,action,details) VALUES ('system','integration.append','{}') RETURNING id",
    )
    .fetch_one(db.pool())
    .await
    .unwrap()
    .get("id");
    assert!(
        query("UPDATE audit_log SET details='{\"changed\":true}' WHERE id=$1")
            .bind(audit_id)
            .execute(db.pool())
            .await
            .is_err()
    );

    // Close every application connection, reconnect, rerun migrations, and
    // prove that exact replay/result bytes survive a real service restart.
    db.pool().close().await;
    drop(db);
    let restarted = Database::connect(scoped_url.as_str(), 4)
        .await
        .expect("migrations and committed results survive restart");
    assert!(restarted.ready().await);
    let migration_count: i64 = query_scalar("SELECT count(*) FROM _sqlx_migrations")
        .fetch_one(restarted.pool())
        .await
        .unwrap();
    assert_eq!(migration_count, 10);
    let persisted_request_count: i64 = query_scalar("SELECT count(*) FROM device_requests")
        .fetch_one(restarted.pool())
        .await
        .unwrap();
    assert_eq!(persisted_request_count, 3);
    let persisted_action_count: i64 =
        query_scalar("SELECT count(*) FROM remote_actions WHERE id=$1")
            .bind(action_id)
            .fetch_one(restarted.pool())
            .await
            .unwrap();
    assert_eq!(persisted_action_count, 1);
    let restarted_gateway = restarted.gateway_by_mtls(&gateway_mtls).await.unwrap();
    assert_eq!(
        restarted
            .mobile_relay(owner.id, relay_installation_id)
            .await
            .unwrap()
            .gateway
            .id,
        relay_gateway_id
    );
    let restarted_catalog = restarted.list_gateway_catalog(owner.id).await.unwrap();
    assert_eq!(restarted_catalog.len(), 1);
    assert_eq!(restarted_catalog[0].gateway_id, restarted_gateway.id);
    let bootstrap_replay = restarted
        .begin_gateway_bootstrap(bootstrap.id, &bootstrap_token, &bootstrap_request)
        .await
        .unwrap();
    match bootstrap_replay {
        BeginGatewayBootstrap::Completed(result) => {
            assert_eq!(result.certificate_der, completed_gateway.certificate_der)
        }
        BeginGatewayBootstrap::Issue(_) => panic!("restart lost completed gateway result"),
    }
    let enrollment_replay = restarted
        .begin_enrollment_issuance(
            enrollment.id,
            &enrollment_token,
            &enrollment_request,
            &hardware_uid,
            &restarted_gateway,
        )
        .await
        .unwrap();
    match enrollment_replay {
        BeginEnrollmentIssuance::Completed(result) => {
            assert_eq!(result.hpke_enc, completed_enrollment.hpke_enc);
            assert_eq!(result.hpke_ciphertext, completed_enrollment.hpke_ciphertext);
        }
        BeginEnrollmentIssuance::Issue(_) => panic!("restart lost completed enrollment result"),
    }
    let validated = envelope.validate_wrapper().unwrap();
    assert_eq!(
        restarted
            .ingest_verified_envelope(&restarted_gateway, &envelope, &validated, &transcript_hash,)
            .await
            .unwrap(),
        IngestOutcome::DuplicateCommitted
    );
    restarted.pool().close().await;
    drop(restarted);
    let drop_statement = format!("DROP SCHEMA \"{schema}\" CASCADE");
    query(&drop_statement)
        .execute(&admin)
        .await
        .expect("remove isolated test schema");
}

fn fixture_certificate(san_uri: &str, marker: u8) -> ValidatedCertificate {
    let leaf_der = vec![marker; 96];
    let subject_public_key = vec![marker.wrapping_add(1); 65];
    ValidatedCertificate {
        fingerprint_sha256: sha256(&leaf_der),
        subject_public_key_sha256: sha256(&subject_public_key),
        leaf_der,
        chain_der: vec![vec![marker.wrapping_add(2); 96]],
        provider_id: format!("integration-ca:{marker:02x}"),
        serial_hex: format!("{marker:02x}"),
        san_uri: san_uri.to_owned(),
        valid_after: Utc::now() - TimeDelta::minutes(5),
        valid_until: Utc::now() + TimeDelta::days(30),
    }
}

async fn ingest_protocol_payload(
    database: &Database,
    gateway: &Gateway,
    companion_id: Uuid,
    gateway_id: Uuid,
    sequence: u64,
    payload_type: &str,
    payload: Value,
) {
    let payload = serde_json::to_vec(&payload).unwrap();
    let envelope = DeviceEnvelope {
        schema: DEVICE_ENVELOPE_SCHEMA.to_owned(),
        companion_id,
        gateway_id,
        sequence: sequence.to_string(),
        issued_epoch: "0".to_owned(),
        nonce_b64: URL_SAFE_NO_PAD.encode([u8::try_from(sequence).unwrap(); 16]),
        request_id: Uuid::new_v4(),
        key_version: 1,
        payload_type: payload_type.to_owned(),
        payload_b64: URL_SAFE_NO_PAD.encode(payload),
        signature_b64: URL_SAFE_NO_PAD.encode([0x5A_u8; 32]),
    };
    let validated = envelope.validate_wrapper().unwrap();
    let transcript_hash = sha256(&device_transcript(&envelope, &validated).unwrap());
    assert_eq!(
        database
            .ingest_verified_envelope(gateway, &envelope, &validated, &transcript_hash)
            .await
            .unwrap(),
        IngestOutcome::Accepted
    );
}
