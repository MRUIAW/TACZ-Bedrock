# 0.3.20：透明度证据、HUD 与命中反馈

## 基线与续接审查

基于 `91e4d2b`（0.3.19），续接已有未提交的渲染/HUD/网络/PNG/测试实现，没有丢弃或重做。
保留 display 驱动材质、刚性 mesh-once 路径、UI 绘制、服务端反馈包和纯规则测试。
续接修正：xmake PNG 必须 `encoding="binary"`（原文本读取会转码，26 张图全损坏）；
HUD 缓存补齐 buffer/frame-builder/lighting 身份；仅当前 client 可绘制；命中 handler 禁用门控、
队列限长、跨世界异步声音失效；原版准星在动画快照尚未发布时也按实际持枪隐藏。
百分比先 float 除法再 DecimalFormat 的细节用真实 Java 17 验证：`1/40 -> 003%`，不是精确有理数的 `002%`。

用户已经验收 0.3.19：m4a1/glock_17/deagle 换弹弹匣、glock/deagle 收枪、b93r 不再脸前弹体、
open bolt 单数字，以及 `ccrp:sr_3m`、`sfms:mgl416`、`cyber_armorer:g58_dian`、`classicr:ak12`、`twx:twx_b3`。
未收到 HK416D 白字、P320 inspect、`/tacz guns`、`/tacz packs` 的验收结果；不能推定通过。
0.3.19 已后续部署成功，原 CHANGELOG 的当时“游戏运行中待部署”不是现在的状态。

## TACZ 实际规则：规划前提有误

以下路径均相对 `_reference/TACZ/src/main/java/com/tacz/guns/`：

- `client/renderer/item/GunItemRendererWrapper.java:217–220`：`enable_transparency=true`
  才对整枪用 `RenderType.entityTranslucent(texture)`，否则 `entityCutout(texture)`，不是按贴花像素自动混合。
- `client/resource/pojo/display/gun/GunDisplay.java:105–106`：boolean 默认 false。
  安装的 `ccrp:timeless_50_display`、`tacz:hk416d_display` 都**省略该字段**，Java 两者均走 cutout。
- `client/model/bedrock/BedrockPart.java:61–75`：本节点 cube 后递归 children；illuminated
  只改 packed light 为 15/15 并传给子节点，没有专用混合、polygon offset 或后绘制 decal pass。
  本版保留已有 full-bright/手臂材质处理，不通过挪面或共享材质突变掩盖问题。
- `client/resource/pojo/model/FaceItem.java:8–12` 只解析 uv/uv_size；
  `client/model/bedrock/BedrockCubePerFace.java:60–69` 不消费 uv_rotation。继续忽略，与该版 TACZ 一致。
- Java 1.20.1 `rendertype_entity_cutout.fsh` 明确 discard sampled alpha < .1：
  <https://raw.githubusercontent.com/InventivetalentDev/minecraft-assets/1.20.1/assets/minecraft/shaders/core/rendertype_entity_cutout.fsh>。

## 安装枪包只读 alpha 审计

复跑：`python scripts/analyze-gun-alpha.py <gunpacks-directory>`（标准库，无 PIL，不修改文件）。
统计是原始 PNG footprint，负/越界 UV 按 repeat 统计；不是 GPU 过滤后 alpha，也不是证明原生 sampler 为 repeat。

| display | Java 透明开关 | 模型 UV / PNG | per-face | uv_rotation | 含 fractional-alpha 的面 |
|---|---|---|---:|---:|---:|
| ccrp:timeless_50_display | false | 128×128 / 256×256 | 1,358 | 20 | 3 |
| tacz:hk416d_display | false | 256×256 / 512×512 | 4,224 | 0 | 0 |

Timeless 的 model/texture 分别为 `ccrp:gun/timeless_50_long_geo`、`ccrp:gun/uv/timeless_50`。
`pattern` 的 parent=`slide_long_illuminated`；cube origin `[-.64,8.875,-.4925]`、size `[1.28,1,2.555]`，
唯一 west 面 uv `[0,101]`、size `[68.25,27.75]`，确认是小面上的大 UV 区域。
该 footprint 有 alpha0 **3,899**、alpha255 **2,187**、fractional **1,586** texel；
alpha104–106 合计仅 **10**。常见不透明颜色包括 `(153,153,153,255)` 1,157 次、白色 760 次。
不是“只有约 .41 的灰色透明雕花”。另两处 fractional 来自隐藏 righthand_pos 占位面。
V 末端 `128.75/128 > 1`，wrap/clamp/过滤边缘仍有风险。UV 按 geometry 尺寸归一化是正确的，不能改成 PNG 尺寸。

HK416D 没有 fractional-alpha face，不能把它的白字归因于相同的半透明混合差异。
0.3.19 的薄面/重合面证据仍成立，但没有定位到用户所见白字的同帧 Java/GPU 深度根因。

## 原生实现与不能声称已完成的部分

- 默认保留 material6 moving-block alphatest；声明透明时整枪切换 material7，取当前 native
  `MovingBlockActorRenderer::mBlockMaterials[BlockRenderLayer::RenderlayerBlend]`（layer3）。
  不改共享材质，不增加不安全 hook。所有透明 display 的 group 使用同一 blend 材质，不按 alpha 擅自改变 Java 的开关。
- 只在资源创建时分类 UV footprint（含保守一像素过滤边框），先 fully-opaque groups，后 cutout/fractional groups，
  每阶段保持原 DFS 顺序。默认 path1 使用已有不可变 group mesh + bone matrix，无每帧顶点重建/上传；arms 不受此开关影响。
  这不是逐三角形 back-to-front 排序，也不等价保证 Java translucent buffer 的排序；path0 仍是旧单 mesh 诊断路径。
