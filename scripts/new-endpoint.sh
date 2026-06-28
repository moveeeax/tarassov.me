#!/usr/bin/env bash
#
# Scaffold a new Drogon controller and wire it into Api.hpp + get_endpoints().
#
# Usage:
#   ./scripts/new-endpoint.sh [--with-test] [--no-openapi]
#                             <ControllerName> <method> <path>
#
# Example:
#   ./scripts/new-endpoint.sh OrdersController Get  /api/v1/orders
#   ./scripts/new-endpoint.sh --with-test OrdersController Post /api/v1/orders
#   ./scripts/new-endpoint.sh --with-test OrdersController Get /api/v1/orders/{1}
#
# Flags:
#   --with-test       Also create tests/api/test_<lower>.cpp with a smoke test.
#   --no-openapi      Do NOT patch docs/openapi.yaml — just print the stub.
#                     (By default the spec IS patched, so a bare run keeps the
#                      drift checker green instead of going red.)
#   --patch-openapi   Deprecated no-op kept for back-compat (now the default).
#
# What it does:
#   1. Creates src/api/<ControllerName>.hpp with one handler.
#      - Path placeholders ({1},{2},...) become trailing `const std::string&`
#        params (Drogon binds one per placeholder). A single {1} gets a uuid
#        guard. Mutating verbs (Post/Put/Delete/Patch) get API_REQUIRE_ADMIN.
#   2. Adds `#include "api/<ControllerName>.hpp"` to src/api/Api.hpp.
#   3. Inserts a row into get_endpoints() (src/api/Endpoints.hpp).
#   4. Patches docs/openapi.yaml (unless --no-openapi).
#   5. (Optional) Creates tests/api/test_<lower>.cpp.
#
# Safe to re-run for the same controller with a different method — it will
# append a new ADD_METHOD_TO line + handler stub instead of overwriting.
#
set -euo pipefail

WITH_TEST=0
PATCH_OPENAPI=1 # default ON; a bare run must not introduce OpenAPI drift.
ARGS=()
for arg in "$@"; do
    case "$arg" in
    --with-test) WITH_TEST=1 ;;
    --no-openapi) PATCH_OPENAPI=0 ;;
    --patch-openapi) PATCH_OPENAPI=1 ;; # deprecated: now the default.
    --help | -h)
        sed -n '2,30p' "$0"
        exit 0
        ;;
    *) ARGS+=("$arg") ;;
    esac
done

