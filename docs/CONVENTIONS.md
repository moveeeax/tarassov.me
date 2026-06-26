# Conventions — how to add a domain entity

This is the canonical, copy-this checklist for adding a new resource end to
end. It points at the real files that already implement `users` / `roles`, so
you extend an existing pattern instead of guessing which controller to mimic.

> Fast path: `./scripts/new-resource.sh Product` scaffolds the backend stubs
> (domain + repository + CRUD controller + endpoint registry + openapi block +
> test) following every rule below. Then fill in the fields and the frontend.

The friction this guards against: one field name lives in ~6 places (SQL,
`from_row`, `to_json`, the openapi schema, the generated TS types, the page),
and several of those drift **silently** — the build stays green. Follow the
order below and run the drift checkers.

---

## 1. Migration — `migrations/NNN_<slug>.sql`

`./scripts/new-migration.sh add_products` writes a numbered, idempotent
skeleton. Inside: `CREATE TABLE IF NOT EXISTS`, indexes, and the
`updated_at` trigger (copy the `touch_updated_at` block from
`migrations/001_users_and_roles.sql`). Applied at boot in numeric order by
`MigrationRunner` under an advisory lock (safe with multiple replicas).

## 2. Domain DTO — `src/domain/<Entity>.hpp`

Model after `src/domain/User.hpp` / `Role.hpp`. Three things, and **every
field appears in all three** — keep them in sync:

- the `struct` fields (use `std::optional<T>` for nullable columns);
- `static <Entity> from_row(const Row&)` — `row["col"].template as<T>()`,
  guard nullable columns with `if (!row["col"].is_null())`;
- `inline void to_json(nlohmann::json&, const <Entity>&)` (ADL — `json j = e;`
  just works).

**Never serialize secrets** in `to_json` (see `User::password_hash`). The
invariant is enforced by `tests/unit/test_domain_serialization.cpp` — add a
case there if your entity has a sensitive field.

## 3. Repository — `src/repositories/<Entity>Repository.hpp`

Extend `CrudBase` (model after `RoleRepository`; `UserRepository` is the
hand-written variant because it joins roles):

- `class FooRepository : public CrudBase<FooRepository, Domain::Foo, std::string>`
  + four `static constexpr` constants (`kTable` / `kColumns` / `kIdColumn` /
  `kOrderBy`). CrudBase then supplies `find(id)` / `list(limit, offset)` /
  `count()` — don't re-hand-roll them. Hand-write only the bespoke writes.
- Typed exceptions deriving from the generic bases in
  `repositories/RepoErrors.hpp` — `struct FooNotFound : NotFoundError` (→404),
  `struct DuplicateFoo : ConflictError` (→409). They carry their own code, so
  `with_repo_errors` maps them without knowing the concrete type.
- Every write wraps `Database::get().execute_write([&](auto& txn){…})`. The
  lambda MUST take `auto& txn` (it receives `detail::TracingTxn&`, not a raw
  `pqxx::work&`). Use `execute_read_primary` for read-after-write.
- Wrap UNIQUE/FK-tripping writes in `detail::translate_sql(...)`
  (`repositories/SqlErrors.hpp`) to turn a SQLSTATE into your typed exception —
  otherwise a constraint violation surfaces as a raw 500.
- **Per-user resources:** add `static constexpr const char* kOwnerColumn =
  "owner_id";` to unlock CrudBase's `find_owned/list_owned/count_owned`, and
  gate the controller with `API_REQUIRE_OWNER`. Scaffold it with
  `new-resource.sh <Entity> --owned`. Plain `find`/`list` on an owned table is
  an IDOR.
- The repository must NOT know about HTTP. It throws; the controller maps.

## 4. Controller — `src/api/<Entity>Controller.hpp`

Model after `AdminController`. Includes: `api/Guards.hpp`,
`api/HandlerSupport.hpp`, `api/RequestUtils.hpp`, `api/Validation.hpp`,
`utils/ErrorResponse.hpp` — **never** `api/Api.hpp` (include cycle).