- 对 cutout 增加另一张缓存纹理：alpha `<26 -> 0`、`>=26 -> 255`，RGB 不变，
  使原生 alpha-test 尽量保留 Java .1 阈值接受的颜色。**这是兼容近似**：预处理后过滤不等于过滤后 discard，
  原生 cutoff/sampler 未证明相同；不能据此断言 Timeless 已修好。
- `/tacz rdebug blend 0`：原图/旧默认 alpha 行为；`1`：display 透明路由 + cutout mask（默认）。
  两支都是 Java cutout 的 Timeless/HK416D 在此 A/B 中**不会自动变为混合枪**。
  强制实验：`/tacz rdebug 7 0`；恢复 `/tacz rdebug 6 0`。这些调试开关仅内置服务器进程有效。
- RTX/Deferred 复用当前游戏管线提供的 moving-block layer 材质；lighting/buffer/frame-builder 身份变化释放重建缓存。
  没有证据要求独立 RTX 材质，也没有实机证据证明此 blend 在 RTX 正确。
  `[gun-alpha-state]`、`[gun-depth]`、`[gun-blend-descriptor]` 记录所选路径/legacy 深度和混合描述；
  RenderDragon GPU 状态可能覆盖它们。没有改 depth test/write 去伪造 Java 等价；该项及 RTX 透明验收**仍待实机**。
  blend material 缺失时明确警告并退到 cutout，不把退路叫作透明兼容。

## HUD、准星、命中反馈

- `src-client/client/ui/GunHud.cpp` 使用 Lamium 同类 `AfterUIRenderEvent` + `MinecraftUIRenderContext`
  drawText/drawImage/flushImages；仅 hud_screen 和当前 joined client。world=0 时先返回，不查 native 世界服务。
  字体遵循基岩默认字体；按 TACZ bottom-right GUI 单位、1.5/.8 字号、39×13 HUD 与10×10 fire-mode图。
- 背包匹配 AmmoId，每50ms缓存；creative=9999、infinite=∞、inventory feed 隐藏 reserve。
  有效弹量=弹匣+closed/manual chamber，open忽略raw chamber；正常格式 `031`（不是字面 `030+1`），
  低弹严格 `ammo < capacity*.25 && ammo < 10`，percent=`000%`。
- `hud_empty` 优先，否则空弹 HUD 图标染红。缺图跳过并日志，不画 missing-texture 棋盘格。
  不实现 Java 的版本小字、dummy ammo、ammo box、配件容量加成；inventory-fed HUD 读取背包不代表供弹机制已补齐；不写回枪 NBT。
  没有热量机制，不画热量条。
- `mods/tacz/config/hud.json` 的 `crosshair_type`，默认 TACZ `dot_1`，支持22种命名；非法值回退。
  重启加载；第三人称/HUD隐藏/换弹/animation hide/收枪不显示。ADS>.9 隐藏，display `show_crosshair=true`
  可强制 ADS 显示，但不覆盖换弹/animation hide；false 不意味着隐藏 hip 准星。
  原版准星完全替换；没有 ShoulderSurfing/改装界面兼容（这些机制未实现）。
- `HitFeedbackPacket` 只由服务端 accepted damage 后发给 shooter，保留**发射时** GunId，
  head/kill 由服务端弹道/伤害结果决定，不是点击/客户端射线预测。kill判断用 isDead/!isAlive，避免整数血量截断误判。
  client线程播放 display 的 flesh_hit/head_hit/kill（kill替代hurt，不叠放）；跨世界任务失效，标记队列最多128条。
  300ms淡出，四象限gap4，只有kill扩大到8；最近300ms爆头红色，无另造kill颜色。
- 缓存绑定 world/client/UI texture-group/buffer/frame-builder/lighting；切换或退出释放自有纹理到原 owner。
  新UI绘制签名及原生准星hook均有编译期校验。声音沿用现有内存流资源策略，字节不在流播放中提前释放。
- `/tacz rdebug probe 0|1` 默认0，旧黄色probe行仅调试出现。击杀计数 overlay：**Not yet**。

## 用户决策与回归清单

默认按真实 Java display 保留 cutout，不自动把 Timeless/HK416D 强制 blend，也不改枪包/uv_rotation/深度偏移。
请先对照下列 A/B 结果再决定是否继续做 sampler/depth 的 GPU 级调查；如果只有强制7正确，也应记录为偏离 Java 的实验，非默认修复证据。

1. `/tacz setgun ccrp:timeless_50`：静止/移动/开火/换弹看 slide pattern，blend0/1同视角截图；观察是否有wrap条纹。
2. HK416D 白字同样 A/B；预期本次 alpha 改动不必改善它，不能把“仍闪”当作未开透明开关。
3. 生存用 `/tacz ammo 64`，确认正确AmmoId备弹随换弹/拾取变化；creative备弹9999；normal/percent枪各试一次。
4. m4a1/glock closed bolt验证弹匣+1合计；m249/aa12 open bolt空弹=000无+1；30容量7红8白、40容量9红10白。
5. hip默认dot，ADS与show_crosshair override、换弹/inspect hide、切枪；确认无双准星。hud.json改type后重启验证。
6. 对生物身体、头、最后一击：白/红标记、kill扩散与对应音；未造成accepted damage不应出现预测标记。
7. RTX开/关、切回raster、透明材质7实验（恢复6）、换枪/换世界/回菜单再进入；无粉图/残留UI/崩溃，日志重新bind。
8. `/tacz rdebug probe 1`出现旧行、0隐藏；复测0.3.19已验收枪械；仍待用户验证P320、guns/packs列表。

构建、测试与部署证据见本版 CHANGELOG；这些离线结果不替代以上实机验收。
