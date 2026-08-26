-- A raw encounter code is never persisted. Its SHA-256 digest is the stable
-- redemption identity; the first successful request binds that identity to
-- exactly one Kitsu hardware UID and one published pack.
CREATE TABLE pet_pack_unlocks (
    code_digest BYTEA PRIMARY KEY CHECK (octet_length(code_digest) = 32),
    code_id BIGINT NOT NULL CHECK (code_id BETWEEN 1 AND 4294967295),
    hardware_uid TEXT NOT NULL,
    pack_id BIGINT NOT NULL CHECK (pack_id BETWEEN 1 AND 4294967295),
    rarity TEXT NOT NULL CHECK (rarity IN (
        'common', 'uncommon', 'rare', 'very_rare', 'epic',
        'legendary', 'mythical'
    )),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    last_downloaded_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT pet_pack_unlock_hardware_uid_format
        CHECK (hardware_uid ~ '^KT[0-9A-F]{4}$')
);

CREATE INDEX pet_pack_unlock_hardware_idx
    ON pet_pack_unlocks(hardware_uid, created_at DESC);
