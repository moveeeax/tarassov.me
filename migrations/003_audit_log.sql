-- Migration 003: audit_log — an append-only trail of privileged actions.
-- The template ships exactly the operations that need a trail (admin user/role
-- CRUD, invites), so wire Security::Audit::record() into those mutations.
--
-- NOTE: the MigrationRunner wraps this file in one transaction — no BEGIN/COMMIT.

CREATE TABLE IF NOT EXISTS audit_log (
    id          BIGSERIAL PRIMARY KEY,
    actor_id    TEXT,                        -- principal subject (uuid) or NULL for system
    action      TEXT NOT NULL,               -- e.g. "user.create", "role.delete"
    target_type TEXT NOT NULL,               -- e.g. "user", "role"
    target_id   TEXT,                        -- id of the affected entity (uuid or int as text)
    details     JSONB NOT NULL DEFAULT '{}', -- action-specific context
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_audit_log_created_at ON audit_log (created_at DESC);
CREATE INDEX IF NOT EXISTS idx_audit_log_actor ON audit_log (actor_id);
