# Changelog

All notable changes to this project are documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.5.5] — 2026-07-19

### Added
- **SEO**: Open Graph + Twitter Card tags with a rendered 1200×630 og-image
  (the closed BookCard cover), `rel=canonical` on all public pages, JSON-LD
  `Person` schema (jobTitle, sameAs profiles), real `robots.txt` and
  `sitemap.xml` (previously the SPA fallback served HTML for both).
- Sharper meta descriptions on the vCard and blog pages.

## [1.5.4] — 2026-07-19

### Fixed
- **Contact form: replies now reach the sender.** The email carries a
  `Reply-To` with the submitter's address, so hitting "Reply" answers the
  person who wrote — the envelope From stays the site's authenticated
  MAIL_FROM (SPF/DKIM alignment). `Email::SendEmail::send()` gains an
  optional `reply_to` argument.

## [1.5.3] — 2026-07-19

### Fixed
- **Runtime image ships `ca-certificates`**: `--no-install-recommends` was
  silently dropping the CA bundle, so any outbound TLS from the app failed —
  discovered when the Mailer's SMTP STARTTLS to Brevo died with curl's
  "Problem with the SSL CA cert". Outbound email now works against a real
  TLS relay.

## [1.5.2] — 2026-07-19

### Changed
- **Public site / PROJECTS**: cards now link to GitHub (`github.com/moveeeax`).
  `cpp-rapid-rest-template` and `telegram-rest-gateway` cards (GitLab-only
  repos) replaced with cost-optimization proof: `tf-cost-diff` and
  `aws-orphan-finder`. "More on GitLab" footer and the social icon switched
  to GitHub.
- Credly badge link updated to the new profile URL
  (`credly.com/users/michael-tarassov/badges`).

## [1.5.1] — 2026-07-19

Infrastructure release: the repo moved from GitLab to GitHub
(`github.com/moveeeax/tarassov.me`), CI runs on GitHub Actions, images are
published to Docker Hub under `moveeeax/`.

### Fixed
- **`db_replica_lag_seconds` phantom lag**: the metric reported unbounded
  "lag" on a write-idle primary (frozen `pg_last_xact_replay_timestamp()`),
  firing `CppApiHighReplicaLag` with no real replication lag. The query now
  reports 0 when the replica has replayed all received WAL.
- e2e test harness: transient HTTP errors during server-startup polling no
  longer crash the binary via a joinable-thread `std::terminate`.
- GCC 13 `-Werror` build: migrated off libpqxx's deprecated
  `exec_params(...)` to `exec(query, pqxx::params)`; assorted warning fixes.

### Changed
- CI: GitHub Actions on hosted runners; vcpkg dependencies cached as their
  own image layer + GHCR cache anchor; heavy jobs run in parallel (green
  pipeline ~12 min wall).
- Release images are `linux/amd64` only (the prod nodes are amd64; the
  arm64/QEMU build was dropped).

## [1.5.0] — 2026-06-28

First production release of the personal site (tarassov.me): the public
BookCard vCard + blog, a working contact form, and admin image uploads.

### Added
- **Public site** (`/`): the BookCard vCard with real CV content, an "Опыт/стек"
  block, a real "Проекты" block (demo portfolio removed), and a live
  latest-posts widget backed by `GET /api/v1/public/posts`.
- **Contact form**: `POST /api/v1/public/contact` → email via SMTP (Mailer),
  wired through `CONTACT_EMAIL`.
- **Image storage**: S3/MinIO backend + an authenticated admin upload endpoint
  for post images (local disk backend stays the dev default).

### Changed
- README rewritten for the personal site; the Posts query key is centralized on
  the frontend.
- `values-prod.example` carries the storage + contact settings; demo comments
  dropped. Helm prod wiring filled in for a real deploy.

