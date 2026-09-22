#!/usr/bin/env bash
# Package a zff release tarball (x86_64).
# Usage: ./scripts/package.sh [version]   (default: ./scripts/version.sh get)
#
# Produces dist/zff-<version>-linux-x86_64.tar.gz containing:
#   lib/{libzff-core.so*, libzff-plugins.so*}, include/zff/,
#   docs (README/ARCHITECTURE/ELEMENTS/ROADMAP/llms*), examples/webrtc_page.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VERSION="${1:-$("${SCRIPT_DIR}/version.sh" get)}"
VERSION="$(echo "${VERSION}" | tr -d '[:space:]' | sed 's/^v//')"
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "Invalid version '${VERSION}'" >&2
    exit 1
}

IMAGE="zff-build:x86"
if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "Builder image '${IMAGE}' missing; run ./scripts/build-docker.sh x86 first." >&2
    exit 1
fi

PKG_BUILD_DIR="build-pkg"
STAGE="dist/stage"
ARCHIVE="dist/zff-${VERSION}-linux-x86_64.tar.gz"
rm -rf "${REPO_ROOT}/${PKG_BUILD_DIR}" "${REPO_ROOT}/${STAGE}" "${REPO_ROOT}/${ARCHIVE}"
mkdir -p "${REPO_ROOT}/dist"

echo "==> Building release in ${IMAGE}..."
mkdir -p "${REPO_ROOT}/${PKG_BUILD_DIR}"
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "${REPO_ROOT}:/workspace" \
    -w "/workspace/${PKG_BUILD_DIR}" \
    "${IMAGE}" \
    bash -c "
        set -euo pipefail
        cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF .. &&
        ninja zff-core zff-plugins demo_webrtc_page
    "

echo "==> Staging..."
STAGE_ABS="${REPO_ROOT}/${STAGE}/zff-${VERSION}-linux-x86_64"
mkdir -p "${STAGE_ABS}/lib" "${STAGE_ABS}/include" "${STAGE_ABS}/examples/webrtc_page"
cp "${REPO_ROOT}/${PKG_BUILD_DIR}/libzff-core.so"* "${STAGE_ABS}/lib/"
cp "${REPO_ROOT}/${PKG_BUILD_DIR}/libzff-plugins.so"* "${STAGE_ABS}/lib/"
cp -r "${REPO_ROOT}/include/zff" "${STAGE_ABS}/include/"
for doc in README.md ARCHITECTURE.md ELEMENTS.md ROADMAP.md llms.txt llms-full.txt VERSION; do
    cp "${REPO_ROOT}/${doc}" "${STAGE_ABS}/"
done
cp "${REPO_ROOT}/${PKG_BUILD_DIR}/examples/webrtc_page/demo_webrtc_page" \
    "${STAGE_ABS}/examples/webrtc_page/" 2>/dev/null || true
cp "${REPO_ROOT}/examples/webrtc_page/index.html" \
    "${REPO_ROOT}/examples/webrtc_page/README.md" \
    "${STAGE_ABS}/examples/webrtc_page/"

echo "==> Archiving ${ARCHIVE}..."
tar -czf "${REPO_ROOT}/${ARCHIVE}" -C "${REPO_ROOT}/${STAGE}" "zff-${VERSION}-linux-x86_64"
rm -rf "${REPO_ROOT}/${PKG_BUILD_DIR}" "${REPO_ROOT}/${STAGE}"
ls -lh "${REPO_ROOT}/${ARCHIVE}"
echo "==> Done."
