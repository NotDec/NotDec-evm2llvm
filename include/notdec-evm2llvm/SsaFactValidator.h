#pragma once

#include "llvm/Support/Error.h"

#include "notdec-evm2llvm/TacProgram.h"

namespace notdec::evm2llvm {

// Checks the fact-level assumptions required before TAC variables can be
// lowered as SSA values. This pass only validates facts; it does not build IR.
llvm::Error validateSsaFacts(const TacProgram &program);

}  // namespace notdec::evm2llvm
