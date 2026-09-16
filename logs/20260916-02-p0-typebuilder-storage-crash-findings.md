# P0 定位：EVM storage 的失效 UType 指针导致 TypeBuilder 崩溃（2026-09-16）

承接 `logs/20260916-01-evm-status-and-next-fixes.md` 第三节。本轮把“P0 崩溃”
从“栈上看到 Unhandled UType variant”推进到“定位到具体的内存/指针机制”。

## 结论摘要

崩溃的直接原因是：**TypeBuilder::convertStorageRecord 在构造 storage 树时，
拿到的一个 UType* 指向的已经不是 UType 对象**。UType* 是裸指针
（`using UTypePtr = UType *`，见 binarysub.h:66），节点由类型池（arena）持有；
这个指针指向的内存已被复用成别的对象，于是：

- `Ty->v.index()` 读出垃圾值（实测出现过 46、96、121、-34）；
- 该值不匹配任何 variant，convert() 走到最后的 else /
  convertVisibleSetTerms 的 `assert(!Terms.empty())`；
- 在 Release 构建里断言被吃掉，printType 直接踩空指针 → SIGSEGV；
  在带断言的构建里则是 SIGABRT。

## 复现

崩溃样本（solidity-patterns suite 的真实用例）：

    cd /sn640/NotDec
    /tmp/build-evm-p0/bin/notdec test/evm/solidity-patterns/cases/0014_proxy_like.ll -o /tmp/x.ll --tr-level=2

同一崩溃在 0002_delegatecall_no_nonpayable 等用例、type_recovery.evm 的
10_evm_storage_bytes_long、以及 apehex 样本
0358_19494430_b7d763ebaf_31239f3e312c.ll 上都能复现。

## 观测到的数据流

在 TypeBuilder::convertStorageRecord 与
ConstraintsGenerator::getOrCreateStorageField 临时加打印后（跑 0014_proxy_like）：

    DEBUG storage field created name='slot:1' var=0x557204ae6950
    DEBUG storage field created name='slot:0' var=0x557204aea260
    DEBUG storage field path='slot:0' leaf=0x557204a24fe0 variant=9
    DEBUG storage field path='slot:1' leaf=0x557204a24fe0 variant=9
    DEBUG storage root=0x557204a2c770 variant=5 fields=2
    DEBUG raw root-leaf ptr=0x5572048ab250 idx=18446744073709551584 bytes:
          5577538682ab 557200000000 557204b28460 557204b28460 557204b28460 1 0 0
    DEBUG root-leaf-path=''

要点：

1. 约束生成阶段只创建了两个 storage 字段：slot:0、slot:1，都是
   make_variable 出来的变量节点。
2. 求解后的 URecordType（variant=5，2 个字段）里，两个字段都解析到
   **同一个** UType（variant=9，URecordType）。
3. 但 storage 树的**根叶节点** Root.Leaf 是一个 variant 索引为
   18446744073709551584（即 -34 的补码）的“UType”。
4. 该地址的原始内存是：

       6f6c5f6d76652c34  -> ASCII "4,evm_lo"
       6c5f6d7600000000  -> ASCII "vm_l" + 4 个 NUL
       55... 55... 55... -> 三个相同堆指针
       1 0 0             -> 形如 vector{ptr,size=1}

   说明该内存已被复用为“字符串 + vector 头”的对象，不再是 UType。
5. Root.LeafPath 是空串。这里需要更正上一版的一处推测：这个“空名/根叶”
   字段**不是**本轮新引入的异常。2026-09-15 的旧二进制在
   09_evm_storage_bytes_short 上成功产出过：

       struct struct_0 {
         top:0 whole; /* storage path:  */
         struct_1 slot_0; /* storage path: slot:0 (slot 0) */
       }; /* EVM storage root */

   也就是说根级 `whole` 字段（LeafPath 为空）本来就是 EVM storage 的
   预期形态，它对应“storage 根也有一个整块类型”的表达。
   **真正变化的是这个字段的 UType 指针在转换时已经失效**，而不是
   空名字段的出现。这解释了为什么 09/11~16 的 oracle 差异里都会有
   `top:0 whole`：那是设计行为，oracle 只是还没跟上。

## 与旧构建的差异（重要）

build-notdec-nothreads2 里的 bin/notdec（2026-09-15 05:55）**比它同目录的
静态库旧**：external/binarysub/libbinarysub_lib.a 与
src/libnotdec-typerecovery.a 是 2026-09-16 05:29 的，而可执行文件没有重链。
该目录的 `ninja -n` 也显示还有 46 个目标待构建。

因此：

- 该二进制自带的 binarysub 不是当前源码，它报的
  `TypeBuilder.cpp:1135 Unhandled UType variant` 只是同一现象的旧行号；
