# ADR 0006 — API versioning: URL-path `/api/v1` with additive-only evolution

Status: Accepted — 2026-06-26

## Context

The API prefix `/api` was hardcoded across ~33 backend route literals
(`src/api/*Controller.hpp` `ADD_METHOD_TO` + `src/api/Endpoints.hpp`
`get_endpoints()`), the OpenAPI spec (`docs/openapi.yaml`), the bidirectional
drift guards (`scripts/check-openapi-drift.sh`, `scripts/check-routes-registered.sh`),
the scaffolders (`scripts/new-endpoint.sh`, `scripts/new-resource.sh`), the
frontend typed client, and a **hidden fifth surface** — the auth/rate-limit
allowlists `public_paths`/`protected_paths` (`config/config*.json`,
`src/utils/Strings.hpp`) read at runtime by the advices in `src/api/Middleware.hpp`
and covered by **no** drift guard.

The template's core value is forkability plus machine-enforced spec↔code honesty:
the drift guards diff literal `(method, /path)` tuples, and the typed frontend
client keys one shape per OpenAPI `paths` entry. A versioning scheme that keeps
the version **on that same path axis** stays fully covered by both guards; a
header/media-type scheme would move it onto the one axis neither can inspect.

At adoption there are **no external API consumers** (the only client is our own
frontend, which we deploy), so a clean flag-day cutover is acceptable and no
backward-compat alias is needed.

## Decision

1. **Every API route is the literal `/api/v1/<resource>...`.** The version segment
   is `v<integer>`, kept as plain text in each route literal — there is **no
   shared `kApiPrefix` macro**, because a macro would defeat the regex-based
   literal extraction in the drift guards. Probe/infra routes (`/`, `/healthz`,
   `/ready`, `/health`, `/metrics`) stay unversioned. The Swagger UI + spec are
   versioned too (`/api/v1/docs`, `/api/v1/openapi.yaml`).
2. **No backward-compat alias.** With no external clients, backend and frontend
   cut over to `/api/v1` together in one coordinated release. (If external
   clients ever exist, reintroduce a pinned `/api/ → /api/v1` rewrite as a
   `registerSyncAdvice` registered before auth/rate-limit — see the council
   report — rather than a flag-day.)
3. **Within a major, evolution is additive-only** — add fields / endpoints /
   optional params; never remove, rename, retype, or retighten. A new major
   `/api/v2` is a sparse, deliberate, last-resort surface added *beside* v1 (only
   the routes that actually break), so v1 keeps running untouched and the drift
   guards stay green with no script change. (Greenfield forks with no external
   consumers may rename freely before their first tagged release — the typed
   client makes renames compile-safe end to end.)
4. **The convention is machine-enforced:** `new-resource.sh` derives the route
   from a single `API_VERSION` var; `new-endpoint.sh` hard-rejects a non
   `/api/v<N>/` path (no auto-prefix — that risks `/api/v1/api/orders`); and
   `check-routes-registered.sh` fails on any unversioned `/api` route.

## Consequences

- **+** The deployed-frontend coupling, both drift guards, and the typed client
  all keep covering the version because it lives in the path. `/api/v2` is cheap
  and additive. The version is visible in curl / logs / traces / metrics.
- **+** A forker cannot accidentally ship an unversioned route (scaffolder +
  lint) or desync the spec (drift guard).
- **−** The initial `/api → /api/v1` re-prefix was a large one-time mechanical
  diff across five textual surfaces (controllers, registry, spec, auth/rate-limit
  allowlists, frontend). The version is duplicated per route (no shared prefix
  seam) on purpose, so the drift guards can see it.
- **−** Backend and frontend must deploy together for the cutover (no alias). The
  non-typed frontend literals (`client.ts` refresh paths) and the auth allowlists
  are not type-checked, so they are migrated by hand — the allowlist case is now
  guarded by the version lint; the `client.ts` case is covered by tests.

## Not adopted (deferred)

- A backward-compat `/api → /api/v1` alias (no external clients today).
- An `oasdiff`/`openapi-diff` CI gate to mechanically enforce additive-only
  (kept as a code-review/ADR norm for now; add later if the API gains external
  consumers).
