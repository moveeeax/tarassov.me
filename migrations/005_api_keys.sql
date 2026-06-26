-- 005: API keys / personal access tokens for machine clients.
--
-- A key is shown to the user exactly ONCE at creation; only its SHA-256 hash is
-- stored (keys are high-entropy, so a fast hash is fine — unlike passwords). The
-- request layer hashes the presented key and looks it up here. Revocation is a
-- soft delete (revoked_at) so an audit of "what existed" survives.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction.

CREATE TABLE IF NOT EXISTS api_keys (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id      UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name         TEXT        NOT NULL,
    key_hash     TEXT        NOT NULL UNIQUE,  -- sha256_hex(secret); never the secret
    prefix       TEXT        NOT NULL,         -- first chars of the key, for display
    last_used_at TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Lookups: by hash (every authenticated API request) and by owner (management).
CREATE INDEX IF NOT EXISTS idx_api_keys_user_id ON api_keys (user_id);
-- Active keys only — the hot auth path filters revoked_at IS NULL.
CREATE INDEX IF NOT EXISTS idx_api_keys_active_hash ON api_keys (key_hash) WHERE revoked_at IS NULL;
