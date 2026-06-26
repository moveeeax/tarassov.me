# Security policy

## Reporting a vulnerability

Email `security@tarassov.me` with a description of the issue, reproduction
steps, and (if possible) a suggested fix. Do **not** open a public issue.
You will receive an acknowledgement within 3 business days and a remediation
timeline within 10 business days.

Please provide:

- Affected version / commit SHA
- Steps to reproduce
- Impact assessment (confidentiality / integrity / availability)
- Any proof-of-concept code

## Scope

In scope:

- Remote code execution or memory safety issues in the C++ code.
- Authentication / authorization bypasses (JWT validation, RBAC).
- Rate-limit / idempotency bypasses.
- Secret leakage via logs, error responses, or crash dumps.
- Supply-chain risks in dependencies declared in `vcpkg.json` or `CMakeLists.txt`.

Out of scope:

- Issues in third-party libraries already reported upstream (please link to the
  CVE; we will bump the version).
- Findings in demo/example code that does not run in production.
- Denial-of-service via unbounded client input against a dev deployment
  without rate-limiting enabled.

## Disclosure

We follow a **90-day coordinated disclosure** window from the acknowledgement
date. If a fix is not shipped by then, we will work with the reporter on a
mutually acceptable extension. Credits in the release notes on request.

## Failure-mode policies (fail-open vs fail-closed)

When the dependencies that back security middleware are unavailable, each
piece either **fails open** (let the request through) or **fails closed**
(reject the request). Operators must understand which is which to plan
incidents — a Redis outage is not the same as a database outage.

| Middleware             | When it fails | Behavior  | Why |
|------------------------|---------------|-----------|-----|
| Auth (JWT / Bearer)    | Secret missing at startup | Refuses to start | Cannot accept traffic with auth disabled — would silently authorize all requests. |
| Auth (JWT / Bearer)    | Per-request validation throws | **Fail closed** (401) | Defense in depth — never authorize a request whose validation we couldn't complete. |
| Rate limit             | Redis down | **Fail open** (allow, log warn) | Configurable via `rate_limit.fail_open`. Default is open: a stuck Redis would otherwise turn into a self-inflicted DoS. |
| Idempotency            | Redis down | **Fail open** (process request normally) | Hard-failing here would block all mutating traffic on a dependency that's only there for retry safety. The trade-off: a duplicate request during the outage may be processed twice. |
| Cache (read path)      | Redis down | **Fail open** (treat as miss, hit DB) | Cache is an optimization, not a correctness layer. |
| Cache (write path)     | Redis down | Skip silently (warn) | Same. |
| Database (any)         | Postgres down | **Fail closed** | The only authoritative store — there's no safe degraded mode. |
| Migrations on startup  | Postgres down | Refuse to start | A misapplied schema is worse than no service. |
| Tracing / metrics      | OTLP endpoint down | Drop spans/metrics, keep serving | Observability is non-blocking by design. |

**Operational implication:** during a Redis outage, attackers can bypass
rate limiting and idempotency. If your threat model can't tolerate that,
flip `rate_limit.fail_open` to `false` and accept that a Redis incident
will return 503s.

## Hardening checklist for deployments

Before shipping this template to production:

- [ ] `auth.mode=jwt` with a non-default `JWT_SECRET` (or RS256 public key).
- [ ] `rate_limit.enabled=true`.
- [ ] `idempotency.enabled=true` for non-idempotent endpoints.
- [ ] Secrets sourced from External Secrets / Vault, not inline in values.yaml.
- [ ] `networkPolicy.enabled=true` with selectors tuned for the cluster.
- [ ] TLS termination at the ingress (cert-manager or equivalent).
- [ ] Image scan (Trivy / Snyk) blocking CRITICAL.
- [ ] PrometheusRule alerts wired to pager.
