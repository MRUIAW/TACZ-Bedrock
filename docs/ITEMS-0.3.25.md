# 0.3.25 临时测试版：完整槽位身份、真实弹数、部分拾取修复（A1待续）

**用户决定先发布当前修复用于测试；A1 上限统一尚未完成，下一版继续。**
本版包含槽位身份图标、数值Tag读取、部分拾取bool修复和give溢出掉落；A2已移除，Player::take只观察。
**已知可见影响：部分操作仍可能按64堆叠，而不是枪包上限（例如7.62x39超过60）。用户接受该临时风险。**
已知绕过点为ItemStackBase::set/isFullStack/isStackable、FillingContainer::_getSlotWithRemainingSpace/_addResource；
漏斗/通用容器/UI/网络/客户端预测仍未完整归类。部分add返回true导致复制的根因已独立修复，
不能把这个bool修复说成A1统一完成，也不把编译通过当成实机拾取验收。

## 0.3.24的实际结果与证据

用户测试 `d05b631`：120发得到60+60、空槽整堆拾取通过；热栏/背包槽图仍占位，枪tooltip弹数恒0；
有剩余容量但无空槽时发生部分拾取复制／客户端迟同步。**撤销“剩余source栈已足够保证原生拾取守恒”的假设**。

`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs/2026-10-27.log`：

- 352–355、418–435、1047–1061行：batch与render入口都运行，但所读render-data副本均`identity=none`。
  numeric `itemId`是类别/原生绘制ID，不含GunId；同一个tacz:modern_kinetic_gun对应许多枪，**不能反查为某把枪**。
  旧日志只证明身份键缺失，不能进一步断言整个UserData都为null、或某个具体runtime数字一定属于本模组。
- 488、2042行：tooltip ammo小图成功绘制（300ms/556x45），说明上传和drawImage路径有效；
  349等行能解析AK47中文名，406–417能解析GunId却弹数全部0，故不能把两个问题都归结为纹理或NBT全部丢失。
- 824、925行give requested=120/inserted=120；859行整堆add inserted=60/remaining=0。
- 3915–3916行部分拾取：ground输入60、accepted=59、inserted=59、剩1；之后accepted=0。
   结合下文反汇编：旧适配器的部分插入返回true，跳过native false分支里的地面余量提交。
   不是“native add必须全有或全无”，也不能把64读取写成已证明的复制直接原因。

## 槽图：从完整槽位读取，不从绘制副本反查

新增有完整签名检查的 `InventoryItemRenderer::$update(IClientInstance&,UIControl&,UIScene const&)`：

1. 先运行native update，保持原生绘制字段和UI更新逻辑。
2. `UIControl::findAllCollectionIndices`获得实际owner所属collection和index；
   容器/背包经该scene的 `ContainerScreenController::_getVisualItemStack(collection,index)` 读取完整栈，
   与原生hover同一条视觉物品解析路径（含派生controller的virtual实现）。
3. HUD的`hotbar_items[0..8]`直接读当前local player背包对应槽；Mojang hud JSON明确使用该collection。
4. 只接受完整栈base ID和aux与绘制副本一致、且身份合法的候选；不同身份候选发生冲突时回退原生，
   **绝不使用另一个槽的GunId，不按名称/类别ID猜图**。
5. sidecar只存普通身份值、world token和owner地址校验值；不持有controller或ItemStack引用。
   每次update覆盖；同类别切换GunId也令批次失效；退世界/disable清空。native render-data本身不被注入NBT。
6. batch/render从此sidecar取身份，继续走Default独立PNG绘制；数量/选框等仍native。

`[item-icons] resolve/source`会并列记录collection/index、render/source类型、实际getId/aux、
copyNBT/sourceNBT、完整GunId/AmmoId及是否匹配。这样可实测本次动态runtime ID是否对应，
而不是将测试fixture里的示例数字852硬编码到游戏。随后应看到`batch ... selected=Default`和`rendered screen=...`。

SDK的`brstd::function_ref`在本机是完整16字节实现，本次直接使用其正常lambda构造与签名断言；
不是过去禁止的空placeholder/手工ABI布局。无伪造batch、无native类型裸内存构造。

**目标覆盖**：热栏、普通背包、箱子/容器公共renderer；本轮没有启动游戏，以上screen均未新增实机验收。
特殊OreUI/飞行图/独立renderer仍不承诺；collection解析失败会安全保留占位图，新日志用于准确区分失败分支。

参考实现/布局（只读证据，不当作游戏数据写入）：
`amatouhake/Lamium` 的 `features/inspection/hover/HoverTracker.cpp`；
`Mojang/bedrock-samples/resource_pack/ui/{hud_screen,ui_common}.json`。

## 弹数恒0：SDK数值转换错误，与身份问题不同