Per handler:
1. First line: a guard from `Guards.hpp` —
   `API_REQUIRE_ADMIN(req, callback)` or `API_REQUIRE_PRINCIPAL(req, callback, p)`.
   Every mutating endpoint needs one (under `AUTH_MODE=none` it's a no-op, so
   it never gets in the way during dev — but it's there for prod).
2. Parse + validate: `Validation::parse_body(req, body, callback)` then
   `Validation::require/email/string_length`, accumulate into
   `Validation::Errors`, bail with `Validation::response_400(errs)`.
3. Repository call inside `with_repo_errors(callback, "op", [&]{ … })`
   (`HandlerSupport.hpp`) — it maps `Duplicate*`→409, `*NotFound`→404,
   anything else→500-with-log. Don't hand-roll the catch ladder.
4. List endpoints: `parse_page_params(req, default, max)` →
   `{data, total, limit, offset}` (see `AdminController::listUsers`).
5. Success: `Response::ok(...)` / `Response::created(...)` — never build the
   `HttpResponse` by hand.
6. **`callback(...)` exactly once on every path**, including early returns and
   the guard expansions.

## 5. Route registry + OpenAPI

- Add the route to `Api::get_endpoints()` in **`src/api/Endpoints.hpp`** (NOT
  `Api.hpp`) for every `ADD_METHOD_TO`.
- Add the `#include "api/<Entity>Controller.hpp"` to `src/api/Api.hpp`.
- Add the path block + `components/schemas/<Entity>` to `docs/openapi.yaml`
  (hand-edited). Run `./scripts/check-openapi-drift.sh` — it verifies
  `(method, path)` parity (it does **not** check schema bodies, so review
  those by eye).

## 6. Frontend — `frontend/`

- Types come from `docs/openapi.yaml` via `npm run gen:api`
  (openapi-typescript → `src/lib/api/schema.gen.ts`); import the flat aliases
  from `@/lib/api/types`.
- Hook: `src/hooks/useAdmin<Entity>.ts` (model `useAdminRoles`); query keys
  from `src/lib/api/queryKeys.ts` (never inline string tuples).
- Page: `src/pages/admin/<Entity>.tsx` — `usePagedQuery` + `PaginationFooter`
  + `<DataTable>` for the list, `useApiMutation` for create/update/delete.
- Route: add under `<RequireAdmin>` in `src/App.tsx`.

## 7. Tests

- Integration suite `tests/integration/test_<entity>.cpp` extending
  `TestHelpers::CoreBackedTest` (`config_overrides`, `requires_postgres`,
  `post_init`); use `TestHelpers::make_request/authed/truncate_users`.
- Buckets are classified by DIRECTORY, not a filter list: a file in
  `tests/integration/` (or `tests/api/`) is compiled into the integration
  binary by CMake's `CONFIGURE_DEPENDS` glob with no registration step.
  `./scripts/check-test-buckets.sh` (run in CI) just guards placement — it
  fails on a suite-name clash across buckets, so a DB-dependent suite can't
  silently shadow a unit suite.

## What NOT to reach for

These were considered and rejected as overengineering for a starter template:
generic `Repository<T>` (the `execute_*` lambdas already are the base layer),
a service layer everywhere (`AccountEmails` is the one place reuse justified
it), an `IDatabase` interface for mocking (repositories are tested
integration-style against real Postgres), and a schema-driven form generator.
Keep handlers readable and the flow visible in one file.

## Gotchas — hard-won rules

These don't fall out of reading the code; they were learned the hard way.

1. **All modules are header-only.** Never add a `.cpp` under `src/`. CMake compiles `src/main.cpp` / `worker_main.cpp` explicitly; `file(GLOB tests/...)` only picks up tests — a new `src/foo.cpp` silently never links. The only `.cpp` are `main.cpp` and `worker_main.cpp`.
2. **`Api::get_endpoints()` (`src/api/Endpoints.hpp`) is the single source of truth for routes.** After an `ADD_METHOD_TO(...)`, add the matching line there, or `scripts/check-openapi-drift.sh` fails CI and `--print-routes` won't show it. Controllers include `api/Guards.hpp` + `api/RequestUtils.hpp` but NOT `api/Api.hpp` (cycle).
3. **JSON: nlohmann::json only, never jsoncpp.** Drogon uses jsoncpp internally, but project code is all nlohmann. Parse with `json::parse(req->body())`, respond with `data.dump()` + `CT_APPLICATION_JSON`. **Never call `req->getJsonObject()`.**
4. **`AUTH_MODE=none` by default** — every endpoint public. On any mutating endpoint add a guard from `api/Guards.hpp` (`API_REQUIRE_ADMIN` / `API_REQUIRE_PRINCIPAL`) or `Security::Auth::require_role(...)`, or document why it's public.
5. **`Core::initialize()` has a strict init order:** Config → Observability → Database → Migrations → Cache → Messaging → Tasks → Security → Jobs → Mailer; shutdown is the reverse. Don't reorder (Cache uses Observability metrics; Database depends on Config).
6. **`req->attributes()->get<T>(key)` on a miss returns a default-constructed `T`** (older Drogon threw `std::out_of_range` — both exist). Never rely on the throw: `find(key)` first, then `get<T>` — see `Security::Auth::principal_of`. An empty principal reaching SQL as a uuid has crashed a handler before.
7. **Every Redis call is fail-open.** `CacheManager` methods swallow `sw::redis::Error` and log a warn; wrap direct `get_client()` calls in try/catch. See `docs/adr/`.
8. **Drogon's log level is hardcoded in `main.cpp` (`Logger::kInfo`).** `LOG_LEVEL` only affects spdlog. For Drogon debug, edit `setLogLevel` in `main.cpp`.
9. **`callback(...)` exactly once on every path** — including exceptions and early returns — or the client hangs until timeout.
10. **Tests don't call controller methods directly with a fake `req`** — use `tests/test_helpers.hpp` (`make_request(...)`).
11. **`docs/openapi.yaml` is edited by hand.** The drift checker compares `(method, path)`; update the spec on any route change.
12. **`make migrate-reset` / `make down-v` / `make dist-clean` are destructive** (drop data) — don't run them casually.
13. **Validate Helm before pushing chart changes: `make helm-validate`.** It renders the umbrella with `values-ci.yaml` and asserts deploy invariants (ingress port == service port, `baseDomain` expanded, `MAIL_VIA_JOBS` injected). Gotcha: `MAIL_VIA_JOBS` only renders on the api when `cpp-api.mail.enabled: true` (`deployment.yaml` gates `{{- if .Values.mail.enabled }}`). Vendored `helm/*/charts/*.tgz` are gitignored — run `helm dependency build` (which `helm-validate` does) before rendering/deploying.
14. **PCH `REUSE_FROM` is a single fragile build point.** integration/e2e/worker tests `REUSE_FROM` the unit PCH; flag drift (esp. sanitizer defines) silently breaks reuse or forces a full rebuild — which is why the ASan build lives in a separate `build-san` tree. Changing the `HEAVY_PCH` set or flags → rebuild all test buckets AND the sanitizer build.

### Frontend gotchas

- **`schema.gen.ts`** is a committed stub so `git clone` builds; `npm run gen:api` (or `make frontend-gen-api`) regenerates it from `docs/openapi.yaml`. CI always regenerates before `tsc`.
- **HttpOnly cookies — not readable from JS.** Sessions live in `__Host-access` + `__Host-refresh`; the SPA relies on `useMe()` (TanStack Query) to check auth.
- **Same-origin only.** Dev: Vite proxy `/api → :8080`. Prod: nginx `proxy_pass /api → app:8080`. No CORS config needed.
- **Permission bitmask mirror.** `frontend/src/lib/auth/permissions.ts` duplicates `Domain::Permission::k*` — change both together (a drift test enforces it).
- **No auto-login after register** — the flow goes to `/account/check-email`; the user clicks the email link, then logs in.
- **Self-protection in the admin UI.** The backend returns 400 on self-delete / self-role-change; the UI disables those controls when `me.id === target.id`.

### Where to look (question → file)

The scaffolding scripts are the entry points:

| Want to… | Look at / run |
|---|---|
| Add a backend endpoint | `./scripts/new-endpoint.sh` |
| Add a migration | `./scripts/new-migration.sh` |
| Add a frontend page | `./scripts/new-react-page.sh` |
| Bring up the whole stack | `make up && make frontend-up` |
| Create an admin | `<bin> --create-admin EMAIL [PASS]` |
| Debug routes / health / traces | `make routes` / `make health` / `make tail-trace TID=<id>` |
| Regenerate frontend types | `make frontend-gen-api` |
| Run "like CI" locally | `make ci-local` |
| Check spec ↔ code drift | `./scripts/check-openapi-drift.sh` |
| Validate Helm render | `make helm-validate` |
| Worked CRUD example | `docs/EXAMPLES.md` |
| ADRs / architecture decisions | `docs/adr/` |
