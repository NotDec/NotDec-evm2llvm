# 剩余 UAF：范围验证 + 生命周期盘点 + 根除方案（2026-09-16）

承接 `logs/20260916-04-remaining-uaf-cross-thread-trace.md`。本轮做了三个范围实验，并把 `CompactVarSet` / `PersistentSet` / `CoalesceMemo` 的生命周期完整盘了一遍。

## 一、范围实验（ASan 构建，每次跑完整 solidity-patterns suite 计一轮）

| 实验 | 结果 |
| --- | --- |
| `NOTDEC_DISABLE_SHARED_COALESCE_MEMO=1`（关闭跨 root 共享 memo） | **6/6 轮 0 报告** |
| 默认（共享 memo 开启，对照） | 6 轮里 1 轮命中 |
| 关闭变量集工厂的 slot 回收（临时探针，已回退） | 3 轮里 2 轮仍命中 |

结论：

1. 问题**锁定在跨 root 共享 coalesce memo 这条路径**：关掉它，6 轮全部干净。
2. **不是**变量集工厂 slot 回收本身的问题：关掉回收后仍然命中，说明之前 ASan 报告里 `recycleNodeLocked` 只是恰好复用了那块被释放内存的分配点，不是根因。
3. 共享 memo 路径的特征：`CoalesceMemo::Arena` 是**跨 worker 线程共享**的 arena，每个 group 的临时 `TypeSimplifier`/compact arena 会销毁，而 memo 里的 `CompactTypePtr`/`CompactVarSet` 跨 arena 存活。

## 二、生命周期盘点

### 2.1 对象与持有关系

    CoalesceMemo (bulkSimplifyDetailed 栈上局部, binarysub.cpp:6356)
      mutex
      Arena: CompactTypeArena           <- memo 的共享 arena
        varState: shared_ptr<CompactVarSet::ArenaState>
          factory: PersistentSet::Factory
            nodes: TypedBumpArena<Node>   (节点本体, 不释放)
            createdNodes / freeNodes: vector<Node*>
          hashConsIndex: unordered_map<hash, vector<unique_ptr<Storage>>>
        typeStorage: unique_ptr<TypedBumpArena<CompactType>>
        consIndex: unordered_map<hash, vector<CompactTypePtr>>
      VarMemo/TreeMemo: map<hash, vector<CoalesceMemoEntry>>
        CoalesceMemoEntry{ Pol, CompactTypePtr Ty /*memo arena*/, UTypePtr Type /*全局池*/ }

    CompactType (memo arena 或 per-group 临时 arena)
      vars: CompactVarSet
        storage: const Storage*        <- 非拥有裸指针!
      record/function/ptrLoad/ptrStore: CompactTypePtr

    CompactVarSet::Storage { owner: ArenaState*, set: PersistentSet, size }
      ^ 由 ArenaState::hashConsIndex 以 unique_ptr 拥有

    PersistentSet { factory: Factory* (裸指针), root: Node* }
      Node { left, right, value: SimpleType, height, hash, atomic refCount }
      refCount==0 且被 release/recover 时进入 Factory::freeNodes，slot 可被 makeNode 原地复用

### 2.2 已确认的安全设计

- `utype_pool()`：进程级 append-only deque，`UType` 节点地址终身稳定（`binarysub.h:120-129`）。
- `CoalesceMemo` 是 `bulkSimplifyDetailed` 的栈局部变量；worker（TBB）在该函数返回前 join，memo 比 worker 活得久，所以 memo arena 本身不会在求解期间析构。
- `copyCompactTypeInto(memo->Arena, Ty)`（binarysub.cpp:5441）会把源 arena 的 `CompactType` 复制进 memo arena，并用 `cloneVarSet` 重建变量集（`binarysub.h:606-619`）——这正是为"per-group arena 先死、memo 后死"设计的。
- `CompactTypeArena::clear()`（binarysub.cpp:2161）先 `oldStorage.reset()`（销毁 `CompactType`，释放其 `PersistentSet` 句柄）再 `oldVarState.reset()`，顺序上避免了句柄比 arena 活得久。

### 2.3 危险点：`CompactVarSet` 是非拥有句柄

    class CompactVarSet {
      const Storage *storage = nullptr;   // 裸指针
      // 没有 shared_ptr<ArenaState>
    };

`storage` 指向 `ArenaState::hashConsIndex` 里的 `Storage`，而 `ArenaState` 的存活完全依赖 `CompactTypeArena::varState` 这个 `shared_ptr`。因此：

