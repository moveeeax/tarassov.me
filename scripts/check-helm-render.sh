#!/usr/bin/env bash
#
# Render the cpp-env umbrella with CI values and assert semantic properties of
# the rendered manifests. Catches the deploy-path bug CLASS that compilation and
# unit tests can't — and that bit us repeatedly: ingress↔service port drift
# (frontend served 8080 but ingress said 80), baseDomain left at example.com,
# and MAIL_VIA_JOBS silently dropped because its env is gated on mail.enabled.
#
# It needs no cluster — pure `helm template` smoke test. Run via `make
# helm-validate` or the helm-charts CI job. Re-runs `helm dependency build` so a
# stale vendored subchart (charts/*.tgz are gitignored) can't skew the render.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHART="$ROOT/helm/cpp-env"
VALUES="$CHART/values-ci.yaml"

command -v helm >/dev/null 2>&1 || {
    echo "helm not installed — brew install helm"
    exit 1
}
command -v yq >/dev/null 2>&1 || {
    echo "yq not installed — brew install yq"
    exit 1
}

echo "==> helm dependency build (re-vendor subcharts)"
helm dependency build "$CHART" >/dev/null

echo "==> helm lint (umbrella with CI values)"
helm lint "$CHART" -f "$VALUES" >/dev/null || {
    echo "FAIL: helm lint"
    exit 1
}

echo "==> helm template (CI values) + semantic assertions"
RENDERED="$(helm template ci-smoke "$CHART" -f "$VALUES")"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}
q() { printf '%s' "$RENDERED" | yq "$1"; }

# 1+2. Ingress backend port must equal the service port (8080) for BOTH api and
#      the rootless-nginx frontend. This is the exact 80↔8080 drift bug.
[ "$(q 'select(.kind=="Ingress" and .metadata.name=="api") | .spec.rules[0].http.paths[0].backend.service.port.number')" = "8080" ] ||
    fail "api ingress backend port != 8080"
[ "$(q 'select(.kind=="Ingress" and .metadata.name=="app") | .spec.rules[0].http.paths[0].backend.service.port.number')" = "8080" ] ||
    fail "app/web ingress backend port != 8080 (rootless nginx listens on 8080)"

# 3. MAIL_VIA_JOBS must render on the api container — it's gated on
#    cpp-api.mail.enabled, so a missing enabled silently drops it (mail breaks).
[ "$(q 'select(.kind=="Deployment" and .metadata.name=="api") | .spec.template.spec.containers[0].env[] | select(.name=="MAIL_VIA_JOBS") | .value')" = "true" ] ||
    fail "MAIL_VIA_JOBS not rendered =true on api (set cpp-api.mail.enabled=true)"

# 4. Host templating must expand from baseDomain (catches an un-overridden
#    baseDomain — the values-ci.yaml domain is ci.example.test).
host="$(q 'select(.kind=="Ingress" and .metadata.name=="api") | .spec.rules[0].host')"
[ "$host" = "api.ci.ci.example.test" ] || fail "api ingress host did not expand from baseDomain (got: '$host')"

# 5. No Service may render a null port (a templating slip that 503s the route).
nullports="$(q 'select(.kind=="Service") | .spec.ports[] | select(.port == null) | .name' | grep -c . || true)"
[ "$nullports" = "0" ] || fail "$nullports service port(s) rendered null"

# 6. The minimal preset must stay minimal — render values-minimal.yaml and
#    assert the heavy optional services are gone, so the lighter onboarding
#    preset can't silently rot back into the kitchen sink.
echo "==> helm template (minimal preset)"
MINIMAL="$(helm template ci-smoke "$CHART" -f "$VALUES" -f "$CHART/values-minimal.yaml")"
workloads="$(printf '%s' "$MINIMAL" | yq 'select(.kind=="Deployment" or .kind=="StatefulSet") | .metadata.name')"
printf '%s' "$workloads" | grep -qi kafka && fail "values-minimal still renders a Kafka workload"
printf '%s' "$workloads" | grep -qi jaeger && fail "values-minimal still renders a Jaeger workload"

# 7. Production security floor. prod-check.sh validates the JSON app-config, but
#    the deploy-path env comes from Helm — so the rate limiter shipping fail-OPEN
#    in prod (a Redis blip silently disables login throttling) was invisible to
#    it. Render the cpp-api chart with the TRACKED prod example overlay and
#    assert the security-critical env the cluster actually runs with.
echo "==> helm template (cpp-api prod example) + security assertions"
API_CHART="$ROOT/helm/cpp-api"
PROD_RENDERED="$(helm template prod-smoke "$API_CHART" -f "$API_CHART/values-prod.example.yaml")"
penv() {
    printf '%s' "$PROD_RENDERED" |
        yq "select(.kind==\"Deployment\") | .spec.template.spec.containers[0].env[] | select(.name==\"$1\") | .value"
}

[ "$(penv RATE_LIMIT_ENABLED)" = "true" ] || fail "prod overlay: RATE_LIMIT_ENABLED != true (login is unthrottled)"
[ "$(penv RATE_LIMIT_FAIL_OPEN)" = "false" ] ||
    fail "prod overlay: rate limiter is fail-OPEN — a Redis outage disables the brute-force throttle on login. Set rateLimit.failOpen=false."
[ "$(penv AUTH_MODE)" = "jwt" ] || fail "prod overlay: AUTH_MODE != jwt (endpoints would be public)"
[ "$(penv AUTH_COOKIE_SECURE)" = "true" ] || fail "prod overlay: AUTH_COOKIE_SECURE != true (__Host- cookies need Secure)"

# No plaintext secret may be baked into a TRACKED overlay — DB password / JWT
# secret must arrive via --set or external-secrets (rendered as secretKeyRef),
# so their plain .value is empty here. A non-empty value = a committed secret.
for s in DATABASE_PASSWORD JWT_SECRET; do
    [ -z "$(penv "$s")" ] || fail "prod overlay: $s carries a plaintext value — pass it via --set / external-secrets, never a tracked overlay"
done

echo "==> helm-render: all assertions passed"
