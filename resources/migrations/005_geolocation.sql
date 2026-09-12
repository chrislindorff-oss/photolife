-- Schema v5: optional GPS coordinates on captures, parsed from EXIF (a JPEG
-- rendition, or a RAW file's embedded preview JPEG). Backs a "geo" badge on
-- thumbnails that have location data.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE capture ADD COLUMN latitude REAL;
ALTER TABLE capture ADD COLUMN longitude REAL;
