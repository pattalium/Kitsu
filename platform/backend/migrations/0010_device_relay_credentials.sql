-- Account-free native relay credentials are scoped to one immutable mobile
-- installation.  The application chooses 256 random bits and the service
-- stores only their digest.  A pending credential may create and submit a
-- first-use enrollment claim.  A successfully completed claim activates it;
-- before then, it cannot carry device traffic or open an action session.
CREATE TABLE mobile_relay_credentials (
    installation_id UUID PRIMARY KEY
        REFERENCES mobile_relay_installations(installation_id) ON DELETE CASCADE,
    token_digest BYTEA NOT NULL UNIQUE CHECK (octet_length(token_digest) = 32),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    activated_at TIMESTAMPTZ
);

CREATE INDEX mobile_relay_credentials_active_idx
    ON mobile_relay_credentials(installation_id) WHERE activated_at IS NOT NULL;

COMMENT ON TABLE mobile_relay_credentials IS
    'Account-free installation credentials. Only SHA-256 digests are stored; activation requires a completed device-key enrollment claim.';
