-- Migration 007: add_post_topic_tags
-- Created: 2026-07-23T00:00:00Z
--
-- Migrations are applied in numeric order on app boot (or via
-- RUN_MIGRATIONS_ONLY=1 ./tarassov_me). Use --verify-migrations to
-- list pending without applying.
--
-- The MigrationRunner already wraps this file in ONE transaction (under an
-- advisory lock) together with the schema_migrations bookkeeping. Do NOT add
-- BEGIN/COMMIT — an embedded COMMIT ends that transaction early and breaks
-- atomicity. Prefer idempotent DDL (IF NOT EXISTS / ON CONFLICT DO NOTHING).

-- Blog tag cloud + section labels (design: "Облако тегов для блога").
--   topic — the section label shown above the title (e.g. "Kubernetes").
--   tags  — cross-cutting keyword tags driving the index tag cloud + chips.
--           Stored comma-joined in a single TEXT column (tags are constrained
--           keywords with no commas), split/joined in the domain layer. Keeps
--           every query on plain string params — no pqxx array binding.
ALTER TABLE posts ADD COLUMN IF NOT EXISTS topic TEXT NOT NULL DEFAULT '';
ALTER TABLE posts ADD COLUMN IF NOT EXISTS tags  TEXT NOT NULL DEFAULT '';
