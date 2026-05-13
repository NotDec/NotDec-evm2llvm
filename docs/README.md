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
