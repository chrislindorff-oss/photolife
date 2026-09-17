-- Schema v6: tracks which renditions have been checked for EXIF/GPS
-- coordinates, so captures catalogued before GPS support existed get a
-- one-time backfill pass instead of being treated as "unchanged" forever
-- and never re-probed.
--
-- Postgres port of sqlite/006_geo_backfill.sql. Plain ALTER TABLE ADD COLUMN
-- is standard SQL and needs no change between engines.

ALTER TABLE rendition ADD COLUMN geo_checked INTEGER NOT NULL DEFAULT 0;
