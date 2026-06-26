# Benchmarks

The pitch for a C++ service is latency and footprint. Those numbers are
**hardware- and workload-specific**, so this template ships the *harness* to
measure them on your machine rather than a marketing number you can't reproduce.
The "10x vs a managed runtime" framing in the README is the general reason to
reach for C++ here; the absolute numbers below are what you should verify for
your own deployment.

> Status: this file documents the methodology. Fill the results table in for your
> hardware — `make bench` does the work. Maintainer baseline numbers will be
> published here once measured on reference hardware; until then, treat the table
> as a template, not a claim.

## What to measure

| Metric | How |
|---|---|
| Latency p50 / p99 / p99.9 | `make bench` (wrk) against `/healthz` (no DB) and `/api/jobs` (DB path) |
| Throughput (req/s) | same wrk run |
| Runtime image size | `docker build --target runtime` then `docker images` (the slim runtime stage, **not** the builder/test image) |
| Idle / under-load RSS | `docker stats` locally, or `kubectl top pod` on a cluster |
| Cold start to `/ready` | time from container start to a 200 on `/ready` |

Measure `/healthz` and a DB-backed route separately: the first isolates the HTTP
+ middleware path, the second includes Postgres/Redis round-trips, so a slow
number there points at the database, not the framework.

## Harness

`scripts/bench.sh` drives [wrk](https://github.com/wg/wrk) against the running
app under a set of config presets in `config/bench/`:

| Preset | What it varies |
|---|---|
| `baseline` | 4 server threads, DB pool 10 — the default shape |
| `threads8` | 8 server threads — scaling with cores |
| `pool20` / `pool50` | DB connection pool 20 / 50 — DB-bound headroom |
| `max` | the aggressive preset — find the ceiling |

```bash
make up                          # bring up app + Postgres + Redis
make bench                       # all presets, default endpoint
# or target one preset / endpoint / wrk args:
./scripts/bench.sh baseline /healthz
./scripts/bench.sh all /api/jobs -c100 -d10s
```

Environment overrides: `WRK_THREADS`, `WRK_CONNS`, `WRK_DURATION`, `APP_URL`.

## Methodology notes

- **Warm up** before recording — discard the first run (JIT-free here, but the
  DB pool, caches, and page cache still warm).
- **Pin the variable**: change one preset dimension at a time.
- Run the load generator off the box under test when you care about the tail —
  co-locating wrk steals CPU from the server and inflates p99.
- Record the hardware (CPU model, cores, RAM) and the commit — numbers without
  that context aren't comparable.

## Results (fill in for your hardware)

Hardware: _CPU / cores / RAM_ — commit: _short SHA_

| Preset | Endpoint | req/s | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| baseline | /healthz | | | | |
| baseline | /api/jobs | | | | |
| max | /healthz | | | | |

Footprint: runtime image _MB_ · idle RSS _MB_ · under-load RSS _MB_ · cold start _ms_
