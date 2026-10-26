# 0.3.21：HUD 坐标/字体、030/031、贴花采样

## 0.3.20 用户结果

- `7fce9c2` 已部署；准星、身体/头/击杀标记与声音：用户验收 OK，保留。
- percent 和数字混用符合 display `ammo_count_style`，不统一改成数字。
  本轮重新跑 production loader：325/325 loaded；18 枪 / 18 display 为 percent：
  twx **13**、cyber_armorer 2（mantis_blade/maxtac）、ccrp 1（mp9_thunder）、sfms 2（s10exodus/red）。
  规划清单写“14 twx”与总数18不相容；当前安装数据实际是13 twx，18总数正确。
- 用户明确：Timeless slide pattern、HK416D 白字仍错，blend0/1 无可见差异。
  alpha 阈值路线没有修复该现象；不继续以强制 blend 或再改 alpha 宣称解决。
- BDS 实测、枪包 data 同步、server-only warning：按用户决定放在路线图最后。双端编译不等于开展 BDS 联调。

## HUD：完整读取 GunHudOverlay（包括缓存逻辑）后对照

参考 `_reference/TACZ/src/main/java/com/tacz/guns/client/gui/overlay/GunHudOverlay.java`：
current/reserve `81–118`，布局/字号/版本/图标 `120–176`，50ms缓存/creative/dummy/弹药箱 `179–219`。

- 本版将位置提取为纯函数 `model::hud::layout(w,h,fontWidth)`，对应 Java pose scale 后的屏幕坐标：
  竖线 `(w-75,h-43)..(w-74,h-25)`；current `(w-70,h-43)`；reserve `(w-68+1.5*fontWidth,h-43)`；
  version `(w-70,h-29)`；gun `(w-117,h-44)` 39×13；mode `(int(w-68.5+1.5*fontWidth),h-38)` 10×10。
  保留 Java version/mode 的整数截断；不同宽度/分辨率有单测。
- **一个 Java GUI unit 对应一个 Bedrock UI unit**：w/h取 `GuiData::mScreenSizeData.clientUIScreenSize`，
  不是 raw pixel/clientScreenSize；原生 UI 在提交时应用 `mGuiScale`，这里不再乘一次 scale。
  比较截图时应匹配实际 GUI 缩放/像素尺寸，不能只比较两个游戏设置菜单上的缩放标签。
- 0.3.20 请求 `getFontFromFontType("default").getFont()`，会经 FontHandle 的字体 override 解析，不能保证
  是原始 Minecraft bitmap。0.3.21 从游戏 FontRepository 原始 default 字体取 shared_ptr，必要时用
  handle retained default；不改全局字体/粗体偏好，不复制 Java 字体贴图。每帧仅借用，native flush 后不持久缓存。
  将数字 advance 规范到6 GUI units，再乘 current1.5 / reserve.8 / version.5，避免 native/UI font metrics 改变
  与39×13图标的比例。宽度测量和绘制使用同一字体、同一规范尺度；具体像素边缘仍需同GUI尺度实机对图。
- 恢复小灰色**带阴影**版本行，当前/备弹无阴影。格式为
  `<SharedConstants Major.Minor.Patch>-<TACZ_VERSION>`：本版本目标游戏显示 **`1.26.51-0.3.21`**，
  不含 Launcher 名称或 `.01` build revision，不使用可能被兼容协议简化的 network version。
- 严格低弹红 `FF5555`；背包直读黄 `FFFF55`，inventory+dummy 青 `55FFFF`；非inventory dummy备弹青，
  其他备弹 `AAAAAA`。读到原 TACZ `DummyAmmo` Int NBT 时只做 HUD 读取，不实现/伪造 dummy 供弹机制。
  overheat优先级有纯测试，但运行时未实现热量机制，不画热量条、不伪造过热。
- 延续正确AmmoId背包50ms缓存、creative9999、infinite∞、inventory不画备弹、空弹hud_empty/无图染红。
  不复刻 Java 第一帧使用过期缓存或客户端修改 ammo NBT 的副作用。配件容量/弹药箱机制仍未实现。
