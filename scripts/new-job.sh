#!/usr/bin/env bash
#
# Scaffold a new background-job handler.
#
# Usage:
#   ./scripts/new-job.sh <job-type> [HandlerName]
#
# Example:
#   ./scripts/new-job.sh reindex            # -> src/jobs/handlers/ReindexJob.hpp
#   ./scripts/new-job.sh send_report SendReport
#
# Emits a header-only handler that self-registers with Jobs::Dispatcher via a
# JobHandlerRegistrar (mirrors how controllers self-register with Drogon), then
# tells you the ONE line to add to src/worker_main.cpp: an #include of the new
# header. After that, a job submitted with that type is dispatched to your
# handler; unknown types still go straight to the DLQ.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <job-type> [HandlerName]" >&2
    echo "Example: $0 reindex" >&2
    exit 1
fi

JOB_TYPE="$1"
if [[ ! "$JOB_TYPE" =~ ^[a-z0-9_]+$ ]]; then
    echo "ERROR: job-type must be lower snake_case [a-z0-9_], got '$JOB_TYPE'" >&2
    exit 2
fi

# Derive PascalCase handler name from the job type unless given explicitly.
if [[ $# -ge 2 ]]; then
    NAME="$2"
else
    NAME=""
    for part in ${JOB_TYPE//_/ }; do
        # PascalCase each underscore-delimited part (portable: bash 3.2 lacks ${part^}).
        first="$(printf '%s' "$part" | cut -c1 | tr '[:lower:]' '[:upper:]')"
        rest="$(printf '%s' "$part" | cut -c2-)"
        NAME+="${first}${rest}"
    done
fi

# camelCase form of the PascalCase name (Foo -> foo), used as the handler fn name.
# Portable lowercase-first-char (bash 3.2 lacks ${NAME,}).
CAMEL_NAME="$(printf '%s' "$NAME" | cut -c1 | tr '[:upper:]' '[:lower:]')$(printf '%s' "$NAME" | cut -c2-)"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/src/jobs/handlers"
FILE="$DIR/${NAME}Job.hpp"
mkdir -p "$DIR"

if [[ -e "$FILE" ]]; then
    echo "ERROR: $FILE already exists" >&2
    exit 3
fi

GUARD_VAR="k_$(echo "$JOB_TYPE" | tr '[:upper:]' '[:lower:]')_job"

cat >"$FILE" <<EOF
/**
 * @file ${NAME}Job.hpp
 * @brief Handler for the "${JOB_TYPE}" background job. Self-registers with
 *        Jobs::Dispatcher at static-init — #include this header from
 *        src/worker_main.cpp (next to the other handler includes) and the
 *        worker will route "${JOB_TYPE}" jobs here.
 */

#pragma once

#include <nlohmann/json.hpp>

#include "jobs/Dispatcher.hpp"

namespace Jobs::Handlers {

using json = nlohmann::json;

/// Process one "${JOB_TYPE}" job. Throw std::exception to retry-then-DLQ, or
/// Jobs::PermanentJobError to DLQ immediately (no retries).
inline json ${CAMEL_NAME}_handler(const json& payload) {
    // TODO: implement. \`payload\` is whatever the producer passed to
    // Jobs::get().submit("${JOB_TYPE}", payload).
    (void)payload;
    return {{"message", "TODO: implement ${JOB_TYPE}"}};
}

// Self-registration — runs before main() spawns worker threads.
inline const Jobs::JobHandlerRegistrar ${GUARD_VAR}{"${JOB_TYPE}", &${CAMEL_NAME}_handler};

}  // namespace Jobs::Handlers
EOF

echo "==> Created $FILE"
echo ""
echo "Next:"
echo "  1. Implement ${CAMEL_NAME}_handler() in $FILE"
echo "  2. Add to src/worker_main.cpp (with the other handler includes):"
echo "         #include \"jobs/handlers/${NAME}Job.hpp\""
echo "  3. Add \"${JOB_TYPE}\" to WORKER_TYPES so the worker drains that queue."
