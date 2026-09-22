#!/usr/bin/env bash
# Kaggle CUDA test platform for zff (free T4/P100 GPU).
#
# Run INSIDE a Kaggle notebook (GPU accelerator ON) as one cell:
#   !bash scripts/kaggle-cuda-test.sh [repo-url] [branch]
#
# What it does:
#   1. Installs zff build deps via apt (Kaggle images are Ubuntu-based
#      with sudo; NVIDIA driver + CUDA toolkit are preinstalled).
#   2. Clones zff (default: https://github.com/zzlee/zff.git) or reuses
#      /kaggle/working/zff if already present.
#   3. Native cmake build (no docker on Kaggle) and runs ONLY the CUDA
#      integration test (full ctest needs Xvfb/ALSA loopbacks etc.).
#
# No zff CUDA code is compiled against the toolkit: the test uses only
# public libav* hwcontext API. nvidia-smi must show a GPU first.
set -euo pipefail

REPO_URL="${1:-https://github.com/zzlee/zff.git}"
BRANCH="${2:-main}"
WORKDIR="/kaggle/working/zff"

echo "==> GPU check"
nvidia-smi --query-gpu=name,driver_version --format=csv || {
    echo "ERROR: no NVIDIA GPU. Enable Accelerator: GPU in notebook settings." >&2
    exit 1
}

echo "==> Step 0: system ffmpeg NVENC smoke test (non-fatal)"
if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -hide_banner -y -v error \
        -f lavfi -i testsrc=size=320x240:rate=30:duration=1 \
        -c:v h264_nvenc -f null - 2>&1 | head -n 5 || true
else
    echo "    (no system ffmpeg; skipping smoke test)"
fi

echo "==> Build dependencies"
sudo apt-get update -qq
sudo apt-get install -y -qq \
    build-essential cmake ninja-build pkg-config nasm yasm git \
    libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev \
    libavdevice-dev libswscale-dev libswresample-dev \
    libasound2-dev libgl1-mesa-dev libx11-dev libxext-dev \
    libfreetype-dev libsrt-gnutls-dev xxd > /dev/null
echo "    deps installed"

# zff needs FFmpeg >= 6.1 (ch_layout, AVFrame.time_base). Kaggle's
# Ubuntu 22.04 ships 4.4, so build 6.1 from source when system is old.
# Includes nv-codec-headers for NVENC; CUDA toolkit is preinstalled.
FFMPEG_PREFIX="/opt/zff-ffmpeg"
need_ffmpeg_build() {
    pkg-config --atleast-version=58 libavutil 2>/dev/null || return 0
    return 1
}
if need_ffmpeg_build; then
    echo "==> System FFmpeg too old; building FFmpeg 6.1 from source (~10 min)"
    if [ -x /usr/local/cuda/bin/nvcc ]; then
        export PATH="/usr/local/cuda/bin:${PATH}"
    fi
    sudo mkdir -p "${FFMPEG_PREFIX}" /tmp/zffdeps
    sudo chown -R "$(id -u):$(id -g)" "${FFMPEG_PREFIX}" /tmp/zffdeps
    # Export FIRST: ffmpeg's configure finds ffnvcodec.pc via pkg-config
    export PKG_CONFIG_PATH="${FFMPEG_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export LD_LIBRARY_PATH="${FFMPEG_PREFIX}/lib:${LD_LIBRARY_PATH:-}"
    cd /tmp/zffdeps
    if [ ! -d nv-codec-headers ]; then
        git clone --depth 1 https://git.videolan.org/git/ffmpeg/nv-codec-headers.git
    fi
    make -C nv-codec-headers PREFIX="${FFMPEG_PREFIX}" install
    if [ ! -d ffmpeg-6.1.2 ]; then
        curl -fsSL -o ffmpeg-6.1.2.tar.xz https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz
        tar xf ffmpeg-6.1.2.tar.xz
    fi
    cd ffmpeg-6.1.2
    ./configure --prefix="${FFMPEG_PREFIX}" \
        --disable-static --enable-shared --disable-doc --disable-debug \
        --enable-cuda --enable-cuvid --enable-nvenc \
        --extra-cflags="-I${FFMPEG_PREFIX}/include" \
        --extra-ldflags="-L${FFMPEG_PREFIX}/lib" > /tmp/zffdeps/ffconfig.log 2>&1 \
        || { echo "FFmpeg configure failed:"; tail -n 20 /tmp/zffdeps/ffconfig.log; exit 1; }
    make -j"$(nproc)" > /tmp/zffdeps/ffbuild.log 2>&1 \
        || { echo "FFmpeg build failed:"; tail -n 20 /tmp/zffdeps/ffbuild.log; exit 1; }
    make install > /tmp/zffdeps/ffinstall.log 2>&1
    cd /tmp/zffdeps
    echo "    FFmpeg $(pkg-config --modversion libavutil) ready at ${FFMPEG_PREFIX}"
fi

echo "==> Source"
if [ -d "${WORKDIR}/.git" ]; then
    git -C "${WORKDIR}" fetch origin "${BRANCH}" --depth 1
    git -C "${WORKDIR}" checkout "${BRANCH}"
    git -C "${WORKDIR}" pull --ff-only origin "${BRANCH}" || true
else
    rm -rf "${WORKDIR}"
    git clone --depth 1 --branch "${BRANCH}" "${REPO_URL}" "${WORKDIR}"
fi
cd "${WORKDIR}"
git log --oneline -1

echo "==> Configure + build (tests only need core/plugins + cuda test)"
cmake -S . -B build-kaggle -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build-kaggle --target test_hwaccel_cuda

echo "==> Run CUDA integration test"
./build-kaggle/test_hwaccel_cuda
