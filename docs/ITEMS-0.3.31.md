# 0.3.31：消耗同步边界与撤回 alpha 实验

目标：Minecraft Bedrock **1.26.51.01 / LeviLamina 26.51.7**。
代码检查点：`45422e7`（按要求先双端/测试、提交并部署；其包版本暂仍为0.3.30）。
正式版本随后统一为0.3.31。未启动游戏代替用户验收。

## 1. 已验收与此前误读

- 用户验收0.3.30 `67f9be9`：生存部分拾取、创造满包吸取并丢弃溢出、箱子/漏斗/拖拽拆堆正确。
  热栏/背包/箱子/漏斗图标持续显示，重开界面及拖拽预览正确。最新提供的历史日志为`2026-10-27.9.log`。
- **0.3.29“满包站地面弹药旁换弹同步已修复”从未成立，是此前报告误读。**
  一槽60发，消耗4→56，服务端拾取4→60，但客户端保持56。重进/交互才恢复；服务器数量和地面扣量正确。
- 对照原版弓箭立即补满；最新“不发送”实验也失败：60发，开火30后换弹，背包客户端仍60而服务器30。
  因此不能把普通拾取通过、桥接上限正确、编译通过当作消耗同步验收。
- 用户确认cyber_armorer相关槽图**原图背景就是不透明**。原来的“黑透明”并非已证实混合错误。

## 2. 本机只读反汇编与 SDK 证据

以下RVA只用于调查，新增hook仍使用SDK符号和`static_assert`，没有按RVA安装。

| 入口 | 本机证据 | 结论边界 |
| --- | --- | --- |
| `FillingContainer::removeItem` `0x25a4930` | 调用setItem/clearSlot，随后setContainerChanged（`0x26af200`）通知listeners | 通知不等于立即发包 |
| InventoryMenu主虚表`0xe827a70` | contentChanged为`0x76bc0`，该函数直接ret；removeSlot在`0x25a5db0`转发container.removeItem | 不能依赖此回调完成同步 |
| `BaseContainerMenu::broadcastChanges` `0x25a1c30` | `0x25a1fa1`比较count，`0x25a1fb7`比较stack，随后检查isSlotDirty；`0x25a2027`调用slotChanged | 容器会比较快照，但不保证每个menu都会发送 |
| 同函数提交基线 | `0x25a2037`复制ItemStackBase，`0x25a205b`复制net-ID variant | 基线是完整ItemStack，不是仅count；NBT/ID必须随值复制 |
| `ServerPlayer::slotChanged` `0x291b480` | `0x291b4a0–0x291b4b3`比较menu是否为`this+0xd30`，**自己InventoryMenu或result slot直接跳过**；其他容器走InventorySlotPacket | “只调用own menu broadcast就会发送”同样错误 |
| `Inventory::setItem` `0x1d5150` | 转发setItemWithForceBalance，默认`false` | 普通更改不是强制平衡的服务器消耗边界 |
| `Inventory::setItemWithForceBalance` `0x1d5170` | 保留原生stack-net-manager分支，回调/旧路径进入FillingContainer版本`0x25a4ae0` | 不自行伪造/重置net ID |
| `Player::inventoryChanged` `0x20ba30` | `0x20bb42`调用`InventoryTransactionManager::addAction`（`0x266ff70`），传入forceBalanced | 除menu快照外还有独立事务跟踪 |
| `addAction`的forceBalanced分支 | `0x267027c–0x267028c`：不平衡且forceBalanced时调用原生平衡函数；平衡后走发送/清理分支 | 消耗不能一直作为普通未平衡action与后续变化合并 |
| `0x1d5c40` | 遍历可tick物品、调用Item的inventoryTick后条件发送 | **不是**普通库存差分函数，不能据此推断removeItem会自动发送 |

确认的问题是**自定义服务器消耗/发包没有作为完整原生同步边界提交**：旧全刷只改变客户端，
不发送则客户端连扣量也收不到；原生另有自己的menu快照、net-manager和事务平衡机制。
`mLastSlots`过时是需要修正的一致性缺口，但**并未证明它单独导致本次拾取缺包**，尤其own menu的slotChanged本来就被抑制。
同样未证明“客户端拒绝stale net ID”或“网络包乱序”，不把这些猜测写成根因。

