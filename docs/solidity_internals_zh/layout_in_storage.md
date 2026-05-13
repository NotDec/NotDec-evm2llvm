# Storage 与 Transient Storage 中的状态变量布局

> 说明
>
> 本节描述的规则同时适用于 `storage` 和 `transient storage` 两种数据位置。两者的布局彼此独立，互不干扰。因此，`storage` 和 `transient storage` 的状态变量可以安全交错声明，不会产生副作用。`transient storage` 只支持 value types。（适合放“简单标量”，不适合放结构化容器。）

合约的状态变量会以紧凑方式存入 storage，也就是说，多个值有时会共享同一个 storage slot。

除动态大小数组和 mapping 之外，数据会按照状态变量的声明顺序连续存放，从 `slot 0` 开始。每个变量都会根据其类型确定占用的字节数。多个连续的、占用不足 32 字节的值类型会在可能的情况下被打包进同一个 storage slot，规则如下：

- slot 中第一个项按低位对齐存放
- value type 只使用存储所需的字节数
- 如果某个 value type 放不下当前 slot 的剩余空间，就会放到下一个 slot
- struct 和数组数据总是从新 slot 开始，并且其内部成员按这些规则紧密打包
- struct 或数组数据后面的项也总是从新 slot 开始

对于使用继承的合约，状态变量的顺序由 C3 线性化后的合约顺序决定，且从最基底的合约开始。如果符合上面的打包规则，不同合约中的状态变量可以共享同一个 storage slot。

struct 和数组的元素会像独立值一样按顺序存放。

