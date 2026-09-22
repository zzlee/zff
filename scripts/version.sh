#!/usr/bin/env bash
# Minimal version helper. VERSION (MAJOR.MINOR.PATCH) is the single source.
# Usage: ./scripts/version.sh get | check-tag [tag]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VERSION_FILE="${REPO_ROOT}/VERSION"

cmd="${1:-get}"

case "${cmd}" in
    get)
        tr -d '[:space:]' < "${VERSION_FILE}"
        echo
        ;;
    check-tag)
        tag="${2:-}"
        [[ -n "${tag}" ]] || { echo "Usage: version.sh check-tag <tag>" >&2; exit 1; }
        ver="$(tr -d '[:space:]' < "${VERSION_FILE}")"
        if [[ "${tag}" != "v${ver}" ]]; then
            echo "Tag '${tag}' does not match VERSION '${ver}' (expected 'v${ver}')" >&2
            exit 1
        fi
        echo "Tag '${tag}' matches VERSION."
        ;;
    *)
        echo "Usage: version.sh get | check-tag [tag]" >&2
        exit 1
        ;;
esac