if [[ ${#ARGS[@]} -lt 3 ]]; then
    echo "Usage: $0 [--with-test] [--no-openapi] <ControllerName> <HttpMethod> <path>" >&2
    echo "Example: $0 OrdersController Get /api/v1/orders" >&2
    exit 1
fi

NAME="${ARGS[0]}"       # e.g. OrdersController
METHOD_RAW="${ARGS[1]}" # Get | Post | Put | Delete | Patch
ROUTE="${ARGS[2]}"      # e.g. /api/v1/orders

# Enforce the versioning convention (docs/adr/0001-api-versioning.md): an API
# route must be /api/v<N>/... . Hard-reject (don't auto-prefix — that risks
# /api/v1/api/v1/orders). Bare infra/probe routes are allowed unversioned.
case "$ROUTE" in
/ | /healthz | /ready | /health | /metrics) ;; # infra routes stay unversioned
/api/v[0-9]*/*) ;;                             # correctly versioned
/api/*)
    echo "ERROR: API route '$ROUTE' must be versioned as /api/v<N>/... (e.g. /api/v1/orders)." >&2
    echo "       See docs/adr/0001-api-versioning.md." >&2
    exit 2
    ;;
esac

# Normalize method to Drogon's enum form (first letter upper, rest lower).
METHOD="$(printf '%s' "$METHOD_RAW" | tr '[:upper:]' '[:lower:]')"
METHOD="$(printf '%s%s' "$(printf '%s' "$METHOD" | cut -c1 | tr '[:lower:]' '[:upper:]')" \
    "$(printf '%s' "$METHOD" | cut -c2-)")"

case "$METHOD" in
Get | Post | Put | Delete | Patch) ;;
*)
    echo "ERROR: method must be Get|Post|Put|Delete|Patch, got '$METHOD_RAW'" >&2
    exit 2
    ;;
esac

# Derive a handler name: OrdersController+Get → listOrders; Post → createOrders.
HANDLER_STEM="$(printf '%s' "$NAME" | sed -E 's/Controller$//')"
case "$METHOD" in
Get) HANDLER="list${HANDLER_STEM}" ;;
Post) HANDLER="create$(printf '%s' "$HANDLER_STEM" | sed 's/s$//')" ;;
Put) HANDLER="update$(printf '%s' "$HANDLER_STEM" | sed 's/s$//')" ;;
Delete) HANDLER="delete$(printf '%s' "$HANDLER_STEM" | sed 's/s$//')" ;;
Patch) HANDLER="patch$(printf '%s' "$HANDLER_STEM" | sed 's/s$//')" ;;
esac

# ── Path placeholders → handler signature ───────────────────────────────
# Drogon binds each {N} in the route to one trailing `const std::string&`
# argument, in order. Count the placeholders so the generated handler's
# signature (and the test call) actually matches what Drogon will invoke —
# the old scaffold always emitted the zero-param signature, which silently
# failed to bind for path-param routes.
# `grep -c` returns 1 (and an empty stdout under pipefail) when there's no
# match, which would trip `set -e` — guard with `|| true`.
NUM_PARAMS="$(printf '%s' "$ROUTE" | grep -oE '\{[0-9]+\}' | wc -l | tr -d '[:space:]' || true)"
NUM_PARAMS="${NUM_PARAMS:-0}"

SIG_PARAMS="" # e.g. ", const std::string& p1, const std::string& p2"
TEST_ARGS=""  # e.g. , "p1-value", "p2-value"
i=1
while [[ "$i" -le "$NUM_PARAMS" ]]; do
    SIG_PARAMS="${SIG_PARAMS}, const std::string& p${i}"
    TEST_ARGS="${TEST_ARGS}, \"00000000-0000-0000-0000-000000000000\""
    i=$((i + 1))
done

# Mutating verbs are admin-guarded by default: AUTH_MODE=none makes every
# route public otherwise, so a fresh POST/PUT/DELETE/PATCH would ship as an
# unauthenticated mutation. Get stays open.
IS_MUTATION=0
case "$METHOD" in
Post | Put | Delete | Patch) IS_MUTATION=1 ;;
esac

# Build the handler body once so the create-file (heredoc) and extend-file
# (awk) paths emit identical code. Indentation is 8 spaces (inside class).
build_handler_body() {
    if [[ "$IS_MUTATION" -eq 1 ]]; then
        printf '        // TODO: relax/justify if this route is intentionally public\n'
        printf '        API_REQUIRE_ADMIN(req, callback);\n'
    fi
    if [[ "$NUM_PARAMS" -eq 1 ]]; then
        printf '        if (!is_valid_uuid(p1)) {\n'
        printf '            callback(ErrorResponse::bad_request("invalid_id"));\n'
        printf '            return;\n'
        printf '        }\n'
    fi
    printf '        json response = {{"message", "%s::%s — TODO"}};\n' "$NAME" "$HANDLER"
    # POST conventionally returns 201; keep the stub and its test in agreement.
    if [[ "$METHOD" = "Post" ]]; then
        printf '        callback(Response::created(response));\n'
    else
        printf '        callback(Response::ok(response));\n'
    fi
}
HANDLER_BODY="$(build_handler_body)"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILE="$ROOT/src/api/${NAME}.hpp"
API_HPP="$ROOT/src/api/Api.hpp"             # controller #include lives here
ENDPOINTS_HPP="$ROOT/src/api/Endpoints.hpp" # the route registry moved here

# ── 1. Create or extend the controller file ─────────────────────────────
if [[ ! -f "$FILE" ]]; then
    cat >"$FILE" <<EOF
/**
 * @file ${NAME}.hpp
 * @brief ${NAME} — generated by scripts/new-endpoint.sh.
 */

#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

// Controllers must NOT include api/Api.hpp (it includes the controllers — that
// would cycle). Pull only the small shared helpers.
#include "api/Guards.hpp"        // API_REQUIRE_ADMIN / API_REQUIRE_PRINCIPAL
#include "api/RequestUtils.hpp"  // parse_int / parse_page_params / is_valid_uuid
#include "api/Validation.hpp"
#include "utils/ErrorResponse.hpp"  // Response::ok / ErrorResponse::*

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class ${NAME} : public HttpController<${NAME}> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(${NAME}::${HANDLER}, "${ROUTE}", ${METHOD});
    METHOD_LIST_END

    void ${HANDLER}(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback${SIG_PARAMS}) {
${HANDLER_BODY}
    }
};

}  // namespace Api
EOF
    echo "==> Created $FILE"
