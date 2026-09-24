#!/usr/bin/env bash
# Package a zff release tarball.
# Usage: ./scripts/package.sh [version] [arch]
#   version default: ./scripts/version.sh get
#   arch: x86_64 (default) | aarch64 (Xilinx SC6f0 cross via qcap image)
#
# Produces dist/zff-<version>-linux-<arch>.tar.gz containing:
#   usr/{lib/libzff-*.so*, include/zff/, lib/{cmake,pkgconfig}},
#   docs (README/ARCHITECTURE/ELEMENTS/ROADMAP/llms*), examples/webrtc_page.
# NOTE: prefix=/ triggers GNUInstallDirs usrmerge rewriting (usr/lib);
# prefix=/usr keeps the standard FHS staging layout.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VERSION="${1:-$("${SCRIPT_DIR}/version.sh" get)}"
VERSION="$(echo "${VERSION}" | tr -d '[:space:]' | sed 's/^v//')"
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "Invalid version '${VERSION}'" >&2
    exit 1
}
ARCH="${2:-x86_64}"
case "${ARCH}" in
    x86_64)
        IMAGE="zff-build:x86"
        BUILD_CMAKE_ARGS="-DBUILD_SHARED_LIBS=ON"
        BUILD_ENV_PRE=""
        ;;
    aarch64)
        IMAGE="${QCAP_BUILD_IMAGE:-zff-build:xlnk2_arm64}"
        # Headless target: no display sinks; WebRTC/SVT auto-degrade.
        BUILD_CMAKE_ARGS="-DBUILD_SHARED_LIBS=ON -DENABLE_DISPLAY=OFF -DCMAKE_PREFIX_PATH=/opt/zff-ffmpeg-xlnk2\;/opt/qcap/qcap-3rdparty/xlnk2_arm64 -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
        BUILD_ENV_PRE="source /opt/qcap-dev-init && unset PKG_CONFIG_SYSROOT_DIR && export PKG_CONFIG_PATH=/opt/zff-ffmpeg-xlnk2/lib/pkgconfig:/opt/qcap/qcap-3rdparty/xlnk2_arm64/lib/pkgconfig:\${SDKTARGETSYSROOT}/usr/lib/pkgconfig &&"
        ;;
    *)
        echo "Unknown arch '${ARCH}' (x86_64|aarch64)" >&2
        exit 1
        ;;
esac
if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "Builder image '${IMAGE}' missing." >&2
    exit 1
fi

PKG_BUILD_DIR="build-pkg-${ARCH}"
STAGE="dist/stage"
ARCHIVE="dist/zff-${VERSION}-linux-${ARCH}.tar.gz"
rm -rf "${REPO_ROOT}/${PKG_BUILD_DIR}" "${REPO_ROOT}/${STAGE}" "${REPO_ROOT}/${ARCHIVE}"
mkdir -p "${REPO_ROOT}/dist"

echo "==> Building release (${ARCH}) in ${IMAGE}..."
mkdir -p "${REPO_ROOT}/${PKG_BUILD_DIR}"
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -e HOME="${HOME}" \
    -v "${REPO_ROOT}:/workspace" \
    -w "/workspace/${PKG_BUILD_DIR}" \
    "${IMAGE}" \
    bash -lc "
        set -euo pipefail
        ${BUILD_ENV_PRE}
        cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
            -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
            ${BUILD_CMAKE_ARGS} .. &&
        ninja zff-core zff-plugins &&
        (ninja demo_webrtc_page || echo '(demo skipped: WebRTC unavailable)')
    "

echo "==> Installing to stage via cmake --install..."
STAGE_ABS="${REPO_ROOT}/${STAGE}/zff-${VERSION}-linux-${ARCH}"
mkdir -p "${STAGE_ABS}/examples/webrtc_page"
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "${REPO_ROOT}:/workspace" \
    -w "/workspace/${PKG_BUILD_DIR}" \
    "${IMAGE}" \
    bash -c "
        set -euo pipefail
        DESTDIR=/workspace/${STAGE}/zff-${VERSION}-linux-${ARCH} \
            cmake --install .
    "
for doc in README.md ARCHITECTURE.md ELEMENTS.md ROADMAP.md llms.txt llms-full.txt VERSION; do
    cp "${REPO_ROOT}/${doc}" "${STAGE_ABS}/"
done
cp "${REPO_ROOT}/${PKG_BUILD_DIR}/examples/webrtc_page/demo_webrtc_page" \
    "${STAGE_ABS}/examples/webrtc_page/" 2>/dev/null || true
cp "${REPO_ROOT}/examples/webrtc_page/index.html" \
    "${REPO_ROOT}/examples/webrtc_page/README.md" \
    "${STAGE_ABS}/examples/webrtc_page/"

echo "==> Archiving ${ARCHIVE}..."
tar -czf "${REPO_ROOT}/${ARCHIVE}" -C "${REPO_ROOT}/${STAGE}" "zff-${VERSION}-linux-${ARCH}"
rm -rf "${REPO_ROOT}/${PKG_BUILD_DIR}" "${REPO_ROOT}/${STAGE}"
ls -lh "${REPO_ROOT}/${ARCHIVE}"
echo "==> Done."
