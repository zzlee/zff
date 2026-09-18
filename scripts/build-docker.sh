#!/usr/bin/env bash
# Build a zff builder Docker image.
#
# Usage: ./scripts/build-docker.sh <variant> [docker build args ...]
#
# Supported variants:
#   x86 (or dev)   docker/Dockerfile.x86       zff-build:x86
#   jetson         docker/Dockerfile.jetson    zff-build:jetson (future)
#   petalinux      docker/Dockerfile.petalinux zff-build:petalinux (future)
#   aarch64        docker/Dockerfile.arm64     zff-build:aarch64 (future)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

VARIANT="${1:-}"
[[ -z "${VARIANT}" ]] && usage
shift || true

case "${VARIANT}" in
    x86|dev)
        DOCKERFILE="docker/Dockerfile.x86"
        IMAGE="zff-build:x86"
        ;;
    jetson)
        DOCKERFILE="docker/Dockerfile.jetson"
        IMAGE="zff-build:jetson"
        ;;
    petalinux)
        DOCKERFILE="docker/Dockerfile.petalinux"
        IMAGE="zff-build:petalinux"
        ;;
    aarch64|arm64)
        DOCKERFILE="docker/Dockerfile.arm64"
        IMAGE="zff-build:aarch64"
        ;;
    *)
        echo "Error: unknown builder variant '${VARIANT}'" >&2
        usage
        ;;
esac

if [[ ! -f "${REPO_ROOT}/${DOCKERFILE}" ]]; then
    echo "Error: Dockerfile '${DOCKERFILE}' not found for variant '${VARIANT}'" >&2
    exit 1
fi

echo "==> Building ${IMAGE} from ${DOCKERFILE}..."
exec docker build -f "${REPO_ROOT}/${DOCKERFILE}" -t "${IMAGE}" "${REPO_ROOT}" "$@"
