-- A native mobile installation may relay an owner's physical companions
-- without possessing a gateway client certificate. The logical gateway UUID
-- remains part of every device-signed transcript; the installation binding is
-- immutable and owner-scoped.
CREATE UNIQUE INDEX gateways_id_owner_unique
    ON gateways(id, owner_id);

CREATE TABLE mobile_relay_installations (
    installation_id UUID PRIMARY KEY,
    owner_id UUID NOT NULL REFERENCES owners(id) ON DELETE CASCADE,
    gateway_id UUID NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT mobile_relay_gateway_owner
        FOREIGN KEY (gateway_id, owner_id)
        REFERENCES gateways(id, owner_id) ON DELETE CASCADE
);

CREATE INDEX mobile_relay_installations_owner_idx
    ON mobile_relay_installations(owner_id, created_at);

COMMENT ON TABLE mobile_relay_installations IS
    'Immutable owner-authenticated native installation to logical gateway bindings; no mobile mTLS identity or device secret material.';
