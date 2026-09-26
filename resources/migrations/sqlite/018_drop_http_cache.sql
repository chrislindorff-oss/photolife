-- Schema v18: the conditional-GET cache moves out of the catalogue into its
-- own file under the app's cache directory (see pl::net::HttpCache). Its rows
-- were only ever re-fetchable API responses, so nothing is lost by dropping
-- them. Database::open() compacts the file afterwards to return the space.

DROP TABLE IF EXISTS http_cache;
