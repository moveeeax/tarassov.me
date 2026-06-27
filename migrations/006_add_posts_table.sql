-- Migration 006: add_posts_table
-- Created: 2026-06-27T13:45:41Z
--
-- Migrations are applied in numeric order on app boot (or via
-- RUN_MIGRATIONS_ONLY=1 ./tarassov_me). Use --verify-migrations to
-- list pending without applying.
--
-- The MigrationRunner already wraps this file in ONE transaction (under an
-- advisory lock) together with the schema_migrations bookkeeping. Do NOT add
-- BEGIN/COMMIT — an embedded COMMIT ends that transaction early and breaks
-- atomicity. Prefer idempotent DDL (IF NOT EXISTS / ON CONFLICT DO NOTHING).

-- Blog posts for the public site. Authored via the admin API; the public
-- read endpoints only expose rows with status = 'published'.
CREATE TABLE IF NOT EXISTS posts (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug         CITEXT UNIQUE NOT NULL,            -- URL key (citext from migration 001)
    title        TEXT        NOT NULL,
    summary      TEXT        NOT NULL DEFAULT '',   -- list/teaser blurb
    body         TEXT        NOT NULL DEFAULT '',   -- Markdown source
    status       VARCHAR(16) NOT NULL DEFAULT 'draft',  -- draft | published
    published_at TIMESTAMPTZ,                       -- set when first published
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Public listing: newest published first. Partial index keeps drafts out.
CREATE INDEX IF NOT EXISTS idx_posts_published
    ON posts (published_at DESC) WHERE status = 'published';

-- Bump updated_at on every UPDATE via the shared function from
-- migrations/000_updated_at_trigger.sql.
DROP TRIGGER IF EXISTS posts_touch_updated_at ON posts;
CREATE TRIGGER posts_touch_updated_at
    BEFORE UPDATE ON posts
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
