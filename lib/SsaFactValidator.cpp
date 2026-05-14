#include "notdec-evm2llvm/SsaFactValidator.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "llvm/Support/Error.h"

namespace notdec::evm2llvm {
namespace {

llvm::Error makeError(const std::string &message) {
  return llvm::createStringError(std::errc::invalid_argument, "%s", message.c_str());
}

}  // namespace

llvm::Error validateSsaFacts(const TacProgram &program) {
  std::map<FactId, FactId> definingStmtByVar;
  std::map<FactId, const TacStatement *> stmtById;
  std::map<FactId, FactId> blockByStmt;
  std::map<FactId, FactId> functionByBlock;
  std::set<std::pair<FactId, FactId>> localEdges;
  bool hasPhiStmt = false;

  for (const auto &[functionId, function] : program.Functions) {
    for (const auto &blockId : function.Blocks) {
      functionByBlock[blockId] = functionId;
    }
  }

  for (const auto &[blockId, block] : program.Blocks) {
    for (const auto &successor : block.Successors) {
      localEdges.insert({blockId, successor});
    }

    for (const auto &stmt : block.Statements) {
      hasPhiStmt |= stmt.Op == "PHI";
      if (!stmtById.emplace(stmt.Id, &stmt).second) {
        return makeError("duplicate TAC statement id " + stmt.Id);
      }
      blockByStmt[stmt.Id] = blockId;

      std::set<FactId> defsInStmt;
      for (const auto &def : stmt.Defs) {
        if (!defsInStmt.insert(def).second) {
          return makeError("duplicate TAC def " + def + " in statement " + stmt.Id);
        }

        auto previous = definingStmtByVar.find(def);
        if (previous != definingStmtByVar.end() && previous->second != stmt.Id) {
          return makeError("duplicate TAC def for var " + def + " in statements " +
                           previous->second + " and " + stmt.Id);
        }
        definingStmtByVar[def] = stmt.Id;
      }
    }
  }

  if (hasPhiStmt && !program.HasPhiIncomingFacts) {
    return makeError("missing PHIIncoming.csv; SSA-only PHI lowering requires "
                     "Gigahorse facts with predecessor-specific PHI inputs");
  }

  for (const auto &[edge, incomingList] : program.PhiIncomingByEdge) {
    if (localEdges.count(edge) == 0) {
      return makeError("PHIIncoming edge is not a LocalBlockEdge: " +
                       edge.first + " -> " + edge.second);
    }

    auto predFunction = functionByBlock.find(edge.first);
    auto blockFunction = functionByBlock.find(edge.second);
    if (predFunction == functionByBlock.end()) {
      return makeError("PHIIncoming pred block has no function: " + edge.first);
    }
    if (blockFunction == functionByBlock.end()) {
      return makeError("PHIIncoming block has no function: " + edge.second);
    }
    if (predFunction->second != blockFunction->second) {
      return makeError("PHIIncoming crosses functions: " + edge.first + " -> " +
                       edge.second);
    }

    for (const auto &incoming : incomingList) {
      auto stmt = stmtById.find(incoming.PhiStmt);
      if (stmt == stmtById.end()) {
        return makeError("PHIIncoming references missing PHI statement " +
                         incoming.PhiStmt);
      }
      if (stmt->second->Op != "PHI") {
        return makeError("PHIIncoming references non-PHI statement " +
                         incoming.PhiStmt);
      }
      if (stmt->second->Defs.size() != 1) {
        return makeError("PHIIncoming statement must have exactly one def: " +
                         incoming.PhiStmt);
      }

      auto phiBlock = blockByStmt.find(incoming.PhiStmt);
      if (phiBlock == blockByStmt.end() || phiBlock->second != incoming.Block) {
        return makeError("PHIIncoming block does not own PHI statement " +
                         incoming.PhiStmt);
      }
      if (incoming.PredBlock != edge.first || incoming.Block != edge.second) {
        return makeError("PHIIncoming row does not match its edge for PHI " +
                         incoming.PhiStmt);
      }
    }
  }

  return llvm::Error::success();
}

}  // namespace notdec::evm2llvm
