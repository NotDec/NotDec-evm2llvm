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
  --evm2llvm /path/to/evm2llvm
```

The script runs Gigahorse first, then passes the generated `out/` facts directory
to `evm2llvm`.

Gigahorse selection order:

1. `$GIGAHORSE_BIN`
2. `gigahorse` on `PATH`, usually the official Docker wrapper
3. `$GIGAHORSE_DIR/gigahorse.py`
4. `/sn640/gigahorse-toolchain/gigahorse.py`

Pass `--gigahorse-dir /path/to/gigahorse-toolchain` to force a local checkout.
When using the Docker wrapper, the script puts its temporary work directory under
`$HOME/.cache/notdec-evm2llvm` because the official wrapper only mounts `$HOME`.

End-to-end tests that run Gigahorse are off by default because the first run can
compile Souffle programs. Enable them explicitly:

```bash
cmake -S . -B build -G Ninja -DNOTDEC_EVM2LLVM_ENABLE_GIGAHORSE_TESTS=ON
ctest --test-dir build -R evm2llvm.gigahorse --output-on-failure
```

## Apehex Batch Loop

The small batch loop used during `apehex_evm_contracts` reruns now lives in
`/sn640/NotDecChainExp/evm2llvm_apehex_pilot/docs/apehex_batch_loop.md`.

It documents the helper script, the rerun flow for prior failures, and the
expected run-directory layout.

## Gigahorse PHI Source Runs

When investigating `PHIIncoming.csv` or other SSA facts, run Gigahorse with
inlining disabled by default.

At the moment, the inliner does not add much value for downstream
decompilation, so it is disabled by default. This keeps the exported CFG and
PHI sources closer to the original function boundaries, which makes it easier
to tell whether a problem comes from:

1. the original block-level PHI facts;
2. an inline rewrite that changed the return edge;
3. a later lowering rule that guessed a value after the fact.

The wrapper accepts extra Gigahorse flags, so the common form is:

```bash
scripts/notdec-evm2llvm.py contract.hex -o contract.ll \
  --evm2llvm /path/to/evm2llvm \
  --gigahorse-extra-arg=--restart \
  --gigahorse-extra-arg=--disable_inline
```

Only enable inlining when the test goal is specifically about inline behavior.
When re-running PHI investigations, keep `--restart` on so the new export does
not silently reuse an older work directory.
