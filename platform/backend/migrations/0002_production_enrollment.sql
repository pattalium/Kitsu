-- Production device enrollment and first-gateway bootstrap.  Private keys and
-- plaintext companion secrets are deliberately absent from this schema.

ALTER TYPE enrollment_status ADD VALUE IF NOT EXISTS 'issuing';
CREATE TYPE certificate_status AS ENUM ('active', 'revoked', 'expired');
CREATE TYPE gateway_bootstrap_status AS ENUM
    ('pending', 'issuing', 'claimed', 'expired', 'cancelled');

CREATE TABLE companion_certificates (
    id UUID PRIMARY KEY,
    companion_id UUID NOT NULL REFERENCES companions(id) ON DELETE CASCADE,
    key_version INTEGER NOT NULL CHECK (key_version > 0),
    serial_hex TEXT NOT NULL,
    certificate_sha256 BYTEA NOT NULL UNIQUE CHECK (octet_length(certificate_sha256) = 32),
    subject_public_key_sha256 BYTEA NOT NULL CHECK (octet_length(subject_public_key_sha256) = 32),
    san_uri TEXT NOT NULL,
    leaf_der BYTEA NOT NULL CHECK (octet_length(leaf_der) BETWEEN 1 AND 65536),
    chain_der BYTEA[] NOT NULL CHECK (cardinality(chain_der) BETWEEN 1 AND 8),
    provider_id TEXT NOT NULL CHECK (char_length(provider_id) BETWEEN 1 AND 2048),
    status certificate_status NOT NULL DEFAULT 'active',
    valid_after TIMESTAMPTZ NOT NULL,
    valid_until TIMESTAMPTZ NOT NULL,
    issued_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    revoked_at TIMESTAMPTZ,
    revocation_reason TEXT,
    CONSTRAINT companion_certificate_validity CHECK (valid_until > valid_after),
    CONSTRAINT companion_certificate_serial_nonempty CHECK (length(serial_hex) BETWEEN 2 AND 80),
    CONSTRAINT companion_certificate_san_format CHECK (
        san_uri = 'urn:kitsu:companion:' || lower(companion_id::text)
    ),
    CONSTRAINT companion_certificate_identity_unique UNIQUE (id, companion_id),
    FOREIGN KEY (companion_id, key_version)
        REFERENCES companion_secret_versions(companion_id, key_version)
        ON DELETE CASCADE
);
CREATE INDEX companion_certificate_lifecycle_idx
    ON companion_certificates(companion_id, status, valid_until);
CREATE INDEX companion_certificate_san_idx
    ON companion_certificates(san_uri);

ALTER TABLE enrollment_challenges
    ADD COLUMN claim_request_sha256 BYTEA CHECK (
        claim_request_sha256 IS NULL OR octet_length(claim_request_sha256) = 32
    ),
    ADD COLUMN provider_job_id TEXT CHECK (
        provider_job_id IS NULL OR char_length(provider_job_id) BETWEEN 1 AND 2048
    ),
    ADD COLUMN provider_ambiguous BOOLEAN NOT NULL DEFAULT FALSE,
    ADD COLUMN provider_started_at TIMESTAMPTZ,
    ADD COLUMN issuance_id UUID,
    ADD COLUMN issuance_started_at TIMESTAMPTZ,
    ADD COLUMN reserved_companion_id UUID,
    ADD COLUMN device_certificate_id UUID REFERENCES companion_certificates(id),
    ADD COLUMN hpke_enc BYTEA CHECK (hpke_enc IS NULL OR octet_length(hpke_enc) = 65),
    ADD COLUMN hpke_ciphertext BYTEA CHECK (
        hpke_ciphertext IS NULL OR octet_length(hpke_ciphertext) = 48
    ),
    ADD CONSTRAINT enrollment_production_result_state CHECK (
        device_certificate_id IS NULL OR (
            status = 'claimed' AND companion_id IS NOT NULL AND
            reserved_companion_id IS NOT NULL AND companion_id = reserved_companion_id AND
            claimed_gateway_id IS NOT NULL AND claimed_at IS NOT NULL AND
            claim_request_sha256 IS NOT NULL AND provider_job_id IS NOT NULL AND
            NOT provider_ambiguous AND hpke_enc IS NOT NULL AND hpke_ciphertext IS NOT NULL
        )
    ),
    ADD CONSTRAINT enrollment_provider_attempt_state CHECK (
        (provider_job_id IS NULL AND NOT provider_ambiguous) OR
        provider_started_at IS NOT NULL
    ),
    ADD CONSTRAINT enrollment_certificate_matches_companion
        FOREIGN KEY (device_certificate_id, companion_id)
        REFERENCES companion_certificates(id, companion_id);
