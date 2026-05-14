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

struct RuntimeHandles {
  llvm::Value *Mem = nullptr;
  llvm::Value *Calldata = nullptr;
  llvm::Value *Returndata = nullptr;
  llvm::Value *Env = nullptr;
};

// Lowers one TAC instruction at a time into the current LLVM basic block.
// Values is the main TAC scalar state. Slots stay temporarily for PHI edge
// stores until native LLVM PHI lowering replaces that path.
class InstructionLowerer {
 public:
  InstructionLowerer(llvm::IRBuilder<> &builder, llvm::LLVMContext &context,
                     llvm::Type *wordType,
                     std::map<FactId, llvm::Value *> &values,
                     std::map<FactId, llvm::AllocaInst *> &slots,
                     const TacProgram &program, RuntimeHandles handles);

  llvm::Error lower(const TacStatement &stmt);
  llvm::Expected<llvm::Value *> loadWord(const FactId &var);
  llvm::Expected<llvm::Value *> loadPhiEdgeWord(const FactId &var);
  llvm::Error defineWord(const FactId &var, llvm::Value *value);
  llvm::Error storeWord(const FactId &var, llvm::Value *value);
  llvm::APInt parseWordConstant(const std::string &text) const;

 private:
  llvm::Expected<llvm::Value *> lowerUnary(const TacStatement &stmt);
  llvm::Expected<llvm::Value *> lowerBinary(const TacStatement &stmt);
  llvm::Expected<llvm::Value *> lowerStateRead(const TacStatement &stmt);
  llvm::Error lowerStateWrite(const TacStatement &stmt);
  llvm::Value *boolToWord(llvm::Value *value);
  llvm::Function *runtimeFunction(const char *name);

  llvm::IRBuilder<> &Builder;
  llvm::LLVMContext &Context;
  llvm::Type *WordType;
  std::map<FactId, llvm::Value *> &Values;
  std::map<FactId, llvm::AllocaInst *> &Slots;
  const TacProgram &Program;
  RuntimeHandles Handles;
};

}  // namespace notdec::evm2llvm
