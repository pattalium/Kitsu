-- Initial enrollment and steady-state device traffic intentionally terminate
-- on different listeners.  A device cannot use the mTLS-only steady listener
-- until it has obtained its first certificate from the bootstrap listener.
ALTER TABLE gateway_lan_profiles
    ADD COLUMN bootstrap_port INTEGER NOT NULL DEFAULT 7442,
    ADD CONSTRAINT gateway_lan_profile_bootstrap_port
        CHECK (bootstrap_port BETWEEN 1 AND 65535),
    ADD CONSTRAINT gateway_lan_profile_distinct_ports
        CHECK (bootstrap_port <> port);

ALTER TABLE gateway_lan_profiles
    ALTER COLUMN bootstrap_port DROP DEFAULT;

COMMENT ON COLUMN gateway_lan_profiles.bootstrap_port IS
    'Unauthenticated first-certificate bootstrap listener; never a public Internet endpoint.';

COMMENT ON COLUMN gateway_lan_profiles.port IS
    'Steady-state device mTLS listener; never a public Internet endpoint.';
