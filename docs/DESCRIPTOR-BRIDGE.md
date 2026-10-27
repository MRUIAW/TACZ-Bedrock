# 描述符桥接：在基岩版上按 NBT 决定物品堆叠上限

本文说明 TACZ-Bedrock 在 LeviLamina 客户端/服务端上使用的"描述符桥接"（descriptor bridge）。
它让**同一个物品 ID 的物品，按 NBT 拥有各自不同的最大堆叠数**，并且让游戏原版的拾取、合并、
容器转移、漏斗、联机同步、客户端预测等全部路径都使用同一个正确的上限。

本文写给其他 LeviLamina 模组开发者：如果你的模组也是"一个物品 + NBT 区分种类"，并且不同种类
需要不同的堆叠上限，可以直接参考这里的方法。文中的地址与偏移均来自 Minecraft Bedrock
**1.26.51.01 / LeviLamina 26.51.7**，换版本必须重新核对。

实现代码：`src/nbt/StackLimitProbe.cpp`、`src/pack/DescriptorProbe.{h,cpp}`、
单元测试 `tests/DescriptorProbeTests.cpp`。测量与验收记录：`docs/ITEMS-0.3.25.md`、
`docs/ITEMS-0.3.28.md`、`CHANGELOG.md` 0.3.28 / 0.3.29。

---

## 1. 能做到什么

- 一个物品 ID（例如 `tacz:ammo`）在行为包里只注册一次，堆叠上限按每个物品栈的 NBT 决定，
  例如 `AmmoId=tacz:762x39` → 60，`AmmoId=tacz:12g` → 36，`AmmoId=tacz:308` → 48。
- 游戏原版逻辑**自己**使用正确的上限，不需要为每一条路径单独写补丁：
  - 生存模式拾取掉落物，包括背包只能装下一部分时把余量正确留在地上；
  - 背包/箱子之间 shift+点击、拖拽、右键拆分、合并；
  - 漏斗、投掷器等容器之间的自动转移；
  - 服务端物品请求（ItemStackRequest）校验与客户端预测，两端看到的数量一致；
  - `/give`、`Inventory::add` 等插入逻辑。
- 新增或修改一种"种类"只需要改数据（枪包里的 `stack_size`），**不需要注册新物品**，
  因此可以在运行时热重载，这一点和 Java 版 Forge 的做法一致。
- 查不到对应关系时安全退回原版返回值，不会把一种物品的上限错用到另一种物品上。

## 2. 要解决的问题

### 2.1 Java 版为什么简单

Java 版 TACZ 的弹药只有一个物品 `tacz:ammo`，靠 NBT `AmmoId` 区分口径。它重写
`AmmoItem.getMaxStackSize(ItemStack stack)`：读出 AmmoId，返回枪包里的 `stack_size`。

这能工作，是因为 **Forge 把原版所有读取堆叠上限的地方都收拢到 `ItemStack.getMaxStackSize()`，
而它会把完整的 `ItemStack`（含 NBT）交给 `Item.getMaxStackSize(ItemStack)`**。拾取、合并、
容器、漏斗、网络同步都经过这一个入口，所以只改一处就全部正确。

### 2.2 基岩版的统一入口拿不到 NBT

基岩版也有一个统一入口，是 `Item` 上的虚函数：

```cpp
virtual uchar Item::getMaxStackSize(ItemDescriptor const&) const;   // mc/world/item/Item.h
```

它的参数 `ItemDescriptor` 只描述"什么物品"，**不带 NBT**：

```cpp
class ItemDescriptor {                       // 16 字节
    std::unique_ptr<ItemDescriptor::BaseDescriptor> mImpl;   // 实现对象
};
struct InternalItemDescriptor : ItemDescriptor::BaseDescriptor {   // 24 字节
    ItemDescriptor::ItemEntry mItemEntry;    // { Item const* mItem; short mAuxValue; }
};
```

反汇编 `ItemStackBase::getDescriptor()` 可以确认：生成 descriptor 时只把 `Item*` 和 aux 传给
`InternalItemDescriptor` 的构造，`mUserData`（NBT）没有传进去。

