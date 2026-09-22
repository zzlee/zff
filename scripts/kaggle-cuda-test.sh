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

echo "==> Build dependencies"
sudo apt-get update -qq
sudo apt-get install -y -qq \
    build-essential cmake ninja-build pkg-config \
    libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev \
    libavdevice-dev libswscale-dev libswresample-dev \
    libasound2-dev libgl1-mesa-dev libx11-dev libxext-dev \
    libfreetype-dev libsrt-gnutls-dev xxd > /dev/null
echo "    deps installed"

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
