#include "notdec-evm2llvm/EvmRuntimeDecls.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace notdec::evm2llvm {

void declareEvmRuntimeHelpers(llvm::Module &module) {
  auto &context = module.getContext();
  auto *wordType = llvm::Type::getIntNTy(context, 256);
  auto *ptrType = llvm::PointerType::get(context, 0);
  auto *voidType = llvm::Type::getVoidTy(context);

  module.getOrInsertFunction("evm_mload", wordType, ptrType, wordType);
  module.getOrInsertFunction("evm_mstore", voidType, ptrType, wordType, wordType);
  module.getOrInsertFunction("evm_sload", wordType, wordType);
  module.getOrInsertFunction("evm_sstore", voidType, wordType, wordType);
  module.getOrInsertFunction("evm_calldataload", wordType, ptrType, wordType);
  module.getOrInsertFunction("evm_calldatasize", wordType, ptrType);
  module.getOrInsertFunction("evm_return", voidType, ptrType, wordType, wordType);
  module.getOrInsertFunction("evm_revert", voidType, ptrType, wordType, wordType);
}

}  // namespace notdec::evm2llvm