外层还有一个 `uchar ItemStackBase::getMaxStackSize() const`，它拿得到完整栈，但游戏里大量代码
**并不调用它**，而是自己调用 `getDescriptor()`，再通过 Item 的虚表调用内层入口。已经反汇编确认的
例子（1.26.51.01，调用返回地址 RVA）：

| 原生函数 | 读取上限的位置 |
| --- | --- |
| `ItemStackBase::set` | `0x24870f1` |
| `ItemStackBase::isFullStack` | `0x2488c01` |
| `ItemStackBase::isStackable` | `0x2488ce8` |
| `FillingContainer::_getSlotWithRemainingSpace` | `0x25a4835` |
| `FillingContainer::_addResource`（源/目标） | `0x25a3ebb` / `0x25a4051` |
| `ItemStackBase::getMaxStackSize` 本身 | `0x247de54` |
| 物品请求/容器转移相关 | `0x7c98b20`、`0x7c993dc`、`0x7c9a5e4`、`0x2060bd7`、`0x206194e`、`0x2061ca8`、`0x4601fc8`、`0x997035f`、`0x998e523`、`0x998e69e` |

### 2.3 只 hook 外层会出什么问题

如果只 hook 外层 `ItemStackBase::getMaxStackSize()`，一部分路径按 NBT 得到 60，另一部分路径
读到行为包里写的 64。同一次操作里两个数字混用，就会出现这些现象（都在本项目实测过）：

- 箱子里已有 53 发，从背包 shift 移入一组 60 发 → 箱子 60+49、背包剩 4 发（64 − 60 = 4）；
- 背包里已有同种物品时，从箱子 shift 移回背包失败；
- 漏斗下方容器某格已满 60 后，漏斗仍认为这一格"还能放 4 个"，卡住不再往新格子放；
- 背包满时站在掉落物上消耗物品，客户端显示的数量先降后跳回 60，要等下一次同步才恢复；
- 部分拾取时插入数量和"是否全部拾取"的判断用了不同的上限，地上物品没有扣减或被删除。

逐个补这些路径的补丁会越补越多，而且永远无法证明已经补全。

### 2.4 直接 hook 内层为什么也不够

内层入口只拿到 `Item*` + aux。所有种类都是同一个 `Item`、aux 都是 0，
单靠参数**分不出是哪一种**，所以需要把"这个 descriptor 是从哪个物品栈生成的"这一信息
从生成处带到读取处——这就是"桥接"。

## 3. 原理

1. **生产处登记**：hook `ItemDescriptor ItemStackBase::getDescriptor() const`。原函数返回后，
   如果这个栈是我们的物品、NBT 合法，就把"这个 descriptor 的身份"映射到"它对应的种类和上限"。
2. **消费处查表**：hook 内层 `Item::$getMaxStackSize(ItemDescriptor const&)`（以及
   `ComponentItem::$getMaxStackSize`，见 §5.3）。如果 `this` 是我们的物品，就用 descriptor 的
   身份查表；查到有效记录就返回记录里的上限，查不到就返回原版值。

descriptor 的身份用 **`mImpl` 指向的实现对象地址 + 当前线程 ID** 表示：

- `ItemDescriptor` 本身经常按值返回、移动，包装对象的地址会变；但移动 `unique_ptr` 不会改变
  `mImpl` 指向的对象，所以同一个 descriptor 在移动前后键不变。
- 加上线程 ID，是因为客户端线程和（单人模式的）内置服务端线程各自独立生成和读取 descriptor，
  同一时刻两个线程上的地址复用不能互相串号。

缓存里保存的指针**只作为整数键比较**，从不解引用，也不持有任何原生对象的引用。

## 4. 实现

### 4.1 数据结构（纯逻辑，可单元测试）

`src/pack/DescriptorProbe.h`：

