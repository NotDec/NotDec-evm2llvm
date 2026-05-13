#pragma once

namespace llvm {
class Module;
}

namespace notdec::evm2llvm {

// Runtime helpers model EVM stateful behavior without pretending storage,
// calldata, or memory are native LLVM memory. Stage 1 only declares them.
void declareEvmRuntimeHelpers(llvm::Module &module);

}  // namespace notdec::evm2llvm
