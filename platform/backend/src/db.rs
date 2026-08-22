use std::{borrow::Cow, time::Duration};

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use chrono::{DateTime, TimeDelta, Utc};
use serde::Serialize;
use serde_json::{json, Map, Value};
use uuid::Uuid;

use crate::persistence as sqlx;
use sqlx::{
    postgres::{PgPoolOptions, PgRow},
    PgPool, Postgres, Row, Transaction,
};
use subtle::ConstantTimeEq;

use crate::{
    crypto::{oidc_subject_digest, sha256, EncryptedBytes},
    error::ApiError,
    issuer::RevokedCertificate,
    mtls::MtlsIdentity,
    oidc::OidcPrincipal,
    pki::{SealedSecret, ValidatedCertificate},
    wire::{
        CreateActionRequest, DeviceEnvelope, DevicePayload, RemoteAction, ValidatedEnvelope,
        REMOTE_ACTION_SCHEMA,
    },
};

// Short leases permit another instance to repeat the same provider request
// while AWS Private CA still guarantees idempotency. The original provider
// boundary timestamp is retained separately and never extended by retries.
// The route's configured certificate-provider timeout is capped at 150 seconds
// and has a 30-second local-work margin.  Keep a live worker's lease beyond
// that bound, while still allowing a crashed worker to retry AWS PCA's exact
// idempotency token before its five-minute provider window closes.
const ISSUANCE_LEASE_SECONDS: i64 = 195;
const PROVIDER_RETRY_WINDOW_SECONDS: i64 = 255;
const DEVICE_RELAY_OWNER_ISSUER: &str = "urn:kitsu:device-relay";
const DEVICE_RELAY_MAXIMUM_COMPANIONS: i64 = 3;
const DEVICE_RELAY_PENDING_RETENTION_SECONDS: i64 = 86_400;

#[derive(Clone)]
pub struct Database {
    pool: PgPool,
}

#[derive(Clone)]
pub struct Owner {
    pub id: Uuid,
    pub issuer: String,
    pub subject: String,
}

pub struct BrowserAttempt {
    pub id: Uuid,
    pub pkce_nonce: [u8; 12],
    pub pkce_ciphertext: Vec<u8>,
    pub nonce_digest: [u8; 32],
    pub return_url: String,
}

pub struct BrowserSession {
    pub id: Uuid,
    pub owner: Owner,
    pub csrf_digest: [u8; 32],
    pub expires_at: DateTime<Utc>,
}

#[derive(Clone)]
pub struct Gateway {
    pub id: Uuid,
    pub owner_id: Uuid,
    /// Present for a certificate-backed gateway projection. Owner-authenticated
    /// mobile relays use the same stable logical gateway identity without
    /// manufacturing an mTLS credential.
    pub certificate_id: Option<Uuid>,
    pub certificate_sha256: Option<[u8; 32]>,
}

#[derive(Clone, Serialize)]
pub struct MobileRelayView {
    pub installation_id: Uuid,
    pub gateway_id: Uuid,
    pub created_at: DateTime<Utc>,
}

pub struct MobileRelay {
    pub view: MobileRelayView,
    pub gateway: Gateway,
}

pub struct DeviceRelay {
    pub relay: MobileRelay,
    pub activated: bool,
}

#[derive(Serialize)]
pub struct GatewayCatalogView {
    pub gateway_id: Uuid,
    pub display_name: String,
    pub host: String,
    pub bootstrap_port: u16,
    pub port: u16,
    pub server_name: String,
    pub ca_cert_der_b64: String,
    pub spki_sha256_b64: String,
    pub state: String,
}

#[derive(Serialize)]
pub struct CertificateRotationView {
    pub id: Uuid,
    pub gateway_id: Uuid,
    pub old_certificate_id: Uuid,
    pub status: String,
    pub expires_at: DateTime<Utc>,
    pub overlap_ends_at: Option<DateTime<Utc>>,
}

#[derive(Serialize)]
pub struct ActivatedCertificateView {
    pub gateway_id: Uuid,
    pub certificate_id: Uuid,
    pub overlap_ends_at: DateTime<Utc>,
}

pub struct ReservedEnrollment {
    pub id: Uuid,
    pub owner_id: Uuid,
    pub hardware_uid: String,
    pub display_name: String,
    pub gateway_id: Uuid,
    pub companion_id: Uuid,
    pub key_version: u32,
    pub issuance_id: Uuid,
    pub request_sha256: [u8; 32],
    pub provider_job_id: Option<String>,
}

pub enum BeginEnrollmentIssuance {
    Issue(ReservedEnrollment),
    Completed(EnrollmentIssuanceResult),
}

#[derive(Clone)]
pub struct EnrollmentIssuanceResult {
    pub companion_id: Uuid,
    pub gateway_id: Uuid,
    pub key_version: u32,
    pub certificate_der: Vec<u8>,
    pub certificate_chain_der: Vec<Vec<u8>>,
    pub hpke_enc: [u8; 65],
    pub hpke_ciphertext: [u8; 48],
}

#[derive(Serialize)]
pub struct GatewayBootstrapView {
    pub id: Uuid,
    pub display_name: String,
    pub status: String,
    pub expires_at: DateTime<Utc>,
}

pub struct ReservedGatewayBootstrap {
    pub id: Uuid,
    pub owner_id: Uuid,
    pub display_name: String,
    pub gateway_id: Uuid,
    pub issuance_id: Uuid,
    pub request_sha256: [u8; 32],
    pub provider_job_id: Option<String>,
}

pub enum BeginGatewayBootstrap {
    Issue(ReservedGatewayBootstrap),
    Completed(GatewayBootstrapResult),
}

#[derive(Clone)]
pub struct GatewayBootstrapResult {
    pub gateway_id: Uuid,
    pub certificate_der: Vec<u8>,
    pub certificate_chain_der: Vec<Vec<u8>>,
}

pub struct SecretRecord {
    pub companion_id: Uuid,
    pub gateway_id: Uuid,
    pub key_version: u32,
    pub kms_key_id: String,
    pub wrapped_dek: Vec<u8>,
    pub encrypted: EncryptedBytes,
}

pub struct NewSecretRecord {
    pub companion_id: Uuid,
    pub key_version: u32,
    pub kms_key_id: String,
    pub wrapped_dek: Vec<u8>,
    pub encrypted: EncryptedBytes,
}

pub struct ActionSecretRecord {
    pub companion_id: Uuid,
    pub key_version: u32,
    pub kms_key_id: String,
    pub wrapped_dek: Vec<u8>,
    pub encrypted: EncryptedBytes,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IngestOutcome {
    Accepted,
    DuplicateCommitted,
}

#[derive(Serialize)]
pub struct EnrollmentView {
    pub id: Uuid,
    pub hardware_uid: String,
    pub display_name: String,
    pub status: String,
    pub expires_at: DateTime<Utc>,
}

#[derive(Serialize)]
pub struct ActionView {
    pub id: Uuid,
    pub companion_id: Uuid,
    pub action_type: String,
    pub parameters: Value,
    pub status: String,
    pub created_at: DateTime<Utc>,
    pub expires_at: DateTime<Utc>,
    pub completed_at: Option<DateTime<Utc>>,
    pub result: Option<Value>,
}

pub struct CreatedAction {
    pub view: ActionView,
    pub wire: RemoteAction,
    pub inserted: bool,
}

#[derive(Serialize)]
pub struct CompanionListItem {
    pub id: Uuid,
    pub hardware_uid: String,
    pub display_name: String,
    pub status: String,
    pub last_seen_at: Option<DateTime<Utc>>,
}

#[derive(Serialize)]
pub struct AccountDeletionView {
    pub status: String,
    pub requested_at: DateTime<Utc>,
    pub execute_after: DateTime<Utc>,
    pub cancelled_at: Option<DateTime<Utc>>,
    pub completed_at: Option<DateTime<Utc>>,
}

pub struct DueAccountDeletion {
    pub owner_id: Uuid,
    pub issuer: String,
    pub subject: String,
    /// True once the identity provider has confirmed that the external
    /// identity no longer exists. A worker crash after persisting this marker
    /// must resume at local crypto-erasure instead of silently stranding the
    /// request in `deleting` forever.
    pub identity_revoked: bool,
}

#[derive(Serialize)]
pub struct PublicContactView {
    pub id: Uuid,
    pub category: String,
    pub reply_contact: String,
    pub message: String,
    pub status: String,
    pub created_at: DateTime<Utc>,
    pub resolved_at: Option<DateTime<Utc>>,
}

#[derive(Serialize)]
pub struct SnapshotProjection {
    pub companion: Value,
    pub vitals: Value,
    pub mood: Value,
    pub bond: Value,
    pub evolution: Value,
    pub connectivity: Value,
    pub mesh: Value,
    pub counts: Value,
    pub recent_events: Vec<Value>,
    pub cursor: String,
}

impl Database {
    pub async fn connect(database_url: &str, max_connections: u32) -> anyhow::Result<Self> {
        let pool = PgPoolOptions::new()
            .max_connections(max_connections)
            .min_connections(1)
            .acquire_timeout(Duration::from_secs(10))
            .idle_timeout(Duration::from_secs(300))
            .connect(database_url)
            .await?;
        let migrator = sqlx::migrate::Migrator {
            migrations: Cow::Owned(vec![
                sqlx::migrate::Migration::new(
                    1,
                    Cow::Borrowed("initial"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!("../migrations/0001_initial.sql")),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    2,
                    Cow::Borrowed("production enrollment"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!("../migrations/0002_production_enrollment.sql")),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    3,
                    Cow::Borrowed("enrollment issuance invariant"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!(
                        "../migrations/0003_enrollment_issuance_invariant.sql"
                    )),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    4,
                    Cow::Borrowed("gateway catalog"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!("../migrations/0004_gateway_catalog.sql")),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    5,
                    Cow::Borrowed("gateway bootstrap port"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!(
                        "../migrations/0005_gateway_bootstrap_port.sql"
                    )),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    6,
                    Cow::Borrowed("retention and account deletion"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!(
                        "../migrations/0006_retention_and_account_deletion.sql"
                    )),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    7,
                    Cow::Borrowed("identity deletion"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!("../migrations/0007_identity_deletion.sql")),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    8,
                    Cow::Borrowed("public contact"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!("../migrations/0008_public_contact.sql")),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    9,
                    Cow::Borrowed("mobile relay"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!("../migrations/0009_mobile_relay.sql")),
                    false,
                ),
                sqlx::migrate::Migration::new(
                    10,
                    Cow::Borrowed("device relay credentials"),
                    sqlx::migrate::MigrationType::Simple,
                    Cow::Borrowed(include_str!(
                        "../migrations/0010_device_relay_credentials.sql"
                    )),
                    false,
                ),
            ]),
            ..sqlx::migrate::Migrator::DEFAULT
        };
        migrator.run(&pool).await?;
        Ok(Self { pool })
    }

    pub fn pool(&self) -> &PgPool {
        &self.pool
    }

    pub async fn ready(&self) -> bool {
        sqlx::query_scalar::<_, i32>("SELECT 1")
            .fetch_one(&self.pool)
            .await
            .is_ok()
    }

    pub async fn ensure_owner_companion(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
    ) -> Result<(), ApiError> {
        ensure_owner_companion(&self.pool, owner_id, companion_id).await
    }

    pub async fn latest_device_snapshot(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
    ) -> Result<Option<Value>, ApiError> {
        ensure_owner_companion(&self.pool, owner_id, companion_id).await?;
        Ok(sqlx::query_scalar::<_, Value>(
            r#"
            SELECT e.body FROM device_events e
            WHERE e.companion_id=$1 AND e.event_type='companion.snapshot'
            ORDER BY e.id DESC LIMIT 1
            "#,
        )
        .bind(companion_id)
        .fetch_optional(&self.pool)
        .await?)
    }

    pub async fn request_account_deletion(
        &self,
        owner_id: Uuid,
        delay: Duration,
    ) -> Result<AccountDeletionView, ApiError> {
        let delay = i64::try_from(delay.as_secs()).map_err(ApiError::internal)?;
        let row = sqlx::query(
            r#"
            INSERT INTO account_deletion_requests
                (owner_id,status,requested_at,execute_after,cancelled_at,completed_at)
            VALUES ($1,'pending',clock_timestamp(),clock_timestamp()+make_interval(secs=>$2),NULL,NULL)
            ON CONFLICT (owner_id) DO UPDATE SET
                status='pending',requested_at=clock_timestamp(),
                execute_after=clock_timestamp()+make_interval(secs=>$2),
                cancelled_at=NULL,completed_at=NULL
            WHERE account_deletion_requests.status IN ('pending','cancelled')
            RETURNING status,requested_at,execute_after,cancelled_at,completed_at
            "#,
        )
        .bind(owner_id)
        .bind(delay)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Conflict(
            "account deletion is already in progress or completed",
        ))?;
        Ok(account_deletion_from_row(&row))
    }

