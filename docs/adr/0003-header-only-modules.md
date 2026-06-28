# 0003 — Header-only module layout

**Status:** Accepted

## Context

Every module under `src/` (Database, Cache, Auth, Jobs, …) is a single
`.hpp` file with implementation inline. The only `.cpp` files are
`main.cpp` and `worker_main.cpp` — the executable entry points.

This is unusual for production C++ and easy to misread as "lazy header
hygiene." It's a deliberate template choice and worth recording.

## Decision

Keep modules header-only as long as the template stays a one-binary service.

Rationale:
- **Readability for forks.** A new user clones the template, sees one
  `.hpp` per concern, and knows where everything lives. Splitting each
  module across `.hpp` + `.cpp` doubles file count for no architectural
  win at this size.
- **No public ABI to worry about.** The template is a self-contained
  service, not a library. Every translation unit is rebuilt together,
  so the usual case for `.cpp` separation (preventing recompiles, hiding
  implementation) doesn't pay off here.
- **`inline` and templates dominate.** Most module code is templated
  (e.g. `Retry::run`, `Database::execute_*`, `cache_aside`) — those have
  to live in headers anyway. The remaining non-templated bits get
  `inline` and tag along.
- **Tests link the same headers.** No surprise about which symbols are
  available where.

## Consequences

- **Build is slower than it has to be.** Every `.cpp` (today: just
  `main.cpp`) re-parses every module on every change. Acceptable today
  because the dependency graph is tiny.
- **Symbols leak into every TU.** Anonymous namespaces inside headers
  (used for helpers) keep linkage clean. Be careful adding non-`inline`
  globals — they cause ODR violations.
- **Adding a new module is one file**, not two — friction for forks
  stays low.

## When to revisit

Revisit if any of:
- The build takes more than 30 seconds incremental on a developer
  machine.
- Test compile time dominates the test loop.
- A module grows past ~1000 lines and would benefit from interface/impl
  separation for readability.

## Alternatives

Standard `.hpp` + `.cpp` split — doubles file count, slows incremental
builds for new contributors who edit `.hpp` (everything rebuilds anyway).
The wins (precompiled headers, parallel compilation) only kick in past a
codebase size we haven't reached.
