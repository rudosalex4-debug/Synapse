-- One resumable form per MAX dialog. Product writes and replies commit with inbox.
CREATE TABLE bot_forms (
 max_user_id text PRIMARY KEY REFERENCES max_dialogs(max_user_id) ON DELETE CASCADE,
 state jsonb NOT NULL CHECK (jsonb_typeof(state)='object'),
 updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX bot_inbox_pending_user_order ON bot_inbox ((payload->>'user_id'),id) WHERE status='pending';
CREATE INDEX outbox_pending_chat_order ON outbox (chat_id,id) WHERE status IN ('pending','processing');