    pub async fn cancel_account_deletion(
        &self,
        owner_id: Uuid,
    ) -> Result<AccountDeletionView, ApiError> {
        let row = sqlx::query(
            r#"
            UPDATE account_deletion_requests SET status='cancelled',
                cancelled_at=clock_timestamp()
            WHERE owner_id=$1 AND status='pending' AND execute_after>clock_timestamp()
            RETURNING status,requested_at,execute_after,cancelled_at,completed_at
            "#,
        )
        .bind(owner_id)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Conflict("account deletion is not cancellable"))?;
        Ok(account_deletion_from_row(&row))
    }

    pub async fn account_deletion(
        &self,
        owner_id: Uuid,
    ) -> Result<Option<AccountDeletionView>, ApiError> {
        let row = sqlx::query(
            "SELECT status,requested_at,execute_after,cancelled_at,completed_at FROM account_deletion_requests WHERE owner_id=$1",
        )
        .bind(owner_id)
        .fetch_optional(&self.pool)
        .await?;
        Ok(row.as_ref().map(account_deletion_from_row))
    }

    pub async fn apply_retention(
        &self,
        event_days: i32,
        action_days: i32,
        audit_days: i32,
    ) -> Result<(), ApiError> {
        let mut tx = self.pool.begin().await?;
        sqlx::query("SELECT set_config('kitsu.retention_mode','on',true)")
            .execute(&mut *tx)
            .await?;
        sqlx::query("DELETE FROM browser_oauth_attempts WHERE expires_at < clock_timestamp()-interval '1 day'")
            .execute(&mut *tx).await?;
        sqlx::query("DELETE FROM browser_sessions WHERE expires_at < clock_timestamp()-interval '7 days' OR revoked_at < clock_timestamp()-interval '7 days'")
            .execute(&mut *tx).await?;
        sqlx::query(
            "DELETE FROM rate_limit_buckets WHERE expires_at < clock_timestamp()-interval '1 day'",
        )
        .execute(&mut *tx)
        .await?;
        // Bound account-free bootstrap rows without touching owner-created
        // relays or a first-use claim that can still safely resume. A claimed
        // companion or any durable audit foreign key also keeps the identity.
        sqlx::query(
            r#"
            WITH candidates AS MATERIALIZED (
              SELECT g.id AS gateway_id
              FROM gateways g
              JOIN mobile_relay_installations m ON m.gateway_id=g.id
              JOIN mobile_relay_credentials c
                ON c.installation_id=m.installation_id
              JOIN owners o ON o.id=m.owner_id
              WHERE o.issuer=$1 AND o.subject=m.installation_id::text
                AND c.activated_at IS NULL
                AND c.created_at<clock_timestamp()
                      -make_interval(secs => $2::double precision)
                AND NOT EXISTS (
                  SELECT 1 FROM gateway_companions gc
                  WHERE gc.gateway_id=g.id AND gc.unbound_at IS NULL
                )
                AND NOT EXISTS (
                  SELECT 1 FROM enrollment_challenges e
                  WHERE e.owner_id=m.owner_id AND (
                    e.status='claimed' OR
                    (e.status='pending' AND e.expires_at>clock_timestamp()) OR
                    (e.status='issuing' AND NOT (
                      e.provider_job_id IS NULL AND
                      e.provider_started_at IS NOT NULL AND
                      e.provider_started_at<=clock_timestamp()
                        -make_interval(secs => $3::double precision)
                    ))
                  )
                )
                AND NOT EXISTS (
                  SELECT 1 FROM audit_log a
                  WHERE a.actor_gateway_id=g.id OR a.actor_owner_id=m.owner_id
                )
              FOR UPDATE OF g,c,o SKIP LOCKED
            )
            DELETE FROM gateways g USING candidates d
            WHERE g.id=d.gateway_id
            "#,
        )
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .bind(DEVICE_RELAY_PENDING_RETENTION_SECONDS)
        .bind(PROVIDER_RETRY_WINDOW_SECONDS)
        .execute(&mut *tx)
        .await?;
        // The gateway delete cascades its relay installation and credential;
        // removing the now-orphaned synthetic owner also removes expired
        // enrollment challenges. Human/OIDC owners never match this issuer.
        sqlx::query(
            r#"
            DELETE FROM owners o
            WHERE o.issuer=$1
              AND NOT EXISTS (SELECT 1 FROM gateways g WHERE g.owner_id=o.id)
              AND NOT EXISTS (
                SELECT 1 FROM mobile_relay_installations m WHERE m.owner_id=o.id
              )
              AND NOT EXISTS (SELECT 1 FROM companions c WHERE c.owner_id=o.id)
              AND NOT EXISTS (
                SELECT 1 FROM audit_log a WHERE a.actor_owner_id=o.id
              )
            "#,
        )
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .execute(&mut *tx)
        .await?;
        sqlx::query("DELETE FROM device_requests WHERE received_at < clock_timestamp()-make_interval(days=>$1)")
            .bind(event_days).execute(&mut *tx).await?;
        sqlx::query("DELETE FROM device_events WHERE received_at < clock_timestamp()-make_interval(days=>$1)")
            .bind(event_days).execute(&mut *tx).await?;
        sqlx::query(
            "DELETE FROM peer_history WHERE updated_at < clock_timestamp()-make_interval(days=>$1)",
        )
        .bind(event_days)
        .execute(&mut *tx)
        .await?;
        sqlx::query("DELETE FROM remote_actions WHERE created_at < clock_timestamp()-make_interval(days=>$1) AND status NOT IN ('queued','delivered')")
            .bind(action_days).execute(&mut *tx).await?;
        sqlx::query(
            "DELETE FROM audit_log WHERE occurred_at < clock_timestamp()-make_interval(days=>$1)",
        )
        .bind(audit_days)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            "DELETE FROM public_contact_messages WHERE (status='resolved' AND resolved_at<clock_timestamp()-interval '30 days') OR (status='open' AND created_at<clock_timestamp()-interval '180 days')",
        )
        .execute(&mut *tx)
        .await?;
        tx.commit().await?;
        Ok(())
    }

    pub async fn create_public_contact(
        &self,
        category: &str,
        reply_contact: &str,
        message: &str,
        source_address_digest: &[u8; 32],
    ) -> Result<Uuid, ApiError> {
        let id = Uuid::new_v4();
        sqlx::query(
            r#"
            INSERT INTO public_contact_messages
                (id,category,reply_contact,message,source_address_digest)
            VALUES ($1,$2,$3,$4,$5)
            "#,
        )
        .bind(id)
        .bind(category)
        .bind(reply_contact)
        .bind(message)
        .bind(source_address_digest.as_slice())
        .execute(&self.pool)
        .await?;
        Ok(id)
    }

    pub async fn list_public_contacts(
        &self,
        limit: i64,
    ) -> Result<Vec<PublicContactView>, ApiError> {
        let rows = sqlx::query(
            r#"
            SELECT id,category,reply_contact,message,status,created_at,resolved_at
            FROM public_contact_messages
            ORDER BY CASE status WHEN 'open' THEN 0 ELSE 1 END,created_at DESC
            LIMIT $1
            "#,
        )
        .bind(limit.clamp(1, 200))
        .fetch_all(&self.pool)
        .await?;
        Ok(rows.iter().map(public_contact_from_row).collect())
    }

    pub async fn resolve_public_contact(&self, id: Uuid) -> Result<PublicContactView, ApiError> {
        let row = sqlx::query(
            r#"
            UPDATE public_contact_messages
            SET status='resolved',resolved_at=COALESCE(resolved_at,clock_timestamp())
            WHERE id=$1
            RETURNING id,category,reply_contact,message,status,created_at,resolved_at
            "#,
        )
        .bind(id)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::NotFound)?;
        Ok(public_contact_from_row(&row))
    }

    pub async fn prepare_due_account_deletions(&self) -> Result<Vec<DueAccountDeletion>, ApiError> {
        let mut tx = self.pool.begin().await?;
        let rows = sqlx::query(
            r#"
            SELECT r.owner_id,o.issuer,o.subject,r.identity_revoked_at IS NOT NULL AS identity_revoked
            FROM account_deletion_requests r JOIN owners o ON o.id=r.owner_id
            WHERE r.status IN ('pending','deleting')
              AND r.execute_after<=clock_timestamp()
            ORDER BY r.execute_after
            LIMIT 20
            FOR UPDATE OF r SKIP LOCKED
            "#,
        )
        .fetch_all(&mut *tx)
        .await?;
        let mut due = Vec::with_capacity(rows.len());
        for row in rows {
            let owner_id: Uuid = row.get("owner_id");
            let issuer: String = row.get("issuer");
            let subject: String = row.get("subject");
            let identity_revoked: bool = row.get("identity_revoked");
            let digest = oidc_subject_digest(&issuer, &subject);
            sqlx::query(
                "INSERT INTO deleted_oidc_subjects (issuer_subject_sha256) VALUES ($1) ON CONFLICT DO NOTHING",
            )
            .bind(digest.as_slice())
            .execute(&mut *tx)
            .await?;
            sqlx::query(
                "UPDATE account_deletion_requests SET status='deleting' WHERE owner_id=$1 AND status IN ('pending','deleting')",
            )
            .bind(owner_id)
            .execute(&mut *tx)
            .await?;
            due.push(DueAccountDeletion {
                owner_id,
                issuer,
                subject,
                identity_revoked,
            });
        }
        tx.commit().await?;
        Ok(due)
    }

    pub async fn mark_account_identity_revoked(&self, owner_id: Uuid) -> Result<(), ApiError> {
        let changed = sqlx::query(
            "UPDATE account_deletion_requests SET identity_revoked_at=COALESCE(identity_revoked_at,clock_timestamp()) WHERE owner_id=$1 AND status='deleting'",
        )
        .bind(owner_id)
        .execute(&self.pool)
        .await?;
        if changed.rows_affected() != 1 {
            return Err(ApiError::Conflict("account deletion state changed"));
        }
        Ok(())
    }

    pub async fn process_account_deletion(&self, owner_id: Uuid) -> Result<(), ApiError> {
        let mut tx = self.pool.begin().await?;
        sqlx::query("SELECT set_config('kitsu.retention_mode','on',true)")
            .execute(&mut *tx)
            .await?;
        let eligible: bool = sqlx::query_scalar(
            "SELECT EXISTS(SELECT 1 FROM account_deletion_requests WHERE owner_id=$1 AND status='deleting' AND identity_revoked_at IS NOT NULL)",
        )
        .bind(owner_id)
        .fetch_one(&mut *tx)
        .await?;
        if !eligible {
            return Err(ApiError::Conflict("account identity is not revoked"));
        }
        sqlx::query("DELETE FROM browser_sessions WHERE owner_id=$1")
            .bind(owner_id)
            .execute(&mut *tx)
            .await?;
        sqlx::query("DELETE FROM enrollment_challenges WHERE owner_id=$1")
            .bind(owner_id)
            .execute(&mut *tx)
            .await?;
        sqlx::query("DELETE FROM gateway_bootstraps WHERE owner_id=$1")
            .bind(owner_id)
            .execute(&mut *tx)
            .await?;
        sqlx::query("DELETE FROM gateway_lan_profiles WHERE gateway_id IN (SELECT id FROM gateways WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("DELETE FROM remote_actions WHERE owner_id=$1")
            .bind(owner_id)
            .execute(&mut *tx)
            .await?;
        sqlx::query("DELETE FROM device_events WHERE companion_id IN (SELECT id FROM companions WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("DELETE FROM peer_history WHERE companion_id IN (SELECT id FROM companions WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("DELETE FROM owner_companion_cursors WHERE owner_id=$1")
            .bind(owner_id)
            .execute(&mut *tx)
            .await?;
        sqlx::query("DELETE FROM companion_state_projections WHERE companion_id IN (SELECT id FROM companions WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("DELETE FROM device_requests WHERE companion_id IN (SELECT id FROM companions WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("DELETE FROM device_sequences WHERE companion_id IN (SELECT id FROM companions WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("DELETE FROM companion_secret_versions WHERE companion_id IN (SELECT id FROM companions WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("UPDATE companion_certificates SET status='revoked',revoked_at=COALESCE(revoked_at,clock_timestamp()) WHERE companion_id IN (SELECT id FROM companions WHERE owner_id=$1) AND status<>'revoked'").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("UPDATE gateway_certificates SET status='revoked',revoked_at=COALESCE(revoked_at,clock_timestamp()) WHERE gateway_id IN (SELECT id FROM gateways WHERE owner_id=$1) AND status<>'revoked'").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("UPDATE gateways SET status='revoked',revoked_at=COALESCE(revoked_at,clock_timestamp()),last_proof_at=NULL WHERE owner_id=$1").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("UPDATE companions SET status='retired',hardware_uid='deleted:'||id::text,display_name='Deleted companion',last_seen_at=NULL,updated_at=clock_timestamp() WHERE owner_id=$1").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("UPDATE audit_log SET actor_owner_id=NULL,companion_id=NULL,details='{}'::jsonb WHERE actor_owner_id=$1 OR companion_id IN (SELECT id FROM companions WHERE owner_id=$1)").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("UPDATE owners SET subject='deleted:'||id::text,email=NULL,display_name=NULL,last_login_at=clock_timestamp() WHERE id=$1").bind(owner_id).execute(&mut *tx).await?;
        sqlx::query("UPDATE account_deletion_requests SET status='completed',completed_at=clock_timestamp() WHERE owner_id=$1").bind(owner_id).execute(&mut *tx).await?;
        tx.commit().await?;
        Ok(())
    }

    pub async fn revoked_certificates(&self) -> Result<Vec<RevokedCertificate>, ApiError> {
        let rows = sqlx::query(
            r#"
            SELECT serial_hex, revoked_at, provider_id
            FROM gateway_certificates
            WHERE status='revoked' AND revoked_at IS NOT NULL
              AND serial_hex IS NOT NULL AND provider_id IS NOT NULL
            UNION ALL
            SELECT serial_hex, revoked_at, provider_id
            FROM companion_certificates
            WHERE status='revoked' AND revoked_at IS NOT NULL
            ORDER BY provider_id, serial_hex
            "#,
        )
        .fetch_all(&self.pool)
        .await?;
        Ok(rows
            .into_iter()
            .map(|row| RevokedCertificate {
                serial_hex: row.get("serial_hex"),
                revoked_at: row.get("revoked_at"),
                provider_id: row.get("provider_id"),
            })
            .collect())
    }

    pub async fn upsert_owner(&self, principal: &OidcPrincipal) -> Result<Owner, ApiError> {
        let deleted = oidc_subject_digest(&principal.issuer, &principal.subject);
        if sqlx::query_scalar::<_, bool>(
            "SELECT EXISTS(SELECT 1 FROM deleted_oidc_subjects WHERE issuer_subject_sha256=$1)",
        )
        .bind(deleted.as_slice())
        .fetch_one(&self.pool)
        .await?
        {
            return Err(ApiError::Forbidden);
        }
        let id = Uuid::new_v4();
        let row = sqlx::query(
            r#"
            INSERT INTO owners (id, issuer, subject, email, display_name)
            VALUES ($1, $2, $3, $4, $5)
            ON CONFLICT (issuer, subject) DO UPDATE SET
                email = EXCLUDED.email,
                display_name = EXCLUDED.display_name,
                last_login_at = clock_timestamp()
            RETURNING id, issuer, subject
            "#,
        )
        .bind(id)
        .bind(&principal.issuer)
        .bind(&principal.subject)
        .bind(&principal.email)
        .bind(&principal.display_name)
        .fetch_one(&self.pool)
        .await?;
        Ok(owner_from_row(&row))
    }

    #[allow(clippy::too_many_arguments)]
    pub async fn create_browser_oauth_attempt(
        &self,
        id: Uuid,
        state_digest: &[u8; 32],
        state_cookie_digest: &[u8; 32],
        nonce_digest: &[u8; 32],
        pkce_nonce: &[u8; 12],
        pkce_ciphertext: &[u8],
        return_url: &str,
        expires_at: DateTime<Utc>,
    ) -> Result<(), ApiError> {
        sqlx::query(
            r#"
            INSERT INTO browser_oauth_attempts
                (id, state_digest, state_cookie_digest, nonce_digest,
                 pkce_nonce, pkce_ciphertext, return_url, expires_at)
            VALUES ($1,$2,$3,$4,$5,$6,$7,$8)
            "#,
        )
        .bind(id)
        .bind(state_digest.as_slice())
        .bind(state_cookie_digest.as_slice())
        .bind(nonce_digest.as_slice())
        .bind(pkce_nonce.as_slice())
        .bind(pkce_ciphertext)
        .bind(return_url)
        .bind(expires_at)
        .execute(&self.pool)
        .await?;
        Ok(())
    }

    pub async fn consume_browser_oauth_attempt(
        &self,
        state_digest: &[u8; 32],
        cookie_digest: &[u8; 32],
    ) -> Result<BrowserAttempt, ApiError> {
        let row = sqlx::query(
            r#"
            UPDATE browser_oauth_attempts
            SET consumed_at = clock_timestamp()
            WHERE state_digest = $1 AND state_cookie_digest = $2
              AND consumed_at IS NULL AND expires_at > clock_timestamp()
            RETURNING id, pkce_nonce, pkce_ciphertext, nonce_digest, return_url
            "#,
        )
        .bind(state_digest.as_slice())
        .bind(cookie_digest.as_slice())
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        Ok(BrowserAttempt {
            id: row.get("id"),
            pkce_nonce: exact_bytes(row.get::<Vec<u8>, _>("pkce_nonce"))?,
            pkce_ciphertext: row.get("pkce_ciphertext"),
            nonce_digest: exact_bytes(row.get::<Vec<u8>, _>("nonce_digest"))?,
            return_url: row.get("return_url"),
        })
    }

    pub async fn create_browser_session(
        &self,
        owner_id: Uuid,
        token_digest: &[u8; 32],
        csrf_digest: &[u8; 32],
        expires_at: DateTime<Utc>,
        user_agent_digest: Option<&[u8; 32]>,
    ) -> Result<Uuid, ApiError> {
        let id = Uuid::new_v4();
        sqlx::query(
            r#"
            INSERT INTO browser_sessions
                (id, owner_id, token_digest, csrf_digest, expires_at, user_agent_digest)
            VALUES ($1,$2,$3,$4,$5,$6)
            "#,
        )
        .bind(id)
        .bind(owner_id)
        .bind(token_digest.as_slice())
        .bind(csrf_digest.as_slice())
        .bind(expires_at)
        .bind(user_agent_digest.map(|digest| digest.as_slice()))
        .execute(&self.pool)
        .await?;
        Ok(id)
    }

    pub async fn browser_session(
        &self,
        token_digest: &[u8; 32],
    ) -> Result<BrowserSession, ApiError> {
        let row = sqlx::query(
            r#"
            UPDATE browser_sessions s
            SET last_seen_at = clock_timestamp()
            FROM owners o
            WHERE s.owner_id = o.id AND s.token_digest = $1
              AND s.revoked_at IS NULL AND s.expires_at > clock_timestamp()
            RETURNING s.id, s.csrf_digest, s.expires_at,
                      o.id AS owner_id, o.issuer, o.subject
            "#,
        )
        .bind(token_digest.as_slice())
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        Ok(BrowserSession {
            id: row.get("id"),
            csrf_digest: exact_bytes(row.get::<Vec<u8>, _>("csrf_digest"))?,
            expires_at: row.get("expires_at"),
            owner: Owner {
                id: row.get("owner_id"),
                issuer: row.get("issuer"),
                subject: row.get("subject"),
            },
        })
    }

    pub async fn revoke_browser_session(&self, session_id: Uuid) -> Result<(), ApiError> {
        sqlx::query("UPDATE browser_sessions SET revoked_at = clock_timestamp() WHERE id = $1")
            .bind(session_id)
            .execute(&self.pool)
            .await?;
        Ok(())
    }

    pub async fn create_ws_ticket(
        &self,
        session_id: Uuid,
        digest: &[u8; 32],
    ) -> Result<(), ApiError> {
        sqlx::query(
            r#"
            INSERT INTO browser_ws_tickets (ticket_digest, session_id, expires_at)
            VALUES ($1,$2,clock_timestamp() + interval '30 seconds')
            "#,
        )
        .bind(digest.as_slice())
        .bind(session_id)
        .execute(&self.pool)
        .await?;
        Ok(())
    }

    pub async fn consume_ws_ticket(&self, digest: &[u8; 32]) -> Result<Owner, ApiError> {
        let row = sqlx::query(
            r#"
            UPDATE browser_ws_tickets t
            SET consumed_at = clock_timestamp()
            FROM browser_sessions s, owners o
            WHERE t.ticket_digest = $1 AND t.session_id = s.id AND s.owner_id = o.id
              AND t.consumed_at IS NULL AND t.expires_at > clock_timestamp()
              AND s.revoked_at IS NULL AND s.expires_at > clock_timestamp()
            RETURNING o.id, o.issuer, o.subject
            "#,
        )
        .bind(digest.as_slice())
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        Ok(owner_from_row(&row))
    }

    pub async fn check_rate_limit(
        &self,
        scope: &str,
        subject_digest: &[u8; 32],
        limit: i32,
        window_seconds: i64,
    ) -> Result<(), ApiError> {
        let count: i32 = sqlx::query_scalar(
            r#"
            INSERT INTO rate_limit_buckets
                (scope, subject_hash, window_started_at, request_count, expires_at)
            VALUES ($1,$2,clock_timestamp(),1,
                    clock_timestamp() + make_interval(secs => $3::double precision))
            ON CONFLICT (scope, subject_hash) DO UPDATE SET
                window_started_at = CASE
                    WHEN rate_limit_buckets.expires_at <= clock_timestamp()
                    THEN clock_timestamp() ELSE rate_limit_buckets.window_started_at END,
                request_count = CASE
                    WHEN rate_limit_buckets.expires_at <= clock_timestamp()
                    THEN 1 ELSE rate_limit_buckets.request_count + 1 END,
                expires_at = CASE
                    WHEN rate_limit_buckets.expires_at <= clock_timestamp()
                    THEN clock_timestamp() + make_interval(secs => $3::double precision)
                    ELSE rate_limit_buckets.expires_at END
            RETURNING request_count
            "#,
        )
        .bind(scope)
        .bind(subject_digest.as_slice())
        .bind(window_seconds)
        .fetch_one(&self.pool)
        .await?;
        if count > limit {
            return Err(ApiError::RateLimited);
        }
        Ok(())
    }

    pub async fn create_enrollment(
        &self,
        owner_id: Uuid,
        hardware_uid: &str,
        display_name: &str,
        token_digest: &[u8; 32],
        ttl: Duration,
    ) -> Result<EnrollmentView, ApiError> {
        validate_identity_text(hardware_uid, display_name)?;
        let id = Uuid::new_v4();
        let ttl_seconds = i64::try_from(ttl.as_secs()).map_err(ApiError::internal)?;
        let mut tx = self.pool.begin().await?;
        let expires_at: DateTime<Utc> = sqlx::query_scalar(
            r#"
            INSERT INTO enrollment_challenges
                (id, owner_id, token_digest, hardware_uid, display_name, expires_at)
            VALUES ($1,$2,$3,$4,$5,
                    clock_timestamp()+make_interval(secs => $6::double precision))
            RETURNING expires_at
            "#,
        )
        .bind(id)
        .bind(owner_id)
        .bind(token_digest.as_slice())
        .bind(hardware_uid)
        .bind(display_name)
        .bind(ttl_seconds)
        .fetch_one(&mut *tx)
        .await
        .map_err(map_conflict)?;
        insert_audit(
            &mut tx,
            "owner",
            Some(owner_id),
            None,
            None,
            "enrollment.created",
            "enrollment",
            id.to_string(),
            json!({"hardware_uid_sha256": hex::encode(sha256(hardware_uid.as_bytes()))}),
        )
        .await?;
        tx.commit().await?;
        Ok(EnrollmentView {
            id,
            hardware_uid: hardware_uid.to_owned(),
            display_name: display_name.to_owned(),
            status: "pending".into(),
            expires_at,
        })
    }

    pub async fn begin_enrollment_issuance(
        &self,
        enrollment_id: Uuid,
        token_digest: &[u8; 32],
        request_sha256: &[u8; 32],
        hardware_uid: &str,
        gateway: &Gateway,
    ) -> Result<BeginEnrollmentIssuance, ApiError> {
        let mut tx = self.pool.begin().await?;
        let row = sqlx::query(
            r#"
            SELECT id,owner_id,hardware_uid,display_name,status::text,
                   expires_at<=clock_timestamp() AS expired,
                   claim_request_sha256,issuance_started_at,reserved_companion_id,
                   provider_job_id,
                   companion_id,claimed_gateway_id,
                   COALESCE(issuance_started_at>clock_timestamp()
                       -make_interval(secs => $3::double precision),false)
                       AS lease_live,
                   provider_started_at,
                   COALESCE(provider_started_at>clock_timestamp()
                       -make_interval(secs => $4::double precision),false)
                       AS provider_window_live
            FROM enrollment_challenges
            WHERE id=$1 AND token_digest=$2
            FOR UPDATE
            "#,
        )
        .bind(enrollment_id)
        .bind(token_digest.as_slice())
        .bind(ISSUANCE_LEASE_SECONDS)
        .bind(PROVIDER_RETRY_WINDOW_SECONDS)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Expired)?;
        let owner_id: Uuid = row.get("owner_id");
        if owner_id != gateway.owner_id {
            return Err(ApiError::Forbidden);
        }
        if row.get::<String, _>("hardware_uid") != hardware_uid {
            return Err(ApiError::Conflict("hardware UID does not match enrollment"));
        }
        let status: String = row.get("status");
        let provider_started_at = row.get::<Option<DateTime<Utc>>, _>("provider_started_at");
        // Expiry prevents a new claim, but it must not orphan a certificate
        // whose external-provider boundary was crossed while the claim was
        // valid.  Such a row is already bound to the exact request digest and
        // reserved identity, so only that request may resume it.
        if row.get::<bool, _>("expired")
            && status != "claimed"
            && !(status == "issuing" && provider_started_at.is_some())
        {
            sqlx::query(
                "UPDATE enrollment_challenges SET status='expired' WHERE id=$1 AND status IN ('pending','issuing')",
            )
            .bind(enrollment_id)
            .execute(&mut *tx)
            .await?;
            tx.commit().await?;
            return Err(ApiError::Expired);
        }
        let stored_request = row
            .get::<Option<Vec<u8>>, _>("claim_request_sha256")
            .map(exact_bytes::<32>)
            .transpose()?;
        if stored_request.is_some_and(|stored| stored != *request_sha256) {
            return Err(ApiError::Conflict(
                "enrollment token was bound to another request",
            ));
        }
        if status == "claimed" {
            let completed =
                load_completed_enrollment(&mut tx, enrollment_id, request_sha256).await?;
            tx.commit().await?;
            return Ok(BeginEnrollmentIssuance::Completed(completed));
        }
        if status != "pending" && status != "issuing" {
            return Err(ApiError::Expired);
        }
        if status == "issuing" && row.get::<bool, _>("lease_live") {
            return Err(ApiError::Conflict("enrollment issuance is in progress"));
        }
        let provider_job_id: Option<String> = row.get("provider_job_id");
        if status == "issuing"
            && provider_job_id.is_none()
            && provider_started_at.is_some()
            && !row.get::<bool, _>("provider_window_live")
        {
            // The previous process may have crossed the external CA boundary
            // without receiving/persisting its job ARN. Reissuing after the
            // provider's idempotency window could mint an orphan certificate,
            // so this ambiguous state is deliberately operator-recoverable only.
            return Err(ApiError::ReplacementRequired);
        }
        let companion_id = row
            .get::<Option<Uuid>, _>("reserved_companion_id")
            .unwrap_or_else(Uuid::new_v4);
        let issuance_id = Uuid::new_v4();
        sqlx::query(
            r#"
            UPDATE enrollment_challenges SET status='issuing',
                claim_request_sha256=COALESCE(claim_request_sha256,$2),
                issuance_id=$3,issuance_started_at=clock_timestamp(),
                reserved_companion_id=$4
            WHERE id=$1
            "#,
        )
        .bind(enrollment_id)
        .bind(request_sha256.as_slice())
        .bind(issuance_id)
        .bind(companion_id)
        .execute(&mut *tx)
        .await?;
        let reserved = ReservedEnrollment {
            id: enrollment_id,
            owner_id,
            hardware_uid: row.get("hardware_uid"),
            display_name: row.get("display_name"),
            gateway_id: gateway.id,
            companion_id,
            key_version: 1,
            issuance_id,
            request_sha256: *request_sha256,
            provider_job_id,
        };
        tx.commit().await?;
        Ok(BeginEnrollmentIssuance::Issue(reserved))
    }

    pub async fn abort_enrollment_issuance(
        &self,
        enrollment_id: Uuid,
        issuance_id: Uuid,
    ) -> Result<(), ApiError> {
        sqlx::query(
            r#"
            UPDATE enrollment_challenges SET
                status=CASE WHEN expires_at<=clock_timestamp()
                    THEN 'expired'::enrollment_status
                    ELSE 'pending'::enrollment_status END,
                issuance_id=NULL,
                issuance_started_at=NULL,
                provider_started_at=NULL,
                provider_ambiguous=FALSE
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
              AND provider_job_id IS NULL
            "#,
        )
        .bind(enrollment_id)
        .bind(issuance_id)
        .execute(&self.pool)
        .await?;
        Ok(())
    }

    pub async fn record_enrollment_provider_job(
        &self,
        reserved: &ReservedEnrollment,
        provider_job_id: &str,
    ) -> Result<(), ApiError> {
        let stored: String = sqlx::query_scalar(
            r#"
            UPDATE enrollment_challenges
            SET provider_job_id=COALESCE(provider_job_id,$3),provider_ambiguous=FALSE
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
            RETURNING provider_job_id
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.issuance_id)
        .bind(provider_job_id)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Conflict("enrollment issuance lease was lost"))?;
        if stored != provider_job_id {
            return Err(ApiError::Conflict("enrollment CA job changed"));
        }
        Ok(())
    }

    pub async fn mark_enrollment_provider_attempt(
        &self,
        reserved: &ReservedEnrollment,
    ) -> Result<(), ApiError> {
        let changed = sqlx::query(
            r#"
            UPDATE enrollment_challenges SET
                provider_started_at=COALESCE(provider_started_at,clock_timestamp()),
                provider_ambiguous=TRUE
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
              AND (provider_started_at IS NULL OR provider_started_at>
                   clock_timestamp()-make_interval(secs => $3::double precision))
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.issuance_id)
        .bind(PROVIDER_RETRY_WINDOW_SECONDS)
        .execute(&self.pool)
        .await?;
        if changed.rows_affected() != 1 {
            return Err(ApiError::Conflict("enrollment issuance lease was lost"));
        }
        Ok(())
    }

    pub async fn release_enrollment_issuance(
        &self,
        enrollment_id: Uuid,
        issuance_id: Uuid,
    ) -> Result<(), ApiError> {
        sqlx::query(
            r#"
            UPDATE enrollment_challenges SET
                status=CASE WHEN provider_started_at IS NOT NULL
                    THEN 'issuing'::enrollment_status
                    WHEN expires_at<=clock_timestamp() THEN 'expired'::enrollment_status
                    ELSE 'pending'::enrollment_status END,
                issuance_id=NULL,issuance_started_at=NULL,
                provider_ambiguous=CASE WHEN provider_started_at IS NULL
                    THEN FALSE ELSE provider_job_id IS NULL END
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
            "#,
        )
        .bind(enrollment_id)
        .bind(issuance_id)
        .execute(&self.pool)
        .await?;
        Ok(())
    }

    pub async fn complete_enrollment_issuance(
        &self,
        reserved: &ReservedEnrollment,
        secret: &NewSecretRecord,
        certificate: &ValidatedCertificate,
        sealed: &SealedSecret,
    ) -> Result<EnrollmentIssuanceResult, ApiError> {
        if secret.companion_id != reserved.companion_id
            || secret.key_version != reserved.key_version
            || sealed.ciphertext.len() != 48
            || certificate.san_uri != format!("urn:kitsu:companion:{}", reserved.companion_id)
        {
            return Err(ApiError::Invalid("inconsistent enrollment result"));
        }
        let mut tx = self.pool.begin().await?;
        let locked = sqlx::query(
            r#"
            SELECT owner_id,hardware_uid,claim_request_sha256,provider_job_id
            FROM enrollment_challenges
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
              AND reserved_companion_id=$3
              AND provider_job_id IS NOT NULL AND NOT provider_ambiguous
            FOR UPDATE
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.issuance_id)
        .bind(reserved.companion_id)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Conflict("enrollment issuance lease was lost"))?;
        if locked.get::<Uuid, _>("owner_id") != reserved.owner_id
            || locked.get::<String, _>("hardware_uid") != reserved.hardware_uid
            || exact_bytes::<32>(locked.get::<Vec<u8>, _>("claim_request_sha256"))?
                != reserved.request_sha256
            || locked.get::<String, _>("provider_job_id") != certificate.provider_id
        {
            return Err(ApiError::Conflict("enrollment changed"));
        }
        let gateway_owner: Option<Uuid> = sqlx::query_scalar(
            "SELECT owner_id FROM gateways WHERE id=$1 AND status='active' FOR SHARE",
        )
        .bind(reserved.gateway_id)
        .fetch_optional(&mut *tx)
        .await?;
        if gateway_owner != Some(reserved.owner_id) {
            return Err(ApiError::Forbidden);
        }

        sqlx::query(
            r#"
            INSERT INTO companions
                (id,owner_id,hardware_uid,display_name,active_key_version)
            VALUES ($1,$2,$3,$4,$5)
            "#,
        )
        .bind(secret.companion_id)
        .bind(reserved.owner_id)
        .bind(&reserved.hardware_uid)
        .bind(&reserved.display_name)
        .bind(i32::try_from(secret.key_version).map_err(ApiError::internal)?)
        .execute(&mut *tx)
        .await
        .map_err(map_conflict)?;
        sqlx::query(
            r#"
            INSERT INTO companion_secret_versions
                (companion_id,key_version,kms_key_id,wrapped_dek,
                 secret_nonce,secret_ciphertext)
            VALUES ($1,$2,$3,$4,$5,$6)
            "#,
        )
        .bind(secret.companion_id)
        .bind(i32::try_from(secret.key_version).map_err(ApiError::internal)?)
        .bind(&secret.kms_key_id)
        .bind(&secret.wrapped_dek)
        .bind(secret.encrypted.nonce.as_slice())
        .bind(&secret.encrypted.ciphertext)
        .execute(&mut *tx)
        .await?;
        sqlx::query("INSERT INTO device_sequences (companion_id,key_version) VALUES ($1,$2)")
            .bind(secret.companion_id)
            .bind(i32::try_from(secret.key_version).map_err(ApiError::internal)?)
            .execute(&mut *tx)
            .await?;
        sqlx::query("INSERT INTO gateway_companions (gateway_id,companion_id) VALUES ($1,$2)")
            .bind(reserved.gateway_id)
            .bind(secret.companion_id)
            .execute(&mut *tx)
            .await?;
        let certificate_id = Uuid::new_v4();
        sqlx::query(
            r#"
            INSERT INTO companion_certificates
                (id,companion_id,key_version,serial_hex,certificate_sha256,
                 subject_public_key_sha256,san_uri,leaf_der,chain_der,provider_id,
                 valid_after,valid_until)
            VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)
            "#,
        )
        .bind(certificate_id)
        .bind(secret.companion_id)
        .bind(i32::try_from(secret.key_version).map_err(ApiError::internal)?)
        .bind(&certificate.serial_hex)
        .bind(certificate.fingerprint_sha256.as_slice())
        .bind(certificate.subject_public_key_sha256.as_slice())
        .bind(&certificate.san_uri)
        .bind(&certificate.leaf_der)
        .bind(&certificate.chain_der)
        .bind(&certificate.provider_id)
        .bind(certificate.valid_after)
        .bind(certificate.valid_until)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE enrollment_challenges SET status='claimed',claimed_at=clock_timestamp(),
                claimed_gateway_id=$2,companion_id=$3,device_certificate_id=$4,
                hpke_enc=$5,hpke_ciphertext=$6,issuance_id=NULL
            WHERE id=$1
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.gateway_id)
        .bind(secret.companion_id)
        .bind(certificate_id)
        .bind(sealed.enc.as_slice())
        .bind(sealed.ciphertext.as_slice())
        .execute(&mut *tx)
        .await?;
        insert_audit(
            &mut tx,
            "gateway",
            Some(reserved.owner_id),
            Some(reserved.gateway_id),
            Some(secret.companion_id),
            "enrollment.claimed",
            "companion",
            secret.companion_id.to_string(),
            json!({
                "key_version": secret.key_version,
                "certificate_id": certificate_id,
                "certificate_sha256": hex::encode(certificate.fingerprint_sha256)
            }),
        )
        .await?;
        tx.commit().await?;
        Ok(EnrollmentIssuanceResult {
            companion_id: secret.companion_id,
            gateway_id: reserved.gateway_id,
            key_version: secret.key_version,
            certificate_der: certificate.leaf_der.clone(),
            certificate_chain_der: certificate.chain_der.clone(),
            hpke_enc: sealed.enc,
            hpke_ciphertext: sealed
                .ciphertext
                .as_slice()
                .try_into()
                .map_err(|_| ApiError::Unavailable)?,
        })
    }

    pub async fn create_gateway_bootstrap(
        &self,
        owner_id: Uuid,
        display_name: &str,
        token_digest: &[u8; 32],
        ttl: Duration,
    ) -> Result<GatewayBootstrapView, ApiError> {
        validate_display_name(display_name)?;
        let id = Uuid::new_v4();
        let ttl_seconds = i64::try_from(ttl.as_secs()).map_err(ApiError::internal)?;
        let mut tx = self.pool.begin().await?;
        let expires_at: DateTime<Utc> = sqlx::query_scalar(
            r#"
            INSERT INTO gateway_bootstraps
                (id,owner_id,display_name,token_digest,expires_at)
            VALUES ($1,$2,$3,$4,
                    clock_timestamp()+make_interval(secs => $5::double precision))
            RETURNING expires_at
            "#,
        )
        .bind(id)
        .bind(owner_id)
        .bind(display_name)
        .bind(token_digest.as_slice())
        .bind(ttl_seconds)
        .fetch_one(&mut *tx)
        .await?;
        insert_audit(
            &mut tx,
            "owner",
            Some(owner_id),
            None,
            None,
            "gateway.bootstrap_created",
            "gateway_bootstrap",
            id.to_string(),
            json!({}),
        )
        .await?;
        tx.commit().await?;
        Ok(GatewayBootstrapView {
            id,
            display_name: display_name.to_owned(),
            status: "pending".to_owned(),
            expires_at,
        })
    }

    pub async fn begin_gateway_bootstrap(
        &self,
        bootstrap_id: Uuid,
        token_digest: &[u8; 32],
        request_sha256: &[u8; 32],
    ) -> Result<BeginGatewayBootstrap, ApiError> {
        let mut tx = self.pool.begin().await?;
        let row = sqlx::query(
            r#"
            SELECT owner_id,display_name,status::text,
                   expires_at<=clock_timestamp() AS expired,
                   claim_request_sha256,issuance_started_at,reserved_gateway_id,
                   provider_job_id,
                   COALESCE(issuance_started_at>clock_timestamp()
                       -make_interval(secs => $3::double precision),false)
                       AS lease_live,
                   provider_started_at,
                   COALESCE(provider_started_at>clock_timestamp()
                       -make_interval(secs => $4::double precision),false)
                       AS provider_window_live
            FROM gateway_bootstraps
            WHERE id=$1 AND token_digest=$2
            FOR UPDATE
            "#,
        )
        .bind(bootstrap_id)
        .bind(token_digest.as_slice())
        .bind(ISSUANCE_LEASE_SECONDS)
        .bind(PROVIDER_RETRY_WINDOW_SECONDS)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Expired)?;
        let status: String = row.get("status");
        let provider_started_at = row.get::<Option<DateTime<Utc>>, _>("provider_started_at");
        if row.get::<bool, _>("expired")
            && status != "claimed"
            && !(status == "issuing" && provider_started_at.is_some())
        {
            sqlx::query(
                "UPDATE gateway_bootstraps SET status='expired' WHERE id=$1 AND status IN ('pending','issuing')",
            )
            .bind(bootstrap_id)
            .execute(&mut *tx)
            .await?;
            tx.commit().await?;
            return Err(ApiError::Expired);
        }
        let stored_request = row
            .get::<Option<Vec<u8>>, _>("claim_request_sha256")
            .map(exact_bytes::<32>)
            .transpose()?;
        if stored_request.is_some_and(|stored| stored != *request_sha256) {
            return Err(ApiError::Conflict(
                "bootstrap token was bound to another CSR",
            ));
        }
        if status == "claimed" {
            let completed =
                load_completed_gateway_bootstrap(&mut tx, bootstrap_id, request_sha256).await?;
            tx.commit().await?;
            return Ok(BeginGatewayBootstrap::Completed(completed));
        }
        if status != "pending" && status != "issuing" {
            return Err(ApiError::Expired);
        }
        if status == "issuing" && row.get::<bool, _>("lease_live") {
            return Err(ApiError::Conflict(
                "gateway certificate issuance is in progress",
            ));
        }
        let provider_job_id: Option<String> = row.get("provider_job_id");
        if status == "issuing"
            && provider_job_id.is_none()
            && provider_started_at.is_some()
            && !row.get::<bool, _>("provider_window_live")
        {
            return Err(ApiError::ReplacementRequired);
        }
        let gateway_id = row
            .get::<Option<Uuid>, _>("reserved_gateway_id")
            .unwrap_or_else(Uuid::new_v4);
        let issuance_id = Uuid::new_v4();
        sqlx::query(
            r#"
            UPDATE gateway_bootstraps SET status='issuing',
                claim_request_sha256=COALESCE(claim_request_sha256,$2),
                issuance_id=$3,issuance_started_at=clock_timestamp(),
                reserved_gateway_id=$4
            WHERE id=$1
            "#,
        )
        .bind(bootstrap_id)
        .bind(request_sha256.as_slice())
        .bind(issuance_id)
        .bind(gateway_id)
        .execute(&mut *tx)
        .await?;
        let reserved = ReservedGatewayBootstrap {
            id: bootstrap_id,
            owner_id: row.get("owner_id"),
            display_name: row.get("display_name"),
            gateway_id,
            issuance_id,
            request_sha256: *request_sha256,
            provider_job_id,
        };
        tx.commit().await?;
        Ok(BeginGatewayBootstrap::Issue(reserved))
    }

    pub async fn abort_gateway_bootstrap(
        &self,
        bootstrap_id: Uuid,
        issuance_id: Uuid,
    ) -> Result<(), ApiError> {
        sqlx::query(
            r#"
            UPDATE gateway_bootstraps SET
                status=CASE WHEN expires_at<=clock_timestamp()
                    THEN 'expired'::gateway_bootstrap_status
                    ELSE 'pending'::gateway_bootstrap_status END,
                issuance_id=NULL,
                issuance_started_at=NULL,
                provider_started_at=NULL,
                provider_ambiguous=FALSE
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
              AND provider_job_id IS NULL
            "#,
        )
        .bind(bootstrap_id)
        .bind(issuance_id)
        .execute(&self.pool)
        .await?;
        Ok(())
    }

    pub async fn record_gateway_provider_job(
        &self,
        reserved: &ReservedGatewayBootstrap,
        provider_job_id: &str,
    ) -> Result<(), ApiError> {
        let stored: String = sqlx::query_scalar(
            r#"
            UPDATE gateway_bootstraps
            SET provider_job_id=COALESCE(provider_job_id,$3),provider_ambiguous=FALSE
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
            RETURNING provider_job_id
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.issuance_id)
        .bind(provider_job_id)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Conflict(
            "gateway bootstrap issuance lease was lost",
        ))?;
        if stored != provider_job_id {
            return Err(ApiError::Conflict("gateway CA job changed"));
        }
        Ok(())
    }

    pub async fn mark_gateway_provider_attempt(
        &self,
        reserved: &ReservedGatewayBootstrap,
    ) -> Result<(), ApiError> {
        let changed = sqlx::query(
            r#"
            UPDATE gateway_bootstraps SET
                provider_started_at=COALESCE(provider_started_at,clock_timestamp()),
                provider_ambiguous=TRUE
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
              AND (provider_started_at IS NULL OR provider_started_at>
                   clock_timestamp()-make_interval(secs => $3::double precision))
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.issuance_id)
        .bind(PROVIDER_RETRY_WINDOW_SECONDS)
        .execute(&self.pool)
        .await?;
        if changed.rows_affected() != 1 {
            return Err(ApiError::Conflict(
                "gateway bootstrap issuance lease was lost",
            ));
        }
        Ok(())
    }

    pub async fn release_gateway_bootstrap(
        &self,
        bootstrap_id: Uuid,
        issuance_id: Uuid,
    ) -> Result<(), ApiError> {
        sqlx::query(
            r#"
            UPDATE gateway_bootstraps SET
                status=CASE WHEN provider_started_at IS NOT NULL
                    THEN 'issuing'::gateway_bootstrap_status
                    WHEN expires_at<=clock_timestamp() THEN 'expired'::gateway_bootstrap_status
                    ELSE 'pending'::gateway_bootstrap_status END,
                issuance_id=NULL,issuance_started_at=NULL,
                provider_ambiguous=CASE WHEN provider_started_at IS NULL
                    THEN FALSE ELSE provider_job_id IS NULL END
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
            "#,
        )
        .bind(bootstrap_id)
        .bind(issuance_id)
        .execute(&self.pool)
        .await?;
        Ok(())
    }

    pub async fn complete_gateway_bootstrap(
        &self,
        reserved: &ReservedGatewayBootstrap,
        certificate: &ValidatedCertificate,
    ) -> Result<GatewayBootstrapResult, ApiError> {
        if certificate.san_uri != format!("urn:kitsu:gateway:{}", reserved.gateway_id) {
            return Err(ApiError::Invalid("inconsistent gateway certificate"));
        }
        let mut tx = self.pool.begin().await?;
        let locked = sqlx::query(
            r#"
            SELECT owner_id,claim_request_sha256,provider_job_id
            FROM gateway_bootstraps
            WHERE id=$1 AND status='issuing' AND issuance_id=$2
              AND reserved_gateway_id=$3
              AND provider_job_id IS NOT NULL AND NOT provider_ambiguous
            FOR UPDATE
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.issuance_id)
        .bind(reserved.gateway_id)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Conflict(
            "gateway bootstrap issuance lease was lost",
        ))?;
        if locked.get::<Uuid, _>("owner_id") != reserved.owner_id
            || exact_bytes::<32>(locked.get::<Vec<u8>, _>("claim_request_sha256"))?
                != reserved.request_sha256
            || locked.get::<String, _>("provider_job_id") != certificate.provider_id
        {
            return Err(ApiError::Conflict("gateway bootstrap changed"));
        }
        sqlx::query(
            r#"
            INSERT INTO gateways (id,owner_id,status,activated_at)
            VALUES ($1,$2,'active',clock_timestamp())
            "#,
        )
        .bind(reserved.gateway_id)
        .bind(reserved.owner_id)
        .execute(&mut *tx)
        .await?;
        let certificate_id = Uuid::new_v4();
        sqlx::query(
            r#"
            INSERT INTO gateway_certificates
                (id,gateway_id,certificate_sha256,spiffe_id,status,valid_after,
                 valid_until,activated_at,serial_hex,subject_public_key_sha256,
                 san_uri,leaf_der,chain_der,provider_id)
            VALUES ($1,$2,$3,$4,'active',$5,$6,clock_timestamp(),$7,$8,$9,$10,$11,$12)
            "#,
        )
        .bind(certificate_id)
        .bind(reserved.gateway_id)
        .bind(certificate.fingerprint_sha256.as_slice())
        .bind(Option::<String>::None)
        .bind(certificate.valid_after)
        .bind(certificate.valid_until)
        .bind(&certificate.serial_hex)
        .bind(certificate.subject_public_key_sha256.as_slice())
        .bind(&certificate.san_uri)
        .bind(&certificate.leaf_der)
        .bind(&certificate.chain_der)
        .bind(&certificate.provider_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE gateway_bootstraps SET status='claimed',claimed_at=clock_timestamp(),
                gateway_id=$2,certificate_id=$3,issuance_id=NULL
            WHERE id=$1
            "#,
        )
        .bind(reserved.id)
        .bind(reserved.gateway_id)
        .bind(certificate_id)
        .execute(&mut *tx)
        .await?;
        insert_audit(
            &mut tx,
            "owner",
            Some(reserved.owner_id),
            Some(reserved.gateway_id),
            None,
            "gateway.bootstrap_claimed",
            "gateway",
            reserved.gateway_id.to_string(),
            json!({
                "certificate_id": certificate_id,
                "certificate_sha256": hex::encode(certificate.fingerprint_sha256)
            }),
        )
        .await?;
        tx.commit().await?;
        Ok(GatewayBootstrapResult {
            gateway_id: reserved.gateway_id,
            certificate_der: certificate.leaf_der.clone(),
            certificate_chain_der: certificate.chain_der.clone(),
        })
    }

    pub async fn gateway_by_mtls(&self, mtls: &MtlsIdentity) -> Result<Gateway, ApiError> {
        let mut tx = self.pool.begin().await?;
        // Rotation overlap is enforced at authentication time, so an instance
        // restart or a missed housekeeping tick can never extend an old cert.
        sqlx::query(
            r#"
            UPDATE gateway_certificates c
            SET status='revoked', revoked_at=COALESCE(revoked_at,clock_timestamp())
            FROM gateway_certificate_rotations r
            WHERE c.id=r.old_certificate_id
              AND c.certificate_sha256=$1
              AND c.status='active' AND r.status='activated'
              AND r.overlap_ends_at<=clock_timestamp()
            "#,
        )
        .bind(mtls.certificate_sha256.as_slice())
        .execute(&mut *tx)
        .await?;
        let row = sqlx::query(
            r#"
            WITH certificate_proof AS (
                UPDATE gateway_certificates c
                SET last_proof_at=clock_timestamp()
                FROM gateways g
                WHERE c.gateway_id=g.id AND c.certificate_sha256=$1
                  AND c.status='active' AND g.status='active'
                  AND c.valid_after<=clock_timestamp() AND c.valid_until>clock_timestamp()
                  AND COALESCE(c.san_uri,c.spiffe_id)=$2
                RETURNING c.id AS certificate_id,c.gateway_id,c.certificate_sha256
            )
            UPDATE gateways g
            SET last_proof_at=clock_timestamp()
            FROM certificate_proof p
            WHERE g.id=p.gateway_id
            RETURNING g.id,g.owner_id,p.certificate_id,p.certificate_sha256
            "#,
        )
        .bind(mtls.certificate_sha256.as_slice())
        .bind(&mtls.uri_san)
        .fetch_optional(&mut *tx)
        .await?;
        let Some(row) = row else {
            // Preserve an overlap-expiry revocation even though authentication
            // itself fails. This keeps certificate state honest for operators.
            tx.commit().await?;
            return Err(ApiError::Unauthorized);
        };
        let gateway = Gateway {
            id: row.get("id"),
            owner_id: row.get("owner_id"),
            certificate_id: Some(row.get("certificate_id")),
            certificate_sha256: Some(exact_bytes(row.get::<Vec<u8>, _>("certificate_sha256"))?),
        };
        tx.commit().await?;
        Ok(gateway)
    }

    #[allow(clippy::too_many_arguments)]
    pub async fn upsert_gateway_catalog(
        &self,
        gateway: &Gateway,
        display_name: &str,
        host: &str,
        bootstrap_port: u16,
        port: u16,
        server_name: &str,
        ca_cert_der: &[u8],
        spki_sha256: &[u8; 32],
    ) -> Result<(), ApiError> {
        let changed = sqlx::query(
            r#"
            INSERT INTO gateway_lan_profiles
                (gateway_id,certificate_id,display_name,host,bootstrap_port,port,
                 server_name,ca_cert_der,spki_sha256)
            SELECT g.id,c.id,$3,$4,$5,$6,$7,$8,$9
            FROM gateways g
            JOIN gateway_certificates c ON c.id=$2 AND c.gateway_id=g.id
            WHERE g.id=$1 AND g.status='active' AND c.status='active'
              AND c.valid_after<=clock_timestamp() AND c.valid_until>clock_timestamp()
            ON CONFLICT (gateway_id) DO UPDATE SET
                certificate_id=EXCLUDED.certificate_id,
                display_name=EXCLUDED.display_name,
                host=EXCLUDED.host,
                bootstrap_port=EXCLUDED.bootstrap_port,
                port=EXCLUDED.port,
                server_name=EXCLUDED.server_name,
                ca_cert_der=EXCLUDED.ca_cert_der,
                spki_sha256=EXCLUDED.spki_sha256,
                updated_at=clock_timestamp()
            "#,
        )
        .bind(gateway.id)
        .bind(gateway.certificate_id)
        .bind(display_name)
        .bind(host)
        .bind(i32::from(bootstrap_port))
        .bind(i32::from(port))
        .bind(server_name)
        .bind(ca_cert_der)
        .bind(spki_sha256.as_slice())
        .execute(&self.pool)
        .await?;
        if changed.rows_affected() != 1 {
            return Err(ApiError::Unauthorized);
        }
        Ok(())
    }

    pub async fn list_gateway_catalog(
        &self,
        owner_id: Uuid,
    ) -> Result<Vec<GatewayCatalogView>, ApiError> {
        let rows = sqlx::query(
            r#"
            SELECT p.gateway_id,p.display_name,p.host,p.bootstrap_port,p.port,p.server_name,
                   p.ca_cert_der,p.spki_sha256,g.status::text AS state
            FROM gateway_lan_profiles p
            JOIN gateways g ON g.id=p.gateway_id
            JOIN gateway_certificates c
              ON c.id=p.certificate_id AND c.gateway_id=p.gateway_id
            WHERE g.owner_id=$1 AND g.status='active' AND c.status='active'
              AND c.valid_after<=clock_timestamp() AND c.valid_until>clock_timestamp()
            ORDER BY lower(p.display_name),p.gateway_id
            "#,
        )
        .bind(owner_id)
        .fetch_all(&self.pool)
        .await?;
        rows.into_iter()
            .map(|row| {
                let bootstrap_port = u16::try_from(row.get::<i32, _>("bootstrap_port"))
                    .map_err(ApiError::internal)?;
                let port = u16::try_from(row.get::<i32, _>("port")).map_err(ApiError::internal)?;
                Ok(GatewayCatalogView {
                    gateway_id: row.get("gateway_id"),
                    display_name: row.get("display_name"),
                    host: row.get("host"),
                    bootstrap_port,
                    port,
                    server_name: row.get("server_name"),
                    ca_cert_der_b64: URL_SAFE_NO_PAD.encode(row.get::<Vec<u8>, _>("ca_cert_der")),
                    spki_sha256_b64: URL_SAFE_NO_PAD.encode(row.get::<Vec<u8>, _>("spki_sha256")),
                    state: row.get("state"),
                })
            })
            .collect()
    }

    pub async fn secret_for_envelope(
        &self,
        gateway: &Gateway,
        envelope: &DeviceEnvelope,
    ) -> Result<SecretRecord, ApiError> {
        if gateway.id != envelope.gateway_id {
            return Err(ApiError::Forbidden);
        }
        let row = sqlx::query(
            r#"
            SELECT s.kms_key_id, s.wrapped_dek, s.secret_nonce,
                   s.secret_ciphertext, s.key_version
            FROM companions c
            JOIN gateway_companions gc ON gc.companion_id = c.id
            JOIN companion_secret_versions s ON s.companion_id = c.id
            WHERE c.id = $1 AND c.status = 'active'
              AND gc.gateway_id = $2 AND gc.unbound_at IS NULL
              AND s.key_version = $3 AND s.revoked_at IS NULL
              AND (s.accept_until IS NULL OR s.accept_until > clock_timestamp())
            "#,
        )
        .bind(envelope.companion_id)
        .bind(gateway.id)
        .bind(i32::try_from(envelope.key_version).map_err(ApiError::internal)?)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        Ok(SecretRecord {
            companion_id: envelope.companion_id,
            gateway_id: gateway.id,
            key_version: u32::try_from(row.get::<i32, _>("key_version"))
                .map_err(ApiError::internal)?,
            kms_key_id: row.get("kms_key_id"),
            wrapped_dek: row.get("wrapped_dek"),
            encrypted: EncryptedBytes {
                nonce: exact_bytes(row.get::<Vec<u8>, _>("secret_nonce"))?,
                ciphertext: row.get("secret_ciphertext"),
            },
        })
    }

    /// HMAC must already be verified. Replay preconditions are locked before
    /// JSON parsing, and all work commits with the new sequence.
    pub async fn ingest_verified_envelope(
        &self,
        gateway: &Gateway,
        envelope: &DeviceEnvelope,
        validated: &ValidatedEnvelope,
        transcript_sha256: &[u8; 32],
    ) -> Result<IngestOutcome, ApiError> {
        let mut tx = self.pool.begin().await?;
        // Serialize all key versions for one companion before checking the
        // request UUID. Without this companion-level lock, two simultaneous
        // uploads of the same signed WAL record could both miss the initial
        // request lookup and the loser would incorrectly receive a replay
        // error instead of the required idempotent acceptance.
        sqlx::query_scalar::<_, Uuid>(
            "SELECT id FROM companions WHERE id=$1 AND status='active' FOR UPDATE",
        )
        .bind(envelope.companion_id)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        let existing_request = sqlx::query(
            r#"
            SELECT envelope_sequence,transcript_sha256
            FROM device_requests
            WHERE companion_id=$1 AND request_id=$2
            "#,
        )
        .bind(envelope.companion_id)
        .bind(envelope.request_id)
        .fetch_optional(&mut *tx)
        .await?;
        if let Some(existing) = existing_request {
            let same_sequence = existing.get::<i64, _>("envelope_sequence") == validated.sequence;
            let same_transcript =
                existing.get::<Vec<u8>, _>("transcript_sha256").as_slice() == transcript_sha256;
            if same_sequence && same_transcript {
                tx.commit().await?;
                metrics::counter!("kitsu_device_envelope_retries_total").increment(1);
                return Ok(IngestOutcome::DuplicateCommitted);
            }
            return Err(ApiError::Conflict(
                "request ID was reused with different signed content",
            ));
        }
        let last: i64 = sqlx::query_scalar(
            r#"
            SELECT last_sequence FROM device_sequences
            WHERE companion_id = $1 AND key_version = $2
            FOR UPDATE
            "#,
        )
        .bind(envelope.companion_id)
        .bind(i32::try_from(envelope.key_version).map_err(ApiError::internal)?)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        if validated.sequence <= last {
            return Err(ApiError::Replay);
        }
        // Intentionally after sequence lock/check. Any error returns and drops
        // the transaction, so malformed authenticated bytes never burn a seq.
        let payload = envelope.parse_payload(&validated.payload)?;
        sqlx::query(
            r#"
            INSERT INTO device_requests
                (companion_id,request_id,key_version,envelope_sequence,nonce,transcript_sha256)
            VALUES ($1,$2,$3,$4,$5,$6)
            "#,
        )
        .bind(envelope.companion_id)
        .bind(envelope.request_id)
        .bind(i32::try_from(envelope.key_version).map_err(ApiError::internal)?)
        .bind(validated.sequence)
        .bind(validated.nonce.as_slice())
        .bind(transcript_sha256.as_slice())
        .execute(&mut *tx)
        .await
        .map_err(|_| ApiError::Replay)?;

        persist_payload(&mut tx, gateway, envelope, validated.sequence, &payload).await?;
        sqlx::query(
            r#"
            UPDATE device_sequences SET last_sequence=$3, updated_at=clock_timestamp()
            WHERE companion_id=$1 AND key_version=$2
            "#,
        )
        .bind(envelope.companion_id)
        .bind(i32::try_from(envelope.key_version).map_err(ApiError::internal)?)
        .bind(validated.sequence)
        .execute(&mut *tx)
        .await?;
        sqlx::query("UPDATE companions SET last_seen_at=clock_timestamp(),updated_at=clock_timestamp() WHERE id=$1")
            .bind(envelope.companion_id)
            .execute(&mut *tx)
            .await?;
        insert_audit(
            &mut tx,
            "device",
            Some(gateway.owner_id),
            Some(gateway.id),
            Some(envelope.companion_id),
            "device.envelope.accepted",
            "request",
            envelope.request_id.to_string(),
            json!({
                "sequence": validated.sequence.to_string(),
                "payload_type": envelope.payload_type,
                "key_version": envelope.key_version
            }),
        )
        .await?;
        tx.commit().await?;
        metrics::counter!("kitsu_device_envelopes_total", "payload_type" => envelope.payload_type.clone()).increment(1);
        Ok(IngestOutcome::Accepted)
    }

    pub async fn list_companions(
        &self,
        owner_id: Uuid,
    ) -> Result<Vec<CompanionListItem>, ApiError> {
        let rows = sqlx::query(
            "SELECT id,hardware_uid,display_name,status::text,last_seen_at FROM companions WHERE owner_id=$1 ORDER BY created_at",
        )
        .bind(owner_id)
        .fetch_all(&self.pool)
        .await?;
        Ok(rows
            .into_iter()
            .map(|row| CompanionListItem {
                id: row.get("id"),
                hardware_uid: row.get("hardware_uid"),
                display_name: row.get("display_name"),
                status: row.get("status"),
                last_seen_at: row.get("last_seen_at"),
            })
            .collect())
    }

    pub async fn snapshot(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
    ) -> Result<SnapshotProjection, ApiError> {
        let companion = sqlx::query(
            r#"
            SELECT id,hardware_uid,display_name,status::text,created_at,last_seen_at
            FROM companions WHERE id=$1 AND owner_id=$2
            "#,
        )
        .bind(companion_id)
        .bind(owner_id)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::NotFound)?;
        let projection = sqlx::query(
            "SELECT vitals,mood,bond,evolution,mesh,source_envelope_sequence,updated_at FROM companion_state_projections WHERE companion_id=$1",
        )
        .bind(companion_id)
        .fetch_optional(&self.pool)
        .await?;
        let gateway = sqlx::query(
            r#"
            SELECT g.id,g.last_proof_at
            FROM gateway_companions gc JOIN gateways g ON g.id=gc.gateway_id
            WHERE gc.companion_id=$1 AND gc.unbound_at IS NULL AND g.status='active'
            ORDER BY g.last_proof_at DESC NULLS LAST LIMIT 1
            "#,
        )
        .bind(companion_id)
        .fetch_optional(&self.pool)
        .await?;
        let last_seen: Option<DateTime<Utc>> = companion.get("last_seen_at");
        let online = last_seen.is_some_and(|seen| seen > Utc::now() - TimeDelta::seconds(90));
        let counts = sqlx::query(
            r#"
            SELECT
              (SELECT count(*) FROM peer_history WHERE companion_id=$1) AS peers,
              (SELECT count(*) FROM device_events WHERE companion_id=$1 AND event_type LIKE 'mesh.message%') AS messages,
              (SELECT count(*) FROM device_events e
                 WHERE e.companion_id=$1 AND e.event_type LIKE 'mesh.message%'
                   AND e.id > COALESCE((SELECT last_read_event_id FROM owner_companion_cursors
                                        WHERE owner_id=$2 AND companion_id=$1),0)) AS unread_messages,
              (SELECT COALESCE(max(id),0) FROM device_events WHERE companion_id=$1) AS cursor
            "#,
        )
        .bind(companion_id)
        .bind(owner_id)
        .fetch_one(&self.pool)
        .await?;
        let event_rows = sqlx::query(
            r#"
            SELECT id,event_id,event_type,observed_epoch,boot_id,monotonic_ms,body,received_at
            FROM device_events WHERE companion_id=$1 ORDER BY id DESC LIMIT 20
            "#,
        )
        .bind(companion_id)
        .fetch_all(&self.pool)
        .await?;
        let device_snapshot = sqlx::query_scalar::<_, Value>(
            r#"
            SELECT body FROM device_events
            WHERE companion_id=$1 AND event_type='companion.snapshot'
            ORDER BY id DESC LIMIT 1
            "#,
        )
        .bind(companion_id)
        .fetch_optional(&self.pool)
        .await?;
        let recent_events = event_rows
            .into_iter()
            .map(|row| {
                json!({
                    "cursor": row.get::<i64,_>("id").to_string(),
                    "event_id": row.get::<Uuid,_>("event_id"),
                    "event_type": row.get::<String,_>("event_type"),
                    "observed_epoch": row.get::<Option<i64>,_>("observed_epoch"),
                    "boot_id": row.get::<i64,_>("boot_id"),
                    "monotonic_ms": row.get::<i64,_>("monotonic_ms"),
                    "body": row.get::<Value,_>("body"),
                    "received_at": row.get::<DateTime<Utc>,_>("received_at")
                })
            })
            .collect();
        let empty = || json!({});
        let mut connectivity = serde_json::Map::from_iter([
            ("online".to_owned(), json!(online)),
            ("provenance".to_owned(), json!("gateway_mtls_device_hmac")),
            (
                "gateway_id".to_owned(),
                json!(gateway.as_ref().map(|row| row.get::<Uuid, _>("id"))),
            ),
            ("last_seen_at".to_owned(), json!(last_seen)),
            (
                "gateway_last_proof_at".to_owned(),
                json!(gateway
                    .as_ref()
                    .and_then(|row| row.get::<Option<DateTime<Utc>>, _>("last_proof_at"))),
            ),
        ]);
        // These values are copied only from an authenticated, companion-HMAC
        // snapshot. Unknown or malformed values are omitted rather than
        // inventing a reassuring `false` value.
        if let Some(device) = device_snapshot
            .as_ref()
            .and_then(Value::as_object)
            .filter(|body| {
                body.get("schema").and_then(Value::as_str) == Some("kitsu.companion-snapshot.v1")
            })
        {
            if let Some(value) = device
                .get("remote_connectivity_allowed")
                .filter(|value| value.is_boolean())
            {
                connectivity.insert("remote_connectivity_allowed".to_owned(), value.clone());
            }
            if let Some(version) = device
                .get("firmware_version")
                .and_then(Value::as_str)
                .filter(|text| {
                    !text.is_empty() && text.len() <= 64 && !text.chars().any(char::is_control)
                })
            {
                connectivity.insert("firmware_version".to_owned(), json!(version));
            }
            if let Some(wifi) = device.get("wifi").and_then(Value::as_object) {
                if let Some(value) = wifi.get("configured").filter(|value| value.is_boolean()) {
                    connectivity.insert("wifi_configured".to_owned(), value.clone());
                }
                if let Some(state) = wifi.get("state").and_then(Value::as_str).filter(|text| {
                    !text.is_empty() && text.len() <= 64 && !text.chars().any(char::is_control)
                }) {
                    connectivity.insert("wifi_state".to_owned(), json!(state));
                }
            }
            if let Some(gateway_state) = device.get("gateway").and_then(Value::as_object) {
                for (source, target) in [
                    ("configured", "gateway_configured"),
                    ("enrolled", "gateway_enrolled"),
                ] {
                    if let Some(value) =
                        gateway_state.get(source).filter(|value| value.is_boolean())
                    {
                        connectivity.insert(target.to_owned(), value.clone());
                    }
                }
                if let Some(state) = gateway_state
                    .get("lan_state")
                    .and_then(Value::as_str)
                    .filter(|text| {
                        !text.is_empty() && text.len() <= 64 && !text.chars().any(char::is_control)
                    })
                {
                    connectivity.insert("lan_state".to_owned(), json!(state));
                    connectivity.insert("gateway_lan_state".to_owned(), json!(state));
                }
            }
        }
        Ok(SnapshotProjection {
            companion: json!({
                "id": companion.get::<Uuid,_>("id"),
                "hardware_uid": companion.get::<String,_>("hardware_uid"),
                "display_name": companion.get::<String,_>("display_name"),
                "status": companion.get::<String,_>("status"),
                "created_at": companion.get::<DateTime<Utc>,_>("created_at")
            }),
            vitals: projection
                .as_ref()
                .map(|row| row.get("vitals"))
                .unwrap_or_else(empty),
            mood: projection
                .as_ref()
                .map(|row| row.get("mood"))
                .unwrap_or_else(empty),
            bond: projection
                .as_ref()
                .map(|row| row.get("bond"))
                .unwrap_or_else(empty),
            evolution: projection
                .as_ref()
                .map(|row| row.get("evolution"))
                .unwrap_or_else(empty),
            mesh: projection
                .as_ref()
                .map(|row| row.get("mesh"))
                .unwrap_or_else(empty),
            connectivity: Value::Object(connectivity),
            counts: json!({
                "peers": counts.get::<i64,_>("peers"),
                "messages": counts.get::<i64,_>("messages"),
                "unread_messages": counts.get::<i64,_>("unread_messages")
            }),
            recent_events,
            cursor: counts.get::<i64, _>("cursor").to_string(),
        })
    }

    pub async fn list_peers(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
    ) -> Result<Vec<Value>, ApiError> {
        ensure_owner_companion(&self.pool, owner_id, companion_id).await?;
        let rows = sqlx::query(
            r#"
            SELECT public_key,role,display_name,seen_count,last_seen_epoch,
                   last_seen_boot_id,last_seen_monotonic_ms,signal_scope,
                   rssi_deci_dbm,snr_deci_db,journal_sequence,updated_at
            FROM peer_history WHERE companion_id=$1 ORDER BY updated_at DESC LIMIT 256
            "#,
        )
        .bind(companion_id)
        .fetch_all(&self.pool)
        .await?;
        Ok(rows.into_iter().map(peer_json).collect())
    }

    pub async fn list_events(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
        after: i64,
        limit: i64,
    ) -> Result<Vec<Value>, ApiError> {
        ensure_owner_companion(&self.pool, owner_id, companion_id).await?;
        let rows = sqlx::query(
            r#"
            SELECT id,event_id,event_type,observed_epoch,boot_id,monotonic_ms,body,received_at
            FROM device_events WHERE companion_id=$1 AND id>$2 ORDER BY id LIMIT $3
            "#,
        )
        .bind(companion_id)
        .bind(after.max(0))
        .bind(limit.clamp(1, 200))
        .fetch_all(&self.pool)
        .await?;
        Ok(rows
            .into_iter()
            .map(|row| {
                json!({
                    "cursor": row.get::<i64,_>("id").to_string(),
                    "event_id": row.get::<Uuid,_>("event_id"),
                    "event_type": row.get::<String,_>("event_type"),
                    "observed_epoch": row.get::<Option<i64>,_>("observed_epoch"),
                    "boot_id": row.get::<i64,_>("boot_id"),
                    "monotonic_ms": row.get::<i64,_>("monotonic_ms"),
                    "body": row.get::<Value,_>("body"),
                    "received_at": row.get::<DateTime<Utc>,_>("received_at")
                })
            })
            .collect())
    }

    pub async fn action_secret_for_owner(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
    ) -> Result<ActionSecretRecord, ApiError> {
        let row = sqlx::query(
            r#"
            SELECT s.key_version,s.kms_key_id,s.wrapped_dek,
                   s.secret_nonce,s.secret_ciphertext
            FROM companions c
            JOIN companion_secret_versions s
              ON s.companion_id=c.id AND s.key_version=c.active_key_version
            WHERE c.id=$1 AND c.owner_id=$2 AND c.status='active'
              AND s.revoked_at IS NULL
              AND (s.accept_until IS NULL OR s.accept_until>clock_timestamp())
            "#,
        )
        .bind(companion_id)
        .bind(owner_id)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::NotFound)?;
        Ok(ActionSecretRecord {
            companion_id,
            key_version: u32::try_from(row.get::<i32, _>("key_version"))
                .map_err(ApiError::internal)?,
            kms_key_id: row.get("kms_key_id"),
            wrapped_dek: row.get("wrapped_dek"),
            encrypted: EncryptedBytes {
                nonce: exact_bytes(row.get::<Vec<u8>, _>("secret_nonce"))?,
                ciphertext: row.get("secret_ciphertext"),
            },
        })
    }

    pub async fn create_action(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
        idempotency_key: &str,
        request_hash: &[u8; 32],
        request: &CreateActionRequest,
        wire: &RemoteAction,
    ) -> Result<CreatedAction, ApiError> {
        ensure_owner_companion(&self.pool, owner_id, companion_id).await?;
        if wire.companion_id != companion_id || wire.action_type != request.action_type {
            return Err(ApiError::Invalid(
                "signed action does not match owner request",
            ));
        }
        let validated = wire.validate_wrapper()?;
        let created_at = DateTime::from_timestamp(validated.created_epoch, 0)
            .ok_or(ApiError::Invalid("invalid action creation time"))?;
        let expires_at = DateTime::from_timestamp(validated.expires_epoch, 0)
            .ok_or(ApiError::Invalid("invalid action expiry time"))?;
        let inserted = sqlx::query(
            r#"
            INSERT INTO remote_actions
                (id,owner_id,companion_id,idempotency_key,request_sha256,
                 action_type,parameters,key_version,wire_nonce,wire_parameters,
                 wire_signature,created_at,expires_at)
            VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)
            ON CONFLICT (owner_id,idempotency_key) DO NOTHING
            RETURNING id,companion_id,action_type,parameters,status::text,
                      created_at,expires_at,completed_at,result,key_version,
                      wire_nonce,wire_parameters,wire_signature
            "#,
        )
        .bind(wire.action_id)
        .bind(owner_id)
        .bind(companion_id)
        .bind(idempotency_key)
        .bind(request_hash.as_slice())
        .bind(&request.action_type)
        .bind(Value::Object(request.parameters.clone()))
        .bind(i32::try_from(validated.key_version).map_err(ApiError::internal)?)
        .bind(validated.nonce.as_slice())
        .bind(&validated.params)
        .bind(validated.signature.as_slice())
        .bind(created_at)
        .bind(expires_at)
        .fetch_optional(&self.pool)
        .await?;
        let (row, was_inserted) = if let Some(row) = inserted {
            (row, true)
        } else {
            let row = sqlx::query(
                r#"
                SELECT id,companion_id,action_type,parameters,status::text,
                       created_at,expires_at,completed_at,result,request_sha256,
                       key_version,wire_nonce,wire_parameters,wire_signature
                FROM remote_actions WHERE owner_id=$1 AND idempotency_key=$2
                "#,
            )
            .bind(owner_id)
            .bind(idempotency_key)
            .fetch_one(&self.pool)
            .await?;
            if row.get::<Vec<u8>, _>("request_sha256").as_slice() != request_hash {
                return Err(ApiError::Conflict(
                    "idempotency key was used for a different request",
                ));
            }
            (row, false)
        };
        Ok(CreatedAction {
            view: action_from_row(&row),
            wire: remote_action_from_row(&row)?,
            inserted: was_inserted,
        })
    }

    pub async fn pending_actions(&self, gateway: &Gateway) -> Result<Vec<RemoteAction>, ApiError> {
        sqlx::query(
            "UPDATE remote_actions SET status='expired' WHERE expires_at<=clock_timestamp() AND status IN ('queued','delivered')",
        )
        .execute(&self.pool)
        .await?;
        let rows = sqlx::query(
            r#"
            SELECT a.id,a.companion_id,a.action_type,a.created_at,a.expires_at,
                   a.key_version,a.wire_nonce,a.wire_parameters,a.wire_signature
            FROM remote_actions a
            JOIN gateway_companions gc ON gc.companion_id=a.companion_id
            WHERE gc.gateway_id=$1 AND gc.unbound_at IS NULL
              AND a.status='queued' AND a.expires_at>clock_timestamp()
            ORDER BY a.created_at LIMIT 100
            "#,
        )
        .bind(gateway.id)
        .fetch_all(&self.pool)
        .await?;
        rows.iter().map(remote_action_from_row).collect()
    }

    pub async fn gateway_for_companion(
        &self,
        companion_id: Uuid,
    ) -> Result<Option<Uuid>, ApiError> {
        Ok(sqlx::query_scalar(
            "SELECT gateway_id FROM gateway_companions WHERE companion_id=$1 AND unbound_at IS NULL",
        )
        .bind(companion_id)
        .fetch_optional(&self.pool)
        .await?)
    }

    pub async fn gateway_by_id(&self, gateway_id: Uuid) -> Result<Option<Gateway>, ApiError> {
        let row = sqlx::query(
            r#"
            SELECT g.id,g.owner_id,c.id AS certificate_id,c.certificate_sha256
            FROM gateways g
            LEFT JOIN LATERAL (
                SELECT id,certificate_sha256 FROM gateway_certificates
                WHERE gateway_id=g.id AND status='active'
                  AND valid_after<=clock_timestamp() AND valid_until>clock_timestamp()
                ORDER BY last_proof_at DESC NULLS LAST,activated_at DESC
                LIMIT 1
            ) c ON true
            WHERE g.id=$1 AND g.status='active'
            "#,
        )
        .bind(gateway_id)
        .fetch_optional(&self.pool)
        .await?;
        row.map(|row| {
            Ok(Gateway {
                id: row.get("id"),
                owner_id: row.get("owner_id"),
                certificate_id: row.get("certificate_id"),
                certificate_sha256: row
                    .get::<Option<Vec<u8>>, _>("certificate_sha256")
                    .map(exact_bytes)
                    .transpose()?,
            })
        })
        .transpose()
    }

    /// Create a certificate-less logical gateway when needed, then bind it to
    /// one native installation. The binding never changes after creation.
    pub async fn create_or_get_mobile_relay(
        &self,
        owner_id: Uuid,
        installation_id: Uuid,
        gateway_id: Uuid,
    ) -> Result<MobileRelayView, ApiError> {
        if owner_id.is_nil() || installation_id.is_nil() || gateway_id.is_nil() {
            return Err(ApiError::Invalid("invalid mobile relay identity"));
        }
        let mut tx = self.pool.begin().await?;
        sqlx::query(
            r#"
            INSERT INTO gateways (id,owner_id,status,activated_at)
            VALUES ($1,$2,'active',clock_timestamp())
            ON CONFLICT (id) DO NOTHING
            "#,
        )
        .bind(gateway_id)
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            INSERT INTO mobile_relay_installations
                (installation_id,owner_id,gateway_id)
            SELECT $1,g.owner_id,g.id
            FROM gateways g
            WHERE g.id=$3 AND g.owner_id=$2 AND g.status='active'
            ON CONFLICT (installation_id) DO NOTHING
            "#,
        )
        .bind(installation_id)
        .bind(owner_id)
        .bind(gateway_id)
        .execute(&mut *tx)
        .await?;
        let row = sqlx::query(
            r#"
            SELECT m.installation_id,m.gateway_id,m.created_at
            FROM mobile_relay_installations m
            JOIN gateways g ON g.id=m.gateway_id AND g.owner_id=m.owner_id
            WHERE m.installation_id=$1 AND m.owner_id=$2
              AND g.status='active'
            "#,
        )
        .bind(installation_id)
        .bind(owner_id)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::NotFound)?;
        if row.get::<Uuid, _>("gateway_id") != gateway_id {
            return Err(ApiError::Conflict(
                "mobile relay is bound to another gateway",
            ));
        }
        let view = MobileRelayView {
            installation_id: row.get("installation_id"),
            gateway_id: row.get("gateway_id"),
            created_at: row.get("created_at"),
        };
        tx.commit().await?;
        Ok(view)
    }

    pub async fn mobile_relay(
        &self,
        owner_id: Uuid,
        installation_id: Uuid,
    ) -> Result<MobileRelay, ApiError> {
        if owner_id.is_nil() || installation_id.is_nil() {
            return Err(ApiError::NotFound);
        }
        let row = sqlx::query(
            r#"
            SELECT m.installation_id,m.gateway_id,m.created_at,g.owner_id
            FROM mobile_relay_installations m
            JOIN gateways g ON g.id=m.gateway_id AND g.owner_id=m.owner_id
            WHERE m.installation_id=$1 AND m.owner_id=$2
              AND g.status='active'
            "#,
        )
        .bind(installation_id)
        .bind(owner_id)
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::NotFound)?;
        let gateway_id: Uuid = row.get("gateway_id");
        Ok(MobileRelay {
            view: MobileRelayView {
                installation_id: row.get("installation_id"),
                gateway_id,
                created_at: row.get("created_at"),
            },
            gateway: Gateway {
                id: gateway_id,
                owner_id: row.get("owner_id"),
                certificate_id: None,
                certificate_sha256: None,
            },
        })
    }

    /// Creates one account-free relay identity. A dedicated internal owner per
    /// installation preserves all existing gateway/enrollment ownership
    /// constraints without granting the credential access to owner routes.
    /// The credential remains pending until a device enrollment claim made
    /// through this relay succeeds.
    pub async fn create_or_get_device_relay(
        &self,
        installation_id: Uuid,
        gateway_id: Uuid,
        token_digest: &[u8; 32],
    ) -> Result<DeviceRelay, ApiError> {
        if installation_id.is_nil() || gateway_id.is_nil() {
            return Err(ApiError::Invalid("invalid device relay identity"));
        }
        let subject = installation_id.to_string();
        let candidate_owner_id = Uuid::new_v4();
        let mut tx = self.pool.begin().await?;
        sqlx::query(
            r#"
            INSERT INTO owners (id,issuer,subject,display_name)
            VALUES ($1,$2,$3,'Device relay')
            ON CONFLICT (issuer,subject) DO NOTHING
            "#,
        )
        .bind(candidate_owner_id)
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .bind(&subject)
        .execute(&mut *tx)
        .await?;
        // Serializes first-use races for the same installation without a
        // process-local lock. The owner row is dedicated to this installation.
        let owner_id: Uuid =
            sqlx::query_scalar("SELECT id FROM owners WHERE issuer=$1 AND subject=$2 FOR UPDATE")
                .bind(DEVICE_RELAY_OWNER_ISSUER)
                .bind(&subject)
                .fetch_one(&mut *tx)
                .await?;

        let existing = sqlx::query(
            r#"
            SELECT m.installation_id,m.gateway_id,m.created_at,m.owner_id,
                   g.status::text AS gateway_status,c.token_digest,c.activated_at
            FROM mobile_relay_installations m
            JOIN gateways g ON g.id=m.gateway_id AND g.owner_id=m.owner_id
            LEFT JOIN mobile_relay_credentials c
              ON c.installation_id=m.installation_id
            WHERE m.installation_id=$1
            FOR UPDATE OF g
            "#,
        )
        .bind(installation_id)
        .fetch_optional(&mut *tx)
        .await?;
        if let Some(row) = existing {
            if row.get::<Uuid, _>("owner_id") != owner_id {
                return Err(ApiError::Unauthorized);
            }
            let stored = row
                .get::<Option<Vec<u8>>, _>("token_digest")
                .map(exact_bytes::<32>)
                .transpose()?
                .ok_or(ApiError::Unauthorized)?;
            if !bool::from(stored.ct_eq(token_digest)) {
                return Err(ApiError::Unauthorized);
            }
            if row.get::<Uuid, _>("gateway_id") != gateway_id {
                return Err(ApiError::Conflict(
                    "device relay is bound to another gateway",
                ));
            }
            if row.get::<String, _>("gateway_status") != "active" {
                return Err(ApiError::Forbidden);
            }
            let relay = device_relay_from_row(&row)?;
            tx.commit().await?;
            return Ok(relay);
        }

        sqlx::query(
            r#"
            INSERT INTO gateways (id,owner_id,status,activated_at)
            VALUES ($1,$2,'active',clock_timestamp())
            ON CONFLICT (id) DO NOTHING
            "#,
        )
        .bind(gateway_id)
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        let gateway_owner: Option<Uuid> = sqlx::query_scalar(
            "SELECT owner_id FROM gateways WHERE id=$1 AND status='active' FOR SHARE",
        )
        .bind(gateway_id)
        .fetch_optional(&mut *tx)
        .await?;
        if gateway_owner != Some(owner_id) {
            return Err(ApiError::Conflict(
                "device relay gateway identity is unavailable",
            ));
        }
        sqlx::query(
            r#"
            INSERT INTO mobile_relay_installations
                (installation_id,owner_id,gateway_id)
            VALUES ($1,$2,$3)
            "#,
        )
        .bind(installation_id)
        .bind(owner_id)
        .bind(gateway_id)
        .execute(&mut *tx)
        .await
        .map_err(map_conflict)?;
        sqlx::query(
            r#"
            INSERT INTO mobile_relay_credentials (installation_id,token_digest)
            VALUES ($1,$2)
            "#,
        )
        .bind(installation_id)
        .bind(token_digest.as_slice())
        .execute(&mut *tx)
        .await
        .map_err(map_conflict)?;
        let row = sqlx::query(
            r#"
            SELECT m.installation_id,m.gateway_id,m.created_at,m.owner_id,
                   c.activated_at
            FROM mobile_relay_installations m
            JOIN mobile_relay_credentials c
              ON c.installation_id=m.installation_id
            WHERE m.installation_id=$1 AND m.owner_id=$2
            "#,
        )
        .bind(installation_id)
        .bind(owner_id)
        .fetch_one(&mut *tx)
        .await?;
        let relay = device_relay_from_row(&row)?;
        tx.commit().await?;
        Ok(relay)
    }

    /// Authenticates one installation-scoped credential. Missing
    /// installations and wrong credentials intentionally have one result.
    pub async fn device_relay(
        &self,
        installation_id: Uuid,
        token_digest: &[u8; 32],
    ) -> Result<DeviceRelay, ApiError> {
        if installation_id.is_nil() {
            return Err(ApiError::Unauthorized);
        }
        let row = sqlx::query(
            r#"
            SELECT m.installation_id,m.gateway_id,m.created_at,m.owner_id,
                   c.activated_at
            FROM mobile_relay_installations m
            JOIN mobile_relay_credentials c
              ON c.installation_id=m.installation_id
            JOIN gateways g ON g.id=m.gateway_id AND g.owner_id=m.owner_id
            JOIN owners o ON o.id=m.owner_id
            WHERE m.installation_id=$1 AND c.token_digest=$2
              AND o.issuer=$3 AND o.subject=$4 AND g.status='active'
            "#,
        )
        .bind(installation_id)
        .bind(token_digest.as_slice())
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .bind(installation_id.to_string())
        .fetch_optional(&self.pool)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        device_relay_from_row(&row)
    }

    /// Revokes one account-free relay without crossing its dedicated
    /// synthetic-owner boundary. Certificate and audit identities remain as
    /// tombstones so revocation history and foreign-key integrity survive.
    pub async fn forget_device_relay(
        &self,
        installation_id: Uuid,
        token_digest: &[u8; 32],
    ) -> Result<Uuid, ApiError> {
        if installation_id.is_nil() {
            return Err(ApiError::NotFound);
        }
        let mut tx = self.pool.begin().await?;
        let relay = sqlx::query(
            r#"
            SELECT m.owner_id,m.gateway_id,c.token_digest
            FROM mobile_relay_installations m
            JOIN mobile_relay_credentials c
              ON c.installation_id=m.installation_id
            JOIN owners o ON o.id=m.owner_id
            JOIN gateways g ON g.id=m.gateway_id AND g.owner_id=m.owner_id
            WHERE m.installation_id=$1
              AND o.issuer=$2 AND o.subject=$3
            FOR UPDATE OF m,c,o,g
            "#,
        )
        .bind(installation_id)
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .bind(installation_id.to_string())
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::NotFound)?;
        let stored_digest = exact_bytes::<32>(relay.get::<Vec<u8>, _>("token_digest"))?;
        if !bool::from(stored_digest.ct_eq(token_digest)) {
            return Err(ApiError::Unauthorized);
        }
        let owner_id: Uuid = relay.get("owner_id");
        let gateway_id: Uuid = relay.get("gateway_id");

        // Creation gives every account-free installation exactly one owner
        // and one gateway. Refuse a broad cleanup if that invariant was ever
        // violated instead of touching unrelated owner data.
        let isolated: bool = sqlx::query_scalar(
            r#"
            SELECT
              (SELECT count(*) FROM mobile_relay_installations
               WHERE owner_id=$1)=1
              AND
              (SELECT count(*) FROM gateways WHERE owner_id=$1)=1
            "#,
        )
        .bind(owner_id)
        .fetch_one(&mut *tx)
        .await?;
        if !isolated {
            return Err(ApiError::Conflict("device relay ownership is not isolated"));
        }

        sqlx::query(
            r#"
            UPDATE enrollment_challenges
            SET status='cancelled',issuance_id=NULL,issuance_started_at=NULL
            WHERE owner_id=$1 AND status IN ('pending','issuing')
            "#,
        )
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE gateway_bootstraps
            SET status='cancelled',issuance_id=NULL,issuance_started_at=NULL
            WHERE owner_id=$1 AND status IN ('pending','issuing')
            "#,
        )
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE remote_actions
            SET status='cancelled',completed_at=COALESCE(completed_at,clock_timestamp()),
                result=COALESCE(result,'{"code":"relay_forgotten"}'::jsonb)
            WHERE owner_id=$1 AND status IN ('queued','delivered')
            "#,
        )
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE gateway_companions gc
            SET unbound_at=COALESCE(gc.unbound_at,clock_timestamp())
            FROM companions c
            WHERE gc.gateway_id=$1 AND gc.companion_id=c.id
              AND c.owner_id=$2 AND gc.unbound_at IS NULL
            "#,
        )
        .bind(gateway_id)
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE companion_secret_versions s
            SET revoked_at=COALESCE(s.revoked_at,clock_timestamp()),
                accept_until=LEAST(COALESCE(s.accept_until,clock_timestamp()),
                                   clock_timestamp())
            FROM companions c
            WHERE s.companion_id=c.id AND c.owner_id=$1
            "#,
        )
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE companion_certificates cc
            SET status='revoked',revoked_at=COALESCE(cc.revoked_at,clock_timestamp()),
                revocation_reason=COALESCE(cc.revocation_reason,'cessation_of_operation')
            FROM companions c
            WHERE cc.companion_id=c.id AND c.owner_id=$1
              AND cc.status<>'revoked'
            "#,
        )
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            "UPDATE gateway_certificate_rotations SET status='cancelled' WHERE gateway_id=$1 AND status='pending'",
        )
        .bind(gateway_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            "UPDATE gateway_certificates SET status='revoked',revoked_at=COALESCE(revoked_at,clock_timestamp()) WHERE gateway_id=$1 AND status<>'revoked'",
        )
        .bind(gateway_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query("DELETE FROM gateway_lan_profiles WHERE gateway_id=$1")
            .bind(gateway_id)
            .execute(&mut *tx)
            .await?;
        sqlx::query(
            r#"
            UPDATE companions
            SET status='retired',hardware_uid='forgotten:'||id::text,
                display_name='Forgotten companion',last_seen_at=NULL,
                updated_at=clock_timestamp()
            WHERE owner_id=$1
            "#,
        )
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE gateways
            SET status='revoked',revoked_at=COALESCE(revoked_at,clock_timestamp()),
                last_proof_at=NULL
            WHERE id=$1 AND owner_id=$2
            "#,
        )
        .bind(gateway_id)
        .bind(owner_id)
        .execute(&mut *tx)
        .await?;

        let revoked = sqlx::query(
            "DELETE FROM mobile_relay_credentials WHERE installation_id=$1 AND token_digest=$2",
        )
        .bind(installation_id)
        .bind(token_digest.as_slice())
        .execute(&mut *tx)
        .await?;
        if revoked.rows_affected() != 1 {
            return Err(ApiError::Conflict("device relay state changed"));
        }
        let removed = sqlx::query(
            "DELETE FROM mobile_relay_installations WHERE installation_id=$1 AND owner_id=$2 AND gateway_id=$3",
        )
        .bind(installation_id)
        .bind(owner_id)
        .bind(gateway_id)
        .execute(&mut *tx)
        .await?;
        if removed.rows_affected() != 1 {
            return Err(ApiError::Conflict("device relay state changed"));
        }
        sqlx::query(
            "DELETE FROM rate_limit_buckets WHERE subject_hash=$1 AND scope LIKE 'device_relay.%'",
        )
        .bind(token_digest.as_slice())
        .execute(&mut *tx)
        .await?;
        let tombstoned = sqlx::query(
            r#"
            UPDATE owners
            SET subject='forgotten:'||id::text,email=NULL,display_name=NULL,
                last_login_at=clock_timestamp()
            WHERE id=$1 AND issuer=$2 AND subject=$3
            "#,
        )
        .bind(owner_id)
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .bind(installation_id.to_string())
        .execute(&mut *tx)
        .await?;
        if tombstoned.rows_affected() != 1 {
            return Err(ApiError::Conflict("device relay state changed"));
        }
        tx.commit().await?;
        Ok(gateway_id)
    }

    /// Creates a one-use device enrollment for this relay. A pending
    /// credential may hold one live activation attempt; an active credential
    /// may fill the installation's remaining three companion slots.
    #[allow(clippy::too_many_arguments)]
    pub async fn create_device_relay_enrollment(
        &self,
        installation_id: Uuid,
        credential_digest: &[u8; 32],
        hardware_uid: &str,
        display_name: &str,
        claim_token_digest: &[u8; 32],
        ttl: Duration,
    ) -> Result<EnrollmentView, ApiError> {
        if installation_id.is_nil() {
            return Err(ApiError::Unauthorized);
        }
        let ttl_seconds = i64::try_from(ttl.as_secs()).map_err(ApiError::internal)?;
        let mut tx = self.pool.begin().await?;
        let relay = sqlx::query(
            r#"
            SELECT m.owner_id,m.gateway_id,c.activated_at
            FROM mobile_relay_installations m
            JOIN mobile_relay_credentials c
              ON c.installation_id=m.installation_id
            JOIN gateways g ON g.id=m.gateway_id AND g.owner_id=m.owner_id
            JOIN owners o ON o.id=m.owner_id
            WHERE m.installation_id=$1 AND c.token_digest=$2
              AND o.issuer=$3 AND o.subject=$4 AND g.status='active'
            FOR UPDATE OF c,g
            "#,
        )
        .bind(installation_id)
        .bind(credential_digest.as_slice())
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .bind(installation_id.to_string())
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        // A missing or wrong relay principal remains a uniform 401 even when
        // the caller also supplied malformed enrollment fields.
        validate_identity_text(hardware_uid, display_name)?;
        let owner_id: Uuid = relay.get("owner_id");
        let gateway_id: Uuid = relay.get("gateway_id");
        let occupied: i64 = sqlx::query_scalar(
            r#"
            SELECT
              (SELECT count(*) FROM companions
               WHERE owner_id=$1 AND status='active')
              +
              (SELECT count(*) FROM enrollment_challenges
               WHERE owner_id=$1 AND
                 ((status='issuing' AND NOT (
                    provider_job_id IS NULL AND provider_started_at IS NOT NULL AND
                    provider_started_at<=clock_timestamp()
                      -make_interval(secs => $2::double precision)
                  )) OR
                  (status='pending' AND expires_at>clock_timestamp())))
            "#,
        )
        .bind(owner_id)
        .bind(PROVIDER_RETRY_WINDOW_SECONDS)
        .fetch_one(&mut *tx)
        .await?;
        if occupied >= DEVICE_RELAY_MAXIMUM_COMPANIONS {
            return Err(ApiError::Conflict("device relay companion limit reached"));
        }
        if relay
            .get::<Option<DateTime<Utc>>, _>("activated_at")
            .is_none()
            && occupied != 0
        {
            return Err(ApiError::Conflict(
                "device relay activation enrollment already exists",
            ));
        }

        let enrollment_id = Uuid::new_v4();
        let expires_at: DateTime<Utc> = sqlx::query_scalar(
            r#"
            INSERT INTO enrollment_challenges
                (id,owner_id,token_digest,hardware_uid,display_name,expires_at)
            VALUES ($1,$2,$3,$4,$5,
                    clock_timestamp()+make_interval(secs => $6::double precision))
            RETURNING expires_at
            "#,
        )
        .bind(enrollment_id)
        .bind(owner_id)
        .bind(claim_token_digest.as_slice())
        .bind(hardware_uid)
        .bind(display_name)
        .bind(ttl_seconds)
        .fetch_one(&mut *tx)
        .await
        .map_err(map_conflict)?;
        insert_audit(
            &mut tx,
            "system",
            None,
            None,
            None,
            "enrollment.created",
            "enrollment",
            enrollment_id.to_string(),
            json!({
                "relay_gateway_id": gateway_id,
                "hardware_uid_sha256": hex::encode(sha256(hardware_uid.as_bytes()))
            }),
        )
        .await?;
        tx.commit().await?;
        Ok(EnrollmentView {
            id: enrollment_id,
            hardware_uid: hardware_uid.to_owned(),
            display_name: display_name.to_owned(),
            status: "pending".into(),
            expires_at,
        })
    }

    /// Idempotently unlocks steady relay traffic after the shared enrollment
    /// transaction has bound the claim's first-use device key to this exact
    /// logical gateway. Physical confirmation is enforced locally by firmware
    /// and is not attested to this service.
    pub async fn activate_device_relay(
        &self,
        installation_id: Uuid,
        enrollment_id: Uuid,
        credential_digest: &[u8; 32],
    ) -> Result<(), ApiError> {
        let changed = sqlx::query(
            r#"
            UPDATE mobile_relay_credentials c
            SET activated_at=COALESCE(c.activated_at,clock_timestamp())
            FROM mobile_relay_installations m,owners o,enrollment_challenges e
            WHERE c.installation_id=$1 AND c.token_digest=$2
              AND m.installation_id=c.installation_id
              AND o.id=m.owner_id AND o.issuer=$3 AND o.subject=$4
              AND e.id=$5 AND e.owner_id=m.owner_id AND e.status='claimed'
              AND e.claimed_gateway_id=m.gateway_id AND e.companion_id IS NOT NULL
            "#,
        )
        .bind(installation_id)
        .bind(credential_digest.as_slice())
        .bind(DEVICE_RELAY_OWNER_ISSUER)
        .bind(installation_id.to_string())
        .bind(enrollment_id)
        .execute(&self.pool)
        .await?;
        if changed.rows_affected() == 1 {
            return Ok(());
        }
        // Preserve uniform credential failures while distinguishing a valid
        // pending installation from a claim that does not belong to it.
        self.device_relay(installation_id, credential_digest)
            .await?;
        Err(ApiError::Forbidden)
    }

    pub async fn create_certificate_rotation(
        &self,
        owner_id: Uuid,
        gateway_id: Uuid,
        token_digest: &[u8; 32],
        ttl: Duration,
        overlap: Duration,
    ) -> Result<CertificateRotationView, ApiError> {
        let ttl = TimeDelta::from_std(ttl).map_err(ApiError::internal)?;
        let overlap_seconds = i32::try_from(overlap.as_secs()).map_err(ApiError::internal)?;
        let mut tx = self.pool.begin().await?;
        sqlx::query(
            "UPDATE gateway_certificate_rotations SET status='expired' WHERE gateway_id=$1 AND status='pending' AND expires_at<=clock_timestamp()",
        )
        .bind(gateway_id)
        .execute(&mut *tx)
        .await?;
        let old = sqlx::query(
            r#"
            SELECT c.id
            FROM gateways g JOIN gateway_certificates c ON c.gateway_id=g.id
            WHERE g.id=$1 AND g.owner_id=$2 AND g.status='active'
              AND c.status='active' AND c.valid_after<=clock_timestamp()
              AND c.valid_until>clock_timestamp()
            ORDER BY c.last_proof_at DESC NULLS LAST,c.activated_at DESC
            LIMIT 1 FOR UPDATE OF g,c
            "#,
        )
        .bind(gateway_id)
        .bind(owner_id)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::NotFound)?;
        let id = Uuid::new_v4();
        let old_certificate_id: Uuid = old.get("id");
        let expires_at = Utc::now() + ttl;
        sqlx::query(
            r#"
            INSERT INTO gateway_certificate_rotations
                (id,gateway_id,old_certificate_id,token_digest,expires_at,overlap_seconds)
            VALUES ($1,$2,$3,$4,$5,$6)
            "#,
        )
        .bind(id)
        .bind(gateway_id)
        .bind(old_certificate_id)
        .bind(token_digest.as_slice())
        .bind(expires_at)
        .bind(overlap_seconds)
        .execute(&mut *tx)
        .await
        .map_err(map_conflict)?;
        insert_audit(
            &mut tx,
            "owner",
            Some(owner_id),
            Some(gateway_id),
            None,
            "gateway.certificate_rotation_created",
            "gateway",
            gateway_id.to_string(),
            json!({"rotation_id": id, "old_certificate_id": old_certificate_id}),
        )
        .await?;
        tx.commit().await?;
        Ok(CertificateRotationView {
            id,
            gateway_id,
            old_certificate_id,
            status: "pending".into(),
            expires_at,
            overlap_ends_at: None,
        })
    }

    pub async fn activate_certificate_rotation(
        &self,
        rotation_id: Uuid,
        token_digest: &[u8; 32],
        mtls: &MtlsIdentity,
    ) -> Result<ActivatedCertificateView, ApiError> {
        let mut tx = self.pool.begin().await?;
        let rotation = sqlx::query(
            r#"
            SELECT r.gateway_id,r.old_certificate_id,r.new_certificate_id,
                   r.status::text,r.expires_at,r.overlap_seconds,r.overlap_ends_at,
                   g.owner_id,g.status::text AS gateway_status,
                   COALESCE(old.san_uri,old.spiffe_id) AS old_uri_san
            FROM gateway_certificate_rotations r
            JOIN gateways g ON g.id=r.gateway_id
            JOIN gateway_certificates old ON old.id=r.old_certificate_id
            WHERE r.id=$1 AND r.token_digest=$2
            FOR UPDATE OF r,g,old
            "#,
        )
        .bind(rotation_id)
        .bind(token_digest.as_slice())
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::Unauthorized)?;
        let status: String = rotation.get("status");
        let gateway_status: String = rotation.get("gateway_status");
        if gateway_status != "active" {
            return Err(ApiError::Forbidden);
        }
        let gateway_id: Uuid = rotation.get("gateway_id");

        if status == "activated" {
            let certificate_id: Uuid = rotation
                .get::<Option<Uuid>, _>("new_certificate_id")
                .ok_or_else(|| {
                    ApiError::internal(anyhow::anyhow!("activated rotation has no certificate"))
                })?;
            let fingerprint: Vec<u8> = sqlx::query_scalar(
                "SELECT certificate_sha256 FROM gateway_certificates WHERE id=$1 AND gateway_id=$2",
            )
            .bind(certificate_id)
            .bind(gateway_id)
            .fetch_one(&mut *tx)
            .await?;
            if fingerprint.as_slice() != mtls.certificate_sha256.as_slice() {
                return Err(ApiError::Conflict(
                    "rotation was already activated by another certificate",
                ));
            }
            let overlap_ends_at = rotation
                .get::<Option<DateTime<Utc>>, _>("overlap_ends_at")
                .ok_or_else(|| {
                    ApiError::internal(anyhow::anyhow!(
                        "activated rotation has no overlap deadline"
                    ))
                })?;
            sqlx::query(
                "UPDATE gateway_certificates SET last_proof_at=clock_timestamp() WHERE id=$1",
            )
            .bind(certificate_id)
            .execute(&mut *tx)
            .await?;
            sqlx::query("UPDATE gateways SET last_proof_at=clock_timestamp() WHERE id=$1")
                .bind(gateway_id)
                .execute(&mut *tx)
                .await?;
            tx.commit().await?;
            return Ok(ActivatedCertificateView {
                gateway_id,
                certificate_id,
                overlap_ends_at,
            });
        }
        if status != "pending" || rotation.get::<DateTime<Utc>, _>("expires_at") <= Utc::now() {
            if status == "pending" {
                sqlx::query(
                    "UPDATE gateway_certificate_rotations SET status='expired' WHERE id=$1",
                )
                .bind(rotation_id)
                .execute(&mut *tx)
                .await?;
                tx.commit().await?;
            }
            return Err(ApiError::Expired);
        }
        let old_uri_san: Option<String> = rotation.get("old_uri_san");
        if old_uri_san.as_deref() != Some(mtls.uri_san.as_str()) {
            return Err(ApiError::Conflict(
                "replacement certificate changed the gateway URI SAN",
            ));
        }
        if sqlx::query_scalar::<_, Uuid>(
            "SELECT gateway_id FROM gateway_certificates WHERE certificate_sha256=$1",
        )
        .bind(mtls.certificate_sha256.as_slice())
        .fetch_optional(&mut *tx)
        .await?
        .is_some()
        {
            return Err(ApiError::Conflict(
                "replacement certificate is already registered",
            ));
        }

        let certificate_id = Uuid::new_v4();
        let overlap_ends_at =
            Utc::now() + TimeDelta::seconds(i64::from(rotation.get::<i32, _>("overlap_seconds")));
        sqlx::query(
            r#"
            INSERT INTO gateway_certificates
                (id,gateway_id,certificate_sha256,spiffe_id,status,valid_after,
                 valid_until,activated_at,last_proof_at,san_uri)
            VALUES ($1,$2,$3,NULL,'active',$4,$5,clock_timestamp(),clock_timestamp(),$6)
            "#,
        )
        .bind(certificate_id)
        .bind(gateway_id)
        .bind(mtls.certificate_sha256.as_slice())
        .bind(mtls.not_before)
        .bind(mtls.not_after)
        .bind(&mtls.uri_san)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            r#"
            UPDATE gateway_certificate_rotations SET
                status='activated',new_certificate_id=$2,
                activated_at=clock_timestamp(),overlap_ends_at=$3
            WHERE id=$1
            "#,
        )
        .bind(rotation_id)
        .bind(certificate_id)
        .bind(overlap_ends_at)
        .execute(&mut *tx)
        .await?;
        sqlx::query("UPDATE gateways SET last_proof_at=clock_timestamp() WHERE id=$1")
            .bind(gateway_id)
            .execute(&mut *tx)
            .await?;
        insert_audit(
            &mut tx,
            "gateway",
            Some(rotation.get("owner_id")),
            Some(gateway_id),
            None,
            "gateway.certificate_rotated",
            "gateway_certificate",
            certificate_id.to_string(),
            json!({
                "rotation_id": rotation_id,
                "old_certificate_id": rotation.get::<Uuid,_>("old_certificate_id"),
                "overlap_ends_at": overlap_ends_at
            }),
        )
        .await?;
        tx.commit().await?;
        Ok(ActivatedCertificateView {
            gateway_id,
            certificate_id,
            overlap_ends_at,
        })
    }

    pub async fn revoke_gateway_certificate(
        &self,
        owner_id: Uuid,
        gateway_id: Uuid,
        certificate_id: Uuid,
    ) -> Result<(), ApiError> {
        let mut tx = self.pool.begin().await?;
        let certificate = sqlx::query(
            r#"
            SELECT c.status::text
            FROM gateway_certificates c JOIN gateways g ON g.id=c.gateway_id
            WHERE c.id=$1 AND c.gateway_id=$2 AND g.owner_id=$3
            FOR UPDATE OF c,g
            "#,
        )
        .bind(certificate_id)
        .bind(gateway_id)
        .bind(owner_id)
        .fetch_optional(&mut *tx)
        .await?
        .ok_or(ApiError::NotFound)?;
        if certificate.get::<String, _>("status") == "revoked" {
            tx.commit().await?;
            return Ok(());
        }
        let replacement_exists: bool = sqlx::query_scalar(
            r#"
            SELECT EXISTS(
                SELECT 1 FROM gateway_certificates
                WHERE gateway_id=$1 AND id<>$2 AND status='active'
                  AND valid_after<=clock_timestamp() AND valid_until>clock_timestamp()
                  AND last_proof_at IS NOT NULL
            )
            "#,
        )
        .bind(gateway_id)
        .bind(certificate_id)
        .fetch_one(&mut *tx)
        .await?;
        if !replacement_exists {
            return Err(ApiError::Conflict(
                "cannot revoke the gateway's only proven certificate",
            ));
        }
        sqlx::query(
            "UPDATE gateway_certificates SET status='revoked',revoked_at=clock_timestamp() WHERE id=$1",
        )
        .bind(certificate_id)
        .execute(&mut *tx)
        .await?;
        sqlx::query(
            "UPDATE gateway_certificate_rotations SET status='cancelled' WHERE old_certificate_id=$1 AND status='pending'",
        )
        .bind(certificate_id)
        .execute(&mut *tx)
        .await?;
        insert_audit(
            &mut tx,
            "owner",
            Some(owner_id),
            Some(gateway_id),
            None,
            "gateway.certificate_revoked",
            "gateway_certificate",
            certificate_id.to_string(),
            json!({}),
        )
        .await?;
        tx.commit().await?;
        Ok(())
    }

    pub async fn list_actions(
        &self,
        owner_id: Uuid,
        companion_id: Uuid,
        limit: i64,
    ) -> Result<Vec<ActionView>, ApiError> {
        ensure_owner_companion(&self.pool, owner_id, companion_id).await?;
        let rows = sqlx::query(
            r#"
            SELECT id,companion_id,action_type,parameters,status::text,
                   created_at,expires_at,completed_at,result
            FROM remote_actions
            WHERE owner_id=$1 AND companion_id=$2
            ORDER BY created_at DESC LIMIT $3
            "#,
        )
        .bind(owner_id)
        .bind(companion_id)
        .bind(limit.clamp(1, 200))
        .fetch_all(&self.pool)
        .await?;
        Ok(rows.iter().map(action_from_row).collect())
    }

    /// Records only the transport write attempt. `remote_actions.status` stays
    /// queued until a companion-authenticated action_acceptance or result.
    pub async fn record_action_delivery_attempt(
        &self,
        gateway: &Gateway,
        action_id: Uuid,
        instance_id: Uuid,
        socket_accepted: bool,
    ) -> Result<(), ApiError> {
        sqlx::query(
            r#"
            INSERT INTO action_delivery_attempts
                (action_id,gateway_id,instance_id,accepted_by_socket)
            SELECT a.id,$2,$3,$4
            FROM remote_actions a
            JOIN gateway_companions gc ON gc.companion_id=a.companion_id
            WHERE a.id=$1 AND gc.gateway_id=$2 AND gc.unbound_at IS NULL
            "#,
        )
        .bind(action_id)
        .bind(gateway.id)
        .bind(instance_id)
        .bind(socket_accepted)
        .execute(&self.pool)
        .await?;
        Ok(())
    }
}

