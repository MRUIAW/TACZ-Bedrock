# 0.3.32：枪械 NBT 未平衡事务阻塞原生拾取

目标仍为 **Minecraft 1.26.51.01 / LeviLamina 26.51.7**。代码检查点`d33e51d`：双端Release和200项测试通过后立即提交，再补正式版本/文档。未启动游戏冒充验收。

## 1. 重新核对0.3.31实测

日志：`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs/2026-10-28.3.log`。

用户确认无地面弹药时“背包60，开火30，换弹后立即30”通过；地面补弹仍按奇偶轮交替滞留/恢复，服务器与地面数量始终正确。
本次按slot5过滤弹药路径，但**保留了决定性的一条slot2枪NBT写入**；它不是可全部忽略的噪声。

| 时间 / 原日志行 | 事实 |
| --- | --- |
| 02:03:54.255 / 2669–2672 | extract60→35，forceBalanced=true，发送InventoryTransactionPacket和slot5绝对35；基线35 |
| 同时 / **2673–2675** | **gun slot2，1→1，forceBalanced=false，pendingBefore=false→pendingAfter=true**；只发送枪槽包并提交快照，没结束原生事务 |
| 02:03:54.259 / 2677–2682 | 客户端事务应用slot5：60→35，绝对槽包仍35；枪槽包也收到 |
| 02:03:54.275 / 2683–2688 | 原生拾取35→59（24发），59→60（1发），对应地面source动作24→0/1→0；事务一直pending=true |
| 同时 / 2689 | own InventoryMenu的slot5被比较出35→60，但slotChanged ownMenuSuppressed=true，无发送 |
| 02:03:59.555 / 2844–2848 | 第二轮服务器60→41并发送；又出现枪NBT普通1→1 pending |
| 02:03:59.562 / 2852–2854 | 客户端实际35→41，不是服务器事务的60→41；客户端原生事务未平衡 |
| 02:03:59.573 / 2858–2861 | 服务器原生拾取41→60，仍pending；own menu依旧不发 |
| 02:03:59.585–.588 / 2862–2934 | 随后的原生InventoryContent全刷，slot5 sent/received/after均60，形成“第二次恢复” |

旧action诊断单凭isClientSide打印`server`在整合游戏中有歧义；以上收包侧动作由相邻client handle/applied确认，不能按那一个前缀归属。
0.3.32改用实际`ServerPlayerComponent`核实动作端别，并补非零物品组诊断。

**更正：mLastSlots不是这次拾取漏同步的根因。** 0.3.31已经把发送值35提交进快照，原生后来仍比较到60；own menu本来就不负责发包。
“提交快照就能恢复原生拾取”的假设被本日志否定。没有stale-net-ID拒包/乱序证据。

## 2. 原版拾取正常如何同步：本机只读证据

没有使用下列RVA安装hook；所有观察hook仍是26.51.7 SDK符号＋签名断言。

1. **`Player::take`，RVA `0x1f6c60`**：
   - `0x1f741e–0x1f742c`调用原生Inventory::add；库存变更先加入入包action。
   - 完整拾取在`0x1f743a–0x1f7492`，部分拾取在`0x1f750b–0x1f7589`建立world(source=2)移除action并`addAction(...,false)`。
     部分分支的world数量是`原数量−剩余数量`，正好抵消入包数量，不需要我们拆actor/修数量。
   - `0x1f760c–0x1f761b`：有余量时转返回false/清理；**不经过**后面的TakeItemActorPacket发送路径。
   - 完全拾取的`0x1f7886–0x1f78d9`发actor拿取包，虚表`0xe887ce0`的getId在`0x3547aa0`返回`0x11`，即TakeItemActor。
     它服务完整拾取表现；不能用它解释部分拾取数量同步。
2. **`InventoryTransaction::addAction`，`0x266f0a0`**：
   `0x266f65a–0x266f686`将旧栈计为负数、新栈计为正数，加入content groups。
