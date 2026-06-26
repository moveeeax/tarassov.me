#!/usr/bin/env bash
#
# Measure warm incremental rebuild time after touching each "hot" header — the
# DATA GATE for the T1 de-header decision (ADR 0003). The header-only design is
# only a problem if editing a hot module forces a slow rebuild; this measures
# whether it actually does, instead of guessing.
#
# Decision rule (ADR 0003): if every hot-header rebuild is under 30s, the
# header-only design is fine and breaking it up is premature pessimization. If a
# module exceeds 30s, THAT module is a candidate for extracting its bodies into
# a single compiled object (the `app_core` static library), not the whole tree.
#
# Usage: scripts/bench-incremental.sh [cmake-preset]   (default: dev)
# Needs a working local toolchain (VCPKG_ROOT + the dev preset). For a
# Docker-based run, build the `builder` stage and run this against /app/build.
set -euo pipefail

PRESET="${1:-dev}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

command -v cmake >/dev/null 2>&1 || {
    echo "cmake not found" >&2
    exit 1
}

# Hot headers: the most-edited / widest-fan-in modules (git churn + LOC).
HOT=(
    src/core/Core.hpp
    src/api/Middleware.hpp
    src/jobs/Jobs.hpp
    src/database/Database.hpp
    src/cache/Cache.hpp
    src/security/Auth.hpp
)

echo "==> warming build (preset=$PRESET) — first run compiles everything"
cmake --preset "$PRESET" >/dev/null
cmake --build --preset "$PRESET" >/dev/null

# Two numbers per header: the INNER LOOP (rebuild just the app — what a dev
# hits editing+running locally) and the FULL build (app + every test bucket —
# what CI and `make test` hit). They tell different stories: the inner loop
# measures whether header-only hurts day-to-day; the gap to the full build
# measures the redundant per-binary recompile that an app_core static lib would
# collapse (compile the bodies once, link them into all 5 executables).
APP_TARGET="${APP_TARGET:-${PROJECT_NAME:-cpp_api_template}}"
echo ""
printf '%-28s %10s %10s\n' "touched header" "app-only" "full"
printf '%-28s %10s %10s\n' "----------------------------" "----------" "----------"
worst_app=0
for f in "${HOT[@]}"; do
    touch "$ROOT/$f"
    SECONDS=0
    cmake --build --preset "$PRESET" --target "$APP_TARGET" >/dev/null 2>&1
    app=$SECONDS
    touch "$ROOT/$f"
    SECONDS=0
    cmake --build --preset "$PRESET" >/dev/null 2>&1
    printf '%-28s %9ds %9ds\n' "$f" "$app" "$SECONDS"
    [ "$app" -gt "$worst_app" ] && worst_app=$app
done

echo ""
if [ "$worst_app" -le 30 ]; then
    echo "==> inner-loop verdict: worst app rebuild ${worst_app}s <= 30s — header-only is FINE for"
    echo "    day-to-day editing. If the FULL column is much larger, that's the 5 executables each"
    echo "    recompiling the bodies — an app_core static lib (T1 Phase 2) would cut it, helping"
    echo "    full builds + CI, not the inner loop. Worth it only if full-build/CI time is a pain."
else
    echo "==> inner-loop verdict: worst app rebuild ${worst_app}s > 30s — even editing the app is"
    echo "    slow; extract that module's bodies into app_core (T1 Phase 2)."
fi
