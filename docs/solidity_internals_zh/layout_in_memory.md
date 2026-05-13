# Memory 的布局

Solidity 预留了四个 32 字节槽位，具体字节区间如下：

- `0x00` - `0x3f`（64 字节）：用于哈希等操作的临时 scratch 区
- `0x40` - `0x5f`（32 字节）：当前已分配内存大小，也就是 free memory pointer
- `0x60` - `0x7f`（32 字节）：zero slot

scratch 区可以在语句之间使用，也就是在 inline assembly 中可以复用。zero slot 被用作动态内存数组的初始值，永远不应该写入。free memory pointer 初始指向 `0x80`。

Solidity 总是把新对象放到 free memory pointer 指向的位置，而且 memory 从不释放。未来这点可能会改变。

Solidity 中 memory 数组的元素总是占用 32 字节的倍数。这一点对 `bytes1[]` 也成立，但对 `bytes` 和 `string` 不成立。多维 memory 数组本质上是指向 memory 数组的指针。动态数组的长度存放在数组的第一个槽位，后面紧跟数组元素。

> **警告**
>
> 有些 Solidity 操作需要超过 64 字节的临时 memory，因此无法放进 scratch 区。这些临时数据会放在 free memory pointer 所指的位置，但由于它们生命周期很短，指针不会更新。memory 可能会被清零，也可能不会。也正因为如此，不能假设 free memory 指向的是一个已经清零的区域。
>
> 直接使用 `msize` 找到一块肯定清零的 memory 看起来似乎不错，但如果在没有更新 free memory pointer 的情况下把这块地址长期拿来用，可能会产生意外结果。

## 与 Storage 布局的差异

如上所述，memory 的布局和 [storage](layout_in_storage.md) 不同。下面是一些例子。

### 数组布局差异

下面的数组在 storage 中只占 32 字节（1 个 slot），但在 memory 中占 128 字节（4 个 32 字节元素）。

```solidity
uint8[4] a;
```

### Struct 布局差异

下面的 struct 在 storage 中占 96 字节（3 个 32 字节 slot），但在 memory 中占 128 字节（4 个 32 字节元素）。

```solidity
struct S {
    uint a;
    uint b;
    uint8 c;
    uint8 d;
}
```
