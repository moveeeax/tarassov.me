# C++ REST Service Template

Enterprise baseline for a C++20 REST service. Fork it, point it at your Postgres/Redis,
and start writing endpoints instead of reinventing auth, rate limiting, tracing, and DLQs.

<!-- Live badges point at the canonical repo. init-project.sh rebrands both the
     project name AND the host (pass your domain as the 3rd arg) and then fails
     if any template/author token survived — so a fork won't ship these links. -->
[![pipeline](https://gitlab.com/tarassov.me/tarassov-me/badges/master/pipeline.svg)](https://gitlab.com/tarassov.me/tarassov-me/-/pipelines)
[![release](https://gitlab.com/tarassov.me/tarassov-me/-/badges/release.svg)](https://gitlab.com/tarassov.me/tarassov-me/-/releases)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Drogon](https://img.shields.io/badge/Drogon-HTTP%20Framework-green.svg)
![PostgreSQL](https://img.shields.io/badge/PostgreSQL-15-336791.svg)
![Redis](https://img.shields.io/badge/Redis-7-DC382D.svg)
![Docker](https://img.shields.io/badge/Docker-Compose-2496ED.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

**Status:** stable template (v1.x) — the structure and APIs are settled and meant
to be forked as a base for real services; breaking changes follow SemVer. Issues
and PRs welcome (see [CONTRIBUTING.md](CONTRIBUTING.md)).

## Contents

- [Why this template](#why-this-template)
- [What's in the box](#whats-in-the-box)
- [Quick start](#quick-start)
- [Authentication](#authentication)
- [Adding an endpoint](#adding-an-endpoint)
- [Ops CLI](#ops-cli) · [Configuration](#configuration) · [Observability](#observability-quickstart)
- [Kubernetes](#kubernetes)
- [Repo layout](#repo-layout) · [Dev workflow](#dev-workflow)
- [Contributing](#contributing) · [Security](#security) · [License](#license)

## Why this template

C++ gives you the latency budget; this template gives you everything around
the handler so you don't spend the first month rebuilding it:

| You could start from… | What you'd still have to build |
|---|---|
| Bare **Drogon / Crow / oatpp** | Auth, sessions, rate limiting, idempotency, migrations, DLQ, tracing, CI, charts — the framework ends where this template begins |
| **userver** | A batteries-included monolith framework with its own idioms; great inside Yandex-scale infra, heavyweight to adopt piecemeal |
| **A Go/Python service** | The 10x latency/footprint you came to C++ to avoid |

**Good fit:** an HTTP/JSON service that needs C++ performance plus the
boring-but-mandatory production middleware, deployed via Docker/K8s.
**Poor fit:** gRPC-first systems, GUI apps, embedded targets without Docker —
the value here is the integration glue, and that glue assumes containers.

Performance is the reason to be here, and it's hardware-specific — measure it on
yours with `make bench`; [docs/BENCHMARKS.md](docs/BENCHMARKS.md) has the
methodology and a results template.


## What's in the box

**HTTP layer**
- Drogon async HTTP server, multi-threaded event loops.
- Unified JSON error body (`{error, status, message, ...extras}`) across every
  controller and middleware — no five-shape drift.
- Composable request validators (`Api::Validation::*`) that accumulate errors
  and return a single 400 with a structured `errors` array.
- One-line JSON responses (`Response::ok` / `Response::created`) and
  `Validation::parse_body` — no hand-rolled HttpResponse boilerplate
  per endpoint. Cache-aside reads go through `Cache::get()` and are
  fail-open by convention (a Redis hiccup never blocks a request).

**Security**
- JWT (HS256) auth with `exp`/`nbf`/`iss`/`aud` validation and RBAC
  (`Security::Auth::require_role`). Static Bearer mode for dev; `none` by default.
- Redis-backed **sliding-window** rate limiter (atomic Lua, immune to the
  2x fixed-window boundary burst) with per-IP / per-user scope,
  `X-RateLimit-*` headers, fail-open / fail-closed choice.
- Idempotency-Key middleware: body-hash keyed replay cache with 422 on
  same-key-different-body conflicts.
- Secrets never live in committed JSON — values are `${VAR}` placeholders
  interpolated from env at load time.

**Reliability**
- Retry-with-backoff wrapper (`Retry::run`) transparently applied to
  `Database::execute_read/write` with pqxx / redis transient-error classifiers.
- Dead-letter queue for jobs (`jobs:dlq:<type>`), with `/api/jobs/dlq` and
  `/api/jobs/dlq/{id}/requeue` endpoints, and a `jobs_dlq_depth` Prometheus gauge.
- Graceful shutdown: SIGTERM flips `/ready` to 503, then Drogon drains after a
  configurable pre-stop delay. Worker version finishes the in-flight job before exiting.

**Observability**
- Prometheus metrics with cardinality-safe path normalisation.
- OpenTelemetry traces (OTLP HTTP exporter).
- W3C Trace Context: incoming `traceparent` parsed (or generated) and echoed back
  as `X-Request-Id` + `traceparent` on every response.
- Structured logs via spdlog with the trace-id in each access log line.

**Testing**
- 305 unit/integration tests against real Postgres + Redis, bucketed by
  explicit suite filters (`make test-unit` runs sidecar-free).
- **HTTP end-to-end suite**: a real Drogon server + client exercising the
  whole middleware chain on the wire — auth gate, cookie sessions, refresh
  rotation/revocation, Idempotency-Key replay, permission bitmask, tracing
  headers. `make test-e2e`.
- Frontend unit tests (vitest) for the session-refresh machinery and the
  permission mirror; `package-lock.json` committed for reproducible builds.

**Ops**
- Helm charts for the API, worker, and frontend, plus a `tarassov-me-env` umbrella that
  deploys all three (with in-cluster Postgres/Redis/etc.) as one environment.
  `preStop` hook +
  `terminationGracePeriodSeconds`, `ServiceMonitor`, opt-in `PrometheusRule`
  with baseline SLO alerts, opt-in `ExternalSecret` skeleton for Vault /
  AWS / GCP secret stores.
- GitLab CI: **arm64** Docker build (the GitLab runner is arm64), unit +
  integration tests, Trivy image scan, clang-format / cppcheck / clang-tidy
  lints, ASan + UBSan sanitizer build. The GitHub `release.yml` job publishes
  the **multi-arch** (amd64 + arm64) image on `v*` tags — see the arch note
  under [Kubernetes](#kubernetes) before deploying to an amd64 cluster.
- `CODEOWNERS`, `SECURITY.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, PR templates.
- Production config profile (`config/config.production.json`) gated by
  `make prod-check` — auth on, cookies secure, limiter fail-closed, docs UI
  off, non-weak secrets — before anything ships.
- Prometheus **alert rules** out of the box (5xx rate, p99 latency, DLQ
  backlog, scrape-down, retry exhaustion).
- Renovate config: docker tags, GitHub-Action SHAs, npm lockfile and the
  vcpkg baseline stay current instead of fossilizing.
- `make warm-cache` pulls the CI-built dependency image — first build in
  minutes instead of the ~30-minute cold vcpkg compile.

**Account / Admin (flask-base parity)**
- Register / login / logout / refresh / me — JWT in HttpOnly cookies,
  refresh-token rotation with Redis-backed revocation.
- Email confirmation, password reset, email change, password change
  via signed timed link tokens (libsodium-derived per-purpose keys).
- Admin user management (list / invite / detail / update / delete /
  roles) with self-protection (no self-delete, no self-role-change).
- **Audit trail** (`audit_log`): every admin mutation on users and roles is
  recorded; read it via `GET /api/admin/audit` (paginated + filterable by
  action/actor/target/date), gated on a dedicated `kAuditRead` permission bit.
- Email pipeline: SMTP via libcurl + inja templates; Mailpit dev
  sidecar at http://localhost:8025. Account emails are delivered through
  the jobs queue (`account_email` type, handled by
  `src/email/AccountEmailWorker.hpp`) when `mail.via_jobs` is on (the
  default) — so confirm/reset links get DLQ + retries — with an inline
  send fallback when Jobs is off or the enqueue fails.
- Argon2id password hashing (libsodium).

**Frontend (separate React SPA)**
- Vite + React 18 + TypeScript + Tailwind + shadcn/ui primitives.
- TanStack Query + react-hook-form + zod + a hand-written fetch client
  with openapi-typescript-generated types (codegen from `docs/openapi.yaml`).
- All flask-base pages ported: Login / Register / Confirm / Unconfirmed
  / Profile / Change Password / Change Email / Reset Password / Admin
  Dashboard / Users / User detail / Invite / Roles (CRUD), plus admin
  Jobs (paginated list + DLQ requeue) and Audit-log browsers beyond flask-base.
- HttpOnly-cookie auth, no JWT in localStorage. Same-origin via Vite
  proxy in dev and nginx in prod — no CORS gymnastics.
- Lives in `frontend/` (monorepo); builds to its own image, deploys
  independently behind nginx (`frontend/Dockerfile`).

**Docs**
- [`docs/openapi.yaml`](docs/openapi.yaml) — OpenAPI 3.1 for every registered route.
- [`docs/CONFIG.md`](docs/CONFIG.md) — one table with every env var, JSON key,
  and default.
- [`docs/PATTERNS-FROM-FLASK-BASE.md`](docs/PATTERNS-FROM-FLASK-BASE.md) —
  what we lift from flask-base and what we change.
- [`docs/adr/0005-spa-split.md`](docs/adr/0005-spa-split.md) — why the
  frontend lives in its own project.

<!-- TODO(owner): screenshots — drop PNGs into docs/screenshots/, then
     uncomment this whole block (incl. the heading) and add it to Contents.
     Suggested set: SPA login, admin/users, Grafana dashboard, Jaeger trace.

## Screenshots

<p align="center">
  <img src="docs/screenshots/admin-users.png" width="49%" alt="Admin users">
  <img src="docs/screenshots/grafana.png" width="49%" alt="Grafana dashboard">
</p>
-->

## Quick start

**Prerequisites:** Docker + Docker Compose v2. On macOS the build runs in a
Linux VM (Docker Desktop or Colima) — **give it ≥ 8 GiB of memory**. The first
build compiles the C++ dependency set from source via vcpkg; under-provisioned
VMs (the Colima default is 2 GiB) OOM the builder and surface it as a cryptic
`EOF` / `rpc error: Unavailable`, which reads like a code bug but isn't. With
Colima: `colima start --cpu 4 --memory 8`. Run `make doctor` to check the
toolchain and the VM's memory, and `make warm-cache` to pull a prebuilt
dependency layer (falls back to the upstream template cache) so the first build
is minutes, not ~30.

```bash
git clone https://gitlab.com/tarassov.me/tarassov-me.git my-service
cd my-service

make doctor        # verify Docker + VM memory before the first (cold) build
make warm-cache    # optional: prime the vcpkg dependency layer (~30 min -> ~3)

# Rename template identity (project name, image registry, helm charts, etc.)
./scripts/init-project.sh my-service docker.io/myorg

# Build + run Postgres + Redis + the API, wait for ready, hit /healthz
make quickstart

# Sanity check a few endpoints
make smoke

# Tail logs
make logs
```

The service listens on `:8080` (HTTP) and `:9090` (Prometheus `/metrics`).

### Day-one bootstrap (auth + frontend together)

**Lean default** — app + Postgres + Redis + Mailpit, **Docker only, nothing built locally**:

```bash
make up                  # minimal stack (AUTH_MODE=none — every route public, migrations auto-run)
make health              # /healthz, /ready, head of /metrics
make frontend-up         # optional: add the React SPA on :3001
```

**Or the whole experience in one command** — auth (JWT) + SPA + worker + monitoring, zero config (heavy):

```bash
make up-everything       # app, postgres, redis, mailpit, worker, replica, sentinel, kafka,
                         # frontend, monitoring. docker/.env.everything wires AUTH_MODE=jwt +
                         # cookies + mail automatically.

# create the first admin (migrations ran automatically on app startup)
docker compose -f docker/docker-compose.yml exec app \
    ./tarassov_me --create-admin admin@local password

# open http://localhost:3001  — SPA, log in as admin@local / password
# open http://localhost:8025  — Mailpit (confirm / reset links land here)
# open http://localhost:3000  — Grafana (admin / admin)
# open http://localhost:16686 — Jaeger
```

Need a middle ground? Start from `make up` and add only what you want — `make up-monitoring`,
`make up-worker`, `make up-kafka`, … see [More stack variants](#more-stack-variants).

To stop everything: `make down`. To wipe volumes too (DESTRUCTIVE, drops users): `make down-v`.

### First day — walkthrough for a new project

After `init-project.sh` you're on your own schema / business logic.

**Fastest path — a whole CRUD resource in one shot:**
`./scripts/new-resource.sh Product` (or `make new-resource ENTITY=Product`)
generates the migration, domain DTO, repository (on `CrudBase`), admin-gated
controller, route-registry row, OpenAPI block and an integration test —
following [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md). Then fill in your real
columns. The finer-grained steps below are for adding pieces individually.

Typical order:

1. **Add your first migration** — drop `migrations/001_<topic>.sql`. See
   [`migrations/README.md`](migrations/README.md) for naming and
   [`docs/EXAMPLES.md`](docs/EXAMPLES.md) for a worked `users` example.
2. **Generate a controller** — `./scripts/new-endpoint.sh FooController Get /api/foo`
   creates the stub, wires the include into `src/api/Api.hpp`, adds a row
   to `Api::get_endpoints()` in `src/api/Endpoints.hpp`, and prints an
   OpenAPI stub. Pass `--with-test` to
   also emit `tests/api/test_<name>.cpp`, and `--patch-openapi` to insert
   the path block into `docs/openapi.yaml` in place instead of just
   printing it. (Or use `make new-endpoint NAME=… METHOD=… PATH_=… WITH_TEST=1 PATCH_OPENAPI=1`.)
3. **Add a DTO + repository** for anything touching SQL — follow the
   pattern in `docs/EXAMPLES.md` (domain struct with `from_row` + `to_json`,
   a repository that owns every SQL string and raises typed exceptions).
4. **`make dev`** — rebuilds and restarts just the app container (no full
   stack restart, Postgres volume stays put).
5. **`make test-quick`** — the fast TDD loop; runs against the cached
   test-runner image in ~5 s instead of the ~2 min cold rebuild of `make test`.
6. **Verify migrations didn't drift** — `docker compose exec app ./tarassov_me --verify-migrations` exits
   non-zero if anything is pending.
7. **Background work** — `./scripts/new-job.sh reindex` (or
   `make new-job TYPE=reindex`) scaffolds a self-registering job handler under
   `src/jobs/handlers/`; it prints the one `#include` to add to
   `src/worker_main.cpp`. Submit work with `Jobs::submit("reindex", payload)`.

### Run the full tests

| Command | When to use |
|---|---|
| `make test` | Cold run — rebuilds the test image, full suite, ~2 min |
| `make test-quick` | Fast TDD loop — reuses cached image, ~5 s |
| `make test-unit` | Only unit tests; skips anything needing Postgres/Redis |
| `make test-e2e` | HTTP end-to-end binary: real Drogon server + client, middleware on the wire |

### More stack variants

| Command | Adds |
|---|---|
| `make up-replica` | Postgres read replica |
| `make up-sentinel` | Redis Sentinel (3 nodes) |
| `make up-kafka` | Kafka + Zookeeper |
| `make up-worker` | Background-job worker |
| `make up-monitoring` | Prometheus + Grafana + Jaeger |
| `make frontend-up` | React SPA (built + served by nginx on :3001) |
| `make up-everything` | All of the above + frontend (uses `docker/.env.everything`) |

All profiles share the same `make down` / `make down-v`.

## Authentication

Default is `none` — every endpoint is public. Flip one env var:

```bash
# Static bearer token (simplest)
docker compose run --rm --service-ports \
    -e AUTH_MODE=bearer -e AUTH_BEARER_TOKEN=dev-secret-123 app

curl -H 'Authorization: Bearer dev-secret-123' http://localhost:8080/api/jobs
```

Or JWT HS256:

```bash
docker compose run --rm --service-ports \
    -e AUTH_MODE=jwt \
    -e JWT_SECRET=change-me \
    -e JWT_ISSUER=my-issuer \
    -e JWT_AUDIENCE=my-api app

# Mint a test token from the host; no Python or Node needed
TOKEN=$(make jwt SECRET=change-me ROLES=admin)
curl -H "Authorization: Bearer $TOKEN" http://localhost:8080/api/jobs
```

Protected endpoints that require a specific role:

```cpp
void deleteAccount(const HttpRequestPtr& req, ...) {
    if (auto err = Security::Auth::require_role(req, "admin")) {
        callback(err);  // 403 {error: "forbidden", required_role: "admin"}
        return;
    }
    // ...
}
```

See [`docs/openapi.yaml`](docs/openapi.yaml) for which endpoints have `BearerAuth`
in their security block.

## Adding an endpoint

The template already ships a full auth / RBAC / admin / audit domain
(`AuthController`, `AccountController`, `AdminController`, `AuditController`)
next to the infrastructure controllers (`HealthController`, `JobsController`) —
you start from a working example, not a blank `src/api/`. For *your* resource, a
worked CRUD stack — typed DTO + repository + controller + migration + test —
lives in [`docs/EXAMPLES.md`](docs/EXAMPLES.md). Copy from there, don't try to
invent it from scratch.

1. Add the route to a controller (existing or new) under `src/api/`:
   ```cpp
   METHOD_LIST_BEGIN
   ADD_METHOD_TO(MyController::listThings, "/api/things", Get);
   METHOD_LIST_END
   ```
2. Register the new route in `Api::get_endpoints()` in `src/api/Endpoints.hpp`
   (the route registry, surfaced at `/`).
3. Update `docs/openapi.yaml` — manually, and CI holds you to it:
   `scripts/check-openapi-drift.sh` fails the pipeline on any
   (method, path) mismatch between spec and `Api::get_endpoints()`.
4. If the endpoint mutates, validate inputs through `Api::Validation::*` and
   return errors via `Api::Validation::response_400(errs)` for bulk errors or
   `ErrorResponse::{bad_request,not_found,...}()` for single-shot.
5. Add a test in `tests/api/` (controller methods via `TestHelpers::make_request`),
   `tests/integration/` (real Postgres/Redis), or `tests/e2e/` (the full
   Drogon server over real HTTP — middleware chain included).

## Ops CLI

Run the binary with any of these to bypass the server loop:

| Flag | Effect |
|---|---|
| `--print-routes` | Print the registered endpoint table and exit. No DB/Redis required. |
| `--dump-config` | Resolve config (JSON + env overrides) and print as JSON. No subsystems. |
| `--verify-migrations` | Connect to the DB and list migrations not yet applied. Exits 1 if any are pending — handy as a CI gate. |
| `--run-migrations` | Apply all pending migrations and exit. Same effect as `RUN_MIGRATIONS_ONLY=1` env, surfaced as a flag for native dev / `make migrate-local`. |
| `--help` / `-h` | Show usage. |

The positional arg (if present, before flags) is the config path, same as the
default boot mode.

## Configuration

See the full table in [`docs/CONFIG.md`](docs/CONFIG.md). Short version:

- Defaults live in `config/config.json` with `${VAR}` placeholders.
- Env vars override everything.
- Per-deployment overlay: drop a `config/local.json`, point `CONFIG_FILE` at it.
  `config/local.json` is git-ignored.

## Observability quickstart

With `make up-monitoring`:

- Metrics: http://localhost:9094 (Prometheus), http://localhost:3000 (Grafana, admin/admin)
- Traces: http://localhost:16686 (Jaeger UI)
- Every HTTP response carries `X-Request-Id` — use that to correlate logs/traces.

The app emits `OTLP_ENDPOINT`-tuned OTLP HTTP to whatever you configure; default
`http://jaeger:4318/v1/traces` in the `with-monitoring` profile.

## Kubernetes

Four charts:

- `helm/tarassov-me` — the HTTP service
- `helm/tarassov-me-worker` — the background-job worker
- `helm/tarassov-me-frontend` — the React SPA (rootless nginx)
- `helm/tarassov-me-env` — umbrella that deploys all three as one environment (plus
  in-cluster Postgres / Redis / Mailpit / Jaeger / Kafka); see `make helm-validate`

Render locally:

```bash
helm template api helm/tarassov-me --set image.repository=my-registry/tarassov-me
helm template worker helm/tarassov-me-worker --set image.repository=my-registry/tarassov-me
```

**First prod deploy — start from the example overlays.** Each chart ships a
tracked, secret-free `values-prod.example.yaml`. Copy it, fill in the TODOs
(hosts, image, datastore endpoints), and deploy:

```bash
cp helm/tarassov-me/values-prod.example.yaml helm/tarassov-me/values-prod.yaml   # gitignored
helm upgrade --install api ./helm/tarassov-me -n prod -f helm/tarassov-me/values-prod.yaml \
  --set externalDatabase.password="$DB_PASSWORD" \
  --set externalRedis.password="$REDIS_PASSWORD" \
  --set auth.jwtSecret="$JWT_SECRET"
# repeat for tarassov-me-worker (its datastore/auth MUST match) and tarassov-me-frontend
```

**Image architecture — match it to your nodes.** The GitLab pipeline builds and
pushes an **arm64-only** image (its runner is arm64); only the GitHub
`release.yml` tag job publishes a **multi-arch** (amd64 + arm64) manifest. If
your cluster is amd64 (the example overlays' `nodeSelector` assumes it) you must
deploy an amd64 image — pull from a multi-arch tag, or build for amd64 yourself:

```bash
docker buildx build --platform linux/amd64 --target runtime \
  -t your-registry/your-project:$(git rev-parse --short HEAD)-amd64 --push .
```

A mismatch shows up as `exec format error` / `CrashLoopBackOff` on first roll-out.

Per-cluster secrets and overrides go in untracked files — `helm/**/values-prod.yaml`,
`helm/values.*.yaml`, and `helm/*.local.yaml` are all gitignored. Either pass
real secrets via `--set` / a private values file, or wire `externalSecrets`
to Vault / AWS Secrets Manager / etc. **Never** put a real secret in a tracked
`*.example.yaml`.

Opt-ins you'll almost certainly want in prod:

- `autoscaling.enabled=true` (HPA)
- `pdb.enabled=true` (PodDisruptionBudget)
- `networkPolicy.enabled=true` — tune the selectors for YOUR cluster namespaces first
- `serviceMonitor.enabled=true` + `monitoring.alertsEnabled=true` (Prometheus-operator CRDs)
- `externalSecrets.enabled=true` — pull DB/Redis/JWT secrets from Vault or similar

The [SECURITY.md](SECURITY.md) hardening checklist walks through every one.

## Repo layout

```
src/
  api/           HTTP controllers + endpoint registry + middleware pipeline
    Api.hpp           controller includes + middleware wiring (register_controllers)
    Endpoints.hpp     get_endpoints() — the route registry (drift-checked vs openapi.yaml)
    Middleware.hpp    middleware bodies (auth → rate limit → idempotency → cors → trace)
    Guards.hpp        handler guard macros (API_REQUIRE_ADMIN / _PRINCIPAL / _JOBS_READY)
    RequestUtils.hpp  parse_int, clamp_int, parse_page_params, is_valid_uuid, normalize_path_for_metrics
    Validation.hpp    composable request-body validators
    *Controller.hpp   Built-in controllers: Auth, Account, Admin, Audit, Health, Jobs (full auth/admin domain). Add your own with scripts/new-endpoint.sh; docs/EXAMPLES.md walks through a full Users CRUD.
  cache/         Redis client (standalone or Sentinel HA)
  core/          Application lifecycle (init, health, shutdown)
  database/      Postgres pool + migrations
  jobs/          Redis-backed job queue + DLQ
  messaging/     Kafka producer/consumer
  observability/ Logger, Prometheus, OpenTelemetry tracer, W3C Trace Context
  security/      Auth (JWT/Bearer) + RateLimit + Idempotency
  tasks/         Drogon-timer-based task scheduler
  utils/         Config (JSON + env), Retry, Strings, ErrorResponse

tests/
  unit/          Pure C++ tests (no external services)
  integration/   Tests that need real Postgres / Redis
  api/           Controller-method tests via TestHelpers::make_request
  e2e/           Real Drogon server + HTTP client (separate binary)

docker/          Dockerfile + docker-compose.yml + env presets
helm/            Helm charts (tarassov-me, tarassov-me-worker, tarassov-me-frontend + tarassov-me-env umbrella), values documented
scripts/         make-jwt.sh, smoke.sh, init-project.sh, bench.sh,
                 new-resource.sh (full CRUD), new-endpoint.sh (single
                 controller, --with-test / --patch-openapi), new-job.sh
                 (job handler), new-react-page.sh, new-migration.sh,
                 check-openapi-drift.sh, lint-openapi.sh, env-check.sh
docs/            openapi.yaml, CONFIG.md, EXAMPLES.md, INDEX.md, adr/, Doxyfile
```

## Dev workflow

| Command | What it does |
|---|---|
| `make up` / `make up-dev` | Start base stack (and worker, with `up-dev`) |
| `make doctor` | Sanity-check the local toolchain (cmake, ninja, jq, vcpkg, …) |
| `make env-check` | Report `${VAR}` placeholders in `config/config.json` that are unset and have no default |
| `make prod-check` | Semantic production gate: auth on, secure cookies, fail-closed limiter, strong secrets |
| `make warm-cache` | Pull the CI-built builder image — skip the cold vcpkg compile |
| `make build-local` | Native cmake build via the `dev` preset, no Docker |
| `make compile-commands` | Generate `compile_commands.json` for clangd |
| `make test` / `make test-quick` | Full / cached suite in Docker |
| `make test-local NAME=Jobs*` | Native gtest run with a `--gtest_filter` |
| `make test-watch` | Re-run unit tests on src/ or tests/ change (watchexec or entr) |
| `make watch` | Rebuild + restart on `src/` change (entr or watchexec) |
| `make coverage` | gcovr HTML report in `coverage/index.html`; fails under `COVERAGE_MIN`% line coverage (default 40, a regression floor) |
| `make ci-local` | Reproduce CI locally: format check + drift + spectral + tidy + tests |
| `make helm-lint` | `helm lint` + smoke `helm template` over all four charts |
| `make routes` / `make health` | Print endpoint table / hit health probes |
| `make psql` / `make redis-cli` | Open a shell against the running stack |
| `make migrate` / `make migrate-local` / `make migrate-status` / `make migrate-reset` | Run (Docker or native) / inspect / nuke-and-reapply migrations |
| `make init NAME=… [REGISTRY=…]` | Rebrand the template for your fork via `scripts/init-project.sh` |
| `make new-resource ENTITY=Product` | Scaffold a full CRUD resource via `scripts/new-resource.sh` |
| `make new-endpoint NAME=… METHOD=… PATH_=… [WITH_TEST=1] [PATCH_OPENAPI=1]` | Scaffold a controller via `scripts/new-endpoint.sh` |
| `make new-job TYPE=… [HANDLER=…]` | Scaffold a background-job handler via `scripts/new-job.sh` |
| `make new-migration SLUG=…` | Generate the next `migrations/NNN_<slug>.sql` |
| `make seed` | Apply optional `migrations/seeds/*.sql` fixtures |
| `make logs` / `make logs-pretty` / `make logs-worker` | Tail logs (json-pretty via jq, worker variant) |
| `make tail-trace TID=<id>` | Filter app logs by a specific trace id |
| `make fmt` / `make lint` / `make tidy` / `make lint-openapi` | clang-format / format-check / clang-tidy / spectral |
| `make jwt SECRET=... ROLES=admin` | Print a dev JWT to stdout |
| `make dev-token SECRET=... ROLES=admin` | Mint a JWT into `.dev-token` (gitignored, picked up by `make smoke`) |
| `make smoke` | curl through critical endpoints (uses `.dev-token` if present) |
| `make clean` / `make dist-clean` | Wipe build/, logs, Doxygen output (+vcpkg_installed for dist-clean) |
| `make down` / `make down-v` | Stop everything (and wipe volumes with `down-v`) |

`make help` prints this table at any time.

### Faster local builds

**Docker path (recommended):** `make warm-cache` pulls the dependency image
your CI already built, so `make build` / `make test` reuse those layers and
skip the ~30-minute cold vcpkg compile entirely.

**Native path — vcpkg binary cache:**

A cold local build pulls and compiles every dependency once. Wire up vcpkg's
binary cache so subsequent builds (and CI clones) reuse the artefacts:

```bash
mkdir -p ~/.vcpkg-bincache
export VCPKG_BINARY_SOURCES='clear;files,~/.vcpkg-bincache,readwrite'
make build-local        # first run populates the cache
make dist-clean && make build-local   # second run is near-instant
```

Persist this in your `~/.zshrc` / `~/.bashrc`. The dev container already
mounts a vcpkg buildtrees + downloads volume, so no extra setup there.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). TL;DR: conventional commits, clang-format
clean, tests passing, don't break the error-response shape without updating
`docs/openapi.yaml`.

## Security

See [SECURITY.md](SECURITY.md) — private disclosure via `security@tarassov.me`,
plus a production-hardening checklist.

## License

MIT. See [LICENSE](LICENSE). Third-party dependencies and their licenses (plus
the flask-base attribution for the ported account/admin surface) are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
