# 0.3.24：槽位批次、动态名称／tooltip、弹药插入

## 证据与尚未证明的部分

- 用户验收0.3.23 `9fc3404`：HK416D侧字不再闪烁（顶点位置精度修复通过）；旧setgun枪可用，生存换弹扣备弹正确。
  保持UV边界分面和局部坐标重基原样，第三方gunpacks不改。
- `2026-10-26.15.log`：260行stack/name hook=true；268行InventoryItemRenderer hook=true；
  没有任何`[item-icons] rendered screen=`；299/302/303行BEI显示当前语言`zh_CN`，437行Lamium tooltip有活动。
  这些只证明安装成功和游戏中文环境，**不能反推旧render入口调用过、纹理上传过或被谁覆盖**。
- 对照Lamium的实际flat-item实现：`render/ItemIcon.cpp`使用SharedMesh、atlas.items和
  `ItemRenderer::renderGuiItemInChunk`，不是每个平面槽都调用旧`InventoryItemRenderer::render`。
  这是明确存在的绕行路径，**当前用户这三个screen是否全部选该分支，仍需新运行日志确认**。
- 名称原先仅hook getHoverName，且新物品写死英文display.Name；redacted/name/hover-text路径未覆盖，
  也没有真实调用日志。改为公共访问器`getFullLanguageCode()`而非直接读`mCode`；不声称旧字段本身已被证明读错。
- 原弹药代码每次只创建一堆，再靠native add后总数差让外层继续；任一次native add无进展就停止。
  仅hook栈上限不能统一native canAdd/add/descriptor判断（Item的descriptor上限入口不带NBT；BP基线64，7.62为60）。
  源码还证实自动英文display.Name和TaczAutoName参与NBT比较。**旧日志没有native add/canAdd读数，
  不能断言本次拾取拒绝一定是64/60冲突，或一定是NBT差异**。

## 图标：改变批次路由，不混入atlas

`InventoryItemRenderer::$getBatchType() const -> UIBatchType`：世界内仅对有pack slot PNG的TACZ身份返回
原生`Default`批次；其他物品／无slot／菜单全部保留origin。这样进入既有安全的`$render`路径，
在真实owner的position/size画自有PNG；成功则不再提交该槽原生占位图，失败仍origin。
不用共享atlas纹理替换、猜测hotbar坐标、伪造ComponentRenderBatch或by-value空类型。

新增证据链（带世界／screen／身份）：

1. `batch-entry`：实际入口、物品ID/数量、身份和native batch数值；`batch`：切到Default。
2. `render-entry`：真实screen/pass/hidden；`entry`：身份与slot路径。
3. `[ui-texture] uploaded`：PNG尺寸、上传成功、world；缺文件／解码失败另有日志。
4. `rendered screen=...`：成功绘制位置／大小／path；失败明确`paint failed`并回退。

**实现覆盖目标**是使用该公共renderer的热栏、背包、箱子／其他容器；本轮没有启动游戏，
**这三类screen均未新验收**。必须同时看画面和分screen成功日志，不能再拿hook=true代替。
飞行／拖拽预览、特殊OreUI、自定义screen、glint、第三人称和掉落物仍不承诺pack外观。

## 名称与资源

- 每次请求读取当前I18n的完整language code（SDK not_null返回、签名断言），统一`zh_CN`/`zh-CN`→`zh_cn`；
  每key按当前语言→en_us→原key回退。新give不再写死英文display.Name；旧TaczAutoName仅在与display.Name相等时视为自动名。
- 覆盖`getName`、`getHoverName`、`getRedactedName`、`getRedactedHoverName`与完整hover text。
  真自定义名称仍优先，标题清除有效§格式并强制`§r§f`白色。没有在绘制中改物品NBT。
- 实际安装包与参考包都写的是`§e7.62x39mm 步枪弹`／`§e7.62x39mm Bullet`，所以标题是白色“步枪弹”，
  **不会硬编码为截图另一版本的“铜壳弹”**；枪名中文为“AKM 突击步枪”。
