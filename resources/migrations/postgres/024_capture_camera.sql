-- Schema v24: a photo's camera and focal length, read from its EXIF on demand
-- for the Photographic Life List. Postgres port of
-- sqlite/024_capture_camera.sql.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
CREATE TABLE capture_camera (
    capture_id   INTEGER PRIMARY KEY REFERENCES capture(id) ON DELETE CASCADE,
    camera       TEXT,
    focal_length DOUBLE PRECISION
);
