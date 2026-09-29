-- Preserve the existing opt-in; its timestamp prevents publications made before
-- consent (or before a later re-enable) from generating matching notifications.
ALTER TABLE users ADD COLUMN product_notifications_since timestamptz;
UPDATE users SET product_notifications_since=clock_timestamp() WHERE product_notifications;

-- Inserted by the publish transaction only. Never backfill from open requests.
CREATE TABLE matching_notification_jobs (
 request_id uuid PRIMARY KEY REFERENCES help_requests(id),
 created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
 completed_at timestamptz,
 status text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','done','skipped')),
 CHECK ((status='pending')=(completed_at IS NULL))
);
CREATE INDEX matching_notification_jobs_pending
 ON matching_notification_jobs(created_at,request_id) WHERE status='pending';

-- This ledger survives outbox retention, preventing publication/recipient replay.
-- Recipient locks serialize cooldown checks and insertion with opt-out/profile
-- changes. Do not purge this table independently of its originating requests.
CREATE TABLE matching_notification_dispatches (
 request_id uuid NOT NULL REFERENCES matching_notification_jobs(request_id),
 recipient_id uuid NOT NULL REFERENCES users(id),
 created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
 delivery_reserved_at timestamptz,
 PRIMARY KEY(request_id,recipient_id)
);
CREATE INDEX matching_notification_dispatches_recipient
 ON matching_notification_dispatches(recipient_id,created_at DESC);
CREATE INDEX matching_notification_dispatches_delivery
 ON matching_notification_dispatches(recipient_id,delivery_reserved_at DESC)
 WHERE delivery_reserved_at IS NOT NULL;

-- Ordering and LIMIT operate inside the raw topic candidate sample, before the
-- eligibility joins. A dense topic therefore cannot trigger a full user scan.
CREATE INDEX competencies_matching_notification_sample
 ON competencies(topic_id,user_id,id);

-- New competencies and resume events re-check only the changed helper, against
-- <=20 indexed open requests. User/generation coalescing bounds repeated saves.
CREATE TABLE matching_notification_refreshes (
 user_id uuid PRIMARY KEY REFERENCES users(id),
 generation uuid NOT NULL,
 created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
 completed_at timestamptz
);
CREATE INDEX matching_notification_refreshes_pending
 ON matching_notification_refreshes(created_at,user_id) WHERE completed_at IS NULL;

