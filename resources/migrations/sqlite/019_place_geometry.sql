-- Schema v19: each place's outline, as iNaturalist's geometry_geojson (a
-- Polygon or MultiPolygon, compact JSON text) -- drawn on the map for the
-- active reference tree, and used to keep "Restrict to Active Reference Tree
-- Locality" to the region itself rather than its bounding box. NULL = not
-- fetched yet; '' = iNaturalist has none. The bounding box is the fallback.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE place ADD COLUMN geometry_geojson TEXT;
