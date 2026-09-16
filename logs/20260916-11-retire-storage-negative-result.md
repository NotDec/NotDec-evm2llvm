# 归档存储（方案 A 简化版）验证失败 —— 尚未修好（2026-09-16）

## 做了什么

按 TSan 报告实现"clear() 不释放、改为归档"：

- CompactTypeArena::typeStorage 从 unique_ptr 改为 shared_ptr；
- 新增 retiredTypeStorage，clear() 把旧存储 move 进去而不是析构；
- 旧存储随 CompactTypeArena 生命周期释放。

## 结果：崩溃依旧

并行加压复现（4 并发 × 每进程 16 线程 × 40 次）：**160 次里 7 次崩溃**，栈完全相同：

    convertVisibleSetTerms  TypeBuilder.cpp:412
    TypeBuilder::convert    TypeBuilder.cpp:1176 (UUnion)
    convertStorageRecord    TypeBuilder.cpp:998/1015

说明：TSan 报的 clear() 竞态是真的，但**它不是这次野指针的唯一来源**；只保护 typeStorage 不足以修掉。

## 该改动已回退

未验证有效的改动不留：归档存储只增加内存驻留，且没有消除崩溃。external/binarysub 已恢复到 5fd11b9 干净状态。

## 下一步该看哪里

TSan 报告里还有两组没处理的线索，优先级更高：

1. **9 条 make_utype_node 竞态（binarysub.h:295）**：全局 utype_pool 的 hash-cons 插入路径。可疑点：
   - UType::hashCache 是 mutable 非原子写（utypeNodeHash 会写它）；
   - utype_pool() 是 std::deque，插入时 map 可能重分配；若另有代码在无锁情况下遍历/持有 deque 迭代器（TSan 里 5 条 _Deque_iterator::_M_set_node + 2 条 deque::emplace_back 正好对应），就会读到撕裂的 map；
   - 这两者都可能让"已发布节点"的地址/内容被破坏。
2. **TypeBuilder::convert 1085 的 1 条竞态**：说明 HType 侧也有共享状态被并发访问。

建议的定位手段：

- 用 TSan suppression 过滤掉 TBB 噪声，集中看 binarysub 报告；
- 给每个 UType 节点加"创建线程 id + 序号"字段（临时），在崩溃点打印该节点的创建信息，判断它是不是被并发改写/复用的节点；
- 把 utype_pool 从 std::deque 换成"分块 + 稳定地址"的自有结构（chunk 指针数组本身不变），彻底消除插入与遍历的相互影响；同时把 hashCache 改为 std::atomic。

## 现状

- 崩溃仍在：solidity_patterns 每轮 1~2 个用例（24763 / 0032），只在 TBB 多线程路径触发。
- oracle 对齐的 3 个 commit 与 PHI 修复不受影响；TSan 构建在 /tmp/build-evm-tsan。

## 追加排查（同轮）：UType 池的无锁读者

grep 了整个 binarysub：没有 for-each 式的无锁遍历 utype_pool()。唯一在锁外访问池内部状态的是 binarysub.cpp:6451 的 utype_pool().size()（在 processGroup 内、worker 线程执行），以及若干 SimplifyDiag 诊断里的 utype_pool().size()。

utype_pool().size() 读 deque 的 _M_finish，与其它线程的 emplace_back（写 _M_finish/map）构成真实 data race，但只影响读到的计数，不足以解释 union term 向量里的野指针。因此 UType 池这条线索优先级下调。

下一步应转向：
1. TypeBuilder::convert:1085 的那条竞态（HType 侧共享状态）；
2. 给 UType/CompactType/CompactVarSet 节点临时加 (creator_thread, seq) 标记，在崩溃点打印来源，确认是节点被复用改写还是指针被算错。
