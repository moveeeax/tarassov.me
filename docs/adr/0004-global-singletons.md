# 0004 — Global singletons for subsystems

**Status:** Accepted

## Context

Every subsystem is a singleton accessed via `Module::get()`:
- `Config::get()`, `Database::get()`, `Cache::get()`, `Observability::get()`,
  `Security::Auth::get()`, `Jobs::get()`, etc.

Each module holds a `std::unique_ptr<Impl>` initialized by `Module::initialize()`
and torn down by `Module::shutdown()`. `Core::initialize()` orchestrates the
order (Config → Observability → Database → Migrations → Cache → Messaging →
Tasks → Security → Jobs → Mailer).

Why this is worth a record: globals are a code smell in many shops, and a
new contributor will reasonably ask "should I use DI here?" before adding
a module. The answer is "no, follow the existing pattern" — this ADR
explains why.

## Decision

Keep the global-singleton pattern for subsystems. New modules follow the
same shape:

```cpp
namespace MyModule {
    inline std::unique_ptr<Impl> global_instance = nullptr;
    inline void initialize(...) { /* assert not initialized; create */ }
    inline Impl& get() { /* assert initialized; return ref */ }
    inline bool is_initialized() { return global_instance != nullptr; }
    inline void shutdown() { global_instance.reset(); }
}
```

## Consequences

**Wins:**
- Controllers and middleware reach any subsystem with one line: `Database::get().execute_read(...)`. No constructor injection, no service locator boilerplate.
- Initialization order is explicit and centralized in `Core::initialize()` — easy to audit, easy to extend.
- Shutdown is mirror-symmetric and predictable. Reverse-order teardown is enforced by `Core::shutdown()`.
- Tests that need real subsystems just call `Module::initialize(...)` (see `test_helpers.hpp::reset_all_globals`).

**Costs:**
- Unit tests that want to mock a subsystem can't — there's no seam to swap
  out `Database::get()`. The current testing strategy works around this by
  using real Postgres/Redis containers (integration-flavored tests). For
  pure-logic modules (Validation, Retry, Trace) the tests stay unit-style.
- Two services in the same process (e.g. an embedded admin API alongside
  the main API) would conflict over the singleton. We don't do that today;
  if a use case appears, the containing module would need to grow a
  per-instance constructor.
- Initialization-order bugs surface as runtime exceptions, not compile errors.
  `Core::initialize()` mitigates this by being the only call site in production code.

## When to revisit

If we add per-tenant or per-test instance lifetimes (e.g. multiple
`DatabaseManager` objects in one process), the singleton has to
become a default rather than the only option — wrap the existing
shape in a non-singleton class and make `get()` return a configured
instance.

## Alternatives

- **DI container** (Boost.DI, Hypodermic, hand-rolled). Adds a layer of
  indirection that buys nothing for a service this size; gains hypothetical
  testability we don't currently exercise.
- **Pass references explicitly** through controllers. Adds a parameter to
  every controller method and middleware function — high friction for a
  speculative win.