async fn persist_payload(
    tx: &mut Transaction<'_, Postgres>,
    gateway: &Gateway,
    envelope: &DeviceEnvelope,
    sequence: i64,
    payload: &DevicePayload,
) -> Result<(), ApiError> {
    match payload {
        DevicePayload::EventBatch { events } => {
            for event in events {
                sqlx::query(
                    r#"
                    INSERT INTO device_events
                        (event_id,companion_id,gateway_id,envelope_sequence,event_type,
                         observed_epoch,boot_id,monotonic_ms,body)
                    VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)
                    ON CONFLICT (companion_id,event_id) DO NOTHING
                    "#,
                )
                .bind(event.event_id)
                .bind(envelope.companion_id)
                .bind(gateway.id)
                .bind(sequence)
                .bind(&event.event_type)
                .bind(event.observed.epoch)
                .bind(i64::from(event.observed.boot_id))
                .bind(i64::from(event.observed.monotonic_ms))
                .bind(Value::Object(event.body.clone()))
                .execute(&mut **tx)
                .await?;
                if event.event_type == "companion.snapshot" {
                    persist_projection(
                        tx,
                        envelope.companion_id,
                        event.event_id,
                        sequence,
                        &event.body,
                    )
                    .await?;
                }
            }
        }
        DevicePayload::PeerSnapshot { peers } => {
            for peer in peers {
                let public_key = hex::decode(&peer.public_key)
                    .map_err(|_| ApiError::Invalid("invalid peer public key"))?;
                let (scope, rssi, snr) = peer
                    .signal
                    .as_ref()
                    .map(|signal| {
                        (
                            Some(signal.scope.as_str()),
                            Some(signal.rssi_deci_dbm),
                            Some(signal.snr_deci_db),
                        )
                    })
                    .unwrap_or((None, None, None));
                sqlx::query(
                    r#"
                    INSERT INTO peer_history
                        (companion_id,public_key,role,display_name,seen_count,
                         last_seen_epoch,last_seen_boot_id,last_seen_monotonic_ms,
                         signal_scope,rssi_deci_dbm,snr_deci_db,journal_sequence,
                         source_envelope_sequence)
                    VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)
                    ON CONFLICT (companion_id,public_key) DO UPDATE SET
                      role=EXCLUDED.role, display_name=EXCLUDED.display_name,
                      seen_count=GREATEST(peer_history.seen_count,EXCLUDED.seen_count),
                      last_seen_epoch=EXCLUDED.last_seen_epoch,
                      last_seen_boot_id=EXCLUDED.last_seen_boot_id,
                      last_seen_monotonic_ms=EXCLUDED.last_seen_monotonic_ms,
                      signal_scope=EXCLUDED.signal_scope,rssi_deci_dbm=EXCLUDED.rssi_deci_dbm,
                      snr_deci_db=EXCLUDED.snr_deci_db,journal_sequence=EXCLUDED.journal_sequence,
                      source_envelope_sequence=EXCLUDED.source_envelope_sequence,
                      updated_at=clock_timestamp()
                    WHERE peer_history.source_envelope_sequence < EXCLUDED.source_envelope_sequence
                    "#,
                )
                .bind(envelope.companion_id)
                .bind(public_key)
                .bind(peer.role.as_str())
                .bind(&peer.name)
                .bind(i64::from(peer.seen_count))
                .bind(peer.last_seen.epoch)
                .bind(i64::from(peer.last_seen.boot_id))
                .bind(i64::from(peer.last_seen.monotonic_ms))
                .bind(scope)
                .bind(rssi)
                .bind(snr)
                .bind(i64::from(peer.journal_sequence))
                .bind(sequence)
                .execute(&mut **tx)
                .await?;
            }
        }
        DevicePayload::ActionAcceptance {
            action_id,
            accepted_epoch,
        } => {
            sqlx::query(
                r#"
                UPDATE remote_actions SET
                    status='delivered',
                    first_delivered_at=COALESCE(first_delivered_at,clock_timestamp()),
                    device_accepted_epoch=$3
                WHERE id=$1 AND companion_id=$2 AND status='queued'
                  AND expires_at>clock_timestamp()
                "#,
            )
            .bind(action_id)
            .bind(envelope.companion_id)
            .bind(accepted_epoch)
            .execute(&mut **tx)
            .await?;
        }
        DevicePayload::ActionResult {
            action_id,
            status,
            completed_epoch,
            result,
        } => {
            let completed_at = DateTime::from_timestamp(*completed_epoch, 0)
                .ok_or(ApiError::Invalid("invalid action completion time"))?;
            sqlx::query(
                r#"
                UPDATE remote_actions SET status=$3::action_status,
                    completed_at=$4,result=$5
                WHERE id=$1 AND companion_id=$2
                  AND status IN ('queued','delivered','expired')
                "#,
            )
            .bind(action_id)
            .bind(envelope.companion_id)
            .bind(status.as_db())
            .bind(completed_at)
            .bind(Value::Object(result.clone()))
            .execute(&mut **tx)
            .await?;
        }
        DevicePayload::Heartbeat { .. } | DevicePayload::GatewayHello { .. } => {}
    }
    Ok(())
}

