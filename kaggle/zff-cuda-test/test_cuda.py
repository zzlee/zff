"""Kaggle kernel (script): run zff CUDA integration test on a free GPU.

Executed remotely via `kaggle kernels push`. All output is captured in
the kernel log; scripts/kaggle-run.sh (local) polls and renders the
verdict. See docs/KAGGLE_CUDA.md.
"""
import subprocess
import sys

REPO = "https://github.com/zzlee/zff.git"
BRANCH = "main"


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
    rc = run(["bash", "-c",
              f"curl -fsSL https://raw.githubusercontent.com/zzlee/zff/{BRANCH}/scripts/kaggle-cuda-test.sh "
              f"| bash -s -- {REPO} {BRANCH}"])
    print("KAGGLE_CUDA_RESULT=" + ("PASS" if rc == 0 else "FAIL"), flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
