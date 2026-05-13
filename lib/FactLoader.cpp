#include "notdec-evm2llvm/FactLoader.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

namespace notdec::evm2llvm {
namespace {

using Rows = std::vector<std::vector<std::string>>;

llvm::Error makeError(const std::string &message) {
  return llvm::createStringError(std::errc::invalid_argument, "%s", message.c_str());
}

std::string factPath(const std::string &dir, const std::string &fileName) {
  return (llvm::Twine(dir) + "/" + fileName).str();
}

llvm::Expected<Rows> loadRows(const std::string &dir,
                              const std::string &fileName,
                              unsigned expectedColumns) {
  auto path = factPath(dir, fileName);
  auto bufferOrError = llvm::MemoryBuffer::getFile(path);
  if (!bufferOrError) {
    return llvm::createStringError(
        bufferOrError.getError(),
        "failed to read required fact file " + path + ": " +
            bufferOrError.getError().message());
  }

  Rows rows;
  llvm::StringRef text = (*bufferOrError)->getBuffer();
  llvm::SmallVector<llvm::StringRef, 8> lines;
  text.split(lines, '\n', -1, false);

  for (auto line : lines) {
    line = line.rtrim("\r");
    if (line.empty()) {
      continue;
    }

    llvm::SmallVector<llvm::StringRef, 8> columns;
    line.split(columns, '\t', -1, false);
    if (columns.size() != expectedColumns) {
      return makeError(fileName + " has a row with " +
                       std::to_string(columns.size()) + " columns, expected " +
                       std::to_string(expectedColumns));
    }

    std::vector<std::string> row;
    row.reserve(columns.size());
    for (auto column : columns) {
      row.push_back(column.str());
    }
    rows.push_back(std::move(row));
  }

  return rows;
}

llvm::Expected<Rows> loadOptionalRows(const std::string &dir,
                                      const std::string &fileName,
                                      unsigned expectedColumns) {
  auto path = factPath(dir, fileName);
  if (!llvm::sys::fs::exists(path)) {
    return Rows{};
  }
  return loadRows(dir, fileName, expectedColumns);
}

template <typename T>
llvm::Error takeRows(llvm::Expected<Rows> rowsOrError, T &&fn) {
  if (!rowsOrError) {
    return rowsOrError.takeError();
  }
  for (const auto &row : *rowsOrError) {
    fn(row);
  }
  return llvm::Error::success();
}

}  // namespace

llvm::Expected<TacProgram> loadFacts(const FactLoadConfig &config) {
  TacProgram program;
  std::map<FactId, std::string> opByStmt;
  std::map<FactId, FactId> blockByStmt;
  std::map<FactId, std::vector<std::pair<unsigned, FactId>>> usesByStmt;
  std::map<FactId, std::vector<std::pair<unsigned, FactId>>> defsByStmt;
  std::map<FactId, std::vector<FactId>> blocksByFunction;
  std::map<FactId, FactId> functionByBlock;
  std::map<FactId, std::vector<std::pair<unsigned, FactId>>> formalsByFunction;
  std::map<FactId, FactId> privateCallByBlock;
  std::map<FactId, std::vector<std::pair<unsigned, FactId>>> actualReturnsByBlock;
  std::set<FactId> functions;
  std::set<FactId> entryBlocks;
  std::set<FactId> publicFunctions;
  std::map<FactId, std::string> namesByFunction;

  if (auto error = takeRows(loadRows(config.FactsDir, "TAC_Op.csv", 2),
                            [&](const auto &row) { opByStmt[row[0]] = row[1]; })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "TAC_Block.csv", 2),
                            [&](const auto &row) { blockByStmt[row[0]] = row[1]; })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "TAC_Use.csv", 3),
                            [&](const auto &row) {
                              usesByStmt[row[0]].push_back({std::stoul(row[2]), row[1]});
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "TAC_Def.csv", 3),
                            [&](const auto &row) {
                              defsByStmt[row[0]].push_back({std::stoul(row[2]), row[1]});
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "TAC_Variable_Value.csv", 2),
                            [&](const auto &row) {
                              program.VariableValues[row[0]] = row[1];
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "Function.csv", 1),
                            [&](const auto &row) { functions.insert(row[0]); })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "IRFunctionEntry.csv", 1),
                            [&](const auto &row) { entryBlocks.insert(row[0]); })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "InFunction.csv", 2),
                            [&](const auto &row) {
                              functionByBlock[row[0]] = row[1];
                              blocksByFunction[row[1]].push_back(row[0]);
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "FormalArgs.csv", 3),
                            [&](const auto &row) {
                              formalsByFunction[row[0]].push_back({std::stoul(row[2]), row[1]});
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadOptionalRows(config.FactsDir, "IRFunctionCall.csv", 2),
                            [&](const auto &row) {
                              privateCallByBlock[row[0]] = row[1];
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadOptionalRows(config.FactsDir, "ActualReturnArgs.csv", 3),
                            [&](const auto &row) {
                              actualReturnsByBlock[row[0]].push_back(
                                  {std::stoul(row[2]), row[1]});
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadOptionalRows(config.FactsDir, "PHIIncoming.csv", 4),
                            [&](const auto &row) {
                              PhiIncoming incoming{row[0], row[1], row[2], row[3]};
                              program.PhiIncomingByEdge[{incoming.PredBlock,
                                                         incoming.Block}]
                                  .push_back(std::move(incoming));
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadOptionalRows(config.FactsDir, "PublicFunction.csv", 2),
                            [&](const auto &row) { publicFunctions.insert(row[0]); })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadOptionalRows(config.FactsDir, "HighLevelFunctionName.csv", 2),
                            [&](const auto &row) { namesByFunction[row[0]] = row[1]; })) {
    return std::move(error);
  }

  for (const auto &[stmtId, blockId] : blockByStmt) {
    auto op = opByStmt.find(stmtId);
    if (op == opByStmt.end()) {
      return makeError("statement " + stmtId + " is in TAC_Block but missing TAC_Op");
    }

    auto &block = program.Blocks[blockId];
    block.Id = blockId;
    TacStatement stmt;
    stmt.Id = stmtId;
    stmt.Op = op->second;

    auto addOperands = [](const auto &source, std::vector<FactId> &target) {
      auto sorted = source;
      std::sort(sorted.begin(), sorted.end());
      for (const auto &[_, var] : sorted) {
        target.push_back(var);
      }
    };
    addOperands(usesByStmt[stmtId], stmt.Uses);
    addOperands(defsByStmt[stmtId], stmt.Defs);
    block.Statements.push_back(std::move(stmt));
  }

  for (auto &[_, block] : program.Blocks) {
    std::sort(block.Statements.begin(), block.Statements.end(),
              [](const TacStatement &lhs, const TacStatement &rhs) {
                return factIdLess(lhs.Id, rhs.Id);
              });
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "LocalBlockEdge.csv", 2),
                            [&](const auto &row) {
                              auto &block = program.Blocks[row[0]];
                              block.Id = row[0];
                              block.Successors.push_back(row[1]);
                            })) {
    return std::move(error);
  }

  if (auto error = takeRows(loadRows(config.FactsDir, "IRFallthroughEdge.csv", 2),
                            [&](const auto &row) {
                              auto &block = program.Blocks[row[0]];
                              block.Id = row[0];
                              block.FallthroughSuccessor = row[1];
                            })) {
    return std::move(error);
  }

  for (auto &[blockId, callee] : privateCallByBlock) {
    PrivateCallInfo call;
    call.CalleeFunction = callee;
    auto returns = actualReturnsByBlock[blockId];
    std::sort(returns.begin(), returns.end());
    for (const auto &[_, var] : returns) {
      call.ReturnVars.push_back(var);
    }
    program.PrivateCallsByBlock[blockId] = std::move(call);
  }

  for (const auto &functionId : functions) {
    auto blockList = blocksByFunction.find(functionId);
    if (blockList == blocksByFunction.end() || blockList->second.empty()) {
      return makeError("function " + functionId + " has no InFunction blocks");
    }

    TacFunction function;
    function.Id = functionId;
    function.IsPublic = publicFunctions.count(functionId) != 0 || functionId == "0x0";
    function.Name = namesByFunction.count(functionId) != 0
                        ? namesByFunction[functionId]
                        : "evm_function_" + functionId;
    function.Blocks = blockList->second;
    std::sort(function.Blocks.begin(), function.Blocks.end(), factIdLess);

    std::vector<std::pair<unsigned, FactId>> formals = formalsByFunction[functionId];
    std::sort(formals.begin(), formals.end());
    for (const auto &[_, var] : formals) {
      function.Formals.push_back(var);
    }

    std::vector<FactId> returnVars;
    for (const auto &blockId : function.Blocks) {
      auto block = program.Blocks.find(blockId);
      if (block == program.Blocks.end()) {
        continue;
      }
      for (const auto &stmt : block->second.Statements) {
        if (stmt.Op != "RETURNPRIVATE") {
          continue;
        }
        if (stmt.Uses.size() > returnVars.size() + 1) {
          returnVars.assign(stmt.Uses.begin() + 1, stmt.Uses.end());
        }
      }
    }
    function.ReturnVars = std::move(returnVars);

    for (const auto &entryBlock : entryBlocks) {
      auto owner = functionByBlock.find(entryBlock);
      if (owner != functionByBlock.end() && owner->second == functionId) {
        function.EntryBlock = entryBlock;
        break;
      }
    }
    if (function.EntryBlock.empty()) {
      return makeError("function " + functionId + " has no IRFunctionEntry block");
    }

    program.Functions[functionId] = std::move(function);
  }

  return program;
}

}  // namespace notdec::evm2llvm
