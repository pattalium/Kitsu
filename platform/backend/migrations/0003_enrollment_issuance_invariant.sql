-- `issuing` was added to enrollment_status in migration 0002. PostgreSQL
-- requires that ALTER TYPE transaction to commit before the new enum value can
-- appear in a stored CHECK expression.
ALTER TABLE enrollment_challenges
    ADD CONSTRAINT enrollment_issuance_state CHECK (
        status <> 'issuing' OR (
            claim_request_sha256 IS NOT NULL AND reserved_companion_id IS NOT NULL AND
            ((issuance_id IS NOT NULL AND issuance_started_at IS NOT NULL) OR
             (issuance_id IS NULL AND issuance_started_at IS NULL AND
              (provider_job_id IS NOT NULL OR provider_ambiguous)))
        )
    );
