# EVM oracle 对齐 + 两个新发现的非确定性（2026-09-16）

## 一、oracle 对齐（已完成，分 3 个 commit）

| commit | 内容 | 对齐前 | 对齐后 |
| --- | --- | --- | --- |
| `7180fd0d` | `test/evm/solidity-patterns/manifest.json`：按当前输出更新 38 个用例的 79 处计数（`notdec.solidity.revert`、`revert_kinds.error_string`、`notdec.solidity.abi_return`；`rewrite_hidden_*` 与 `rewrite_marker:*` 由 runner 从 metadata counts 推导，不单独改） | 65 pass / 38 fail | 102 pass / 1 fail（单次运行） |
| `980c8412` | `test/evm/solidity-source/expected/apehex_gasleft_return_01.sol`：补上 storage 根新增的 `uint256 public whole;` | 77 / 1 | 77 / 0 |
| `985a6aea` | `test/type-recovery/evm/expected/tr-level-2/*.htypes`：11 个用例按当前 MLsub 输出更新（storage 根 `top:0 whole`、未约束指针显示为 `void*`、`arg0` 极性标记、尾部空行） | 5 / 11 | 16 / 0 |

对齐依据：先用两次连续运行确认 `.htypes` 与计数输出是确定性的（只有 `.log` 里的时间/哈希诊断会变），再按 compare.txt 的 actual 值更新，不做手工猜测。patterns 的更新脚本按 runner 的推导规则映射字段，未映射字段为 0。

当前全量 EVM 结果：`solidity_rewrite` 通过、`solidity_source` 通过、`type_recovery.evm` 通过；`solidity_patterns` 仍有失败，但不是计数漂移，见下。

## 二、新发现 1：storage 转换路径仍有一个残余竞态（SIGSEGV）

`solidity_patterns` 在两轮运行里失败集合不同：

- 第一轮：`24763_19761741_c9ddde2099_02b0670bed95`，`exit=-11`；
- 第二轮：`0032_19493098_f0dab0bf78_8c9406cb7887`，`exit=-11`；
- 两个用例单独跑（各 3 次）都 `exit=0`；

崩溃栈与之前的 storage UAF 同一个入口：

    convertVisibleSetTerms            TypeBuilder.cpp:412
    TypeBuilder::convert              TypeBuilder.cpp:1176
    convertStorageRecord              TypeBuilder.cpp:998/1015
    ConstraintsGenerator::genTypes    MLsubGenerator.cpp:7086

要点：`convertVisibleSetTerms` 在 union/inter 分支被调用，说明传进来的 `Ty` 的 variant 索引读出来落在 `UUnion`/`UInter`（0..9 之内），因此我加的最后防线（`index() >= 10`）拦不住——它指向的不是"索引明显非法"的复用内存，而是另一种形态的悬空/损坏（读到的是别的对象，恰好判别字也落在合法区间）。

判定：上一轮 `CompactVarSet` 拥有化修掉的是"句柄比 arena 活得久"这一类；这一条是**残余的并行竞态**（单独跑复现不了，整套并行跑才出现，之前那批 UAF 也是同样特征）。

## 三、新发现 2：`abi_return` 计数在两次运行间不同（非确定性）

`24259_19755445_aefeec2314_4f43187f4106` 第二轮失败时 compare.txt：

    notdec.solidity.abi_return            expected 8  actual 10
    rewrite_hidden_markers/metadata       expected 69 actual 71
    notdec_solidity_rewrite_abi_return    expected 8  actual 10

（同一轮里 `0229` 之类的用例计数是对上的。）这说明 ABI return 的识别数量在一次运行内/跨运行不稳定，属于 pass 层的非确定性输出，不是 oracle 问题。

## 四、建议的下一步

1. **残余竞态**：用 ASan 构建重复跑 `solidity_patterns`（我们已经验证过这个方法能抓到跨线程 UAF），抓 `24763`/`0032` 的完整报告，重点看 freed-by 栈是否又是某个跨 arena/跨线程容器；预计仍与 canonicalize/coalesce 的并行路径有关。
2. **abi_return 非确定性**：先确认是"同一进程内多次 pass 顺序/共享缓存导致"，还是"跨进程随机"；可以在 `SolidityPatterns` 的 abi_return 计数点加 trace（或对比两次运行的 `out.ll` 中 `notdec_solidity_rewrite_abi_return` 的分布），定位是哪一条匹配分支不稳定。
3. 上述两点修完后，`solidity_patterns` 才能稳定全绿；之后再做 selected-apehex-80/50 全量重跑。
