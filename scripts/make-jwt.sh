#!/usr/bin/env bash
# Emit a minimal HS256 JWT suitable for hitting a locally-running instance
# of the template with AUTH_MODE=jwt. Uses only openssl + base64 — no Python
# or Node dependencies. Designed for `curl -H "Authorization: Bearer $(make jwt)"`.

set -euo pipefail

SECRET="${JWT_SECRET:-dev-secret-please-rotate}"
ISSUER="${JWT_ISSUER:-my-issuer}"
AUDIENCE="${JWT_AUDIENCE:-my-api}"
SUBJECT="${JWT_SUB:-dev-user}"
ROLES="${JWT_ROLES:-admin}"          # comma-separated
# Permission bitmask claim — require_admin/require_permission on the backend
# read THIS, not roles. 255 (0xff) = Domain::Permission::kAdminister.
PERMS="${JWT_PERMS:-255}"
TTL_SEC="${JWT_EXP:-3600}"
KID="${JWT_KID:-}"                   # emitted as `kid` header claim when set

while [[ $# -gt 0 ]]; do
    case "$1" in
        --secret) SECRET="$2"; shift 2 ;;
        --iss)    ISSUER="$2"; shift 2 ;;
        --aud)    AUDIENCE="$2"; shift 2 ;;
        --sub)    SUBJECT="$2"; shift 2 ;;
        --roles)  ROLES="$2"; shift 2 ;;
        --perms)  PERMS="$2"; shift 2 ;;
        --exp)    TTL_SEC="$2"; shift 2 ;;
        --kid)    KID="$2"; shift 2 ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--secret STR] [--iss STR] [--aud STR] [--sub STR] [--roles a,b,c] [--perms N] [--exp SEC]

Emits a HS256 JWT to stdout. Every flag also reads an env default (see top of script).

Examples:
  ./scripts/make-jwt.sh
  ./scripts/make-jwt.sh --sub alice --roles admin,viewer --exp 600
  ./scripts/make-jwt.sh --perms 1   # non-admin: General bit only
  curl -H "Authorization: Bearer \$(./scripts/make-jwt.sh)" http://localhost:8080/api/jobs
EOF
            exit 0
            ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }

NOW=$(date +%s)
EXP=$((NOW + TTL_SEC))

# Build roles JSON array from comma-separated input.
IFS=',' read -ra ROLE_ARR <<< "$ROLES"
ROLES_JSON="["
for i in "${!ROLE_ARR[@]}"; do
    [[ $i -gt 0 ]] && ROLES_JSON+=','
    ROLES_JSON+="\"${ROLE_ARR[$i]}\""
done
ROLES_JSON+="]"

if [[ -n "$KID" ]]; then
    HEADER_JSON=$(printf '{"alg":"HS256","typ":"JWT","kid":"%s"}' "$KID")
else
    HEADER_JSON='{"alg":"HS256","typ":"JWT"}'
fi
HEADER=$(printf '%s' "$HEADER_JSON" | b64url)
PAYLOAD=$(printf '{"sub":"%s","iss":"%s","aud":"%s","iat":%d,"exp":%d,"permissions":%d,"roles":%s}' \
    "$SUBJECT" "$ISSUER" "$AUDIENCE" "$NOW" "$EXP" "$PERMS" "$ROLES_JSON" | b64url)
SIGNING="${HEADER}.${PAYLOAD}"
SIG=$(printf '%s' "$SIGNING" | openssl dgst -sha256 -hmac "$SECRET" -binary | b64url)

echo "${SIGNING}.${SIG}"
