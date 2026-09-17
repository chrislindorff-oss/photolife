-- Schema v9: photos the user has hand-picked as a "best shot" of their
-- identified species. Unlike `representative` (one machine-picked cover per
-- taxon per project, rebuilt on every coverage refresh) this is a small
-- user-curated set: any number of captures may be starred, and the star
-- follows the capture's live identification (capture_match). Global to the
-- catalogue, like capture_match.
--
-- Postgres port of sqlite/009_best_shots.sql. capture_id is the primary key
-- and is always supplied by the caller (the starred capture's id), never
-- autoincremented, so it stays a plain INTEGER PRIMARY KEY.

CREATE TABLE best_shot (
    capture_id INTEGER PRIMARY KEY REFERENCES capture(id) ON DELETE CASCADE,
    created_at TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'))
);
