-- Schema v15: the capture's filename-derived name (capture.name_text) at the
-- moment the user made a decision, so a later rename that points somewhere
-- else can be noticed. MatchEngine never overrides a user decision itself;
-- when the name has changed since, it reports the decision as possibly stale
-- and lets the user choose. NULL = decided before this column existed (the
-- engine checks those once, then records the name).
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE capture_match ADD COLUMN decided_name_text TEXT;