```cpp
struct DescriptorRecord {
    uintptr_t item=0;      // 生成时 descriptor 指向的 Item 地址（指纹，只比较不解引用）
    int aux=0;             // 生成时的 aux（指纹）
    int limit=0;           // 该种类的堆叠上限
    std::string ammo;      // 种类 ID，便于日志
    uint64_t bornMs=0;     // 生成时间
    uint64_t producer=0;   // 生产调用者地址（诊断）
    bool copied=false;     // 是否经复制传播而来
};
class DescriptorBridge {
    using Key=std::pair<uint64_t,uintptr_t>;   // {线程 ID, mImpl 地址}
    std::map<Key,DescriptorRecord> entries;
public:
    static constexpr uint64_t LifetimeMs=250;  // 记录有效期
    static constexpr size_t Capacity=4096;     // 满了直接清空，不猜
    void erase(uint64_t thread,uintptr_t impl);
    void put(uint64_t thread,uintptr_t impl,DescriptorRecord record);
    Lookup lookup(uint64_t thread,uintptr_t impl,uintptr_t item,int aux,uint64_t now) const;
    void clear();
};
```

`lookup` 只有同时满足以下条件才返回 `Candidate`：

- 键 `{线程, mImpl}` 存在；
- 记录未过期（距生成 ≤ 250 ms）；
- 记录里的 `Item*` 和 aux 与本次传入的 descriptor 一致（防止地址被另一个 descriptor 复用）。

否则返回 `Missing` / `Expired` / `FingerprintMismatch`，调用方退回原版值。

### 4.2 Hook 列表

全部使用当前 SDK 的成员签名，并用 `static_assert` 在编译期核对，避免签名不一致导致的崩溃：

```cpp
static_assert(sizeof(ItemDescriptor)==16 && sizeof(InternalItemDescriptor)==24);
static_assert(std::is_same_v<decltype(&ItemStackBase::getDescriptor),ItemDescriptor (ItemStackBase::*)() const>);
using Max=uchar (Item::*)(ItemDescriptor const&) const;
static_assert(std::is_same_v<decltype(&Item::$getMaxStackSize),Max>);
```

| Hook | 作用 |
| --- | --- |
| `ItemStackBase::getDescriptor` | 生产处：调用原函数，若是我们的物品且 NBT 合法，`put({线程, mImpl}, 记录)`；非我们的物品则 `erase` 该键，清掉可能残留的旧记录 |
| `Item::$getMaxStackSize(ItemDescriptor const&)` | 消费处：先调原函数得到原版值；若是我们的物品且查到 `Candidate`，返回记录的上限 |
| `ComponentItem::$getMaxStackSize(ItemDescriptor const&)` | 同上。数据驱动物品走这个重写；若与 `Item::$getMaxStackSize` 折叠为同一地址则只装一次 |
| `ItemDescriptor::ItemDescriptor(Item const&, int)`、`ItemDescriptor(std::string_view, int)` | 直接构造的 descriptor 没有 NBT 来源：构造后 `erase` 该键，防止复用旧地址误命中 |
| `InternalItemDescriptor::$clone` | descriptor 被**复制**时会 clone 出新的实现对象，地址改变：把原记录传播到新地址，并保留原生成时间（复制不能续命） |
| `ItemDescriptor::operator=(ItemDescriptor const&)` | 赋值复制同理传播 |

消费处的核心逻辑：

```cpp
LL_TYPE_INSTANCE_HOOK(ProbeItemLimit,ll::memory::HookPriority::Normal,Item,&Item::$getMaxStackSize,uchar,
    ItemDescriptor const& descriptor) {
    auto native=origin(descriptor);
    int limit=0;
    if (session && !inspecting) try { limit=observe(*this,descriptor,native,caller,0,token); } catch (...) { ++errors; }
    return limit>0 ? static_cast<uchar>(limit) : native;
}

int observe(Item const& item,ItemDescriptor const& descriptor,int native,...) {
    if (!knownItem(reinterpret_cast<uintptr_t>(&item))) return 0;   // 只处理我们的物品
    auto actual=descriptor.getItem();
    auto hit=lookup(uintptr_t(descriptor.mImpl.get()),uintptr_t(actual),descriptor.getAuxValue(),token);
    if (actual!=&item) hit={Resolution::FingerprintMismatch,{}};
    return hit.status==Resolution::Candidate && hit.record ? hit.record->limit : 0;
}
```

