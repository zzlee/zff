#!/usr/bin/env bash
# Run the zff CUDA integration test on Kaggle free GPU, fully from CLI.
#
# One-time setup (pick one auth method):
#   A. kaggle.json (classic):
#        uv tool install kaggle
#        # kaggle.com -> Settings -> Account -> API -> Create New Token
#        mkdir -p ~/.kaggle && mv ~/Downloads/kaggle.json ~/.kaggle/ && chmod 600 ~/.kaggle/kaggle.json
#   B. access token (as in ~/vibecoding/kaggle-eval):
#        # ~/.kaggle/access_token holds the token; username via env:
#        export KAGGLE_USER=zzlee1234
#
# Usage:
#   ./scripts/kaggle-run.sh [kernel-slug]       # default: zff-cuda-test
#   KAGGLE_USER=zzlee1234 ./scripts/kaggle-run.sh
#
# Flow: kernels push (runs remotely) -> poll status -> fetch logs ->
# exit 0 on KAGGLE_CUDA_RESULT=PASS, 1 otherwise.
# See docs/KAGGLE_CUDA.md for the manual-notebook alternative.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KAGGLE_BIN="${KAGGLE_BIN:-kaggle}"
SLUG="${1:-zff-cuda-test}"
KERNEL_DIR="${REPO_ROOT}/kaggle/${SLUG}"

command -v "${KAGGLE_BIN}" >/dev/null || {
    echo "kaggle CLI not found. Install: uv tool install kaggle" >&2
    exit 2
}
# Auth: kaggle.json preferred; else access_token file + KAGGLE_USER.
if [ -f ~/.kaggle/kaggle.json ]; then
    USER_NAME="$(python3 -c "import json;print(json.load(open('$HOME/.kaggle/kaggle.json'))['username'])")"
elif [ -f ~/.kaggle/access_token ]; then
    USER_NAME="${KAGGLE_USER:-}"
    if [ -z "${USER_NAME}" ]; then
        echo "Set KAGGLE_USER (access_token file carries no username)." >&2
        exit 2
    fi
    export KAGGLE_USERNAME="${USER_NAME}"
    export KAGGLE_KEY="$(cat ~/.kaggle/access_token)"
else
    echo "No Kaggle auth: need ~/.kaggle/kaggle.json or ~/.kaggle/access_token." >&2
    exit 2
fi
[ -d "${KERNEL_DIR}" ] || {
    echo "Kernel dir ${KERNEL_DIR} missing." >&2
    exit 2
}
KERNEL_ID="${USER_NAME}/${SLUG}"

# Fill the metadata id template on first run (keeps the file generic).
if grep -q "__KAGGLE_USER__" "${KERNEL_DIR}/kernel-metadata.json"; then
    sed -i "s/__KAGGLE_USER__/${USER_NAME}/" "${KERNEL_DIR}/kernel-metadata.json"
    echo "==> Set kernel id to ${KERNEL_ID}"
fi

echo "==> Pushing kernel ${KERNEL_ID} (remote run starts)"
"${KAGGLE_BIN}" kernels push -p "${KERNEL_DIR}"

echo "==> Polling status (GPU queue + ~10 min job)"
for i in $(seq 1 120); do
    STATUS="$("${KAGGLE_BIN}" kernels status "${KERNEL_ID}" 2>/dev/null || true)"
    echo "    [${i}] ${STATUS}"
    case "${STATUS}" in
        *complete*|*Complete*)
            break
            ;;
        *error*|*Error*|*failed*|*Failed*|*cancelled*|*Cancelled*)
            echo "ERROR: kernel run failed: ${STATUS}" >&2
            "${KAGGLE_BIN}" kernels logs "${KERNEL_ID}" 2>/dev/null | tail -n 30 || true
            exit 1
            ;;
    esac
    sleep 60
done

echo "==> Fetching logs"
LOG="$("${KAGGLE_BIN}" kernels logs "${KERNEL_ID}" 2>/dev/null || true)"
echo "${LOG}" | grep -E "TEST|PASS|FAIL|SKIP|INFO|KAGGLE_CUDA_RESULT|ERROR" | head -n 30

if echo "${LOG}" | grep -q "KAGGLE_CUDA_RESULT=PASS"; then
    echo "==> KAGGLE CUDA TEST: PASS"
    exit 0
else
    echo "==> KAGGLE CUDA TEST: FAIL (see full log: kaggle kernels logs ${KERNEL_ID})" >&2
    exit 1
fi
