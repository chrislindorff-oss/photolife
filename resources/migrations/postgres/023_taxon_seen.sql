-- Schema v23: taxa the user has seen but not (yet) photographed -- the hand
-- ticks in the Reference Tree's Seen column. Postgres port of
-- sqlite/023_taxon_seen.sql: keyed by iNaturalist id, so a tick is about the
-- taxon in every tree that has it.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
CREATE TABLE taxon_seen (
    inat_id INTEGER PRIMARY KEY,
    seen_at TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'))
);