else
    # Append a new handler to the existing file. We look for METHOD_LIST_END
    # and METHOD_LIST_BEGIN to insert the ADD_METHOD_TO line + handler body.
    if grep -q "ADD_METHOD_TO(${NAME}::${HANDLER}," "$FILE"; then
        echo "==> ${NAME}::${HANDLER} already wired in $FILE, skipping file edit"
    else
        # Insert ADD_METHOD_TO before METHOD_LIST_END
        awk -v line="    ADD_METHOD_TO(${NAME}::${HANDLER}, \"${ROUTE}\", ${METHOD});" '
            /METHOD_LIST_END/ && !done { print line; done=1 } { print }
        ' "$FILE" >"$FILE.tmp" && mv "$FILE.tmp" "$FILE"

        # Append handler body before the final closing brace of the class.
        # We match the class's single "};" followed by "}  // namespace Api".
        # The body (guard + uuid check + stub) is read from a temp file so the
        # multi-line block survives intact — awk -v would eat the newlines.
        BODY_TMP="$(mktemp)"
        printf '%s\n' "$HANDLER_BODY" >"$BODY_TMP"
        awk -v handler="$HANDLER" -v sig="$SIG_PARAMS" -v bodyfile="$BODY_TMP" '
            /^\};/ && !done {
                print "";
                print "    void " handler "(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback" sig ") {";
                while ((getline bline < bodyfile) > 0) print bline;
                close(bodyfile);
                print "    }";
                done=1;
            }
            { print }
        ' "$FILE" >"$FILE.tmp" && mv "$FILE.tmp" "$FILE"
        rm -f "$BODY_TMP"

        echo "==> Extended $FILE with ${HANDLER}"
    fi
fi

# ── 2. Wire include into src/api/Api.hpp ────────────────────────────────
if ! grep -q "#include \"api/${NAME}.hpp\"" "$API_HPP"; then
    awk -v inc="#include \"api/${NAME}.hpp\"" '
        /#include "api\/JobsController.hpp"/ {
            print; print inc; printed=1; next
        }
        { print }
    ' "$API_HPP" >"$API_HPP.tmp" && mv "$API_HPP.tmp" "$API_HPP"
    echo "==> Added #include for $NAME to $API_HPP"
fi

# ── 3. Insert into get_endpoints() (now in Endpoints.hpp) ───────────────
METHOD_UPPER="$(printf '%s' "$METHOD_RAW" | tr '[:lower:]' '[:upper:]')"
ROW="        {\"${METHOD_UPPER}\", \"${ROUTE}\", \"${NAME}::${HANDLER}\"},"
if grep -qF "\"${ROUTE}\", \"${NAME}::${HANDLER}\"" "$ENDPOINTS_HPP"; then
    echo "==> get_endpoints() already has ${METHOD_UPPER} ${ROUTE}"
