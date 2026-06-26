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
| `API_PUBLIC_PATHS` | `api.public_paths` | csv | `/,…,/api/auth/login,/api/auth/register,/api/auth/refresh,/api/account/confirm/*,/api/account/reset-password-request,/api/account/reset-password/*,/api/account/change-email/*` | Paths that bypass auth + rate limit. Exact-match; a trailing `*` is a prefix match (used for the token-bearing account routes). |
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
| `RATE_LIMIT_TRUST_PROXY` | `rate_limit.trust_proxy` | bool | `false` | Use `X-Forwarded-For` |
| `RATE_LIMIT_FAIL_OPEN` | `rate_limit.fail_open` | bool | `true` | Allow if Redis is down |
| `RATE_LIMIT_WHITELIST` | `rate_limit.whitelist` | csv | — | IPs / user IDs that bypass |

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
| `DOCS_ENABLED` | `docs.enabled` | bool | `false` | Mount `/api/docs` + `/api/openapi.yaml` — dev only |
| `DOCS_OPENAPI_PATH` | `docs.openapi_path` | string | `docs/openapi.yaml` | Path served at `/api/openapi.yaml` |

## Object storage

`Storage::get()` is a get/put/remove seam (`src/storage/Storage.hpp`). Only the
`local` filesystem backend ships; swap in S3/GCS by subclassing `StorageBackend`.

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `STORAGE_BACKEND` | `storage.backend` | string | `local` | Only `local` is built in; any other value fails fast at boot |
| `STORAGE_LOCAL_ROOT` | `storage.local.root` | string | `data/uploads` | Directory the local backend writes objects under (gitignored) |
| `STORAGE_PUBLIC_BASE_URL` | `storage.public_base_url` | string | — | Prepended to a key by `url()` (e.g. a CDN base); empty → returns the bare key |

## Observability

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `LOG_NAME` | `logging.name` | string | `cpp_api` | |
| `LOG_FILE` | `logging.file` | string | `logs/app.log` | |
| `LOG_LEVEL` | `logging.level` | enum | `info` | trace/debug/info/warn/error/critical |
| `LOG_FORMAT` | `logging.format` | enum | `text` | `text` (human) or `json` (one JSON object per line for Loki/ELK) |
| `METRICS_ADDRESS` | `observability.metrics_address` | string | `0.0.0.0:9090` | |
| `SERVICE_NAME` | `observability.service_name` | string | `cpp_api_service` | Also emitted as `service` field in JSON logs |
| `OTLP_ENDPOINT` | `observability.otlp_endpoint` | string | — | OTLP HTTP traces endpoint. Empty + `trace_stdout=false` → no-op tracer |
| `TRACE_STDOUT` | `observability.trace_stdout` | bool | `false` | Synchronous stdout span exporter for debugging. When `OTLP_ENDPOINT` is empty and this is off, tracing is a no-op |

## Database

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `DATABASE_PRIMARY_URL` | `database.primary` | string | `postgresql://localhost:5432/appdb` | Connection string |
| `DATABASE_REPLICA_URLS` | `database.replicas` | csv | — | Read replicas |
| `DB_POOL_SIZE` | `database.pool_size` | int | `10` | Per-pool connections (primary + each replica). Keep ≥ `server.threads`: a smaller pool makes IO threads queue on `acquire()`; a much larger pool leaves the extra connections inert (and the `db_pool` saturation gauge under-reports). |
| `DB_ACQUIRE_TIMEOUT_MS` | `database.acquire_timeout_ms` | int | `5000` | |
| `DB_STATEMENT_TIMEOUT_MS` | `database.statement_timeout_ms` | int | `30000` | Per-connection PostgreSQL `statement_timeout`. `0` disables. |
| `DB_MIGRATIONS_ENABLED` | `database.migrations_enabled` | bool | `true` | Set `false` when init-container runs them |
| `DB_MIGRATIONS_DIR` | `database.migrations_dir` | string | `migrations` | |
| `DB_RETRY_MAX_ATTEMPTS` | `database.retry.max_attempts` | int | `3` | |
| `DB_RETRY_BASE_DELAY_MS` | `database.retry.base_delay_ms` | int | `100` | |
| `DB_RETRY_MAX_DELAY_MS` | `database.retry.max_delay_ms` | int | `2000` | |
| `DB_RETRY_JITTER` | `database.retry.jitter` | bool | `true` | Full-jitter backoff |

For individual Postgres URL components used by the sample config:
`DATABASE_USER`, `DATABASE_PASSWORD`, `DATABASE_HOST`, `DATABASE_PORT`, `DATABASE_NAME`.

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
| `KAFKA_PRODUCER_ID` | `messaging.kafka.producer.client_id` | string | `cpp_producer` | |
| `KAFKA_CONSUMER_ENABLED` | `messaging.kafka.consumer.enabled` | bool | `false` | |
| `KAFKA_GROUP_ID` | `messaging.kafka.consumer.group_id` | string | `cpp_consumer_group` | |

## Jobs

| Env | JSON key | Type | Default |
|---|---|---|---|
| `JOBS_ENABLED` | `jobs.enabled` | bool | `false` |
| `JOBS_RESULT_TTL` | `jobs.result_ttl` | int | `86400` |
| `JOBS_MAX_RETRIES` | `jobs.max_retries` | int | `3` |
| `JOBS_DLQ_METRIC_REFRESH_SEC` | `jobs.dlq_metric_refresh_sec` | int | `10` | Exports `jobs_dlq_depth{type="..."}` plus an aggregate `type="_total"` |
| `DB_REPLICA_LAG_METRIC_REFRESH_SEC` | `database.replica_lag_metric_refresh_sec` | int | `15` | Refresh interval for the `db_replica_lag_seconds` gauge. Only registered when read replicas are configured (primary has no replay timestamp). |

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

## Worker (second binary, `cpp_api_template_worker`)

| Env | JSON key | Type | Default |
|---|---|---|---|
| `WORKER_ID` | `worker.id` | string | `worker-1` |
| `WORKER_TYPES` | `worker.types` | csv | `default` | Queues the worker pulls from. MUST include `account_email`, `email.send`, and `webhook.deliver` or those jobs pile up undrained. |
| `WORKER_CONCURRENCY` | `worker.concurrency` | int | `2` |
| `WORKER_HEALTH_PORT` | `worker.health_port` | int | `9091` |
| `WORKER_BRPOP_TIMEOUT` | `worker.brpop_timeout` | int | `5` |

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

CONFIG_FILE=config/local.json ./cpp_api_template
```
