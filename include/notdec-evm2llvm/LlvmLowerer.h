#pragma once

#include <memory>
#include <string>

#include "llvm/Support/Error.h"

#include "notdec-evm2llvm/TacProgram.h"

namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace notdec::evm2llvm {

struct LlvmLowererConfig {
  std::string ModuleName = "notdec.evm2llvm";
};

llvm::Expected<std::unique_ptr<llvm::Module>> lowerToLlvm(
    llvm::LLVMContext &context, const TacProgram &program,
    const LlvmLowererConfig &config);

}  // namespace notdec::evm2llvm
