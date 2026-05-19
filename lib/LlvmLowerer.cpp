#include "notdec-evm2llvm/LlvmLowerer.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
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

bool hasFunctionPredecessor(const TacProgram &program, const TacFunction &function,
                            const FactId &blockId) {
  for (const auto &predId : function.Blocks) {
    auto block = program.Blocks.find(predId);
    if (block == program.Blocks.end()) {
      continue;
    }
    if (std::find(block->second.Successors.begin(), block->second.Successors.end(),
                  blockId) != block->second.Successors.end()) {
      return true;
    }
  }
  return false;
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

const TacStatement *terminalStatement(const TacBlock &block) {
  for (auto it = block.Statements.rbegin(); it != block.Statements.rend(); ++it) {
    if (it->Op != "PHI") {
      return &*it;
    }
  }
  return nullptr;
}

const TacStatement *terminatorMetadataStatement(const TacBlock &block) {
  const auto *stmt = terminalStatement(block);
  if (stmt == nullptr) {
    return nullptr;
  }

  if (stmt->Op == "JUMP" || stmt->Op == "JUMPI" || stmt->Op == "STOP" ||
      stmt->Op == "THROW" || stmt->Op == "RETURNPRIVATE" ||
      stmt->Op == "RETURN" || stmt->Op == "REVERT") {
    return stmt;
  }
  return nullptr;
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

using PhiNodeMap = std::map<FactId, llvm::PHINode *>;

std::string joinFactIds(const std::vector<FactId> &ids) {
  std::string result;
  for (const auto &id : ids) {
    if (!result.empty()) {
      result += ",";
    }
    result += id;
  }
  return result;
}

llvm::MDNode *metadataForStmt(llvm::LLVMContext &context,
                              const TacProgram &program,
                              const TacStatement &stmt) {
  std::vector<llvm::Metadata *> fields = {
      llvm::MDString::get(context, "tac=" + stmt.Id),
      llvm::MDString::get(context, "op=" + stmt.Op),
  };

  auto originals = program.OriginalStatementsByStmt.find(stmt.Id);
  if (originals != program.OriginalStatementsByStmt.end() &&
      !originals->second.empty()) {
    fields.push_back(
        llvm::MDString::get(context, "evm.pc=" + joinFactIds(originals->second)));
  }

  auto inlineInfo = program.InlineInfoByStmt.find(stmt.Id);
  if (inlineInfo != program.InlineInfoByStmt.end() &&
      inlineInfo->second != "nil") {
    fields.push_back(
        llvm::MDString::get(context, "inline=" + inlineInfo->second));
  }

  return llvm::MDNode::get(context, fields);
}

void attachStmtMetadata(llvm::Instruction &instruction,
                        llvm::LLVMContext &context,
                        const TacProgram &program,
                        const TacStatement &stmt) {
  instruction.setMetadata("notdec.evm",
                          metadataForStmt(context, program, stmt));
}

class InsertedInstructionAnnotator {
 public:
  explicit InsertedInstructionAnnotator(llvm::BasicBlock &block)
      : Block(block), HadTail(!block.empty()) {
    if (HadTail) {
      Tail = std::prev(block.end());
    }
  }

  void annotate(llvm::LLVMContext &context, const TacProgram &program,
                const TacStatement *stmt) {
    if (stmt == nullptr) {
      return;
    }

    auto it = HadTail ? std::next(Tail) : Block.begin();
    for (; it != Block.end(); ++it) {
      attachStmtMetadata(*it, context, program, *stmt);
    }
  }

 private:
  llvm::BasicBlock &Block;
  bool HadTail = false;
  llvm::BasicBlock::iterator Tail;
};

llvm::APInt parseWordConstant(const std::string &text) {
  llvm::StringRef value(text);
  unsigned radix = 10;
  if (value.consume_front("0x") || value.consume_front("0X")) {
    radix = 16;
  }
  return llvm::APInt(256, value, radix);
}

llvm::Expected<llvm::Value *> valueForPhiIncoming(
    llvm::LLVMContext &context, const TacProgram &program,
    const std::map<FactId, llvm::Value *> &values, const FactId &var) {
  auto value = values.find(var);
  if (value != values.end()) {
    return value->second;
  }

  auto constant = program.VariableValues.find(var);
  if (constant != program.VariableValues.end()) {
    return llvm::ConstantInt::get(context, parseWordConstant(constant->second));
  }

  return makeError("missing SSA value for PHI incoming variable " + var);
}

llvm::Error fillPhiIncoming(const TacProgram &program, const TacFunction &function,
                            const std::map<FactId, llvm::BasicBlock *> &llvmBlocks,
                            const std::map<FactId, llvm::Value *> &values,
                            const PhiNodeMap &phiNodes,
                            llvm::LLVMContext &context) {
  for (const auto &[edge, incomingList] : program.PhiIncomingByEdge) {
    if (!containsBlock(function, edge.first) || !containsBlock(function, edge.second)) {
      continue;
    }

    auto predBlock = llvmBlocks.find(edge.first);
    if (predBlock == llvmBlocks.end()) {
      return makeError("PHI incoming predecessor has no LLVM block " + edge.first);
    }

    for (const auto &incoming : incomingList) {
      auto phi = phiNodes.find(incoming.PhiStmt);
      if (phi == phiNodes.end()) {
        return makeError("PHI incoming references a PHI without native node " +
                         incoming.PhiStmt);
      }

      auto valueOrError =
          valueForPhiIncoming(context, program, values, incoming.Var);
      if (!valueOrError) {
        return valueOrError.takeError();
      }
      phi->second->addIncoming(*valueOrError, predBlock->second);
    }
  }

  return llvm::Error::success();
}

llvm::Expected<llvm::Value *> entryPhiSeedValue(
    llvm::LLVMContext &context, const TacProgram &program, const TacFunction &function,
    const std::map<FactId, llvm::Value *> &values, const TacStatement &stmt) {
  std::set<FactId> edgeVars;
  for (const auto &[edge, incomingList] : program.PhiIncomingByEdge) {
    if (edge.second != function.EntryBlock || !containsBlock(function, edge.first)) {
      continue;
    }
    for (const auto &incoming : incomingList) {
      if (incoming.PhiStmt == stmt.Id) {
        edgeVars.insert(incoming.Var);
      }
    }
  }

  std::vector<FactId> candidates;
  for (const auto &use : stmt.Uses) {
    if (edgeVars.count(use) == 0) {
      candidates.push_back(use);
    }
  }
  if (candidates.size() != 1) {
    return makeError("entry PHI " + stmt.Id + " needs one initial value, found " +
                     std::to_string(candidates.size()));
  }
  return valueForPhiIncoming(context, program, values, candidates[0]);
}

llvm::Error fillSyntheticEntryPhiIncoming(
    const TacProgram &program, const TacFunction &function,
    llvm::BasicBlock *syntheticEntryBlock,
    const std::map<FactId, llvm::Value *> &values, const PhiNodeMap &phiNodes,
    llvm::LLVMContext &context) {
  if (syntheticEntryBlock == nullptr) {
    return llvm::Error::success();
  }

  const auto &entryBlock = program.Blocks.at(function.EntryBlock);
  for (const auto &stmt : entryBlock.Statements) {
    if (stmt.Op != "PHI") {
      continue;
    }

    auto phi = phiNodes.find(stmt.Id);
    if (phi == phiNodes.end()) {
      continue;
    }

    auto valueOrError = entryPhiSeedValue(context, program, function, values, stmt);
    if (!valueOrError) {
      return valueOrError.takeError();
    }
    phi->second->addIncoming(*valueOrError, syntheticEntryBlock);
  }
  return llvm::Error::success();
}

llvm::Error lowerTerminator(const TacProgram &program, const TacFunction &function,
                            const TacBlock &block,
                            std::map<FactId, llvm::BasicBlock *> &llvmBlocks,
                            std::map<FactId, llvm::Value *> &values,
                            InstructionLowerer &instructionLowerer,
                            llvm::IRBuilder<> &builder) {
  const auto *terminal = terminalStatement(block);
  if (terminal != nullptr) {
    if (terminal->Op == "REVERT" || terminal->Op == "THROW") {
      builder.CreateUnreachable();
      return llvm::Error::success();
    }
    if (terminal->Op == "RETURN" || terminal->Op == "STOP") {
      builder.CreateRetVoid();
      return llvm::Error::success();
    }
  }

  std::vector<FactId> successors;
  for (const auto &successor : block.Successors) {
    if (containsBlock(function, successor)) {
      successors.push_back(successor);
    }
  }

  if (successors.empty()) {
    if (terminal != nullptr && terminal->Op == "RETURNPRIVATE") {
      const auto &stmt = *terminal;
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

  if (terminal == nullptr || terminal->Uses.empty()) {
    return makeError("conditional block " + block.Id + " has no condition use");
  }

  auto conditionOrError = instructionLowerer.loadWord(terminal->Uses[0]);
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
  llvm::BasicBlock *syntheticEntryBlock = nullptr;
  if (hasFunctionPredecessor(program, function, function.EntryBlock)) {
    // LLVM forbids predecessors on the function entry block. Gigahorse can use a
    // real entry block as a loop header, so keep that block and jump to it from
    // a synthetic LLVM-only entry.
    syntheticEntryBlock =
        llvm::BasicBlock::Create(context, "entry", llvmFunction);
  }
  for (const auto &blockId : functionBlockLoweringOrder(program, function)) {
    if (!program.Blocks.count(blockId)) {
      return makeError("function " + function.Id + " references missing block " + blockId);
    }
    llvmBlocks[blockId] =
        llvm::BasicBlock::Create(context, "bb." + sanitizeLlvmName(blockId), llvmFunction);
  }

  if (!llvmBlocks.count(function.EntryBlock)) {
    return makeError("function " + function.Id + " entry block is missing");
  }

  if (syntheticEntryBlock != nullptr) {
    llvm::IRBuilder<> builder(syntheticEntryBlock);
    builder.CreateBr(llvmBlocks[function.EntryBlock]);
  }

  llvm::IRBuilder<> entryBuilder(llvmBlocks[function.EntryBlock]);
  PhiDefMap phiDefs;
  std::set<FactId> phisWithIncoming;
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
    }
  }

  std::map<FactId, llvm::Value *> values;
  PhiNodeMap phiNodes;

  index = 0;
  for (auto &arg : llvmFunction->args()) {
    if (index >= 4) {
      auto formalIndex = index - 4;
      values[function.Formals[formalIndex]] = &arg;
    }
    ++index;
  }

  // Create all native PHI placeholders before lowering instructions. This lets
  // loop-carried uses refer to the PHI result before incoming edges are known.
  for (const auto &blockId : functionBlockLoweringOrder(program, function)) {
    const auto &block = program.Blocks.at(blockId);
    llvm::IRBuilder<> builder(llvmBlocks[blockId]);
    for (const auto &stmt : block.Statements) {
      if (stmt.Op != "PHI" || phisWithIncoming.count(stmt.Id) == 0) {
        continue;
      }
      auto def = phiDefs.find(stmt.Id);
      if (def == phiDefs.end()) {
        return makeError("PHI has no def at " + stmt.Id);
      }
      auto *phi = builder.CreatePHI(wordType, 0, sanitizeLlvmName(def->second));
      attachStmtMetadata(*phi, context, program, stmt);
      phiNodes[stmt.Id] = phi;
      values[def->second] = phi;
    }
  }

  for (const auto &blockId : functionBlockLoweringOrder(program, function)) {
    const auto &block = program.Blocks.at(blockId);
    llvm::IRBuilder<> builder(llvmBlocks[blockId]);

    InstructionLowerer instructionLowerer(builder, context, wordType, values,
                                          program, handles);
    for (const auto &stmt : block.Statements) {
      if (stmt.Op == "PHI" && phisWithIncoming.count(stmt.Id) != 0) {
        continue;
      }
      if (stmt.Op == "CALLPRIVATE") {
        InsertedInstructionAnnotator annotator(*builder.GetInsertBlock());
        if (auto error = lowerPrivateCall(program, function, block, stmt, llvmFunctions,
                                          instructionLowerer, handles, builder)) {
          return error;
        }
        annotator.annotate(context, program, &stmt);
        continue;
      }
      if (stmt.Op == "RETURNPRIVATE") {
        continue;
      }
      InsertedInstructionAnnotator annotator(*builder.GetInsertBlock());
      if (auto error = instructionLowerer.lower(stmt)) {
        return error;
      }
      annotator.annotate(context, program, &stmt);
    }

    InsertedInstructionAnnotator terminatorAnnotator(*builder.GetInsertBlock());
    if (auto error =
            lowerTerminator(program, function, block, llvmBlocks, values,
                            instructionLowerer, builder)) {
      return error;
    }
    terminatorAnnotator.annotate(context, program,
                                 terminatorMetadataStatement(block));
  }

  if (auto error = fillPhiIncoming(program, function, llvmBlocks, values,
                                   phiNodes, context)) {
    return error;
  }
  if (auto error = fillSyntheticEntryPhiIncoming(program, function,
                                                 syntheticEntryBlock, values,
                                                 phiNodes, context)) {
    return error;
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
