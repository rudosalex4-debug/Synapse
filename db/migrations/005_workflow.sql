-- Local educational workflow. No MAX delivery or public contact data.
CREATE TABLE help_requests (
 id uuid PRIMARY KEY, author_id uuid NOT NULL REFERENCES users(id),
 title text NOT NULL CHECK (char_length(title) BETWEEN 1 AND 120),
 body text NOT NULL CHECK (char_length(body) BETWEEN 30 AND 2000),
 learning_goal text NOT NULL CHECK (learning_goal IN ('understand','practice','troubleshoot','learning_path')),
 attempt text CHECK (char_length(attempt)<=1000),
 topic_id text NOT NULL REFERENCES topics(id), facets jsonb NOT NULL CHECK (jsonb_typeof(facets)='object'),
 required_facets jsonb NOT NULL CHECK (jsonb_typeof(required_facets)='array'),
 desired_experience text CHECK (desired_experience IN ('self_study','practice','teaching','participation')),
 taxonomy_version text NOT NULL,
 status text NOT NULL DEFAULT 'draft' CHECK (status IN ('draft','open','in_progress','resolved','cancelled','expired','closed_unresolved')),
 revision bigint NOT NULL DEFAULT 0 CHECK (revision>=0),
 created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(), expires_at timestamptz
);
CREATE INDEX help_requests_author_page ON help_requests(author_id,created_at DESC,id DESC);
CREATE INDEX help_requests_open_topic_page ON help_requests(topic_id,created_at DESC,id DESC) WHERE status='open';
CREATE INDEX help_requests_open_expiry ON help_requests(expires_at) WHERE status='open';

CREATE TABLE help_offers (
 id uuid PRIMARY KEY, request_id uuid NOT NULL REFERENCES help_requests(id), helper_id uuid NOT NULL REFERENCES users(id),
 message text NOT NULL CHECK (char_length(message) BETWEEN 1 AND 1000),
 status text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','accepted','declined','withdrawn')),
 competency_snapshot jsonb NOT NULL CHECK (jsonb_typeof(competency_snapshot)='object'),
 score double precision NOT NULL CHECK (score>=0 AND score<=100), narrower boolean NOT NULL,
 created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
 UNIQUE(request_id,helper_id)
);
CREATE UNIQUE INDEX help_offers_one_accepted ON help_offers(request_id) WHERE status='accepted';
CREATE INDEX help_offers_helper ON help_offers(helper_id,created_at DESC);

CREATE TABLE conversations (
 id uuid PRIMARY KEY, request_id uuid UNIQUE NOT NULL REFERENCES help_requests(id),
 offer_id uuid UNIQUE NOT NULL REFERENCES help_offers(id),
 author_id uuid NOT NULL REFERENCES users(id), helper_id uuid NOT NULL REFERENCES users(id),
 status text NOT NULL DEFAULT 'active' CHECK (status IN ('active','closed')),
 next_sequence bigint NOT NULL DEFAULT 0 CHECK (next_sequence>=0),
 created_at timestamptz NOT NULL DEFAULT now(), closed_at timestamptz,
 outcome text CHECK (outcome IN ('helpful','partly_helpful','not_helpful','no_result')),
 comment text CHECK (char_length(comment)<=500),
 CHECK (author_id<>helper_id),
 CHECK ((status='active' AND closed_at IS NULL AND outcome IS NULL) OR (status='closed' AND closed_at IS NOT NULL AND outcome IS NOT NULL))
);
CREATE INDEX conversations_author ON conversations(author_id,created_at DESC);
CREATE INDEX conversations_helper ON conversations(helper_id,created_at DESC);
CREATE INDEX conversations_active_helper ON conversations(helper_id) WHERE status='active';

CREATE TABLE conversation_messages (
 id uuid PRIMARY KEY, conversation_id uuid NOT NULL REFERENCES conversations(id),
 sender_id uuid NOT NULL REFERENCES users(id), sequence bigint NOT NULL CHECK (sequence>0),
 client_message_id uuid NOT NULL, text text NOT NULL CHECK (char_length(text) BETWEEN 1 AND 2000),
 created_at timestamptz NOT NULL DEFAULT now(),
 UNIQUE(conversation_id,sequence), UNIQUE(conversation_id,sender_id,client_message_id)
);
CREATE TABLE user_blocks (
 blocker_id uuid NOT NULL REFERENCES users(id), blocked_id uuid NOT NULL REFERENCES users(id),
 created_at timestamptz NOT NULL DEFAULT now(), PRIMARY KEY(blocker_id,blocked_id), CHECK (blocker_id<>blocked_id)
);
CREATE INDEX user_blocks_reverse ON user_blocks(blocked_id,blocker_id);
CREATE TABLE safety_reports (
 id uuid PRIMARY KEY, reporter_id uuid NOT NULL REFERENCES users(id),
 conversation_id uuid NOT NULL REFERENCES conversations(id),
 category text NOT NULL CHECK (category IN ('abuse','spam','other')),
 text text NOT NULL CHECK (char_length(text) BETWEEN 1 AND 2000),
 status text NOT NULL DEFAULT 'new' CHECK(status='new'), created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE workflow_idempotency (
 actor_id uuid NOT NULL REFERENCES users(id), method text NOT NULL, route text NOT NULL,
 key uuid NOT NULL, payload_hash text NOT NULL, result jsonb NOT NULL,
 created_at timestamptz NOT NULL DEFAULT now(), PRIMARY KEY(actor_id,method,route,key)
);