如果合约指定了 [自定义 storage layout](https://docs.soliditylang.org/en/latest/internals/layout_in_storage.html#custom-storage-layout)，则分配给静态 storage 变量的 slot 会按照布局基准值整体平移。动态数组和 mapping 的位置也会间接受影响，因为它们依赖的静态 slot 被平移了。自定义布局在最派生合约中指定，并且会按照上面的顺序，从最基底合约开始，对所有 storage slot 做调整。

下面这个例子中，合约 `C` 继承自 `A` 和 `B`，同时指定了一个自定义的 storage base slot。结果是继承树中所有 storage 变量的 slot 都会按照 `C` 指定的值进行平移。

```solidity
// SPDX-License-Identifier: GPL-3.0
pragma solidity ^0.8.29;

struct S {
    int32 x;
    bool y;
}

contract A {
    uint a;
    uint128 transient b;
    uint constant c = 10;
    uint immutable d = 12;
}

contract B {
    uint8[] e;
    mapping(uint => S) f;
    uint16 g;
    uint16 h;
    bytes16 transient i;
    S s;
    int8 k;
}

contract C is A, B layout at 42 {
    bytes21 l;
    uint8[10] m;
    bytes5[8] n;
    bytes5 o;
}
```

在这个例子里，storage 布局从继承而来的状态变量 `a` 开始，它直接存放在 base slot `42` 中。`transient`、`constant` 和 `immutable` 变量分别存放在其他位置，因此 `b`、`i`、`c` 和 `d` 都不会影响 storage 布局。

接下来是动态数组 `e` 和 mapping `f`。它们各自都会保留一个完整的 slot，这个 slot 的地址将被用来 [计算](https://docs.soliditylang.org/en/latest/internals/layout_in_storage.html#mappings-and-dynamic-arrays) 真正存放数据的位置。这个 slot 不能与其他变量共享，因为最终生成的地址必须唯一。

接下来的两个变量 `g` 和 `h` 各占 2 字节，可以一起打包到 `slot 45` 中，偏移分别是 `0` 和 `2`。

由于 `s` 是一个 struct，它的两个成员会连续打包，每个成员占 5 字节。即使它们理论上仍然能放进 `slot 45`，struct 和数组也总是从新 slot 开始。因此 `s` 放在 `slot 46`，下一个变量 `k` 放在 `slot 47`。

另一方面，基类可以和派生类共享 slot，所以 `l` 不需要新开一个 slot。

然后变量 `m` 是一个包含 10 个元素的数组，占用 10 字节，放进 `slot 48`。
`n` 也是数组，但由于其元素大小的关系，无法把第一个 slot 完美填满，于是会溢出到下一个 slot。
最后，变量 `o` 会落在 `slot 51`，即使它和 `n` 的元素类型相同。正如前面所说，struct 和数组后面的变量总是从新 slot 开始。

合起来看，`C` 的 storage 和 transient storage 布局可以表示如下：

- Storage：

  ```text
  42 [aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]
  43 [eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee]
  44 [ffffffffffffffffffffffffffffffff]
  45 [                            hhgg]
  46 [                           yxxxx]
  47 [          lllllllllllllllllllllk]
  48 [                      mmmmmmmmmm]
  49 [  nnnnnnnnnnnnnnnnnnnnnnnnnnnnnn]
  50 [                      nnnnnnnnnn]
  51 [                           ooooo]
  ```

- Transient storage：

  ```text
  00 [iiiiiiiiiiiiiiiibbbbbbbbbbbbbbbb]
  ```

注意，storage 说明符只会在 `C` 的继承层次结构中影响 `A` 和 `B`。如果单独部署它们，storage 会从 `0` 开始：

- `A` 的 storage 布局：

  ```text
  00 [aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]
  ```

- `B` 的 storage 布局：

  ```text
  00 [eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee]
  01 [ffffffffffffffffffffffffffffffff]
  02 [                            hhgg]
  03 [                           yxxxx]
  04 [                               k]
  ```

> **警告**
>
> 当使用小于 32 字节的元素时，合约的 gas 消耗可能更高。这是因为 EVM 以 32 字节为单位操作。因此，如果元素小于 32 字节，EVM 必须执行更多操作，才能把 32 字节的值裁剪成目标大小。
>
> 如果你处理的是 storage 值，使用较小位宽的类型可能有收益，因为编译器会把多个元素打包进同一个 storage slot，从而把多个读写合并成一次操作。
>
> 但如果你并不是在同一时间读写一个 slot 里的所有值，这种做法也可能适得其反：当一个值写入多值 storage slot 时，必须先把整个 slot 读出来，再和新值组合，避免破坏同一个 slot 里的其他数据。
>
> 对于函数参数或 memory 值，没有这种内在收益，因为编译器不会把这些值打包。
>
> 最后，为了让 EVM 更容易优化，建议尽量安排 storage 变量和 `struct` 成员的顺序，使它们可以紧密打包。例如，把 storage 变量按 `uint128, uint128, uint256` 的顺序声明，比按 `uint128, uint256, uint128` 更好，前者只占 2 个 storage slot，而后者会占 3 个。

> **注意**
>
> state variable 的 storage 布局被视为 Solidity 外部接口的一部分，因为 storage pointer 可以传给库。这意味着，这一节里描述的规则如果发生变化，就会成为语言的 breaking change。鉴于其重要性，任何这类改动都必须非常谨慎地推进。若真的要做这种 breaking change，我们希望提供一个兼容模式，让编译器生成支持旧布局的 bytecode。

## Mappings 和动态数组

由于大小不可预测，mapping 和动态数组类型不能夹在前后状态变量之间存放。
相反，从上述规则的角度看，它们只占用 32 字节；它们包含的元素则会从通过 Keccak-256 哈希计算出的另一个 storage slot 开始存放。

假设 mapping 或数组经过上面的 storage layout 规则后最终落在 `slot p`。对于动态数组，这个 slot 存放数组元素个数（byte array 和 string 是例外，见后文）；对于 mapping，这个 slot 保持为空，但它仍然是必须的，因为即使两个 mapping 紧挨着，它们的内容也必须落在不同的 storage 位置。

数组数据从 `keccak256(p)` 开始，布局方式和静态大小数组相同：一个元素接一个元素，如果元素不超过 16 字节，则可以共享 storage slot。动态数组中的动态数组会递归地应用这一规则。对于类型为 `uint24[][]` 的 `x`，元素 `x[i][j]` 的位置计算如下（仍然假设 `x` 本身存放在 slot `p`）：

```text
slot = keccak256(keccak256(p) + i) + floor(j / floor(256 / 24))
```

并且可以通过如下方式从 slot 数据 `v` 中取出元素：

```text
(v >> ((j % floor(256 / 24)) * 24)) & type(uint24).max
```

mapping key `k` 对应的值位于：

```text
keccak256(h(k) . p)
```

其中 `.` 表示拼接，`h` 会根据 key 的类型进行处理：

- 对于 value type，`h` 会像把值存入 memory 时那样，把值补齐到 32 字节
- 对于 string 和 byte array，`h(k)` 只是原始未填充的数据

如果 mapping 的 value 不是 value type，那么计算出的 slot 只是数据起点。若 value 是 struct 类型，还需要加上对应 struct 成员的偏移才能访问具体成员。

例如，考虑下面这个合约：

```solidity
// SPDX-License-Identifier: GPL-3.0
pragma solidity >=0.4.0 <0.9.0;

contract C {
    struct S { uint16 a; uint16 b; uint256 c; }
    uint x;
    mapping(uint => mapping(uint => S)) data;
}
```

现在计算 `data[4][9].c` 的 storage 位置。mapping 本身的位置是 `1`，因为前面的变量 `x` 占用了 32 字节。也就是说，`data[4]` 存放在 `keccak256(uint256(4) . uint256(1))`。`data[4]` 的类型仍然是 mapping，而 `data[4][9]` 的数据起点位于：

```text
keccak256(uint256(9) . keccak256(uint256(4) . uint256(1)))
```

struct `S` 中成员 `c` 的 slot 偏移是 `1`，因为 `a` 和 `b` 被打包进了同一个 slot。所以 `data[4][9].c` 的 slot 是：

```text
keccak256(uint256(9) . keccak256(uint256(4) . uint256(1))) + 1
```

该值的类型是 `uint256`，因此它只占一个 slot。

## `bytes` 和 `string`

`bytes` 和 `string` 的编码方式完全相同。

通常来说，它们的编码类似于 `bytes1[]`：有一个数组本身的 slot，以及一个通过对该 slot 位置做 `keccak256` 得到的数据区。
不过对于短值（少于 32 字节），数组元素会和长度一起存放在同一个 slot 中。

具体来说，如果数据长度最多为 31 字节，元素会存放在高位字节中（左对齐），最低位字节存储 `length * 2`。如果 byte array 的数据长度为 32 字节或更多，则主 slot `p` 会存储 `length * 2 + 1`，数据则像普通数组一样存放在 `keccak256(p)`。因此只要检查最低位是否为 1，就能区分短数组和长数组：短数组不置位，长数组置位。

> **注意**
>
> 当前还不支持处理非法编码的 slot，但未来可能会加入。如果你通过 IR 编译，读取一个非法编码的 slot 会触发 `Panic(0x22)` 错误。

## JSON 输出

合约的 storage（或 transient storage）布局可以通过 [标准 JSON 接口](https://docs.soliditylang.org/en/latest/using-the-compiler.html#compiler-input-and-output-json-description) 请求得到。输出是一个 JSON 对象，包含两个键：`storage` 和 `types`。

`storage` 是一个数组，每个元素的格式如下：

```json
{
    "astId": 2,
    "contract": "fileA:A",
    "label": "x",
    "offset": 0,
    "slot": "0",
    "type": "t_uint256"
}
```

上面的例子是源单元 `fileA` 中 `contract A { uint x; }` 的 storage layout。字段含义如下：

- `astId`：状态变量声明对应的 AST 节点 ID
- `contract`：合约名，包含路径前缀
- `label`：状态变量名
- `offset`：根据编码方式计算出的 storage slot 内字节偏移
- `slot`：状态变量所在或起始的 storage slot。这个数字可能非常大，因此 JSON 中用字符串表示
- `type`：作为键使用的类型 ID，用来引用变量的类型信息

给定的 `type`，这里是 `t_uint256`，表示 `types` 里的一个条目，其格式如下：

```json
{
    "encoding": "inplace",
    "label": "uint256",
    "numberOfBytes": "32"
}
```

其中：

- `encoding` 表示数据在 storage 中的编码方式，可取值如下：
  - `inplace`：数据连续地存放在 storage 中
  - `mapping`：基于 Keccak-256 哈希的方式
  - `dynamic_array`：基于 Keccak-256 哈希的方式
  - `bytes`：根据数据大小决定是单 slot 还是基于 Keccak-256 哈希
- `label`：规范化的类型名
- `numberOfBytes`：使用的字节数，十进制字符串
  - 如果 `numberOfBytes > 32`，表示使用了多个 slot

某些类型除了上面这四项之外还会包含额外信息。mapping 还会包含 `key` 和 `value` 类型（同样是这个 types 映射中的条目），数组会包含 `base` 类型，struct 会列出其 `members`，格式与顶层的 `storage` 相同。

> **注意**
>
> 合约 storage layout 的 JSON 输出目前仍然被视为实验性功能，未来可能在不破坏兼容性的版本中发生变化。

下面的例子展示了一个合约及其 storage / transient storage 布局，包含 value type 和 reference type，既有打包编码，也有嵌套类型。

```solidity
// SPDX-License-Identifier: GPL-3.0
pragma solidity ^0.8.28;
contract A {
    struct S {
        uint128 a;
        uint128 b;
        uint[2] staticArray;
        uint[] dynArray;
    }

    uint x;
    uint transient y;
    uint w;
    uint transient z;

    S s;
    address addr;
    address transient taddr;
    mapping(uint => mapping(address => bool)) map;
    uint[] array;
    string s1;
    bytes b1;
}
```

### Storage Layout

```json
{
  "storage": [
    {
      "astId": 15,
      "contract": "fileA:A",
      "label": "x",
      "offset": 0,
      "slot": "0",
      "type": "t_uint256"
    },
    {
      "astId": 19,
      "contract": "fileA:A",
      "label": "w",
      "offset": 0,
      "slot": "1",
      "type": "t_uint256"
    },
    {
      "astId": 24,
      "contract": "fileA:A",
      "label": "s",
      "offset": 0,
      "slot": "2",
      "type": "t_struct(S)13_storage"
    },
    {
      "astId": 26,
      "contract": "fileA:A",
      "label": "addr",
      "offset": 0,
      "slot": "6",
      "type": "t_address"
    },
    {
      "astId": 34,
      "contract": "fileA:A",
      "label": "map",
      "offset": 0,
      "slot": "7",
      "type": "t_mapping(t_uint256,t_mapping(t_address,t_bool))"
    },
    {
      "astId": 37,
      "contract": "fileA:A",
      "label": "array",
      "offset": 0,
      "slot": "8",
      "type": "t_array(t_uint256)dyn_storage"
    },
    {
      "astId": 39,
      "contract": "fileA:A",
      "label": "s1",
      "offset": 0,
      "slot": "9",
      "type": "t_string_storage"
    },
    {
      "astId": 41,
      "contract": "fileA:A",
      "label": "b1",
      "offset": 0,
      "slot": "10",
      "type": "t_bytes_storage"
    }
  ],
  "types": {
    "t_address": {
      "encoding": "inplace",
      "label": "address",
      "numberOfBytes": "20"
    },
    "t_array(t_uint256)2_storage": {
      "base": "t_uint256",
      "encoding": "inplace",
      "label": "uint256[2]",
      "numberOfBytes": "64"
    },
    "t_array(t_uint256)dyn_storage": {
      "base": "t_uint256",
      "encoding": "dynamic_array",
      "label": "uint256[]",
      "numberOfBytes": "32"
    },
    "t_bool": {
      "encoding": "inplace",
      "label": "bool",
      "numberOfBytes": "1"
    },
    "t_bytes_storage": {
      "encoding": "bytes",
      "label": "bytes",
      "numberOfBytes": "32"
    },
    "t_mapping(t_address,t_bool)": {
      "encoding": "mapping",
      "key": "t_address",
      "label": "mapping(address => bool)",
      "numberOfBytes": "32",
      "value": "t_bool"
    },
    "t_mapping(t_uint256,t_mapping(t_address,t_bool))": {
      "encoding": "mapping",
      "key": "t_uint256",
      "label": "mapping(uint256 => mapping(address => bool))",
      "numberOfBytes": "32",
      "value": "t_mapping(t_address,t_bool)"
    },
    "t_string_storage": {
      "encoding": "bytes",
      "label": "string",
      "numberOfBytes": "32"
    },
    "t_struct(S)13_storage": {
      "encoding": "inplace",
      "label": "struct A.S",
      "members": [
        {
          "astId": 3,
          "contract": "fileA:A",
          "label": "a",
          "offset": 0,
          "slot": "0",
          "type": "t_uint128"
        },
        {
          "astId": 5,
          "contract": "fileA:A",
          "label": "b",
          "offset": 16,
          "slot": "0",
          "type": "t_uint128"
        },
        {
          "astId": 9,
          "contract": "fileA:A",
          "label": "staticArray",
          "offset": 0,
          "slot": "1",
          "type": "t_array(t_uint256)2_storage"
        },
        {
          "astId": 12,
          "contract": "fileA:A",
          "label": "dynArray",
          "offset": 0,
          "slot": "3",
          "type": "t_array(t_uint256)dyn_storage"
        }
      ],
      "numberOfBytes": "128"
    },
    "t_uint128": {
      "encoding": "inplace",
      "label": "uint128",
      "numberOfBytes": "16"
    },
    "t_uint256": {
      "encoding": "inplace",
      "label": "uint256",
      "numberOfBytes": "32"
    }
  }
}
```

### Transient Storage Layout

```json
{
  "storage": [
    {
      "astId": 17,
      "contract": "fileA:A",
      "label": "y",
      "offset": 0,
      "slot": "0",
      "type": "t_uint256"
    },
    {
      "astId": 21,
      "contract": "fileA:A",
      "label": "z",
      "offset": 0,
      "slot": "1",
      "type": "t_uint256"
    },
    {
      "astId": 28,
      "contract": "fileA:A",
      "label": "taddr",
      "offset": 0,
      "slot": "2",
      "type": "t_address"
    }
  ],
  "types": {
    "t_address": {
      "encoding": "inplace",
      "label": "address",
      "numberOfBytes": "20"
    },
    "t_uint256": {
      "encoding": "inplace",
      "label": "uint256",
      "numberOfBytes": "32"
    }
  }
}
```
