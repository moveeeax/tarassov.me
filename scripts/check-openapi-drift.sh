#!/usr/bin/env bash
# Check that every (method, path) tuple registered in Api::get_endpoints() is
# documented in docs/openapi.yaml — and vice versa. Catches "I added GET
# /api/foo and forgot to update the spec" *and* "I changed POST → PUT in
# code but the spec still says POST" drift, both of which slipped past the
# old path-only comparison.
#
# Strategy: extract (METHOD, /path) tuples from both sources, normalize
# {id}-style params, sort, diff.

set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CODE="$REPO/src/api/Endpoints.hpp"
SPEC="$REPO/docs/openapi.yaml"

if [[ ! -f "$CODE" || ! -f "$SPEC" ]]; then
    echo "error: expected $CODE and $SPEC to exist" >&2
    exit 2
fi

# Normalize: collapse {id}/{name}/etc. to literal :id so the two sources
# agree on placeholder spelling. Lower-case the method to keep comparison
# simple — the registry uses upper-case, the spec uses lower-case.
normalize() {
    sed -E 's/\{[^}]+\}/:id/g' \
      | tr '[:upper:]' '[:lower:]' \
      | sort -u
}

# CODE: each registry row is `{"GET", "/api/foo", "..."},` — emit "get /api/foo".
CODE_TUPLES=$(
    grep -oE '"[A-Z]+",[[:space:]]*"/[^"]*"' "$CODE" \
        | sed -E 's/"([A-Z]+)",[[:space:]]*"(\/[^"]*)"/\1 \2/' \
        | normalize
)

# SPEC: walk paths: → /path → method. Two-space indent for top-level paths
# and four-space indent for the operation. Avoid python+yaml so this still
# runs on minimal CI images that only have awk.
SPEC_TUPLES=$(
    awk '
        /^paths:/ { in_paths = 1; next }
        /^[^[:space:]]/ && in_paths { in_paths = 0 }
        in_paths && /^  \/[^:]*:/ {
            line = $0
            sub(/:.*$/, "", line)
            sub(/^  /, "", line)
            current_path = line
            next
        }
        in_paths && current_path != "" && /^    (get|post|put|delete|patch|options|head):/ {
            method = $0
            sub(/^    /, "", method)
            sub(/:.*$/, "", method)
            print method, current_path
        }
    ' "$SPEC" | normalize
)

# Compare tuples.
only_code=$(comm -23 <(printf '%s\n' "$CODE_TUPLES") <(printf '%s\n' "$SPEC_TUPLES") || true)
only_spec=$(comm -13 <(printf '%s\n' "$CODE_TUPLES") <(printf '%s\n' "$SPEC_TUPLES") || true)

status=0

if [[ -n "$only_code" ]]; then
    echo "✗ (method, path) tuples in Api::get_endpoints() but missing from openapi.yaml:"
    printf '%s\n' "$only_code" | sed 's/^/    /'
    status=1
fi

if [[ -n "$only_spec" ]]; then
    echo "✗ (method, path) tuples in openapi.yaml but not registered in Api::get_endpoints():"
    printf '%s\n' "$only_spec" | sed 's/^/    /'
    status=1
fi

if [[ $status -eq 0 ]]; then
    echo "✓ OpenAPI spec is in sync with Api::get_endpoints() (method + path)"
fi
exit $status
