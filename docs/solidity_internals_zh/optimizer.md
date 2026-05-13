# 优化器

Solidity 编译器的优化分成三个层次，按执行顺序依次是：

- 基于 Solidity 源码直接分析的代码生成期优化
- 基于 Yul IR 的优化
- 基于 opcode 的优化

opcode 优化器会把一组 [简化规则](https://github.com/argotorg/solidity/blob/develop/libevmasm/RuleList.h) 应用到 opcode 上，同时合并相同的代码块并删除未使用代码。

Yul 优化器更强，因为它可以跨函数工作。比如，Yul 中没有任意跳转，因此可以分析每个函数的副作用。若第一个函数不修改 storage，而第二个函数会修改 storage，并且它们的参数和返回值互不依赖，就可以交换调用顺序。类似地，如果某个函数没有副作用，而它的结果最终乘以 0，那么这个函数调用可以直接删除。

代码生成期优化会影响从 Solidity 输入生成的初始低层代码。在 legacy pipeline 中，字节码会立即生成，大部分这类优化是隐式且不可配置的，唯一例外是会改变二元运算中字面量顺序的优化。

IR-based pipeline 则会先生成与 Solidity 结构高度对应的 Yul IR，把大部分优化推迟到 Yul 优化器里。在这种情况下，代码生成期优化只在少数难以在 Yul IR 中处理、但结合高层分析信息又很容易处理的场景里使用。典型例子是某些惯用 `for` 循环中递增计数器时绕过 checked arithmetic。

当前，`--optimize` 会同时启用：

- 生成字节码的 opcode 优化器
- 内部生成的 Yul 代码的 Yul 优化器，例如 ABI coder v2

你可以用 `solc --ir-optimized --optimize` 生成优化后的 Yul IR。类似地，`solc --strict-assembly --optimize` 可用于独立的 Yul 模式。

> **注意**
>
> 某些优化步骤，例如 [peephole optimizer](https://en.wikipedia.org/wiki/Peephole_optimization) 和 [unchecked loop increment optimizer](#unchecked-loop-increment)，默认始终开启，只能通过 [Standard JSON](https://docs.soliditylang.org/en/latest/using-the-compiler.html#compiler-input-and-output-json-description) 关闭。

> **注意**
>
> 空的优化序列，也就是 `:`，即使没有 `--optimize` 也会被接受。这么做是为了完全禁用用户提供的 Yul [optimizer sequence](#selecting-optimizations)。默认情况下，即便优化器没有整体开启，`unused pruner` 步骤也还是会运行。

## 优化 Solidity 代码的收益

优化器的总体目标是简化复杂表达式，从而同时减少代码体积和执行成本，也就是减少合约部署 gas，以及合约外部调用时的 gas。

它还会做函数特化或内联。尤其是函数内联，通常会让代码体积变大，但往往能带来更多后续简化机会，因此值得做。

## 优化与非优化代码的差异

最明显的差异是常量表达式会在编译期求值。

对于 ASM 输出，还会看到等价或重复代码块被合并或删除，例如比较 `--asm` 和 `--asm --optimize` 的输出。

对于 Yul / IR 输出，差异会更明显，例如函数可能被内联、合并或重写，从而消除冗余。可以比较 `--ir` 与 `--optimize --ir-optimized` 的结果。

## `--optimize-runs`

`--optimize-runs` 表示部署后的代码在合约生命周期内大致会执行多少次。它本质上是在代码大小（部署成本）和执行成本（部署后成本）之间做权衡。

- `runs = 1`：代码更短，但执行更贵
- 较大的 `runs`：代码更长，但更省 gas

该参数最大值为 `2**32-1`。

> **注意**
>
> 很多人会误以为这个参数表示优化器循环执行的次数，但并不是。优化器会一直运行，直到无法继续改进代码为止。

## 基于 Opcode 的优化器

opcode 优化器在汇编代码上工作。它会在 `JUMP` 和 `JUMPDEST` 处分割基本块。

在每个基本块内，优化器会分析指令，并把对 stack、memory 或 storage 的每次修改记录成一个表达式。这个表达式由一条指令和若干参数组成，参数本身又是指向其他表达式的指针。

它还会使用一个名为 `CommonSubexpressionEliminator` 的组件，负责找到所有输入上都恒等的表达式并把它们合并成一个表达式类。处理方式是：

1. 先在已知表达式列表中查找新表达式
2. 如果没找到匹配项，就按规则简化，比如 `constant + constant = sum_of_constants` 或 `X * 1 = X`

由于这是递归过程，即使第二个因子是更复杂的表达式，只要我们知道它恒等于 1，`X * 1 = X` 仍然可用。

某些优化步骤会符号化地跟踪 storage 和 memory 位置。例如，这些信息可以用来在编译期直接计算 Keccak-256。考虑下面的序列：

```none
PUSH 32
PUSH 0
CALLDATALOAD
PUSH 100
DUP2
MSTORE
KECCAK256
```

或者等价的 Yul：

```yul
let x := calldataload(0)
mstore(x, 100)
let value := keccak256(x, 32)
```

这种情况下，优化器会追踪 memory 位置 `calldataload(0)` 上的值，进而发现 Keccak-256 可以在编译期算出来。

前提是 `mstore` 和 `keccak256` 之间没有其他修改 memory 的指令。也就是说，如果有指令写入 memory 或 storage，就必须清除当前已知的 memory / storage 信息。例外情况是：如果我们能很容易看出某条指令不会写到某个位置，就不必清除对应信息。

例如：

```yul
let x := calldataload(0)
mstore(x, 100)
// 当前已知：memory[x] -> 100
let y := add(x, 32)
// 不会清除 memory[x] 的知识，因为 y 不会写到 [x, x + 32)
mstore(y, 200)
// 现在可以把这个 keccak256 在编译期算出来
let value := keccak256(x, 32)
```

因此，对某个位置 `l` 的 storage / memory 修改，必须清除所有可能等于 `l` 的符号位置的知识。更具体地说：

- 对 storage，优化器要清除所有可能等于 `l` 的符号位置知识
- 对 memory，优化器要清除所有不可能至少离 `l` 有 32 字节距离的符号位置知识

如果 `m` 表示任意位置，那么这个清除决策是通过计算 `sub(l, m)` 完成的：

- 对 storage，如果该值被求值为一个非零字面量，就保留 `m` 的知识
- 对 memory，如果该值被求值为一个介于 `32` 和 `2**256 - 32` 之间的字面量，就保留 `m` 的知识
- 其他情况都要清除 `m` 的知识

完成这一步后，我们就知道栈末尾需要保留哪些表达式，也知道对 memory 和 storage 做了哪些修改。优化器会把这些信息和基本块一起存储起来，用于连接各个基本块。同时，stack、storage 和 memory 的知识会传递到后续基本块。

如果我们知道所有 `JUMP` 和 `JUMPI` 的目标，就能构造出完整控制流图。如果存在一个未知跳转目标，就必须清除某个基本块的输入状态知识，因为它可能会成为未知 `JUMP` 的目标。

如果 opcode 优化器发现某个 `JUMPI` 的条件是常量，它会把这个条件跳转改写为无条件跳转。

最后一步是重新生成每个块的代码。优化器会从块末尾的栈表达式构造依赖图，并删除不在这个依赖图中的操作。随后，它会按原始顺序生成对 memory 和 storage 的修改（丢弃那些被判定不需要的修改）。最后再把栈上需要的所有值生成到正确位置。

这些步骤会对每个基本块执行一次，且如果新生成的代码更小，就用它替换旧代码。

如果一个基本块在 `JUMPI` 处分裂，并且在分析过程中条件值变成了常量，那么 `JUMPI` 会根据常量值被替换。比如下面这段 Solidity：

```solidity
uint x = 7;
data[7] = 9;
if (data[x] != x + 2) // 这个条件永远不成立
  return 2;
else
  return 1;
```

会被简化成：

```solidity
data[7] = 9;
return 1;
```

### Simple Inlining

从 Solidity 0.8.2 开始，优化器新增了一步：它会把某些跳到“简单”指令块、且这些指令块最终以 `jump` 结束的跳转，替换成这些指令的拷贝。这相当于把简单的小 Solidity 或 Yul 函数做内联。

具体来说，形如 `PUSHTAG(tag) JUMP` 的序列可能会被替换，只要这个 `JUMP` 被标记为“进入函数”的跳转，并且 `tag` 后面有一个基本块，且该基本块最后以另一个被标记为“从函数返回”的 `JUMP` 结束。

一个典型的内部 Solidity 函数调用汇编示例是：

```text
      tag_return
      tag_f
      jump      // in
    tag_return:
      ...call f 之后的 opcode...

    tag_f:
      ...function f 的主体...
      jump      // out
```

如果函数体是连续的基本块，Inliner 可以把 `tag_f jump` 替换为 `tag_f` 处的块：

```text
      tag_return
      ...function f 的主体...
      jump
    tag_return:
      ...call f 之后的 opcode...

    tag_f:
      ...function f 的主体...
      jump      // out
```

理想情况下，后续优化步骤会把返回标签的 push 移动到剩余的 jump 附近：

```text
      ...function f 的主体...
      tag_return
      jump
    tag_return:
      ...call f 之后的 opcode...

    tag_f:
      ...function f 的主体...
      jump      // out
```

此时 `PeepholeOptimizer` 会删除这个返回跳转。再进一步，所有对 `tag_f` 的引用都被清掉，使其变成未使用，最终可以删除整个函数定义：

```text
    ...function f 的主体...
    ...call f 之后的 opcode...
```

也就是说，函数 `f` 被内联了，原始定义可以删除。

只要启发式判断在函数生命周期内，内联比不内联更便宜，就会尝试这种内联。这个启发式依赖于：

- 函数体大小
- 指向该 tag 的其他引用数量，也就是大致的调用次数
- 合约预计执行次数，也就是全局优化参数 `runs`

## 基于 Yul 的优化器

Yul 优化器由多个阶段和组件组成，它们会以语义等价的方式转换 AST。目标是得到更短，或者至少不会长太多、但能让后续优化继续发挥作用的代码。

> **警告**
>
> 由于优化器还在快速演进，这里的信息可能已经过时。如果你依赖某个特定功能，最好直接联系团队确认。

当前优化器采用纯贪心策略，不做回溯。

Yul 优化器的主要组件包括：

- `SSATransform`
- `CommonSubexpressionEliminator`
- `ExpressionSimplifier`
- `UnusedAssignEliminator`
- `FullInliner`

### 优化步骤

下面是 Yul 优化器的全部步骤，按字母顺序排列：

| 缩写 | 全名 |
| --- | --- |
| `f` | `BlockFlattener` |
| `l` | `CircularReferencesPruner` |
| `c` | `CommonSubexpressionEliminator` |
| `C` | `ConditionalSimplifier` |
| `U` | `ConditionalUnsimplifier` |
| `n` | `ControlFlowSimplifier` |
| `D` | `DeadCodeEliminator` |
| `E` | `EqualStoreEliminator` |
| `v` | `EquivalentFunctionCombiner` |
| `e` | `ExpressionInliner` |
| `j` | `ExpressionJoiner` |
| `s` | `ExpressionSimplifier` |
| `x` | `ExpressionSplitter` |
| `I` | `ForLoopConditionIntoBody` |
| `O` | `ForLoopConditionOutOfBody` |
| `o` | `ForLoopInitRewriter` |
| `i` | `FullInliner` |
| `g` | `FunctionGrouper` |
| `h` | `FunctionHoister` |
| `F` | `FunctionSpecializer` |
| `T` | `LiteralRematerialiser` |
| `L` | `LoadResolver` |
| `M` | `LoopInvariantCodeMotion` |
| `m` | `Rematerialiser` |
| `V` | `SSAReverser` |
| `a` | `SSATransform` |
| `t` | `StructuralSimplifier` |
| `r` | `UnusedAssignEliminator` |
| `p` | `UnusedFunctionParameterPruner` |
| `S` | `UnusedStoreEliminator` |
| `u` | `UnusedPruner` |
| `d` | `VarDeclInitializer` |

某些步骤依赖 `BlockFlattener`、`FunctionGrouper`、`ForLoopInitRewriter` 保证的属性。因此 Yul 优化器会始终先执行这些步骤，再执行用户提供的步骤。

### 选择优化

默认情况下，优化器会把预定义序列应用到生成的汇编上。你可以通过 `--yul-optimizations` 指定自己的序列：

```bash
solc --optimize --ir-optimized --yul-optimizations 'dhfoD[xarrscLMcCTU]uljmul:fDnTOcmu'
```

步骤顺序很重要，会直接影响输出质量。而且执行某个步骤后，可能会为已经执行过的步骤解锁新的优化机会，所以重复执行某些步骤往往有益。

`[...]` 内的序列会循环执行多轮，直到 Yul 代码不再变化，或者达到最大轮数（当前是 12）。一个序列里可以出现多个方括号，但不能嵌套。

还有一些硬编码步骤会在用户提供序列之前和之后固定执行，不论你是否提供了默认序列。

cleanup 序列分隔符 `:` 是可选的。它用于提供自定义 cleanup 序列，以替换默认 cleanup 序列。如果省略，优化器就会直接使用默认 cleanup 序列。`:` 也可以放在用户序列开头，此时优化序列为空；如果放在末尾，则表示 cleanup 序列为空。

## 预处理

预处理组件负责把程序转换成更适合优化的某种规范化形式。这个规范化形式会在后续优化过程中保持。

### Disambiguator

Disambiguator 会接收 AST，并返回一个新副本，其中所有标识符都拥有唯一名称。这是其他所有优化阶段的前置条件。

这样做的好处之一是：标识符查找不再需要考虑作用域，其他步骤的分析会更简单。

后续所有阶段都满足“名称始终唯一”的性质。也就是说，如果需要引入一个新标识符，就必须生成一个新的唯一名字。

### FunctionHoister

FunctionHoister 会把所有函数定义移动到最外层 block 的末尾。只要它在 disambiguation 之后执行，这种变换就和原语义等价。

原因是，把一个定义移动到更高层级的 block 不会降低它的可见性，而且不同函数中定义的变量不可能互相引用。

这个阶段的好处是函数更容易查找，且可以不必完整遍历 AST 就对单个函数做独立优化。

### FunctionGrouper

FunctionGrouper 必须在 Disambiguator 和 FunctionHoister 之后执行。

它的作用是把所有最顶层且不是函数定义的元素，移动到根 block 的第一个语句中，形成一个单独的 block。

执行后，程序会变成如下规范形式：

```text
{ I F... }
```

其中 `I` 是一个（可能为空的）不包含任何函数定义的 block，`F` 是一组函数定义，并且这些函数内部也不包含函数定义。

这个阶段的好处是，我们总是知道函数列表从哪里开始。

### ForLoopConditionIntoBody

这个变换会把 `for` 循环的条件移动到循环体中。

原因是 `ExpressionSplitter` 不能处理迭代条件表达式（下面例子中的 `C`）。

```text
for { Init... } C { Post... } {
    Body...
}
```

会被变换成：

```text
for { Init... } 1 { Post... } {
    if iszero(C) { break }
    Body...
}
```

这个变换也可以和 `LoopInvariantCodeMotion` 一起使用，因为循环不变式条件可以因此被移到循环外。

### ForLoopInitRewriter

这个变换会把 `for` 循环的初始化部分移动到循环外：

```text
for { Init... } C { Post... } {
    Body...
}
```

变成：

```text
Init...
for {} C { Post... } {
    Body...
}
```

这样后续优化会更容易，因为可以忽略 `for` 初始化块复杂的作用域规则。

### VarDeclInitializer

这个步骤会重写变量声明，使它们全部显式初始化。像 `let x, y` 这样的声明会被拆成多个声明语句。

目前只支持用零字面量初始化。

## 伪 SSA 转换

这一组组件的目标是把程序变成更长的形式，这样其他组件更容易处理。最终表示会类似于 static-single-assignment（SSA）形式，但不会使用显式的 `phi` 函数，因为 Yul 语言里没有这种从不同控制流分支合并值的机制。

相反，当控制流汇合时，如果某个变量在某个分支里被重新赋值，就会声明一个新的 SSA 变量来保存它的当前值，这样后续表达式仍然只需要引用 SSA 变量。

### ExpressionSplitter

ExpressionSplitter 会把像 `add(mload(0x123), mul(mload(0x456), 0x20))` 这样的表达式拆成一串唯一变量的声明，每个声明对应原表达式的一个子表达式，使得每个函数调用的参数都只是变量。

这个例子会变成：

```yul
{
    let _1 := 0x20
    let _2 := 0x456
    let _3 := mload(_2)
    let _4 := mul(_3, _1)
    let _5 := 0x123
    let _6 := mload(_5)
    let z := add(_6, _4)
}
```

这个变换不会改变 opcode 或函数调用的顺序。

它不适用于循环迭代条件，因为循环控制流不允许在所有情况下把内部表达式这样“展开”。可以先用 `ForLoopConditionIntoBody` 把条件移入循环体，再处理。

最终程序应该处于一种 *expression-split form*：除了循环条件外，函数调用不能嵌套在表达式里，而且所有函数调用参数都必须是变量。

这种形式的好处是：更容易重新排序 opcode 序列，也更容易做函数调用内联和表达式树重组。缺点是人类更难读。

### SSATransform

这个阶段尽量把对已有变量的重复赋值替换成新变量声明。

例子：

```yul
{
    let a := 1
    mstore(a, 2)
    a := 3
}
```

会变成：

```yul
{
    let a_1 := 1
    let a := a_1
    mstore(a_1, 2)
    let a_3 := 3
    a := a_3
}
```

精确语义如下：

对于任何在代码中被赋值的变量 `a`，执行下面变换：

- 把 `let a := v` 改成 `let a_i := v   let a := a_i`
- 把 `a := v` 改成 `let a_i := v   a := a_i`，其中 `i` 是一个尚未使用的编号

同时，始终记录 `a` 当前使用的 `i`，并把每次对 `a` 的引用替换成 `a_i`。

如果变量 `a` 在某个 block 结束时被赋值过，那么它的当前值映射会被清除；如果它是在 `for` 循环体或 post block 中赋值，那么在 `for` 循环 init block 结束时也会清除。若变量的值按照上述规则被清除，而该变量是在外层声明的，那么在控制流汇合处会创建一个新的 SSA 变量，包括循环 post/body block 的开头，以及 `if` / `switch` / `for` / block 语句之后的位置。

完成这一步后，建议再运行 `UnusedAssignEliminator` 来删除不必要的中间赋值。

这一步在 `ExpressionSplitter` 和 `CommonSubexpressionEliminator` 紧接着运行时效果最好，因为这样不会生成过多变量。反过来，`CommonSubexpressionEliminator` 也可能在 SSA transform 之后更高效。

### UnusedAssignEliminator

SSA transform 总会生成形如 `a := a_i` 的赋值，即使在很多场景下它们是不必要的。

例如：

```yul
{
    let a := 1
    a := mload(a)
    a := sload(a)
    sstore(a, 1)
}
```

经过 SSA transform 后：

```yul
{
    let a_1 := 1
    let a := a_1
    let a_2 := mload(a_1)
    a := a_2
    let a_3 := sload(a_2)
    a := a_3
    sstore(a_3, 1)
}
```

`UnusedAssignEliminator` 会删除对 `a` 的三次赋值，因为 `a` 的值根本没有被使用，于是变成严格 SSA 形式：

```yul
{
    let a_1 := 1
    let a_2 := mload(a_1)
    let a_3 := sload(a_2)
    sstore(a_3, 1)
}
```

判断一个赋值是否未使用，最难的部分在于控制流汇合。

工作方式如下：

- AST 会被遍历两次：一次收集信息，一次真正删除
- 在信息收集阶段，我们维护一个映射，把每个赋值语句标记为 `unused`、`undecided` 或 `used`
- 当访问一个赋值时，它先进入 `undecided` 状态（`for` 循环的说明见下文），并把同一个变量的其他未决定赋值改成 `unused`
- 当某个变量被引用时，所有仍处于 `undecided` 的赋值都会变成 `used`

在控制流分裂时，映射会复制到各个分支；在控制流汇合时，两个映射按如下方式合并：

- 只存在于一个映射中的条目，或状态相同的条目，原样保留
- 冲突状态按下面规则解决：
  - `unused`, `undecided` -> `undecided`
  - `unused`, `used` -> `used`
  - `undecided`, `used` -> `used`

对于 `for` 循环，condition、body 和 post 部分会被访问两次，并考虑 condition 处的控制流汇合。也就是说，我们模拟三条路径：0 次循环、1 次循环、2 次循环，然后在最后合并。

为什么模拟三次以上没有必要？因为一个赋值状态在一次迭代开始时，会确定性地变成迭代结束时的状态。把这个状态映射函数记作 `f`，而三种状态 `unused`、`undecided`、`used` 对应于 `0`、`1`、`2`，合并操作就是 `max`。

真正应该计算的是：

```text
max(s, f(s), f(f(s)), f(f(f(s))), ...)
```

由于 `f` 只有三种输出，反复迭代最多三次就会进入循环，因此：

```text
max(s, f(s), f(f(s))) = max(s, f(s), f(f(s)), f(f(f(s))), ...)
```

所以循环最多跑两次就够了。

对于带 default 分支的 `switch`，不存在跳过 `switch` 的控制流路径。

当变量离开作用域时，所有仍处于 `undecided` 的赋值都会变成 `unused`，除非该变量是函数返回参数，此时状态会变成 `used`。

第二次遍历时，所有处于 `unused` 状态的赋值都会被删除。

这一步通常紧跟 SSA transform 运行，用来把伪 SSA 完成掉。

## 工具

### Movability

Movability 是表达式的一个属性。大致意思是：该表达式没有副作用，而且其求值只依赖于变量值和环境的 call-constant 状态。

大多数表达式都是 movable。以下几类会让表达式不可移动：

- 函数调用
- 有副作用的 opcode，例如 `call` 或 `selfdestruct`
- 读写 memory、storage 或外部状态信息的 opcode
- 依赖当前 PC、memory size 或 returndata size 的 opcode

### DataflowAnalyzer

DataflowAnalyzer 不是一个优化步骤本身，而是供其他组件使用的工具。

它在遍历 AST 时，会追踪每个变量的当前值，只要这个值是 movable 表达式。它会记录每个变量当前所赋值表达式里包含了哪些变量。

每当变量 `a` 被赋值时，`a` 的当前存储值会更新；如果某个变量 `b` 的当前表达式里包含了 `a`，那么 `b` 的所有已存储值都会被清空。

在控制流汇合处，如果一个变量在任何控制流路径中被赋值过，或可能被赋值，则会清除关于它的知识。例如，进入 `for` 循环时，所有会在 body 或 post block 中被赋值的变量都会被清空。

## 表达式级简化

### CommonSubexpressionEliminator

这个步骤使用 DataflowAnalyzer，把那些在语法上与变量当前值匹配的子表达式，替换成对该变量的引用。由于这些子表达式必须是 movable，所以这是等价变换。

所有本身就是标识符的子表达式，如果它们的当前值也是标识符，就会被替换成它们的当前值。

这两条规则结合起来，可以做局部值编号（local value numbering）。也就是说，如果两个变量的值相同，其中一个最终一定不会被使用。随后 `UnusedPruner` 或 `UnusedAssignEliminator` 就能把这些变量彻底删掉。

如果先运行 `ExpressionSplitter`，这个步骤会更高效。若代码已经是伪 SSA，变量的值能保持更久，因此表达式也更容易被替换。

如果先运行了 `CommonSubexpressionEliminator`，`ExpressionSimplifier` 往往也能做出更好的替换。

### ExpressionSimplifier

ExpressionSimplifier 使用 DataflowAnalyzer，并利用一组表达式等价变换，例如 `X + 0 -> X`，来简化代码。

它会尝试在每个子表达式上匹配模式，比如 `X + 0`。在匹配过程中，它会把变量解析成当前赋值表达式，这样即使在伪 SSA 形式下，也能匹配更深层的模式。

像 `X - X -> 0` 这样的模式，只有在 `X` 是 movable 时才能应用，因为否则会丢掉潜在副作用。由于变量引用永远是 movable，即使它当前对应的值不是 movable，`ExpressionSimplifier` 在 split form 或伪 SSA form 中依然更强大。

### LiteralRematerialiser

待补充。

### LoadResolver

LoadResolver 会把 `sload(x)` 和 `mload(x)` 这类表达式替换成当前已知存储在 storage / memory 里的值。

如果代码已经是 SSA form，这一步效果最好。

前置条件：Disambiguator、ForLoopInitRewriter。

## 语句级简化

### CircularReferencesPruner

移除那些彼此互相调用、但既没有被外部引用，也没有被最外层上下文引用的函数。

### ConditionalSimplifier

ConditionalSimplifier 会根据控制流为条件变量插入赋值，从而破坏 SSA form。

当前这个工具还很有限，主要是因为我们还没有很好地支持布尔类型。由于条件只检查表达式是否非零，因此无法分配一个精确值。

目前支持的特性：

- `switch` 分支：插入 `<condition> := <caseLabel>`
- 在带终止控制流的 `if` 之后插入 `<condition> := 0`

未来计划：

- 允许替换为 `1`
- 把用户自定义函数的终止行为考虑进来

如果先运行 dead code removal，并且代码已经是 SSA form，这一步效果最好。

前置条件：Disambiguator。

### ConditionalUnsimplifier

ConditionalSimplifier 的反向操作。

### ControlFlowSimplifier

这个步骤会简化多种控制流结构：

- 把空 body 的 `if` 替换成 `pop(condition)`
- 删除空的 default `switch` case
- 如果不存在 default case，删除空的 `switch` case
- 把没有 case 的 `switch` 替换成 `pop(expression)`
- 把只有一个 case 的 `switch` 变成 `if`
- 把只有 default case 的 `switch` 替换成 `pop(expression)` 加其 body
- 把常量表达式 `switch` 替换成匹配的 case body
- 把带终止控制流、且没有其他 `break` / `continue` 的 `for` 替换成 `if`
- 删除函数末尾的 `leave`

这些操作都不依赖数据流。`StructuralSimplifier` 会做一些类似但依赖数据流的任务。

ControlFlowSimplifier 在遍历过程中会记录 `break` 和 `continue` 的存在与否。

前置条件：Disambiguator、FunctionHoister、ForLoopInitRewriter。

重要：它会引入 EVM opcode，所以目前只能用于 EVM 代码。

### DeadCodeEliminator

这个步骤删除不可达代码。

不可达代码是指位于如下语句之后的 block 内容：

- `leave`
- `return`
- `invalid`
- `break`
- `continue`
- `selfdestruct`
- `revert`
- 或者递归无限的用户定义函数调用

函数定义会保留，因为它们可能被更早的代码调用，因此仍然视为可达。

由于 `for` 循环 init block 中声明的变量作用域会扩展到循环体，所以必须先运行 ForLoopInitRewriter。

前置条件：ForLoopInitRewriter、FunctionHoister、FunctionGrouper。

### EqualStoreEliminator

这个步骤会删除 `mstore(k, v)` 和 `sstore(k, v)` 调用，如果之前已经有相同的 `mstore(k, v)` / `sstore(k, v)`，中间没有其他 store，且 `k` 和 `v` 的值都没有变化。

如果在 SSATransform 和 CommonSubexpressionEliminator 之后运行，这个步骤效果很好，因为 SSA 会保证变量不会变化，而 CSE 会在值已知相同的时候复用完全相同的变量。

前置条件：Disambiguator、ForLoopInitRewriter。

### UnusedPruner

这个步骤会删除所有未被引用的函数定义。

它也会删除未被引用的变量声明。如果某个声明的赋值表达式不是 movable，则表达式会保留，但其结果会被丢弃。

所有 movable 的表达式语句（没有赋值的表达式）都会被删除。

### StructuralSimplifier

这是一个更通用的步骤，会在结构层面做多种简化：

- 把空 body 的 `if` 变成 `pop(condition)`
- 把条件恒真的 `if` 替换为其 body
- 删除条件恒假的 `if`
- 把只有一个 case 的 `switch` 变成 `if`
- 把只有 default case 的 `switch` 变成 `pop(expression)` 和 body
- 把字面量表达式的 `switch` 替换为匹配的 case body
- 把条件恒假的 `for` 替换成其初始化部分

这个组件使用 DataflowAnalyzer。

### BlockFlattener

这个步骤会消除嵌套 block：把内层 block 的语句插入到外层 block 的合适位置。

它依赖 FunctionGrouper，并且不会扁平化最外层 block，以保持 FunctionGrouper 产生的形式。

例如：

```yul
{
    {
        let x := 2
        {
            let y := 3
            mstore(x, y)
        }
    }
}
```

会变成：

```yul
{
    {
        let x := 2
        let y := 3
        mstore(x, y)
    }
}
```

只要代码已经完成 disambiguation，这不会有问题，因为变量作用域只会变大。

### LoopInvariantCodeMotion

这个优化会把可移动的 SSA 变量声明移到循环外。

只考虑循环 body 或 post block 顶层的语句，也就是说，条件分支内部的变量声明不会被移出循环。

如果先运行 ExpressionSplitter 和 SSATransform，效果会更好。

前置条件：Disambiguator、ForLoopInitRewriter、FunctionHoister。

## 函数级优化

### FunctionSpecializer

这个步骤会根据字面量参数对函数进行特化。

如果函数 `f(a, b) { sstore(a, b) }` 以字面量参数调用，例如 `f(x, 5)`，其中 `x` 是标识符，那么可以生成一个只接收一个参数的新函数 `f_1`：

```yul
function f_1(a_1) {
    let b_1 := 5
    sstore(a_1, b_1)
}
```

后续优化步骤就能对这个函数做更多简化。这个步骤主要对不会被内联的函数有帮助。

前置条件：Disambiguator、FunctionHoister。

建议把 LiteralRematerialiser 也作为前置步骤，虽然不是正确性所必需。

### UnusedFunctionParameterPruner

这个步骤会删除函数中未使用的参数。

如果某些参数没用，比如 `function f(a,b,c) -> x, y { x := div(a,b) }` 里的 `c` 和 `y`，就会删除这些参数，并创建一个新的“linking”函数：

```yul
function f(a,b) -> x { x := div(a,b) }
function f2(a,b,c) -> x, y { x := f(a,b) }
```

然后把所有对 `f` 的引用替换成 `f2`。

之后应该运行 inliner，确保所有对 `f2` 的引用都被替换回 `f`。

前置条件：Disambiguator、FunctionHoister、LiteralRematerialiser。

LiteralRematerialiser 不是正确性所必需，但它可以处理一些例子，比如 `function f(x) -> y { revert(y, y) }`，其中字面量 `y` 会被替换成它的值 `0`，从而让函数可以重写。

### UnusedStoreEliminator

这个优化组件会删除冗余的 `sstore` 和 memory store 语句。

对于 `sstore`，如果所有外部流出的路径都会 revert（显式 `revert()`、`invalid()` 或无限递归），或者会进入另一个 `sstore`，且优化器能判断后者会覆盖前者，那么这个语句就会被删除。

不过，如果初始 `sstore` 和 revert / 覆盖它的 `sstore` 之间存在读操作，这条语句就不会被删除。这些读操作包括：

- 外部调用
- 任何带 storage 访问的用户定义函数
- 对一个无法证明与初始 `sstore` 不同的 slot 的 `sload`

例如：

```yul
{
    let c := calldataload(0)
    sstore(c, 1)
    if c {
        sstore(c, 2)
    }
    sstore(c, 3)
}
```

会在运行 UnusedStoreEliminator 后变成：

```yul
{
    let c := calldataload(0)
    if c { }
    sstore(c, 3)
}
```

对于 memory store，处理通常简单一些，至少在最外层 Yul block 中如此：如果在任何代码路径上都不会读到这些写入，它们都会被删除。

但在函数分析层面，方法类似于 `sstore`，因为当离开函数作用域后，我们不知道 memory 位置是否会被读取，所以只有当所有代码路径都会导致 memory overwrite 时，语句才会被删除。

最适合在 SSA form 下运行。

前置条件：Disambiguator、ForLoopInitRewriter。

### EquivalentFunctionCombiner

如果两个函数在语法上等价，允许变量重命名，但不允许任何重排序，那么对其中一个函数的所有引用都会被替换成另一个。

真正删除函数由 UnusedPruner 完成。

## 函数内联

### ExpressionInliner

这个组件执行受限的函数内联，只会内联那些可以嵌入函数式表达式中的函数，也就是满足以下条件的函数：

- 只返回一个值
- 函数体形式类似 `r := <functional expression>`
- 右侧不引用自己，也不引用 `r`

此外，对所有参数，都必须满足：

- 传入参数是 movable
- 该参数在函数体中引用次数少于 2 次，或者这个参数本身很便宜（cost 最多为 1，例如小于等于 `0xff` 的常量）

也就是说，如果函数形如 `function f(...) -> r { r := E }`，其中 `E` 不引用 `r`，而且调用参数都是 movable 表达式，那么它就能被内联。

内联后的结果总是一个单独表达式。

这个组件只能用于标识符唯一的源代码。

### FullInliner

FullInliner 会把某些函数调用直接替换成函数体。

在大多数情况下这不太划算，因为它只是增加代码体积，并不会带来明显收益。代码通常很贵，我们通常更希望代码更短，而不是更高效。不过在某些情况下，函数内联会对后续优化步骤有正面作用，比如某个参数是常量时。

在内联时，会用启发式方法判断函数调用是否应该被内联。当前启发式不会把调用内联到“大”函数里，除非被调用函数非常小。只使用一次的函数会被内联，中等大小的函数也会被内联，而带常量参数的调用则允许稍大一些的函数。

未来可能会加入回溯组件：它不会立刻内联函数，而是只做特化，也就是生成一个副本，并把某个参数始终替换为常量。之后我们可以对这个特化后的函数再次运行优化器。如果收益很大，就保留这个特化版本，否则仍使用原始函数。

FunctionHoister 和 ExpressionSplitter 建议作为前置步骤，因为它们能让这一步更高效，但不是正确性所必需。特别是，如果函数参数里还嵌套着函数调用，FullInliner 不会内联；而先运行 ExpressionSplitter 可以保证输入里没有这种嵌套调用。

## 清理

cleanup 在优化器最后执行。它会尽量把拆开的表达式重新合并成更深层的嵌套形式，同时也会尽可能减少变量数量，让 stack machine 更容易编译。

### ExpressionJoiner

这是 ExpressionSplitter 的反向操作。它会把那些恰好只有一次引用的变量声明，合并回复杂表达式。

这个阶段会完整保留函数调用和 opcode 执行顺序。它不会使用 opcode 的交换律信息；如果把变量的值移动到使用位置会改变任何函数调用或 opcode 执行顺序，那么就不会进行这种变换。

注意，这个组件不会移动赋值语句的被赋值值，也不会移动被引用超过一次的变量。

例如，`let x := add(0, 2) let y := mul(x, mload(2))` 不会被变换，因为这会交换 `add` 和 `mload` 的执行顺序，尽管 `add` 本身是 movable 的。

在重排 opcode 时，变量引用和字面量会被忽略。因为这样，`let x := add(0, 2) let y := mul(x, 3)` 会变成 `let y := mul(add(0, 2), 3)`，即便 `add` 的求值会发生在字面量 `3` 之后。

### SSAReverser

这是一个很小的步骤，用来在与 CommonSubexpressionEliminator 和 UnusedPruner 组合时，逆转 SSATransform 的效果。

我们生成的 SSA 形式对代码生成不太友好，因为它会产生很多局部变量。更好的方式是复用已有变量，通过赋值而不是新声明。

SSATransform 会把：

```yul
let a := calldataload(0)
mstore(a, 1)
```

改写成：

```yul
let a_1 := calldataload(0)
let a := a_1
mstore(a_1, 1)
let a_2 := calldataload(0x20)
a := a_2
```

问题在于，凡是引用 `a` 的地方，实际使用的是 `a_1`。SSATransform 会通过交换声明和赋值来改写这类语句。上面的片段会变成：

```yul
let a := calldataload(0)
let a_1 := a
mstore(a_1, 1)
a := calldataload(0x20)
let a_2 := a
```

这只是一个很简单的等价变换。但如果随后运行 CommonSubexpressionEliminator，它会把所有 `a_1` 的出现替换成 `a`，直到 `a` 被重新赋值。然后 UnusedPruner 会把 `a_1` 整个删除，从而完全逆转 SSATransform。

### StackCompressor

EVM 代码生成的一个难点是：表达式栈向下访问的硬限制只有 16 个槽位。某种意义上，这就限制了局部变量数量上限为 16。

StackCompressor 会把 Yul 代码编译成 EVM bytecode。每当栈差值太大时，它会记录问题发生在哪个函数里。

对于每个导致此问题的函数，Rematerialiser 会被调用，并带有一个特殊请求：尽量激进地消除按其值成本排序的特定变量。

如果失败，这个过程会重复多次。

### Rematerialiser

Rematerialisation 阶段会尝试用某个变量最近一次赋值的表达式替换该变量引用。

只有当这个表达式的求值成本相对较低时，这才有收益。并且只有当从赋值点到使用点之间，这个表达式的值没有改变时，语义上才等价。

这个阶段的主要好处是：如果它能让某个变量被完全消除，就能节省 stack slot；另外，如果表达式非常便宜，还可能省掉一个 EVM `DUP` 指令。

Rematerialiser 使用 DataflowAnalyzer 跟踪变量的当前值，这些值总是 movable。

如果值很便宜，或者变量被显式请求消除，那么这个变量引用就会被替换成它的当前值。

### ForLoopConditionOutOfBody

这是 ForLoopConditionIntoBody 的反向变换。

对于任意 movable 的 `c`，它会把：

```none
for { ... } 1 { ... } {
if iszero(c) { break }
...
}
```

变成：

```none
for { ... } c { ... } {
...
}
```

并且会把：

```none
for { ... } 1 { ... } {
if c { break }
...
}
```

变成：

```none
for { ... } iszero(c) { ... } {
...
}
```

在这个步骤之前，应该先运行 LiteralRematerialiser。

## 基于代码生成的优化器

当前，代码生成期优化器提供两种优化。

第一种在 legacy code generator 中可用，它会把字面量移到交换律二元运算符的右侧，从而更好地利用结合律。

第二种在 IR-based code generator 中可用，它会在生成某些惯用 `for` 循环的计数器递增代码时，允许使用 unchecked arithmetic。这样可以通过识别保证计数器不会溢出的条件，避免浪费 gas，也不需要在循环体里塞一个冗长的 unchecked arithmetic 块。

### Unchecked Loop Increment

这个 overflow check 优化在 Solidity `0.8.22` 中引入，目标是识别哪些条件下 `for` 循环计数器可以安全递增而无需溢出检查。

该优化只适用于如下通用形式的 `for` 循环：

```solidity
for (uint i = X; i < Y; ++i) {
    // loop body 里不会修改变量 i
}
```

条件以及计数器只会递增这两个事实，保证它不会溢出。适用该优化的精确要求如下：

- 循环条件必须是 `i < Y` 这种比较，其中 `i` 是本地计数器变量（下文称为 loop counter），`Y` 是某个表达式
- 条件里必须使用内建运算符 `<`，且只有 `<` 会触发这个优化。`<=` 之类的不会触发。用户自定义运算符也不符合条件
- 循环表达式必须是计数器的前缀或后缀自增，即 `i++` 或 `++i`
- 计数器必须是内建整数类型的局部变量
- 计数器不能被循环体或条件表达式修改
- 比较必须在与计数器相同的类型上进行，也就是说，右侧表达式的类型必须可以隐式转换成计数器类型，从而比较之前不会把左侧隐式扩宽

为了说明最后一条，考虑下面这个例子：

```solidity
for (uint8 i = 0; i < uint16(1000); i++) {
    // ...
}
```

这里计数器 `i` 会在比较前从 `uint8` 隐式转换成 `uint16`，而条件实际上永远为真，所以不能移除递增时的溢出检查。