采用的原生写入链为：

`Inventory::setItemWithForceBalance(slot, remaining, true)` → 原生net-manager/库存更新 →
`Player::inventoryChanged` / 原生事务平衡路径；随后目标槽`InventorySlotPacket` → 同槽完整基线提交。

保留Java TACZ按AmmoId逐槽提取的语义，而不调用只按基础物品类型匹配的removeResource去误扣其他AmmoId。
未宣称已完整反汇编证明Bow/所有Crossbow实现都走同一入口；加入只读`RangedWeaponItem::releaseUsing`对照诊断，
供下一轮原版弓箭实测比较forceBalanced、包型及数量。最终原生执行分支和客户端应用结果仍待新日志确认。

## 3. 库存写入审计与实现

| 位置 | 处理 |
| --- | --- |
| `AmmoItem.cpp::extractAmmo` | 复制原栈，只扣需要量，调用原生setItemWithForceBalance(true)，立即发送该槽并提交该槽基线；含扣空槽。去掉全刷，不靠tick纠正 |
| `AmmoItem.cpp::insertAmmo` / give | 相同原生平衡写入和目标槽同步；原有兼容NBT、先合并后空槽、split、脚边溢出不变 |
| `GunItem.cpp::writeSelectedGun` | 保留用户实测有效的setSelectedItem及**单槽**InventorySlotPacket；仅补对应基线提交，不把弹药等其他槽的中间值发出去 |
| `PackItem.cpp::giveGun` | 不再使用addAndRefresh（会全刷但只记录新枪槽不够）；改用原生Inventory::add，只有由空变非空的目标槽发送并提交；满包仍原生drop，不截获通用add |
| 换弹/取消/退弹 | Shooter的脚本供弹调用统一extractAmmo，枪状态写回统一writeSelectedGun；当前ScriptApi没有独立“向背包退弹”写入器，putAmmoInMagazine返回溢出数不等于实现背包退款；不擅自新加退款玩法 |
| 普通拾取、箱子shift、漏斗、drag/split | 不改变任何原生容量/插入/预测/创造策略；take hook只在诊断开启时观察，不纠正物品/actor/数量 |

`recordSentInventorySlot`使用完整`ItemStack::operator=`，与原生broadcast的base+net-ID复制语义一致；
测试覆盖count、NBT、ID及“未发送的其他槽不能被顺手提交”。未来原生真实变化（例如丢枪1→0、补弹56→60）仍能被比较到。
未初始化/越界基线不扩大、不调用全容器broadcast初始化，保留原生建表；诊断写`baseline=not-initialized`。

**线程**：所有实际写入调用来自服务器命令、服务器玩法/level tick及服务器包处理；没有异步投递或渲染线程写基线。
helper要求服务器线程，先检查当前server level、非client player，再用`ServerPlayer::tryGetFromEntity`核实，并核对menu指向该inventory。
客户端收包诊断只读取客户端本地库存，不触及ServerPlayer/mLastSlots。没有缓存menu/stack/actor指针跨tick或跨世界。
不添加“给原生共享字段上私有mutex”这种无法同步原生访问的伪线程安全措施；将来若引入异步调用必须先调度到服务器线程。

## 4. 限量诊断：默认关闭

`/tacz rdebug ammosync 1`开启并重置预算，`0`关闭；再次`1`重新取样。
共享原子开关用于单人整合客户端；每类最多256条（server语义/client应用/pickup/outgoing packet各自预算），预算耗尽不继续刷屏。
独立BDS进程开关不会自动同步远端客户端，远端诊断控制不在本版单人范围内。

查`[ammo-sync]`，按slot对齐：

- `path=InventoryTransactionManager::addAction`：before/after、forceBalanced、pendingBefore/After。
- `path=explicit-InventorySlotPacket+baseline reason=extract|give-ammo|gun-nbt|give-gun/native-add`：槽位数、baselineBefore/After。
- `path=sendNetworkPacket/InventorySlotPacket`：实际发送入口、slot、sent；也记录Content每槽或Transaction包发送。
- `path=Player::take/native reason=pickup`：同槽before/after、总量及baselineAfter；不再访问可能被移除的地面actor。
- `client path=InventorySlotPacket/handle` / `InventoryContentPacket/handle`：本地before、payload received、本地after。
- `client path=InventoryTransactionPacket/handle` 与`/applied`：收到事务及实际槽位变化。
  若原生处理延迟，handler返回时未变不等于最终拒绝，必须结合后续原生包/界面观察；不据一条received日志宣称已应用。
