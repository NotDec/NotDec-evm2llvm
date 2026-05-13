#include "notdec-evm2llvm/TacProgram.h"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace notdec::evm2llvm {
namespace {

std::pair<unsigned long long, std::string_view> splitFactId(
    const FactId &id) {
  auto pos = id.find("0x");
  if (pos == std::string::npos) {
    return {0, id};
  }

  auto start = pos + 2;
  auto end = start;
  while (end < id.size() && std::isxdigit(static_cast<unsigned char>(id[end]))) {
    ++end;
  }

  auto numeric = std::strtoull(id.substr(start, end - start).c_str(), nullptr, 16);
  return {numeric, std::string_view(id).substr(end)};
}

}  // namespace

bool factIdLess(const FactId &lhs, const FactId &rhs) {
  auto [lhsNumber, lhsSuffix] = splitFactId(lhs);
  auto [rhsNumber, rhsSuffix] = splitFactId(rhs);
  if (lhsNumber != rhsNumber) {
    return lhsNumber < rhsNumber;
  }
  if (lhsSuffix != rhsSuffix) {
    return lhsSuffix < rhsSuffix;
  }
  return lhs < rhs;
}

std::string sanitizeLlvmName(const std::string &name) {
  std::string result;
  result.reserve(name.size());

  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
      result.push_back(c);
    } else {
      result.push_back('_');
    }
  }

  if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) {
    result.insert(result.begin(), '_');
  }
  return result;
}

}  // namespace notdec::evm2llvm
