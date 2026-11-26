#!/usr/bin/env bash
# CI helper: prints the version (project() in app/CMakeLists.txt) and, on a tag v*, fails if they differ.
set -euo pipefail
v=$(sed -n 's/^project(LibreTap VERSION \([0-9][0-9.]*\).*/\1/p' "$(dirname "$0")/../CMakeLists.txt")
[ -n "$v" ] || { echo "::error::version not found in CMakeLists.txt" >&2; exit 1; }
if [[ "${GITHUB_REF:-}" == refs/tags/v* && "${GITHUB_REF#refs/tags/v}" != "$v" ]]; then
    echo "::error::tag ${GITHUB_REF#refs/tags/} differs from the version $v in CMakeLists.txt" >&2
    exit 1
fi
echo "$v"