### 4.3 只对自己的物品下手

`Item::$getMaxStackSize` 是所有物品共用的虚函数，而且像这样很小的函数在编译后可能和其他类的同形函数
**折叠成同一段机器码**——hook 里的 `this` 和参数可能根本不是 `Item`/`ItemDescriptor`。因此：

- 进入世界后，先用一个真实的临时栈 `ItemStack stack; stack.reinit("tacz:ammo",1,0);` 取得我们物品的
  `Item*`，或从第一次见到的真实完整栈登记它（最多 8 个地址，兼顾客户端与内置服务端各自的注册表）；
- hook 里**先做整数比较** `this == 已登记的 Item 地址`，不相等就直接返回原版值，不读任何字段、
  不解析参数。

### 4.4 生命周期与线程

- 只在世界会话内工作：进入世界后的 level tick 开启，退出世界、`ServerStoppingEvent`、模组 disable 时
  清空全部缓存；不跨世界复用指针键。主菜单下 hook 只转发原函数。
- 一把互斥锁保护缓存；递归保护 `thread_local bool inspecting`，防止我们自己在 hook 内调用
  `getDescriptor()` 时再次登记。
- 每次 hook 调用都包在 `try/catch` 里，异常只计数，不影响原版返回值。

### 4.5 开关与日志

- `constexpr bool EnforceLimit` 控制是否真正改返回值；为 `false` 时是纯测量模式，只统计不改行为。
- 每秒输出 `[stack-probe] summary`，退出世界输出 `total` 与 `world-exit`，按"调用者 RVA + 线程 + 路线"
  汇总：调用次数、命中/未命中数、原版值直方图、桥接值直方图、种类直方图、未命中原因。

## 5. 推荐的接入流程：先测量，再生效

### 5.1 测量

先以 `EnforceLimit=false` 运行，在游戏里把所有相关操作做一遍（拾取、部分拾取、箱子 shift 双向、
拖拽、拆分、漏斗、消耗物品、生存/创造），然后看日志：

```
[stack-probe] total world=7 caller=0x24870f1 nativeImage=true thread=56696 route=Item
  calls=939 resolvedCandidate=939 unresolved=0 different=939
  native=[64=939] would=[60=939] ammo=[tacz:762x39=939] resolution=[candidate=939]
```

本项目 0.3.28 的实测（`2026-10-27.6.log`）：**20 个调用点/线程组合，unresolved=0，errors=0**，
原版值全部是 64，桥接值全部是 60。也就是说，在这个游戏版本里，读取我们物品上限的所有内层调用
都能追溯到一次 `getDescriptor()`。

如果某个调用点出现 `unresolved>0`，`resolution` 会告诉你原因：

- `missing`：这个 descriptor 不是由 `getDescriptor()` 生成的（例如从网络、JSON、配方构造），
  需要找到它的生产处；
- `expired`：生成到读取超过 250 ms，可能被长时间缓存，考虑延长有效期或在缓存处补登记；
- `fingerprint-mismatch`：地址被复用成了另一个物品/aux，说明有未观测到的构造或销毁路径。

### 5.2 生效

全部命中后改为 `EnforceLimit=true`。本项目 0.3.29 生效后实测：箱子 shift 双向、漏斗、背包满时
站在掉落物上消耗物品、部分拾取，全部恢复正常，日志显示 `native=[60=..]`（外层也已经得到 60）。

### 5.3 `ComponentItem` 路线

数据驱动物品（行为包定义的物品）是 `ComponentItem`，它重写了 `getMaxStackSize`。测量时有一组调用的
返回地址不在游戏映像内（`nativeImage=false`），属于 `ComponentItem` 路线，同样 100% 命中。
两条路线都要 hook；如果两者地址折叠为同一个函数，安装一次即可（日志 `folded=true`）。

## 6. 已知限制与注意事项

