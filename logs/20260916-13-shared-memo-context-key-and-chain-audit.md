# Cross-root shared memo 的上下文键修复 + EVM 链路复查（2026-09-16 续）

## 一、修复：shared memo 漏了 `newRecVars` 上下文

上篇日志已经把 `24259` 的 `abi_return` 8/10 非确定性定位到
`CoalesceMemo`。这次把它修到了可复现的根因：

`CoalesceMemo` 原 key 只包含：

    (CompactType structural hash, polarity)

但 `go()` 展开同一个 CompactType 时还依赖当前 group 的：

    TypeSimplifier::newRecVars   // variable -> recursive bound

同一个结构在不同 root/group 里，变量可能一边是普通变量、一边是递归
bound，展开结果结构不同且 `outPathHit == false`，于是两个 group 都认为
结果 path-independent，先写入 shared memo 的并行 group 决定了另一个
group 的输出。

修复（external/binarysub `683cf62`）：

- `CoalesceMemoEntry` 增加 `Context` 字段；
- `coalesceCompactType()` 在展开前对当前 `newRecVars` 做一次结构化
  fingerprint（variable id/size + bound structuralHash）；
- `lookupSharedMemo` / `storeSharedMemo` 同时匹配 `Context`，context
  不同的 group 不再跨 root 复用结果。

这样可以保留“同 structure + 同 recursive-bound 上下文”的共享收益，
只禁止上下文不同的错误共享。

## 二、验证

- `24259` 默认并行 30 次：30/30 都是 `abi_return=8`（修复前 30 次里
  5 次为 10）；
- `notdec.evm.solidity_patterns` 连续 6 轮全部通过（修复前 6 轮里 2 轮
  因 `24259` 失败）；
- `notdec.evm.solidity_rewrite`、`notdec.evm.solidity_source`、
  `notdec.type_recovery.evm.tr_level_2`、`notdec.lifting.wasm` 全部通过；
- `TypeBuilderTest` 9/9、`MLsubGeneratorTest` 20/20；
- ASan 构建下 `24763`、`0032` 各 5 次 16 线程无 AddressSanitizer 报告；
- `24763` / `0032` 普通构建各 400 次 16 线程加压，0 崩溃；
- 最大 pattern 用例之一 `0258` 单跑 wall/RSS 与旧 shared-memo 构建持平
  （0.8s / ~139MB），shared memo 的有效共享没有被 context key 明显破坏。

## 三、EVM 链路复查结果

### 已确认正常

- `external/NotDec-evm2llvm` 自带 33 个 fixture 测试
  （emit / llvm-as / opt-verify / poison warning / branch checks）
  连跑 50 轮全部通过；
- 主链路 EVM suite：solidity_patterns、solidity_rewrite、
  solidity_source、type_recovery.evm、lifting.wasm 全部可通过；
- `TypeBuilderTest` / `MLsubGeneratorTest` 可构建且通过（主 build
  默认未 build，需要显式 `ninja TypeBuilderTest MLsubGeneratorTest`）。

### 仍需继续修

1. **HType dump 的结构级并行非确定性仍在**
   `--dump-htypes` 对 `24259` 跑 5 次仍然得到 5 个不同文件；其中部分
   差异是 `rec_*` 命名/decl 编号，部分是 union/record 布局差异。
   `NOTDEC_BINARYSUB_THREADS=1` 时只剩 `rec_*` 命名差异，
   说明并行 group 里还有 shared memo 之外的共享状态或顺序依赖。
   目前不影响已有 suite 的计分 oracle，但会影响逐文本回归和更大范围的
   非确定性。下一步优先查：
   - `LocalResults` 组装成 `uMap` 时的遍历/命名顺序；
   - TypeBuilder 的 `ExactRecordLayoutDecls` 是否因 root 处理顺序不同而
     产生不同 decl 复用；
   - `FreshName` 计数是否可用于把 `struct_N`/`rec_N` 改成结构稳定命名。
2. **`MLsubGeneratorTest` / `TypeBuilderTest` 默认不 build**
   主 build 的 ctest 会显示 `_NOT_BUILT`。建议 CI 显式构建这两个
   target，并把 binarysub 的关键回归（StorageTreeNode::Leaf、
   shared-memo context）加进去。
3. **evm2llvm 自己的 33 个 fixture 测试不在主 ctest 中**
   目前要单独 `ctest --test-dir external/NotDec-evm2llvm/build`。
   建议在主项目增加聚合 target 或 CI 步骤，避免 EVM frontend 改动后
   只跑主 EVM suite 而漏掉 emit/verifier 回归。
4. **Gigahorse 端到端测试默认 off**
   bytecode -> Gigahorse -> evm2llvm 的真正前置链路没有默认 CI。
   至少加一个 off-by-default 的 smoke test target，或在有 Gigahorse
   的机器上定期跑。`--restart --disable_inline` 的 PHI 调查口径已经
   写在 docs/README.md，可以直接作为脚本参数。
5. **非 EVM oracle 漂移**
   `notdec.type_recovery.llvm_ir.tr_level_2` 当前 8/14、
   `sysy` 0/9、`realworld` 的 fortune extra-constraints 哈希失配
   都是已有问题，与本次 EVM 修复无关，需要单独排期。

## 四、提交

- external/binarysub `683cf62` coalesce: key shared memo by
  recursive-bound context
- 主项目 `8372a2ea` 更新 submodule 指针
- 本日志所在 evm2llvm 子模块 commit 见仓库历史
