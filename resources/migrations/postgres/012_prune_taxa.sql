-- Taxa deliberately removed from a project's tree. Recorded so a later
-- refresh (which re-walks the whole subtree from iNaturalist) never
-- resurrects them. A project counts as "customized/pruned" iff it has any
-- row here.
CREATE TABLE project_excluded_taxon (
    project_id  INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE,
    taxon_id    INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    excluded_at TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"')),
    PRIMARY KEY (project_id, taxon_id)
);

CREATE INDEX idx_project_excluded_taxon_project ON project_excluded_taxon(project_id);
