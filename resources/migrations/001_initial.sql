-- Schema v1: the taxonomic tree, the photo catalogue, and the links between them.
--
-- The migration runner (pl::Database) wraps this script in a transaction and
-- bumps PRAGMA user_version afterwards, so this file must not touch user_version
-- itself. Statements are split on a ';' that ends a line, so keep every
-- statement terminator at end-of-line and avoid ';' inside string literals.

-- A node in the taxonomic tree. parent_id is NULL for roots (e.g. a kingdom).
-- rank is a free-text Linnaean rank ('kingdom', 'family', 'species', ...) rather
-- than an enum, so unofficial ranks ('subspecies', 'morph') are allowed.
CREATE TABLE taxon (
    id              INTEGER PRIMARY KEY,
    parent_id       INTEGER REFERENCES taxon(id) ON DELETE CASCADE,
    rank            TEXT NOT NULL,
    scientific_name TEXT NOT NULL,
    common_name     TEXT,
    created_at      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    UNIQUE(parent_id, scientific_name)
);

CREATE INDEX idx_taxon_parent ON taxon(parent_id);
CREATE INDEX idx_taxon_scientific_name ON taxon(scientific_name);

-- One row per image file discovered under a watched root.
CREATE TABLE photo (
    id           INTEGER PRIMARY KEY,
    path         TEXT NOT NULL UNIQUE,
    content_hash TEXT,
    file_size    INTEGER,
    width        INTEGER,
    height       INTEGER,
    captured_at  TEXT,
    camera_make  TEXT,
    camera_model TEXT,
    lens         TEXT,
    imported_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX idx_photo_content_hash ON photo(content_hash);
CREATE INDEX idx_photo_captured_at ON photo(captured_at);

-- Which taxa a photo depicts. A photo may be linked to several taxa; at most one
-- of those links is the primary subject (enforced by the partial unique index).
CREATE TABLE photo_taxon (
    photo_id   INTEGER NOT NULL REFERENCES photo(id) ON DELETE CASCADE,
    taxon_id   INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    is_primary INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (photo_id, taxon_id)
);

CREATE INDEX idx_photo_taxon_taxon ON photo_taxon(taxon_id);
CREATE UNIQUE INDEX idx_photo_taxon_one_primary ON photo_taxon(photo_id) WHERE is_primary = 1;
