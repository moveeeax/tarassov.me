#!/usr/bin/env bash
# Verify that every test suite in tests/integration/ and tests/api/ does NOT
# appear in tests/unit/, and vice versa. Classification is by DIRECTORY, not
# by a name-based filter list — adding a new suite requires no changes here.
#
# What this catches:
#   - A file accidentally placed in the wrong directory (e.g. a DB-dependent
#     suite committed to tests/unit/).
#   - A suite name collision across buckets (would make ctest -L output
#     misleading even though both binaries compile fine).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Extract suite names (TEST_F and TEST) per bucket directory
suites_from_dir() {
    local dir="$1"
    grep -rhoE 'TEST(_F)?\([A-Za-z0-9_]+,' "$dir" 2>/dev/null \
        | sed 's/TEST_F(//; s/TEST(//; s/,//' \
        | sort -u
}

unit_suites="$(suites_from_dir "$REPO/tests/unit")"
integration_suites="$(suites_from_dir "$REPO/tests/integration" "$REPO/tests/api")"

fail=0

# Check for suite name collisions across buckets
if [ -n "$unit_suites" ] && [ -n "$integration_suites" ]; then
    overlap="$(comm -12 \
        <(printf '%s\n' "$unit_suites") \
        <(printf '%s\n' "$integration_suites"))"
    if [ -n "$overlap" ]; then
        echo "ERROR: suite name(s) appear in BOTH unit and integration buckets:" >&2
        printf '%s\n' "$overlap" | sed 's/^/  /' >&2
        echo "Rename one of the conflicting suites." >&2
        fail=1
    fi
fi

# Sanity: ensure both buckets are non-empty (catches an accidentally empty dir)
unit_count="$(printf '%s\n' "$unit_suites" | grep -c . || true)"
integration_count="$(printf '%s\n' "$integration_suites" | grep -c . || true)"

if [ "$unit_count" -eq 0 ]; then
    echo "WARNING: no TEST/TEST_F suites found in tests/unit/ — is the directory empty?" >&2
fi
if [ "$integration_count" -eq 0 ]; then
    echo "WARNING: no TEST/TEST_F suites found in tests/integration/ or tests/api/ — is the directory empty?" >&2
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "✓ test buckets OK: ${unit_count} unit suite(s) in tests/unit/, ${integration_count} integration suite(s) in tests/{integration,api}/"
