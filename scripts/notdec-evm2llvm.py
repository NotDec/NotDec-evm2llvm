#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


EVM_OPCODES = {
    0x00: "STOP",
    0x01: "ADD",
    0x02: "MUL",
    0x03: "SUB",
    0x04: "DIV",
    0x05: "SDIV",
    0x06: "MOD",
    0x07: "SMOD",
    0x08: "ADDMOD",
    0x09: "MULMOD",
    0x0A: "EXP",
    0x0B: "SIGNEXTEND",
    0x10: "LT",
    0x11: "GT",
    0x12: "SLT",
    0x13: "SGT",
    0x14: "EQ",
    0x15: "ISZERO",
    0x16: "AND",
    0x17: "OR",
    0x18: "XOR",
    0x19: "NOT",
    0x1A: "BYTE",
    0x1B: "SHL",
    0x1C: "SHR",
    0x1D: "SAR",
    0x20: "SHA3",
    0x30: "ADDRESS",
    0x31: "BALANCE",
    0x32: "ORIGIN",
    0x33: "CALLER",
    0x34: "CALLVALUE",
    0x35: "CALLDATALOAD",
    0x36: "CALLDATASIZE",
    0x37: "CALLDATACOPY",
    0x38: "CODESIZE",
    0x39: "CODECOPY",
    0x3A: "GASPRICE",
    0x3B: "EXTCODESIZE",
    0x3C: "EXTCODECOPY",
    0x3D: "RETURNDATASIZE",
    0x3E: "RETURNDATACOPY",
    0x3F: "EXTCODEHASH",
    0x40: "BLOCKHASH",
    0x41: "COINBASE",
    0x42: "TIMESTAMP",
    0x43: "NUMBER",
    0x44: "PREVRANDAO",
    0x45: "GASLIMIT",
    0x46: "CHAINID",
    0x47: "SELFBALANCE",
    0x48: "BASEFEE",
    0x49: "BLOBHASH",
    0x4A: "BLOBBASEFEE",
    0x50: "POP",
    0x51: "MLOAD",
    0x52: "MSTORE",
    0x53: "MSTORE8",
    0x54: "SLOAD",
    0x55: "SSTORE",
    0x56: "JUMP",
    0x57: "JUMPI",
    0x58: "PC",
    0x59: "MSIZE",
    0x5A: "GAS",
    0x5B: "JUMPDEST",
    0x5F: "PUSH0",
    0xF0: "CREATE",
    0xF1: "CALL",
    0xF2: "CALLCODE",
    0xF3: "RETURN",
    0xF4: "DELEGATECALL",
    0xF5: "CREATE2",
    0xFA: "STATICCALL",
    0xFD: "REVERT",
    0xFE: "INVALID",
    0xFF: "SELFDESTRUCT",
}

for opcode in range(0x60, 0x80):
    EVM_OPCODES[opcode] = f"PUSH{opcode - 0x5F}"
for opcode in range(0x80, 0x90):
    EVM_OPCODES[opcode] = f"DUP{opcode - 0x7F}"
for opcode in range(0x90, 0xA0):
    EVM_OPCODES[opcode] = f"SWAP{opcode - 0x8F}"
for opcode in range(0xA0, 0xA5):
    EVM_OPCODES[opcode] = f"LOG{opcode - 0xA0}"


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
        "--results_file",
        str(work_dir / "results.json"),
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
        "evm2llvm now requires this fact when TAC PHI statements are present. "
        "Use a Gigahorse build with PHIIncoming support for predecessor-accurate "
        "PHI inputs.",
        file=sys.stderr,
    )


def read_bytecode_bytes(bytecode: Path) -> bytes:
    text = bytecode.read_text().strip()
    if text.startswith("0x"):
        text = text[2:]
    text = "".join(text.split())
    if len(text) % 2 != 0:
        raise ValueError(f"bytecode has an odd number of hex digits: {bytecode}")
    return bytes.fromhex(text)


def write_bytecode_listing(bytecode: Path, contract_dir: Path) -> None:
    code = read_bytecode_bytes(bytecode)
    listing = contract_dir / "evm-bytecode.txt"
    contract_dir.mkdir(parents=True, exist_ok=True)
    with listing.open("w") as file:
        file.write("pc\tbytes\topcode\toperand\n")
        pc = 0
        while pc < len(code):
            opcode = code[pc]
            name = EVM_OPCODES.get(opcode, f"UNKNOWN_0x{opcode:02x}")
            push_size = opcode - 0x5F if 0x60 <= opcode <= 0x7F else 0
            operand = code[pc + 1 : pc + 1 + push_size]
            raw = code[pc : pc + 1 + push_size]
            operand_text = "0x" + operand.hex() if operand else ""
            file.write(
                f"0x{pc:x}\t0x{raw.hex()}\t{name}\t{operand_text}\n"
            )
            pc += 1 + push_size


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

        write_bytecode_listing(args.bytecode, facts_dir.parent)
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
