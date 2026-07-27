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

# Extract suite names (TEST_F and TEST) from one or more bucket directories.
# Greps "$@", not just "$1": the integration bucket spans tests/integration AND
# tests/api (CMake globs both into a single labeled binary). Reading only the
# first argument silently dropped every tests/api suite from the collision
# check below — i.e. the guard didn't run for a quarter of the bucket.
suites_from_dirs() {
    grep -rhoE 'TEST(_F)?\([A-Za-z0-9_]+,' "$@" 2>/dev/null \
        | sed 's/TEST_F(//; s/TEST(//; s/,//' \
        | sort -u
}

# Count non-empty lines of a suite list ("" -> 0).
count_suites() {
    printf '%s\n' "$1" | grep -c . || true
}

unit_suites="$(suites_from_dirs "$REPO/tests/unit")"
integration_dir_suites="$(suites_from_dirs "$REPO/tests/integration")"
api_dir_suites="$(suites_from_dirs "$REPO/tests/api")"
integration_suites="$(printf '%s\n%s\n' "$integration_dir_suites" "$api_dir_suites" |
    sort -u | grep -v '^$' || true)"

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

# Sanity: ensure every bucket DIRECTORY is non-empty (catches an accidentally
# empty dir). Checked per directory, not on the merged integration list: a
# populated tests/integration would otherwise hide an empty tests/api.
unit_count="$(count_suites "$unit_suites")"
integration_count="$(count_suites "$integration_suites")"

if [ "$unit_count" -eq 0 ]; then
    echo "WARNING: no TEST/TEST_F suites found in tests/unit/ — is the directory empty?" >&2
fi
if [ "$(count_suites "$integration_dir_suites")" -eq 0 ]; then
    echo "WARNING: no TEST/TEST_F suites found in tests/integration/ — is the directory empty?" >&2
fi
if [ "$(count_suites "$api_dir_suites")" -eq 0 ]; then
    echo "WARNING: no TEST/TEST_F suites found in tests/api/ — is the directory empty?" >&2
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "✓ test buckets OK: ${unit_count} unit suite(s) in tests/unit/, ${integration_count} integration suite(s) in tests/{integration,api}/"