async fn persist_projection(
    tx: &mut Transaction<'_, Postgres>,
    companion_id: Uuid,
    event_id: Uuid,
    sequence: i64,
    body: &Map<String, Value>,
) -> Result<(), ApiError> {
    let object = |name: &'static str| -> Result<Value, ApiError> {
        let value = body.get(name).cloned().unwrap_or_else(|| json!({}));
        if !value.is_object() {
            return Err(ApiError::Invalid(
                "companion snapshot sections must be objects",
            ));
        }
        Ok(value)
    };
    sqlx::query(
        r#"
        INSERT INTO companion_state_projections
            (companion_id,source_event_id,source_envelope_sequence,
             vitals,mood,bond,evolution,mesh)
        VALUES ($1,$2,$3,$4,$5,$6,$7,$8)
        ON CONFLICT (companion_id) DO UPDATE SET
          source_event_id=EXCLUDED.source_event_id,
          source_envelope_sequence=EXCLUDED.source_envelope_sequence,
          vitals=EXCLUDED.vitals,mood=EXCLUDED.mood,bond=EXCLUDED.bond,
          evolution=EXCLUDED.evolution,mesh=EXCLUDED.mesh,
          updated_at=clock_timestamp()
        WHERE companion_state_projections.source_envelope_sequence < EXCLUDED.source_envelope_sequence
        "#,
    )
    .bind(companion_id)
    .bind(event_id)
    .bind(sequence)
    .bind(object("vitals")?)
    .bind(object("mood")?)
    .bind(object("bond")?)
    .bind(object("evolution")?)
    .bind(object("mesh")?)
    .execute(&mut **tx)
    .await?;
    Ok(())
}

