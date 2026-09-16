# 残余崩溃根因：StorageTreeNode::Leaf 未初始化（2026-09-16 续）

## 结论

`solidity_patterns` 并行加压下的野指针崩溃不是 arena 生命周期竞态，而是
`TypeBuilder::convertStorageRecord()` 里一个未初始化裸指针：

    struct StorageTreeNode {
      std::map<std::string, StorageTreeNode> Children;
      UTypePtr Leaf;          // <- 未初始化
      std::string LeafPath;
    };
    ...
    StorageTreeNode Root;     // 默认初始化，Leaf 是栈上残留值

`StorageFields` 只有在包含空路径 key（`""`）时才会给 `Root.Leaf` 赋值；
storage 访问路径实际都形如 `slot:0...`，因此 `Root.Leaf` 保持未初始化。
`buildNode()` 随后执行 `if (Node.Leaf != nullptr) convert(Node.Leaf)`，
把栈上残留值当成 UType 解引用。TBB 并发 + 16 线程会改变栈复用内容，
所以只有并行加压时稳定复现。

## core 证据（build-notdec-nothreads2，24763，16 线程）

core 中崩溃点 `convertVisibleSetTerms()` 的 `Ty` 寄存器回填为：

    Ty = 0x5652d556aa10
    Ty->v.index() 所在字节 = 2（恰好伪装成 UUnion）
    ((std::vector<UType*>*)Ty)->_M_start = 0xfffffffffffff000（-4096）

而 `0x5652d556aa10` 的分配头部是 `size=0x3af` 的普通 `operator new`
块，不是 `utype_pool()` 的任何一个 deque block；该块开头是 LLVM
`DenseMap` 的 empty-key sentinel `-4096`。也就是说，被当成 UType 的
指针来自栈残留，而不是悬空的池节点。

回溯到 `convertStorageRecord()` frame：

    #5 buildNode lambda, TypeBuilder.cpp:998
       Node.Leaf = 0x5652d556aa10
       Node.Children.size() = 4, Node.LeafPath = ""（空）
    #7 convertStorageRecord, Ty = <valid storage-root URecordType>

同一个 storage root 的实际 `fields` 只有 `slot:0.map.key`、
`slot:0.map.value` 等非空路径，没有空路径 key，所以 `Root.Leaf` 不
应该被使用。

## 为什么 oracle 里会出现 storage 根 `whole` 字段

旧构建单线程跑 `09_evm_storage_bytes_short` 也会输出：

    struct struct_0 {
      top:0 whole; /* storage path:  */
      ...
    };

跟踪 `NOTDEC_BINARYSUB_TRACE=1` 显示该用例 `StorageFields` 实际只有
`slot:0.bytes.length`、`slot:0.bytes.short_data`，没有 `""` key。
`top:0` 是旧栈残留恰好指向某个 top UType 的结果；`12/13` 用例里
同一位置甚至变成 `top:6` / `top:64`。这是未定义行为，不是有意义的
storage root whole 字段，因此：

- `test/type-recovery/evm/expected/tr-level-2/09,10,11,12,13,14,15,16` 
  删除 `top:* whole; /* storage path:  */` 行；
- `test/evm/solidity-source/expected/apehex_gasleft_return_01.sol`
  删除由同一 bug 产生的 `uint256 public whole;`。

## 修复

external/binarysub:

    UTypePtr Leaf = nullptr;

commit: `TypeBuilder: initialize StorageTreeNode::Leaf`

该初始化同时覆盖 map 内部节点；Root 及所有 `Children[Part]` 都不会再
出现未初始化 Leaf。

## 验证

- 普通 RelWithDebInfo（`/tmp/build-evm-p0`）重建；
- `24763`：4 worker × 100 次 × 16 线程 = 400 次，0 崩溃；
- `0032`：4 worker × 100 次 × 16 线程 = 400 次，0 崩溃；
- `notdec.type_recovery.evm.tr_level_2`：oracle 更新后通过；
- `notdec.evm.solidity_rewrite`：通过；
- `notdec.evm.solidity_source`：oracle 更新后通过；
- `notdec.evm.solidity_patterns`：不再出现 `24763/0032` 的崩溃；
  多轮运行可过，但仍有一个**独立的**并行非确定性（见下）。

## 仍未解决：cross-root shared coalesce memo 引发的 ABI return 非确定性

`24259_19755445_aefeec2314_4f43187f4106` 的 `abi_return` 计数在
多线程下随机为 8 或 10（manifest 期望 8），差异集中在
`public__0x4a00cc48_0x74f` 是否识别到 ABI return。统计：

- 默认并行：30 次中 25 次 = 8，5 次 = 10；
- `NOTDEC_BINARYSUB_THREADS=1`：30/30 = 8；
- `NOTDEC_DISABLE_SHARED_COALESCE_MEMO=1`：20/20 = 8；
- `NOTDEC_SHARE_MU=0`：20 次中仍有 1 次 = 10；
- `NOTDEC_MU_DETERMINISTIC=0`：20 次中 4 次 = 10；
- `NOTDEC_DISABLE_ANALYZE_CACHE=1`：20 次中 3 次 = 10。

即：这是 cross-root shared coalesce memo（`lookupSharedMemo` /
`storeSharedMemo`）在并行插入/命中下的确定性缺陷，而不是之前的野指针。
`NOTDEC_DISABLE_SHARED_COALESCE_MEMO=1` 连续 3 轮 `solidity_patterns`
全绿，可作为临时兜底；默认仍开启。

下一步应对比 shared memo 打开/关闭时 `public__0x4a00cc48_0x74f`
的 `HighTypes`/return-buffer UType，确认共享条目的第一个插入者在
不同 group / `newRecVars` 上下文下返回了结构等价但语义不同的展开。
优先审查 shared key 是否遗漏了 `newRecVars` / origin 上下文。

## 附录：node-source 方案的适用性

原计划的 creator 标记实验没有再做，因为这个 core 已经证明“UType
指针本身不在池内”，问题在转换输入而不是池内节点复用。TSan 报的
arena clear 竞态仍可能是真实问题，但不再是本崩溃的触发源。
