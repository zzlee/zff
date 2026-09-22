"""Kaggle kernel (script): run zff CUDA integration test on a free GPU.

Executed remotely via `kaggle kernels push`. All output is captured in
the kernel log; scripts/kaggle-run.sh (local) polls and renders the
verdict. See docs/KAGGLE_CUDA.md.
"""
import hashlib
import subprocess
import sys
import urllib.request

REPO = "https://github.com/zzlee/zff.git"
BRANCH = "main"
SCRIPT_URL = (
    "https://raw.githubusercontent.com/zzlee/zff/"
    f"{BRANCH}/scripts/kaggle-cuda-test.sh"
)
LOCAL_SCRIPT = "/tmp/kaggle-cuda-test.sh"


def run(cmd):
    print(f"$ {' '.join(cmd)}", flush=True)
    r = subprocess.run(cmd)
    print(f"exit={r.returncode}", flush=True)
    return r.returncode


def main():
    rc = run(["nvidia-smi", "--query-gpu=name,driver_version",
              "--format=csv"])
    if rc != 0:
        print("FATAL: no GPU on this kernel worker", flush=True)
        return 2
    # Download to a file first (never pipe curl to bash): checksum it,
    # syntax-check it, then run it. Any transport mangling shows up here.
    with urllib.request.urlopen(SCRIPT_URL) as resp:
        body = resp.read()
    print(f"downloaded {len(body)} bytes", flush=True)
    print("sha256=" + hashlib.sha256(body).hexdigest(), flush=True)
    with open(LOCAL_SCRIPT, "wb") as f:
        f.write(body)
    rc = run(["bash", "-n", LOCAL_SCRIPT])
    if rc != 0:
        print("FATAL: downloaded script failed syntax check", flush=True)
        return 2
    rc = run(["bash", LOCAL_SCRIPT, REPO, BRANCH])
    print("KAGGLE_CUDA_RESULT=" + ("PASS" if rc == 0 else "FAIL"), flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