- `/tacz rdebug hud`：下一次持枪 HUD 在 client log 打一行 `[hud-layout]`，包括真实 magazine、rawChamber、bolt、
  有效弹量/最终文字、percent、GUI宽高/scale、bitmap资源路径、digit advance/fontUnit/wrapHeight/nativeScale和版本。
  默认不增加屏幕调试文字；仅内置服务器命令可直接控制同进程 client。

## 030/031：未发现丢膛内弹的 HUD 算法错误

当前生产审计中 AK47 `closed_bolt`、capacity30，初始 magazine30/rawChamber=true/effectiveChamber=true。
`nbt::freshGun` 使用 `gun::initialAmmo`；0.3.20 HUD 已调用同一有效chamber规则，30+true会显示031。
新测试覆盖 initialAmmo→HUD：closed/manual031、open030；30/false 或29/true都应030。
**截图没有对应帧的原始 NBT，不能声称截图实际上有膛内弹，也不能补一个无条件+1。**
本版保留计数算法，请在fresh setgun、不射击时执行hud helper：若client log是30/true/closed却text030，才是新的明确回归；
若是29/true或30/false，则030是实际状态（后者需再调查状态来源，不应靠HUD造弹）。

## 逐面 UV 与 texel 矩形：Java 和本端相同

参考：`BedrockCubePerFace.java:60–69`、`BedrockPolygon.java:10–17`、`FaceUVsItem.java:31–45`；
本端 `src/model/GeoModel.cpp:49–90,119–130`。明确结果：

1. 分母是 geometry description `texture_width/height`，**不是实际PNG宽高**。
2. `u2=u+uv_size.x`，`v2=v+uv_size.y`；角序 `(u2,v1),(u1,v1),(u1,v2),(u2,v2)`。
   negative size 保留反向，不取abs或排序。per-face没有半texel/1/16 UV偏移；Java f/f1实际为0。
   Java `/16`仅作用几何位置，不作用UV。
3. JSON up/down分别映射几何DOWN/UP（east/west也换向），这是两端相同的face路由，**没有额外UV反转**。

以下为 `(u1,v1)→(u2,v2)`，保留反向。每一行 Java / native **完全相同**；C++测试直接检查生产 `parseGeoModel` 的四角输出。

| 面 | description / PNG | normalized两端 | 实际PNG texel两端（不是整数裁剪框） |
|---|---|---|---|
| Timeless pattern/cube0/west |128² /256²| (0,.7890625)→(.533203125,1.005859375) | (0,202)→(136.5,257.5) |
| HK bone/cube0/west（序号） |256² /512²| (.84765625,.1776124984)→(.8966066241,.1876222640) | (434,90.937599)→(459.062592,96.062599) |
| HK bone/cube2/west（HK416文字） |256² /512²| (.8475341797,.2069091797)→(.8868408203,.2227783203) | (433.9375,105.9375)→(454.0625,114.0625) |
| HK bone/cube3/east（反向细字） |256² /512²| (.8966064453,.1405029297)→(.8436279297,.1524658203) | (459.0625,71.9375)→(431.9375,78.0625) |

HK这三面均在 `bone`（parent `gun_body2`）下。Timeless末端V超1；HK这些白字面不越界，单独的wrap/clamp
差异不足以解释三处HK字样。未修改UV归一化、up/down、negative size或枪包原文件。

## Sampler / mip：证据边界与本版可安全做到的部分

Java entityCutout 的 texture state 是 `(blur=false,mipmap=false)`，`AbstractTexture.setFilter(false,false)`
会设 min/mag为GL_NEAREST。SimpleTexture上传level0（prepareImage mipmax0），没有mcmeta时默认不clamp；
本轮目标PNG旁均没有 `.png.mcmeta`。参考mapped vanilla代码：

