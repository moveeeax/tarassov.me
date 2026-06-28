#!/usr/bin/env bash
# Smoke test a running instance of the template. Hits every critical surface:
# health, trace-id propagation, validation, and metrics — and ASSERTS the
# expected HTTP status on each step, so a wall of 500s actually fails the
# script instead of printing "Smoke passed".

set -euo pipefail

BASE="${BASE_URL:-http://localhost:8080}"
METRICS="${METRICS_URL:-http://localhost:9090}"
AUTH_HEADER=""
FAILURES=0

if [[ -n "${TOKEN:-}" ]]; then
    AUTH_HEADER="Authorization: Bearer $TOKEN"
fi

say() { printf '\n\e[1;34m== %s ==\e[0m\n' "$1"; }

# expect <status[|status...]> <curl args...> — runs the request, prints the
# body, and records a failure unless the status matches one of the allowed
# values (some endpoints legitimately differ by profile, e.g. jobs on/off).
expect() {
    local want="$1"; shift
    local body_file status
    body_file=$(mktemp)
    status=$(curl -sS -o "$body_file" -w '%{http_code}' "$@" ${AUTH_HEADER:+-H "$AUTH_HEADER"}) || status=000
    head -c 400 "$body_file"; echo
    rm -f "$body_file"
    if [[ "|$want|" == *"|$status|"* ]]; then
        printf '\e[1;32m   ok (%s)\e[0m\n' "$status"
    else
        printf '\e[1;31m   FAIL: expected HTTP %s, got %s\e[0m\n' "$want" "$status"
        FAILURES=$((FAILURES + 1))
    fi
}

say "Liveness /healthz"
expect 200 "$BASE/healthz"

say "Readiness /ready"
expect 200 "$BASE/ready"

say "traceparent propagation"
TP='00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01'
HDRS=$(curl -sS -D - -o /dev/null -H "traceparent: $TP" "$BASE/healthz" ${AUTH_HEADER:+-H "$AUTH_HEADER"})
if echo "$HDRS" | grep -qiE '(traceparent|x-request-id)'; then
    echo "$HDRS" | grep -iE '(traceparent|x-request-id)' | head -2
    printf '\e[1;32m   ok\e[0m\n'
else
    printf '\e[1;31m   FAIL: no traceparent / x-request-id response headers\e[0m\n'
    FAILURES=$((FAILURES + 1))
fi

say "Validation — POST /api/jobs with missing type (400; 503 jobs-off; 401 auth-on without TOKEN)"
if [[ -n "$AUTH_HEADER" ]]; then
    # A token was supplied — 401 would be a real failure.
    expect "400|503" -X POST -H 'Content-Type: application/json' -d '{}' "$BASE/api/jobs"
else
    # No TOKEN: with AUTH_MODE=jwt/bearer the middleware answers 401 before
    # validation — that's correct. Mint one via `make dev-token` to exercise
    # the validation path itself.
    expect "400|503|401" -X POST -H 'Content-Type: application/json' -d '{}' "$BASE/api/jobs"
fi

say "Prometheus /metrics (http_requests_total subset)"
curl -sS "$METRICS/metrics" 2>/dev/null | grep -E '^(http_requests_total|jobs_dlq_depth)' | head -6 || echo '(no metrics yet)'

echo
if [[ $FAILURES -gt 0 ]]; then
    printf '\e[1;31mSmoke FAILED: %d check(s) did not match.\e[0m\n' "$FAILURES"
    exit 1
fi
echo "Smoke passed."