async fn ensure_owner_companion(
    pool: &PgPool,
    owner_id: Uuid,
    companion_id: Uuid,
) -> Result<(), ApiError> {
    let exists: bool =
        sqlx::query_scalar("SELECT EXISTS(SELECT 1 FROM companions WHERE id=$1 AND owner_id=$2)")
            .bind(companion_id)
            .bind(owner_id)
            .fetch_one(pool)
            .await?;
    if !exists {
        return Err(ApiError::NotFound);
    }
    Ok(())
}

async fn load_completed_enrollment(
    tx: &mut Transaction<'_, Postgres>,
    enrollment_id: Uuid,
    request_sha256: &[u8; 32],
) -> Result<EnrollmentIssuanceResult, ApiError> {
    let row = sqlx::query(
        r#"
        SELECT e.companion_id,e.claimed_gateway_id,c.key_version,c.leaf_der,
               c.chain_der,e.hpke_enc,e.hpke_ciphertext,e.claim_request_sha256
        FROM enrollment_challenges e
        JOIN companion_certificates c ON c.id=e.device_certificate_id
        WHERE e.id=$1 AND e.status='claimed'
        "#,
    )
    .bind(enrollment_id)
    .fetch_optional(&mut **tx)
    .await?
    .ok_or(ApiError::Conflict(
        "completed enrollment result is unavailable",
    ))?;
    if exact_bytes::<32>(row.get::<Vec<u8>, _>("claim_request_sha256"))? != *request_sha256 {
        return Err(ApiError::Conflict(
            "enrollment token was bound to another request",
        ));
    }
    Ok(EnrollmentIssuanceResult {
        companion_id: row.get("companion_id"),
        gateway_id: row.get("claimed_gateway_id"),
        key_version: u32::try_from(row.get::<i32, _>("key_version")).map_err(ApiError::internal)?,
        certificate_der: row.get("leaf_der"),
        certificate_chain_der: row.get("chain_der"),
        hpke_enc: exact_bytes(row.get::<Vec<u8>, _>("hpke_enc"))?,
        hpke_ciphertext: exact_bytes(row.get::<Vec<u8>, _>("hpke_ciphertext"))?,
    })
}

