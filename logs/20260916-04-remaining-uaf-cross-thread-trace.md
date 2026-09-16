# 剩余 3 个 UAF 的追查（2026-09-16 续）

承接 `logs/20260916-03-asan-findings-and-fixes.md` 的"发现三"。本轮把剩下的 heap-use-after-free 从"现象"推进到"跨线程 + 变量集工厂回收"这一层，并给出可验证的下一步。

## 复现方式

这 3 个用例（`0032`、`0334`、`25928`）是**非确定性**的：单跑同一用例常常 0 报告，跑整套 suite 或重复多轮才会命中。本轮已找到稳定复现路径：

    # ASan 构建（见 03 号日志）
    for round in 1 2 3; do
      ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/build-evm-asan \
        -R "notdec.evm.solidity_patterns" --output-on-failure
    done

本轮 round1 即在 `0334_19494307_668d201319_1354ce2e324d` 命中，完整报告已另存 `/tmp/uaf-report.log`。

注意：给 MLsubGenerator 加任何 `std::cerr` 诊断打印都会改变堆布局、把该 bug 掩盖掉（加了打印的 ASan 构建连跑整套 0 报告）。所以定位这类问题不能用打印，要用 ASan 报告本身 + addr2line/gdb。

## 关键证据（ASan 报告）

READ 侧与之前一致：主线程 T0 在 `TypeBuilder::convert()` 入口读 `Ty->v`，来自 `convertStorageRecord`（`genTypes` 的 storage 转换）。

freed 侧是新的、决定性的：

    freed by thread T27 here:
    #7 PersistentSet<...>::Factory::recycleNodeLocked(Node*)            PersistentSet.h:342
    #8 PersistentSet<...>::Factory::recoverCreatedNodesLocked()        PersistentSet.h:346
    #9 PersistentSet<...>::Factory::add(...)                           PersistentSet.h:209
    #10 CompactVarSet::cloneInto(ArenaState&)                          binarysub.h:614
    #11 CompactTypeArena::cloneVarSet(...)                             binarysub.h:854
    #12 copyCompactTypeInto(...) lambda                                binarysub.cpp:5001
    #22 copyCompactTypeInto(...)                                       binarysub.cpp:5008
    #23 coalesceCompactType storeSharedMemo lambda                     binarysub.cpp:5431
    #28 TypeSimplifier::coalesceCompactType(...)                       binarysub.cpp:5994

被释放的区块是 `std::vector<PersistentSet<TypeRef<TypeNode>,...>::Node*>` 的缓冲区（8 字节区块，即一个 `Node*` 槽），由**求解 worker 线程 T27** 释放；主线程随后仍通过同一个地址读 `UType`。

## 判断

1. 坏指针不是 `utype_pool()` 的节点（该池进程级 append-only、节点不释放），而是指向了 **CompactVarSet 的持久化集合工厂（`PersistentSet::Factory`）回收/重建时释放的 node 槽存储**。
2. 释放发生在 `cloneInto` → `factory.add()` → `recoverCreatedNodesLocked()` 的回收路径上，也就是**求解器跨 root 共享 memo（`CoalesceMemo`）把变量集克隆进 memo arena** 的路径。
3. 这是跨线程生命周期问题：`CoalesceMemo`（含 `Arena`、`VarMemo`、`TreeMemo`）是跨 worker 共享的；`CompactVarSet::ArenaState` 用 `shared_ptr` 保命的设计本意是防止这种悬空，但实际仍有引用在工厂回收后继续使用。
4. 同一个地址在两次运行里分别是 40 字节（`_Rb_tree_node<UType*>`，即 03 号日志的发现一）和 8 字节（node 槽），说明这个 UType 指针所在的内存被两类不同的分配器复用——现象上就是之前看到的"变体索引是垃圾值 / 随机 SIGSEGV"。

## 建议的下一步（按性价比排序）

1. **先验证是否就是跨 root 共享 memo**：加一个环境变量开关（例如 `NOTDEC_DISABLE_SHARED_COALESCE_MEMO=1`），让 `lookupSharedMemo`/`storeSharedMemo` 的 `Shared` 传 `nullptr`，然后在 ASan 下重复跑 solidity-patterns 多轮。若 ASan 报告归零，范围就锁定在这条共享路径；同时也能作为线上兜底开关。
2. **核查 `PersistentSet::Factory` 的生命周期与回收语义**（`external/binarysub/include/binarysub/detail/PersistentSet.h`）：
   - `recycleNodeLocked()`（:313）与 `recoverCreatedNodesLocked()`（:344）是否可能在仍有外部 `CompactVarSet`/`CompactType` 引用时回收节点；
   - `Factory::add()`（:196）里"先给结果 root 外部引用、再回收本次构造的不可达节点"的顺序在并发 reader 下是否成立；
   - `CompactTypeArena::clear()`（binarysub.cpp:2161）把 `varState` 换新的同时旧 `shared_ptr` 置空，memo 里是否还有 `CompactVarSet` 持有旧 state。
3. **兜底防线**（独立于根因，建议早点加）：在 `TypeBuilder::convert()` 入口用 `Ty->v.index() >= 10` 判定非法指针并跳过/降级为 `top`，把"悬空指针 → SIGSEGV/断言 abort"降级为一条告警 + 一个字段丢失，至少不再中断整份反编译。

## 本轮未做

- 没有改 binarysub 的求解/回收逻辑（跨线程正确性改动风险高，需要先锁定范围再动）。
- 没有提交任何诊断代码：MLsubGenerator 里的临时打印已全部删除，工作树恢复干净。
