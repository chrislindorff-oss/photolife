-- Schema v14: keyword tags imported from a Lightroom Classic catalog (.lrcat),
-- kept as an extra taxon-matching signal alongside filenames/folders.
--
-- Postgres port of sqlite/014_lightroom_keywords.sql -- see 001_initial.sql's
-- header for the porting rules.

CREATE TABLE capture_keyword_hint (
    id          INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    capture_id  INTEGER NOT NULL REFERENCES capture(id) ON DELETE CASCADE,
    source      TEXT NOT NULL DEFAULT 'lightroom',
    raw_keyword TEXT NOT NULL,
    taxon_id    INTEGER REFERENCES taxon(id) ON DELETE SET NULL,
    confidence  DOUBLE PRECISION NOT NULL DEFAULT 0,
    imported_at TEXT NOT NULL DEFAULT (to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"')),
    UNIQUE(capture_id, source, raw_keyword)
);

CREATE INDEX idx_capture_keyword_hint_capture ON capture_keyword_hint(capture_id);
CREATE INDEX idx_capture_keyword_hint_taxon ON capture_keyword_hint(taxon_id);
