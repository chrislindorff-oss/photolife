-- Schema v24: the camera and focal length of a photo, read from its EXIF when
-- the Photographic Life List shows those columns -- a cache, filled on
-- demand for the photos it lists (not a whole-library scan). A row means
-- the file has been read; NULLs mean it didn't say. Its own table, so
-- filling it in doesn't count as changing the photos.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
CREATE TABLE capture_camera (
    capture_id   INTEGER PRIMARY KEY REFERENCES capture(id) ON DELETE CASCADE,
    camera       TEXT,
    focal_length REAL
);
