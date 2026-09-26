-- Schema v17: whether a taxon is native or introduced in a region, for the
-- "introduced species" coverage stats. Like taxon_status, a record applies in
-- trees whose region is its place or lies within it (see computeCoverage).
-- source: a checklist label (e.g. "VBA 2021") or 'iNaturalist'; a checklist
-- record wins over iNaturalist's for the same taxon.
--
-- project.origins_fetched_at: when iNaturalist's introduced taxa were last
-- fetched for the tree's region (NULL = not yet).
--
-- Postgres port of sqlite/017_taxon_origin.sql -- see 001_initial.sql's
-- header for the porting rules.

CREATE TABLE taxon_origin (
    id             INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    taxon_id       INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    place_inat_id  INTEGER,
    origin         TEXT NOT NULL,
    source         TEXT NOT NULL,
    UNIQUE(taxon_id, place_inat_id, source)
);

CREATE INDEX idx_taxon_origin_taxon ON taxon_origin(taxon_id);

ALTER TABLE project ADD COLUMN origins_fetched_at TEXT;
