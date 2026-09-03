-- Schema v1: the local photo catalogue produced by the filesystem scanner.
--
-- Only the scan-side tables live here. Taxonomy, projects and matches (see the
-- PhotoLife Blueprint, section 6) arrive in a later migration once the
-- iNaturalist client exists and their columns have settled.
--
-- The migration runner (pl::Database) wraps this script in one transaction and
-- sets PRAGMA user_version afterwards, so this file must not touch user_version.
-- Statements are split on a ';' that ends a line, so keep every terminator at
-- end-of-line and avoid ';' inside string literals.

-- A directory seen while scanning a watched root. Kept even when it holds no
-- images directly, so rank/name inference from the path is reusable and stable
-- across rescans. kind: 'unknown' until the matching engine classifies it as
-- 'taxon' | 'group' | 'locality' | 'staging'.
CREATE TABLE folder (
    id            INTEGER PRIMARY KEY,
    path          TEXT NOT NULL UNIQUE,
    parent_id     INTEGER REFERENCES folder(id) ON DELETE CASCADE,
    name          TEXT NOT NULL,
    depth         INTEGER NOT NULL,
    inferred_rank TEXT,
    inferred_name TEXT,
    kind          TEXT NOT NULL DEFAULT 'unknown'
);

CREATE INDEX idx_folder_parent ON folder(parent_id);

-- One row per shot. A RAW file and its same-stem JPEG are two renditions of one
-- capture; base_name is the shared filename stem (no extension) used to group
-- them. captured_on is ISO-8601; date_source is 'filename' | 'exif' | 'none'.
-- name_text / locality_text / organ_tags are parsed straight from base_name at
-- scan time; resolving name_text to a taxon is the matching engine's job.
CREATE TABLE capture (
    id            INTEGER PRIMARY KEY,
    folder_id     INTEGER NOT NULL REFERENCES folder(id) ON DELETE CASCADE,
    base_name     TEXT NOT NULL,
    name_text     TEXT,
    locality_text TEXT,
    organ_tags    TEXT,
    captured_on   TEXT,
    date_source   TEXT NOT NULL DEFAULT 'none',
    first_seen    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    last_seen     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    UNIQUE(folder_id, base_name)
);

CREATE INDEX idx_capture_folder ON capture(folder_id);
CREATE INDEX idx_capture_captured_on ON capture(captured_on);

-- A concrete file on disk backing a capture. kind: 'jpeg' | 'raw' | 'raw_preview'.
-- (path, file_size, mtime) lets a rescan skip unchanged files without rehashing;
-- content_hash keys the thumbnail cache so moved/renamed files keep their thumbs.
CREATE TABLE rendition (
    id           INTEGER PRIMARY KEY,
    capture_id   INTEGER NOT NULL REFERENCES capture(id) ON DELETE CASCADE,
    path         TEXT NOT NULL UNIQUE,
    kind         TEXT NOT NULL,
    ext          TEXT NOT NULL,
    content_hash TEXT,
    file_size    INTEGER,
    mtime        INTEGER,
    width        INTEGER,
    height       INTEGER,
    thumb_key    TEXT,
    scanned_at   TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX idx_rendition_capture ON rendition(capture_id);
CREATE INDEX idx_rendition_content_hash ON rendition(content_hash);