3. **`InventoryTransaction::addItemToContent`，`0x266f7d0`**：
   按物品ID、aux及**完整NBT标签**分组；`0x266f8c0–0x266f8e1`检查标签存在性，`0x266f810–0x266f81f`比较标签，
   新组在`0x266f969–0x266f97f`复制标签。SDK的InventoryTransactionItemGroup也明确包含mTag/mCount/mOverflow。
4. **`InventoryTransactionManager::addAction`，`0x266ff70`**：
   `0x2670200–0x2670213`遍历全部groups，任一非零或overflow都不发送；只有全平衡才在`0x2670215–0x2670222`
   调用玩家sendInventoryTransaction，随后清空current。`forceBalanced=true`才会走`0x267027c–0x2670291`的原生平衡路径。
5. **原生发送**：ServerPlayer::sendInventoryTransaction `0x29198b0`生成普通ComplexInventoryTransaction，
   经sendComplexInventoryTransaction `0x2919900`，在`0x2919a55`真正发送InventoryTransactionPacket。
6. **tick末未平衡事务不是可靠的延迟队列**：`Player::updateInventoryTransactions` `0x1efad0`：
   服务器走`0x1efbe1→0x1efcdd`，将`this+0xa78`的current清零并析构，**不是下一tick必发**。
   客户端未平衡分支在`0x1efc18`建立type=1的事务，随后发送；SDK ComplexInventoryTransaction::Type中1就是InventoryMismatch。
   与第二轮收到全量InventoryContent的恢复现象一致，不能把恢复轮当作正常拾取通过。

正常情况下，pickup的“入包ammo +N”加“world ammo −N”抵消，原生事务立即发送，部分拾取也成立。
但我们的普通枪setter先留下`旧枪NBT −1`、`新枪NBT +1`两个不同组。ammo组归零了，**枪组仍不为零**，
所以本应发送的拾取一起被扣住并在服务器tick丢弃。下一轮客户端与服务器from不同，又触发mismatch恢复。
这条链同时解释了日志pending=true、own-menu无包、地面数量正确和奇偶轮交替。

## 3. 修复选择：结束枪写回事务，不在take后掩盖问题

LeviLamina **v26.51.7** 的`Player.cpp`明确显示：

`Player::setSelectedItem(item)` → 选中container为Inventory时调用`Inventory::setItem(slot,item)`。

本机`Inventory::setItem` `0x1d5150`又明确以`forceBalanced=false`转发原生setItemWithForceBalance。
0.3.31只修了弹药提取的true，枪写回仍false，遗漏正发生在extract后的reload gun state写回。

现在`writeSelectedGun`保留相同的ItemStack/NBT构建、选中槽、**原有单槽InventorySlotPacket**和同槽快照提交，
只把实际写入改成原生`setItemWithForceBalance(slot,item,true)`（通过受测试的writeServerInventorySlot helper）。
原生setter继续选择自己的ItemStackNetManager/legacy事务分支、保留ID策略；不手写ID、不清除transaction/expected actions，不跨线程。

**没有采纳take后发送绝对ammo槽包的候选方案**：绝对数量虽幂等，但仅发送包不消除未平衡枪组，
会隐藏“缺包”表现而不恢复正常原生事务。本次有更上游、范围更小的原生写入修复，不需要给每次拾取再加一轮库存扫描/包。
take hook继续完全只读、默认关闭。若新实机证据仍有其他堵塞源，再按证据处理，不用无限补包兜底。

## 4. 回归自检边界

