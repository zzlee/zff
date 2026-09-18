#!/usr/bin/env bash
# Start an interactive development shell in the builder container.
#
# Usage: ./scripts/dev.sh <variant>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VARIANT="${1:-x86}"

case "${VARIANT}" in
    x86|dev)    IMAGE="zff-build:x86" ;;
    jetson)     IMAGE="zff-build:jetson" ;;
    petalinux)  IMAGE="zff-build:petalinux" ;;
    *)          IMAGE="zff-build:${VARIANT}" ;;
esac

echo "==> Starting interactive shell in ${IMAGE}..."
exec docker run --rm -it \
    -u "$(id -u):$(id -g)" \
    -v "${REPO_ROOT}:/workspace" \
    -w /workspace \
    "${IMAGE}" bash