async fn load_completed_gateway_bootstrap(
    tx: &mut Transaction<'_, Postgres>,
    bootstrap_id: Uuid,
    request_sha256: &[u8; 32],
) -> Result<GatewayBootstrapResult, ApiError> {
    let row = sqlx::query(
        r#"
        SELECT b.gateway_id,c.leaf_der,c.chain_der,b.claim_request_sha256
        FROM gateway_bootstraps b
        JOIN gateway_certificates c ON c.id=b.certificate_id
        WHERE b.id=$1 AND b.status='claimed'
        "#,
    )
    .bind(bootstrap_id)
    .fetch_optional(&mut **tx)
    .await?
    .ok_or(ApiError::Conflict(
        "completed gateway bootstrap result is unavailable",
    ))?;
    if exact_bytes::<32>(row.get::<Vec<u8>, _>("claim_request_sha256"))? != *request_sha256 {
        return Err(ApiError::Conflict(
            "bootstrap token was bound to another CSR",
        ));
    }
    Ok(GatewayBootstrapResult {
        gateway_id: row.get("gateway_id"),
        certificate_der: row.get("leaf_der"),
        certificate_chain_der: row.get("chain_der"),
    })
}

#[allow(clippy::too_many_arguments)]
async fn insert_audit(
    tx: &mut Transaction<'_, Postgres>,
    actor_type: &str,
    owner_id: Option<Uuid>,
    gateway_id: Option<Uuid>,
    companion_id: Option<Uuid>,
    action: &str,
    target_type: &str,
    target_id: String,
    details: Value,
) -> Result<(), ApiError> {
    sqlx::query(
        r#"
        INSERT INTO audit_log
            (actor_type,actor_owner_id,actor_gateway_id,companion_id,
             action,target_type,target_id,details)
        VALUES ($1,$2,$3,$4,$5,$6,$7,$8)
        "#,
    )
    .bind(actor_type)
    .bind(owner_id)
    .bind(gateway_id)
    .bind(companion_id)
    .bind(action)
    .bind(target_type)
    .bind(target_id)
    .bind(details)
    .execute(&mut **tx)
    .await?;
    Ok(())
}

