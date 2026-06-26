#!/usr/bin/env bash
# Pre-deployment gate: assert that a config profile + current environment
# describe a PRODUCTION-safe setup. Complements env-check.sh (which only
# finds unset placeholders) with semantic checks: auth on, cookies secure,
# rate limiter fail-closed, docs UI off, secrets non-weak.
#
# Usage:
#   ./scripts/prod-check.sh [config/config.production.json]
#   make prod-check
#
# Exit: 0 when every check passes, 1 otherwise.

set -euo pipefail

CONFIG="${1:-config/config.production.json}"
FAILURES=0

if [[ ! -f "$CONFIG" ]]; then
    echo "error: $CONFIG not found" >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "error: jq is required (brew install jq / apt install jq)" >&2
    exit 2
fi

fail() {
    printf '\e[1;31m✗ %s\e[0m\n' "$1"
    FAILURES=$((FAILURES + 1))
}
pass() { printf '\e[1;32m✓ %s\e[0m\n' "$1"; }

# Resolve a value the way the app does: env var wins, else JSON literal.
# (Placeholders in JSON resolve from env too — we check the env directly.)
resolved() {
    local json_path="$1" env_var="$2"
    if [[ -n "$env_var" && -n "${!env_var:-}" ]]; then
        printf '%s' "${!env_var}"
        return
    fi
    jq -r "$json_path // empty" "$CONFIG"
}

echo "== prod-check: $CONFIG =="

# 1. Auth must not be 'none'.
MODE=$(resolved '.auth.mode' 'AUTH_MODE')
case "$MODE" in
jwt | bearer) pass "auth.mode = $MODE" ;;
*'${'*) fail "auth.mode unresolved placeholder ('$MODE') — set AUTH_MODE" ;;
*) fail "auth.mode = '$MODE' (must be jwt or bearer in production)" ;;
esac

# 2. JWT secret present and not laughably weak.
if [[ "$MODE" == "jwt" ]]; then
    SECRET="${JWT_SECRET:-}"
    if [[ -z "$SECRET" ]]; then
        fail "JWT_SECRET is unset"
    elif [[ ${#SECRET} -lt 32 ]]; then
        fail "JWT_SECRET is shorter than 32 chars (have ${#SECRET}) — use 'openssl rand -hex 32'"
    elif [[ "$SECRET" == *dev* || "$SECRET" == *test* || "$SECRET" == *secret* || "$SECRET" == *change* ]]; then
        fail "JWT_SECRET looks like a dev placeholder"
    else
        pass "JWT_SECRET set (${#SECRET} chars)"
    fi
fi

# 3. Cookies secure when enabled.
if [[ "$(jq -r '.auth.cookies.enabled' "$CONFIG")" == "true" ]]; then
    if [[ "$(jq -r '.auth.cookies.secure' "$CONFIG")" == "true" ]]; then
        pass "auth.cookies.secure = true"
    else
        fail "auth.cookies.secure must be true in production (cookies over https only)"
    fi
fi

# 4. Rate limiter: on and fail-closed.
if [[ "$(jq -r '.rate_limit.enabled' "$CONFIG")" == "true" ]]; then
    pass "rate_limit.enabled = true"
else
    fail "rate_limit.enabled must be true in production"
fi
if [[ "$(jq -r '.rate_limit.fail_open' "$CONFIG")" == "false" ]]; then
    pass "rate_limit.fail_open = false"
else
    fail "rate_limit.fail_open must be false (Redis outage must not disable the limiter)"
fi
# The auth/account surface is public (no JWT) but must still be throttled, or
# login/register/reset are wide open to brute-force / mail-bombing.
if [[ "$(jq -r '.rate_limit.protected_requests // "null"' "$CONFIG")" != "null" ]]; then
    pass "rate_limit.protected_requests set (login/register/reset throttled)"
else
    fail "rate_limit.protected_requests must be set so the public auth endpoints are rate-limited"
fi

# 5. Swagger UI off.
DOCS=$(resolved '.docs.enabled' 'DOCS_ENABLED')
if [[ "$DOCS" == "false" || -z "$DOCS" ]]; then
    pass "docs.enabled = false"
else
    fail "docs.enabled must be false in production (Swagger UI exposes the surface)"
fi

# 6. Database password enforcement + non-weak value when provided.
if [[ "${DATABASE_REQUIRE_SECURE_PASSWORD:-}" == "true" ]]; then
    pass "DATABASE_REQUIRE_SECURE_PASSWORD=true"
else
    fail "set DATABASE_REQUIRE_SECURE_PASSWORD=true so weak DB passwords abort boot"
fi
case "${DATABASE_PASSWORD:-}" in
'' | postgres | password | changeme | admin | root | 123456)
    fail "DATABASE_PASSWORD is unset or a known-weak default"
    ;;
*) pass "DATABASE_PASSWORD set" ;;
esac

# 7. JSON log format (aggregator-ready).
if [[ "$(jq -r '.logging.format' "$CONFIG")" == "json" ]]; then
    pass "logging.format = json"
else
    fail "logging.format should be json in production"
fi

# 8. Delegate placeholder completeness to env-check.
if ./scripts/env-check.sh "$CONFIG" >/dev/null 2>&1; then
    pass "env-check: all required placeholders satisfied"
else
    fail "env-check found unsatisfied placeholders — run: ./scripts/env-check.sh $CONFIG"
fi

echo
# This gate only sees the JSON app-config. On Kubernetes the deploy-path env is
# rendered by Helm, so a fail-open rate limiter / public auth.mode can hide in
# the chart values where this check can't see it. `make helm-validate`
# (scripts/check-helm-render.sh) renders the prod overlay and asserts those —
# run BOTH before shipping.
echo "NOTE: also run 'make helm-validate' — it checks the rendered Helm deploy-path"
echo "      (rate-limit fail-open, auth mode/cookies, no committed secrets)."
echo
if [[ $FAILURES -gt 0 ]]; then
    printf '\e[1;31mprod-check FAILED: %d issue(s).\e[0m\n' "$FAILURES"
    exit 1
fi
echo "prod-check passed — configuration is production-shaped."