SDK `IntTag`/`ByteTag` 的转换operator约束为 `std::integral`。
`CompoundTagVariant::operator double()` 的visitor仅在具体tag可转换为double时返回值，否则返回默认0。
0.3.24数值读取先转double，因而Int弹匣数和Byte膛内标记都被变成0；不会抛错，原纯builder测试也覆盖不到它。

现在switch实际tag类型，读具体tag的`.data`；支持Byte/Short/Int/Int64/Float/Double并保留Java取整规则。
原生编译断言证实`IntTag/ByteTag -> int`可转换、`-> double`不可；纯测试用相同约束的tag验证此回归。
不从玩家当前手持枪偷换hover目标，也不把缺字段的枪硬设满弹。

数值读取复查：`src`、`src-client`、`src-server` 的 `static_cast<double/float>`、
`get<double/float>`、`getDouble/getFloat` 均检查；没有发现第二处native整数Tag转浮点的同类错误。
`GunItem.cpp::readGun` 直接转int；HUD、客户端射击与服务端Shooter/换弹沿用该state读取；
pickup读取的是mCount及StringTag AmmoId，不走浮点。`AnimationStateMachine.cpp::NbtView::number`
和`GunLogic.cpp`处理的是nlohmann JSON，其整数到double转换正常；其他命中为数学计算。
因此本次同类native转换修复在PackItem tooltip，不虚称修改了原本正确的HUD/射击/换弹读数。

`ClientGunTooltip.java:134–136`的准确规则：

- `effectiveChamber = rawHasBullet && bolt != OPEN_BOLT ? 1 : 0`。
- current = 当前弹匣数 + effectiveChamber。
- max = 有效弹匣容量 + **当前effectiveChamber**；不是闭膛永远无条件+1。
- fresh AK47 30+1→31/31；发射后29+1→30/31；无膛内弹0/30。
  open-bolt即使raw标记true仍30/30；manual-action同样取决于raw膛内状态。
- Java扩容先按attachment有效level取extended_mag_ammo_amount，再加effectiveChamber。
  builder支持传入有效capacity，测试40+1→41/41、无chamber40/40；**装配/扩容玩法仍未实现**，
  本版不谎称能从未支持的附件NBT正确装配整套Java modifier。

## 小图怎么画、为何暂不改行距

普通说法：**字体开始画tooltip时，我们认出弹药那一行，记住它在屏幕上的位置；
文字画完后，再用drawImage把枪包的弹药小图贴在该位置左侧。**
具体是`Font::$drawCached`匹配已生成文本的§标记，`HoverTextRenderer::$render`后flushText，再画16×16 PNG。
不需要字体本身支持图片，不修改枪包图。

Font调用确有linePadding，可安全观察，但仅改绘制padding不能同步先前已测量的tooltip背景大小、裁剪和自动换行；
Java还有10px行高/分组额外间距，不是一个全局padding就能复刻。本版按用户许可**保留Bedrock原生行距**，
记录`[item-tooltip] native wrapHeight/padding`，不改全局字体或其他模组tooltip布局。

## A1 反汇编：部分插入的native契约

只读分析本机1.26.51.01的 `Minecraft.Windows.exe`；以下均为RVA，不是可跨版本硬编码hook地址。
代码hook仍使用SDK完整成员签名，不安装裸RVA跳转。证据定位方法：SDK虚表序号、RTTI lambda锚点、
`.pdata`函数区间、capstone反汇编与直接E8交叉引用；不能把其他类同一虚表偏移的调用误认作Item。

定位锚点为 `setItemWithForceBalance@Inventory` 的lambda RTTI：type-return leaf `0x22e4c0`、
functor虚表引用 `0xe6d5858`、父函数 `0x1d5170`，定位Inventory虚表 `0xe6afe10`。
其add槽45→`0x1d5fa0`，canAdd槽46→`0x1d60a0`；add尾跳 `FillingContainer::add 0x25a3ce0`。

1. `FillingContainer::add` 在 `0x25a3de6` 调 `_addResource 0x25a3e50`，
   `0x25a3df1` 用 `ItemStackBase::set 0x24870a0` 写回source余量；
   `0x25a3dfa–0x25a3e08` 比较本轮是否有进展。**部分插入后最终无进展返回false；耗尽source返回true**。
2. `Player::take 0x1f6c60` 的inventory add虚调用位于 `0x1f741e–0x1f742c`；
   `0x1f7432–0x1f7434` 测试bool。true跳过余量分支；false进入 `0x1f74e1`。
3. false分支比较原始/local计数，按差额构建已消耗数量事务；
   `0x1f75ce–0x1f75df` 将local余量写回原ItemActor；非零余量在 `0x1f760c` 仍返回false。
   **native take返回false并不表示没有发生部分拾取。**

