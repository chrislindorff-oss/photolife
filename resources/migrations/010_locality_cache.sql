-- Schema v10: a cache of reverse-geocoded localities (town/suburb, state,
-- country) for GPS coordinates seen on captures, from OpenStreetMap's
-- Nominatim. Keyed by coordinates rounded to 3 decimal places (roughly
-- 100m), so nearby photos share one lookup -- both to respect Nominatim's
-- usage policy (cache aggressively, no bulk/repeat queries) and to avoid one
-- row per photo for a cluster of shots taken at the same site. locality may
-- be an empty string: that is a real, cached result meaning Nominatim had no
-- address for that point (e.g. open ocean), so it is not retried forever.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

CREATE TABLE geocode_cache (
    id         INTEGER PRIMARY KEY,
    lat_round  REAL NOT NULL,
    lon_round  REAL NOT NULL,
    locality   TEXT NOT NULL,
    fetched_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    UNIQUE(lat_round, lon_round)
);
