-- Schema v23: taxa the user has seen but not (yet) photographed -- the hand
-- ticks in the Reference Tree's Seen column. A photographed taxon is seen
-- anyway (its tick comes from capture_match), so only hand ticks are stored.
-- Keyed by iNaturalist id rather than taxon.id so a tick is about the taxon
-- everywhere -- every tree that has it -- and outlives its taxon row being
-- pruned and fetched again.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
CREATE TABLE taxon_seen (
    inat_id INTEGER PRIMARY KEY,
    seen_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
