#!/usr/bin/env bash
# Check that every route a controller registers with Drogon via ADD_METHOD_TO is
# also listed in Api::get_endpoints() (src/api/Endpoints.hpp) — the single source
# of truth that --print-routes and check-openapi-drift.sh read from.
#
# Without this, a hand-written ADD_METHOD_TO silently drifts out of the registry:
# the route works, but it's invisible to --print-routes AND to the OpenAPI drift
# check (which compares the registry to the spec, not the live controllers). The
# symmetric counterpart to scripts/check-openapi-drift.sh.
#
# Strategy: extract (METHOD, /path) tuples from every ADD_METHOD_TO in
# src/api/*.hpp and from get_endpoints(), normalize {param} placeholders, and
# assert each ADD_METHOD_TO tuple is present in the registry. (One direction:
# the registry legitimately has extra entries like /healthz, /metrics that are
# registered without ADD_METHOD_TO.)
set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
ENDPOINTS="$REPO/src/api/Endpoints.hpp"
API_DIR="$REPO/src/api"

[[ -f "$ENDPOINTS" ]] || {
    echo "error: $ENDPOINTS not found" >&2
    exit 2
}

norm_path() { sed -E 's/\{[^}]*\}/{}/g'; }

# Registry tuples: {"METHOD", "/path", ...} -> "METHOD /path"
registry="$(grep -hoE '\{"[A-Z]+",[[:space:]]*"[^"]+"' "$ENDPOINTS" |
    sed -E 's/\{"([A-Z]+)",[[:space:]]*"([^"]+)"/\1 \2/' | norm_path | sort -u)"

# Controller tuples: ADD_METHOD_TO(Ctrl::m, "/path", Method) -> "METHOD /path"
missing=0
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    path="$(printf '%s' "$line" | grep -oE '"[^"]+"' | head -1 | tr -d '"')"
    method="$(printf '%s' "$line" | sed -E 's/.*"[^"]*",[[:space:]]*([A-Za-z]+).*/\1/' | tr '[:lower:]' '[:upper:]')"
    [[ -z "$path" || -z "$method" ]] && continue
    tuple="$method $(printf '%s' "$path" | norm_path)"
    if ! grep -qxF "$tuple" <<<"$registry"; then
        echo "MISSING from Api::get_endpoints() (Endpoints.hpp): $tuple" >&2
        missing=1
    fi
done < <(grep -rhoE 'ADD_METHOD_TO\([^;]*\)' "$API_DIR"/*.hpp)

if [[ "$missing" -ne 0 ]]; then
    echo "" >&2
    echo "Add the missing route(s) to Api::get_endpoints() in src/api/Endpoints.hpp" >&2
    echo "(method + path), or --print-routes and the OpenAPI drift check won't see them." >&2
    exit 1
fi

# Versioning lint (docs/adr/0001-api-versioning.md): every /api route must be
# /api/v<N>/... . Catches a forker re-introducing an unversioned route. Pulls
# paths from both the controllers and the registry.
unversioned="$(
    {
        grep -rhoE 'ADD_METHOD_TO\([^,]+,[[:space:]]*"[^"]+"' "$API_DIR"/*.hpp |
            grep -oE '"[^"]+"$' | tr -d '"'
        grep -hoE '\{"[A-Z]+",[[:space:]]*"[^"]+"' "$ENDPOINTS" | grep -oE '"[^"]+"$' | tr -d '"'
    } | sort -u | grep -E '^/api/' | grep -vE '^/api/v[0-9]+/' || true
)"
if [[ -n "$unversioned" ]]; then
    echo "UNVERSIONED API route(s) — must be /api/v<N>/... (see docs/adr/0001-api-versioning.md):" >&2
    echo "$unversioned" | sed 's/^/  /' >&2
    exit 1
fi

echo "✓ routes registered: every ADD_METHOD_TO is in Api::get_endpoints(), all /api routes versioned"
