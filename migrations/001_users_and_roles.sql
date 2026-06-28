-- Migration 001: users + roles + permission bitmask
-- flask-base parity: app/models/user.py — same shape, different storage.
--
-- The Permission bitmask values match flask-base's `Permission` class:
--   GENERAL     = 0x01
--   ADMINISTER  = dedicated sentinel bit (0x40000000) — see migration 004.
--
-- Add new permissions by carving out unused LOW bits (0x02, 0x04, 0x08, …).
-- ADMINISTER is a RESERVED high bit, NOT 0xff "all bits": with 0xff a role that
-- accumulated the low bits would accidentally become admin. (This file still
-- seeds the admin role as 0xff below for history; migration 004 converts it to
-- the sentinel — don't change the value here, migrations are immutable.)
--
-- NOTE: MigrationRunner wraps each file in ONE transaction (under an advisory
-- lock) and records schema_migrations in that same transaction. Do NOT add
-- BEGIN/COMMIT here — an embedded COMMIT ends the runner's transaction early,
-- drops the lock mid-DDL, and breaks the atomic version bookkeeping.

CREATE EXTENSION IF NOT EXISTS "pgcrypto";   -- gen_random_uuid()
CREATE EXTENSION IF NOT EXISTS "citext";     -- case-insensitive email

-- Roles -----------------------------------------------------------------
CREATE TABLE IF NOT EXISTS roles (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR(64) UNIQUE NOT NULL,
    -- Bitmask. See Permission class in src/domain/Role.hpp for the
    -- canonical bit definitions.
    permissions INTEGER     NOT NULL DEFAULT 0,
    -- Exactly one row should have default = TRUE; new users without an
    -- explicit role assignment fall into it.
    is_default  BOOLEAN     NOT NULL DEFAULT FALSE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_roles_one_default
    ON roles (is_default) WHERE is_default = TRUE;

-- Seed the two starter roles flask-base ships. Idempotent — re-running
-- the migration just no-ops because of the UNIQUE on name.
INSERT INTO roles (name, permissions, is_default) VALUES
    ('User',          1,    TRUE),    -- 0x01 GENERAL
    ('Administrator', 255,  FALSE)    -- 0xff ADMINISTER
ON CONFLICT (name) DO NOTHING;

-- Users -----------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email         CITEXT       UNIQUE NOT NULL,
    -- argon2id hash from libsodium (crypto_pwhash_str). NULL is allowed so
    -- the invite flow can create a row before the user sets their own
    -- password — exactly like flask-base's User.password_hash being
    -- nullable until /join-from-invite/.
    password_hash TEXT,
    first_name    VARCHAR(64),
    last_name     VARCHAR(64),
    confirmed     BOOLEAN      NOT NULL DEFAULT FALSE,
    role_id       INTEGER      NOT NULL REFERENCES roles(id) ON DELETE RESTRICT,
    created_at    TIMESTAMPTZ  NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ  NOT NULL DEFAULT now()
);

-- NB: no separate index on email — the UNIQUE constraint already creates one.
CREATE INDEX IF NOT EXISTS idx_users_role_id ON users (role_id);
CREATE INDEX IF NOT EXISTS idx_users_confirmed ON users (confirmed) WHERE confirmed = FALSE;

-- updated_at trigger ----------------------------------------------------
-- flask-base parity: SQLAlchemy event hooks; we use a plain SQL trigger.
CREATE OR REPLACE FUNCTION users_touch_updated_at() RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS users_touch_updated_at ON users;
CREATE TRIGGER users_touch_updated_at
    BEFORE UPDATE ON users
    FOR EACH ROW EXECUTE FUNCTION users_touch_updated_at();