0.3.24自定义 `Inventory::add` 的 `return inserted>0` 精确选错了第2步分支。
本版已改为“remaining==0才true”，允许partial mutation并返回false；**原生并非all-or-nothing**。
当前仍保留自定义插入计划，不能称为已完成A1或已恢复纯原生Inventory::add。

`Player::take` 现在只观察：原参数/返回值不变，不分离actor、不强制refresh、不覆盖取消结果、
不构造网络包。`[ammo-pickup]`记录实读groundBefore/groundAfter、inventory差额、native bool和conserved；
native调用后按ActorUniqueID重新查实体，不使用原ItemActor/ItemStack引用。
**A2预留/生成余量actor代码已从源码移除，并非暗中启用的后备方案。**

### 上限读取点：已证实与未覆盖

| 原生位置/操作 | 证据 | 当前覆盖/边界 |
| --- | --- | --- |
| ItemStackBase::getMaxStackSize `0x247df40` | getDescriptor后调用Item虚表槽100/+0x320 | 已有AmmoId hook；只覆盖真正调用此函数的地方 |
| Item::getMaxStackSize `0xc19f0` | `movzx eax,byte ptr[rcx+0xa8]; ret` | **未hook**；返回共享mMaxStackSize，本item定义64，不能区分AmmoId60 |
| ItemStackBase::set `0x24870a0` | `0x24870de–0x24870eb` 内联descriptor getter，`0x248711f–0x2487128` clamp | **未统一**；不是上面stack getter的调用者；native余量写回也走这里 |
| isFullStack `0x2488bb0` | `0x2488bee–0x2488bfb` 内联getter，`0x2488c2c`计数比较 | **未统一**；完整栈在函数入口可得 |
| isStackable() `0x2488c90` | `0x2488cd5–0x2488ce2` 内联getter，max<2为false，之后仍有damage/NBT判定 | **未统一**；不能直接用limit>1代替全部native判定 |
| isStackable(other) `0x2488fe0` | 调无参isStackable、Item槽35/+0x118 isStackedByData、aux比较和userdata路径 | isStackedByData不是上限；不能随意改为忽略aux/NBT |
| hasSameUserData | 现有typed hook | 仅忽略明确标记的旧自动名，其余真实NBT保留；不替代item/aux检查 |
| Inventory::add/canAdd | `0x1d5fa0/0x1d60a0` 调 `_getSlotWithRemainingSpace` | 当前自定义adapter；add已纠正bool，canAdd仍只读预测，不是native全路径统一 |
| FillingContainer::_getSlotWithRemainingSpace `0x25a4730` | `0x25a4819` getDescriptor；`0x25a4822–0x25a482f` 内联max；`0x25a4864`计数比较；`0x25a489f` stackability/NBT | **未统一**；64/60差异确实能影响是否认为槽已满 |
| FillingContainer::_addResource `0x25a3e50` | source `0x25a3ea8–0x25a3eb5`，target `0x25a403e–0x25a404b` 内联max，另有`0x25a4095` stack getter | **未统一**；source与target均有完整栈，不应按同一个类别ID猜AmmoId |
| Player::take | `0x1f7220` 调stack getter；false部分分支按上文提交 | stack getter覆盖该读；set/slot-search/addResource的内联读仍不能据此声称覆盖 |
| /tacz give | 已有纯拆堆/insertAmmo计划 | 遵守AmmoId上限，溢出drop；**不等于native /give已审计** |
| Container通用add/merge、漏斗、craft/UI拖拽、server ItemStackRequest及client prediction | 尚未完成调用图归类 | **A1待续**；本版接受部分操作仍按64堆叠，不能从SDK有这些方法或hook安装成功推断覆盖 |

另外在与请求处理代码相邻的函数发现确实绕开stack getter的读取：
`0x7c98a10` 在 `0x7c98b05` 从完整栈生成descriptor，`0x7c98b0d–0x7c98b1a` 取max，
`0x7c98b51–0x7c98b55` 返回max-count；`0x7c992b0` 在 `0x7c993c0/0x7c993c8–0x7c993d6` 同样读取max。
另有 `0x7c9a5d1`、`0x7c9c057`。这些函数**尚未可靠匹配到SDK具体方法名/完整调用图**，
不因地址相邻就宣称已证明它们分别是哪一个服务端校验或UI路径，更不安装猜测ABI的hook。

### Descriptor、直接字段与A2决策

`ItemStackBase::getDescriptor 0x2486a70` 的item分支 `0x2486bdf–0x2486bfd`
仅把Item指针和aux(+0x20)传给构造函数 `0x24d0b30`，**不传userdata(+0x10)**。
SDK的16字节ItemDescriptor持有unique_ptr BaseDescriptor，不提供AmmoId访问器；
该构造在 `0x24d0c3d–0x24d0c72` 新建并存入24字节item+aux实现，不携带NBT。
因此hook descriptor getter本身，不能凭空区分同base/aux的两种ammo。