-- Existing installations may contain proxy-provisioned gateway certificates.
-- The production bootstrap path always fills these nullable lifecycle fields;
-- nullability keeps this migration forward-only for those installations.
ALTER TABLE gateway_certificates
    ADD COLUMN serial_hex TEXT,
    ADD COLUMN subject_public_key_sha256 BYTEA CHECK (
        subject_public_key_sha256 IS NULL OR octet_length(subject_public_key_sha256) = 32
    ),
    ADD COLUMN san_uri TEXT,
    ADD COLUMN leaf_der BYTEA CHECK (leaf_der IS NULL OR octet_length(leaf_der) BETWEEN 1 AND 65536),
    ADD COLUMN chain_der BYTEA[],
    ADD COLUMN provider_id TEXT,
    ADD CONSTRAINT gateway_certificate_identity_unique UNIQUE (id, gateway_id),
    ADD CONSTRAINT gateway_production_certificate_complete CHECK (
        provider_id IS NULL OR (
            char_length(provider_id) BETWEEN 1 AND 2048 AND
            serial_hex IS NOT NULL AND char_length(serial_hex) BETWEEN 2 AND 80 AND
            subject_public_key_sha256 IS NOT NULL AND san_uri IS NOT NULL AND
            leaf_der IS NOT NULL AND chain_der IS NOT NULL AND
            cardinality(chain_der) BETWEEN 1 AND 8
        )
    ),
    ADD CONSTRAINT gateway_certificate_san_format CHECK (
        san_uri IS NULL OR san_uri = 'urn:kitsu:gateway:' || lower(gateway_id::text)
    );
-- Certificate rotation deliberately produces more than one certificate with
-- the same stable companion/gateway URI SAN. Fingerprints, not SANs, identify
-- individual certificates.
CREATE INDEX gateway_certificate_san_idx
    ON gateway_certificates(san_uri) WHERE san_uri IS NOT NULL;

CREATE TABLE gateway_bootstraps (
    id UUID PRIMARY KEY,
    owner_id UUID NOT NULL REFERENCES owners(id) ON DELETE CASCADE,
    display_name TEXT NOT NULL,
    token_digest BYTEA NOT NULL UNIQUE CHECK (octet_length(token_digest) = 32),
    status gateway_bootstrap_status NOT NULL DEFAULT 'pending',
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    expires_at TIMESTAMPTZ NOT NULL,
    claim_request_sha256 BYTEA CHECK (
        claim_request_sha256 IS NULL OR octet_length(claim_request_sha256) = 32
    ),
    issuance_id UUID,
    issuance_started_at TIMESTAMPTZ,
    provider_job_id TEXT CHECK (
        provider_job_id IS NULL OR char_length(provider_job_id) BETWEEN 1 AND 2048
    ),
    provider_ambiguous BOOLEAN NOT NULL DEFAULT FALSE,
    provider_started_at TIMESTAMPTZ,
    reserved_gateway_id UUID,
    gateway_id UUID REFERENCES gateways(id),
    certificate_id UUID REFERENCES gateway_certificates(id),
    claimed_at TIMESTAMPTZ,
    CONSTRAINT gateway_bootstrap_expiry CHECK (expires_at > created_at),
    CONSTRAINT gateway_bootstrap_provider_attempt_state CHECK (
        (provider_job_id IS NULL AND NOT provider_ambiguous) OR
        provider_started_at IS NOT NULL
    ),
    CONSTRAINT gateway_bootstrap_name_length CHECK (
        char_length(display_name) BETWEEN 1 AND 80
    ),
    CONSTRAINT gateway_bootstrap_issuance_state CHECK (
        status <> 'issuing' OR (
            claim_request_sha256 IS NOT NULL AND reserved_gateway_id IS NOT NULL AND
            ((issuance_id IS NOT NULL AND issuance_started_at IS NOT NULL) OR
             (issuance_id IS NULL AND issuance_started_at IS NULL AND
              (provider_job_id IS NOT NULL OR provider_ambiguous)))
        )
    ),
    CONSTRAINT gateway_bootstrap_result_state CHECK (
        status <> 'claimed' OR (
            gateway_id IS NOT NULL AND reserved_gateway_id IS NOT NULL AND
            gateway_id = reserved_gateway_id AND certificate_id IS NOT NULL AND
            claimed_at IS NOT NULL AND claim_request_sha256 IS NOT NULL AND
            provider_job_id IS NOT NULL AND NOT provider_ambiguous
        )
    ),
    CONSTRAINT gateway_bootstrap_certificate_matches_gateway
        FOREIGN KEY (certificate_id, gateway_id)
        REFERENCES gateway_certificates(id, gateway_id)
);
CREATE INDEX gateway_bootstrap_pending_idx
    ON gateway_bootstraps(expires_at) WHERE status = 'pending';
CREATE INDEX gateway_bootstrap_issuing_idx
    ON gateway_bootstraps(issuance_started_at) WHERE status = 'issuing';

COMMENT ON TABLE companion_certificates IS
    'Device mTLS certificate lifecycle; contains public certificate material only.';
COMMENT ON TABLE gateway_bootstraps IS
    'Owner-authorized, one-use gateway mTLS bootstrap. Only the token digest is retained.';
