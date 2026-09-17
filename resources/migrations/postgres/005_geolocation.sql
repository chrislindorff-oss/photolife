-- Schema v5: optional GPS coordinates on captures, parsed from EXIF (a JPEG
-- rendition, or a RAW file's embedded preview JPEG). Backs a "geo" badge on
-- thumbnails that have location data.
--
-- Postgres port of sqlite/005_geolocation.sql. REAL -> DOUBLE PRECISION: see
-- 002_taxonomy.sql's header for why.

ALTER TABLE capture ADD COLUMN latitude DOUBLE PRECISION;
ALTER TABLE capture ADD COLUMN longitude DOUBLE PRECISION;
