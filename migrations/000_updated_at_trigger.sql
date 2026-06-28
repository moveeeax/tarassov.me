-- Migration 000: shared touch_updated_at() trigger function
--
-- A single, reusable trigger function that any table with an `updated_at
-- TIMESTAMPTZ` column can attach to so the column is bumped to now() on every
-- UPDATE. scripts/new-resource.sh wires generated tables to this function via
-- new-migration.sh (it emits `CREATE TRIGGER ... EXECUTE FUNCTION
-- touch_updated_at()`), so the function must exist before any resource
-- migration runs — hence version 000, ahead of 001.
--
-- (Migration 001 keeps its own users_touch_updated_at() for flask-base parity;
-- new tables should use this shared one instead of duplicating the function.)
--
-- NOTE: MigrationRunner wraps each file in ONE transaction (under an advisory
-- lock) and records schema_migrations in that same transaction. Do NOT add
-- BEGIN/COMMIT here — an embedded COMMIT ends the runner's transaction early,
-- drops the lock mid-DDL, and breaks the atomic version bookkeeping.

CREATE OR REPLACE FUNCTION touch_updated_at() RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
