-- Private learning outcome: only the request author supplies a next step.
-- Existing conversations remain valid; legacy close requests omit this field.
ALTER TABLE conversations ADD COLUMN next_step text CHECK (char_length(next_step)<=500);
