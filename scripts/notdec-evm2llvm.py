#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def default_gigahorse_dir() -> Path | None:
    env_dir = os.environ.get("GIGAHORSE_DIR")
    if env_dir:
        return Path(env_dir)

    local_dir = Path("/sn640/gigahorse-toolchain")
    if local_dir.exists():
        return local_dir

    return None


def default_evm2llvm() -> Path:
    env_bin = os.environ.get("NOTDEC_EVM2LLVM")
    if env_bin:
        return Path(env_bin)

    source_root = Path(__file__).resolve().parents[1]
    candidates = [
        source_root / "build" / "bin" / "evm2llvm",
        source_root.parent.parent / "build-evm2llvm" / "bin" / "evm2llvm",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate

    return Path("evm2llvm")


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print("+ " + " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, cwd=cwd, check=True)


def contract_name(bytecode: Path) -> str:
    return bytecode.name.removesuffix(bytecode.suffix)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Gigahorse and lower its facts to LLVM IR."
    )
    parser.add_argument("bytecode", type=Path, help="Input EVM bytecode .hex file.")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .ll path.")
    parser.add_argument(
        "--gigahorse-dir",
        type=Path,
        default=default_gigahorse_dir(),
        help="Gigahorse checkout. Defaults to $GIGAHORSE_DIR or /sn640/gigahorse-toolchain.",
    )
    parser.add_argument(
        "--evm2llvm",
        type=Path,
        default=default_evm2llvm(),
        help="evm2llvm binary. Defaults to $NOTDEC_EVM2LLVM or a nearby build.",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        help="Directory for Gigahorse temporary output. Defaults to a temp dir.",
    )
    parser.add_argument(
        "--keep-work-dir",
        action="store_true",
        help="Do not remove the generated Gigahorse work directory.",
    )
    parser.add_argument(
        "--timeout-secs",
        type=int,
        default=120,
        help="Gigahorse per-contract timeout.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="Gigahorse job count.",
    )
    parser.add_argument(
        "--gigahorse-extra-arg",
        action="append",
        default=[],
        help="Extra argument passed to gigahorse.py. Can be repeated.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.gigahorse_dir is None:
        print("missing Gigahorse checkout; pass --gigahorse-dir or set GIGAHORSE_DIR", file=sys.stderr)
        return 2

    gigahorse = args.gigahorse_dir / "gigahorse.py"
    if not gigahorse.exists():
        print(f"missing gigahorse.py: {gigahorse}", file=sys.stderr)
        return 2

    if not args.bytecode.exists():
        print(f"missing bytecode file: {args.bytecode}", file=sys.stderr)
        return 2

    remove_work_dir = False
    if args.work_dir:
        work_dir = args.work_dir
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        work_dir = Path(tempfile.mkdtemp(prefix="notdec-evm2llvm-"))
        remove_work_dir = not args.keep_work_dir

    try:
        gigahorse_cmd = [
            sys.executable,
            str(gigahorse),
            "-w",
            str(work_dir),
            "-j",
            str(args.jobs),
            "-T",
            str(args.timeout_secs),
            "--restart",
            *args.gigahorse_extra_arg,
            str(args.bytecode.resolve()),
        ]
        run(gigahorse_cmd, cwd=args.gigahorse_dir)

        facts_dir = work_dir / contract_name(args.bytecode) / "out"
        if not facts_dir.exists():
            print(f"Gigahorse did not create facts directory: {facts_dir}", file=sys.stderr)
            return 1

        args.output.parent.mkdir(parents=True, exist_ok=True)
        run([str(args.evm2llvm), "--facts", str(facts_dir), "--output", str(args.output)])
        return 0
    finally:
        if remove_work_dir:
            shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
