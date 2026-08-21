CREATE TABLE public_contact_messages (
    id UUID PRIMARY KEY,
    category TEXT NOT NULL CHECK (category IN ('security', 'privacy', 'abuse', 'service')),
    reply_contact TEXT NOT NULL CHECK (char_length(reply_contact) BETWEEN 3 AND 320),
    message TEXT NOT NULL CHECK (char_length(message) BETWEEN 20 AND 4000),
    source_address_digest BYTEA NOT NULL CHECK (octet_length(source_address_digest) = 32),
    status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'resolved')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    resolved_at TIMESTAMPTZ
);
CREATE INDEX public_contact_open_idx
    ON public_contact_messages(created_at) WHERE status='open';

COMMENT ON TABLE public_contact_messages IS
    'Public web contact intake; source addresses are stored only as keyed one-way digests.';
