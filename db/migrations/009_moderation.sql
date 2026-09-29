-- Report-driven moderation. No role is accepted from a client profile.
ALTER TABLE safety_reports DROP CONSTRAINT safety_reports_status_check;
ALTER TABLE safety_reports ADD COLUMN queue_id bigserial UNIQUE;
ALTER TABLE safety_reports ADD COLUMN revision bigint NOT NULL DEFAULT 0 CHECK (revision>=0);
ALTER TABLE safety_reports ADD COLUMN resolution text CHECK (resolution IN ('dismiss','close_conversation','block_pair'));
ALTER TABLE safety_reports ADD COLUMN resolution_note text CHECK (char_length(resolution_note) BETWEEN 1 AND 1000);
ALTER TABLE safety_reports ADD COLUMN resolved_by uuid REFERENCES users(id);
ALTER TABLE safety_reports ADD COLUMN resolved_at timestamptz;
ALTER TABLE safety_reports ADD CONSTRAINT safety_reports_status_check CHECK (status IN ('new','resolved'));
ALTER TABLE safety_reports ADD CONSTRAINT safety_reports_resolution_state_check CHECK (
 (status='new' AND resolution IS NULL AND resolution_note IS NULL AND resolved_by IS NULL AND resolved_at IS NULL)
 OR (status='resolved' AND resolution IS NOT NULL AND resolution_note IS NOT NULL AND resolved_by IS NOT NULL AND resolved_at IS NOT NULL)
);
CREATE INDEX safety_reports_queue ON safety_reports(status,queue_id DESC);
ALTER TABLE conversations ADD COLUMN moderation_closed boolean NOT NULL DEFAULT false;
CREATE TABLE moderation_audit (
 id bigserial PRIMARY KEY,
 moderator_id uuid NOT NULL REFERENCES users(id),
 report_id uuid NOT NULL REFERENCES safety_reports(id),
 action text NOT NULL CHECK (action IN ('view_report','view_messages','decision')),
 decision text CHECK (decision IN ('dismiss','close_conversation','block_pair')),
 first_sequence bigint, last_sequence bigint,
 created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX moderation_audit_report ON moderation_audit(report_id,created_at DESC);
