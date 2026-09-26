-- Schema v18: the conditional-GET cache moves out of the catalogue into its
-- own file under the app's cache directory (see pl::net::HttpCache).
--
-- Postgres port of sqlite/018_drop_http_cache.sql -- see 001_initial.sql's
-- header for the porting rules.

DROP TABLE IF EXISTS http_cache;
