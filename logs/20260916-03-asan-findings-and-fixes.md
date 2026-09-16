# ASan 结果与本轮修复（2026-09-16）

承接 `logs/20260916-02-p0-typebuilder-storage-crash-findings.md`。按建议用 AddressSanitizer 重建运行后，P0 崩溃的直接原因被实锤，并顺带修掉两个真实 bug。

## 构建与运行方式

    cmake -S /sn640/NotDec -B /tmp/build-evm-asan -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -fno-sanitize-recover=all" \
      -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
      -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"
    cmake --build /tmp/build-evm-asan --target notdec -j6
    ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/build-evm-asan -R "notdec.evm|notdec.type_recovery.evm" --output-on-failure

## 发现一：TypeBuilder::convert 的 InProgress 悬空读（已修）

ASan 在 EVM pattern suite 的 20 个用例上报告同一处 heap-use-after-free：

- READ 位置：`TypeBuilder::convert()` 入口，`std::get_if<URecursiveType>(&Ty->v)` 读 variant 判别字；
- 栈：`genTypes`(MLsubGenerator.cpp:7076) -> `convertStorageRecord` -> `buildNode` lambda -> `convert`；
- freed 位置：`InProgress.erase(Ty)`（`TypeBuilder.cpp:1154`）释放了 `std::_Rb_tree_node<binarysub::UType*>`。

机制：`convert()` 里原有的环检测是

    if (InProgress.count(Ty)) { assert(false && ...); }   // release 下断言被吃掉
    InProgress.insert(Ty);
    ...
    InProgress.erase(Ty);

未定锚的环会再次进入 `convert(Ty)`：`count` 命中 -> 断言被吃掉 -> 跳过 `insert` -> 结束时仍然执行 `erase(Ty)`。由于这次没有 insert，erase 释放的是**外层帧**的 set 节点；外层帧返回后继续读 `InProgress`/父帧读到被释放节点复用的内存，最终把垃圾地址当成 `UType*` 传给 `convert()`，表现为 `Unhandled UType variant` 断言或 SIGSEGV。

修复（binarysub `d1830b1`）：新增 `InProgressConversions`（`UType* -> 已完成的部分 HType*`），环再次进入时直接返回 in-progress 结果（成员还没构建完则返回 `top`），保证 `InProgress` 的 insert/erase 只有唯一 owner。

验证：ASan 下同一套 suite 的 heap-use-after-free 报告从 **20 个降到 0**；solidity-patterns 通过数 51 -> 63（ASan 构建）。

注意：`0014_proxy_like` 在 ASan 构建下修复后 exit=0 且 0 报告，但普通构建仍会在 `Unhandled UType variant` 断言处 abort，见下面"发现三"。

## 发现二：DangerousTypePatternScan 的 APInt 断言（已修）

`DangerousTypePatternScan` 在类型恢复之前运行，其 `normalizePointer()` 用 `APInt::getZExtValue()` 读 GEP 常量索引/加数；EVM IR 是 i256，常量超过 64 位就触发

    APInt.h:1544: Assertion `getActiveBits() <= 64 && "Too many bits for uint64_t"`

共 10 个 EVM 用例因此 abort。修复（主项目 `6d2f1b10`）：新增 `getConstantOffset()`，位宽 > 64 的常量按"无法建模的偏移"跳过（沿用该 pass 原有的保守回退），不再调用 `getZExtValue()`。

验证：`0189_19493609_3c0627c9e0_9d16fec0a1c2`、`0725_19498082_e78beb21f7_98e658f9eae8` 由 abort 变为正常完成；solidity-patterns 通过数 50 -> 53（普通构建）。

## 两轮修复后的 EVM 结果

