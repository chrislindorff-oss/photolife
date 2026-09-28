-- Schema v21: who made the server's latest push (see catalogue_sync, v20),
-- shown in the Server Status window beside pushed_at.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE catalogue_sync ADD COLUMN pushed_by TEXT;
