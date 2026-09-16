# 方案 A 实施记录：CompactVarSet 改为拥有 ArenaState（2026-09-16）

承接 `logs/20260916-05-var-set-lifetime-audit-and-fix-plan.md`。本轮按方案 A 实施并验证通过。

## 改了什么

核心：`CompactVarSet` 从"非拥有句柄"改为"拥有句柄"。

1. `CompactVarSet::ArenaState` 继承 `std::enable_shared_from_this<ArenaState>`；
2. `CompactVarSet` 新增成员 `std::shared_ptr<ArenaState> state;`——句柄活着，ArenaState（含 `PersistentSet::Factory` 和 `hashConsIndex`）就不会死；
3. `Storage::owner` 由裸指针 `ArenaState *` 改为 `std::weak_ptr<ArenaState>`（ArenaState 拥有 Storage，强引用会成环泄漏）；
4. 新增 `belongsTo(ArenaState*)`，`canReuseVarSetRoot()`、`mergeVarSets()`、`makeFromCompactVars()` 的 owner 检查改用它；
5. 所有 `internStorage()` 的调用点（`makeVarSet`、`makeVarSetWithBase`、`mergeVarSets`、`cloneInto`）改为同时传 `state`，`cloneInto` 绑定目标 arena 的 state；
6. 进程级 fallback arena 改为 `sharedFromThis()` + no-op deleter 的共享句柄，避免全局静态对象析构顺序问题；
7. 另外在 `TypeBuilder::convert()` 入口加了最后防线：`Ty->v.index() >= 10`（`UType` 的 variant 只有 10 个分支）时判为非法指针，打印 root/path 后降级为 `top`，不再让悬空指针演变成 variant 读或 SIGSEGV。

## 中途的一次反复（记录一下，避免后面误判）

第一次只改完了类定义和 `cloneInto`，漏了 `makeVarSetWithBase` / `mergeVarSets` 里几处仍传裸 `varState` 的 `internStorage` 构造点（编译器不会报错，因为旧构造函数还在）。那一版 ASan 仍然 3/6 轮命中，差点误判方案 A 无效。把全部构造点补齐后：**6/6 轮 0 报告**。

## 验证结果

### ASan 构建（`/tmp/build-evm-asan`），EVM pattern suite 重复 6 轮

| 轮次 | ASan 报告 | guard 命中 |
| --- | --- | --- |
| 1..6 | **全部 0** | 0 |

（对照：修复前默认配置约 1/6～2/3 轮命中；只关共享 memo 是 6/6 干净；只关 slot 回收仍会命中。）

### 普通构建 EVM suite

| suite | 修复前 | 本轮修复后 |
| --- | --- | --- |
| solidity_patterns | 53 pass / 50 fail | **65 pass / 38 fail** |
| solidity_rewrite | 74 / 1 | **77 / 1** |
| solidity_source | 77 / 1 | 77 / 1 |
| type_recovery.evm | 5 / 11 | 5 / 11 |

patterns 通过数 +12、rewrite +3，说明之前那批失败里有相当一部分就是被这个悬空句柄拖垮的（类型转换中途读到垃圾指针导致后续全部走偏/中断）；剩下的是 oracle 漂移，见 03 号日志。

## 为什么这样能根除

- 旧设计里 `CompactVarSet::storage` 指向 `ArenaState::hashConsIndex` 中的 `Storage`，而句柄本身不持有 `ArenaState`；跨 root 共享 coalesce memo 会把 per-group 临时 arena 的句柄带进 memo（`copyCompactTypeInto`/`cloneVarSet` 只是"尽量"重建，一旦有路径漏掉，源 arena 析构后句柄即悬空）。
- 现在句柄自带 `shared_ptr<ArenaState>`：无论谁把句柄带出源 arena，`Storage` 与 `PersistentSet` 节点都不会被提前释放。跨 arena 语义上的隔离仍由 `makeFromCompactVars` 里的 `belongsTo` 检查 + `cloneVarSet` 保证（避免无限 pin 住源 arena 并让内存无谓增长）。
- `TypeBuilder::convert()` 的 index 防线是独立兜底：即使将来又出现别的悬空来源，也只丢一个字段并告警，不会整份反编译崩溃。

## 后续建议

1. 把 `NOTDEC_DISABLE_SHARED_COALESCE_MEMO` 开关保留一段时间（已提交），便于线上/回归快速定位是否又是共享路径；确认稳定后可移除。
2. 若后续做 `CompactVarSet` 的进一步清理，可考虑用 intrusive 计数替代 `shared_ptr`，省掉每个句柄的原子操作；当前先保证正确性。
3. `type_recovery.evm` 仍 5/11，属于 storage oracle 漂移（`top:0 whole` 等），应与崩溃修复分开更新 oracle。
