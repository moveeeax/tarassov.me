-- Migration 002: used_tokens — DB-level one-shot guard for single-use email
-- tokens (confirm / reset-password / change-email).
--
-- Replaces the cache-only nonce, which failed OPEN (a Redis outage let a
-- captured token be replayed to re-set a victim's password). The DB INSERT
-- ... ON CONFLICT DO NOTHING is the authoritative single-use check.
--
-- NOTE: the MigrationRunner wraps this file in one transaction — do NOT add
-- BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS used_tokens (
    token_hash TEXT PRIMARY KEY,                 -- sha256 hex of the token
    used_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ NOT NULL              -- prune rows past this
);

-- Lets a periodic cleanup drop expired markers (DELETE FROM used_tokens
-- WHERE expires_at < now()).
CREATE INDEX IF NOT EXISTS idx_used_tokens_expires_at ON used_tokens (expires_at);
