-- Migration 010: dashboard_widgets
--
-- The MigrationRunner already wraps this file in ONE transaction (under an
-- advisory lock) together with the schema_migrations bookkeeping. Do NOT add
-- BEGIN/COMMIT — an embedded COMMIT ends that transaction early and breaks
-- atomicity. Prefer idempotent DDL (IF NOT EXISTS).

-- One row per widget instance on a user's dashboard. Type, position and size
-- are columns (not one JSON blob) so bounds validation and ordering stay in
-- SQL; `options` holds only the widget's own parameters (period, limit).
CREATE TABLE IF NOT EXISTS dashboard_widgets (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id    UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    widget_type VARCHAR(64) NOT NULL,       -- validated against Domain::Widgets::kCatalog
    grid_x      SMALLINT    NOT NULL,       -- 0..11 on the 12-column grid
    grid_y      SMALLINT    NOT NULL,
    grid_w      SMALLINT    NOT NULL,
    grid_h      SMALLINT    NOT NULL,
    options     JSONB       NOT NULL DEFAULT '{}'::jsonb,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- The only read pattern: one owner's layout in grid order.
CREATE INDEX IF NOT EXISTS idx_dashboard_widgets_owner
    ON dashboard_widgets (owner_id, grid_y, grid_x);

-- Bump updated_at on every UPDATE via the shared function from
-- migrations/000_updated_at_trigger.sql.
DROP TRIGGER IF EXISTS dashboard_widgets_touch_updated_at ON dashboard_widgets;
CREATE TRIGGER dashboard_widgets_touch_updated_at
    BEFORE UPDATE ON dashboard_widgets
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
