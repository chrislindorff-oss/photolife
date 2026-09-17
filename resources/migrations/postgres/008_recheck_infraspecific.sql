-- Schema v8: clear the "infraspecific check done" flag on every project taxon.
--
-- Earlier builds looked for subspecies/varieties via observations/species_counts,
-- which always rolls observations up to species rank and never reports
-- infraspecific taxa -- so every species got marked checked having found nothing.
-- Resetting the flag lets the fixed taxonomy-based check (InfraspecificFiller)
-- run again on the next "Fetch Subspecies/Varieties".
--
-- Postgres port of sqlite/008_recheck_infraspecific.sql. Plain UPDATE is
-- standard SQL and needs no change between engines.

UPDATE project_taxon SET infra_checked = 0;