尚未证明所有直接/内联mMaxStackSize读取已枚举；已证明的实际字段读取在Item getter `+0xa8`。
同偏移机器码可能属于其他对象，不能把字节搜索命中数当作Item字段引用数。
没有全局改共享Item::mMaxStackSize、没有descriptor类别→AmmoId反查、没有长期缓存native栈引用。
线程局部full-stack scope是**待验证候选**，必须处理source/target和重入，不能让一个AmmoId的上限污染另一个。
若今后hook set，还必须保留旧超限栈的合法减数，不能clamp时默默删弹。

**本轮结论是A1未完成，不是A1已证明不可能。** 没有发现“地面扣数路径只有descriptor、完全无法取得完整栈”的证据；
相反set入口有完整栈，所以不伪造该理由启用A2。A2也不能补上漏斗/UI/网络的未归类读取。
此前因A1覆盖不足暂停发布；用户现明确同意将当前修复作为0.3.25临时测试版发布，不再以A1未完成阻塞本版。
下一版（0.3.26）准确归类上述native入口并验证typed适配，再决定A1能否移除自定义add/canAdd；
若确有必要的无NBT入口才记录其具体影响并讨论窄A2或未来每ammo独立item ID。

`/tacz give ammo`的每个拆堆都先插入、再用native Player::drop掉落该堆余量；满背包也继续处理后续拆堆，
返回inserted+dropped，明确日志字段；gun give的单件溢出同样掉落。不会将“未发放的余量”默默丢弃或当成已入包。

## 测试与风险

纯测试覆盖数值Tag转换、所有bolt/chamber组合/扩容容量、槽位base/aux匹配与歧义拒绝、
0..255数量/容量的计划守恒、native add的false-partial返回模型、满背包give溢出和后续拆堆；继续全部UV/HK/换弹回归。
离线测试不能证明native UI回调、同步、动画/声音或模组共存。新sidecar及部分拾取bool修复都尚未实机验收，
favoredSlot/旧超限栈/多玩家/取消hook仍需回归；BDS联调仍按ROADMAP最后。

### 构建/测试验证（不等于实机验收）

- Release Client、Server均编译通过，typed签名断言通过；仅已有hook宏unused-variable和测试u8path弃用warning。
  验证后xmake恢复client配置，`bin/tacz/manifest.json`为0.3.25/client。
- 全部183项纯测试／916,440条断言通过；这些不执行Minecraft回调或native pickup。
- 本轮重新生成已安装枪包审计：325枪／80ammo的UI元数据、give120守恒与UV/HK位置精度回归全部通过。
  本次临时版重新生成报告 `guns-0.3.25-interim-release.json`（approved temp），未改枪包；`git diff --check`通过。
- 本次临时版已重新构建双端并运行全部测试，结果同上。部署前PowerShell检查未发现Minecraft.Windows，
  部署脚本复制前再次检查进程；已部署到 `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz`，未启动或终止游戏。
- 部署后确认0.3.25/client；DLL、PDB、manifest与本地产物SHA256逐一一致，RP全部11个文件、BP全部5个文件的文件集合及哈希一致。
  DLL SHA256：`E862BE3A07F19565D0ABD0F1553CEEFC2D7F8DC00970E3EA20A3A07860EF7C99`。
  仅替换本模组产物/RP/BP，未改安装枪包；实机图标/tooltip/部分拾取同步仍等用户测试。
- 不把0.3.25临时版发布、双端可编译或离线审计当成统一上限已完成。

### 验收

1. 给AK47、HK与两种ammo，热栏/背包/箱子互移；确认图跟随具体GunId/AmmoId，非同类别全画一个图。
   查resolve/source的ID匹配与每screen rendered，不只hook=true；快速换同类别不同枪回归缓存失效。
2. 新AK47 hover应31/31，发射一发30/31；空仓0/30；open-bolt不加虚构的一发。
   切到另一槽hover，确认不借用当前选中枪的弹数；旧setgun栈仍可读。
3. 满背包只余59/60一格空间，地面60：立即变60，地面59；原栈1/60则立即60，地面1。
   无需Q触发同步；反复踩不能增加；腾空一槽再拾余量；检查动画/声音、conserved=true。
4. 满背包give120应地面60+60；只余一发空间应入包1、地面共119。取枪溢出也应掉脚边。
5. 注意7.62x39是否超过60等包上限，记录发生在拾取、拖拽、箱子/漏斗移动还是其他操作；
   此为本临时版已知A1风险，仍必须核对总数量守恒，不能把复制当成可接受的64上限差异。
6. 关闭/重开背包箱子、换世界、主菜单、RTX；保留HK/Timeless/手枪UV和生存备弹扣数已通过结果。
