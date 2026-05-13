# Calldata 的布局

函数调用的输入数据默认采用 [ABI 规范](https://docs.soliditylang.org/en/latest/abi-spec.html) 定义的格式。ABI 规范要求参数按 32 字节对齐填充。内部函数调用则使用不同的约定。

合约构造函数的参数会直接附加在合约代码末尾，同样采用 ABI 编码。构造函数会通过硬编码偏移量读取这些参数，而不是使用 `codesize` 指令，因为一旦在代码后面追加数据，`codesize` 的值也会变化。
