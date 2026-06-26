#!/usr/bin/env bash
# Walk config/config.json (or another config given as $1) and report which
# ${VAR} placeholders without a default expansion would be left empty given
# the current environment. Useful as a pre-flight check before `make up` or
# before running the binary natively — surfaces "DATABASE_PASSWORD is unset"
# instead of letting the app crash on a bad libpqxx connection string.
#
# A placeholder counts as "satisfied" if either:
#   - the env var is set (`printenv NAME` succeeds), OR
#   - the placeholder includes a default (`${VAR:-something}`).
#
# Exit status: 0 if every required placeholder is satisfied, 1 otherwise.
# Output a one-per-line list of unsatisfied placeholders (path · var).

set -euo pipefail

CONFIG="${1:-config/config.json}"
if [[ ! -f "$CONFIG" ]]; then
    echo "error: $CONFIG not found" >&2
    exit 2
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "error: jq is required (brew install jq / apt install jq)" >&2
    exit 2
fi

# jq emits "<json-path>\t<token>" for every ${VAR} or ${VAR:-default} found
# in any leaf string. The host-side loop then drops tokens that have a
# default and tokens whose env var is set, keeping only the unsatisfied ones.
# A single string can contain many tokens (e.g. database.primary builds the
# DSN from USER/PASSWORD/HOST/PORT), so we iterate per token, not per leaf.
missing=0
while IFS=$'\t' read -r path token; do
    [[ -z "$token" ]] && continue
    # Strip leading ${ and trailing }
    inner="${token#\$\{}"
    inner="${inner%\}}"
    if [[ "$inner" == *":-"* ]]; then
        continue   # ${VAR:-default}: never empty
    fi
    var="$inner"
    if printenv "$var" >/dev/null 2>&1; then
        continue
    fi
    printf '  %-46s %s\n' "$path" "$var"
    missing=$((missing + 1))
done < <(
    jq -r '
        paths(scalars) as $p
        | getpath($p) as $v
        | select($v | type == "string")
        | ($v | [scan("\\$\\{[A-Z_][A-Z0-9_]*(?::-[^}]*)?\\}")]) as $tokens
        | select($tokens | length > 0)
        | $tokens[] as $tok
        | "\($p | join("."))\t\($tok)"
    ' "$CONFIG"
)

if [[ $missing -eq 0 ]]; then
    echo "✓ all required env placeholders in $CONFIG are satisfied"
    exit 0
fi
echo "" >&2
echo "✗ $missing placeholder(s) in $CONFIG are unset and have no default." >&2
echo "  Set them in your shell or via direnv/.envrc before running the app." >&2
exit 1
