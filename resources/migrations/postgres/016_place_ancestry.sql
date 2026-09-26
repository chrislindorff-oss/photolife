-- Schema v16: each place's iNaturalist ancestry (ancestor_place_ids, itself
-- last, joined with '/'), so a tree's region can be tested for lying within
-- the region a conservation status was listed for (a Victorian listing
-- applies to a Grampians tree, but not to a South Australian or
-- Australia-wide one). NULL = not fetched yet; bounding boxes are the
-- fallback until it is.
--
-- Same authoring rules as earlier migrations: statement terminators at
-- end-of-line, no ';' inside string literals, no triggers.

ALTER TABLE place ADD COLUMN ancestor_ids TEXT;
