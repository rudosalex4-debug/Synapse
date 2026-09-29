-- Candidate retrieval starts with topic, not with a scan of all users.
-- JSONB facets are checked within the same competency row.
CREATE INDEX competencies_topic_user ON competencies(topic_id, user_id);
