# 0.3.23 B：真实类别物品、槽图与名称

**后续实机结果（0.3.24）**：旧setgun枪和生存备弹扣除通过；热栏／背包仍为占位图、名称仍英文，
120发只给60且地面ammo无法拾取。本页描述0.3.23尝试，不是这些功能已通过验收；
安装hook成功日志不证明实际绘制成功。替代路径、动态tooltip与弹药适配见 [`ITEMS-0.3.24.md`](ITEMS-0.3.24.md)。

## 注册与命令

BP注册一个类别一个ID：

| Item ID | 身份NBT | 基础stack |
|---|---|---:|
| `tacz:modern_kinetic_gun` | `GunId`＋既有射击NBT | 1 |
| `tacz:ammo` | `AmmoId` | 64（由NBT索引hook进一步限制） |
| `tacz:attachment` | `AttachmentId` | 1 |

BP格式1.21.80，不加原生use/charge/食物/攻击武器/耐久组件；枪输入继续由既有拦截处理。
RP原生item atlas有三个**原创几何占位PNG**（可用`generate-item-placeholders.py`重生成），不复制第三方包图。
BP/RP manifest升级0.3.23，保留UUID，并声明BP→RP依赖。原生`/give`只给类别空壳，不会自动补GunId。

- `/tacz give gun tacz:ak47`：向背包添加真实枪物品，不消耗/覆写当前手中物品；creative-tab初始状态：首fire mode、满弹匣、原始chamber=true。
  Open bolt仍由有效chamber规则忽略额外一发；fresh AK47为031。
- `/tacz setgun <gunId>`：保留为上面give gun的命令别名，**不再原地转换/覆写当前物品**。
- `/tacz give ammo tacz:762x39 [count]`，默认1；`/tacz ammo <count>`按手持枪AmmoId发真实弹药。
  count范围1..2304，按每类有效stack拆包，给出实际进入背包数量；未知id/缺BP不偷偷回退铁粒。
- 旧vanilla枪/铁粒等携带GunId/AmmoId的**读取仍接受**，不迁移、不删除、不批量重写旧存档物品。
  旧的超过新stack上限的堆叠不自动拆分，仍能读/消耗；NBT不兼容时不把无身份空壳当成枪。
- AttachmentId String读写accessor已加入；装配逻辑/附件display/index/配件发放命令/弹药箱本轮未实现。

### Ammo stack_size 的平台边界

Bedrock data-driven max_stack_size为1..64；同一个item id的JSON组件不能随AmmoId变化。
因此BP基线64，双端使用有签名断言的 `ItemStackBase::getMaxStackSize() const -> uchar` hook读取AmmoId/index，
正常60/48/36/6等按声明返回；发放函数也按该值拆包，原生堆叠区分不同AmmoId NBT。
运行时hook状态记入`[pack-items]`；**背包拖拽/合并/分堆是否所有native路径均尊重此hook仍需实测**，不能用编译代替。

安装80种ammo均有slot映射。两项声明25565：`twx:twx_ldp1`、`twx:twx_nl1`，超Bedrock组件范围，
明确fallback64并启动警告；不宣称完整支持25565堆叠，不写回枪包。缺省/非正值也fallback64。

## 槽位图标：使用真正的公共槽renderer，不覆盖屏幕坐标猜热栏

Hook：`InventoryItemRenderer::$render(MinecraftUIRenderContext&, IClientInstance&, UIControl&, int)`。
LL26.51.7 client头声明的完整签名有compile-time断言，参数都是引用/int；从完整typed `mItemInstance`读身份。
**没有**hook by-value空`UIItemRenderInfo`、`dragon::RenderMetadata`、function_ref，也没有修改共享ItemGraphics/atlas。

pass0且有有效世界、对应client、未隐藏icon时：

1. gun id→gun index display→gun display `slot`；ammo id→ammo index display→ammo display `slot`。
   PNG按TACZ`assets/ns/textures/...png`解析，完全不从HUD图或gun UV贴图猜路径。
2. 在真实owner控件的cached screen position / size，以已有MinecraftUIRenderContext drawImage/flushImages绘制；
   成功时只替换该槽的原生icon调用，不改ItemStack/NBT/全局atlas，计数、选中框、提示框仍由native UI负责。
