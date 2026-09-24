#!/usr/bin/env bash
# Build zff inside the builder Docker image with user-owned artifacts.
#
# Usage:
#   ./scripts/build.sh <variant> [cmake/ninja args ...]
#
# Variants:
#   x86 (or dev)       Build using zff-build:x86 image (outputs to build-x86/)
#   xlnk2_arm64        Cross-compile for Xilinx SC6f0 (Petalinux) using the
#                      yuan88yuan/qcap-build:xlnk2_arm64-base image.
#                      Output: build-xlnk2_arm64/ (binaries need target HW to run)
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
    xlnk2_arm64)
        # Cross build needs the qcap toolchain env; handled below.
        DEFAULT_BUILD_DIR="build-xlnk2_arm64"
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

if [[ "${VARIANT}" == "xlnk2_arm64" ]]; then
    # Cross-compile with the qcap toolchain (mirrors zstreamer's
    # build_xlnk2_arm64); headless target => no display sinks, no
    # WebRTC/SVT (auto-degrade if absent from the SDK sysroot).
    # Binaries are ARM64 and cannot run here; BUILD_TESTS=ON still
    # proves compilation. ctest runs on target HW only.
    IMAGE="${QCAP_BUILD_IMAGE:-zff-build:xlnk2_arm64}"
    echo "==> Cross-building zff for [${VARIANT}] inside ${IMAGE}..."
    echo "==> Output directory: ${OUT_DIR}"
    docker run --rm \
        -u "$(id -u):$(id -g)" \
        -e HOME="${HOME}" \
        -v "${REPO_ROOT}:/workspace" \
        -w /workspace \
        "${IMAGE}" \
        bash -lc "
            source /opt/qcap-dev-init &&
            unset PKG_CONFIG_SYSROOT_DIR &&
            export PKG_CONFIG_PATH=/opt/zff-ffmpeg-xlnk2/lib/pkgconfig:/opt/qcap/qcap-3rdparty/xlnk2_arm64/lib/pkgconfig:\${SDKTARGETSYSROOT}/usr/lib/pkgconfig &&
            cmake -B ${OUT_DIR} -S . -G Ninja \
                -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
                -DBUILD_TESTS=${BUILD_TESTS} \
                -DBUILD_SHARED_LIBS=ON \
                -DENABLE_DISPLAY=OFF \
                -DCMAKE_PREFIX_PATH=/opt/qcap/qcap-3rdparty/xlnk2_arm64 \
                -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
                -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
                -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
                \"\$@\" &&
            cmake --build ${OUT_DIR} --parallel \$(nproc)
        " bash "$@"
    echo "==> Build complete! Artifacts are in ${OUT_DIR}/"
    exit 0
fi

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
