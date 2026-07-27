#!/usr/bin/env bash
# Guard the three-way sync of the public-site surface. The blog SSR pages are
# served by the BACKEND through TWO nginx configs (dev frontend/nginx.conf and
# the prod helm configmap) and carry a CSP that also lives in the backend
# controller. These copies drifting apart has produced real outages:
#   - a stray API CSP (default-src 'none') bricked every post page (PR #9)
#   - a missing allowlist entry 401'd the whole SSR surface (v2.0.0 deploy)
# This check makes those classes of drift fail CI instead of prod.
set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
DEV_NGINX="$REPO/frontend/nginx.conf"
HELM_NGINX="$REPO/helm/tarassov-me-frontend/templates/configmap.yaml"
CONTROLLER="$REPO/src/api/PublicPagesController.hpp"
STRINGS="$REPO/src/utils/Strings.hpp"
FAIL=0

err() {
    echo "✗ $*" >&2
    FAIL=1
}

# ── 1. The public-site CSP: identical in dev nginx, helm nginx, controller ──
# Each nginx config carries TWO policies: the strict admin-SPA one and the
# public-site one. The public one is the one allowing Google Fonts — and every
# public copy inside a file must itself be identical (location / vs /blog/).
#
# The helm copies additionally splice the configured upload origin into img-src
# ({{ with .Values.uploads.publicOrigin }}), because post images served from an
# S3/CDN host are blocked outright by a bare `img-src 'self' data:`. That
# insertion is the ONE sanctioned difference: it is stripped before comparing,
# so the three copies must still agree on every other directive.
UPLOAD_ORIGIN_TPL='\{\{ with \.Values\.uploads\.publicOrigin \}\} \{\{ \. \}\}\{\{ end \}\}'
extract_nginx_csp() {
    local uniq
    uniq="$(grep -oE 'Content-Security-Policy "[^"]*fonts\.googleapis[^"]*"' "$1" |
        sed -E "s/^Content-Security-Policy \"//; s/\"$//; s/$UPLOAD_ORIGIN_TPL//g" | sort -u)"
    if [[ "$(printf '%s\n' "$uniq" | grep -c .)" -ne 1 ]]; then
        err "$1: the public-site CSP copies inside the file differ from each other"
    fi
    printf '%s' "$uniq" | head -1
}
DEV_CSP="$(extract_nginx_csp "$DEV_NGINX")"
HELM_CSP="$(extract_nginx_csp "$HELM_NGINX")"
# Reassemble the C++ adjacent string literals of kPublicSiteCsp. The literal
# spans several lines, each a quoted fragment; the statement ends on the line
# whose closing quote is followed by ';'.
CPP_CSP="$(awk '/kPublicSiteCsp =/{f=1} f{print; if (/"[[:space:]]*;[[:space:]]*$/) exit}' "$CONTROLLER" |
    grep -oE '"[^"]*"' | sed -E 's/^"//; s/"$//' | tr -d '\n')"

[[ -n "$DEV_CSP" ]] || err "no public-site CSP found in $DEV_NGINX"
[[ -n "$HELM_CSP" ]] || err "no public-site CSP found in $HELM_NGINX"
[[ -n "$CPP_CSP" ]] || err "no kPublicSiteCsp found in $CONTROLLER"
[[ "$DEV_CSP" == "$HELM_CSP" ]] || err "CSP drift: dev nginx.conf != helm configmap"
[[ "$DEV_CSP" == "$CPP_CSP" ]] || err "CSP drift: nginx configs != PublicPagesController kPublicSiteCsp"

# ── 2. The blog proxy surface exists in BOTH nginx configs, same shape ──────
for f in "$DEV_NGINX" "$HELM_NGINX"; do
    grep -qE 'location \^~ /blog/' "$f" || err "$f: missing 'location ^~ /blog/' proxy block"
    grep -qE 'location = /sitemap\.xml' "$f" || err "$f: missing 'location = /sitemap.xml' proxy block"
    grep -qE 'location = /blog\s+\{' "$f" || err "$f: missing 'location = /blog' redirect (the :8080 port-leak guard)"
    grep -qE 'proxy_hide_header Content-Security-Policy' "$f" ||
        err "$f: missing 'proxy_hide_header Content-Security-Policy' (API CSP would brick SSR pages)"
    grep -qE 'absolute_redirect off' "$f" || err "$f: missing 'absolute_redirect off' (redirects leak the pod port)"
    grep -qE 'client_max_body_size' "$f" || err "$f: missing client_max_body_size (uploads 413 at 1m default)"
done

# ── 3. Public-paths allowlist covers the SSR routes EVERYWHERE it's defined ──
# Sources: code default, both shipped config files, helm chart values.
#
# EVERY route the anonymous public surface needs, not just the two page URLs:
# the SSR pages fetch /api/v1/public/posts{,/<slug>} and the contact form POSTs
# /api/v1/public/contact, so a copy that admits the pages but omits their data
# endpoints 401s the page content. That is exactly how the code default drifted
# (it listed only the page URLs while all three shipped copies listed all five).
# Add a route here whenever one is added to Strings.hpp / the configs / values.
PUBLIC_ROUTES=(
    "/sitemap.xml"
    "/blog/\*"
    "/api/v1/public/posts"
    "/api/v1/public/posts/\*"
    "/api/v1/public/contact"
    "/uploads/\*"
)
for route in "${PUBLIC_ROUTES[@]}"; do
    grep -qE "$route" "$STRINGS" || err "src/utils/Strings.hpp kDefaultPublicPathsCsv missing $route"
    for cfgf in "$REPO/config/config.json" "$REPO/config/config.production.json"; do
        grep -qE "\"public_paths\".*$route" "$cfgf" || err "$cfgf public_paths missing $route"
    done
    grep -qE "publicPaths:.*$route" "$REPO/helm/tarassov-me/values.yaml" ||
        err "helm/tarassov-me/values.yaml api.publicPaths missing $route"
done

if [[ "$FAIL" -ne 0 ]]; then
    echo "" >&2
    echo "Public-surface sync check failed — fix ALL copies (dev nginx, helm configmap," >&2
    echo "PublicPagesController, config files, chart values), not just one." >&2
    exit 1
fi
echo "✓ public surface in sync: CSP x3 identical, nginx blocks present in both configs," \
    "allowlist covers ${#PUBLIC_ROUTES[@]} public routes x4 sources"
