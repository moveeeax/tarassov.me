-- Migration 009: fold_leetcode_topics
-- 008 only backfilled EMPTY topics, but the numeric-slug problem posts carry
-- the LeetCode category as their topic (Array/String/Math/...), which the old
-- JS *overrode* to "LeetCode" for display. With server-side facets that
-- override is gone and the topic row exploded into ~25 category chips.
-- Fold them for real: the category is preserved in tags (e.g. topic "Array"
-- posts all carry the "array" tag), so nothing is lost. Idempotent.
UPDATE posts SET topic = 'LeetCode' WHERE slug ~ '^\d+-' AND topic <> 'LeetCode';
