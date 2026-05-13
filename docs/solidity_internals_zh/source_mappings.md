# Source Mappings

作为 AST 输出的一部分，编译器会提供 AST 中每个节点对应的源代码范围。这个信息可以用于很多场景，例如基于 AST 报错的静态分析工具，或者高亮局部变量及其使用位置的调试工具。

此外，编译器还可以生成一份从 bytecode 到源代码范围的映射。这对于在 bytecode 级别工作的静态分析工具、在调试器中显示当前源代码位置、或者处理断点都很重要。这个映射还包含其他信息，例如跳转类型和 modifier depth，下面会说明。

这两类 source mapping 都使用整数 ID 来引用源文件。源文件的 ID 存在于标准 JSON 编译接口解析后的 `output['sources'][sourceName]['id']` 中。

对于一些辅助例程，编译器会生成“内部”源文件，它们不属于原始输入，但会出现在 source mapping 里。这些源文件及其 ID 可以通过 `output['contracts'][sourceName][contractName]['evm']['bytecode']['generatedSources']` 获取。

> **注意**
>
> 对于不属于任何特定源文件的指令，source mapping 会分配一个 `-1` 的整数 ID。这种情况可能出现在由编译器生成的 inline assembly 语句对应的 bytecode 段里。

AST 内部的 source mapping 使用如下格式：

```text
s:l:f
```

其中：

- `s` 是源文件中范围起点的字节偏移
- `l` 是源代码范围长度，单位是字节
- `f` 是上面提到的源文件索引

bytecode 的 source mapping 编码更复杂。它是由 `;` 分隔的一串 `s:l:f:j:m`。每个元素对应一条指令，也就是说不能使用字节偏移，而要使用指令偏移（`push` 指令长度不止 1 字节）。

字段含义如下：

- `s`、`l`、`f` 与上面相同
- `j` 可以是 `i`、`o` 或 `-`，表示跳转指令是进入函数、从函数返回，还是普通跳转（例如循环的一部分）
- `m` 是一个整数，表示“modifier depth”。进入 modifier 中的占位语句（`_`）时深度加 1，离开时减 1。这样调试器就能跟踪一些棘手情况，比如同一个 modifier 被使用两次，或者在一个 modifier 中出现多个占位语句

为了压缩这些 source mapping，尤其是 bytecode 的映射，使用了如下规则：

- 如果某个字段为空，则复用前一个元素对应字段的值
- 如果缺少 `:`，则后面的所有字段都视为空

因此下面两种 source mapping 表达的是同样的信息：

```text
1:2:1;1:9:1;2:1:2;2:1:2;2:1:2
```

```text
1:2:1;:9;2:1:2;;
```

需要特别注意的是，当使用内建的 [verbatim](https://docs.soliditylang.org/en/latest/yul.html#verbatim) 时，source mapping 会失效：因为这个内建会被视为一条指令，而不是潜在的多条指令。
