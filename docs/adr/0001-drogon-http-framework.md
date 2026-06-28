# 0001 — Drogon as the HTTP framework

**Status:** Accepted

## Context

The template needs an async, multithreaded HTTP framework for C++20 with:
- Production-ready performance (event-loop based, not thread-per-connection).
- A controller-style routing API that's easy to scaffold.
- WebSocket and SSL support out of the box.
- Active maintenance and a recent C++ standard baseline.

Candidates considered:
- **Drogon** — event-loop, controller-class API, WebSocket + SSL + ORM, active.
- **Crow** — header-only, simpler, slower under load, less middleware.
- **Pistache** — async, modern, smaller community, no built-in templating.
- **Boost.Beast** — lower-level, more boilerplate, no controllers/middleware.
- **cpprestsdk** — abandoned by Microsoft.

## Decision

Use Drogon. The controller-class macros (`HttpController<>`, `METHOD_LIST_*`,
`ADD_METHOD_TO`) cut routing boilerplate to one line per endpoint. The
event-loop model scales to high RPS without a thread per request. The
built-in advice hooks (`registerSyncAdvice`, `registerPostHandlingAdvice`)
let middleware (auth, rate limiting, idempotency) attach without a custom
filter framework.

## Consequences

- Bound to Drogon's request/response types in controllers and middleware.
- Drogon ships its own JSON layer (jsoncpp); see ADR 0002 for how we work
  around that.
- Migration to a different framework would touch every controller header
  and the entire `api/Api.hpp` middleware chain.

## Alternatives

If Drogon stops being maintained, Pistache is the most likely successor —
similar event-loop model, fewer features but actively developed.
