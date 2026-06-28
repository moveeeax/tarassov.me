# SLOs & alert thresholds

Defines what "healthy" means so the alerts in `docs/prometheus-rules.yml` /
the Helm `PrometheusRule` have a rationale, and on-call has a target rather
than a guess. These are **starting defaults** — set yours from real traffic.

## Service-level objectives (suggested)

| SLO | Target | Measured by |
|---|---|---|
| Availability (non-5xx) | 99.9% / 30d | `1 - rate(http_requests_total{status=~"5.."}) / rate(http_requests_total)` |
| Latency | p99 < 1s | `histogram_quantile(0.99, http_request_duration_seconds)` |
| Job delivery | DLQ drains < 15m | `jobs_dlq_depth` returns to 0 |
| Job timeliness | queue drains, no backlog | `jobs_queue_depth{type="_total"}` stays low |
| DB pool saturation | < 90% utilized | `db_pool_active_connections / db_pool_size` |

Error budget at 99.9% ≈ 43 min/month of full unavailability. The `High5xxRate`
alert fires at 5% (well above budget burn) so it catches incidents, not slow
burn — add a multi-window burn-rate alert if you adopt strict budgeting.

## Alert thresholds → SLO mapping

| Alert | Threshold | Why | Tune when |
|---|---|---|---|
| `High5xxRate` | 5xx > 5% for 5m | Fast incident signal, not budget burn | Lower to 1% once traffic is steady |
| `HighP99Latency` | p99 > 1s for 10m | Matches latency SLO | Set to your real p99 + headroom |
| `ReplicationLagHigh` | lag > 60s for 5m | Stale reads become user-visible | Lower if you serve read-heavy traffic from replicas |
| `DeadLetterQueueGrowing` | DLQ > 0 (or `dlqDepth`) for 15m | Jobs silently failing | Raise `dlqDepth`/`dlqPerTypeDepth` if some failures are expected |
| `RetriesExhaustedSpike` | exhausted retries sustained | Downstream past retry budget | — |
| `JobsQueueBacklog` | queue `_total` > 100 for 10m | Workers outrun by submitters | Set to your throughput headroom |
| `DbPoolSaturationHigh` | active/size > 90% for 5m | Connections about to time out on acquire | Raise pool size first, then revisit |
| `*TargetDown` | no scrape 2m | Process down/wedged | — |

Helm thresholds live in `values.yaml → monitoring.thresholds`; the compose
copy is inlined in `docker/prometheus-rules.yml`. Every alert links to
`docs/RUNBOOK.md` via `runbook_url`.

## What's NOT measured yet (gaps to wire when you need them)

- No multi-window burn-rate alerting (single-threshold only).
- Dashboards are provisioned (`docker/grafana/`) but the panel set is minimal —
  build per-SLO panels from the queries above.