else
    awk -v row="$ROW" '
        /^    };$/ && !done && seen_endpoints {
            print row; done=1
        }
        /inline const std::vector<EndpointInfo>& get_endpoints/ { seen_endpoints=1 }
        { print }
    ' "$ENDPOINTS_HPP" >"$ENDPOINTS_HPP.tmp" && mv "$ENDPOINTS_HPP.tmp" "$ENDPOINTS_HPP"
    # Verify the row actually landed — a moved anchor used to leave this a
    # silent no-op while still printing success.
    if grep -qF "\"${ROUTE}\", \"${NAME}::${HANDLER}\"" "$ENDPOINTS_HPP"; then
        echo "==> Added get_endpoints() row for ${METHOD_UPPER} ${ROUTE}"
    else
        echo "ERROR: failed to insert get_endpoints() row — add it to src/api/Endpoints.hpp by hand" >&2
        exit 1
    fi
fi

# ── 4. OpenAPI stub: print, or patch the spec in place ──────────────────
METHOD_LOWER="$(printf '%s' "$METHOD_RAW" | tr '[:upper:]' '[:lower:]')"
SPEC="$ROOT/docs/openapi.yaml"

# Emit a `parameters:` block (6-space indent, inside the operation) for each
# {N} placeholder so the spec documents the path params Drogon binds.
emit_openapi_params() {
    [[ "$NUM_PARAMS" -eq 0 ]] && return 0
    printf '      parameters:\n'
    for ph in $(printf '%s' "$ROUTE" | grep -oE '\{[0-9]+\}' | tr -d '{}'); do
        printf '        - { name: "%s", in: path, required: true, schema: { type: string } }\n' "$ph"
    done
}

if [[ $PATCH_OPENAPI -eq 1 && -f "$SPEC" ]]; then
    if grep -qE "^  ${ROUTE//\//\\/}:" "$SPEC"; then
        # Path already in spec — append the method only if it isn't there yet.
        # This is a tight match: the operation must appear inside the existing
        # path block. We extract the block, see if the method is present, and
        # if not, insert the operation before the next top-level path.
        if ! awk -v p="$ROUTE" -v m="$METHOD_LOWER" '
            $0 == "  " p ":" { in_block=1; next }
            in_block && /^  \/[^:]*:/ { in_block=0 }
            in_block && $0 ~ ("^    " m ":") { found=1 }
            END { exit found ? 0 : 1 }
        ' "$SPEC"; then
            # Insert the operation just after the path declaration.
            PARAMS_TMP="$(mktemp)"
            emit_openapi_params >"$PARAMS_TMP"
            awk -v p="$ROUTE" -v m="$METHOD_LOWER" -v name="$NAME" -v handler="$HANDLER" -v paramfile="$PARAMS_TMP" -v q="'" '
                {
                    print
                    if ($0 == "  " p ":" && !done) {
                        print "    " m ":"
                        print "      summary: " name "::" handler
                        while ((getline pline < paramfile) > 0) print pline;
                        close(paramfile);
                        print "      responses:"
                        print "        " q "200" q ": { description: OK }"
                        done=1
                    }
                }
            ' "$SPEC" >"$SPEC.tmp" && mv "$SPEC.tmp" "$SPEC"
            rm -f "$PARAMS_TMP"
            echo "==> Added ${METHOD_LOWER} ${ROUTE} to $SPEC"
        else
            echo "==> ${METHOD_LOWER} ${ROUTE} already in $SPEC — skipped"
        fi
    else
        # Whole path is new. Insert under the `paths:` line.
        PARAMS_TMP="$(mktemp)"
        emit_openapi_params >"$PARAMS_TMP"
        awk -v p="$ROUTE" -v m="$METHOD_LOWER" -v name="$NAME" -v handler="$HANDLER" -v paramfile="$PARAMS_TMP" -v q="'" '
            { print }
            /^paths:/ && !done {
                print "  " p ":"
                print "    " m ":"
                print "      summary: " name "::" handler
                while ((getline pline < paramfile) > 0) print pline;
                close(paramfile);
                print "      responses:"
                print "        " q "200" q ": { description: OK }"
                done=1
            }
        ' "$SPEC" >"$SPEC.tmp" && mv "$SPEC.tmp" "$SPEC"
        rm -f "$PARAMS_TMP"
        echo "==> Added path ${ROUTE} (${METHOD_LOWER}) to $SPEC"
    fi
