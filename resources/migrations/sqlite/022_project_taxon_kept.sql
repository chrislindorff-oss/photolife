-- Schema v22: taxa the user chose to keep in a project's tree after a refresh
-- found iNaturalist no longer lists them in the tree's place (or they head a
-- group with nothing left beneath). Recorded so the next refresh doesn't ask
-- again; a later rename of the taxon on iNaturalist is still reported.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
CREATE TABLE project_taxon_kept (
    project_id INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE,
    taxon_id   INTEGER NOT NULL REFERENCES taxon(id) ON DELETE CASCADE,
    kept_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    PRIMARY KEY (project_id, taxon_id)
);
