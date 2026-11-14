-- Schema v26 (Postgres port of sqlite/026_inat_time_entered.sql): an observation's time as it was entered (observed_on_string),
-- beside iNaturalist's reading of it (time_observed). They differ when a
-- camera's clock time went up tagged with another time zone (often +00:00):
-- iNaturalist then shifts it, while the photo's EXIF still has the camera's
-- clock. "Already on iNaturalist?" compares a photo with both.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
ALTER TABLE inat_my_observation ADD COLUMN time_entered TEXT;
-- Observations read before this have no time as entered: read them again
-- (the next check reads all of them).
DELETE FROM inat_my_observation;
