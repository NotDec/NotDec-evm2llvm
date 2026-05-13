#include "notdec-evm2llvm/InstructionLowerer.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"

namespace notdec::evm2llvm {
namespace {

llvm::Error unsupported(const TacStatement &stmt) {
  return llvm::createStringError(std::errc::not_supported,
                                 "unsupported opcode %s at %s", stmt.Op.c_str(),
                                 stmt.Id.c_str());
}

}  // namespace

InstructionLowerer::InstructionLowerer(
    llvm::IRBuilder<> &builder, llvm::LLVMContext &context, llvm::Type *wordType,
    std::map<FactId, llvm::AllocaInst *> &slots, const TacProgram &program)
    : Builder(builder),
      Context(context),
      WordType(wordType),
      Slots(slots),
      Program(program) {}

llvm::APInt InstructionLowerer::parseWordConstant(const std::string &text) const {
  llvm::StringRef value(text);
  unsigned radix = 10;
  if (value.consume_front("0x") || value.consume_front("0X")) {
    radix = 16;
  }
  return llvm::APInt(256, value, radix);
}

llvm::Expected<llvm::Value *> InstructionLowerer::loadWord(const FactId &var) {
  auto slot = Slots.find(var);
  if (slot != Slots.end()) {
    return Builder.CreateLoad(WordType, slot->second, sanitizeLlvmName(var) + ".load");
  }

  auto constant = Program.VariableValues.find(var);
  if (constant != Program.VariableValues.end()) {
    return llvm::ConstantInt::get(Context, parseWordConstant(constant->second));
  }

  return llvm::createStringError(std::errc::invalid_argument,
                                 "variable %s has no slot or constant value", var.c_str());
}

llvm::Error InstructionLowerer::storeWord(const FactId &var, llvm::Value *value) {
  auto slot = Slots.find(var);
  if (slot == Slots.end()) {
    return llvm::createStringError(std::errc::invalid_argument,
                                   "variable %s has no slot", var.c_str());
  }
  Builder.CreateStore(value, slot->second);
  return llvm::Error::success();
}

llvm::Value *InstructionLowerer::boolToWord(llvm::Value *value) {
  return Builder.CreateZExt(value, WordType, "evm.bool");
}

llvm::Expected<llvm::Value *> InstructionLowerer::lowerUnary(
    const TacStatement &stmt) {
  if (stmt.Uses.size() != 1) {
    return llvm::createStringError(std::errc::invalid_argument,
                                   "%s expects one operand at %s", stmt.Op.c_str(),
                                   stmt.Id.c_str());
  }

  auto inputOrError = loadWord(stmt.Uses[0]);
  if (!inputOrError) {
    return inputOrError.takeError();
  }
  auto *input = *inputOrError;

  if (stmt.Op == "MOV") {
    return input;
  }
  if (stmt.Op == "NOT") {
    return Builder.CreateXor(input, llvm::ConstantInt::get(Context, llvm::APInt::getAllOnes(256)),
                             "evm.not");
  }
  if (stmt.Op == "ISZERO") {
    auto *zero = llvm::ConstantInt::get(WordType, 0);
    return boolToWord(Builder.CreateICmpEQ(input, zero, "evm.iszero"));
  }

  return unsupported(stmt);
}

llvm::Expected<llvm::Value *> InstructionLowerer::lowerBinary(
    const TacStatement &stmt) {
  if (stmt.Uses.size() != 2) {
    return llvm::createStringError(std::errc::invalid_argument,
                                   "%s expects two operands at %s", stmt.Op.c_str(),
                                   stmt.Id.c_str());
  }

  auto lhsOrError = loadWord(stmt.Uses[0]);
  if (!lhsOrError) {
    return lhsOrError.takeError();
  }
  auto rhsOrError = loadWord(stmt.Uses[1]);
  if (!rhsOrError) {
    return rhsOrError.takeError();
  }

  auto *lhs = *lhsOrError;
  auto *rhs = *rhsOrError;

  if (stmt.Op == "ADD") {
    return Builder.CreateAdd(lhs, rhs, "evm.add");
  }
  if (stmt.Op == "SUB") {
    return Builder.CreateSub(lhs, rhs, "evm.sub");
  }
  if (stmt.Op == "MUL") {
    return Builder.CreateMul(lhs, rhs, "evm.mul");
  }
  if (stmt.Op == "AND") {
    return Builder.CreateAnd(lhs, rhs, "evm.and");
  }
  if (stmt.Op == "OR") {
    return Builder.CreateOr(lhs, rhs, "evm.or");
  }
  if (stmt.Op == "XOR") {
    return Builder.CreateXor(lhs, rhs, "evm.xor");
  }
  if (stmt.Op == "EQ") {
    return boolToWord(Builder.CreateICmpEQ(lhs, rhs, "evm.eq"));
  }
  if (stmt.Op == "LT") {
    return boolToWord(Builder.CreateICmpULT(lhs, rhs, "evm.lt"));
  }
  if (stmt.Op == "GT") {
    return boolToWord(Builder.CreateICmpUGT(lhs, rhs, "evm.gt"));
  }
  if (stmt.Op == "SLT") {
    return boolToWord(Builder.CreateICmpSLT(lhs, rhs, "evm.slt"));
  }
  if (stmt.Op == "SGT") {
    return boolToWord(Builder.CreateICmpSGT(lhs, rhs, "evm.sgt"));
  }

  return unsupported(stmt);
}

llvm::Error InstructionLowerer::lower(const TacStatement &stmt) {
  if (stmt.Op == "JUMP" || stmt.Op == "JUMPI" || stmt.Op == "STOP" ||
      stmt.Op == "RETURN" || stmt.Op == "REVERT") {
    return llvm::Error::success();
  }

  llvm::Value *value = nullptr;

  if (stmt.Op == "CONST") {
    if (stmt.Defs.size() != 1) {
      return llvm::createStringError(std::errc::invalid_argument,
                                     "CONST expects one def at %s", stmt.Id.c_str());
    }
    auto constant = Program.VariableValues.find(stmt.Defs[0]);
    if (constant == Program.VariableValues.end()) {
      return llvm::createStringError(std::errc::invalid_argument,
                                     "CONST def %s has no TAC_Variable_Value",
                                     stmt.Defs[0].c_str());
    }
    value = llvm::ConstantInt::get(Context, parseWordConstant(constant->second));
  } else if (stmt.Op == "MOV" || stmt.Op == "NOT" || stmt.Op == "ISZERO") {
    auto valueOrError = lowerUnary(stmt);
    if (!valueOrError) {
      return valueOrError.takeError();
    }
    value = *valueOrError;
  } else if (stmt.Op == "ADD" || stmt.Op == "SUB" || stmt.Op == "MUL" ||
             stmt.Op == "AND" || stmt.Op == "OR" || stmt.Op == "XOR" ||
             stmt.Op == "EQ" || stmt.Op == "LT" || stmt.Op == "GT" ||
             stmt.Op == "SLT" || stmt.Op == "SGT") {
    auto valueOrError = lowerBinary(stmt);
    if (!valueOrError) {
      return valueOrError.takeError();
    }
    value = *valueOrError;
  } else {
    return unsupported(stmt);
  }

  if (stmt.Defs.empty()) {
    return llvm::Error::success();
  }
  if (stmt.Defs.size() != 1) {
    return llvm::createStringError(std::errc::not_supported,
                                   "%s has multiple defs at %s", stmt.Op.c_str(),
                                   stmt.Id.c_str());
  }
  return storeWord(stmt.Defs[0], value);
}

}  // namespace notdec::evm2llvm
