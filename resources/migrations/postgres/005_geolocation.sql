-- Schema v5: optional GPS coordinates on captures, parsed from EXIF (a JPEG
-- rendition, or a RAW file's embedded preview JPEG). Backs a "geo" badge on
-- thumbnails that have location data.
--
-- Postgres port of sqlite/005_geolocation.sql. Plain ALTER TABLE ADD COLUMN
-- is standard SQL and needs no change between engines.

ALTER TABLE capture ADD COLUMN latitude REAL;
ALTER TABLE capture ADD COLUMN longitude REAL;
