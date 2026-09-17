-- Schema v11: provenance for captures downloaded from a user's own
-- iNaturalist observations (see the "Download from iNaturalist" tab). Lets a
-- re-run of that scan recognise a photo it has already pulled in, on top of
-- the taxon+date+GPS heuristic used for photos that predate this feature.
--
-- Postgres port of sqlite/011_inat_import.sql. Plain ALTER TABLE ADD COLUMN
-- and a partial unique index are both standard SQL and need no change
-- between engines.

ALTER TABLE capture ADD COLUMN inat_observation_id INTEGER;
ALTER TABLE capture ADD COLUMN inat_photo_id INTEGER;
CREATE UNIQUE INDEX idx_capture_inat_photo ON capture(inat_photo_id) WHERE inat_photo_id IS NOT NULL;
