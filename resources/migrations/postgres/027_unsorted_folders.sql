-- Schema v27 (Postgres port of sqlite/027_unsorted_folders.sql): Unsorted folders, and RAW photos' own clock times.
--
-- capture.time_checked: the photo's files have been looked at once for the
-- camera's clock time (a RAW's own metadata, which its embedded preview
-- often lacks), so a rescan doesn't look again.
--
-- unsorted_folder: folders (with their subfolders) the user has marked
-- Unsorted -- unculled runs of shots. PhotoLife catalogues one photo per run
-- in them (the run's representative, an ordinary capture, matched as usual)
-- and only remembers the others, in unsorted_photo, with the run they
-- belong to -- so rescans and matching handle one entry per run, while the
-- species still count. Unmarking a folder catalogues everything again.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
ALTER TABLE capture ADD COLUMN time_checked INTEGER NOT NULL DEFAULT 0;
CREATE TABLE unsorted_folder (
    path      TEXT PRIMARY KEY,
    marked_at TEXT NOT NULL
);
CREATE TABLE unsorted_photo (
    path       TEXT PRIMARY KEY,
    folder_id  INTEGER REFERENCES folder(id) ON DELETE CASCADE,
    base_name  TEXT NOT NULL,
    file_size  BIGINT NOT NULL,
    mtime      BIGINT NOT NULL,
    taken      TEXT,
    capture_id INTEGER REFERENCES capture(id) ON DELETE SET NULL
);
CREATE INDEX idx_unsorted_photo_capture ON unsorted_photo(capture_id);
CREATE INDEX idx_unsorted_photo_folder ON unsorted_photo(folder_id);
