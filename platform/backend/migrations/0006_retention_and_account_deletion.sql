CREATE TABLE account_deletion_requests (
    owner_id UUID PRIMARY KEY REFERENCES owners(id) ON DELETE CASCADE,
    status TEXT NOT NULL CHECK (status IN ('pending', 'cancelled', 'completed')),
    requested_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    execute_after TIMESTAMPTZ NOT NULL,
    cancelled_at TIMESTAMPTZ,
    completed_at TIMESTAMPTZ,
    CONSTRAINT account_deletion_timeline CHECK (execute_after > requested_at)
);
CREATE INDEX account_deletion_due_idx
    ON account_deletion_requests(execute_after) WHERE status='pending';

-- Application retention is the sole intentional exception to append-only
-- audit history. It is transaction-scoped and used only by the internal
-- retention/deletion worker.
CREATE OR REPLACE FUNCTION reject_audit_mutation() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF current_setting('kitsu.retention_mode', true) = 'on' THEN
        IF TG_OP = 'DELETE' THEN
            RETURN OLD;
        END IF;
        RETURN NEW;
    END IF;
    RAISE EXCEPTION 'audit_log is append-only';
END;
$$;
