# Configuration reference

Every knob has three ways in, tried in order:

1. **Environment variable** (highest priority — for containers).
2. **`config/config.json`** value, with `${VAR}` / `${VAR:-default}` expansion.
3. **Built-in default** baked into the code.

Set `CONFIG_FILE` to point at a different JSON file (e.g.
`config/worker.json` for the worker binary).

---

## App

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `APP_NAME` | `app.name` | string | `App` | Display name used in email subjects / templates |
| `APP_BASE_URL` | `app.base_url` | string | `http://localhost:8080` | Public origin used to build links in account emails (confirm / reset / change-email) |
| `SITE_BASE_URL` | `site.base_url` | string | `""` | Canonical public origin for the SEO surface (sitemap `<loc>`, `rel=canonical`, og:url, JSON-LD). **Required non-empty `https://` URL in production** — the binary refuses to start otherwise; empty in dev falls back to request-header derivation |
| `SITE_PAGES_TEMPLATES_DIR` | `site.pages_templates_dir` | string | `templates/pages` | Directory with server-rendered page templates (blog post shell, 404) |

## Server

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `SERVER_HOST` | `server.host` | string | `0.0.0.0` | Listen address |
| `SERVER_PORT` | `server.port` | int | `8080` | |
| `SERVER_THREADS` | `server.threads` | int | `0` (auto = #cores) | Drogon event-loop threads. Under the **synchronous** pqxx model the in-flight DB-call count is capped by THIS, not by `database.pool_size` — it's the real concurrency knob. `0`/unset auto-sizes to the CPU count; keep `database.pool_size` ≥ threads (the app warns at boot if not). |
| `SERVER_MAX_BODY_BYTES` | `server.max_body_bytes` | int | `10485760` | 10 MB cap on request bodies — prevents memory blow-up from a single client. Bump for file uploads. |
| `SERVER_SSL_ENABLED` | `server.ssl.enabled` | bool | `false` | Off by default — production terminates TLS at the ingress/reverse proxy (the Helm chart assumes this). Exposing the app directly (bare-metal, no proxy)? set `true` + cert/key, else traffic is plain HTTP. |
| `SSL_CERT_FILE` | `server.ssl.cert` | string | — | PEM cert path when SSL on |
| `SSL_KEY_FILE` | `server.ssl.key` | string | — | PEM key path when SSL on |
| `SHUTDOWN_PRE_STOP_DELAY_SEC` | `shutdown.pre_stop_delay_sec` | int | `5` | Seconds between "readiness = 503" and Drogon quit |

## API & middleware

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `API_PUBLIC_PATHS` | `api.public_paths` | csv | `/,/healthz,/ready,/health,/metrics,/api/v1/docs,/api/v1/openapi.yaml,/api/v1/auth/login,/api/v1/auth/register,/api/v1/auth/refresh,/api/v1/account/confirm/*,/api/v1/account/reset-password-request,/api/v1/account/reset-password/*,/api/v1/account/change-email/*,/api/v1/account/join-from-invite/*,/sitemap.xml,/blog/*` | Paths that bypass auth + the general rate limit (`kDefaultPublicPathsCsv` in `src/utils/Strings.hpp`). Exact-match; a trailing `*` is a prefix match (used for the token-bearing account routes). The auth/account subset is still throttled by the `rate_limit.protected_*` limiter. |
| `CORS_ALLOWED_ORIGINS` | `cors.allowed_origins` | csv | — | Empty disables CORS |

## Auth

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `AUTH_MODE` | `auth.mode` | enum | `none` | `none` \| `bearer` \| `jwt` |
| `AUTH_BEARER_TOKEN` | `auth.bearer_token` | string | — | Required when `mode=bearer` |
| `JWT_SECRET` | `auth.jwt.secret` | string | — | Required when `mode=jwt` |
| `JWT_ISSUER` | `auth.jwt.issuer` | string | — | Checked if non-empty |
| `JWT_AUDIENCE` | `auth.jwt.audience` | string | — | Checked if non-empty |
| `JWT_LEEWAY_SEC` | `auth.jwt.leeway_sec` | int | `30` | Clock skew tolerance |
| `JWT_ROLES_CLAIM` | `auth.jwt.roles_claim` | string | `roles` | JSON claim for RBAC |
| `JWT_SCOPES_CLAIM` | `auth.jwt.scopes_claim` | string | `scope` | Space-separated per OAuth2 |
| `AUTH_COOKIES_ENABLED` | `auth.cookies.enabled` | bool | `false` | Cookie sessions for the SPA (access+refresh) |
| `AUTH_COOKIE_ACCESS` | `auth.cookies.access_name` | string | `__Host-access` | Strip `__Host-` prefix for plain-http dev |
| `AUTH_COOKIE_REFRESH` | `auth.cookies.refresh_name` | string | `__Host-refresh` | |
| `AUTH_COOKIE_ACCESS_TTL_SEC` | `auth.cookies.access_ttl_sec` | int | `900` | 15 min |
| `AUTH_COOKIE_REFRESH_TTL_SEC` | `auth.cookies.refresh_ttl_sec` | int | `604800` | 7 days |
| `AUTH_COOKIE_SECURE` | `auth.cookies.secure` | bool | `true` | Set `false` only for http://localhost |
| `AUTH_COOKIE_SAMESITE` | `auth.cookies.samesite` | enum | `Lax` | `Lax` \| `Strict` \| `None` |
| `AUTH_COOKIE_REVOCATION_PREFIX` | `auth.cookies.refresh_revocation_prefix` | string | `auth:refresh:` | Redis prefix for refresh-JTI revocation |

## Rate limit

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `RATE_LIMIT_ENABLED` | `rate_limit.enabled` | bool | `false` | |
| `RATE_LIMIT_REQUESTS` | `rate_limit.requests` | int | `60` | Max per window |
| `RATE_LIMIT_WINDOW_SEC` | `rate_limit.window_sec` | int | `60` | |
| `RATE_LIMIT_SCOPE` | `rate_limit.scope` | enum | `ip_or_user` | `ip` \| `ip_or_user` |
| `RATE_LIMIT_PROTECTED_REQUESTS` | `rate_limit.protected_requests` | int | `10` | Tighter per-window budget for the brute-force surfaces (login/register/refresh + token-bearing account links) |
| `RATE_LIMIT_PROTECTED_WINDOW_SEC` | `rate_limit.protected_window_sec` | int | `60` | |
| `RATE_LIMIT_PROTECTED_PATHS` | `rate_limit.protected_paths` | csv | `/api/v1/auth/login,/api/v1/auth/register,/api/v1/auth/refresh,/api/v1/account/confirm/*,/api/v1/account/reset-password-request,/api/v1/account/reset-password/*,/api/v1/account/change-email/*,/api/v1/account/join-from-invite/*` | Auth-public paths that must STILL be rate-limited (`kDefaultProtectedPathsCsv` in `src/utils/Strings.hpp`). Same matching rules as `api.public_paths`. |
| `RATE_LIMIT_TRUST_PROXY` | `rate_limit.trust_proxy` | bool | `false` | Use `X-Forwarded-For` |
| `RATE_LIMIT_TRUSTED_PROXY_COUNT` | `rate_limit.trusted_proxy_count` | int | `1` | How many trailing `X-Forwarded-For` hops are trusted; values < 1 are clamped to 1 |
| `RATE_LIMIT_FAIL_OPEN` | `rate_limit.fail_open` | bool | `true` | Allow if Redis is down |
| `RATE_LIMIT_WHITELIST` | `rate_limit.whitelist` | csv | — | IPs / user IDs that bypass |

## Security headers & CSRF

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `SECURITY_HSTS` | `security.hsts` | bool | `false` | Emit `Strict-Transport-Security` (only honoured over HTTPS — enable when TLS terminates in front of the app) |
| `SECURITY_HSTS_MAX_AGE` | `security.hsts_max_age` | int | `31536000` | HSTS `max-age` in seconds (1 year) |
| `SECURITY_CSRF_ENABLED` | `security.csrf.enabled` | bool | `false` | Double-submit CSRF check for cookie-session mutations. The binary warns at boot when cookies are on in production without it. |
| `SECURITY_CSRF_COOKIE` | `security.csrf.cookie_name` | string | `csrf-token` | Cookie carrying the CSRF token |
| `SECURITY_CSRF_HEADER` | `security.csrf.header_name` | string | `X-CSRF-Token` | Header the SPA must echo the token back in |

## Idempotency

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `IDEMPOTENCY_ENABLED` | `idempotency.enabled` | bool | `false` | |
| `IDEMPOTENCY_TTL_SEC` | `idempotency.ttl_sec` | int | `86400` | |
| `IDEMPOTENCY_MAX_BODY_KB` | `idempotency.max_body_kb` | int | `1024` | Reject oversized request bodies (413) |
| `IDEMPOTENCY_MAX_RESPONSE_KB` | `idempotency.max_response_kb` | int | `256` | Skip caching oversized responses (no replay) |
| `IDEMPOTENCY_LOCK_TTL_SEC` | `idempotency.lock_ttl_sec` | int | `30` | In-flight lock for concurrent same-key requests |

## Docs / Swagger UI

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `DOCS_ENABLED` | `docs.enabled` | bool | `false` | Mount `/api/v1/docs` + `/api/v1/openapi.yaml` — dev only |
| `DOCS_OPENAPI_PATH` | `docs.openapi_path` | string | `docs/openapi.yaml` | Path served at `/api/v1/openapi.yaml` |

## Object storage

`Storage::get()` is a get/put/remove seam (`src/storage/Storage.hpp`). Two
backends ship: `local` (filesystem) and `s3` (S3-compatible — MinIO / R2 / AWS);
add others by subclassing `StorageBackend`.

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `STORAGE_BACKEND` | `storage.backend` | string | `local` | `local` \| `s3`; any other value fails fast at boot |
| `STORAGE_LOCAL_ROOT` | `storage.local.root` | string | `data/uploads` | Directory the local backend writes objects under (gitignored) |
| `STORAGE_PUBLIC_BASE_URL` | `storage.public_base_url` | string | — | Prepended to a key by `url()` (e.g. a CDN base); empty → returns the bare key. Used by both backends. |
| `S3_ENDPOINT` | `storage.s3.endpoint` | string | — | S3-compatible endpoint URL. **Required** when `backend=s3` (boot fails without it) |
| `S3_REGION` | `storage.s3.region` | string | `us-east-1` | |
| `S3_BUCKET` | `storage.s3.bucket` | string | — | **Required** when `backend=s3` (boot fails without it) |
| `S3_ACCESS_KEY` | `storage.s3.access_key` | string | — | |
| `S3_SECRET_KEY` | `storage.s3.secret_key` | string | — | |

## Observability

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `LOG_NAME` | `logging.name` | string | `tarassov_me` | |
| `LOG_FILE` | `logging.file` | string | `logs/app.log` | |
| `LOG_LEVEL` | `logging.level` | enum | `info` | trace/debug/info/warn/error/critical |
| `LOG_FORMAT` | `logging.format` | enum | `text` | `text` (human) or `json` (one JSON object per line for Loki/ELK) |
| `METRICS_ADDRESS` | `observability.metrics_address` | string | `0.0.0.0:9090` | |
| `SERVICE_NAME` | `observability.service_name` | string | `tarassov_me_service` | Also emitted as `service` field in JSON logs |
| `OTLP_ENDPOINT` | `observability.otlp_endpoint` | string | — | OTLP HTTP traces endpoint. Empty + `trace_stdout=false` → no-op tracer |
| `TRACE_STDOUT` | `observability.trace_stdout` | bool | `false` | Synchronous stdout span exporter for debugging. When `OTLP_ENDPOINT` is empty and this is off, tracing is a no-op |

## Database

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `DATABASE_PRIMARY_URL` | `database.primary` | string | `""` | Full connection string (takes precedence). Empty → a libpq DSN is assembled from the discrete parts below, so the password lives only in `DATABASE_PASSWORD` and never in a URL env var |
| `DATABASE_REPLICA_URLS` | `database.replicas` | csv | — | Read replicas as full connection strings. **First tier** of the replica resolution order below |
| `DATABASE_REPLICA_HOSTS` | `database.replicas` | csv | — | Read replicas as bare **hostnames**; each is assembled into a DSN with the primary's port/user/dbname/password, so the password never lands in a URL env var. What the Helm charts emit (from `externalDatabase.replicaHost`). **Second tier** |
| `DB_POOL_SIZE` | `database.pool_size` | int | `10` | Per-pool connections (primary + each replica). Keep ≥ `server.threads`: a smaller pool makes IO threads queue on `acquire()`; a much larger pool leaves the extra connections inert (and the `db_pool` saturation gauge under-reports). |
| `DB_ACQUIRE_TIMEOUT_MS` | `database.acquire_timeout_ms` | int | `5000` | |
| `DB_STATEMENT_TIMEOUT_MS` | `database.statement_timeout_ms` | int | `30000` | Per-connection PostgreSQL `statement_timeout`. `0` disables. |
| `DB_MIGRATIONS_ENABLED` | `database.migrations_enabled` | bool | `true` | Set `false` when init-container runs them |
| `DB_MIGRATIONS_DIR` | `database.migrations_dir` | string | `migrations` | |
| `DB_RETRY_MAX_ATTEMPTS` | `database.retry.max_attempts` | int | `2` | Kept tight on purpose: retries sleep synchronously on the IO loop (see `Core.hpp`) |
| `DB_RETRY_BASE_DELAY_MS` | `database.retry.base_delay_ms` | int | `20` | |
| `DB_RETRY_MAX_DELAY_MS` | `database.retry.max_delay_ms` | int | `200` | Defaults bound the worst case to ≈0.2s per request |
| `DB_RETRY_JITTER` | `database.retry.jitter` | bool | `true` | Full-jitter backoff |

Individual Postgres components (used when `DATABASE_PRIMARY_URL` is empty):
`DATABASE_HOST` (default `localhost`), `DATABASE_PORT` (`5432`),
`DATABASE_USER` (`app`), `DATABASE_NAME` (`app`), `DATABASE_PASSWORD` (empty).

### How the replica list is resolved

`Core::read_replicas_` tries three sources **in order and stops at the first
non-empty one** — it does not merge them, and an empty value is *skipped*
rather than treated as "no replicas":

1. `DATABASE_REPLICA_URLS` (env, CSV of full connection strings)
2. `DATABASE_REPLICA_HOSTS` (env, CSV of hostnames → DSNs via `Pg::make_conninfo`)
3. `database.replicas` (a JSON array in the config file)

Operational consequence: `DATABASE_REPLICA_URLS=""` does **not** disable
replicas. In Kubernetes the charts never emit tier 1 — they emit tier 2 only
(`DATABASE_REPLICA_HOSTS`, rendered from `externalDatabase.replicaHost`; the
ConfigMap carries no `database.replicas` key), so an empty tier-1 value just
falls through and stale reads continue. To actually go primary-only, clear
`externalDatabase.replicaHost` and roll the pods — see
[RUNBOOK.md § RepLag](RUNBOOK.md#replag).

### Read replicas and `DB_POOL_SIZE`

Setting `DATABASE_REPLICA_URLS` routes most reads to a replica, but a few
paths deliberately read from the **primary** to get read-after-write
consistency (via `Database::execute_read_primary`), regardless of the replica
config: the account email worker, the admin "update-echo" read-back after a
mutation, and `--verify-migrations`. So sizing `DB_POOL_SIZE` (the primary
pool) only for HTTP traffic under-counts: the background email worker competes
for the same primary connections. Budget primary `DB_POOL_SIZE` for request
handlers **plus** the worker, even when replicas absorb the bulk of reads.

## Cache (Redis)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `REDIS_URL` | `cache.url` | string | `tcp://127.0.0.1:6379` | Standalone mode |
| `REDIS_PASSWORD` | `cache.password` | string | — | |
| `CACHE_POOL_SIZE` | `cache.pool_size` | int | `10` | |
| `REDIS_USE_SENTINEL` | `cache.use_sentinel` | bool | `false` | |
| `REDIS_MASTER_NAME` | `cache.sentinel.master_name` | string | `mymaster` | |
| `REDIS_SENTINEL_NODES` | `cache.sentinel.nodes` | csv | — | `host:port,host:port,...` |
| `REDIS_SENTINEL_PASSWORD` | `cache.sentinel.password` | string | falls back to `REDIS_PASSWORD` | |
| `REDIS_SOCKET_TIMEOUT_MS` | `cache.socket_timeout_ms` | int | `500` | Per-command timeout; tighten under low-latency hot paths, loosen for large values / EVAL. |
| `REDIS_POOL_WAIT_TIMEOUT_MS` | `cache.pool_wait_timeout_ms` | int | `500` | Max wait for a free connection from the pool. |

For URL components: `REDIS_HOST`, `REDIS_PORT`.

## Messaging (Kafka)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `MESSAGING_ENABLED` | `messaging.enabled` | bool | `false` | Parent switch |
| `KAFKA_BROKERS` | `messaging.kafka.brokers` | string | `localhost:9092` | |
| `KAFKA_PRODUCER_ENABLED` | `messaging.kafka.producer.enabled` | bool | `false` | |
| `KAFKA_PRODUCER_ID` | `messaging.kafka.producer.client_id` | string | `tarassov_me_producer` | |
| `KAFKA_CONSUMER_ENABLED` | `messaging.kafka.consumer.enabled` | bool | `false` | |
| `KAFKA_GROUP_ID` | `messaging.kafka.consumer.group_id` | string | `cpp_consumer_group` | |

## Jobs

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `JOBS_ENABLED` | `jobs.enabled` | bool | `false` | |
| `JOBS_RESULT_TTL` | `jobs.result_ttl` | int | `86400` | |
| `JOBS_MAX_RETRIES` | `jobs.max_retries` | int | `3` | |
| `JOBS_RETRY_BACKOFF_BASE_MS` | `jobs.retry_backoff_base_ms` | int | `0` | Exponential retry backoff base. `0` keeps the legacy immediate-requeue behaviour |
| `JOBS_RETRY_BACKOFF_MAX_MS` | `jobs.retry_backoff_max_ms` | int | `60000` | Backoff cap |
| `JOBS_VISIBILITY_TIMEOUT_SEC` | `jobs.visibility_timeout_sec` | int | `0` | Lease for in-flight jobs; expired leases are reaped by the worker loop. `0` = no lease |
| `JOBS_DLQ_METRIC_REFRESH_SEC` | `jobs.dlq_metric_refresh_sec` | int | `10` | Exports `jobs_dlq_depth{type="..."}` plus an aggregate `type="_total"` |
| `JOBS_QUEUE_METRIC_REFRESH_SEC` | `jobs.queue_metric_refresh_sec` | int | `10` | Refresh interval for `jobs_queue_depth{type="..."}` |
| `DB_REPLICA_LAG_METRIC_REFRESH_SEC` | `database.replica_lag_metric_refresh_sec` | int | `15` | Refresh interval for the `db_replica_lag_seconds` gauge. Only registered when read replicas are configured (primary has no replay timestamp). |

### Retry backoff and the visibility lease in Kubernetes

The two knobs that make retries survivable both default to `0`, which is a
**no-op, not a safe default**:

- `jobs.retry_backoff_base_ms = 0` → `fail()` re-queues immediately, so three
  attempts finish in single-digit milliseconds and a 3-second SMTP blip
  permanently dead-letters a signup email.
- `jobs.visibility_timeout_sec = 0` → no lease is taken, so
  `reap_expired_leases()` never reclaims anything and a worker pod that dies
  mid-job leaves that job stranded.

`config/config.production.json` sets them (`1000` / `60000` / `300`), but that
file is not mounted in Kubernetes — the Helm ConfigMap is. The worker chart
therefore carries its own values, defaulting to the production-profile numbers:

| Helm value (`helm/tarassov-me-worker`) | Renders to | Default |
|---|---|---|
| `jobs.retryBackoffBaseMs` | `jobs.retry_backoff_base_ms` + `JOBS_RETRY_BACKOFF_BASE_MS` | `1000` |
| `jobs.retryBackoffMaxMs` | `jobs.retry_backoff_max_ms` + `JOBS_RETRY_BACKOFF_MAX_MS` | `60000` |
| `jobs.visibilityTimeoutSec` | `jobs.visibility_timeout_sec` + `JOBS_VISIBILITY_TIMEOUT_SEC` | `300` |

Set `visibilityTimeoutSec` comfortably above the slowest handler's wall time
(SMTP delivery dominates) — a lease that expires while the job is still running
lets a sibling worker pick it up a second time.

## Mail (SMTP)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `MAIL_ENABLED` | `mail.enabled` | bool | `false` | Off → links are logged at INFO instead of sent |
| `MAIL_VIA_JOBS` | `mail.via_jobs` | bool | `true` | Route account emails through the `account_email` job queue when Jobs is enabled (worker must subscribe to that type); falls back to inline send when Jobs is off or enqueue fails |
| `MAIL_SMTP_HOST` | `mail.smtp_host` | string | `mailpit` | `config.json` default targets the Mailpit dev sidecar |
| `MAIL_SMTP_PORT` | `mail.smtp_port` | int | `1025` | |
| `MAIL_SMTP_USERNAME` | `mail.smtp_username` | string | — | Empty → anonymous |
| `MAIL_SMTP_PASSWORD` | `mail.smtp_password` | string | — | |
| `MAIL_SMTP_USE_TLS` | `mail.smtp_use_tls` | bool | `false` | STARTTLS; implicit TLS on port 465 |
| `MAIL_FROM` | `mail.from` | string | `noreply@example.com` | |
| `MAIL_FROM_NAME` | `mail.from_name` | string | `App` | |
| `MAIL_SUBJECT_PREFIX` | `mail.subject_prefix` | string | `[App] ` | Note the trailing space. If the prefix doesn't end in a space, one is inserted between prefix and subject automatically |
| `MAIL_TEMPLATES_DIR` | `mail.templates_dir` | string | `templates/email` | Relative to the working directory |
| `MAIL_TIMEOUT_SEC` | `mail.timeout_sec` | int | `30` | |
| `CONTACT_EMAIL` | `mail.contact_to` | string | — | Recipient of the public contact form (`POST /api/v1/public/contact`). Empty → the endpoint answers 503 `contact_disabled` |

## Worker (second binary, `tarassov_me_worker`)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `WORKER_ID` | `worker.id` | string | `worker-1` | Must be **unique per process**: it keys the `jobs:processing:<id>-<n>` recovery lists that `recover_processing()` sweeps and deletes at startup. In Kubernetes derive it from `metadata.name` — a static value shared by two replicas makes a starting pod re-queue whatever its sibling is executing |
| `WORKER_TYPES` | `worker.types` | csv | `default` | Queues the worker pulls from. MUST include `account_email`, `email.send`, and `webhook.deliver` or those jobs pile up undrained. |
| `WORKER_STRICT_TYPES` | `worker.strict_types` | bool | `false` | A `WORKER_TYPES` entry with no registered handler is normally just a boot warning; `true` upgrades it to a hard refusal to start |
| `WORKER_CONCURRENCY` | `worker.concurrency` | int | `2` | BRPOP threads per process; each takes its own `jobs:processing:<id>-<n>` list |
| `WORKER_HEALTH_PORT` | `worker.health_port` | int | `9091` | |
| `WORKER_BRPOP_TIMEOUT` | `worker.brpop_timeout` | int | `5` | |

## Conventions

- `csv`: comma-separated values, no spaces around commas. Empty components dropped.
- `enum`: invalid values fall back to the default, never throw.
- Passwords and secrets must never be committed to `config/*.json` — use `${VAR}`
  placeholders so the checked-in file stays safe.
- Config files live under `/app/config` in the Docker image; mount a volume or
  set `CONFIG_FILE` to point at something else.

## Local override pattern

```bash
# config/local.json (gitignored)
{
  "server": { "port": 8081 },
  "auth":   { "mode": "jwt" }
}

CONFIG_FILE=config/local.json ./tarassov_me
```
