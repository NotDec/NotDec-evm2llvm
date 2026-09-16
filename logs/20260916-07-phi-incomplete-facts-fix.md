# EVM 链路复检：PHI 不完整事实修复（2026-09-16）

回到 evm2llvm 本体，先做了一次全链路复检，然后修掉 6 月以来一直挂着的两个 evm2llvm 失败样本。

## 一、复检结果

- `notdec.evm.solidity_patterns`：**65 pass / 38 fail，0 个崩溃**（全部 38 个失败都是 oracle 计数漂移，run.log 里 `exit=0`）。之前的 `exit=-6` 崩溃已被 `d1830b1` + `5fd11b9` 两轮修复清掉。
- `notdec.evm.solidity_rewrite`：77 / 1；`notdec.evm.solidity_source`：77 / 1；`notdec.type_recovery.evm`：5 / 11（全部是 storage oracle 漂移，见 03/06 号日志）。
- evm2llvm 本体测试：33/33 通过（修复后复跑仍是 33/33）。
- 6 月 4 日 apehex 批次里两个 evm2llvm 失败样本，用当前源码重建后**仍然失败**，本轮修掉。

## 二、两个失败的成因与修复

### 26708：`PHIIncoming missing predecessor for PHI 0x4e6_0x4 from 0x4e0`

`TAC_Op` 里 `0x4e6_0x4` 确实是 `PHI`，但 `PHIIncoming.csv` 只给了来自 `0x8b6` 的一条 incoming；同块的 `0x4e6_0x0`/`0x4e6_0x1` 都有来自 `0x4e0` 和 `0x8b6` 两条。也就是说**Gigahorse 在部分路径上没有给这个 PHI 变量定值**，而校验器要求"每个 CFG 前驱都必须有 incoming"，直接判为事实不一致并 abort。

这其实不是事实矛盾，而是取值覆盖不全：LLVM 只要求"每条前驱一个 incoming"，并不要求这个值在所有路径上都被定义。修复：

- `SsaFactValidator`：缺前驱 incoming 由 error 改为 warning（来自非前驱的 incoming 仍然是 error）；
- `LlvmLowerer`：新增 `completePhiIncoming()`，在所有 incoming 与 synthetic entry 种子都就位**之后**运行，为每条没有 incoming 的 LLVM 前驱补 `undef` 并告警。

### 22492：`missing SSA value for PHI incoming variable 0x4135_0x0`

该变量在 `PHIIncoming.csv` 里被引用，但 `TAC_Def`/`TAC_Op` 里**完全没有定义**（既不是语句 def，也不在 `TAC_Variable_Value` 里）。原实现直接报错终止，整份模块（含其它上百个函数）都产出失败。修复：`valueForPhiIncoming` 在既无 SSA 值也无常量时改用 `undef` 并告警。

### 中途踩到的坑（值得记）

补 undef 的时机很关键。第一版把补充逻辑放在 `fillPhiIncoming()` 末尾，而 synthetic entry 种子是在之后才加的，于是 entry 作循环头时出现同一个 LLVM 前驱两条 incoming：

    %_0x4fbd_0x0 = phi i256 [ %evm.add, %bb._0x4fc6 ], [ undef, %entry ], [ %_0x4fbdarg0x0, %entry ]

`llvm-as` 直接报 `PHINode should have one entry for each predecessor`，22492 仍然 exit=1。最终把补充逻辑拆成独立 pass（`completePhiIncoming`）放到最后，并在 `fillPhiIncoming` 里跳过"该前驱已有 incoming"的重复事实边，两个样本才都过。

## 三、验证

| 样本 | 修复前 | 修复后 |
| --- | --- | --- |
| 26708_19785111_... | evm2llvm exit=1 | **exit=0，llvm-as 通过** |
| 22492_19734645_... | evm2llvm exit=1 | **exit=0，llvm-as 通过** |

两个样本随后都跑通主项目 `notdec --tr-level=2`（exit=0，正常 dump IR）。evm2llvm 自带 33 个 ctest 全过。

告警量级（供参考）：26708 产生 496 条 PHI 相关 warning，说明该合约的 CFG 前驱覆盖缺口相当多，但 IR 合法且可继续往后走；这类 warning 值得后续汇总统计，判断是 Gigahorse 分析覆盖问题还是前端 facts 导出丢边。

## 四、接下来还剩什么

1. **EVM oracle 统一刷新**：`solidity_patterns` 38 个、`solidity_rewrite` 1 个、`solidity_source` 1 个、`type_recovery.evm` 11 个失败都是计数/形态漂移（含 storage 的 `top:0 whole`），需要一次性对齐当前 MLsub 输出。
2. **未跑过的批次**：selected-apehex-80/50 全量重跑一遍，看看修掉 PHI 问题后还有多少 `notdec_tr` 失败（6 月批次是 36 ok / 14 fail，其中崩溃类已被前两轮修复覆盖）。
3. 可选：把 `completePhiIncoming` 的 warning 数量纳入批次汇总，用来评估 Gigahorse 覆盖缺口。
