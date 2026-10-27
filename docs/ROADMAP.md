# 路线图（0.3.27，逐版更新）

## 对照原里程碑 1「AK47 单枪可玩原型」

原计划：`C:/Users/MRUIAW/.claude/plans/levilamina-https-docs-levimc-org-levila-mighty-engelbart.md`。

- **已落地并部分实机验收**：原格式枪包读取、Lua 换弹/动画、输入与基础网络、弹药/射击模式、子弹运动/命中/伤害/爆头、
  音效、第一人称模型/手臂、后坐力、机瞄、camera option/冲刺规则。0.3.18 用户确认抖动几乎消失、RTX 内部动画正常，原生 CPU 大幅下降。
- **0.3.19**：功能额外弹匣、open-bolt 显示、手枪收枪 nil-track、隐藏占位弹体、Gson 兼容、枪械列表/资产诊断、325 枪离线审计。
  已后续部署，用户部分多枪验收通过（见0.3.20记录），HK416D/P320/guns/packs未收到验收。
- **0.3.20**：用户验收准星、身体/爆头/击杀标记及音效 OK，percent/数字混用符合枪包；HUD字形/比例需调整；
  display开关驱动透明材质、cutout阈值近似、blend/probe调试开关。Timeless/HK416D的Java默认实际是cutout，
  不能宣称两处贴花已修复。证据、偏差与验收：`HUD-ALPHA-0.3.20.md`。
- **0.3.21**：TACZ HUD布局纯函数/原生bitmap字体尺度/带阴影版本行；fresh闭膛031测试与hud快照；
  四张贴花面UV/texel矩形对照相同，增加离线采样图片与显式单mip实验。根因未证明，默认保留旧上传。
  用户已确认Timeless/HK416D在blend0/1均错且无差别，不再把alpha阈值当作已成立修复路线。
  详见 `HUD-SAMPLING-0.3.21.md`。
- **0.3.21用户验收**：HUD已很接近Java、fresh闭膛031本来就正确、空/低弹红色正确；RTX/菜单/换世界稳定。
  Timeless/HK的depth1/2和sampler0/1均无差异。
- **0.3.22**：证实原生UV0低16位溢出导致端点插值倒扫纹理；枪上传缓存边界分面/整数平移，保留mesh-once。
  安装325枪+参考default54枪源面UV审计通过，实机贴花/RTX待验；HK命名骨无helper折叠差异，不能称已修复。
  新候选是half位置量化抹掉0.001pixel外扩，待读实际buffer；见`UV-PACKING-0.3.22.md`和两份UV-AUDIT。
- **0.3.22用户验收**：Timeless/block_17滑套贴花正确，枪包未改；cz75/glock_17弹匣、AK47和其他枪回归OK。HK侧字仍z-fight。
- **0.3.23**：远pivot group坐标重基，保留作者inflate的half精度加固；修正mesh1只在build时读日志的问题。
  用户指定日志13条其实是builds=0帧统计，没有实际格式读数，不能宣称half根因已实机确证；见`HK-DEPTH-0.3.23.md`。
  三个真实类别物品、give命令、slot图/名称的初版hook；配件仅注册/NBT，见`ITEMS-0.3.23.md`。
- **0.3.23用户验收**：HK416D侧字已无z-fight，确认顶点位置精度修复；旧setgun枪和生存换弹扣备弹通过。
  热栏/背包仍占位图、名称英文、tooltip缺失；120只给60、地面ammo无法拾取，初版UI/堆叠未通过。
- **0.3.24**：pack槽物品Default批次路由、实际入口/上传/绘制日志；完整语言code和白色名称覆盖normal/redacted/hovertext；
  Java来源动态tooltip与ammo小图、通用lang嵌入；give显式拆堆、共享只读预测/插入计划及auto-name NBT兼容。
  新热栏/背包/箱子与部分拾取仍待实机验收；不把新hook=true或纯测试当屏幕成功。详见`ITEMS-0.3.24.md`。
- **0.3.24用户验收**：120→60+60及空槽整堆拾取通过；槽图仍占位，绘制副本身份丢失；
  tooltip弹数为0；部分拾取存在复制／迟同步问题，不将旧纯数量计划当作原生拾取提交已通过。
- **0.3.25临时测试版（用户同意先发布）**：从controller/本地背包完整栈关联槽位身份；修复SDK整数tag转double静默0；
  反汇编证实native add允许部分写入且有余量时返回false，纠正旧true分支；移除临时A2，只观察实读地面数量；give溢出掉脚边。
  **A1统一未完成，下一版继续**：已知绕过点为ItemStackBase::set/isFullStack/isStackable、
  FillingContainer::_getSlotWithRemainingSpace/_addResource；漏斗/容器/UI/网络/预测仍未完整归类。
  部分操作可能按64堆叠而非枪包上限（如7.62x39超过60），用户接受该临时风险；部分add返回true导致复制的根因独立修复。
  行距保留原生，新screen/部分拾取仍未实机验收；详见`ITEMS-0.3.25.md`。