| 路径 | 本次处理 / 离线判断 |
| --- | --- |
| 连射、换弹写回、切模式 | 共用writeSelectedGun；均以原生平衡边界结束，不只是换弹时特判；单槽实时包不撤回 |
| 无地面弹药60→30 | 已通过的extract原生forceBalanced＋目标槽包完全不改 |
| 生存完整/部分、创造满包 | native take/add/canAdd和原有返回值、地面actor/余量、discard、音效均不改；pickup不强制balance、不补包 |
| give ammo / gun | 本次不改原有拆堆/平衡扣增/原生add/目标槽包/满包掉脚边；尤其要实测give紧接拾取，不能据源码不变宣称已验收 |
| 箱子shift双向、漏斗、drag/split | 无容量/ItemStackRequest/桥策略改动；原生转移的自然平衡不改 |
| 丢枪/拾回 | 仍使用相同选中槽与原生ID政策，显式包不变；新setter需实机确认正常事务/drop校验 |
| 取消换弹/切枪 | 不增加供弹/退款/定时纠正；已有Lua消费时机不变。取消前后实际消耗按原有语义核对 |
| 主菜单/退出换世界 | 不增加hook；诊断仍会话防护、默认关；实际写回仍只在服务器玩法路径调用，不缓存原生指针 |

新增测试验证helper真的传true、保留完整栈；另用明确标记的**原生规则模型**复现NBT1→1阻塞、两个actor的24+1拾取、连续8轮不交替。
模型不是游戏内native集成测试，不能替代下一轮日志。

## 5. `/tacz rdebug ammosync 1`实机步骤

1. 在世界内先开`/tacz rdebug ammosync 1`。再次1重置采样预算，0关闭。
   新`gun`样本单独限256条，避免占用ammo语义预算；仍建议按弹药slot、reason=pickup过滤。
2. 生存满包、一槽60发7.62x39，旁有地面弹药，先确认不可拾取；开火4→换弹。
   **至少连续6轮**，第一轮也必须回60，地面每轮只减少实际填弹数。再用25/19发及两个地面actor重现`.3.log`。
3. 新日志预期：枪slot action有`gun=true forceBalanced=true pendingAfter=false gunGroupsAfter=0`；
   pickup的最后一个world移除action应`pendingAfter=false unbalancedAfter=0`，同时出现`reason=pickup`的原生InventoryTransactionPacket发送。
   客户端`InventoryTransactionPacket/applied`同槽应由消耗后数量恢复60；**不是等下一轮的Content全刷才恢复**。
   若仍pending，检查`gunGroupsBefore/After`、`ammoGroupsAfter`、`unbalancedBefore/After`，不要再猜快照或stale net ID。
4. 不开debug（或0关闭）再做连续换弹，确认修复不依赖观察hook；预算不够重新1，不能把“没日志”当作没动作。
5. 移走所有地面ammo：60发、开火30、换弹立即30；然后测试扣空、跨槽、不足量、取消/切枪。
6. 连射实时枪弹量、Q丢枪/拾回/重进、survival部分余量/无声、creative满包discard、give两类/溢出、箱子shift双向（含53+60）、漏斗、拆堆。
7. 图标、主菜单/退出/换世界基本回归；本版无新的alpha/材质/枪包改动。

**本轮状态**：静态链＋旧日志支持根因；0.3.32实际发送/客户端应用/ID与交互回归仍待用户新实机验收。ROADMAP相关项保持🟡。

## 6. 构建、测试与部署

- server/client双端Release通过；`xmake build tacz-tests`、`xmake run tacz-tests`：**200项测试／920,410条断言全部通过**。
- `git diff --check`通过；xmake/tooth/BP/RP/版本测试同步0.3.32（含BP依赖RP版本），版本文件无BOM。
- 确认游戏关闭后执行`scripts/deploy-client.sh`，正式部署**0.3.32/client**；未停止/启动游戏。
- 本地产物与部署DLL/PDB/manifest及BP/RP完整文件树哈希一致，未改枪包。
- DLL SHA256：`e033f47c42f2137b33cc389f2bd0f694c93769a5533bd33d98a2efe7ec93a626`。
- 本轮没有新的实机日志，不能以离线模型测试替代连续6轮补弹及交互回归。