- **“旧二进制崩、干净新构建不崩”不能当作修复证据**。

本轮把 build-notdec-nothreads2 用当前源码重建后重跑 EVM suite：

| suite | 干净构建（当前源码） |
| --- | --- |
| notdec.evm.solidity_patterns | 49 pass / **54 fail**（22 个 run.log 带崩溃标记，其中 12 个是同一 `Unhandled UType variant` 断言） |
| notdec.evm.solidity_rewrite | 72 pass / 3 fail |
| notdec.evm.solidity_source | 77 pass / 1 fail |
| notdec.type_recovery.evm.tr_level_2 | 5 pass / 11 fail |

对比之下，09-15 的旧二进制是 patterns 76/27、type_recovery 6/10。
干净构建失败更多，说明这批 EVM oracle 明显落后于当前源码；
其中一部分是崩溃，另一部分是 oracle 漂移，**必须先修崩溃再谈 oracle**。

## 已排除 / 待验证

已排除：

- 不是 `Ty == nullptr`：实测传进来的指针非空，只是指向的内容失效。
- 不是 TypeBuilder::convert 漏了 variant 分支：10 个 variant 全覆盖。
- 不是 convertStruct 的字段范围溢出（那是 binarysub 7993188 修的
  Microsub2 问题，路径不同）。
- 不是“空名字段”本身：见上一节第 5 点，根级 whole 是预期形态。

待验证（下一步的定位顺序）：

1. **确认指针失效的时机**：对求解返回的 storage URecordType 的每个字段
   指针，在 `bulkSimplifyDetailed` 返回后、`genTypes` 转换前后各打印一次
   variant index。如果求解前有效、转换时失效，问题在求解器返回的 `Res`
   或 bulk 结果里保存了失效 UType*；这是一条最直接的证据链。
2. **确认内存复用来源**：坏指针与 slot:* 变量节点在同一段堆
   （0x5572048xxxxx vs 0x557204axxxxx），说明它是类型池释放/复用后的
   地址，而不是野指针。可以在 TypePool/arena 的释放或 bulk 结果重建
   路径上打点。
3. **确认 solve 前后 StorageFields 与 record fields 的对应**：
   在 genTypes 调用 convertStorageRecord 之前打印
   `(name, UTypePtr)`，与 getOrCreateStorageField 记录的
   `(name -> make_variable 返回值)` 对比，确认哪些指针换了地址。

## 修复方向（按优先级）

1. **治本**：让 storage 记录在求解往返中不产生失效 TypeRef。
   优先怀疑“求解器返回的 Res 里记录的 storage 字段 UType* 与约束生成
   阶段 StorageFields 里的变量不是同一批节点”，需要把两者的指针一一对上。
2. **治标（可立刻做，防 SIGSEGV）**：convert() 入口做 variant 合法性检查。
   `Ty->v.index() >= 10` 说明这不是一个 UType：

        if (Ty == nullptr || Ty->v.index() >= 10) {
          std::cerr << "Warning: TypeBuilder: dropping invalid UType pointer "
                    << static_cast<const void *>(Ty) << "\n";
          dumpDebugPath(std::cerr, CurrentRootDebugLabel, CurrentDebugPath);
          return getTopType(0);  // 或返回 nullptr，由调用方跳过该字段
        }

   注意：index 检查廉价且安全（只读 variant 判别字），对“内存复用成其它
   对象”的场景有效；但若内存恰好还是原对象形状则不生效，所以它只是防
   崩溃，不是根因修复。
3. **顺带**：把 09/11~16 的 EVM storage oracle 更新为当前形态
   （含 `top:0 whole`），这部分与崩溃修复分开提交，避免混淆。

## 本轮验证方式（可复现）

    # 干净构建（源码 = 工作树，RelWithDebInfo + 断言）
    cmake -S /sn640/NotDec -B /tmp/build-evm-p0 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build /tmp/build-evm-p0 --target notdec -j4

    # 崩溃用例
    /tmp/build-evm-p0/bin/notdec \
      /sn640/NotDec/test/evm/solidity-patterns/cases/0014_proxy_like.ll \
      -o /tmp/x.ll --tr-level=2

    # 全量 EVM 结果
    ctest --test-dir /tmp/build-evm-p0 -R "notdec.evm|notdec.type_recovery.evm" --output-on-failure

## 调试脚手架清理

本轮为定位加的临时打印（TypeBuilder.cpp 的 dumpDebugPath/dumpUTypesRaw/
unhandled variant 打印、MLsubGenerator.cpp 的 storage 字段创建打印）已经
全部删除，external/binarysub 工作树恢复为 d361eb7 干净状态。
/tmp/build-evm-p0 只是本地工作构建，不在仓库内；
build-notdec-nothreads2 已用当前源码重建（bin/notdec 2026-09-16 05:53）。