- **候选而非证明**：键是"线程 + 地址 + 指纹 + 250 ms"。未被 hook 到的构造/销毁路径、同线程内在
  有效期内的地址复用、跨线程复制、被内联的复制，理论上都可能造成误命中或漏命中。指纹（Item*+aux）
  只能防止"复用成别的物品"，不能区分"同一物品的两个不同栈"。在本项目的物品上测量结果是 0 漏、0 错，
  换物品或换版本必须重新测量。
- **直接读字段**：如果游戏某处不调用虚函数，而是直接读 `Item::mMaxStackSize`（偏移 `+0xa8`），
  桥接管不到。目前只确认了虚函数本身在读这个字段；测量日志里完全不出现的路径，不能据此断言它不读上限。
- **上限范围**：基岩版堆叠数是 `uchar` 且原版限制在 1..64；大于 64 的配置按 64 处理。
- **性能**：每次 `getDescriptor()` 都会经过 hook，我们的物品会加锁写一次 map。本项目里每个世界会话
  有数十万次 descriptor 生产，开销可接受；若要更省，可把统计日志去掉，或把 map 换成无锁小表。
- **与旧补丁的冲突**：桥接生效后，之前为单条路径写的补丁（例如自定义 `Inventory::canAdd`）可能变成
  多余甚至有害。本项目 0.3.29 实测：自定义 `canAdd` 覆盖了原版"创造模式下放不下也返回 true"的语义，
  导致创造模式背包满时不再吸取掉落物。桥接生效后应逐个移除这类补丁，让原版逻辑自己判断。
- **版本绑定**：文中所有 RVA、偏移、类大小均为 1.26.51.01 / LeviLamina 26.51.7。升级后至少要
  重新核对 `ItemDescriptor` / `InternalItemDescriptor` 的大小、相关成员签名，并重新跑一遍测量。
- **不要照抄旧版 SDK 签名**：例如 26.20.7 的头文件里有 `ItemDescriptor` 的复制/移动构造声明，
  26.51.7 里没有；只能使用当前 SDK 中存在、且能通过 `static_assert` 的声明。

## 7. 与其他方案的对比

| 方案 | 做法 | 优点 | 缺点 |
| --- | --- | --- | --- |
| 只 hook 外层 `ItemStackBase::getMaxStackSize()` | 按 NBT 返回 | 简单 | 大量内层调用绕过它，路径之间上限不一致，bug 补不完 |
| 逐条路径打补丁 | hook `add`、`canAdd`、拾取等 | 每个点可控 | 永远无法证明补全；补丁之间、补丁与原版语义之间容易冲突 |
| 每个种类一个物品 ID | 行为包里每种写 `max_stack_size` | 原版天然正确，最稳 | 物品 ID 不再统一；新增种类必须注册新物品，无法热重载 |
| 每个种类分配固定 aux | 内层按 aux 返回上限 | 内层能直接区分；aux 还参与堆叠判断 | 需要保证 aux 在存档、联机、give、创造栏等路径中不被重置；需要持久化 aux↔种类映射 |
| **描述符桥接（本方案）** | 生产处登记、消费处查表 | 保持单物品 + NBT；原版所有经过内层入口的路径自动正确；支持热重载；查不到时退回原版 | 依赖"身份键"的可靠性，需要先测量；绑定游戏版本 |

## 8. 复用清单

1. 确认物品是数据驱动物品（`ComponentItem`）还是自定义 `Item` 子类，找到对应的 `$getMaxStackSize`。
2. 写好 §4.1 的纯逻辑表，并为它写单元测试（键、过期、指纹、复制传播、容量）。
3. 安装 §4.2 的 hook，全部加 `static_assert`；先 `EnforceLimit=false`。
4. 进入世界后登记自己物品的 `Item*`（§4.3），hook 里先比较地址。
5. 在游戏里做完 §5.1 的操作，确认所有调用点 `unresolved=0`、`errors=0`。
6. 改为 `EnforceLimit=true`，回归测试同一套操作。
7. 删除生效后变得多余的旧补丁，回归测试创造模式与生存模式。
8. 每次升级游戏/LeviLamina 版本，重复第 5 步。
