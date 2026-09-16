# 残余崩溃定位：union set 节点里的野指针 + TSan 计划（2026-09-16）

## 一、稳定复现办法（关键进展）

这个崩溃单独跑不复现，但**并行加压后稳定复现**：

    cd /sn640/NotDec
    worker() { for i in $(seq 1 40); do
      NOTDEC_BINARYSUB_THREADS=16 build-notdec-nothreads2/bin/notdec \
        test/evm/solidity-patterns/cases/24763_19761741_c9ddde2099_02b0670bed95.ll \
        -o /tmp/out.ll --tr-level=2 || echo CRASH;
    done; }
    for w in 1 2 3 4; do worker & done; wait

4 个并发进程 × 每进程 16 线程时，几十次内必崩；单进程串行 40 次 0 崩溃。ASan 构建连续 8 轮整套 suite **0 报告** —— 说明这不是 use-after-free（内存没被释放），而是**数据竞争造成的值损坏**。

## 二、core dump 里看到的损坏形态

启用 core dump（`ulimit -c unlimited`，apport 会写到 `/var/lib/apport/coredump/`）后拿到了崩溃现场：

    #3 convertVisibleSetTerms  TypeBuilder.cpp:412  (for (const auto &Term : Terms))
    #4 TypeBuilder::convert    TypeBuilder.cpp:1176 (UUnion 分支)
    #5 convertStorageRecord    TypeBuilder.cpp:998/1015

gdb 打印循环变量：

    Term = <error reading variable: Cannot access memory at address 0xfffffffffffff000>

也就是说：**union（UUnion）节点的 `types` 向量里有一个元素是野指针 `0xfffffffffffff000`（即 -4096）**。崩溃发生在遍历该向量时对元素的第一次解引用。

补充诊断尝试（读 variant 判别字）时，崩溃点前移到 `Term->v.index()`——再次确认这个元素本身就是坏地址，而不是"元素合法但内容损坏"。

注意 `-4096 = 0xfffffffffffff000` 是"页对齐的负数"，不像随机内存垃圾，更像**某个 `int64` 偏移被当成指针**、或指针被按页做减法/掩码的残留。

## 三、已排除

- **不是 use-after-free**：ASan 8 轮干净；core 里的地址也没落在已释放区域的特征上。
- **不是上一轮修掉的 `CompactVarSet` 悬空**：那个已由 `5fd11b9` 修掉（句柄拥有 ArenaState）；本次坏值出现在 union 的 term 向量里，且内存是活的。
- **不是单线程数据相关**：同一输入单独跑 40 次全过，只有并行加压才崩。

## 四、下一步：ThreadSanitizer

ASan 对这个 bug 无效（不是内存生命周期问题），应该用 **TSan**：

    cmake -S /sn640/NotDec -B /tmp/build-evm-tsan -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
      -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
      -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
    cmake --build /tmp/build-evm-tsan --target notdec -j8

（LLVM 共享库未插桩，但 binarysub/类型恢复都在进程内且会插桩；TSan 能报出 racing 的两条栈。）

重点怀疑的写入方（按优先级）：

1. `make_uunion` / `append_flattened_set_terms`：union 的 `types` 向量是在 `make_utype_node` 之前构造的，若某个 scratch 向量被多线程共享（例如 thread_local 误用或复用的 `std::vector` 成员），就会出现"某个 slot 是别的线程写了一半的值"。
2. `coalesceCompactType` 的并行路径：compact → UType 的展开里，若 `UUnion::types` 复用了某个共享缓冲区（或 `CompactType` 的记录 map 被并发读取同时被改写），会出现整值级别的污染。
3. `utype_pool()` 的 hash-cons：节点插入后不可变，但 `probe`/`hashCache` 是非原子写；如果某处把池节点的 `v` 当作可变对象复用过（例如把已有节点 `std::move` 进 probe），会污染已发布节点。

TSan 报告出来后，按"两条栈里哪条在写 `std::vector<UType*>` 的 slot"收敛即可。

## 五、影响面

这套 suite 里每轮 1~2 个用例命中（失败集合每轮不同：`24763`、`0032`、`24259` 都出现过），说明是**整个并行求解路径的系统性竞态**，不是某个 contract 的个例。修掉它之后 `solidity_patterns` 才有希望稳定全绿。

本轮的临时诊断代码已全部回退，`external/binarysub` 工作树干净（停在 `5fd11b9`）。
