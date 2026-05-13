#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
from pathlib import Path


def find_evm2llvm() -> Path:
    env_path = os.environ.get("NOTDEC_EVM2LLVM")
    if env_path:
        return Path(env_path)

    script_dir = Path(__file__).resolve().parent
    candidates = [
        script_dir.parent / "build" / "bin" / "evm2llvm",
        script_dir.parent / ".." / ".." / "build-evm2llvm" / "bin" / "evm2llvm",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate

    return Path("evm2llvm")


def main() -> int:
    parser = argparse.ArgumentParser(description="Gigahorse client for NotDec evm2llvm")
    parser.add_argument(
        "--facts",
        default=os.getcwd(),
        help="Gigahorse output directory. Defaults to current working directory.",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="contract.ll",
        help="Output LLVM IR path.",
    )
    args = parser.parse_args()

    cmd = [str(find_evm2llvm()), "--facts", args.facts, "--output", args.output]
    completed = subprocess.run(cmd)
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
