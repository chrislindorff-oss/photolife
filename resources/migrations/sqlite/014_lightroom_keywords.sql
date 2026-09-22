-- Schema v14: keyword tags imported from a Lightroom Classic catalog (.lrcat),
-- kept as an extra taxon-matching signal alongside filenames/folders. See
-- LightroomImportEngine. One row per (capture, source, raw_keyword) so
-- re-importing the same catalog upserts rather than duplicating.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

CREATE TABLE capture_keyword_hint (
    id          INTEGER PRIMARY KEY,
    capture_id  INTEGER NOT NULL REFERENCES capture(id) ON DELETE CASCADE,
    source      TEXT NOT NULL DEFAULT 'lightroom',
    raw_keyword TEXT NOT NULL,
    taxon_id    INTEGER REFERENCES taxon(id) ON DELETE SET NULL,
    confidence  REAL NOT NULL DEFAULT 0,
    imported_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    UNIQUE(capture_id, source, raw_keyword)
);

CREATE INDEX idx_capture_keyword_hint_capture ON capture_keyword_hint(capture_id);
CREATE INDEX idx_capture_keyword_hint_taxon ON capture_keyword_hint(taxon_id);
