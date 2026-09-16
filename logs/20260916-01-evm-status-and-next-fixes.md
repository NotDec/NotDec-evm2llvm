# evm2llvm 状态盘点与下一步修复（2026-09-16）

## 本次 prompt 与结论

> 看看最新的 evm2llvm 的开发日志是做到哪一步了，下面该修复什么问题。
> 可能日志写到主项目那边 /sn640/NotDec/logs/ 下了？后面日志还是在这边单独创建一个 logs/ 写到里面吧。

- 最新的 evm2llvm 日志在主项目日志目录的 archive 下：
  `/sn640/NotDec/logs/archive/20260602-06-evm-native-load-store-memory-lowering-plan.md`
  （最后追加记录到 2026-06-03，内容：native load/store + 删除旧 helper + alloc 改 calloc）。
- evm2llvm 子模块最后一次代码提交：`e79e244 Ignore local build directory`
  （2026-06-03），此后没有新的 evm2llvm 改动。
- 结论：evm2llvm 源头（阶段 1/2）当时收尾了，但**整条 EVM 链路在当前 NotDec
  main（2026-09-15 `0c9bf33c`）上是坏的**：EVM 4 个 ctest suite 全部失败，
  甚至 6 月 4 日 apehex 批次里原本 ok 的样本现在会 abort。
- 因此"下面该修什么"的答案不是继续加新的语义 pass，而是先修当前回归：
  一个 binarysub TypeBuilder 崩溃 + 两个 evm2llvm 真实 lowering bug + 一批
  过期的 EVM oracle。
- 本文件起，evm2llvm 自身相关日志写到 `external/NotDec-evm2llvm/logs/`，
  不再写主项目 `/sn640/NotDec/logs/`。

## 一、日志/时间线盘点

| 时间 | 位置 | 内容 |
| --- | --- | --- |
| 2026-05-11 ~ 05-21 | `/sn640/NotDec/logs/archive/*Evm2llvm*.md` | Gigahorse facts -> LLVM 前端主线、SSA lowering、jumpi/jump table、poison、apehex 批次问题 |
| 2026-05-22 | `20260522-01-Evm2llvmSolidityPassOrderingPlan.md` | Solidity pass 顺序 |
| 2026-06-02 | `20260602-05-evm-memory-object-type-recovery-pass-ordering-plan.md` | memory object / 类型恢复 pass 顺序 |
| 2026-06-02 | `archive/20260602-06-evm-native-load-store-memory-lowering-plan.md` | **最后一篇 evm2llvm 主线日志**：阶段 1（源头改 native `inttoptr`+load/store，新增 `--memory-model inttoptr|global-array`）、阶段 2（主项目 matcher 迁移 + 删除旧 helper 兼容 + alloc 改 `calloc`） |
| 2026-06-03 | `20260603-02-evm-native-memory-current-status.md` | 当时状态 3/3 EVM ctest 通过；下一步建议：native load/store 接 MLsub、更新 `04_evm_memory_helpers.ll`、接 `calloc/ptrtoint/inttoptr`、动态 offset 保守 |
| 2026-06-04 | `20260604-01-evm-type-recovery-next-plan.md` | 选 80 个 apehex pilot；最小回归 case；多返回/PNDiff 规则 |
| 2026-06-04 | `/sn640/NotDecChainExp/evm_type_recovery_apehex_pilot/20260604-selected80-missing50` | 50 样本批次：36 ok / 14 fail（10 个 notdec_tr 崩溃、3 timeout、1 PHI） |
| 2026-06-05 | `/sn640/NotDecChainExp/notdec_reduce/{18404,7435}` | 对 timeout 样本做 llvm-reduce（未留下结论性日志） |
| 2026-06-12 ~ 06-16 | `/sn640/NotDec2/logs/2026061x-0x-evm-*.md` | EVM 高层语义 / calldata / storage 类型推理规划（写在 NotDec2 工作树） |
| 2026-06-11 ~ 09-16 | 主项目 evm 相关提交 | 抽取 `notdec-typerecovery` / `notdec-evm-pattern-utils` pass library、安装 pkg-config；`0c9bf33c` 取消 inttoptr 常量字段的 EVM 模块 gating |

要点：evm2llvm 本体的日志在 **06-03 就断了**；之后主线上发生的是主项目侧
（类型恢复、pass library 抽取、MicroSub2 集成）的大改动，而 EVM 链路的
回归没有被重新验证。

## 二、当前 EVM 链路实测状态（notdec main 0c9bf33c，binarysub d361eb7）

构建：`/sn640/NotDec/build-notdec-nothreads2`（`bin/notdec`，2026-09-15 05:55 构建）。

```bash
ctest --test-dir build-notdec-nothreads2 -R 'notdec.evm|notdec.type_recovery.evm' --output-on-failure
```

结果：**4/4 suite 失败**