- 原版弓箭对照：`path=RangedWeaponItem::releaseUsing/native`、同一调用范围的`reason=vanilla-ranged`。

hook全部使用26.51.7声明及签名断言；收包的`std::shared_ptr<InventoryTransactionPacket>`是完整标准库类型，
没有对SDK opaque by-value占位类型下hook。关闭时观察hook只检查开关并origin，不解析NBT、扫描库存或分配快照。
无world时只origin；take/vanilla-ranged预算耗尽也不再扫描。日志不代替实机断言。

## 5. alpha 撤回范围

删除临时`/tacz rdebug icon`、slotIconMode、PackSlotPreRender、PackSlotMaterial、sprite参数分支、SlotIconMaterial常量/测试、
iconAlpha采样/辅助/日志/测试；AlphaUsage还原到setRawImage之前，统一恢复`ui_textured_and_glcolor`，与原tooltip路径一致。
**保留**continuous dirty、每次render绘制、owner/world/base/aux验证、clone重绑、cursor/flying、Default batch、PackSlotPasses=1及native fallback。
没改任何枪包PNG、已验收UV/HK逻辑或手持/地面3D模型；真不透明素材不应该被“修透明”。

## 6. 用户验收清单

1. 开`ammosync 1`；背包仅一槽60发且**附近没有地面弹药**。枪开火30并换弹，背包应立即30，不需丢出/点击/重进。
2. **原始复现**：生存满包，一槽60发7.62x39，旁有地面弹药，先确认无法拾取；开火4并换弹。
   同槽应60→56→60（画面可以直接呈最终60），地面恰少4；日志必须有消耗发送/基线56、原生pickup56→60和客户端60应用证据。
3. 扣空/跨两槽/不足量换弹、取消/切枪；按既有Java TACZ语义核对实际消耗，不凭假设增加退款。
4. 连续开火，枪NBT弹量实时变化；Q丢枪、拾回、重进后枪/背包/地面总量一致。
5. 生存部分拾取余量留地上（无声保持原版）；创造满包吸入并丢弃溢出。
6. `/tacz give ammo`合并/新槽/多组/满包掉脚边，`/tacz give gun`空槽/满包；紧接着拾取/丢出/shift移动不滞留。
7. 箱子双向shift（含53+60）、漏斗、拖拽/拆分/旧TaczAutoName兼容回归。
8. 热栏/背包/箱子/漏斗反复开关、cursor/flying图；正常透明PNG透出背景，原本不透明的cyber_armorer保持原样；主菜单、退出/换世界安全。
9. 重新`ammosync 1`做原版弓箭对照；结束`ammosync 0`确认不再输出；日志预算不够时重新1，而不是提高无限日志上限。

**当前状态**：纯测试验证槽位提交契约/预算及现有玩法模型，不执行游戏内原生事务、包应用或像素验收。
无新实机结果前，ROADMAP保持相关同步项🟡，不重复“上一版本已修复”的误报。

## 7. 最终离线验证与部署

- server/client双端Release成功；`xmake build tacz-tests`、`xmake run tacz-tests`：**198项／920,378条断言全部通过**。
- `git diff --check`通过；xmake/tooth/BP/RP/版本测试同步0.3.31，版本文件无UTF-8 BOM；BP依赖RP版本也纳入测试。
- 游戏关闭时执行`scripts/deploy-client.sh`，正式部署**0.3.31/client**。
- 本地产物与部署DLL/PDB/manifest及BP/RP全文件树哈希一致；未修改已安装枪包。
- 最终DLL SHA256：`d735ae3986e37e500a374a936fbc5c3ce34408545bd45ec8653f5b645de6ec76`。
- 本轮没有启动游戏；forceBalanced实际分支、消耗→原生pickup→客户端应用闭环、丢枪及alpha撤回后的像素仍待上述实机清单。
