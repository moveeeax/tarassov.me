#!/usr/bin/env bash
#
# One-time bootstrap: rename the template to your project identity.
#
# Usage:
#   ./scripts/init-project.sh <project-name> [registry] [domain]
#
# Example:
#   ./scripts/init-project.sh my-service docker.io/myorg example.org
#
# Arguments:
#   project-name  Kebab-case project name (e.g. my-service)
#   registry      Container registry (default: docker.io/library)
#   domain        Your host/domain — replaces the author's `tarassov.me` in
#                 badges, demo URLs and the SECURITY.md contact. Omit and it
#                 falls back to the placeholder `example.com`.
#
# What it does:
#   1. Replaces all hardcoded template names across the codebase
#   2. De-brands the author's host/domain (so you don't ship security@theirs)
#   3. Renames Helm chart directories
#   4. Updates project.env
#   5. Verifies no template/author token survived (fails loudly if one did)
#
set -euo pipefail

DRY_RUN=0
FORCE=0
NO_DEMO=0
ARGS=()
for arg in "$@"; do
    case "$arg" in
    --dry-run | -n) DRY_RUN=1 ;;
    --force | -f) FORCE=1 ;;
    --no-demo) NO_DEMO=1 ;;
    --help | -h)
        cat <<USAGE
Usage: $0 [--dry-run] [--force] <project-name> [registry] [domain]

  --dry-run, -n   Print every file that would be touched and the patterns that
                  would run, without modifying anything on disk.
  --force, -f     Run even if project.env shows the template has already been
                  initialised under a different name. Without this flag the
                  script asks for confirmation interactively.
  --no-demo       Strip the pedagogical flask-base reference material a real
                  fork doesn't ship: _reference/flask-base/ (~21 MB Python
                  source) and docs/PATTERNS-FROM-FLASK-BASE.md, AND the README
                  "Live demo" block (demo URL + public demo credentials). The
                  C++ app — auth, User/Role/Audit, jobs — is NOT a demo and is
                  kept. See REMOVING-THE-DEMO.md.

  domain          Your host/domain — replaces the author's tarassov.me in
                  badges / demo URLs / SECURITY.md (default: example.com).

Example:
  $0 my-service docker.io/myorg example.org
  $0 --dry-run my-service docker.io/myorg
USAGE
        exit 0
        ;;
    *) ARGS+=("$arg") ;;
    esac
done

