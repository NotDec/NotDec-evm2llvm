# NotDec EVM to LLVM IR Frontend

This directory keeps design notes and source references for
`external/NotDec-evm2llvm`.

The first implementation stage is intentionally small:

1. read Gigahorse CSV facts;
2. build a TAC/CFG/function model;
3. emit verifier-clean LLVM IR with `i256` EVM words;
4. keep EVM memory, calldata, returndata, env, and storage behind helper calls.

The Solidity internals notes live under `docs/solidity_internals_zh/` so the
project root stays focused on buildable source.

## Bytecode Wrapper

Use `scripts/notdec-evm2llvm.py` when starting from EVM bytecode:

```bash
scripts/notdec-evm2llvm.py contract.hex -o contract.ll \
  --gigahorse-dir /path/to/gigahorse-toolchain \
  --evm2llvm /path/to/evm2llvm
```

The script runs Gigahorse first, then passes the generated `out/` facts directory
to `evm2llvm`.

End-to-end tests that run Gigahorse are off by default because the first run can
compile Souffle programs. Enable them explicitly:

```bash
cmake -S . -B build -G Ninja -DNOTDEC_EVM2LLVM_ENABLE_GIGAHORSE_TESTS=ON
ctest --test-dir build -R evm2llvm.gigahorse --output-on-failure
```