fn action_from_row(row: &PgRow) -> ActionView {
    ActionView {
        id: row.get("id"),
        companion_id: row.get("companion_id"),
        action_type: row.get("action_type"),
        parameters: row.get("parameters"),
        status: row.get("status"),
        created_at: row.get("created_at"),
        expires_at: row.get("expires_at"),
        completed_at: row.get("completed_at"),
        result: row.get("result"),
    }
}

fn remote_action_from_row(row: &PgRow) -> Result<RemoteAction, ApiError> {
    let key_version =
        u32::try_from(row.get::<i32, _>("key_version")).map_err(ApiError::internal)?;
    let nonce: [u8; 16] = exact_bytes(row.get::<Vec<u8>, _>("wire_nonce"))?;
    let signature: [u8; 32] = exact_bytes(row.get::<Vec<u8>, _>("wire_signature"))?;
    let params: Vec<u8> = row.get("wire_parameters");
    Ok(RemoteAction {
        schema: REMOTE_ACTION_SCHEMA.to_owned(),
        action_id: row.get("id"),
        companion_id: row.get("companion_id"),
        key_version,
        nonce_b64: URL_SAFE_NO_PAD.encode(nonce),
        action_type: row.get("action_type"),
        created_epoch: row
            .get::<DateTime<Utc>, _>("created_at")
            .timestamp()
            .to_string(),
        expires_epoch: row
            .get::<DateTime<Utc>, _>("expires_at")
            .timestamp()
            .to_string(),
        params_b64: URL_SAFE_NO_PAD.encode(params),
        signature_b64: URL_SAFE_NO_PAD.encode(signature),
    })
}