if [[ ${#ARGS[@]} -lt 1 ]]; then
    echo "Usage: $0 [--dry-run] <project-name> [registry] [domain]"
    echo "Example: $0 my-service docker.io/myorg example.org"
    exit 1
fi

PROJECT_NAME="${ARGS[0]}"

# Registry / namespace for the published images. Take it as the 2nd arg, OR —
# so a forker can't silently inherit the template author's namespace (the
# hardcoded resert/... in CI, compose, release.yml) — ASK for it interactively
# when omitted. Falls back to the default only non-interactively (CI / dry-run).
if [[ ${#ARGS[@]} -ge 2 ]]; then
    REGISTRY="${ARGS[1]}"
elif [[ $DRY_RUN -eq 0 && -t 0 ]]; then
    printf 'Container registry / namespace for images (e.g. docker.io/myorg, ghcr.io/me) [docker.io/library]: '
    read -r REGISTRY
    REGISTRY="${REGISTRY:-docker.io/library}"
else
    REGISTRY="docker.io/library"
fi

# Bare org/namespace from the registry (docker.io/myorg -> myorg, ghcr.io/me -> me)
# — used to rebrand the GHCR builder-cache namespace so a fork's `make warm-cache`
# doesn't reach for the template author's `resert` namespace.
REGISTRY_ORG="${REGISTRY##*/}"

# The template author's host/domain appears as a GitLab namespace (badge +
# runbook links), the live-demo URLs, and the security contact in SECURITY.md.
# Left alone, a fork SHIPS THE AUTHOR'S security@ address and infra host. Take
# the fork's domain as the 3rd arg; otherwise neutralize to a placeholder that
# obviously needs replacing (better an obviously-fake address than the author's
# real one). The verification step at the end flags any survivor.
AUTHOR_HOST="tarassov.me"
if [[ ${#ARGS[@]} -ge 3 ]]; then
    NEW_HOST="${ARGS[2]}"
else
    NEW_HOST="example.com"
fi

# Derive snake_case from kebab-case: my-service -> my_service
PROJECT_SNAKE="${PROJECT_NAME//-/_}"

if [[ $DRY_RUN -eq 1 ]]; then
    echo "==> DRY RUN — no files will be modified"
fi
echo "==> Bootstrapping project"
echo "    PROJECT_NAME  = ${PROJECT_NAME}"
echo "    PROJECT_SNAKE = ${PROJECT_SNAKE}"
echo "    REGISTRY      = ${REGISTRY}"
echo "    DOMAIN        = ${NEW_HOST}$([[ "${NEW_HOST}" == "example.com" ]] && printf '  (placeholder — pass a 3rd arg to set yours)')"
echo ""

# ── Collect target files ────────────────────────────────────────
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ── Idempotency guard ───────────────────────────────────────────
# project.env is rewritten on success, so its current state tells us whether
# the template has already been re-named. If it has (and to something other
# than the requested name), running again would either be a no-op (same name)
# or would mangle the already-customised tree (different name). Stop and ask.
if [[ -f project.env ]]; then
    EXISTING_NAME="$(awk -F= '/^PROJECT_NAME=/{print $2}' project.env | tr -d '[:space:]')"
    # Default ships as PROJECT_NAME=cpp-api; treat that as "untouched template".
    if [[ -n "$EXISTING_NAME" && "$EXISTING_NAME" != "cpp-api" && "$EXISTING_NAME" != "cpp-rapid-rest-template" ]]; then
        if [[ "$EXISTING_NAME" == "$PROJECT_NAME" ]]; then
            echo "==> Project already initialised as '${EXISTING_NAME}' — nothing to do."
            exit 0
        fi
        if [[ $FORCE -eq 0 && $DRY_RUN -eq 0 ]]; then
            cat >&2 <<EOF
==> project.env shows this repo is already initialised as '${EXISTING_NAME}'.
    Re-running with a different name will not undo the previous rename and
    is almost never what you want.

    Pass --force to proceed anyway, or --dry-run to preview the next pass.
EOF
            exit 4
        fi
        if [[ $FORCE -eq 1 ]]; then
            echo "==> --force: continuing despite existing PROJECT_NAME='${EXISTING_NAME}'"
        fi
    fi
fi

# macOS ships bash 3.2 which lacks `mapfile`; use a portable while-read loop
# with a NUL-separated find to survive paths with spaces.
FILES=()
while IFS= read -r -d '' f; do
    FILES+=("$f")
done < <(
    find . -type f \
        \( -name '*.json' -o -name '*.yml' -o -name '*.yaml' \
        -o -name '*.tpl' -o -name 'CMakeLists.txt' \
        -o -name 'Dockerfile' -o -name 'Makefile' \
        -o -name '*.sh' -o -name '*.hpp' -o -name '*.cpp' \
        -o -name '*.md' -o -name '*.conf' -o -name '*.env' -o -name '.env.*' \
        -o -name '*.txt' -o -name '*.lock' -o -name '*.sample' \
        -o -name '.gitignore' -o -name '.gitlab-ci.yml' \
        -o -name 'Chart.yaml' \) \
        -not -path './.git/*' \
        -not -path './build/*' \
        -not -path './vcpkg_installed/*' \
        -not -path './vcpkg/*' \
        -not -path './frontend/node_modules/*' \
        -not -path './frontend/dist/*' \
        -not -path './_reference/*' \
        -not -path './docs/html/*' \
        -print0
)

echo "==> Found ${#FILES[@]} files to process"

# ── Replacements (order matters: specific → general) ────────────
# Each sed runs in-place across all target files.
# Patterns are ordered from most specific to most general to
# prevent partial matches.

declare -a PATTERNS=(
    # 1. Inconsistent helm image ref (if present)
    "s|resert/cpp-rapid-rest-app|${REGISTRY}/${PROJECT_NAME}|g"
    # 2. CI image name
    "s|resert/cpp-rapid-rest-template|${REGISTRY}/${PROJECT_NAME}|g"
    # 2b. GHCR builder-cache namespace (builder-cache.yml, Makefile GHCR default).
    #     Runs BEFORE the broad bare-owner rule (2a) below — otherwise 2a's
    #     `resert/` → placeholder rewrite would eat the `resert` inside a
    #     `ghcr.io/resert/…` URL first and leave this a no-op.
    "s|ghcr.io/resert|ghcr.io/${REGISTRY_ORG}|g"
    # 2a. Any other reference to the author's Docker Hub namespace (e.g. the
    #     `resert/…` prose in release.yml). Runs AFTER the specific image refs
    #     above so those rewrite to the fork's full ref first; this only mops up
    #     the bare owner so a fork can't inherit it.
    "s|resert/|your-registry/your-project/|g"
    # 3. Repo-level references
    "s|cpp-rapid-rest-template|${PROJECT_NAME}|g"
    # 4. CMake project name, binary names, Dockerfile
    "s|cpp_api_template|${PROJECT_SNAKE}|g"
    # 5. Bench service name
    "s|cpp_api_bench|${PROJECT_SNAKE}_bench|g"
    # 6. OTel service name
    "s|cpp_api_service|${PROJECT_SNAKE}_service|g"
    # 7. Worker OTel service name
    "s|cpp_worker_service|${PROJECT_SNAKE}_worker_service|g"
    # 8. Helm maintainer team
    "s|cpp-api-team|${PROJECT_NAME}-team|g"
    # 9. Helm worker chart name
    "s|cpp-worker|${PROJECT_NAME}-worker|g"
    # 10. Helm chart, K8s, ingress
    "s|cpp-api|${PROJECT_NAME}|g"
    # 11. Worker logger name
    "s|cpp_worker|${PROJECT_SNAKE}_worker|g"
    # 12. Logger, containers, general snake_case
    "s|cpp_api|${PROJECT_SNAKE}|g"
    # 13. Kafka client ID
    "s|cpp_producer|${PROJECT_SNAKE}_producer|g"
    # 14. De-brand the author's host/domain (gitlab namespace in badge + runbook
    #     links, *.demo.<host> URLs, security@<host>). Runs last so it can't
    #     interfere with the project-name patterns above.
    "s|${AUTHOR_HOST//./\\.}|${NEW_HOST}|g"
    # 15. Author's real DEMO / INFRA identifiers. These are the live deployment
    #     source's working defaults — a fork must NOT inherit them. Rewrite to
    #     loud placeholders so the fork's deploy-demo.sh / values fail closed
    #     until the new owner fills them in. The verifier below flags survivors.
    #     - public ingress IP (helm/cpp-env/values-demo.yaml)
    "s|46\\.225\\.37\\.165|YOUR_INGRESS_IP|g"
    #     - kube context (scripts/deploy-demo.sh)
    "s|admin@talos-nbg1|YOUR_KUBE_CONTEXT|g"
    #     - demo admin password (README, deploy-demo.sh, helm/cpp-env/values.yaml)
    "s|DemoAdmin-2026|change-me-demo-pass|g"
    #     - author's personal email, if it ever appears as a template default.
    #       Pattern 14 has already turned tarassov.me into ${NEW_HOST}, so match
    #       the post-rewrite form (michael@${NEW_HOST}) → you@${NEW_HOST}.
    "s|michael@${NEW_HOST//./\\.}|you@${NEW_HOST}|g"
)

# GNU sed accepts `-i`, BSD/macOS sed requires `-i ''` (an explicit backup
# suffix argument). Detect once and pick the right invocation.
if sed --version >/dev/null 2>&1; then
    SED_INPLACE=(sed -i)
else
    SED_INPLACE=(sed -i '')
fi

if [[ $DRY_RUN -eq 1 ]]; then
    echo "==> Patterns that would run:"
    for pattern in "${PATTERNS[@]}"; do
        echo "    sed -i \"$pattern\" <files>"
    done
    echo ""
    echo "==> Files that would be touched (matches at least one pattern):"
    for f in "${FILES[@]}"; do
        if grep -qE 'cpp-rapid-rest-template|cpp_api_template|cpp-api|cpp_api|cpp-worker|cpp_worker|cpp_producer|cpp_api_bench|cpp_api_service|cpp_worker_service|cpp-api-team|resert/|tarassov\.me|46\.225\.37\.165|admin@talos-nbg1|DemoAdmin-2026' "$f" 2>/dev/null; then
            echo "    $f"
        fi
    done
    echo ""
    if [[ -d "helm/cpp-api" && "cpp-api" != "${PROJECT_NAME}" ]]; then
        echo "==> Would rename helm/cpp-api -> helm/${PROJECT_NAME}"
    fi
    if [[ -d "helm/cpp-worker" && "cpp-worker" != "${PROJECT_NAME}-worker" ]]; then
        echo "==> Would rename helm/cpp-worker -> helm/${PROJECT_NAME}-worker"
    fi
    echo "==> Would write project.env (PROJECT_NAME=${PROJECT_NAME}, REGISTRY=${REGISTRY}, GHCR_ORG=${REGISTRY_ORG})"
    if [[ $NO_DEMO -eq 1 ]]; then
        for p in "_reference" "docs/PATTERNS-FROM-FLASK-BASE.md"; do
            [[ -e "$ROOT/$p" ]] && echo "==> Would remove reference material: $p"
        done
        if [[ -f "$ROOT/README.md" ]] && grep -q 'init-project:live-demo:start' "$ROOT/README.md"; then
            echo "==> Would strip the README 'Live demo' block + its Contents entry"
        fi
    fi
    echo ""
    echo "DRY RUN complete. Re-run without --dry-run to apply."
    exit 0
fi

for pattern in "${PATTERNS[@]}"; do
    "${SED_INPLACE[@]}" "$pattern" "${FILES[@]}"
done

echo "==> Text replacements complete"

# ── Rename Helm chart directories ───────────────────────────────
if [[ -d "helm/cpp-api" && "cpp-api" != "${PROJECT_NAME}" ]]; then
    mv "helm/cpp-api" "helm/${PROJECT_NAME}"
    echo "==> Renamed helm/cpp-api -> helm/${PROJECT_NAME}"
fi

if [[ -d "helm/cpp-worker" && "cpp-worker" != "${PROJECT_NAME}-worker" ]]; then
    mv "helm/cpp-worker" "helm/${PROJECT_NAME}-worker"
    echo "==> Renamed helm/cpp-worker -> helm/${PROJECT_NAME}-worker"
fi

# ── Update project.env ──────────────────────────────────────────
cat >project.env <<EOF
PROJECT_NAME=${PROJECT_NAME}
REGISTRY=${REGISTRY}
GHCR_ORG=${REGISTRY_ORG}
EOF
echo "==> Updated project.env"

echo ""

# ── Verify the rename took (instead of telling YOU to grep) ──────────────────
# An INDEPENDENT, broad scan — deliberately NOT the same file set the
# replacement used, so it also catches files that set might miss. Only the
# UNAMBIGUOUS author/template tokens are flagged: bare "cpp-api"/"cpp_api" are
# excluded so a fork named e.g. "cpp-api-gateway" doesn't trip it. Vendor /
# build / generated dirs and this script itself (which necessarily contains the
# tokens) are skipped. -I skips binaries (portable across GNU/BSD grep).
# Strip the flask-base reference material (opt-in). It exists to teach the
# parity mapping; a shipped fork doesn't need ~21 MB of Python or the pattern
# doc. The actual app (auth/User/Role/Audit/jobs) is NOT touched.
if [[ $NO_DEMO -eq 1 ]]; then
    DEMO_PATHS=("_reference" "docs/PATTERNS-FROM-FLASK-BASE.md")
    for p in "${DEMO_PATHS[@]}"; do
        [[ -e "$ROOT/$p" ]] || continue
        if [[ $DRY_RUN -eq 1 ]]; then
            echo "==> [dry-run] would remove $p"
        else
            echo "==> Removing reference material: $p"
            rm -rf "${ROOT:?}/$p"
        fi
    done
    if [[ $DRY_RUN -eq 0 ]]; then
        # Drop the now-dangling doc link so check-doc-links / readers don't trip.
        grep -rIlZ "PATTERNS-FROM-FLASK-BASE" "$ROOT" \
            --exclude-dir=.git --exclude-dir=_reference 2>/dev/null |
            while IFS= read -r -d '' f; do
                sed -i.bak '/PATTERNS-FROM-FLASK-BASE/d' "$f" && rm -f "$f.bak"
            done || true

        # Strip the README "Live demo" section (between the region markers) and
        # its Contents entry. A fork doesn't run the author's demo, so the block
        # — which carries the demo URL + public demo credentials — has no place
        # in a fork's README. The markers were rewritten by the host pattern
        # above (they contain no host literal, so they survive verbatim).
        if [[ -f "$ROOT/README.md" ]]; then
            sed -i.bak \
                -e '/init-project:live-demo:start/,/init-project:live-demo:end/d' \
                -e '/init-project:live-demo:toc/d' \
                "$ROOT/README.md" && rm -f "$ROOT/README.md.bak"
            echo "==> Stripped README 'Live demo' block (--no-demo)"
        fi
    fi
fi

echo "==> Verifying rename completeness"
LEFTOVER_RE='cpp-rapid-rest-template|cpp_api_template|cpp_api_bench|cpp_api_service|cpp_worker_service|cpp_producer|cpp-api-team|resert/|ghcr\.io/resert|tarassov\.me|46\.225\.37\.165|admin@talos-nbg1|DemoAdmin-2026|michael@tarassov\.me'
leftovers="$(grep -rInE "$LEFTOVER_RE" . \
    --exclude="$(basename "$0")" \
    --exclude=.git \
    --exclude-dir=.git --exclude-dir=build \
    --exclude-dir=vcpkg_installed --exclude-dir=vcpkg \
    --exclude-dir=node_modules --exclude-dir=dist \
    --exclude-dir=html --exclude-dir=_reference 2>/dev/null || true)"

if [[ -n "$leftovers" ]]; then
    echo "" >&2
    echo "==> INCOMPLETE: template/author tokens still present:" >&2
    printf '%s\n' "$leftovers" | sed 's/^/  /' >&2
    echo "" >&2
    echo "A surviving security@/demo host means your fork would ship the template" >&2
    echo "author's contact. Fix the files above (or pass a domain as the 3rd arg" >&2
    echo "and re-run), then commit." >&2
    exit 1
fi

echo "==> Verified: no template/author tokens remain."
echo ""
echo "Done. Review the full diff with: git diff"
