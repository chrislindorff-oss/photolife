-- Schema v3: the link between captures and taxa, and the aliases the matching
-- engine learns.
--
-- Postgres port of sqlite/003_matching.sql -- see 001_initial.sql's header
-- for the porting rules.

-- One row per (capture, candidate taxon). Normally a capture has a single row;
-- a photo naming two species (some Fauna files) can have more.
-- method: how the match was made. status: auto | confirmed | rejected | pending.
-- matched_rank is the rank the link was actually made at (a genus, when the
-- name was "Caladenia sp"). qualifier carries sp / aff / s.l. / s.s. / agg /
-- hybrid / undescribed / unidentified.
CREATE TABLE capture_match (
    id           INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    capture_id   INTEGER NOT NULL REFERENCES capture(id) ON DELETE CASCADE,
    taxon_id     INTEGER REFERENCES taxon(id) ON DELETE SET NULL,
    matched_rank TEXT,
    method       TEXT NOT NULL,
    confidence   REAL NOT NULL DEFAULT 0,
    status       TEXT NOT NULL DEFAULT 'pending',
    qualifier    TEXT,
    note         TEXT,
    decided_by   TEXT NOT NULL DEFAULT 'engine',
    decided_at   TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"')),
    UNIQUE(capture_id, taxon_id)
);

CREATE INDEX idx_capture_match_capture ON capture_match(capture_id);
CREATE INDEX idx_capture_match_taxon ON capture_match(taxon_id);
CREATE INDEX idx_capture_match_status ON capture_match(status);

-- A raw name string the user has resolved once, so the same oddity is never
-- queued twice. scope is 'global' or a folder path the alias is confined to.
-- not_a_taxon = 1 records "this folder/file is not a taxon" (locality, staging).
CREATE TABLE name_alias (
    id          INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    raw_text    TEXT NOT NULL,
    raw_folded  TEXT NOT NULL,
    taxon_id    INTEGER REFERENCES taxon(id) ON DELETE CASCADE,
    scope       TEXT NOT NULL DEFAULT 'global',
    not_a_taxon INTEGER NOT NULL DEFAULT 0,
    created_at  TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"')),
    UNIQUE(raw_folded, scope)
);

CREATE INDEX idx_name_alias_folded ON name_alias(raw_folded);

-- The cover image chosen for a taxon within a project (populated in the tree UI).
CREATE TABLE representative (
    project_id  INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE,
    taxon_id    INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    capture_id  INTEGER REFERENCES capture(id) ON DELETE SET NULL,
    PRIMARY KEY (project_id, taxon_id)
);
