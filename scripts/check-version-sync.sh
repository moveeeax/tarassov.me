#!/usr/bin/env bash
# Verify the baked application version matches the newest RELEASED CHANGELOG
# heading.
#
# project(VERSION ...) in CMakeLists.txt renders into generated/version.hpp and
# is what Core::version(), GET /health and GET / report. Nothing compared it to
# anything, so it sat at 1.5.0 while three releases (2.0.0 / 2.0.1 / 2.0.2)
# shipped. Release builds override it from the git tag (docker/Dockerfile ARG
# APP_VERSION -> -DPROJECT_VERSION_OVERRIDE), but that leaves every non-tag
# build — including the runtime images CI smoke-tests — reporting the baked
# value, so the baked value has to stay honest.
#
# '## [Unreleased]' is skipped: only headings whose bracket holds a version.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cmake_version="$(sed -n 's/^project(.*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\).*/\1/p' \
    "$REPO/CMakeLists.txt" | head -1)"
changelog_version="$(sed -n 's/^## \[\([0-9][0-9.]*\)\].*/\1/p' "$REPO/CHANGELOG.md" | head -1)"

if [ -z "$cmake_version" ]; then
    echo "✗ could not parse project(... VERSION x.y.z ...) from CMakeLists.txt" >&2
    exit 1
fi
if [ -z "$changelog_version" ]; then
    echo "✗ could not parse a released '## [x.y.z]' heading from CHANGELOG.md" >&2
    exit 1
fi

if [ "$cmake_version" != "$changelog_version" ]; then
    {
        echo "✗ version drift:"
        echo "    CMakeLists.txt  project(VERSION $cmake_version)"
        echo "    CHANGELOG.md    newest release heading [$changelog_version]"
        echo ""
        echo "Bump project(VERSION ...) in CMakeLists.txt to $changelog_version in the"
        echo "same commit as the changelog entry. helm/*/Chart.yaml appVersion and"
        echo "vcpkg.json version are bumped by hand alongside it."
    } >&2
    exit 1
fi

echo "✓ version in sync: $cmake_version (CMakeLists.txt == newest CHANGELOG heading)"