- 通用十个key/语言从授权TACZ源码资源选入`resources/tacz/lang`并二进制嵌入；晚加载pack可覆盖。
  本机未找到TACZ发布jar，不能声称与用户jar逐字节一致；引用commit和逐key对比测试见resources README。
- pack名称按Java `PackInfoManager`：`assets/<namespace>/gunpack_info.json`中的可翻译name，
  不是目录名或gunpack.meta的namespace。加载时缓存key，hover不反复读文件。

## Tooltip：安全的动态client hover text

对照 `ClientGunTooltip`/`GunTooltip`/`ClientAmmoBoxTooltip`/`GunTooltipPart`、
`ModernKineticGunItem`、`AmmoItem.appendHoverText`、`AttachmentDataUtils`。
Java这些item没有rarity覆写（默认common）；AmmoItem额外描述为灰、pack蓝色斜体。

hook完整定义的`ItemStackBase::getFormattedHovertext(Level&,bool) -> Bedrock::Safety::RedactableString`，
不写静态服务器lore。每次从当前栈NBT读弹数/chamber/mode/HideFlags/DummyAmmo，保持名称、弹数和语言动态；
原有自定义lore追加保留。HideFlags六位按Java顺序独立隐藏，缺省0；类型footer保留。

默认AK47中文顺序和计算（纯测试逐行检查）：

| 内容 | §代码／行为 |
|---|---|
| AKM 突击步枪 | 白 `f`，无嵌入rarity色 |
| 从雨林到海湾… | 灰 `7` |
| ammo slot + ammo名；31/31 | 名基色 `6`，保留该处pack `§e`；计数深灰 `8` |
| 经验等级：0 (MAX) | label `8`，值 `5`；Java现代枪level/maxLevel都为0，不是伪造升级 |
| 枪种：步枪；伤害: 9 | label `8`、值 `b`，冒号遵循源lang实际文本 |
| 25% 原版护甲穿透；150% 爆头伤害 | `6`；armor clamp0..1 |
| 移动速度：-5.2% | `c`，`-0.015 * 3.5`、一位百分比 |
| <<< 在游戏内按下 Z 打开改装界面 >>> | `e`+`o`，Java原文；**只是兼容提示，Z改装功能未实现** |
| TAC Zero 默认枪械包；步枪 | pack `9`+`o`；type `9` |

伤害取extra_damage第一段（无则bullet.damage），再加当前fire_mode_adjust；armor/headshot同样应用模式调整。
支持percent/dummy/inventory显示分支、per-projectile伤害形式及基础爆炸附加文本；倍率为当前实现的Java默认1。
ammo tooltip：白色名称→可选index说明→蓝色“TAC | 弹药”→蓝色斜体pack名。

ammo小图采用与Lamium已使用的安全路径：HoverTextRenderer渲染期间捕获完整Font::$drawCached调用，
以纯§标记匹配已生成文本的ammo行；flushText后绘制16×16自有slot图，预留20单位缩进。
只保存最多16份普通字符串及世界token，不保存native字体指针；退世界清空，纹理走同一资源生命周期。

### 明确偏差／风险

- Bedrock深灰`§8`是`#555555`，Java panel label/count是`#777777`，**不是RGB完全一致**；其他列出的传统颜色一致。
  字体、tooltip背景、行距／分组间距由Bedrock决定；还没有Java 300px/最多3行的描述排版。
- tooltip小图仅在native传来完整匹配文本时画；额外自动折行／替换字体可能影响行坐标或导致安全跳过。
  字体hook、Default批次选择、热栏/背包/箱子画面都要实机确认。
- 配件／改装尚未实现：不声称装配后的伤害/有效重量/扩容/slug/tag modifiers和自定义Java sync倍率完全复刻。
  当前显示的是已支持的基础枪和模式调整；ammo box机制/tooltip本轮不实现。
