-- Schema v4: tracks which species in a reference tree have already been
-- checked for infraspecific children (subspecies, variety, form, ...), so
-- fetching them can be paused and resumed without re-querying iNat for
-- species already checked. See the PhotoLife Blueprint and MEMORY.md.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE project_taxon ADD COLUMN infra_checked INTEGER NOT NULL DEFAULT 0;
