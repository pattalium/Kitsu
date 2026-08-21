-- Account deletion is not complete until the exact Keycloak identity has been
-- disabled at the issuer. A one-way tombstone prevents a still-valid access
-- token from recreating an owner while that external deletion is retried.

ALTER TABLE account_deletion_requests
    DROP CONSTRAINT account_deletion_requests_status_check;
ALTER TABLE account_deletion_requests
    ADD CONSTRAINT account_deletion_requests_status_check
    CHECK (status IN ('pending', 'deleting', 'cancelled', 'completed'));
ALTER TABLE account_deletion_requests
    ADD COLUMN identity_revoked_at TIMESTAMPTZ;

DROP INDEX account_deletion_due_idx;
CREATE INDEX account_deletion_due_idx
    ON account_deletion_requests(execute_after)
    WHERE status IN ('pending', 'deleting');

CREATE TABLE deleted_oidc_subjects (
    issuer_subject_sha256 BYTEA PRIMARY KEY
        CHECK (octet_length(issuer_subject_sha256) = 32),
    deleted_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

COMMENT ON TABLE deleted_oidc_subjects IS
    'One-way issuer/subject tombstones that prevent deleted identities from recreating accounts.';
