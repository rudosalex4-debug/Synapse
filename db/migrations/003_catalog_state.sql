-- Written by seed in the same transaction as topics and facets.
-- A new image must not report readiness against an older catalog.
CREATE TABLE catalog_state (
    singleton boolean PRIMARY KEY DEFAULT true CHECK (singleton),
    version text NOT NULL,
    checksum text NOT NULL CHECK (checksum ~ '^[0-9a-f]{64}$'),
    seeded_at timestamptz NOT NULL DEFAULT now()
);
