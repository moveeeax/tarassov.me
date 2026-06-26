# Operator runbook

What to do when an alert fires or an incident lands. Each alert in
`docs/prometheus-rules.yml` / the Helm `PrometheusRule` carries a
`runbook_url` anchor that points here.

Conventions: `make` targets assume the compose stack; in k8s substitute
`kubectl exec`/`logs`. Trace IDs are in every access-log line (`tid=`) and the
`X-Request-Id` response header — `make tail-trace TID=<id>` follows one request.

---

## ApiTargetDown / WorkerTargetDown {#targetdown}

Prometheus hasn't scraped the process for 2 min — it's down or wedged.

1. `kubectl get pods` / `make ps` — is it crash-looping or gone?
2. Logs: `kubectl logs <pod> --previous` / `make logs`. Look for the init
   order line that's missing (Config→Observability→Database→…→Mailer).
3. Common causes: DB/Redis unreachable at boot (Database throws → Core aborts;
   replica being down does NOT abort, that's handled), bad `JWT_SECRET` under
   `AUTH_MODE=jwt`, port already bound.
4. `/healthz` (alive) vs `/ready` (dependencies). 503 on `/ready` with 200 on
   `/healthz` = draining or a dependency is unhealthy → check `/health`.

## High5xxRate {#high5xx}

>5% of responses are 5xx.

1. Find the failing route: Grafana `http_requests_total{status=~"5.."}` by
   `path`. The path is normalized (`/api/jobs/:id`), tokens redacted.
2. Pull a failing trace (`tid=` in the log) — `make tail-trace TID=<id>` shows
   the `db.*` child spans with the SQL template + pool label.
3. DB-driven? See [HighP99Latency](#p99). Dependency down? See
   [RetriesExhaustedSpike](#retries).

## HighP99Latency {#p99}

p99 > 1s for 10 min. Almost always DB pool saturation or slow queries.

1. `db_queries_total{pool=...}` — are reads hitting the replica or all on
   primary? (replica down → fallback to primary, see its alert).
2. Pool saturation: effective concurrency is `min(pool_size, server.threads)`
   — `pool_size > threads` does nothing. Check `SERVER_THREADS` vs
   `DB_POOL_SIZE` (docs/CONFIG.md).
3. Slow query: `statement_timeout` (default 30s) caps it; find it via the
   trace's `db.statement`. Add an index or paginate.

## ReplicationLagHigh {#replag}

Read replica >60s behind. Stale reads are being served.

1. On the replica: `SELECT now() - pg_last_xact_replay_timestamp();`
2. Check replica I/O / WAL volume pressure and the network to the primary.
3. Mitigation: the app already fails reads over to the primary if a replica
   errors, but lag (not error) still serves stale data. To force primary-only
   temporarily, set `DATABASE_REPLICA_URLS=""` and restart.

## DeadLetterQueueGrowing {#dlq}

Jobs exhausted their retries and landed in the DLQ.

1. Inspect: admin UI **/admin/jobs → DLQ tab**, or
   `GET /api/jobs/dlq` (admin token). Each row has the last `error` and a
   `trace_id` → open the worker trace.
2. Transient cause now fixed (e.g. SMTP was down)? Requeue:
   `POST /api/jobs/dlq/{id}/requeue` or the UI button.
3. Code bug? Fix the handler, redeploy the worker, then requeue.
4. Account emails specifically: check `WORKER_TYPES` includes `account_email`
   and the worker has `JWT_SECRET` + `MAIL_*` (a missing secret DLQ's every
   email with `master_secret must be set`).

## RetriesExhaustedSpike {#retries}

A downstream (DB/Redis) is failing past the retry budget.

1. Which? `retries_total{component=...,outcome="exhausted"}`.
2. Redis: cache ops are fail-open (degraded, not down) EXCEPT session-mint
   (login fails closed if it can't record the refresh JTI) — so a Redis
   outage shows as login failures + rate-limit fail-closed (if configured).
3. DB: see [HighP99Latency](#p99) / [ApiTargetDown](#targetdown).

## JobsQueueBacklog {#queuebacklog}

The waiting queue (`jobs_queue_depth{type="_total"}`) is deep and not draining —
submitters are outrunning the worker pool. This is the *leading* signal; left
alone, jobs eventually age into the DLQ ([DeadLetterQueueGrowing](#dlq)).

1. Which type? `jobs_queue_depth` is labeled by `type`.
2. Workers alive and consuming? Check `up{job="tarassov_me_worker"}` and the worker
   logs. A stuck handler (one slow job type blocking its thread) starves the
   rest — BRPOP concurrency equals the thread count.
3. Genuine load spike? Scale worker replicas / `WORKER_CONCURRENCY`.

## DbPoolSaturationHigh {#dbpool}

A connection pool is over 90% utilized (`db_pool_active_connections /
db_pool_size`). At saturation, `acquire()` blocks up to the acquire timeout and
then throws — surfacing as 5xx and high p99.

1. Which pool? The alert is labeled `pool` (primary/replica).
2. Slow queries holding connections? Check the DB's `pg_stat_activity` and the
   `db.statement` attribute on slow `db.*` spans.
3. Real concurrency demand? Raise `DATABASE_POOL_SIZE` (and the server's
   `max_connections`), or shed load. A leaked connection (active stuck high
   while traffic is idle) points at a handler that never returns its txn.

---

## Backups {#backup}

Off by default — set `backup.enabled=true` and point `backup.s3.*` at a bucket.
The CronJob (`backup-cronjob.yaml`) runs `pg_dump`, gzips it, **verifies the gzip
(`gunzip -t`) and a non-trivial size before uploading**, so a failed dump aborts
the Job instead of silently shipping a truncated file. Two alerts watch it (need
kube-state-metrics): `CppApiBackupJobFailed` (last run failed) and
`CppApiBackupTooOld` (no success within `monitoring.thresholds.backupMaxAgeSeconds`,
default 26h).

Verify a backup is real, don't just trust the green Job:

```sh
aws s3 ls s3://$BUCKET/ | tail        # a recent, non-tiny object exists
aws s3 cp s3://$BUCKET/<latest>.sql.gz - | gunzip -t && echo "gzip OK"
```

Restore is a deliberate human action — see below. **Do a restore drill** before
you depend on these backups; an untested backup is a guess.

## Restore Postgres {#restore}

Backups (if `backup.enabled`) are `pg_dump` gzips in the configured S3 bucket
(`backup-cronjob.yaml`). This is logical backup — no PITR. For PITR move to
pgBackRest / WAL-G.

```sh
# 1. Fetch the dump
aws s3 cp s3://<bucket>/tarassov-me/appdb-<ts>.sql.gz .
# 2. Restore into a FRESH database (never over a live one without a maintenance window)
gunzip -c appdb-<ts>.sql.gz | psql -h <host> -U <user> -d appdb_restore
# 3. Verify, then cut over (repoint DATABASE_PRIMARY_URL, rolling restart).
```

Migrations re-run idempotently on boot (advisory-locked), so a restored older
schema will be brought forward automatically by the next deploy.

## Roll back a release {#rollback}

- k8s: `helm rollback tarassov-me <REVISION>` (`helm history tarassov-me`).
- Images are tagged `vX.Y.Z` in GHCR; pin the previous tag if needed.
- Migrations are forward-only — a rollback of the app does NOT revert schema.
  If a migration is the problem, write a new forward migration that fixes it.