| suite | 结果 | 失败特征 |
| --- | --- | --- |
| `notdec.evm.solidity_patterns` | 76 pass / **27 fail** | 大部分 `exit=-6`，崩在类型恢复 |
| `notdec.evm.solidity_rewrite` | 72 pass / **3 fail** | `storage_bytes_long_high_level_rewrite` 等 |
| `notdec.evm.solidity_source` | 77 pass / **1 fail** | `apehex_gasleft_return_01`，同样是类型恢复 abort |
| `notdec.type_recovery.evm.tr_level_2` | 6 pass / **10 fail** | 见下表 |

类型恢复 EVM suite 明细（共 16 case）：

- **崩溃（3）**：`10_evm_storage_bytes_long`（无 .htypes 产物）、并伴随 storage 相关
  的 pattern/rewrite/source 用例一起崩。
- **oracle 漂移（6）**：`05_evm_aggregate_return_pndiff_sub`、`09`、`11`、`12`、`13`、
  `14`、`15`、`16`（部分为同一 `top:0 whole` 字段差异）。
- **输出差异（1+）**：`01_evm_heap_i256` 的 `main::arg0` 标记与结尾空行。

样例 A（`09_evm_storage_bytes_short`）差异只有两处，属于 MLsub 输出演进：

```diff
 struct struct_0 {
+  top:0 whole; /* storage path:  */
   struct_1 slot_0; /* storage path: slot:0 (slot 0) */
 };
```

这个 `top:0 whole` 字段来自 `TypeBuilder::convertStorageRecord` 的
`Node.Leaf != nullptr` 分支（`external/binarysub/src/TypeBuilder.cpp:997`），
是新增的"整块 storage 根也有叶子类型"表示，需要确认是有意行为还是误加，
再决定改代码还是更新 oracle。

样例 B（`01_evm_heap_i256`）：`main::arg0` 从 `[+]` 变 `[-]`，加一个尾部空行。

## 三、当前最关键的 bug：TypeBuilder 存储路径 abort

所有 `exit=-6` 的失败（pattern/source/type-recovery 三处）都是同一个崩溃：

```
notdec: /sn640/NotDec/external/binarysub/src/TypeBuilder.cpp:1135:
  notdec::ast::HType* notdec::mlsub::TypeBuilder::convert(binarysub::UTypePtr, bool):
  Assertion `false && "Unhandled UType variant"' failed.
```

栈（三处一致）：

```
ConstraintsGenerator::genTypes (MLsubGenerator.cpp:7076)
  -> TypeBuilder::convertStorageRecord (TypeBuilder.cpp:999)
     -> buildNode lambda (TypeBuilder.cpp:982)
        -> TypeBuilder::convert (TypeBuilder.cpp:1121)
           -> convertVisibleSetTerms (TypeBuilder.cpp:409) -> abort
```

观察与判断：

1. `TypeBuilder::convert` 的 variant 链已经覆盖 `UTop/UBot/UUnion/UInter/
   UFunctionType/UTypeVariable/UUnion/UInter/UPointerType/URecordType`，
   `URecursiveType` 单独在函数开头处理。也就是说 **10 个 variant 都覆盖了**，
   行号 1135 只是编译器把 `convertVariable`/'set 处理'内联后的落点，
   实际对不上"未处理 variant"的字面意思。
2. 栈顶真正的 abort 点在 `convertVisibleSetTerms`（TypeBuilder.cpp:409）。
   这个函数只有在跳过 zero-sized marker 之后 `Result == nullptr` 且
   `Terms` 为空时才会走到 `assert(!Terms.empty())`；另一种可能是
   内联符号错位、实际 assert 是 `convert()` 自己那条。
3. 无论哪种，触发路径都是 **storage 字段的 UType 转换**，属于 6 月 EVM
   storage 推理（`2026061x-evm-storage-*.md`）引入的新路径，从未在 EVM
   链路上回归过。
4. Microsub2 侧的同类问题 09-15 已经打过一次补丁
   （binarysub `7993188`：`convertStruct()` 里丢弃 `Start + Size` 溢出的字段），
   但那个补丁只覆盖 `convertStruct` 的重叠分析，**没有覆盖
   `convertStorageRecord` 这条 EVM storage 路径**。

复现（最小）：

```bash
/sn640/NotDec/build-notdec-nothreads2/bin/notdec \
  test/type-recovery/evm/cases/10_evm_storage_bytes_long.ll \
  -o /tmp/x.ll --tr-level=2 --frozen-tr-input-ir --dump-htypes /tmp/x.htypes
# exit=-6，输出到 TypeBuilder.cpp:1135 abort
```

apehex 侧同样复现：`0358_19494430_b7d763ebaf_31239f3e312c.ll`（6 月 4 日批次里
是 ok 的）现在也 abort 在同一处。

### 建议的定位方式（下一步第一件事）

在 `TypeBuilder::convert` 的 else 分支和 `convertVisibleSetTerms` 的
`assert(!Terms.empty())` 前加一次性诊断输出：

```cpp
std::cerr << "Unhandled UType variant: index=" << Ty->v.index()
          << " type=" << binarysub::printType(Ty) << "\n";
