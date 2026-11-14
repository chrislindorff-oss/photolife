-- Schema v25 (Postgres port of sqlite/025_inat_presence.sql): whether photos are already on iNaturalist, for photos posted
-- outside PhotoLife (the iNaturalist app or website, or before PhotoLife).
--
-- inat_my_observation: the details of the user's own observations (not
-- their photos), read from iNaturalist by "Already on iNaturalist?" and
-- kept so a later check reads only the new ones. Compared with the
-- library's photos by species, time and place.
--
-- capture_inat_status: a photo's state as decided by that check or by
-- hand: on_inat (linked to observation_id when known), not_on_inat
-- (remembered, so it isn't suggested again), or maybe (a match waiting for
-- the user). A photo with no row and no capture.inat_observation_id is
-- simply not known to be on iNaturalist.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
CREATE TABLE inat_my_observation (
    id                 BIGINT PRIMARY KEY,
    login              TEXT NOT NULL,
    taxon_inat_id      BIGINT,
    taxon_ancestor_ids TEXT,
    taxon_name         TEXT,
    observed_on        TEXT,
    time_observed      TEXT,
    latitude           DOUBLE PRECISION,
    longitude          DOUBLE PRECISION,
    place_guess        TEXT,
    preview_url        TEXT,
    fetched_at         TEXT NOT NULL
);
CREATE INDEX idx_inat_my_observation_login ON inat_my_observation(login);
CREATE TABLE capture_inat_status (
    capture_id     INTEGER PRIMARY KEY REFERENCES capture(id) ON DELETE CASCADE,
    status         TEXT NOT NULL,
    observation_id BIGINT,
    confidence     TEXT NOT NULL,
    reasons        TEXT,
    decided_by     TEXT NOT NULL,
    check_run      TEXT,
    decided_at     TEXT NOT NULL
);
CREATE INDEX idx_capture_inat_status_status ON capture_inat_status(status);