### Security
- Strict CSP for the `/admin` + `/auth` paths (#16).
- Upload hardening: magic-number content-type check, plus upload/contact
  endpoint hardening from the pre-launch review.
- Nav a11y fix; Helm config guard.

### Fixed
- Post create/update no longer reuses the `status` param inside a `CASE`;
  `PostRepository` status param type clash resolved.
- Upload build: correct Drogon multipart header.

### CI
- Retry the buildx + coverage jobs on transient vcpkg fetch failures.
- Image build/publish jobs now gate on `main` (the repo's release branch)
  instead of the non-existent `master`, so tagged releases actually publish.

## [1.4.0] — 2026-06-26

Fork-readiness hardening + API versioning. Prepares the template to be made
public and forked into production.

### ⚠ Breaking
- **All API routes are now versioned under `/api/v1`** (ADR 0006). Clients must
  call `/api/v1/...`; the bundled frontend moves in the same release. There is
  no compatibility alias — deploy backend and frontend together. Probe/infra
  routes (`/healthz`, `/ready`, `/health`, `/metrics`) stay unversioned.

### Added
- API versioning convention, machine-enforced: `new-resource.sh` derives the
  route from a single `API_VERSION` var, `new-endpoint.sh` rejects unversioned
  paths, and `check-routes-registered.sh` lints for them. ADR 0006 documents it.
- Jobs: exponential retry backoff + a cross-worker visibility-timeout reaper;
  a startup guard that refuses/​warns on a `WORKER_TYPES` entry with no handler.
- Health: degraded (non-critical) probes — surfaced in `/health` without failing
  `/ready`.
- Observability: `trace_id` on every log line; PrometheusRule alerts for api/worker.
- Boot-time prod-safety checks in the binary (rate-limit off, docs UI on, CSRF
  off, weak DB password) that the Helm deploy path can't bypass.

### Changed
- DB connection string is assembled from discrete env parts so the password is
  never materialized into a URL env var; request-path DB retry defaults tightened
  so a transient blip can't stall the IO loop.
- Migrations support a `-- migrate:no-transaction` marker (autocommit, cleared
  statement_timeout) for `CREATE INDEX CONCURRENTLY` / large backfills.
- Helm ships structured JSON logs in prod; frontend drops production sourcemaps;
  critical CI images pinned by digest; emails masked in logs.
- `init-project.sh` scrubs the author's demo/infra identifiers on fork.

### Fixed
- API-key auth throttles the `last_used_at` write (CTE) instead of writing on
  every request. Numerous review-driven correctness fixes (see PRs).

## [1.3.4] — 2026-06-25

Hardening and reliability from a full project review.

### Added
- Jobs: opt-in exponential retry backoff plus a cross-worker visibility-timeout
  reaper that reclaims jobs orphaned by a crashed worker (enabled in production).
- Health: degraded (non-critical) probes — surfaced in `/health` but never fail
  `/ready`, so an optional dependency outage no longer pulls the pod from rotation.
- CI: Trivy scans for the worker and frontend images, a coverage job, frontend
  ESLint in GitLab, and full-history gitleaks.

### Changed
- Frontend: theme bootstrap moved to an external script so the production CSP no
  longer blocks it; admin pages are code-split.
- Helm: frontend and the demo-reset job run with a read-only root filesystem;
  api/worker gain a `startupProbe`.

### Fixed
- Frontend: corrected the admin permission sentinel shown in the UI
  (`0x40000000`, not `0xff`); reverse-tabnabbing and a dialog a11y gap.
- Production deploy: re-enable rate limiting and stop double-running migrations.

## [1.3.3] — 2026-06-25

Frontend redesign + the real fix for notification layout shift.

### Changed
- **Dark-first "dev-tool" redesign**: zinc neutrals + a single indigo accent,
  tighter radius/spacing, borders over shadows, driven by the `index.css`
  design tokens. Dark is now the default theme (light still available via the
  toggle). Polished Card / Nav / DataTable / job-status badges to match.

### Fixed
- **Notifications no longer shift the layout.** Server/form feedback (invalid
  login, saved, etc.) now shows as a TOAST (`position: fixed`, out of document
  flow) instead of an in-flow alert that pushed the form down. Field-level
  validation stays inline. New `useToast` / `useErrorToast`; `FormAlert`
  removed.

## [1.3.2] — 2026-06-24

Frontend UX/accessibility overhaul (a multi-agent review of the SPA) plus
real-client-IP plumbing fixes. No backend API changes.

### Changed
- **Frontend UX + a11y overhaul** (12 items): form errors no longer jolt the
  layout — a new `FormAlert` renders an always-present, `aria-live` slot that
  smoothly expands/collapses instead of inserting an Alert and shoving the
  fields down (CLS), applied across all forms. Unknown routes now show a 404
  page (was a silent redirect home). Nav gains an active-link indicator and a
  mobile hamburger menu. Reset/change/request forms moved to the shared
  `useApiMutation` (consistent pending/error handling). Admin forms use the
  accessible `FormField`; destructive deletes use a focus-trapped
  `ConfirmDialog` instead of native `confirm()`. DataTable shows skeleton rows
  on first load; tables get `scope="col"`. Dark-mode-safe job badges, explicit
  zod validation messages, responsive name grids, skip-to-content link, focus
  rings, and aria-labels on icon buttons / pagination.
- Audit detail now opens in a centered, focus-trapped modal (was appended at
  the bottom of the page, past the table).

### Fixed
- **Real client IP through the frontend path**: the tarassov-me-frontend nginx proxied
  `/api` with `X-Real-IP $remote_addr`, clobbering the real client IP with the
  ingress pod's internal address — requests via `app.<host>` audited a `10.x`
  IP. It now passes the upstream `X-Real-IP` through.
- Helm: `RATE_LIMIT_TRUST_PROXY` is always rendered (was gated on
  `rateLimit.enabled`), so the audit honors `trust_proxy` even with rate
  limiting off.

## [1.3.1] — 2026-06-24

Patch release: trusted client-IP handling and two Helm deploy fixes.

### Fixed
- **Audit honors `trust_proxy`**: the failed-login audit read `X-Real-IP`
  directly (trusting a client-spoofable header even when not behind a proxy) and
  diverged from the rate limiter. Added `RateLimit::client_ip(req)` — resolves
  `rate_limit.trust_proxy` / `trusted_proxy_count` from config (works even when
  rate limiting is disabled) and applies the same trusted-IP logic; the audit
  now uses it. Capturing the *real* client IP still requires the edge to forward
  it (PROXY protocol on the LB + ingress-nginx) and `trust_proxy=true`.
- **Helm**: guard the tarassov-me `mail-smtp-password` Secret against a nil `mail`
  map, so `helm upgrade --reuse-values` (whose reused values omit the optional
  mail block) no longer fails to render.
- **Helm**: pin the tarassov-me-env umbrella + demo image tags to the v-prefixed release
  tag (`v1.3.x`); a bare `1.3.0` matched no published image.

## [1.3.0] — 2026-06-24

Fork-readiness round: programmatic auth, outbound integrations, a storage seam,
and a batch of security/scaffolding hardening ahead of building the first real
project on the template.

**Heads-up — admin permission model changed.** `Permission::kAdminister` moved
from `0xff` to a dedicated sentinel bit (`0x40000000`). It is data-migrated
automatically (migration 004 converts the seeded `Administrator` role), so no
manual action is needed — but if you persisted the old `0xff` admin value
anywhere outside the seeded role, run the same `UPDATE`. Everything else is
additive: new config keys have defaults / env fallbacks, so existing configs
load unchanged.

### Added
- **API keys / personal access tokens** for machine clients (migration 005).
  `Security::ApiKeys::generate/authenticate`; presented via `X-API-Key` or a
  `cpk_`-prefixed `Authorization` token; only the SHA-256 hash is stored.
  Owner-scoped management at `POST/GET/DELETE /api/account/api-keys` (a user
  only ever sees/revokes their own; the secret is shown exactly once).
- **Outbound webhooks**: `Webhooks::send()` enqueues a signed (`X-Webhook-
  Signature: sha256=…` HMAC) POST delivered by the worker with retry/backoff/DLQ.
  Coarse SSRF guard refuses localhost / private / link-local / cloud-metadata
  hosts and non-http(s) schemes; redirects disabled.
- **Object/file storage seam**: `Storage::get()` (put/get/remove/exists/url)
  with a `LocalStorage` disk backend and a path-traversal guard. Swap in S3/GCS
  by subclassing `StorageBackend`. Config under `storage.*`.
- **Generic transactional email**: `Email::SendEmail::send(to, subject, text,
  html)` enqueues an `email.send` job for any ad-hoc mail (not just account
  flows).
- **Owner-scoped resources**: `new-resource.sh --owned` scaffolds an IDOR-safe
  per-user resource (owner FK migration, `CrudBase::find_owned/list_owned/
  count_owned`, `API_REQUIRE_OWNER`). The scaffolder also emits a no-infra
  domain unit test alongside the integration test.
- `Cache::cached<T>(key, ttl, loader)` read-through helper (fail-open).
- **Replica-lag observability**: `db_replica_lag_seconds` gauge +
  `CppApiHighReplicaLag` alert (only active when replicas are configured).
- Tracked, secret-free `values-prod.example.yaml` for the api / worker /
  frontend charts; `init-project.sh --no-demo` strips the flask-base reference
  material; `REMOVING-THE-DEMO.md`.
- CI gates: `CI_REQUIRE_INFRA` fails (vs skips) when integration infra is
  absent; `check-routes-registered.sh` asserts every `ADD_METHOD_TO` is in the
  endpoint registry; `check-helm-render.sh` asserts the prod overlay's security
  floor (rate-limit fail-closed, JWT, secure cookies, no committed secrets).

### Changed
- **Admin is a dedicated sentinel bit, not `0xff`** — a role accumulating the
  eight low feature bits can no longer accidentally become admin. `current_user_
  can` (and the TS mirror) give the sentinel an explicit allow-everything bypass
  so admins still pass feature gates. Migration 004 converts existing admin rows.
- `server.threads` default is now `0` = auto (one IO thread per core); the app
  warns at boot when `database.pool_size < threads` (the real DB-concurrency cap
  is the thread count, not the pool).
- Every container drops **all** Linux capabilities (`capabilities.drop: [ALL]`).
- Worker queue-type defaults include `email.send` and `webhook.deliver`.
- Docs reconciled with the CrudBase scaffolder (EXAMPLES / CONVENTIONS); the
  inaccurate "multi-arch" GitLab-CI claim corrected with deploy-arch guidance;
  `make warm-cache` falls back to the upstream GHCR cache and `make doctor`
  probes Colima memory. `app_core` introduced as the single CMake dependency seam.

### Fixed
- **Failed login attempts are now audited** (`auth.login_failed` with the
  attempted email + source IP) — brute-force was previously invisible.

### Security
- Privilege-escalation prevented via the admin sentinel bit (above).
- API-key secrets hashed at rest; revocation is owner-scoped and returns 404 for
  others' / missing keys (no enumeration oracle).
- Webhook SSRF guard; storage path-traversal guard; containers drop all caps.
- Prod overlay ships the rate limiter **fail-closed**, now CI-asserted.

## [1.2.0] — 2026-06-23

Security hardening, saturation observability, and template/DX fixes. No
breaking API changes — all new config keys have defaults / env fallbacks, so
existing config files load unchanged.

### Added
- **Saturation metrics**: `db_pool_active_connections` + `db_pool_size`
  (labeled by pool — the answer to "is the pool about to time out on
  acquire?") and `jobs_queue_depth` (the waiting queue — a *leading* indicator,
  unlike the lagging `jobs_dlq_depth`). New Prometheus alerts
  `DbPoolSaturationHigh` and `JobsQueueBacklog` with RUNBOOK entries
  (`#dbpool`, `#queuebacklog`) and the matching SLO rows.
- **Opt-in CSRF** double-submit guard (`security.csrf.*`, off by default):
  server middleware verifying a token header against a non-HttpOnly cookie on
  cookie-authenticated mutations, with the SPA client wired to echo it.
- **Stricter rate-limit tier** config for the auth surface
  (`rate_limit.protected_requests` / `protected_window_sec` /
  `protected_paths`).
- `make` front doors: `make new-resource`, `make new-job`, `make init`.
- Coverage gate: `make coverage` now fails under `COVERAGE_MIN`% line coverage
  (default 40, a regression floor — override per-run).
- `scripts/init-project.sh` gains an optional `[domain]` argument to rebrand
  the host alongside the project name.

### Changed
- `with_repo_errors` is decoupled from the User/Role domain: repository
  exceptions now derive from generic `Repositories::NotFoundError` /
  `ConflictError` bases (`repositories/RepoErrors.hpp`), so the shared
  controller plumbing no longer includes the demo repositories and a forked
  domain's own exceptions map to the right status automatically.
- `new-resource.sh` scaffolds the repository on `CrudBase` (the base whose
  whole purpose is `find` / `list` / `count`) instead of hand-rolling them.
- The scaffolders are now discoverable where forkers read — README "first
  steps", `docs/INDEX.md`, and the Make-targets table — and `new-job.sh` is
  executable.
- `init-project.sh` verifies the rename itself (an independent broad scan that
  exits non-zero listing any survivor) instead of printing a grep for you to
  run.

### Fixed
- **Single-source version**: `CMakeLists.txt project() VERSION` was left at
  `1.0.0` through the 1.1.0 release, so the binary mis-reported its version;
  it now tracks the release.
- Dead scaffolding instructions removed: the generated test + `new-resource.sh`
  trailer + `docs/CONVENTIONS.md` no longer point at a non-existent
  "INTEGRATION_FILTER / 5 places" bucket registration (buckets are classified
  by directory).
- `init-project.sh` no longer leaves the template name behind in the vcpkg
  manifest (`vcpkg.json` was excluded by an over-broad `./vcpkg*` filter),
  `.env.*` variants, and helm `NOTES.txt` — nor the author's `security@` /
  demo host in a fork.

### Security
- **The public auth/account surface is now rate-limited.** `api.public_paths`
  exempted login / register / refresh / password-reset from *both* auth and the
  limiter, leaving them open to credential brute-force and mail-bombing. A
  second, stricter per-IP tier (separate `rl:auth:` namespace) re-arms them; the
  production profile enables it by default (10 req/60 s). Health/metrics/static
  stay unthrottled. **Behavior change on upgrade:** a burst against the auth
  endpoints now returns 429 — tune via `RATE_LIMIT_PROTECTED_REQUESTS`.
- **Baseline HTTP security headers** on every response — `X-Content-Type-Options:
  nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, a
  locked-down CSP, and opt-in HSTS (`security.hsts`, on in the production
  profile) — set both in the app and at the nginx edge for the SPA.

## [1.1.0] — 2026-06-23

Pre-release hardening + a public demo. No breaking API changes.

### Added
- Public demo environment at `*.demo.tarassov.me` — `helm/tarassov-me-env/values-demo.yaml`
  + `scripts/deploy-demo.sh` (external-dns + cert-manager, Mailpit/Jaeger UIs),
  with a periodic reset CronJob that wipes and reseeds the data.
- `THIRD_PARTY_NOTICES.md` (dependency licenses + flask-base attribution),
  `docs/TESTING.md` (what the suite covers and doesn't), `docs/BENCHMARKS.md`
  (how to measure latency/footprint), `CODE_OF_CONDUCT.md`, and GitHub issue
  templates.
- Working dark-mode toggle and a real favicon/brand in the SPA.

### Changed / Fixed
- **Security:** constant-time bearer-token compare; the production auth-guard is
  now actually armed (`APP_ENV` wired through config + Helm); the tarassov-me chart
  defaults to `auth.mode=jwt` so a bare install can't ship a public API.
- **Fork experience:** `make quickstart` / `up` build the fork's own code instead
  of pulling the upstream image; neutral registry default; CODEOWNERS and
  Prometheus runbook links no longer hardcode the author's identity;
  `init-project.sh` rebrands the GHCR namespace.
- **Ops:** the backup CronJob verifies the dump (`gunzip -t` + size guard) instead
  of silently shipping a truncated one, with failure/staleness alerts;
  `seccompProfile` on all pods; single-replica PodDisruptionBudgets no longer
  wedge node drains; `make coverage` runs every test bucket.
- **CI:** Trivy fails on HIGH as well as CRITICAL; all GitHub Actions pinned to
  SHAs; the tag-release image job promotes the natively-built per-arch images
  into the `:vX.Y.Z` manifest instead of a QEMU rebuild (which segfaulted
  emulating amd64).
- **Docs:** corrected the README inventory, a non-existent module in an ADR, a
  migration-skeleton trap, dead clone/CI links, and a CHANGELOG contradiction.

## [1.0.0] — 2026-06-10

First tagged release. Highlights of the pre-release hardening pass:

- HTTP end-to-end test layer (real Drogon server + client) covering the
  middleware chain: auth gate, cookie sessions on the wire, refresh
  rotation/revocation, idempotency replay, permission bitmask, tracing.
- Frontend session refresh (401 → /api/auth/refresh → retry) + first
  frontend unit tests; committed package-lock.json for reproducible builds.
- Single-source version (CMake → version.hpp), production config profile
  with `make prod-check`, Prometheus alert rules, Renovate, prebuilt
  builder-image cache (`make warm-cache`).
- Test-bucket filters by explicit suite names; deflaked tamper/expiry
  tests; precompiled headers.

### Added
- Account / Auth / Admin feature surface modeled after flask-base
  (see `docs/PATTERNS-FROM-FLASK-BASE.md` for the file-level mapping):
  - `migrations/001_users_and_roles.sql` — users + roles + permission
    bitmask. Bits match flask-base's `Permission` class
    (`GENERAL=0x01`, `ADMINISTER=0xff`).
  - `src/domain/{User,Role}.hpp` + `src/repositories/*` — typed DTO +
    SQL-only repositories with `DuplicateEmail` / `UserNotFound`
    typed exceptions.
  - `src/security/Password.hpp` — argon2id password hashing via
    libsodium (replaces flask-base's PBKDF2).
  - `src/security/Tokens.hpp` — HMAC-signed timed link tokens with
    per-purpose key derivation (replaces flask-base's
    `URLSafeTimedSerializer`).
  - `src/security/Auth.hpp` — JWT in HttpOnly cookies (`__Host-access`
    + `__Host-refresh`), refresh-token JTI revocation in Redis,
    `current_user_can(req, perm)` / `require_admin(req)` helpers.
  - `src/api/AuthController.hpp` — `/api/auth/{register,login,logout,refresh,me}`.
  - `src/api/AccountController.hpp` —
    `/api/account/{confirm-resend,confirm/{token},reset-password-request,
    reset-password/{token},change-email-request,change-email/{token},
    change-password}`.
  - `src/api/AdminController.hpp` — `/api/admin/{users,users/{id},
    invite,roles}` with self-protection guards.
  - `src/email/Mailer.hpp` — SMTP outbound via libcurl.
  - `src/email/Templates.hpp` + `templates/email/*.{html,txt}` — inja
    rendering for confirm / reset_password / change_email / invite.
  - `src/email/AccountEmails.hpp` — token-issuing senders shared by
    Auth + Account + Admin controllers.
  - CLI ops: `--setup-dev`, `--create-admin EMAIL [PASS]`,
    `--seed-fake [N]`. flask-base parity: `manage.py setup_general /
    add_fake_data`.
  - Mailpit dev sidecar (`docker-compose.yml`) on :8025 (UI) / :1025
    (SMTP). No TLS, no auth — local-only catcher.
- React SPA under `frontend/`:
  - Vite + React 18 + TypeScript + Tailwind + shadcn/ui (Radix
    primitives) + TanStack Query + react-hook-form + zod + Zustand.
  - openapi-fetch typed client driven by `npm run gen:api` against
    `docs/openapi.yaml`. Stub committed so a fresh clone compiles.
  - Pages: Home, About, Login, Register, CheckEmail, ConfirmEmail,
    Unconfirmed, Profile, ChangePassword, ChangeEmail, RequestReset,
    ResetPassword, Admin (Dashboard, Users, UserDetail, InviteUser).
  - `<ProtectedRoute>` with `requireConfirmed` and
    `requirePermission={Permission.Administer}` guards; `<Layout>`
    runs `useMe()` once so every page has a fresh principal.
  - `frontend/Dockerfile` (multi-stage Node→nginx) +
    `frontend/nginx.conf` proxy `/api/* -> app:8080`. New
    docker-compose service exposed on host :3001.
- Make targets: `frontend-{install,dev,build,lint,format,typecheck,
  test,gen-api,up,image}`.
- CI: `frontend` job (typecheck + lint + production build) +
  `openapi-drift`, `clang-tidy`, `sanitizers` jobs (paritet with
  GitLab CI).
- ADR-0005 documenting the SPA split.
- `docs/PATTERNS-FROM-FLASK-BASE.md` — authoritative list of what we
  lift from flask-base, what we change, and what we don't lift.
- `--run-migrations` CLI flag on the app binary (mirrors
  `RUN_MIGRATIONS_ONLY=1`; convenient for native dev / `make migrate-local`).
- New Make targets: `test-watch`, `migrate-local`, `ci-local`, `helm-lint`,
  `new-endpoint`, `new-migration`, `tail-trace TID=…`, `env-check`.
- `scripts/env-check.sh` — reports `${VAR}` placeholders in
  `config/config.json` that are unset and have no default (multi-token
  aware, handles `database.primary` correctly).
- `scripts/new-endpoint.sh --with-test` and `--patch-openapi` flags —
  scaffold a `tests/api/test_<name>.cpp` smoke test and patch
  `docs/openapi.yaml` in place instead of just printing a stub.
- `scripts/init-project.sh` idempotency guard — refuses to re-run with a
  different name unless `--force` is passed.
- `.editorconfig` — whitespace/EOL rules for editors that don't read
  `.clang-format`.
- `envrc.sample` — direnv template covering `VCPKG_ROOT`,
  `VCPKG_BINARY_SOURCES`, `TEST_PG_HOST`, `DATABASE_PASSWORD`.
- `docs/INDEX.md` — one-line navigator across docs / ADRs / scripts /
  configs.
- CMake: CTest labels (`unit` / `integration`) so `ctest -L unit` works
  without re-deriving the gtest filter; `ENABLE_WERROR` option for CI.
- GitHub Actions parity with GitLab: `openapi-drift`, `clang-tidy`, and
  `sanitizers` jobs.
- Repository pattern scaffolding in `docs/EXAMPLES.md`: typed DTOs with
  `nlohmann::to_json`, repositories that own all SQL and raise typed
  `DuplicateKey` / `UserNotFound` exceptions, thin controllers that
  translate those to HTTP status codes.
- `LOG_FORMAT=json` (default `text`) — emits one JSON record per line with
  JSON-escaped message via a custom spdlog flag; includes `service`,
  `thread`, `level`, ISO8601 timestamp for Loki/ELK/Datadog pipelines.
- CLI ops flags on the app binary:
  - `--print-routes` prints the endpoint table and exits (no subsystems).
  - `--dump-config` resolves config (JSON + env) and prints it as JSON.
  - `--verify-migrations` reports pending migrations, exits 1 if any pending.
- `MigrationRunner::list_pending()` — read-only migration diff helper.
- `Jobs::init_blocking_client()` for in-process tests so BRPOP has a socket
  timeout budget that matches its block timeout.
- `docker-compose.yml` `test-redis` service in the `test` profile — isolates
  the test suite from the dev-stack worker that would otherwise BRPOP
  test-submitted jobs off the shared queue.
- `DATABASE_REPLICA_URLS` passthrough in the worker service so the worker
  can route reads to the replica under `up-everything`.
- `.github/workflows/ci.yml` (full build + test + clang-format + shellcheck)
  and `.github/workflows/release.yml` (multi-arch GHCR push on `v*` tags).
- `migrations/README.md` starter doc — naming, `--verify-migrations`, ops.
- Env-var interpolation (`${VAR}` / `${VAR:-default}`) in config JSON values.
- `Config::require<T>()` — throws on missing required config.
- JWT (HS256) auth middleware with exp/nbf/iss/aud validation + RBAC helpers.
- Static bearer-token mode for dev convenience (`auth.mode=bearer`).
- Redis-backed fixed-window rate limiter with per-IP / per-user scope.
- Idempotency-Key middleware for POST/PUT/PATCH/DELETE.
- Dead-letter queue for jobs (`jobs:dlq:*`) with GET `/api/jobs/dlq` and
  POST `/api/jobs/dlq/{id}/requeue`.
- Generic retry-with-backoff utility (`Retry::run`) with pqxx / redis-plus-plus
  transient-error classifiers; applied to `Database::execute_read/write`.
- W3C Trace Context parsing and `X-Request-Id` / `traceparent` response
  headers; per-request trace-id attached to request attributes.
- Validation helpers (`Api::Validation::Errors`, `require`, `string_length`,
  `regex_match`, `int_range`, `one_of`, `email`, `uuid`).
- Graceful shutdown: readiness flips to 503 on SIGTERM; Drogon drain after
  configurable pre-stop delay; worker bounded drain.
- CMake option `ENABLE_SANITIZERS` (ASan + UBSan).
- `.clang-tidy` baseline + CI lint job.
- Sanitizer CI job (unit subset).
- Helm: `preStop` hook, `terminationGracePeriodSeconds`, external-secrets
  ExternalSecret skeleton, PrometheusRule with baseline SLO alerts.
- OpenAPI 3.1 spec under `docs/openapi.yaml`.
- Governance: `CODEOWNERS`, `SECURITY.md`, `CONTRIBUTING.md`, MR/PR templates.

### Changed
- `scripts/check-openapi-drift.sh` now compares `(method, path)` tuples,
  not just paths — catches verb-only changes (`GET → POST` on the same
  route) the previous diff missed.
- CMake test glob uses `CONFIGURE_DEPENDS` so a freshly-added
  `tests/<bucket>/test_*.cpp` is picked up on the next build without a
  manual `cmake --preset dev` reconfigure.
- Added stricter compile warnings by default: `-Wshadow`,
  `-Wnon-virtual-dtor`, `-Wold-style-cast`, `-Wcast-align`,
  `-Woverloaded-virtual`, `-Wnull-dereference`, `-Wdouble-promotion`,
  `-Wformat=2`. Debug build also gets `-fno-omit-frame-pointer`.
- `.devcontainer/devcontainer.json` `postCreateCommand` now calls
  `make compile-commands` instead of duplicating the cmake invocation.
- `Jobs::fail()` after `max_retries` now transitions jobs to status `dead`
  and pushes to the DLQ instead of a terminal `failed` state.
- `config/*.json` no longer contain plaintext passwords — use `${VAR}`
  placeholders.

### Removed
- Demo `users` / `posts` / `events` controllers, their repositories, domain
  types, tests, and migrations. The real auth / account / admin / audit domain
  stays; only the throwaway CRUD demo was removed, and the worked CRUD pattern
  moved to `docs/EXAMPLES.md` so `src/` stays free of example noise when you fork.
- `ensure_test_seed()` helper (no longer needed without the demo schema).

### Security
- Default `auth.mode` is `none` for local development, but the service
  refuses to start in `jwt` mode without a `JWT_SECRET` set.
- OpenSSL linked explicitly for HMAC-SHA256 (JWT signature) and SHA-256
  (Idempotency-Key body hash); constant-time compare via `CRYPTO_memcmp`.

[Unreleased]: https://github.com/moveeeax/tarassov.me/compare/v1.5.5...main
[1.5.5]: https://github.com/moveeeax/tarassov.me/compare/v1.5.4...v1.5.5
[1.5.4]: https://github.com/moveeeax/tarassov.me/compare/v1.5.3...v1.5.4
[1.5.3]: https://github.com/moveeeax/tarassov.me/compare/v1.5.2...v1.5.3
[1.5.2]: https://github.com/moveeeax/tarassov.me/compare/v1.5.1...v1.5.2
[1.5.1]: https://github.com/moveeeax/tarassov.me/releases/tag/v1.5.1
[1.2.0]: https://github.com/moveeeax/tarassov.me/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/moveeeax/tarassov.me/compare/v1.0.0...v1.1.0