```

重建 `binarysub_lib` + `notdec`，跑 `10_evm_storage_bytes_long` 和 `0358`
两个输入，拿到确切的 variant/类型值后按"拦掉非法 storage 字段类型"或
"补该 variant 的 HType 转换"来修。**不要先改 oracle 掩盖崩溃。**

### 2026-09-16 追加：已定位到具体机制

本节的推测已由实测取代，结论见
`logs/20260916-02-p0-typebuilder-storage-crash-findings.md`：

- 崩溃点是 **storage 记录里某个字段的 `UType*` 指针失效**；
- `StorageTreeNode` 的根叶节点 `LeafPath` 为空串，说明这个失效字段的
  `StorageFields` key 是空串（或等价地，`splitPath` 只产出空段）；
- 失效指针的内存已经被别的东西复用（内容是 ASCII 串 + 一个 vector 头），
  不是 `UType`；
- 现有构建目录 `build-notdec-nothreads2` 的 `bin/notdec` 比它的静态库旧，
  它带的 `binarysub` 也不是当前源码，因此"旧二进制崩 / 新构建不崩"的差异
  **不能**当作修复证据。

## 四、evm2llvm 本体的两个真实 bug（2026-09-16 重新构建后仍复现）

用当前源码重建（`ninja -C external/NotDec-evm2llvm/build`，LLVM 22.1.0）
后，对 6 月 4 日批次里的失败样本重跑：

```bash
evm2llvm --facts <gigahorse out>/ --output /tmp/retry.ll
```

1. `22492_19734645_9489004623_d90f6b6ef58d`
   - 输出：`missing SSA value for PHI incoming variable 0x4135_0x0`
   - 源码位置：`lib/LlvmLowerer.cpp:283`（`makeError`）
   - 退出码 1，不产出 .ll；同一 run 还伴随 4 条
     `evm2llvm emits poison seed for multi-value RETURNPRIVATE` 警告。
   - 归类：TAC/SSA 事实缺失时，PHI incoming 变量找不到定义，应该给出更有
     指向性的诊断，或对可安全默认的形态做保守 fallback（当前行为是整份 IR
     产出失败）。

2. `26708_19785111_ed5443326c_5864b22a5ad0`
   - 输出：`PHIIncoming missing predecessor for PHI 0x4e6_0x4 from 0x4e0`
   - 源码位置：`lib/SsaFactValidator.cpp:150`
   - 退出码 1；Gigahorse 侧 `errors: 0`，说明是 facts 导出/前端校验策略问题，
     不是 Gigahorse 失败。

这两个是**纯 evm2llvm 侧**、可独立于主项目类型恢复修复的问题，适合作为
"修完崩溃之后"的第二优先级。

## 五、建议的修复顺序

1. **P0：修 `TypeBuilder::convert`/`convertStorageRecord` 崩溃**
   （先加诊断取到真实 variant；再决定补转换还是丢弃非法 storage 字段）。
   收益最大：4 个 suite 里绝大多数 `exit=-6` 都因此恢复，
   `0358` 这类 6 月原本 ok 的 apehex 样本也会恢复。
2. **P1：evm2llvm 两个 PHI/SSA 失败**
   - `LlvmLowerer.cpp:283`：改进诊断并评估 fallback；
   - `SsaFactValidator.cpp:150`：明确 predecessor 缺失时是拒绝还是补边。
   用 `test/fixtures` 新增回归（可以先用 `22492/26708` 的 facts 裁剪）。
3. **P2：确认 `top:0 whole` 是否有意**
   - 若有意：更新 EVM oracle（`01/05/09/11~16` 等）；
   - 若无意：修 `convertStorageRecord` 的 Leaf 分支。
   这一项必须在 P0 之后做，否则无法区分"漂移"和"崩溃"。
4. **P3：回到 6 月 3 日定的下一步**
   - native load/store 接 MLsub 的常量 offset 小闭环（`04_evm_memory_helpers.ll`
     已就位并能通过，可以以此为锚点扩展 `calloc/ptrtoint/inttoptr` 的
     allocation base 关系）；
   - 动态 offset 仍然保守（记数组/unknown，不猜 fixed field）。
5. **P4：重跑 selected-apehex-80/50 批次**，把 6 月 5 日没结论的 timeout 样本
   （`7435`、`18404`）重新计时，确认是否已被上游 perf 改动顺带修掉。

## 六、日志约定

- 从本文件开始，evm2llvm 相关日志写到
  `external/NotDec-evm2llvm/logs/`，命名沿用
  `YYYYMMDD-NN-<topic>.md`。
- `logs/` 里的日志**随子模块一起提交**（`.gitignore` 不忽略它，目录里带
  `.gitkeep`）。
- 主项目 `/sn640/NotDec/logs/` 里已有的 EVM 历史日志保持原样，不搬动。
