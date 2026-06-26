#!/usr/bin/env bash
# Run spectral lint against docs/openapi.yaml. Used by .pre-commit-config.yaml
# and `make lint-openapi`. Skips with a friendly note if neither npx nor docker
# is available — we don't want pre-commit to block on a missing optional tool.
set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
SPEC="$REPO/docs/openapi.yaml"
RULES="$REPO/.spectral.yaml"

if [[ ! -f "$SPEC" ]]; then
    echo "skip: $SPEC not found"
    exit 0
fi

# spectral exits non-zero on any error/warning by default. We pin --fail-severity
# to error so style warnings don't break commits — flip to "warn" once the spec
# is clean.
SEVERITY="${SPECTRAL_FAIL_SEVERITY:-error}"

if command -v spectral >/dev/null 2>&1; then
    exec spectral lint --ruleset "$RULES" --fail-severity "$SEVERITY" "$SPEC"
fi

if command -v npx >/dev/null 2>&1; then
    exec npx --yes -p @stoplight/spectral-cli spectral lint \
        --ruleset "$RULES" --fail-severity "$SEVERITY" "$SPEC"
fi

if command -v docker >/dev/null 2>&1; then
    exec docker run --rm -v "$REPO":/work -w /work \
        stoplight/spectral:6 lint \
            --ruleset .spectral.yaml --fail-severity "$SEVERITY" docs/openapi.yaml
fi

echo "skip: spectral, npx, and docker all unavailable — install one to enable OpenAPI lint"
exit 0
