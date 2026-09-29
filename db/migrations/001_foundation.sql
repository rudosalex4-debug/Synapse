CREATE TABLE users (
  id uuid PRIMARY KEY, max_user_id text UNIQUE, demo_persona text UNIQUE,
  display_name text NOT NULL CHECK (length(display_name) BETWEEN 1 AND 100), bio text NOT NULL DEFAULT '',
  available_to_help boolean NOT NULL DEFAULT true, max_active_conversations integer NOT NULL DEFAULT 2 CHECK (max_active_conversations BETWEEN 1 AND 5),
  provenance text NOT NULL CHECK (provenance IN ('self_declared','demo')),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  CHECK ((max_user_id IS NOT NULL AND demo_persona IS NULL AND provenance='self_declared') OR (max_user_id IS NULL AND demo_persona IS NOT NULL AND provenance='demo'))
);
CREATE TABLE sessions (token_hash text PRIMARY KEY, user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, expires_at timestamptz NOT NULL, created_at timestamptz NOT NULL DEFAULT now());
CREATE INDEX sessions_expiry ON sessions(expires_at);
CREATE TABLE topics (id text PRIMARY KEY, parent_id text REFERENCES topics(id), level integer NOT NULL CHECK(level>=1), label text NOT NULL, aliases jsonb NOT NULL DEFAULT '[]', active boolean NOT NULL DEFAULT true, taxonomy_version text NOT NULL);
CREATE TABLE facet_definitions (id text PRIMARY KEY, label text NOT NULL, applicable_topic_prefixes jsonb NOT NULL);
CREATE TABLE facet_values (facet_id text NOT NULL REFERENCES facet_definitions(id), id text NOT NULL, label text NOT NULL, PRIMARY KEY(facet_id,id));
CREATE TABLE competencies (
  id uuid PRIMARY KEY, user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, topic_id text NOT NULL REFERENCES topics(id), facets jsonb NOT NULL,
  experience_kind text NOT NULL CHECK (experience_kind IN ('self_study','practice','teaching','participation')), description text NOT NULL DEFAULT '',
  evidence_url text, evidence_visibility text NOT NULL DEFAULT 'private' CHECK (evidence_visibility IN ('private','participants')),
  evidence_status text GENERATED ALWAYS AS (CASE WHEN evidence_url IS NULL THEN 'none' ELSE 'unreviewed' END) STORED,
  provenance text NOT NULL CHECK (provenance IN ('self_declared','demo')), created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX competencies_user ON competencies(user_id);
CREATE TABLE max_dialogs (max_user_id text PRIMARY KEY, chat_id text NOT NULL, active boolean NOT NULL, event_timestamp bigint NOT NULL, updated_at timestamptz NOT NULL DEFAULT now());
CREATE TABLE bot_inbox (
  id bigserial PRIMARY KEY, event_key text UNIQUE NOT NULL, update_type text NOT NULL, payload jsonb NOT NULL,
  status text NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','done','dead')),
  attempts integer NOT NULL DEFAULT 0, available_at timestamptz NOT NULL DEFAULT now(), received_at timestamptz NOT NULL DEFAULT now(), last_error text
);
CREATE INDEX bot_inbox_pending ON bot_inbox(status,available_at);
CREATE TABLE outbox (
  id bigserial PRIMARY KEY, event_key text UNIQUE NOT NULL, kind text NOT NULL, chat_id text NOT NULL, max_user_id text NOT NULL,
  payload jsonb NOT NULL, status text NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','processing','done','dead','cancelled')),
  attempts integer NOT NULL DEFAULT 0, available_at timestamptz NOT NULL DEFAULT now(), lock_until timestamptz, lease_token uuid,
  created_at timestamptz NOT NULL DEFAULT now(), last_error text
);
CREATE INDEX outbox_pending ON outbox(status,available_at);
