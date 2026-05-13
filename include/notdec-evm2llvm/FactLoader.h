#pragma once

#include <string>

#include "llvm/Support/Error.h"

#include "notdec-evm2llvm/TacProgram.h"

namespace notdec::evm2llvm {

struct FactLoadConfig {
  std::string FactsDir;
};

llvm::Expected<TacProgram> loadFacts(const FactLoadConfig &config);

}  // namespace notdec::evm2llvm
