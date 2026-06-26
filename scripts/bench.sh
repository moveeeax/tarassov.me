#!/usr/bin/env bash
# Run wrk benchmark against the app with a given config preset.
# Usage:
#   ./scripts/bench.sh                              # all presets, default endpoint
#   ./scripts/bench.sh baseline                     # single preset
#   ./scripts/bench.sh baseline /api/jobs           # single preset + custom endpoint
#   ./scripts/bench.sh all /healthz -c100 -d10s     # all presets, custom wrk args
#
# Environment:
#   WRK_THREADS   wrk threads        (default: 4)
#   WRK_CONNS     wrk connections    (default: 200)
#   WRK_DURATION  wrk duration       (default: 30s)
#   APP_URL       base URL           (default: http://localhost:8080)

set -euo pipefail

# wrk is the load generator; fail early with a clear message if it's missing
# (global convention: command -v before invoking external tools).
if ! command -v wrk >/dev/null 2>&1; then
    echo "ERROR: 'wrk' not found — install it (brew install wrk / apt install wrk)" >&2
    exit 2
fi

# Accept either docker-compose (v1 dashed) or docker compose (v2 plugin).
# The template's Makefile uses the dashed variant; some dev machines only
# have the plugin. Detect once so we fail early with a clear message.
if command -v docker-compose >/dev/null 2>&1; then
    COMPOSE="docker-compose -f docker/docker-compose.yml --env-file docker/.env"
elif docker compose version >/dev/null 2>&1; then
    COMPOSE="docker compose -f docker/docker-compose.yml --env-file docker/.env"
else
    echo "ERROR: neither 'docker-compose' nor 'docker compose' is available" >&2
    exit 2
fi

APP_URL="${APP_URL:-http://localhost:8080}"
WRK_THREADS="${WRK_THREADS:-4}"
WRK_CONNS="${WRK_CONNS:-200}"
WRK_DURATION="${WRK_DURATION:-30s}"

preset="${1:-all}"
# Default endpoint:
#   - pool* presets vary the DB/Redis pool size, so they MUST hit a path that
#     touches the database or the pool size makes no difference (and the run
#     just measures Drogon + middleware). /api/jobs is the cheapest such path
#     when JOBS_ENABLED; falls back to /healthz for baseline/threads presets.
#   - Override with the 2nd arg for anything else.
# NB: the sync-DB model caps effective concurrency at server.threads, so
# pool_size > threads is inert — pool20/pool50 only differ under a DB endpoint
# AND threads >= pool. See docs/CONFIG.md.
default_endpoint="/healthz"
case "$preset" in
    pool20 | pool50 | max) default_endpoint="/api/jobs" ;;
esac
endpoint="${2:-$default_endpoint}"
shift 2 2>/dev/null || true
# --latency by default so we get tail percentiles, not just the average.
EXTRA_WRK_ARGS=("$@")
if [ ${#EXTRA_WRK_ARGS[@]} -eq 0 ]; then
    EXTRA_WRK_ARGS=(--latency)
fi

# Collect available presets
if [ "$preset" = "all" ]; then
    presets=(baseline pool20 pool50 threads8 max)
else
    presets=("$preset")
fi

wait_ready() {
    local retries=20
    while [ $retries -gt 0 ]; do
        if curl -sf "${APP_URL}/ready" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        retries=$((retries - 1))
    done
    echo "ERROR: app did not become ready" >&2
    return 1
}

run_bench() {
    local name="$1"
    local config_file="config/bench/${name}.json"

    if [ ! -f "$config_file" ]; then
        echo "SKIP: $config_file not found" >&2
        return
    fi

    echo ""
    echo "============================================================"
    echo " Preset: $name"
    echo " Config: $config_file"
    echo " Endpoint: $endpoint"
    echo " wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION}"
    echo "============================================================"

    # Restart app with new config (no rebuild needed)
    $COMPOSE stop app >/dev/null 2>&1
    export CONFIG_FILE="config/bench/${name}.json"
    $COMPOSE up -d app >/dev/null 2>&1

    if ! wait_ready; then
        $COMPOSE logs --tail 10 app
        return 1
    fi

    # Warmup
    curl -sf "${APP_URL}${endpoint}" >/dev/null 2>&1 || true
    sleep 1

    # Guarded array expansion — macOS ships bash 3.2 where `"${arr[@]}"` on
    # an empty array trips `set -u` with "unbound variable".
    wrk -t"${WRK_THREADS}" -c"${WRK_CONNS}" -d"${WRK_DURATION}" \
        ${EXTRA_WRK_ARGS[@]+"${EXTRA_WRK_ARGS[@]}"} "${APP_URL}${endpoint}"
}

# Ensure infra is up. Surface compose errors (port clashes, missing Docker,
# etc.) instead of silently bailing — the old `>/dev/null 2>&1` masked them.
echo "Ensuring infrastructure is running..."
if ! $COMPOSE up -d postgres redis; then
    echo "ERROR: failed to bring up postgres + redis; run 'make down' and retry" >&2
    exit 3
fi
sleep 2

for p in "${presets[@]}"; do
    run_bench "$p"
done

echo ""
echo "Done. App is still running with the last preset."