- **里程碑1单人收尾：代码落地，验收待完成**：各UI槽图/名称/tooltip、ammo发放与拾取、语言切换需新版本实机确认，HK与生存备弹扣除已通过；
  不将编译/离线测试当成里程碑完整通过。当前为单人/内置服务器原型；`setgun`现为`give gun`别名，旧vanilla GunId物品仍读兼容。
  手动拉栓、击杀计数overlay、皮肤等后续机制/表现与BDS联调仍Not yet，不因“真实物品已注册”抹掉它们。
- **已知视觉待办**：新物品slot各screen覆盖、tooltip图/行距/字体共存、B93R 原动画弹匣离屏、P320 等检视倾斜、View Bobbing 瞄准行走左右摆。
  皮肤/Persona 仍有占位路径，不把“手臂能动”等同皮肤完成。

## 0.3.26 收尾

- 0.3.25弹数已通过，未观察到超过60；槽图仍占位，部分拾取余量会真实消失。
- Part1：native add false之后的创造模式丢弃分支导致take=true；仅在当前玩家弹药拾取有余量时抑制该分支，保留原actor/native提交，不用A2。
- 保存/重进后的余量、客户端立即同步仍待新实机验收；图标祖先槽位查找随后单独提交。其他A1读取点仍待续。

## 0.3.27

- 用户验收0.3.26 Part1：生存部分拾取的地面余量正确且持久化；部分拾取无音效与原版一致，保持不改。
- Part1按用户决定移除Actor::isCreative覆盖及其scope，创造模式容量不足时按原版丢弃余量；保留生存add返回语义与修正后的拾取日志。
- Part2完成owner→parent链最近槽位collection查找，有限日志记录每级control名称/collection/index，保持歧义拒绝和world清理。
  Mojang模板指出grid/item上下文，实际祖先名称/层级需新ancestor日志确认；热栏/背包/箱子绘制尚未实机验收。其他A1点仍待续。

## 后续队列（按顺序）

1. **材质回归**：保留Timeless/block_17/HK位置精度已通过结果，不重开alpha/mip/depth试错；RTX稳定性回归；保留HUD/准星/命中反馈；击杀计数 Not yet。
2. **物品验收**：0.3.25完整槽位身份在热栏/背包/箱子真实像素与日志、动态真实弹数、部分拾取即时同步与守恒、满包give掉落；
   **A1待续**：全路径调用图/typed适配和AmmoId上限统一；本轮先测拾取政策/图标，记录超过包上限的具体操作。
   语言切换按用户安排后续测试；特殊UI/拖拽预览可能仍占位，超64声明降到64；第三人称/掉落物真实外观Not yet。
3. **机制**：手动拉栓优先（AWP 等）→ 背包直读+热量（minigun 当前默认30发）→ 充能 → 爆炸 → 近战 → builtin attachments（AUG 默认镜）。
   弹丸散布/每发多弹丸已有基础实现；第三方自定义 shoot/bolt Lua API、扩展字段按审计清单逐项实现，不无条件放行。
4. **效果**：抛壳 → 枪口焰 → 曳光（间隔判定、发光束、第一人称枪口偏移/近处隐藏）及专用 ammo entity 模型。
5. **皮肤**：复制真实像素到自有纹理 → Persona/动态皮肤边界 → 原生手臂实验，不引入不安全 opaque/by-value hook。
6. **后续里程碑**：配件/改装 → 枪械工作台 → 第三人称 → Arcana 扩展兼容。
    **手持/掉落外观待办（本版不实现）**：TACZ ammo在背包可用2D `slot`，手持／第三人称／地面则用display的3D `model`/`texture`；
    后续按视角选择，不将本轮槽图修复误当这些3D外观已完成。
    当前不解密 Vanguard/taczpack.dat；不把第三方包“加载成功”当作玩法/脚本完整兼容。
7. **服务端工作最后（用户决定）**：BDS+客户端实测联调、枪包data同步/一致性/安全、只装服务端提示。
   不在前述客户端/玩法队列中提前插入；当前仅按每版要求维护双端可编译。

逐枪状态与复测优先清单：`GUNS-0.3.19.md`。原因、Gson 证据与决策：`MULTIGUN-0.3.19.md`。
