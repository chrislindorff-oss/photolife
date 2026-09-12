-- Schema v11: provenance for captures downloaded from a user's own
-- iNaturalist observations (see the "Download from iNaturalist" tab). Lets a
-- re-run of that scan recognise a photo it has already pulled in, on top of
-- the taxon+date+GPS heuristic used for photos that predate this feature.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE capture ADD COLUMN inat_observation_id INTEGER;
ALTER TABLE capture ADD COLUMN inat_photo_id INTEGER;
CREATE UNIQUE INDEX idx_capture_inat_photo ON capture(inat_photo_id) WHERE inat_photo_id IS NOT NULL;
