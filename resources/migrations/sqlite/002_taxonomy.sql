-- Schema v2: the taxonomy cache and reference-tree projects.
--
-- Filled from the iNaturalist v1 API (and later regional checklist CSVs). Once a
-- project's subtree is cached the app works offline; project.refreshed_at drives
-- manual updates. See the PhotoLife Blueprint, sections 6 and 8.
--
-- Same authoring rules as 001: statement terminators at end-of-line, no ';'
-- inside string literals, no triggers.

-- An iNaturalist place (a region a project is scoped to).
CREATE TABLE place (
    inat_id      INTEGER PRIMARY KEY,
    name         TEXT NOT NULL,
    display_name TEXT,
    admin_level  INTEGER,
    bbox_swlat   REAL,
    bbox_swlng   REAL,
    bbox_nelat   REAL,
    bbox_nelng   REAL,
    fetched_at   TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

-- A node in the cached taxonomy. inat_id is the stable identity; parent_inat_id
-- and ancestry ("48460/47126/...") come straight from the API. is_active is 0
-- for taxa iNaturalist has since split, merged or renamed.
CREATE TABLE taxon (
    id              INTEGER PRIMARY KEY,
    inat_id         INTEGER NOT NULL UNIQUE,
    parent_inat_id  INTEGER,
    rank            TEXT NOT NULL,
    rank_level      INTEGER,
    name            TEXT NOT NULL,
    common_name     TEXT,
    ancestry        TEXT,
    is_active       INTEGER NOT NULL DEFAULT 1,
    fetched_at      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX idx_taxon_parent ON taxon(parent_inat_id);
CREATE INDEX idx_taxon_rank ON taxon(rank);

-- Every name a taxon is known by. kind: 'accepted' | 'synonym' | 'vernacular'
-- | 'misspelling'. name_folded is a lower-cased, accent-stripped form the
-- matching engine searches on.
CREATE TABLE taxon_name (
    id          INTEGER PRIMARY KEY,
    taxon_id    INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    name_folded TEXT NOT NULL,
    kind        TEXT NOT NULL,
    UNIQUE(taxon_id, name, kind)
);

CREATE INDEX idx_taxon_name_folded ON taxon_name(name_folded);

-- Conservation status for a taxon in a place, from iNaturalist or a checklist.
CREATE TABLE taxon_status (
    id             INTEGER PRIMARY KEY,
    taxon_id       INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    place_inat_id  INTEGER,
    status         TEXT NOT NULL,
    status_folded  TEXT,
    source         TEXT,
    UNIQUE(taxon_id, place_inat_id, source)
);

-- A saved reference tree. source: 'inat' | 'checklist' | 'both'.
CREATE TABLE project (
    id                   INTEGER PRIMARY KEY,
    name                 TEXT NOT NULL UNIQUE,
    root_taxon_inat_id   INTEGER,
    place_inat_id        INTEGER,
    source               TEXT NOT NULL DEFAULT 'inat',
    created_at           TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    refreshed_at         TEXT
);

-- Membership of a taxon in a project's tree. in_region is 1 when the taxon is
-- recorded (or expected) in the project's place; from_checklist is 1 when a
-- checklist import vouched for it.
CREATE TABLE project_taxon (
    project_id    INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE,
    taxon_id      INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    in_region     INTEGER NOT NULL DEFAULT 0,
    from_checklist INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (project_id, taxon_id)
);

CREATE INDEX idx_project_taxon_taxon ON project_taxon(taxon_id);

-- Conditional-GET cache for the API client: one row per URL, holding the last
-- ETag / Last-Modified and the body to serve on a 304.
CREATE TABLE http_cache (
    url           TEXT PRIMARY KEY,
    etag          TEXT,
    last_modified TEXT,
    status        INTEGER,
    body          BLOB,
    fetched_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
