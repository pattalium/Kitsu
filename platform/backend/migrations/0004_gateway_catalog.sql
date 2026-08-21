-- Safe, public-key-only LAN discovery material for the native companion apps.
-- The certificate_id binding ensures a profile must be refreshed by the
-- currently authenticated gateway after client-certificate rotation.
CREATE TABLE gateway_lan_profiles (
    gateway_id UUID PRIMARY KEY REFERENCES gateways(id) ON DELETE CASCADE,
    certificate_id UUID NOT NULL,
    display_name TEXT NOT NULL,
    host TEXT NOT NULL,
    port INTEGER NOT NULL,
    server_name TEXT NOT NULL,
    ca_cert_der BYTEA NOT NULL,
    spki_sha256 BYTEA NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT gateway_lan_profile_certificate
        FOREIGN KEY (certificate_id, gateway_id)
        REFERENCES gateway_certificates(id, gateway_id) ON DELETE RESTRICT,
    CONSTRAINT gateway_lan_profile_display_name
        CHECK (char_length(display_name) BETWEEN 1 AND 80),
    CONSTRAINT gateway_lan_profile_host
        CHECK (char_length(host) BETWEEN 1 AND 253 AND host = lower(host)),
    CONSTRAINT gateway_lan_profile_port CHECK (port BETWEEN 1 AND 65535),
    CONSTRAINT gateway_lan_profile_server_name
        CHECK (char_length(server_name) BETWEEN 1 AND 253 AND server_name = lower(server_name)),
    CONSTRAINT gateway_lan_profile_ca_der
        CHECK (octet_length(ca_cert_der) BETWEEN 1 AND 8192),
    CONSTRAINT gateway_lan_profile_spki CHECK (octet_length(spki_sha256) = 32)
);

COMMENT ON TABLE gateway_lan_profiles IS
    'Certificate-bound, owner-visible LAN connection metadata; never private key material.';