- 一个 `CompactVarSet` 句柄本身**不能保证**其 `storage` 在其生命周期内有效；
- 只要有任何一条路径产出了 `storage->owner != 当前 arena.varState` 的句柄，而源 arena（per-group 临时 arena）随后被销毁，这个句柄就成为悬空；
- 代码里已经针对这个风险加了两处 owner 检查：`canReuseVarSetRoot()`（binarysub.cpp:1822）和 `mergeVarSets()` 里对 `lhs` 的检查（:1874）；`makeFromCompactVars` 的注释（:1924-1927）也明说"把别的 arena 的 `CompactVarSet` 直接保留下来会让 `CompactType` 指向被清空后的源 ArenaState"。**这些检查覆盖的是"复用"；一旦有路径绕开（例如 `mergeVarSets` 只检查 `lhs` 不检查 `rhs`、或调用方直接持有跨 arena 句柄），就出问题。**

### 2.4 与 ASan 证据的对应

之前报告的"坏指针指向已释放的 8 字节 vector 缓冲区"正是这类悬空的典型表现：句柄引用链上的某个指针在源 arena 销毁后仍被读取，读到的内容已被分配器复用成别的东西（`recycleNodeLocked` 的 work vector 只是恰好趴在那个地址上）。这也解释了为什么：

- 关掉共享 memo（不再跨 arena 复制/共享）→ 不再悬空 → 0 报告；
- 关掉 slot 回收（只是不覆盖那块内存）→ 悬空读仍然发生 → 仍报 UAF。

## 三、根除方案（建议按顺序做）

### 方案 A（推荐，治本）：让 `CompactVarSet` 拥有自己的 arena

给 `CompactVarSet` 增加 `std::shared_ptr<ArenaState> state`（或把 `Storage::owner` 改成 `weak_ptr` + 句柄持 `shared_ptr`），使句柄在任何时候都能保证 `storage` 所依赖的 `ArenaState` 存活。具体：

1. `CompactVarSet` 成员加 `std::shared_ptr<ArenaState> state;`，所有构造/赋值路径同步维护；
2. `ArenaState::internStorage()` 返回时把 `state` 一并带上（或让 `CompactVarSet` 构造函数从 `Storage::owner` 反推，同时把 `owner` 改为 `shared_ptr`/`weak_ptr` 以免循环引用）；
3. 保留 `canReuseVarSetRoot`/owner 检查作为调试断言（`assert(storage->owner == target.varState.get())`），跨 arena 一律先 `cloneVarSet`；
4. 代价：每个 `CompactVarSet` 增加一个指针宽度；`cloneInto`/`internStorage` 语义不变。

好处：把"句柄比 arena 活得久"从"靠调用方自觉"变成类型系统级别的保证，之后再出现跨 arena 复制也不会 UAF。

### 方案 B（保守，过渡）：把所有跨 arena 入口显式 clone + 断言

- `CompactTypeArena::makeFromCompactVars()` 入口对 `vars` 做一次 `canReuseVarSetRoot()` 断言（debug 构建下 abort 并打印来源），不满足就 `cloneVarSet`；
- `mergeVarSets()` 对 `rhs` 也加同样的 owner 检查；
- `CompactTypeBuilder::rememberReusableVars()` 只接受本 arena 的句柄。

这个方案改动小、风险低，但防不住未来新增路径，适合作为 A 的过渡。

### 方案 C（兜底防线，独立于 A/B）

在 `TypeBuilder::convert()` 入口检查 `Ty->v.index() >= 10`，把非法指针降级为告警 + `top`，让悬空指针不再以 SIGSEGV/断言 abort 的形式炸掉整份反编译。这条即使根因修掉也建议保留（外部输入不可控）。

### 验证方式（回归门槛）

    # 1) ASan 构建，重复跑 EVM pattern suite 多轮（至少 6 轮）
    for i in 1 2 3 4 5 6; do
      ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/build-evm-asan -R "notdec.evm.solidity_patterns" > /dev/null;
      grep -rl "ERROR: AddressSanitizer" /tmp/build-evm-asan/test/artifacts/notdec.evm.solidity_patterns/*/run.log | wc -l;
    done
    # 2) 普通构建：EVM suite + llvm_ir/sysy 回归对比（只看是否有新增失败）
    # 3) 性能：确认 A 方案没有把单轮时间显著拉长（共享 memo 命中率不变）

## 四、本轮改动与状态

- `src/binarysub.cpp` 增加 `NOTDEC_DISABLE_SHARED_COALESCE_MEMO` 环境开关（默认共享，置 1 关闭），作为范围验证/线上兜底；这是本轮唯一代码改动。
- 临时探针 `NOTDEC_NO_RECYCLE_VARSET_NODES` 已回退，`PersistentSet.h` 恢复原状。
- 未实施 A/B/C 任一修复：A 会动 `CompactVarSet` 的公开类型语义，建议单独一轮实现 + 全量回归。