3. PNG不存在/无身份/纹理上传失败/未处于world/禁用：调用原生origin，占位图仍有效。
4. 复用HUD的世界/TextureGroup/buffer/framebuilder/pipeline key缓存，首次需要时上传，世界退出/管线变化释放；不在主菜单查询玩家或上传。
5. 启动日志`[item-icons] common ... hook=true/false`；首次真正替换各screen时，记录`[item-icons] rendered screen=...`和path/pos/size。

**实现的目标覆盖**：走这个公共renderer的快捷栏、背包网格、容器槽位，不只是手绘hotbar覆盖。
**本轮没有启动游戏，没有任何屏幕被代理实测确认**；需要看`rendered screen=`日志和实际像素验证。
使用其他renderer/独立atlas的OreUI、自定义screen、飞行/拖拽预览及某些glint pass不承诺替换，仍用native占位。
这里没有拿“安全签名能编译”冒充“所有UI已经正常”。若背包native走其他管线，需要其安全item/位置入口再扩展。

安装离线结果：**325/325枪、80/80 ammo** 的slot资产路径存在；资源是否按预期出现在每种UI里仍待实机。
纯测试检查AK47/cz75/glock_17枪和对应ammo的映射；无效/缺slot返回null，不读取用户身份拼接的任意路径。

## 名称、提示与语言切换

Registry预加载包的 `assets/*/lang/*.json`，逐key合并，晚包覆盖；locale统一小写/`-`→`_`，支持default里的`tr-TR.json`，
不是把locale文件名强当ResourceId。每key优先当前语言→en_us→原name key（缺失时可诊断），不是整语言缺失才fallback。
gun/ammo index的`name`就是lookup key；例如AK47英文数据实际是**AKM**，中文是“AKM 突击步枪”，不硬编码为AK47。

发放时写原生Bedrock `display.Name`=包的en_us名称，并写`TaczAutoName`保存本次自动名：

- 即使客户端hook失败，native tooltip/名称至少能显示稳定英文，而不是unknown item。
- client hook `ItemStackBase::getHoverName() const -> std::string`（完整签名断言，无空placeholder值传递），
  world session内读取 `getI18n().getCurrentLanguage()->mCode`，按包lang动态返回；切语言不用重发物品，不在渲染中写NBT。
- 真实玩家自定义名称优先：getCustomName非空且不同于TaczAutoName时保留origin；旧枪有自定义名也不覆盖。
- 仅client SDK导出getHoverName，server不hook它，只写原生英文fallback。服务器仍构建/读取类别物品与AmmoId。
- 如果有UI绕过getHoverName直接使用别的native description/cache，可能仍显示fallback英文；语言切换需热栏名称和hover tooltip分别实测。

## Not yet / 里程碑状态

**第三人称、掉落物真实枪模/pack slot自定义渲染均不在本轮范围**；新物品这些路径显示类别占位图/模型。
配件机制、弹药箱、真实creative每枪自动填充、超64 ammo堆叠、特殊UI atlas/glint/拖拽预览、Persona等仍Not yet。
单人原型物品收尾的代码已落地，但native注册/slot覆盖/语言/生存换弹仍待本轮验收，不能据编译宣称里程碑1已完整通过。
BDS联调、枪包同步、server-only warning仍在ROADMAP最后，不在此版提前开展。

## 验收

1. `/tacz give gun tacz:ak47`，选中新枪，NBT应含GunId/AUTO/30/Byte chambertrue，HUD031。再给HK、Timeless/block_17及两把手枪。
2. 热栏、背包、箱子/容器互移，检查每把不同slot图、缩放/位置、hover/手持名，记录`[item-icons] rendered screen`。
3. `/tacz give ammo tacz:762x39 120`、`/tacz ammo 120`：检查60上限、同AmmoId合并、不同AmmoId不合并、满背包结果；
   生存模式射击后用新ammo换弹，原铁粒AmmoId同样可被正确计数/扣除。
4. 英文/简体中文切换，不重发已有新物品，看名称随语言改变；anvil改名应保留；旧setgun物品仍能射击/换弹。
5. 主菜单、换世界、RTX切换、打开关闭背包与箱子，检查资源释放及无菜单崩溃；第三人称占位是当前预期。

单测：身份/NBT schema、初始弹量/fire mode、locale/key fallback、index/display slot、stack限制、BP行为组件约束；
包含实际default枪UV回归与Timeless/block_17精确跨界fixture。native注册/GUI覆盖/堆叠行为不能由纯单测证明。
