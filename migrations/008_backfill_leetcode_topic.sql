-- Migration 008: backfill_leetcode_topic
-- The public site used to derive a display topic in JS: numeric-slug posts
-- (^\d+-) are LeetCode problem notes. Persist that once so the server owns
-- topic and the JS derivation can be deleted. Idempotent.
UPDATE posts SET topic = 'LeetCode' WHERE slug ~ '^\d+-' AND btrim(topic) = '';
