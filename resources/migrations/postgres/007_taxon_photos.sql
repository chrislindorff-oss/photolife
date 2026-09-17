-- Schema v7: a representative reference photo per taxon, from the iNaturalist
-- API's "default_photo". Used by the Reference Photos tab to show what each
-- species in a reference tree looks like. photo_url is the medium-size iNat
-- image URL; photo_attribution is its licence/credit line.
--
-- Postgres port of sqlite/007_taxon_photos.sql. Plain ALTER TABLE ADD COLUMN
-- is standard SQL and needs no change between engines.

ALTER TABLE taxon ADD COLUMN photo_url TEXT;
ALTER TABLE taxon ADD COLUMN photo_attribution TEXT;
