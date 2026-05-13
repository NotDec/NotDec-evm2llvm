#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace notdec::evm2llvm {

// Raw Gigahorse identifiers are kept as strings. They can contain contextual
// suffixes such as "0x1040x0", so treating them as integers would lose data.
using FactId = std::string;

// One Gigahorse TAC statement. Uses and defs are already sorted by their
// position column, so lowering code can read them in operand order.
struct TacStatement {
  FactId Id;
  std::string Op;
  std::vector<FactId> Uses;
  std::vector<FactId> Defs;
};

// One Gigahorse block. The converter does not infer CFG from jumps; successors
// and fallthrough come only from facts.
struct TacBlock {
  FactId Id;
  std::vector<TacStatement> Statements;
  std::vector<FactId> Successors;
  std::optional<FactId> FallthroughSuccessor;
};

// Function metadata from Gigahorse. The public/name fields affect emitted names
// only; function membership and formals are the semantic parts.
struct TacFunction {
  FactId Id;
  std::string Name;
  FactId EntryBlock;
  bool IsPublic = false;
  std::vector<FactId> Formals;
  std::vector<FactId> ReturnVars;
  std::vector<FactId> Blocks;
};

struct PrivateCallInfo {
  FactId CalleeFunction;
  std::vector<FactId> ReturnVars;
};

// Minimal in-memory model used by the first lowering stage. It mirrors the CSV
// boundary and avoids introducing Solidity or EVM-runtime semantics too early.
struct TacProgram {
  std::map<FactId, TacBlock> Blocks;
  std::map<FactId, TacFunction> Functions;
  std::map<FactId, std::string> VariableValues;
  std::map<FactId, PrivateCallInfo> PrivateCallsByBlock;
};

bool factIdLess(const FactId &lhs, const FactId &rhs);
std::string sanitizeLlvmName(const std::string &name);

}  // namespace notdec::evm2llvm
