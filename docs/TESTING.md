# Testing

What the test suite does and does not cover, and how to run each part. The goal
is that "the suite is green" means something specific — not "305 passed, 138 of
them skipped."

## Buckets

| Bucket | Count | Needs | Runs with | What it covers |
|---|---:|---|---|---|
| **unit** | 167 | nothing (sidecar-free) | `make test-unit` | Pure logic: validation, tokens, JWT, password hashing, rate-limit math, serialization, the permission bitmask, retry/backoff, templates. |
| **integration** | 109 | Postgres + Redis | `make test` | Repositories against a real Postgres, cache + rate limiter against a real Redis, migrations, the account/admin/audit/auth flows, job dispatch + DLQ. |
| **api** | 19 | Postgres + Redis | `make test` | Controller request/response behavior wired through the real handler stack. |
| **e2e** | 10 | Postgres + Redis | `make test-e2e` | A real Drogon server + client on the wire: auth gate, cookie sessions, refresh rotation/revocation, Idempotency-Key replay, tracing headers. |

Total: **305** test cases. `make test-unit` is the fast, dependency-free loop;
`make test` brings up sidecars and runs unit + integration + api; `make test-e2e`
runs the wire-level suite. `make ci-local` runs the lot the way CI does.

## Coverage

`make coverage` builds with instrumentation and runs **all** buckets, so the
number reflects the DB/cache/auth/jobs code too — not just unit-reachable lines.
The integration and e2e buckets need Postgres + Redis (`make up` first); without
them those buckets are skipped and the reported coverage drops accordingly.

## Known gaps (be honest about these before you rely on them)

- **No behavioral coverage** for Kafka messaging, SMTP delivery (the Mailer is
  exercised through the jobs path, not against a real SMTP server), or Postgres
  streaming replication. These have lifecycle/health guards only — wiring, not
  behavior.
- **Frontend** has unit tests for the session-refresh machinery and the
  permission mirror, but no component/route tests for the admin/auth UI.
- **Sanitizers (ASan/UBSan)** currently cover the **unit** bucket only. Compiling
  the integration TUs under ASan OOM'd an 8 GB build VM (heavy header-only TUs,
  no shared object file); extending it is deferred until the bodies are extracted
  into a single compiled `app_core` object (see `docs/adr/0003-header-only-modules.md`).

## A disabled test that marks a real bug

`tests/integration/test_jobs.cpp` contains
`DISABLED_CancelIsAtomicUnderContention`. It is **disabled because it documents
an unfixed race**, not because it is flaky: two concurrent callers of a job's
`cancel()` can both observe the not-yet-cancelled state and both write a terminal
status (a TOCTOU on the job row). Re-enable it once `cancel()` does a single
conditional state transition (e.g. an `UPDATE ... WHERE status = 'pending'`
guard) instead of read-then-write. Until then, treat single-cancel as supported
and concurrent-cancel as undefined.
