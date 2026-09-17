-- Schema v6: tracks which renditions have been checked for EXIF/GPS
-- coordinates, so captures catalogued before GPS support existed get a
-- one-time backfill pass instead of being treated as "unchanged" forever
-- and never re-probed.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE rendition ADD COLUMN geo_checked INTEGER NOT NULL DEFAULT 0;
