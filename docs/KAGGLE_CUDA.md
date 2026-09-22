# Kaggle CUDA Test Runbook

Run the zff CUDA video integration test (`tests/test_hwaccel_cuda.c`) on a
free Kaggle GPU (T4/P100). Two paths: raw notebook (manual) or full CLI
(push → poll → verdict from this machine).

No zff CUDA code is compiled against the toolkit: the test uses only
public libav* hwcontext API (`h264_nvenc` encode, CUDA-accelerated
decode, HW frame upload/download). Only H264 is exercised (Turing
NVENC/NVDEC on free GPUs lack AV1 encode).

## Path A — manual notebook (simplest first run)

1. Kaggle → New Notebook → Settings → Accelerator: **GPU T4 x2**.
2. One cell:
   ```
   !bash scripts/kaggle-cuda-test.sh
   ```
   (clone the repo into the notebook first, or pass a repo URL + branch:
   `!bash scripts/kaggle-cuda-test.sh https://github.com/zzlee/zff.git main`.)
3. Expect (~5–10 min):
   ```
   [INFO] CUDA device opened.
   [PASS] CUDA xfer passed.
   [INFO] Encoded 30 packets, ...
   [PASS] NVENC encode passed.
   [INFO] Decoded 30 frames ...
   [PASS] CUDA decode passed.
   ```

`scripts/kaggle-cuda-test.sh` does: `nvidia-smi` check → system-ffmpeg
NVENC smoke test → apt build deps → FFmpeg 6.1 source build **only if
system libavutil < 58** (Kaggle's Ubuntu 22.04 ships 4.4; zff requires
≥ 6.1 for `ch_layout`/`AVFrame.time_base`, enforced by CMake version
floors) → clone/pull → native cmake build of `test_hwaccel_cuda`
only → run it. Reruns reuse `/kaggle/working/zff`.

## Path B — full CLI from this machine (no browser)

One-time setup (pick one auth method):

```bash
uv tool install kaggle
# A. classic: kaggle.com → Settings → Account → API → Create New Token
mkdir -p ~/.kaggle && mv ~/Downloads/kaggle.json ~/.kaggle/
chmod 600 ~/.kaggle/kaggle.json
# B. access token: put the token in ~/.kaggle/access_token and export the user:
export KAGGLE_USER=zzlee1234
```

Run:

```bash
./scripts/kaggle-run.sh            # uses kaggle/zff-cuda-test/
./scripts/kaggle-run.sh <slug>     # another kernel dir under kaggle/
```

What happens: `kaggle kernels push -p kaggle/zff-cuda-test` uploads
`test_cuda.py` (+ metadata: `enable_gpu`, `enable_internet`, private)
and starts a remote run; the wrapper polls `kernels status` up to
~2 h, then greps `kernels logs` for `KAGGLE_CUDA_RESULT=PASS`.
Exit 0 = PASS. The kernel script itself just pulls
`scripts/kaggle-cuda-test.sh` from pushed `main` and runs it, so the
code under test is always the latest pushed commit.

## Failure playbook

| Symptom | Cause / fix |
|---|---|
| `nvidia-smi` fails / no GPU | Accelerator dropped on save; re-select GPU T4 x2 |
| Smoke test fails, `nvenc` unknown | Driver too old on worker; retry later (new worker) |
| apt lock / transient network | Rerun the cell (flaky on Kaggle); script is idempotent |
| Private repo clone fails | Pass an authenticated URL: `https://<token>@github.com/...` |
| Kernel `error` status in Path B | `kaggle kernels logs <user>/zff-cuda-test` for the traceback |
| GPU quota exhausted (429) | Free tier ~30 h/week; wait for reset |

## Local counterparts (no Kaggle needed)

- VAAPI on this machine's iGPU: `docker run --device=/dev/dri:/dev/dri
  --group-add 44 --group-add 990 ... ./test_hwaccel_vaapi`
- Both HW tests SKIP cleanly without hardware (CI-safe).
- oneAPI/VPL HW (`qsv`) is blocked on Intel packaging `vpl-gpu-rt`
  (`libmfxhw64` uninstallable here); the VAAPI test validates the same
  Quick Sync silicon until then.
