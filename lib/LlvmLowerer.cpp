#include "notdec-evm2llvm/LlvmLowerer.h"

#include <algorithm>
#include <map>
#include <set>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include "notdec-evm2llvm/EvmRuntimeDecls.h"
#include "notdec-evm2llvm/InstructionLowerer.h"

namespace notdec::evm2llvm {
namespace {

llvm::Error makeError(const std::string &message) {
  return llvm::createStringError(std::errc::invalid_argument, "%s", message.c_str());
}

bool containsBlock(const TacFunction &function, const FactId &blockId) {
  return std::find(function.Blocks.begin(), function.Blocks.end(), blockId) !=
         function.Blocks.end();
}

std::string functionName(const TacFunction &function) {
  return (function.IsPublic ? "public_" : "private_") +
         sanitizeLlvmName(function.Name + "_" + function.Id);
}

void collectFunctionVariables(const TacProgram &program, const TacFunction &function,
                              std::set<FactId> &variables) {
  for (const auto &formal : function.Formals) {
    variables.insert(formal);
  }

  for (const auto &blockId : function.Blocks) {
    auto block = program.Blocks.find(blockId);
    if (block == program.Blocks.end()) {
      continue;
    }
    for (const auto &stmt : block->second.Statements) {
      variables.insert(stmt.Uses.begin(), stmt.Uses.end());
      variables.insert(stmt.Defs.begin(), stmt.Defs.end());
    }
  }
}

llvm::Error lowerTerminator(const TacProgram &program, const TacFunction &function,
                            const TacBlock &block,
                            std::map<FactId, llvm::BasicBlock *> &llvmBlocks,
                            InstructionLowerer &instructionLowerer,
                            llvm::IRBuilder<> &builder) {
  std::vector<FactId> successors;
  for (const auto &successor : block.Successors) {
    if (containsBlock(function, successor)) {
      successors.push_back(successor);
    }
  }

  if (successors.empty()) {
    if (!block.Statements.empty() && block.Statements.back().Op == "REVERT") {
      builder.CreateUnreachable();
    } else {
      builder.CreateRetVoid();
    }
    return llvm::Error::success();
  }

  if (successors.size() == 1) {
    builder.CreateBr(llvmBlocks[successors[0]]);
    return llvm::Error::success();
  }

  if (successors.size() != 2) {
    return makeError("block " + block.Id + " has more than two successors");
  }

  if (block.Statements.empty() || block.Statements.back().Uses.empty()) {
    return makeError("conditional block " + block.Id + " has no condition use");
  }

  auto conditionOrError = instructionLowerer.loadWord(block.Statements.back().Uses[0]);
  if (!conditionOrError) {
    return conditionOrError.takeError();
  }

  auto *wordType = llvm::Type::getIntNTy(builder.getContext(), 256);
  auto *zero = llvm::ConstantInt::get(wordType, 0);
  auto *condition =
      builder.CreateICmpNE(*conditionOrError, zero, "evm.branch.cond");

  FactId falseBlock = block.FallthroughSuccessor.value_or(successors[1]);
  FactId trueBlock = successors[0] == falseBlock ? successors[1] : successors[0];
  if (!llvmBlocks.count(trueBlock) || !llvmBlocks.count(falseBlock)) {
    return makeError("conditional block " + block.Id +
                     " has a successor outside the current function");
  }

  builder.CreateCondBr(condition, llvmBlocks[trueBlock], llvmBlocks[falseBlock]);
  return llvm::Error::success();
}

llvm::Error lowerFunction(llvm::Module &module, const TacProgram &program,
                          const TacFunction &function) {
  auto &context = module.getContext();
  auto *wordType = llvm::Type::getIntNTy(context, 256);
  auto *ptrType = llvm::PointerType::get(context, 0);
  auto *voidType = llvm::Type::getVoidTy(context);

  std::vector<llvm::Type *> argTypes = {ptrType, ptrType, ptrType, ptrType};
  argTypes.insert(argTypes.end(), function.Formals.size(), wordType);
  auto *functionType = llvm::FunctionType::get(voidType, argTypes, false);
  auto *llvmFunction =
      llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                             functionName(function), module);

  const char *stateArgNames[] = {"mem", "calldata", "returndata", "env"};
  unsigned index = 0;
  for (auto &arg : llvmFunction->args()) {
    if (index < 4) {
      arg.setName(stateArgNames[index]);
    } else {
      arg.setName(sanitizeLlvmName(function.Formals[index - 4]));
    }
    ++index;
  }

  std::map<FactId, llvm::BasicBlock *> llvmBlocks;
  for (const auto &blockId : function.Blocks) {
    if (!program.Blocks.count(blockId)) {
      return makeError("function " + function.Id + " references missing block " + blockId);
    }
    llvmBlocks[blockId] =
        llvm::BasicBlock::Create(context, "bb." + sanitizeLlvmName(blockId), llvmFunction);
  }

  if (!llvmBlocks.count(function.EntryBlock)) {
    return makeError("function " + function.Id + " entry block is missing");
  }

  llvm::IRBuilder<> entryBuilder(llvmBlocks[function.EntryBlock]);
  std::set<FactId> variables;
  collectFunctionVariables(program, function, variables);

  std::map<FactId, llvm::AllocaInst *> slots;
  for (const auto &var : variables) {
    slots[var] = entryBuilder.CreateAlloca(wordType, nullptr, sanitizeLlvmName(var) + ".slot");
    entryBuilder.CreateStore(llvm::ConstantInt::get(wordType, 0), slots[var]);
  }

  index = 0;
  for (auto &arg : llvmFunction->args()) {
    if (index >= 4) {
      auto formalIndex = index - 4;
      entryBuilder.CreateStore(&arg, slots[function.Formals[formalIndex]]);
    }
    ++index;
  }

  for (const auto &blockId : function.Blocks) {
    const auto &block = program.Blocks.at(blockId);
    llvm::IRBuilder<> builder(llvmBlocks[blockId]);

    InstructionLowerer instructionLowerer(builder, context, wordType, slots, program);
    for (const auto &stmt : block.Statements) {
      if (auto error = instructionLowerer.lower(stmt)) {
        return error;
      }
    }

    if (auto error =
            lowerTerminator(program, function, block, llvmBlocks, instructionLowerer, builder)) {
      return error;
    }
  }

  return llvm::Error::success();
}

}  // namespace

llvm::Expected<std::unique_ptr<llvm::Module>> lowerToLlvm(
    llvm::LLVMContext &context, const TacProgram &program,
    const LlvmLowererConfig &config) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  declareEvmRuntimeHelpers(*module);

  for (const auto &[_, function] : program.Functions) {
    if (auto error = lowerFunction(*module, program, function)) {
      return std::move(error);
    }
  }

  return module;
}

}  // namespace notdec::evm2llvm