- 不接管其他模组追加tooltip的排序；本hook优先级与Lamium等仍需共存测试。原lore保留但位置未逐像素复刻Java。
- 无客户端TACZ时新物品只能显示类别fallback；BDS、同步、server-only提示仍在ROADMAP最后。

## 弹药：统一计划、数量守恒

`planAmmoInsertion`由纯逻辑计算每槽增量和剩余；native `Inventory::canAdd`与`add`和命令共用它。
先合并匹配栈，再占空槽，使用各ammo有效limit（1..64）；满栈不阻塞后续空槽，满背包只接收能容纳的数量。
canAdd只做快照/计算，保留native只读结果作对比日志，不偷偷修改inventory。
add使用typed ItemStack副本和setItem，扣减传入source至剩余；只要插入有进展返回true，未插入部分留在原来源栈。
give显式遍历拆堆，120/60=60+60，不再靠总数差触发外层循环；报告确切插入数量并刷新背包。

合并要求base item、aux及所有有效NBT一致；不同AmmoId不合并。只忽略带TaczAutoName标记且完全匹配的旧自动display.Name，
保留真正改名、lore、其他任意NBT和原生tag类型。`hasSameUserData`窄hook也让走该比较的native拖拽／容器路径遵守此规则。
不跨base type把旧铁粒强行合并成新tacz:ammo；旧超限栈不自动裁剪／拆分，仍可消耗。

`[ammo-stack] give`记requested/inserted/limit；`canAdd`记native与adapter预测；`add`记requested/inserted/remaining。
**尚未取得新实机读数**，尤其需要确认地面部分拾取的调用方、来源更新／同步和容器拖拽路径；不能宣称全部native路径已验证。

## 验收步骤

1. 新give AK47/HK416D；热栏、背包、箱子双向移动，核对pack slot像素、位置、数量、选框与hover。
   收集batch-entry→batch→render-entry→upload→rendered链，不只启动hook行。
2. 中文hover检查上述13行、ammo图和白色标题；射击后31→30，空仓0/30；换弹后恢复。
   后续语言测试时已有物品切英文／中文，不重发；真anvil改名保持。
3. 空背包`/tacz give ammo tacz:762x39 120`应60+60；先留50再给20应60+10。
   满60加地面60仍能占新空槽；满背包仅留一发空间时只拾1发、地面剩59，不能丢失或复制。
4. 不同AmmoId、不同aux/自定义名/lore不合并；旧0.3.23自动名和新无自动名可合并，legacy不同base保持分开。
   再测6/48上限ammo、shift-click箱子、拖拽、满背包发放提示与生存换弹扣数。
5. 主菜单、退世界/换世界、RTX开关、反复开关箱子，检查缓存释放、无崩溃及HK/Timeless/手枪UV回归。

构建／测试／部署实际结果见CHANGELOG；纯测试只证明模型／字符串／数量计划，不替代native拾取或UI验收。

离线重跑：`xmake build tacz-tests tacz-pack-audit`、`xmake run tacz-tests`；
`xmake run tacz-pack-audit <gunpacks目录> <输出JSON>`，再运行
`python scripts/verify-item-ui-audit.py <JSON>` 和 `python scripts/verify-item-depth-audit.py <JSON>`。

本轮实际结果：双端x64 Release通过，178测试／654,221断言通过；安装325枪／80ammo的UI元数据与UV/坐标精度审计通过。
部署前两次进程复查均无Minecraft/bedrock进程，已部署client0.3.24；DLL/PDB/manifest及RP11/BP5全部路径／SHA256一致。
DLL SHA256：client `d6202ad3252eba9586cfde6b3bc4560b947e16d76d2ace9f13e4e748ede98406`；
server `e501f16e376e9d0461694487cab31be02bbb3448e8a706912b5d8206e9cfee81`（保留在外部临时构建目录）。
未启动游戏、未终止进程；不能将已部署误读为新版本实机验收通过。
