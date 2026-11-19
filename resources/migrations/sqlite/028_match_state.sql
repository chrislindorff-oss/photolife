-- Schema v28: what Match Library last matched against.
--
-- match_state.fingerprint: a summary of the reference trees and taxon names
-- at the last full match (every photo). Match Library looks at every photo
-- again only when that has changed (a tree built or refreshed, names
-- imported); otherwise just the photos that need it -- new, or renamed or
-- moved since they were matched (an
-- engine match records the name and folder it went by, in
-- capture_match.decided_name_text) -- plus the unresolved ones when the
-- learned names (name_alias, keyword hints) have changed since the last run
-- (match_state.learned). Matching > Re-match Everything always looks at
-- every photo.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.
CREATE TABLE match_state (
    id          INTEGER PRIMARY KEY,
    fingerprint TEXT NOT NULL,
    learned     TEXT NOT NULL DEFAULT '',
    matched_at  TEXT NOT NULL
);
