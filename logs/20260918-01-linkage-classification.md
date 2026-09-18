# evm2llvm：按函数类别生成正确的 linkage（2026-09-18）

## 背景

NotDec 侧一直靠名字前缀区分"ABI 表面"和"内部函数"：

- `isPrivateHelperCall` / `AbiDecoderHelperRenamePass` / `EvmCalldataAccessPass`
  检查 `private__`；
- Solidity 后端的 helper 候选也检查 `private__`。

但 evm2llvm 生成的所有函数都是 external linkage
（`LlvmLowerer.cpp:createFunctionPrototype` 里写死 `ExternalLinkage`），
而函数名字是 `(IsPublic ? "public_" : "private_") + sanitize(Name + "_" + Id)`：

- `Name` 为空时得到 `private__0x1b7_0x1b7`（双下划线，现有检查能命中）；
- Gigahorse 恢复了高层名字时得到 `private_add_internal_0x10`（单下划线，现有检查漏掉）。

也就是说"内部函数"这件事既没有 IR 级信号，名字前缀法又不可靠。

## 改动

`lib/LlvmLowerer.cpp:createFunctionPrototype`：linkage 由 `TacFunction::IsPublic`
决定（`FactLoader.cpp` 里 `IsPublic = PublicFunction.csv 命中 || functionId == "0x0"`）：

- external：selector 入口和 `0x0` dispatcher（真正对外可调用的 ABI 表面）；
- internal：其余共享代码 outline 出来的 `private_*` 函数。

测试：`test/CMakeLists.txt` 新增 `evm2llvm.fixture.private_call.check-linkage`，
断言 private helper 是 `define internal ...@private_`、public 入口不是 internal。

## 验证

- `cmake --build build --target evm2llvm`；
- `ctest --test-dir build`：34/34 通过（含新增的 linkage 测试，其余 fixture 的
  emit / llvm-as / opt-verify 全部照常）；
- 生成 `private_call` fixture IR：

  ```llvm
  define void @public_fallback___0x0(ptr %mem, ptr %calldata, ptr %returndata, ptr %env) #0 {
  ...
  define internal i256 @private_add_internal_0x10(ptr %mem, ptr %calldata, ptr %returndata, ptr %env, i256 %_0xfarg0, i256 %_0xfarg1) #0 {
  ```

- **地址逃逸检查**：把全部 fixtures 重新生成一遍，扫描 `@private_*` 的非直接调用
  使用（`call ...@private_` 与 `define` 之外的使用），结果为 0。因此 internal
  linkage 不会让 LLVM 的 DCE 误删"实际会被间接调用"的函数（evm2llvm 目前只生成直接调用）。
- 端到端：用新 IR 跑 NotDec（`notdec private_call.ll -o out.sol --tr-level=2`）
  正常产出，没有崩溃或 verifier 报错。

## 影响与后续

- 这个改动让"内部函数"有了 IR 级判据，NotDec 侧可以逐步从名字前缀切到 linkage；
  切换前必须注意：`SelectorEntryOutliningPass` 自己创建的
  `public__notdec_solidity_selector_inline.body` 也是 internal linkage，
  所以不能简单用"internal 即 helper"，需要和入口判定组合。
- 语料（`NotDec/test/evm/**/cases|ir/*.ll`）是旧版 evm2llvm 生成的、仍是 external
  linkage；要让 linkage 判据在 batch 路径上生效，需要重新生成语料（带 Gigahorse 的
  跑批流程见 NotDec 的 `docs/evm/apehex-batch-loop.md`）。
- 已实测的另一个名字问题：`private_add_internal_0x10` 不会被 NotDec 现有的
  `private__` 检查命中（`private_` 单下划线），这也是切到 linkage 判据的动机之一。