- <https://raw.githubusercontent.com/mahtomedi/minecraft/main/src/main/java/net/minecraft/client/renderer/RenderType.java>
- 同目录 `RenderStateShard.java` 的 TextureStateShard、`texture/AbstractTexture.java` 的 setFilter、
  `texture/SimpleTexture.java` 的 doLoad，以及 `com/mojang/blaze3d/platform/NativeImage.java` 的 _upload。

这些是vanilla公开mapped源码对照，不是基岩GPU实测。TACZ调用的render type已在其本地源码核对。
本端旧路径仅提供解码后的level0 ImageBuffer，**不能从这点或moving-block材质名称推断GPU一定用了mips/linear/clamp**。
安装SDK的 `mce::SamplerStateDescription` 是空placeholder，native array实际占96字节；不能按空struct索引或写字段。

本版新增安全、可退回的实验：

- `/tacz rdebug sampler 0`：已验证过的 ImageBuffer 上传，**默认0**。
- `sampler 1`：typed TextureContainer 上传单张level0，明确 `mMipCount=1` / AllowDroppingMips=0。
  不是point/linear或wrap/clamp切换！不伪造Opaque sampler布局，不突变共享material。
- `[gun-sampling]` 打印constructor初始mip count、requested=1、上传后metadata mip count、PNG和UVdescription尺寸/RTX状态。
  metadata不是GPUcapture；若0/1都是1且无差异，就没有证据支持“旧路径自动用了mips”。
- toggle仅在资源边界释放/重建自有cache；继续0.3.18默认path1不可变group meshes/骨骼矩阵，无每帧顶点重建。
  保留现有RTX/native材质选择和手臂处理；实验路线的RTX与菜单/世界切换仍需用户验收。

**结论：排除了CPU逐面UV矩形差异；没有证明原生filter/mip是贴花根因，因此没有宣称贴花已修复。**
这也不证明原生block shader内部的UV处理/导数或GPU深度与Java相同；需后续GPU证据，不能靠改分母补偿。
0.3.20 alpha A/B失败是实机证据；本轮offline模拟能说明采样差异会怎样改变文字，但不能替代原生sampler/GPU深度证据。

## 离线图片（只读枪包，不将第三方图片纳入仓库）

```
python scripts/analyze-decal-sampling.py <gunpacks> <outside-gunpacks-output>
```

已运行输出至 `C:/Users/MRUIAW/AppData/Local/Temp/opencode/sampling-0.3.21/`。
含4张crop、每面5张48×12采样control、report.json，以及放大拼图 `sampling-controls.png`，已在Review展示。
从上往下：Timeless、HK cube0、HK cube2、HK cube3；从左往右：
Java point/repeat、本端UV point/repeat控制、point/clamp假设、linear/clamp假设、mip2+linear/repeat假设。
前两列逐像素一致；后三列用于看风险（尤其小字平均模糊），不是原生GPU效果截图，棋盘格也不是枪身背景。
linear/mip列是byte-RGBA插值/box平均的示意，不模拟sRGB解码、GPU导数/LOD、各向异性或实际投影。
标准库PNG解码/写图，不需要Pillow；拒绝把输出写入安装gunpacks。

## 回归

1. 同实际GUI缩放对比AK47 Java/基岩截图：字形、031、灰9999、版本阴影、39×13枪图标、1×18竖线和10×10模式图。
2. fresh `/tacz setgun tacz:ak47`不射击，`/tacz rdebug hud`保存日志；open-bolt m249空弹无+1；射一发后合计030正确。
3. percent枪（twx等）仍显示百分比；备弹/creative/infinite、空弹hud_empty/染红、低弹边界继续正常。
4. Timeless/HK同角度 sampler0/1，先固定blend0排除mask分支，再恢复blend1；记录`[gun-sampling]`与截图。
   无差异也是有效结果，不应无限调alpha/挪UV；恢复 sampler0。
5. RTX开关与菜单/世界切换、准星和身体/头/kill声音回归。BDS联调按用户决定最后做。

构建/单测不能证明HUD像素完全一致或贴花根因；本版已实现HUD对齐和采样诊断，视觉验收待用户。
