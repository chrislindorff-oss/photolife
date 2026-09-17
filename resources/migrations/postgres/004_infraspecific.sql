-- Schema v4: tracks which species in a reference tree have already been
-- checked for infraspecific children (subspecies, variety, form, ...), so
-- fetching them can be paused and resumed without re-querying iNat for
-- species already checked.
--
-- Postgres port of sqlite/004_infraspecific.sql. Plain ALTER TABLE ADD
-- COLUMN is standard SQL and needs no change between engines.

ALTER TABLE project_taxon ADD COLUMN infra_checked INTEGER NOT NULL DEFAULT 0;
