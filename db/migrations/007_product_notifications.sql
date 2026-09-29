-- Product notifications are opt-in; existing users and service replies are unchanged.
ALTER TABLE users ADD COLUMN product_notifications boolean NOT NULL DEFAULT false;
ALTER TABLE outbox ADD COLUMN product_context jsonb
 CHECK (product_context IS NULL OR jsonb_typeof(product_context)='object');
CREATE INDEX outbox_product_recipient ON outbox((product_context->>'recipientId'))
 WHERE product_context IS NOT NULL AND status='pending';
CREATE INDEX outbox_product_message_window
 ON outbox((product_context->>'recipientId'),(product_context->>'conversationId'),created_at DESC)
 WHERE product_context->>'kind'='message_received';
