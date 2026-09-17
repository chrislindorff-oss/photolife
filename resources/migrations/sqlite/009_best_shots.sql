-- Schema v9: photos the user has hand-picked as a "best shot" of their
-- identified species. Unlike `representative` (one machine-picked cover per
-- taxon per project, rebuilt on every coverage refresh) this is a small
-- user-curated set: any number of captures may be starred, and the star
-- follows the capture's live identification (capture_match). Global to the
-- catalogue, like capture_match.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

CREATE TABLE best_shot (
    capture_id INTEGER PRIMARY KEY REFERENCES capture(id) ON DELETE CASCADE,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
