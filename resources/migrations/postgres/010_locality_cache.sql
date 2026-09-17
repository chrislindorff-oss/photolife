-- Schema v10: a cache of reverse-geocoded localities (town/suburb, state,
-- country) for GPS coordinates seen on captures, from OpenStreetMap's
-- Nominatim. Keyed by coordinates rounded to 3 decimal places (roughly
-- 100m), so nearby photos share one lookup -- both to respect Nominatim's
-- usage policy (cache aggressively, no bulk/repeat queries) and to avoid one
-- row per photo for a cluster of shots taken at the same site. locality may
-- be an empty string: that is a real, cached result meaning Nominatim had no
-- address for that point (e.g. open ocean), so it is not retried forever.
--
-- Postgres port of sqlite/010_locality_cache.sql -- see 001_initial.sql's
-- header for the porting rules, and 002_taxonomy.sql's for why REAL becomes
-- DOUBLE PRECISION (load-bearing here: lat_round/lon_round are compared
-- against ROUND(CAST(... AS NUMERIC), 3)) of a double-precision column
-- elsewhere, and REAL's single-precision rounding would make an exact match
-- fail unpredictably).

CREATE TABLE geocode_cache (
    id         INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    lat_round  DOUBLE PRECISION NOT NULL,
    lon_round  DOUBLE PRECISION NOT NULL,
    locality   TEXT NOT NULL,
    fetched_at TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"')),
    UNIQUE(lat_round, lon_round)
);
