# ADR 0005: Frontend lives in a separate React SPA

**Status:** accepted (2026-05)
**Context:** flask-base ships as a Flask SSR app. We mirror its
account/admin feature set but want the frontend to be a fully
independent project so it can deploy to a CDN, evolve on its own
release cadence, and stay testable without booting the C++ backend.

## Decision

- The backend speaks JSON only over `/api/*`. No HTML rendering, no
  Flask-style flash messages, no server-side templating except for
  the email bodies in `templates/email/`.
- The frontend is a Vite + React + TypeScript SPA in `frontend/`.
  It consumes the backend through a small hand-written fetch wrapper
  (`lib/api/client.ts` — `api.getJson/postJson/...`). Domain types are
  generated from `docs/openapi.yaml` by openapi-typescript into
  `schema.gen.ts` (`npm run gen:api`) and re-exported flat from
  `lib/api/types.ts`.
- Sessions live in HttpOnly cookies (`__Host-access` + `__Host-refresh`)
  set by `/api/auth/login`. The SPA never touches them. Refresh tokens
  carry a JTI tracked in Redis for instant revocation.
- Everything is same-origin: in dev via Vite's `/api` proxy, in prod
  via nginx `proxy_pass /api -> app:8080` from the frontend container.
  `SameSite=Lax` keeps the cookies on without CORS gymnastics.

## Considered alternatives

- **HTMX + server fragments.** Smaller blast radius, closer to flask-
  base's UX, but the frontend stops being "an actually independent
  project" — it becomes a loose pile of templates served from the C++
  binary. Rejected.
- **SSR proxy (Node/Bun rendering Jinja-equivalent templates against
  the JSON API).** Three services in the stack instead of two; double-
  rendering for no benefit a CDN-served SPA can't match. Rejected.
- **Auth via localStorage JWT.** Standard XSS sinkhole. Rejected.

## Consequences

- The backend test suite exercises every controller method directly;
  it doesn't depend on the SPA being built.
- The SPA's e2e tests will run against a real backend container (stage
  6 follow-up) — Playwright spinning up `make up` then driving the SPA.
- Two release artefacts: the C++ image and the nginx-static image.
  Both are tagged off the same git SHA in `.github/workflows/release.yml`.
- One spec is the boundary: `docs/openapi.yaml`. The drift checker
  (`scripts/check-openapi-drift.sh`) keeps registry ↔ spec in sync;
  `npm run gen:api` keeps frontend types ↔ spec in sync.