fn peer_json(row: PgRow) -> Value {
    let public_key = row.get::<Vec<u8>, _>("public_key");
    json!({
        "public_key_b64": URL_SAFE_NO_PAD.encode(&public_key),
        "public_key_hex": hex::encode_upper(&public_key),
        "role": row.get::<String,_>("role"),
        "name": row.get::<String,_>("display_name"),
        "seen_count": row.get::<i64,_>("seen_count"),
        "last_seen": {
            "epoch": row.get::<Option<i64>,_>("last_seen_epoch"),
            "boot_id": row.get::<i64,_>("last_seen_boot_id"),
            "monotonic_ms": row.get::<i64,_>("last_seen_monotonic_ms")
        },
        "signal": row.get::<Option<String>,_>("signal_scope").map(|scope| json!({
            "scope": scope,
            "rssi_deci_dbm": row.get::<Option<i16>,_>("rssi_deci_dbm"),
            "snr_deci_db": row.get::<Option<i16>,_>("snr_deci_db")
        })),
        "journal_sequence": row.get::<i64,_>("journal_sequence"),
        "updated_at": row.get::<DateTime<Utc>,_>("updated_at")
    })
}

fn owner_from_row(row: &PgRow) -> Owner {
    Owner {
        id: row.get("id"),
        issuer: row.get("issuer"),
        subject: row.get("subject"),
    }
}

fn device_relay_from_row(row: &PgRow) -> Result<DeviceRelay, ApiError> {
    let gateway_id: Uuid = row.get("gateway_id");
    let owner_id: Uuid = row.get("owner_id");
    Ok(DeviceRelay {
        relay: MobileRelay {
            view: MobileRelayView {
                installation_id: row.get("installation_id"),
                gateway_id,
                created_at: row.get("created_at"),
            },
            gateway: Gateway {
                id: gateway_id,
                owner_id,
                certificate_id: None,
                certificate_sha256: None,
            },
        },
        activated: row
            .get::<Option<DateTime<Utc>>, _>("activated_at")
            .is_some(),
    })
}

fn account_deletion_from_row(row: &PgRow) -> AccountDeletionView {
    AccountDeletionView {
        status: row.get("status"),
        requested_at: row.get("requested_at"),
        execute_after: row.get("execute_after"),
        cancelled_at: row.get("cancelled_at"),
        completed_at: row.get("completed_at"),
    }
}

fn public_contact_from_row(row: &PgRow) -> PublicContactView {
    PublicContactView {
        id: row.get("id"),
        category: row.get("category"),
        reply_contact: row.get("reply_contact"),
        message: row.get("message"),
        status: row.get("status"),
        created_at: row.get("created_at"),
        resolved_at: row.get("resolved_at"),
    }
}

fn exact_bytes<const N: usize>(bytes: Vec<u8>) -> Result<[u8; N], ApiError> {
    bytes.try_into().map_err(|_| ApiError::Unavailable)
}

fn validate_identity_text(hardware_uid: &str, display_name: &str) -> Result<(), ApiError> {
    if !(4..=128).contains(&hardware_uid.len())
        || !hardware_uid
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':' | b'.'))
        || display_name.trim() != display_name
        || !(1..=80).contains(&display_name.len())
        || display_name.chars().any(char::is_control)
    {
        return Err(ApiError::Invalid("invalid companion identity"));
    }
    Ok(())
}

fn validate_display_name(display_name: &str) -> Result<(), ApiError> {
    if display_name.trim() != display_name
        || !(1..=80).contains(&display_name.len())
        || display_name.chars().any(char::is_control)
    {
        return Err(ApiError::Invalid("invalid gateway display name"));
    }
    Ok(())
}

fn map_conflict(error: sqlx::Error) -> ApiError {
    if error
        .as_database_error()
        .is_some_and(|database| database.is_unique_violation())
    {
        ApiError::Conflict("record already exists")
    } else {
        ApiError::internal(error)
    }
}
