# 路线图（0.3.19，逐版更新）

## 对照原里程碑 1「AK47 单枪可玩原型」

原计划：`C:/Users/MRUIAW/.claude/plans/levilamina-https-docs-levimc-org-levila-mighty-engelbart.md`。

- **已落地并部分实机验收**：原格式枪包读取、Lua 换弹/动画、输入与基础网络、弹药/射击模式、子弹运动/命中/伤害/爆头、
  音效、第一人称模型/手臂、后坐力、机瞄、camera option/冲刺规则。0.3.18 用户确认抖动几乎消失、RTX 内部动画正常，原生 CPU 大幅下降。
- **0.3.19**：功能额外弹匣、open-bolt 显示、手枪收枪 nil-track、隐藏占位弹体、Gson 兼容、枪械列表/资产诊断、325 枪离线审计。
  构建/单测不代替多枪实机验收；游戏运行中，部署待办。
- **里程碑1仍未完整**：正式弹药 HUD/准星/命中反馈；真实枪/弹药物品、GunId 图标名称；手动拉栓；
  BDS+客户端联调、data同步、只装服务端提示。当前为单人/内置服务器原型，probe UI 和 `/tacz setgun` 仍是测试设施。
- **已知视觉待办**：B93R 原动画弹匣离屏、HK416D 静态白字深度冲突、P320 等检视倾斜、View Bobbing 瞄准行走左右摆。
  皮肤/Persona 仍有占位路径，不把“手臂能动”等同皮肤完成。

## 后续队列（按顺序）

1. **UI**：弹药 HUD → 准星 → 命中/爆头/击杀反馈。
2. **物品**：BP 注册 `tacz:modern_kinetic_gun`（及 ammo/attachment），NBT GunId 驱动图标/名称；验证基岩端可行性，失败再讨论退路。
3. **机制**：手动拉栓优先（AWP 等）→ 背包直读+热量（minigun 当前默认30发）→ 充能 → 爆炸 → 近战 → builtin attachments（AUG 默认镜）。
   弹丸散布/每发多弹丸已有基础实现；第三方自定义 shoot/bolt Lua API、扩展字段按审计清单逐项实现，不无条件放行。
4. **效果**：抛壳 → 枪口焰 → 曳光（间隔判定、发光束、第一人称枪口偏移/近处隐藏）及专用 ammo entity 模型。
5. **皮肤**：复制真实像素到自有纹理 → Persona/动态皮肤边界 → 原生手臂实验，不引入不安全 opaque/by-value hook。
6. **后续里程碑**：配件/改装 → 枪械工作台 → 第三人称 → BDS+data同步/一致性/安全 → server-only warning → Arcana 扩展兼容。
   当前不解密 Vanguard/taczpack.dat；不把第三方包“加载成功”当作玩法/脚本完整兼容。

逐枪状态与复测优先清单：`GUNS-0.3.19.md`。原因、Gson 证据与决策：`MULTIGUN-0.3.19.md`。
