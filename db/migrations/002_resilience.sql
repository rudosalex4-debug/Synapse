ALTER TABLE users ADD COLUMN revision bigint NOT NULL DEFAULT 0 CHECK (revision>=0);
ALTER TABLE users ADD CONSTRAINT bio_length CHECK (char_length(bio)<=500);
ALTER TABLE competencies ADD CONSTRAINT description_length CHECK (char_length(description)<=500);
ALTER TABLE competencies ADD CONSTRAINT facets_object CHECK (jsonb_typeof(facets)='object');
ALTER TABLE competencies ADD CONSTRAINT evidence_https CHECK (evidence_url IS NULL OR evidence_url LIKE 'https://%');
CREATE INDEX sessions_user ON sessions(user_id);
CREATE INDEX bot_inbox_retention ON bot_inbox(received_at);
CREATE INDEX outbox_retention ON outbox(created_at);

ALTER TABLE outbox ADD COLUMN event_timestamp bigint NOT NULL DEFAULT 0;
