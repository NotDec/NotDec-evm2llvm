#include "notdec-evm2llvm/LlvmLowerer.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
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
#include "notdec-evm2llvm/SsaFactValidator.h"

namespace notdec::evm2llvm {
namespace {

llvm::Error makeError(const std::string &message) {
  return llvm::createStringError(std::errc::invalid_argument, "%s", message.c_str());
}

bool containsBlock(const TacFunction &function, const FactId &blockId) {
  return std::find(function.Blocks.begin(), function.Blocks.end(), blockId) !=
         function.Blocks.end();
}

void appendPostorder(const TacProgram &program, const TacFunction &function,
                     const FactId &blockId, std::set<FactId> &visited,
                     std::vector<FactId> &postorder) {
  if (!visited.insert(blockId).second) {
    return;
  }

  auto block = program.Blocks.find(blockId);
  if (block == program.Blocks.end()) {
    postorder.push_back(blockId);
    return;
  }

  auto successors = block->second.Successors;
  std::sort(successors.begin(), successors.end(), factIdLess);
  for (const auto &successor : successors) {
    if (containsBlock(function, successor)) {
      appendPostorder(program, function, successor, visited, postorder);
    }
  }
  postorder.push_back(blockId);
}

std::vector<FactId> functionBlockLoweringOrder(const TacProgram &program,
                                               const TacFunction &function) {
  std::set<FactId> visited;
  std::vector<FactId> postorder;
  appendPostorder(program, function, function.EntryBlock, visited, postorder);

  std::vector<FactId> order(postorder.rbegin(), postorder.rend());
  for (const auto &blockId : function.Blocks) {
    if (visited.count(blockId) == 0) {
      order.push_back(blockId);
    }
  }
  return order;
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

using PhiDefMap = std::map<FactId, FactId>;

llvm::Error emitPhiEdgeStores(const TacProgram &program,
                              const PhiDefMap &phiDefs,
                              const FactId &fromBlock,
                              const FactId &toBlock,
                              InstructionLowerer &instructionLowerer) {
  auto incoming = program.PhiIncomingByEdge.find({fromBlock, toBlock});
  if (incoming == program.PhiIncomingByEdge.end()) {
    return llvm::Error::success();
  }

  for (const auto &edgeValue : incoming->second) {
    auto def = phiDefs.find(edgeValue.PhiStmt);
    if (def == phiDefs.end()) {
      return makeError("PHI incoming references missing PHI statement " +
                       edgeValue.PhiStmt);
    }
    auto valueOrError = instructionLowerer.loadPhiEdgeWord(edgeValue.Var);
    if (!valueOrError) {
      return valueOrError.takeError();
    }
    if (auto error = instructionLowerer.storeWord(def->second, *valueOrError)) {
      return error;
    }
  }

  return llvm::Error::success();
}

// Conditional branches need edge blocks when a successor has PHI stores.
// Otherwise stores for both successors would execute before the branch.
llvm::Expected<llvm::BasicBlock *> edgeTargetWithPhiStores(
    const TacProgram &program, const PhiDefMap &phiDefs,
    const FactId &fromBlock, const FactId &toBlock,
    llvm::BasicBlock *toLlvmBlock,
    std::map<FactId, llvm::Value *> &values,
    std::map<FactId, llvm::AllocaInst *> &slots,
    RuntimeHandles handles, llvm::Type *wordType) {
  auto incoming = program.PhiIncomingByEdge.find({fromBlock, toBlock});
  if (incoming == program.PhiIncomingByEdge.end() || incoming->second.empty()) {
    return toLlvmBlock;
  }

  auto *function = toLlvmBlock->getParent();
  auto &context = function->getContext();
  auto *edgeBlock = llvm::BasicBlock::Create(
      context,
      "edge." + sanitizeLlvmName(fromBlock) + ".to." + sanitizeLlvmName(toBlock),
      function, toLlvmBlock);
  llvm::IRBuilder<> edgeBuilder(edgeBlock);
  InstructionLowerer edgeLowerer(edgeBuilder, context, wordType, values, slots,
                                 program, handles);

  if (auto error = emitPhiEdgeStores(program, phiDefs, fromBlock, toBlock,
                                     edgeLowerer)) {
    return std::move(error);
  }
  edgeBuilder.CreateBr(toLlvmBlock);
  return edgeBlock;
}

llvm::Error lowerTerminator(const TacProgram &program, const TacFunction &function,
                            const TacBlock &block,
                            std::map<FactId, llvm::BasicBlock *> &llvmBlocks,
                            std::map<FactId, llvm::Value *> &values,
                            std::map<FactId, llvm::AllocaInst *> &slots,
                            const PhiDefMap &phiDefs,
                            InstructionLowerer &instructionLowerer,
                            RuntimeHandles handles, llvm::Type *wordType,
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
    if (auto error = emitPhiEdgeStores(program, phiDefs, block.Id, successors[0],
                                       instructionLowerer)) {
      return error;
    }
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

  auto *conditionWordType = llvm::Type::getIntNTy(builder.getContext(), 256);
  auto *zero = llvm::ConstantInt::get(conditionWordType, 0);
  auto *condition =
      builder.CreateICmpNE(*conditionOrError, zero, "evm.branch.cond");

  FactId falseBlock = block.FallthroughSuccessor.value_or(successors[1]);
  FactId trueBlock = successors[0] == falseBlock ? successors[1] : successors[0];
  if (!llvmBlocks.count(trueBlock) || !llvmBlocks.count(falseBlock)) {
    return makeError("conditional block " + block.Id +
                     " has a successor outside the current function");
  }

  auto trueTargetOrError =
      edgeTargetWithPhiStores(program, phiDefs, block.Id, trueBlock,
                              llvmBlocks[trueBlock], values, slots, handles,
                              wordType);
  if (!trueTargetOrError) {
    return trueTargetOrError.takeError();
  }
  auto falseTargetOrError =
      edgeTargetWithPhiStores(program, phiDefs, block.Id, falseBlock,
                              llvmBlocks[falseBlock], values, slots, handles,
                              wordType);
  if (!falseTargetOrError) {
    return falseTargetOrError.takeError();
  }

  builder.CreateCondBr(condition, *trueTargetOrError, *falseTargetOrError);
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
    return instructionLowerer.defineWord(returnVars[0], result);
  }

  for (unsigned i = 0; i < returnVars.size(); ++i) {
    auto *value = builder.CreateExtractValue(result, {i}, "private.ret");
    if (auto error = instructionLowerer.defineWord(returnVars[i], value)) {
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
  PhiDefMap phiDefs;
  std::set<FactId> phisWithIncoming;
  std::set<FactId> phiEdgeSlotVars;
  for (const auto &blockId : function.Blocks) {
    const auto &block = program.Blocks.at(blockId);
    for (const auto &stmt : block.Statements) {
      if (stmt.Op != "PHI") {
        continue;
      }
      if (stmt.Defs.size() != 1) {
        return makeError("PHI expects one def at " + stmt.Id);
      }
      phiDefs[stmt.Id] = stmt.Defs[0];
    }
  }
  for (const auto &[edge, incomingList] : program.PhiIncomingByEdge) {
    if (!containsBlock(function, edge.first) || !containsBlock(function, edge.second)) {
      continue;
    }
    for (const auto &incoming : incomingList) {
      phisWithIncoming.insert(incoming.PhiStmt);
      phiEdgeSlotVars.insert(incoming.Var);
    }
  }
  for (const auto &phiStmt : phisWithIncoming) {
    phiEdgeSlotVars.insert(phiDefs.at(phiStmt));
  }

  std::map<FactId, llvm::Value *> values;
  std::map<FactId, llvm::AllocaInst *> slots;
  for (const auto &var : phiEdgeSlotVars) {
    slots[var] =
        entryBuilder.CreateAlloca(wordType, nullptr, sanitizeLlvmName(var) + ".slot");
    entryBuilder.CreateStore(llvm::ConstantInt::get(wordType, 0), slots[var]);
  }

  index = 0;
  for (auto &arg : llvmFunction->args()) {
    if (index >= 4) {
      auto formalIndex = index - 4;
      values[function.Formals[formalIndex]] = &arg;
    }
    ++index;
  }

  for (const auto &blockId : functionBlockLoweringOrder(program, function)) {
    const auto &block = program.Blocks.at(blockId);
    llvm::IRBuilder<> builder(llvmBlocks[blockId]);

    InstructionLowerer instructionLowerer(builder, context, wordType, values, slots,
                                          program, handles);
    for (const auto &stmt : block.Statements) {
      if (stmt.Op == "PHI" && phisWithIncoming.count(stmt.Id) != 0) {
        continue;
      }
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
            lowerTerminator(program, function, block, llvmBlocks, values, slots, phiDefs,
                            instructionLowerer, handles, wordType, builder)) {
      return error;
    }
  }

  return llvm::Error::success();
}

}  // namespace

llvm::Expected<std::unique_ptr<llvm::Module>> lowerToLlvm(
    llvm::LLVMContext &context, const TacProgram &program,
    const LlvmLowererConfig &config) {
  if (auto error = validateSsaFacts(program)) {
    return std::move(error);
  }

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
