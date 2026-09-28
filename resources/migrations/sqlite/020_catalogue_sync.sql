-- Schema v20: bookkeeping for "Shared -- sync with server" catalogues (see
-- src/sync/CatalogueSync). One row. On the server: `version` counts pushes,
-- and the locked_* columns say who may currently push (the edit lock). In a
-- local catalogue: `version` is the server version it was last pulled or
-- pushed at; the lock columns are unused.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

CREATE TABLE catalogue_sync (
    id             INTEGER PRIMARY KEY,
    version        INTEGER NOT NULL DEFAULT 0,
    locked_by      TEXT,
    locked_machine TEXT,
    locked_at      TEXT,
    pushed_at      TEXT
);

INSERT INTO catalogue_sync (id, version) VALUES (1, 0);
