ALTER TABLE users
 ADD COLUMN accepted_rules_version text,
 ADD COLUMN rules_accepted_at timestamptz,
 ADD CONSTRAINT users_rules_acceptance_pair CHECK (
  (accepted_rules_version IS NULL AND rules_accepted_at IS NULL)
  OR (accepted_rules_version IS NOT NULL AND char_length(accepted_rules_version) BETWEEN 1 AND 64 AND rules_accepted_at IS NOT NULL)
 );
-- Existing and new users accept explicitly. No inferred acceptance or backfill.