| suite | 修复前（当前源码） | + ASan 环修复 | + 环修复 + APInt 修复 |
| --- | --- | --- | --- |
| solidity_patterns | 49 pass / 54 fail | 50 / 53 | **53 / 50** |
| solidity_rewrite | 72 / 3 | 72 / 3 | **74 / 1** |
| solidity_source | 77 / 1 | 77 / 1 | 77 / 1 |
| type_recovery.evm | 5 / 11 | 5 / 11 | 5 / 11 |
| ASan 报告数 | — | 0（首次已修） | 3（见发现三） |

ASan 构建下 solidity_patterns 63 / 40、type_recovery.evm 13 / 3；ASan 与普通构建的差异属预期（ASan 改变堆布局，部分 dangling 指针场景被掩盖）。

## 发现三：仍有 3 个用例在 ASan 下报 UAF（未修，已定位到方向）

修复一之后，ASan 下仍有 3 个用例报告 heap-use-after-free：`0032_19493098_f0dab0bf78_8c9406cb7887`、`0334_19494307_668d201319_1354ce2e324d`、`25928_19774281_d048a8d52d_2758caa02f46`。

特征与发现一不同：

- READ 仍是 `convert()` 入口读 `Ty->v`，经 `convertStorageRecord` 进入；
- 但 `Ty` 指向的是一块 **8 字节的 `std::vector<unsigned long>` 旧缓冲区**，其释放栈是：

      binarysub::CompactVarSet::cloneInto
      binarysub::CompactTypeArena::cloneVarSet
      binarysub::copyCompactTypeInto (binarysub.cpp:5001)

也就是说：**求解/简化往返（compact type -> copyCompactTypeInto -> UType）之后，storage 记录里有一个字段的 `UType*` 已经失效**，指向了别的对象（这里是变量集克隆的临时 vector）。`utype_pool()` 本身是进程级 append-only deque、节点不会释放，所以问题在"哪个 `TypeRef` 没跟着换到新 UType 节点"，而不是类型池释放。

下一步定位（建议）：

1. 在 `genTypes` 里 `bulkSimplifyDetailed` 返回后、`convertStorageRecord` 之前，遍历求解出的 storage `URecordType` 的每个字段，检查其指针是否等于 `getOrCreateStorageField` 当时 `make_variable` 的节点；找出换地址的那一个。
2. 若字段指针在 bulk 返回时就已失效，问题在 solver/compact 往返；若只是转换期间被覆盖，重点看 `copyCompactTypeInto`/`cloneVarSet` 生命周期。
3. 临时防线仍建议加：`convert()` 入口 `Ty->v.index() >= 10` 时丢弃该字段并告警（索引检查只读 variant 判别字，安全），避免再出 SIGSEGV；但 `index` 落在 0..9 的复用内存无法被它拦住。

## 其它观察

- `notdec.type_recovery.llvm_ir.tr_level_2`（8/14）和 `notdec.type_recovery.sysy.tr_level_2`（0/9）在修复前后都是 oracle 漂移：进程 exit=0，htypes 与期望不同（例如 `const(i32 1024)` 变成 `addr(0x400)`、多出 `struct_0` 字段）。这些 suite 的 oracle 也落后于当前源码，不属于本轮两个 bug 的回归——修复后没有任何一个 log 出现新增的 `cyclic UType` 警告。
- `notdec.lifting.wasm` 通过（0.43s）。
- `notdec.type_recovery.realworld.tr_level_2` 的 `fortune.o3.wasm` 失败与本轮改动无关：日志里是
  `Error: invalid MLsub extra constraints at ir_anchor.sha256: sha256 mismatch`
  （fixture `test/type-recovery/realworld/support/fortune.o3.wasm.extra.json` 的
  锚点哈希对不上当前输入），随后 `failExtraConstraints()` 主动 abort。
  属于 fixture 过期问题，需要单独刷新锚点哈希。

## 提交

- `external/binarysub` `d1830b1` TypeBuilder: return in-progress view for unanchored UType cycles
- `/sn640/NotDec` `6d2f1b10` passes: guard DangerousTypePatternScan constant offsets at 64 bits
- 本日志所在 evm2llvm 子模块提交见下一次 commit。
