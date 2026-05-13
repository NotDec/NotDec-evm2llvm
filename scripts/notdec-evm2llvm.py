#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def default_gigahorse_bin() -> Path | None:
    env_bin = os.environ.get("GIGAHORSE_BIN")
    if env_bin:
        return Path(env_bin)

    path_bin = shutil.which("gigahorse")
    if path_bin:
        return Path(path_bin)

    return None


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


def docker_visible_root() -> Path:
    return Path.home() / ".cache" / "notdec-evm2llvm"


def build_gigahorse_command(
    args: argparse.Namespace, bytecode: Path, work_dir: Path
) -> tuple[list[str], Path | None]:
    common_args = [
        "-w",
        str(work_dir),
        "-j",
        str(args.jobs),
        "-T",
        str(args.timeout_secs),
        "--restart",
        *args.gigahorse_extra_arg,
        str(bytecode),
    ]

    if args.gigahorse_dir:
        gigahorse = args.gigahorse_dir / "gigahorse.py"
        if not gigahorse.exists():
            raise FileNotFoundError(f"missing gigahorse.py: {gigahorse}")
        return [sys.executable, str(gigahorse), *common_args], args.gigahorse_dir

    if args.gigahorse_bin:
        return [str(args.gigahorse_bin), *common_args], None

    raise FileNotFoundError(
        "missing Gigahorse; install the docker wrapper as `gigahorse`, pass "
        "--gigahorse-bin, or pass --gigahorse-dir"
    )


def prepare_bytecode_for_runner(
    args: argparse.Namespace, bytecode: Path, work_root: Path
) -> Path:
    if args.gigahorse_dir:
        return bytecode.resolve()

    # The official docker wrapper mounts $HOME, not arbitrary paths such as
    # /tmp or /sn640. Copy the input under $HOME so the container can read it.
    input_dir = work_root / "input"
    input_dir.mkdir(parents=True, exist_ok=True)
    copied = input_dir / bytecode.name
    shutil.copyfile(bytecode, copied)
    return copied


def check_docker_visible_path(path: Path) -> None:
    resolved = path.resolve()
    home = Path.home().resolve()
    if not resolved.is_relative_to(home):
        raise ValueError(
            f"{path} is not under $HOME. The official Gigahorse docker wrapper "
            "only mounts $HOME; pass a --work-dir under $HOME or force local "
            "Gigahorse with --gigahorse-dir."
        )


def warn_if_phi_incoming_missing(facts_dir: Path) -> None:
    phi_incoming = facts_dir / "PHIIncoming.csv"
    if phi_incoming.exists():
        return

    print(
        "warning: Gigahorse did not produce PHIIncoming.csv; "
        "PHI lowering will use the older slot fallback. Use a Gigahorse build "
        "with PHIIncoming support for predecessor-accurate PHI inputs.",
        file=sys.stderr,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Gigahorse and lower its facts to LLVM IR."
    )
    parser.add_argument("bytecode", type=Path, help="Input EVM bytecode .hex file.")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .ll path.")
    parser.add_argument(
        "--gigahorse-bin",
        type=Path,
        default=default_gigahorse_bin(),
        help="Gigahorse command. Defaults to $GIGAHORSE_BIN or `gigahorse` on PATH.",
    )
    parser.add_argument(
        "--gigahorse-dir",
        type=Path,
        default=None,
        help="Gigahorse checkout. Forces local gigahorse.py and overrides --gigahorse-bin.",
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
        args.gigahorse_dir = default_gigahorse_dir() if args.gigahorse_bin is None else None

    if not args.bytecode.exists():
        print(f"missing bytecode file: {args.bytecode}", file=sys.stderr)
        return 2

    remove_work_dir = False
    if args.work_dir:
        work_dir = args.work_dir
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        if args.gigahorse_dir:
            work_dir = Path(tempfile.mkdtemp(prefix="notdec-evm2llvm-"))
        else:
            docker_visible_root().mkdir(parents=True, exist_ok=True)
            work_dir = Path(
                tempfile.mkdtemp(
                    prefix="work-", dir=str(docker_visible_root())
                )
            )
        remove_work_dir = not args.keep_work_dir

    try:
        if not args.gigahorse_dir:
            check_docker_visible_path(work_dir)
        bytecode = prepare_bytecode_for_runner(args, args.bytecode, work_dir)
        gigahorse_cmd, cwd = build_gigahorse_command(args, bytecode, work_dir)
        run(gigahorse_cmd, cwd=cwd)

        facts_dir = work_dir / contract_name(args.bytecode) / "out"
        if not facts_dir.exists():
            print(f"Gigahorse did not create facts directory: {facts_dir}", file=sys.stderr)
            return 1

        warn_if_phi_incoming_missing(facts_dir)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        run([str(args.evm2llvm), "--facts", str(facts_dir), "--output", str(args.output)])
        return 0
    except (FileNotFoundError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    finally:
        if remove_work_dir:
            shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
