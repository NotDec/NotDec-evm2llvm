#pragma once

#include <map>
#include <string>

#include "llvm/ADT/APInt.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"

#include "notdec-evm2llvm/TacProgram.h"

namespace llvm {
class AllocaInst;
class Function;
class LLVMContext;
class Type;
class Value;
}  // namespace llvm

namespace notdec::evm2llvm {

// Lowers one TAC instruction at a time into the current LLVM basic block. The
// variable storage map is owned by LlvmLowerer because slots are per function.
class InstructionLowerer {
 public:
  InstructionLowerer(llvm::IRBuilder<> &builder, llvm::LLVMContext &context,
                     llvm::Type *wordType,
                     std::map<FactId, llvm::AllocaInst *> &slots,
                     const TacProgram &program);

  llvm::Error lower(const TacStatement &stmt);
  llvm::Expected<llvm::Value *> loadWord(const FactId &var);
  llvm::APInt parseWordConstant(const std::string &text) const;

 private:
  llvm::Error storeWord(const FactId &var, llvm::Value *value);
  llvm::Expected<llvm::Value *> lowerUnary(const TacStatement &stmt);
  llvm::Expected<llvm::Value *> lowerBinary(const TacStatement &stmt);
  llvm::Value *boolToWord(llvm::Value *value);

  llvm::IRBuilder<> &Builder;
  llvm::LLVMContext &Context;
  llvm::Type *WordType;
  std::map<FactId, llvm::AllocaInst *> &Slots;
  const TacProgram &Program;
};

}  // namespace notdec::evm2llvm
