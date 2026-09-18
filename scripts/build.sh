#!/usr/bin/env bash
# Build zff inside the builder Docker image with user-owned artifacts.
#
# Usage:
#   ./scripts/build.sh <variant> [cmake/ninja args ...]
#
# Variants:
#   x86 (or dev)       Build using zff-build:x86 image (outputs to build-x86/)
#   jetson             Build using zff-build:jetson image (future)
#   petalinux          Build using zff-build:petalinux image (future)
#
# Environment variables:
#   BUILD_TYPE         CMAKE_BUILD_TYPE (default: Release)
#   BUILD_DIR          Output build directory (default: build-<variant>)
#   BUILD_TESTS        ON/OFF (default: ON)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

VARIANT="${1:-}"
[[ -z "${VARIANT}" ]] && usage
shift || true

BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_TESTS="${BUILD_TESTS:-ON}"

case "${VARIANT}" in
    x86|dev)
        IMAGE="zff-build:x86"
        DEFAULT_BUILD_DIR="build-x86"
        ;;
    jetson)
        IMAGE="zff-build:jetson"
        DEFAULT_BUILD_DIR="build-jetson"
        ;;
    petalinux)
        IMAGE="zff-build:petalinux"
        DEFAULT_BUILD_DIR="build-petalinux"
        ;;
    *)
        echo "Error: unknown variant '${VARIANT}'" >&2
        usage
        ;;
esac

OUT_DIR="${BUILD_DIR:-${DEFAULT_BUILD_DIR}}"
mkdir -p "${REPO_ROOT}/${OUT_DIR}"

echo "==> Building zff for [${VARIANT}] inside ${IMAGE}..."
echo "==> Output directory: ${OUT_DIR}"

docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "${REPO_ROOT}:/workspace" \
    -w "/workspace/${OUT_DIR}" \
    "${IMAGE}" \
    bash -c "
        set -euo pipefail
        cmake -G Ninja \
            -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
            -DBUILD_TESTS=${BUILD_TESTS} \
            .. \"\$@\"
        ninja
    " -- "$@"

echo "==> Build complete! Artifacts are in ${OUT_DIR}/"