else
    PARAMS_STUB="$(emit_openapi_params)"
    cat <<OPENAPI

==> Copy this into docs/openapi.yaml under "paths:" (edit as needed):

  ${ROUTE}:
    ${METHOD_LOWER}:
      summary: ${NAME}::${HANDLER}
${PARAMS_STUB:+$PARAMS_STUB
}      responses:
        '200': { description: OK }

OPENAPI
fi

# ── 5. Optional test scaffold ───────────────────────────────────────────
if [[ $WITH_TEST -eq 1 ]]; then
    STEM_LOWER="$(printf '%s' "$NAME" | sed -E 's/Controller$//' | tr '[:upper:]' '[:lower:]')"
    TEST_FILE="$ROOT/tests/api/test_${STEM_LOWER}.cpp"
    if [[ -e "$TEST_FILE" ]]; then
        echo "==> $TEST_FILE already exists — skipped"
    else
        # gtest StatusCode literal + test name suffix matching the verb.
        # POST returns 201 (Response::created); everything else 200.
        if [[ "$METHOD" = "Post" ]]; then
            EXPECTED_STATUS="k201Created"
            STATUS_SUFFIX="201"
        else
            EXPECTED_STATUS="k200OK"
            STATUS_SUFFIX="200"
        fi

        # Mutating handlers are admin-guarded → drive them through the authed
        # admin helper so the guard passes even once AUTH_MODE is enabled.
        # admin_principal() lives in test_fixtures.hpp, so only pull it in then.
        if [[ "$IS_MUTATION" -eq 1 ]]; then
            FIXTURES_INCLUDE='#include "test_fixtures.hpp"'
            REQUEST_EXPR="TestHelpers::authed(TestFixtures::admin_principal(), ${METHOD})"
        else
            FIXTURES_INCLUDE=""
            REQUEST_EXPR="TestHelpers::make_request(${METHOD})"
        fi

        cat >"$TEST_FILE" <<TESTEOF
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

// api/Api.hpp is pulled in for test builds only — it registers every
// controller. Production controllers never include it (it would cycle).
#include "api/Api.hpp"
#include "test_helpers.hpp"
${FIXTURES_INCLUDE}

using json = nlohmann::json;
using namespace drogon;

// Smoke test for ${NAME}::${HANDLER}. Generated by scripts/new-endpoint.sh.
// Replace the assertion with the actual contract once the handler does
// something more interesting than the TODO stub.
class ${NAME}Test : public ::testing::Test {
protected:
    Api::${NAME} controller;
};

TEST_F(${NAME}Test, ${HANDLER}_returns_${STATUS_SUFFIX}) {
    HttpResponsePtr captured;

    controller.${HANDLER}(${REQUEST_EXPR},
                          [&](const HttpResponsePtr& resp) { captured = resp; }${TEST_ARGS});

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), ${EXPECTED_STATUS});

    // The stub returns {"message": "..."} — adjust once the handler is real.
    auto body = json::parse(std::string(captured->body()));
    EXPECT_TRUE(body.contains("message"));
}
TESTEOF
        echo "==> Created $TEST_FILE"
    fi
fi

echo "==> Reminder: run ./scripts/check-openapi-drift.sh to confirm spec ↔ code parity."
echo "==> Done. Rebuild with: make build"
