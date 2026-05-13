#include "notdec-evm2llvm/LlvmLowerer.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
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

llvm::Type *returnTypeFor(llvm::LLVMContext &context,
                          const TacFunction &function) {
  auto *wordType = llvm::Type::getIntNTy(context, 256);
  if (function.ReturnVars.empty()) {
    return llvm::Type::getVoidTy(context);
  }
  if (function.ReturnVars.size() == 1) {
    return wordType;
  }
  return llvm::StructType::get(context,
                               std::vector<llvm::Type *>(function.ReturnVars.size(),
                                                         wordType));
}

llvm::Function *createFunctionPrototype(llvm::Module &module,
                                        const TacFunction &function) {
  auto &context = module.getContext();
  auto *wordType = llvm::Type::getIntNTy(context, 256);
  auto *ptrType = llvm::PointerType::get(context, 0);

  std::vector<llvm::Type *> argTypes = {ptrType, ptrType, ptrType, ptrType};
  argTypes.insert(argTypes.end(), function.Formals.size(), wordType);
  auto *functionType =
      llvm::FunctionType::get(returnTypeFor(context, function), argTypes, false);
  return llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                functionName(function), module);
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
    if (!block.Statements.empty() &&
        (block.Statements.back().Op == "REVERT" ||
         block.Statements.back().Op == "THROW")) {
      builder.CreateUnreachable();
    } else if (!block.Statements.empty() &&
               block.Statements.back().Op == "RETURNPRIVATE") {
      const auto &stmt = block.Statements.back();
      if (stmt.Uses.size() != function.ReturnVars.size() + 1) {
        return makeError("RETURNPRIVATE return count mismatch at " + stmt.Id);
      }

      if (function.ReturnVars.empty()) {
        builder.CreateRetVoid();
      } else if (function.ReturnVars.size() == 1) {
        auto valueOrError = instructionLowerer.loadWord(stmt.Uses[1]);
        if (!valueOrError) {
          return valueOrError.takeError();
        }
        builder.CreateRet(*valueOrError);
      } else {
        auto *returnType = llvm::cast<llvm::StructType>(
            returnTypeFor(builder.getContext(), function));
        llvm::Value *aggregate = llvm::PoisonValue::get(returnType);
        for (unsigned i = 0; i < function.ReturnVars.size(); ++i) {
          auto valueOrError = instructionLowerer.loadWord(stmt.Uses[i + 1]);
          if (!valueOrError) {
            return valueOrError.takeError();
          }
          aggregate =
              builder.CreateInsertValue(aggregate, *valueOrError, {i}, "ret.insert");
        }
        builder.CreateRet(aggregate);
      }
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

llvm::Error lowerPrivateCall(
    const TacProgram &program, const TacFunction &function, const TacBlock &block,
    const TacStatement &stmt,
    const std::map<FactId, llvm::Function *> &llvmFunctions,
    InstructionLowerer &instructionLowerer, RuntimeHandles handles,
    llvm::IRBuilder<> &builder) {
  auto call = program.PrivateCallsByBlock.find(block.Id);
  if (call == program.PrivateCallsByBlock.end()) {
    return makeError("CALLPRIVATE block " + block.Id + " has no IRFunctionCall");
  }

  auto calleeFunction = program.Functions.find(call->second.CalleeFunction);
  if (calleeFunction == program.Functions.end()) {
    return makeError("CALLPRIVATE block " + block.Id + " references missing function " +
                     call->second.CalleeFunction);
  }

  auto llvmCallee = llvmFunctions.find(call->second.CalleeFunction);
  if (llvmCallee == llvmFunctions.end()) {
    return makeError("CALLPRIVATE callee " + call->second.CalleeFunction +
                     " has no LLVM function");
  }

  if (stmt.Uses.size() != calleeFunction->second.Formals.size() + 1) {
    return makeError("CALLPRIVATE argument count mismatch at " + stmt.Id);
  }

  std::vector<llvm::Value *> args = {handles.Mem, handles.Calldata,
                                    handles.Returndata, handles.Env};
  for (unsigned i = 1; i < stmt.Uses.size(); ++i) {
    auto valueOrError = instructionLowerer.loadWord(stmt.Uses[i]);
    if (!valueOrError) {
      return valueOrError.takeError();
    }
    args.push_back(*valueOrError);
  }

  llvm::Value *result = builder.CreateCall(llvmCallee->second, args,
                                           stmt.Defs.empty() ? "" : "private.call");

  const auto &returnVars =
      call->second.ReturnVars.empty() ? stmt.Defs : call->second.ReturnVars;
  if (returnVars.size() != calleeFunction->second.ReturnVars.size()) {
    return makeError("CALLPRIVATE return count mismatch at " + stmt.Id);
  }
  if (returnVars.empty()) {
    return llvm::Error::success();
  }
  if (returnVars.size() == 1) {
    return instructionLowerer.storeWord(returnVars[0], result);
  }

  for (unsigned i = 0; i < returnVars.size(); ++i) {
    auto *value = builder.CreateExtractValue(result, {i}, "private.ret");
    if (auto error = instructionLowerer.storeWord(returnVars[i], value)) {
      return error;
    }
  }
  return llvm::Error::success();
}

llvm::Error lowerFunction(llvm::Module &module, const TacProgram &program,
                          const TacFunction &function, llvm::Function *llvmFunction,
                          const std::map<FactId, llvm::Function *> &llvmFunctions) {
  auto &context = module.getContext();
  auto *wordType = llvm::Type::getIntNTy(context, 256);

  const char *stateArgNames[] = {"mem", "calldata", "returndata", "env"};
  RuntimeHandles handles;
  unsigned index = 0;
  for (auto &arg : llvmFunction->args()) {
    if (index < 4) {
      arg.setName(stateArgNames[index]);
      if (index == 0) {
        handles.Mem = &arg;
      } else if (index == 1) {
        handles.Calldata = &arg;
      } else if (index == 2) {
        handles.Returndata = &arg;
      } else if (index == 3) {
        handles.Env = &arg;
      }
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

    InstructionLowerer instructionLowerer(builder, context, wordType, slots, program,
                                          handles);
    for (const auto &stmt : block.Statements) {
      if (stmt.Op == "CALLPRIVATE") {
        if (auto error = lowerPrivateCall(program, function, block, stmt, llvmFunctions,
                                          instructionLowerer, handles, builder)) {
          return error;
        }
        continue;
      }
      if (stmt.Op == "RETURNPRIVATE") {
        continue;
      }
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

  std::map<FactId, llvm::Function *> llvmFunctions;
  for (const auto &[functionId, function] : program.Functions) {
    llvmFunctions[functionId] = createFunctionPrototype(*module, function);
  }

  for (const auto &[_, function] : program.Functions) {
    if (auto error = lowerFunction(*module, program, function,
                                   llvmFunctions.at(function.Id), llvmFunctions)) {
      return std::move(error);
    }
  }

  return module;
}

}  // namespace notdec::evm2llvm
