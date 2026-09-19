-- Resume point for an interrupted reference-tree build/refresh's species-
-- pagination phase (see ProjectBuilder). One row per project; "checkpoint
-- present" is exactly "row exists". Tied to the exact query parameters a run
-- resolved -- a resume is only honoured if a later run resolves the same
-- root taxon, place and page size, otherwise the stale checkpoint is
-- discarded rather than trusted.
CREATE TABLE project_build_checkpoint (
    project_id       INTEGER PRIMARY KEY REFERENCES project(id) ON DELETE CASCADE,
    root_taxon_inat_id INTEGER NOT NULL,
    place_inat_id    INTEGER,
    per_page         INTEGER NOT NULL,
    next_page        INTEGER NOT NULL,
    species_seen     INTEGER NOT NULL,
    species_total    INTEGER NOT NULL,
    updated_at       TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'))
);
