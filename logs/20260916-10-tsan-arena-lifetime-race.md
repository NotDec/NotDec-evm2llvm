# TSan 结果：arena 生命周期竞态（2026-09-16）

## 构建与运行

    cmake -S /sn640/NotDec -B /tmp/build-evm-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS=-fsanitize=thread ...
    TSAN_OPTIONS=halt_on_error=0:history_size=7 NOTDEC_BINARYSUB_THREADS=16 /tmp/build-evm-tsan/bin/notdec <24763 case> -o /tmp/out.ll --tr-level=2

结果：402 条报告（274 条 SIGSEGV handler 噪声、128 条 data race）。
race 里大部分是 oneTBB task-stealing 的已知误报，binarysub 侧有价值的有 21 类。

## 关键聚类

| 条数 | 位置 |
| --- | --- |
| 17 | CompactVarSet::ArenaState::~ArenaState() binarysub.h:511 |
| 12 | CompactType::~CompactType() binarysub.h:654 |
| 8 | TypedBumpArena<CompactType>::clear() BumpArena.h:53 |
| 5 | TypedBumpArena<PersistentSet Node>::clear() BumpArena.h:53 |
| 12 | bulkSimplifyDetailed 内 binarysub.cpp:6472/6476/6555-6560 |
| 9 | make_utype_node binarysub.h:295 |
| 2 | TypeSimplifier::clear() binarysub.h:1472 |
| 1 | TypeBuilder::convert TypeBuilder.cpp:1085 |

## 最清楚的一条：clear() 释放 arena，worker 还在读 CompactType

写（主线程）：
    TypedBumpArena<CompactType>::clear()   BumpArena.h:53 (operator delete)
    CompactTypeArena::clear()              binarysub.cpp:2199/2200
    TypeSimplifier::clear()                binarysub.h:1499
    TypeSimplifier::bulkSimplifyDetailed() binarysub.cpp:6572 (函数末尾的 clear)

读（TBB worker T28）：
    TypeRef<CompactType>::operator->()     binarysub-utils.h:142
    buildLocalVariableOrigins::collect     binarysub.cpp:6323 (Ty->vars)
    ... collect(Root)                      binarysub.cpp:6345
    processGroup                           binarysub.cpp:6462
    tbb::parallel_for                      parallel_for.h:208

即：bulkSimplifyDetailed 末尾的 clear() 释放 CompactType arena 时，仍有 worker 在通过 CompactTypePtr 读 CompactType 节点。这解释了 core dump 里 union term 向量出现野指针 0xfffffffffffff000：CompactType 内存被释放后复用/覆盖，转换出的 UType 带着垃圾。

17 条 ~ArenaState() 的写栈同样是 TypeSimplifier::clear()：clear() 同时释放 typeStorage（CompactType 本体）和 varState（变量集 Storage）。上一轮 CompactVarSet 拥有化只保护了后者，前者仍是裸指针。

## 为什么 parallel_for 返回后还有 worker 在读

parallel_for(0, GroupsToProcess, processGroup) 是阻塞的，正常返回后 worker 应已结束。两种可能：

1. 同一个 TypeSimplifier 被串行复用（多个 SCC 依次调用 bulkSimplifyDetailed），clear() 释放的是上一次调用的 arena；若上一次的 CompactTypePtr 仍被缓存或结果引用（go0Cache/go1Cache/go1FullCache/canonicalizeProgressStates/RootGroups/LocalRoots），就会读到已释放内存。TypeSimplifier::clear() 自己清这些缓存（binarysub.h:1472 也有 race），说明清理与读之间存在窗口。
2. processGroup 内部还有未 join 的并行工作，或 TBB global_control 让完成语义不直观。

区分办法：给 arena 加自增 id + owner thread id，在 clear() 和 CompactTypePtr 解引用处打印；或在 TSan 下把 NOTDEC_BINARYSUB_THREADS 设为 2 复跑。

## 修复方向

- 方案 A（推荐）：CompactTypeArena::typeStorage/consIndex 也改成被句柄共享拥有（CompactTypePtr 的持有者持 shared_ptr 到 arena 存储），clear() 只换新存储，旧存储由最后一个句柄释放。跨调用/跨线程的 CompactTypePtr 就不会悬空。
- 方案 B（保守兜底）：bulkSimplifyDetailed 末尾不释放，clear() 只留到析构；内存换正确性，可立即消除崩溃。
- 方案 C（审计）：确认所有跨调用保留的 CompactTypePtr 都在 clear() 前清空或复制（缓存/RootGroups/成员变量）。

## 现状

- 这就是 solidity_patterns 每轮 1~2 个用例崩溃（24763、0032）的来源：只在多线程 TBB 路径触发，单线程难复现，ASan 看不到（arena 是正常释放，不是 malloc/free UAF），只有 TSan 能看到释放-读竞态。
- TSan 构建在 /tmp/build-evm-tsan。临时诊断代码已回退，external/binarysub 干净停在 5fd11b9。
