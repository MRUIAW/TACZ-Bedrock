# 路线图（0.3.21，逐版更新）

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
- **里程碑1仍未完整**：UI/透明材质实机验收；真实枪/弹药物品、GunId 图标名称；手动拉栓；
  BDS+客户端联调、data同步、只装服务端提示、击杀计数overlay。当前为单人/内置服务器原型，probe UI 默认关闭，`/tacz setgun` 仍是测试设施。
- **已知视觉待办**：Timeless_50 slide pattern、HK416D白字（alpha A/B失败，UV矩形一致；实际sampler/GPU深度仍待证）、B93R 原动画弹匣离屏、P320 等检视倾斜、View Bobbing 瞄准行走左右摆。
  皮肤/Persona 仍有占位路径，不把“手臂能动”等同皮肤完成。

## 后续队列（按顺序）

1. **UI/材质验收**：HUD同比例截图、Timeless/HK416D sampler实验与实际GPU采样/深度定位；保留已通过准星/命中反馈；击杀计数 Not yet。
2. **物品**：BP 注册 `tacz:modern_kinetic_gun`（及 ammo/attachment），NBT GunId 驱动图标/名称；验证基岩端可行性，失败再讨论退路。
3. **机制**：手动拉栓优先（AWP 等）→ 背包直读+热量（minigun 当前默认30发）→ 充能 → 爆炸 → 近战 → builtin attachments（AUG 默认镜）。
   弹丸散布/每发多弹丸已有基础实现；第三方自定义 shoot/bolt Lua API、扩展字段按审计清单逐项实现，不无条件放行。
4. **效果**：抛壳 → 枪口焰 → 曳光（间隔判定、发光束、第一人称枪口偏移/近处隐藏）及专用 ammo entity 模型。
5. **皮肤**：复制真实像素到自有纹理 → Persona/动态皮肤边界 → 原生手臂实验，不引入不安全 opaque/by-value hook。
6. **后续里程碑**：配件/改装 → 枪械工作台 → 第三人称 → Arcana 扩展兼容。
    当前不解密 Vanguard/taczpack.dat；不把第三方包“加载成功”当作玩法/脚本完整兼容。
7. **服务端工作最后（用户决定）**：BDS+客户端实测联调、枪包data同步/一致性/安全、只装服务端提示。
   不在前述客户端/玩法队列中提前插入；当前仅按每版要求维护双端可编译。

逐枪状态与复测优先清单：`GUNS-0.3.19.md`。原因、Gson 证据与决策：`MULTIGUN-0.3.19.md`。
