# 0.3.19 — 多枪修复、Gson 兼容、离线审计

父提交 `5aac463`。用户已验收 0.3.18：运动抖动/闪烁几乎完全消失，RTX 下枪械内部动画正常，未发现新路径问题。
本版没有新实机验收；游戏仍在运行，**不部署、不关闭游戏**。

## 实机 CPU 证据（用户日志，不是离线基准）

全部是 `[gun-perf] N=120 steady` 的 CPU 提交时间，非 GPU 时间；包含枪和手臂。

| 日志 / 行 | path / RTX | mean / max ms | prepare / draw mean ms | 枪 / 手臂提交 |
|---|---|---:|---:|---:|
| `2026-10-26.2.log:276` | 0 / off | **4.116 / 5.038** | 3.911 / .188 | 1 / 4 |
| `.2.log:289` | 1 / off | **.141 / .486** | .038 / .092 | 29 / 4 |
| `.2.log:427` | 1 / on | .158 / .284 | .026 / .114 | 29 / 4 |
| `.3.log:149` | 1 / on | .163 / .853 | .042 / .109 | 29 / 4 |
| 最新 `.7.log:180` | 1 / on | **.117 / .363** | .027 / .076 | **21 / 4** |

完整目录为 `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs/`。
最新 `.7.log` 没有 path 0；A/B 来自前一轮 `.2.log`（同为 0.3.18）。不能把不同枪/不同时刻的 21 次和 29 次当作同条件比较。
`.7.log:252` 的暖缓存 `[gun-render]` 为 33 次枪提交、CPU .157 mean / .291 max、builds=0/misses=0；
`.7.log:189,245` 的 .204/.236 ms 包含首次 27/42 个 mesh 构建，max 为 14.883/23.151 ms，不是纯暖缓存成本。

## 部分工作复核

保留已有的 15 文件工作：空子弹几何、按 bolt 类型显示 +1、额外弹匣虚拟骨实例及 path 0 对照、共享手臂帧适配、初始弹药纯函数和等价性测试更新。
纠正其中“把 open bolt 的原始 chamber 位清掉”的做法：**TACZ 创造栏本就写 true，实际耗弹与显示必须忽略它**，不更改原 NBT 语义。
补完缺失的 Lua nil 轨道兼容、JSON、资产诊断/命令、审计与测试。

## 原因与处理

### 换弹弹匣

`BedrockGunModel.java:221–236` 的 `renderAdditionalMagazine`：显示自身 cubes/children 后，再调用
`magazineNode.compile`，并绘制其 children。**不应用原 magazine 节点自己的 pivot/动画/scale**，也不带入其旧父链。
`ModelAdditionalMagazineListener` 只有在 channel 更新时打开显示，`cleanAnimationTransform` 每帧清掉该标志。

0.3.18 只做了 additional 节点显示，未实例化 magazine 子树。现在在编译阶段生成额外的不可变 draw runs，
帧阶段以 additional 的矩阵为 magazine 根，子节点仍使用原动画、可见性规则；不求逆（原匣链可为零缩放）。
path 0 的参考 bake 也执行同样的功能绘制，维持 A/B 公平；普通 `bake` 默认不启用枪械功能行为。
命名骨/DFS 顺序/visibility 边界仍保留。AK47 resident groups 43→48，静止仍 29 次，不给额外实例无条件开显示。

用户报告的 m4a1、glock_17、hk_mp5a5、deagle_golden、cz75、deagle、fn_fal、g36k、hk_mk23、m9a4
均由真实资源回归覆盖。**B93R 是例外**：原始 reload/inspect 不 key additional；reload 中
`mag_and_lefthand` 的 position 把原匣移到画面外，另有 `mags` 动画 channel 而模型无同名骨。
没有证据证明应在哪个时间点补画/删除哪条曲线，故不篡改包；需与 Java 相同片段逐帧对照。
单纯名字包含 mag 的骨不特殊隐藏；仍按完整父链变换。

### Open bolt 的 0+1

`AbstractGunItem.fillItemCategory:340–346` + `GunItemBuilder:104–109` 给所有枪设置满匣和原始 chamber=true。
`ModernKineticGunScriptAPI.reduceAmmoOnce:238,277–289` 对 open_bolt 忽略 chamber，只有匣内 ammo 可消费。
现有 `availableRounds/prepareShot/reduceAmmoOnce` 已符合这一点；问题在 probe HUD/命令/回执把 raw bit 直接打印成 +1。
现在统一以 `hasChamberedRound(bolt, raw)` 显示，旧物品无需清 NBT；闭膛/手动栓仍保留 +1。
纯测验证 open bolt 满匣→打空→拒绝额外发射，raw=true 也不增加有效弹药。

### Glock / Deagle 收枪

真实文件都有 `put_away`：Glock .4 s / data .35 s；Deagle .4333 s / data .33 s，pistol fallback 合并正常。
原脚本分别在 `glock_17_state_machine.lua:101`、`deagle_state_machine.lua:93`
调用 `getTrack(STATIC_TRACK_LINE, SWITCH_MODE_TRACK)`，但 SWITCH_MODE_TRACK 未定义。
用户 `.3.log:765–768` 明确出现 sol2 `expected number, received nil`，导致整个 VM fallback 到 static_idle。
`getTrack` 现在把 nil 可选下标视为不存在轨道（-1）；后续 stopAnimation 为 no-op，然后原脚本继续
tail-seek/HOLD 收枪。只放宽这个查找，不把其它错误/未知 API 一律吞掉；不新增假轨道，不修改枪包 Lua。
真实两把手枪的原脚本已测试 trigger、seek、exit 后 continued controller update。

### 子弹贴脸 / 特效

删除 RP 子弹的黄方块 cubes，保留行为实体、服务端运动/命中/伤害/同步。
`EntityBulletRenderer.java:55–73` 只有 ammo display 同时提供 entity 模型和贴图才画普通弹；大多数普通弹无 entity。
目前所有普通弹都不画，**有 entity 的榴弹/火箭等也暂不支持专属模型**；审计逐枪列出该声明。
未来按 ammo display 画真实飞行物模型，不恢复通用黄方块。
tracer 不是普通粒子：TACZ 使用拉伸的发光 beam mesh / energySwirl，每 `tracer_count_interval` 判定，
第一人称有 muzzleRenderOffset 衰减，前 5 tick 内距眼 ≤2 格不画。需要同步判定和渲染接入，留到特效阶段。
枪口焰、弹壳、命中效果分别按各自资源/渲染实现，不把所有效果混成粒子。

### HK416D 白字、P320 检视：待对照，不盲修

HK416D 使用 `hk416a5_geo`；illuminated 命名仅是 sight/sight2。Java 的 BedrockPart/FunctionalBedrockPart
illuminated 分支**只设置最大 packed light**，没有专属 polygon offset / depth func / 后绘制 decal pass。
源文件有零厚度 handguard 面及大量 .001 pixel 级薄面（gun_body2、rail2/3 等）。审计另统计静止时完整四角重合面，
HK416D 有 17 个薄 cube 候选、75 对完整重合 quad（包含反向面，未证明都产生深度冲突），
但不能据此认定用户白字具体是哪张面或 GPU packing/depth 根因。没有图片/Java 同帧证据，不挪面、不改原生共享材质。

P320 inspect 确实 key root、camera、constraint、手臂等旋转。快速复核 `MathUtil.toQuaternion` 和 BedrockPart 的
rest Rz/Ry/Rx + additionalQuaternion 顺序与当前实现一致；约束 Java 也按 ADS progress 加权。
0.3.18 的 bake 等价性不能证明整个 Java 相机视觉完全一致，尚无明确错误可安全修；保留报告，不用经验角度校正。

## Gson 证据与边界

用 Maven Gson **2.10.1** JAR 和仓库 `scripts/GsonProbe.java` 实际运行；JAR 只在临时目录，不入库。
来源 `https://repo.maven.apache.org/maven2/com/google/code/gson/gson/2.10.1/gson-2.10.1.jar`，SHA-256
`4241C14A7727C34FEEA6507EC801318A3D4A90F070E4525681079FB94EE4C593`。
`LazyJsonDataManager:148` 使用 lenient reader 得到 JsonElement，随后 `JsonDataManager:70` / Lazy:112
`gson.fromJson(element, POJO)`，所以以 **tree adapter** 结果为准，不错误套用 streaming-only coercion。

| 输入 | Gson 实测 / 本版采用 |
|---|---|
| `["laser",\n ]` | `["laser",null]`；保留 null，不是删除逗号 |
| `"base": +0.15` | 树值为字符串 `"+0.15"`，浮点字段转换 .15 |
| `"show_crosshair":"true"` / `"FALSE"` | true / false；其它字符串按 Boolean.parseBoolean 为 false |
| `show_crosshair:1` | 字段类型错误，拒绝 |
| `{unquoted:'value'}` | 接受未引号 key/value、单引号字符串 |
| `{x=+1;y:TRUE}` / `[,1,,]` | 接受 `=`/`;`、大小写关键字、缺失 array 元素为 null |
| `{"x":1,}` | 对象尾逗号拒绝 |
| 整数字段 `"2.0"` / `" 2 "` / `"2.5"` | tree adapter 拒绝；stream adapter 部分会接受，不能混用 |
| 整数字段 `2.5` | tree number.intValue 为 2 |
| 字符串字段 `42` / bool | 对应 primitive 文本；无效数值字符串不作为 0 接受 |

实现采用有深度上限的词法/递归 reader，不以全局正则替换字符串内容；常规 JSON/注释走原快路径。
保留未知 enum→null、错误类型失败的 TACZ 规则；null attachment enum 保留可选项，不将它当字符串 "null"。
这不增加新 gameplay API/Arcana 支持，也不承诺 Gson 的任意自定义 adapter/极端超范围数值位级完全相同。

## 列表、诊断、审计

- `/tacz guns [namespace]`：排序、约180字符分块，聊天和日志均列 ID；不存在 namespace 返回 count=0。
- `/tacz packs`：逐物理包列实际 namespace、已载枪数、缺展示资源的枪数。
- `[gun-assets] <id> missing: ...` 启动时每枪汇总一行：model/texture/animation/state machine、显式/默认/片段音效。
  缺可选音效记 info，不让整枪加载失败；所有 325 枪的核心展示资产本次均存在，可选缺音效不等于粉纹理。
- `scripts/audit-gunpacks.py` 调用独立 `tacz-pack-audit`，使用生产 parser/validator/model/animation compiler，
  中间 metadata 只写批准的临时目录。`docs/GUNS-0.3.19.md` 是重跑生成的逐枪表和逐包计数。
  325 index、325 data/display/geometry 成功；不执行第三方 Lua、不接触已移除的 Vanguard 加密文件。
- `classicr:tec_9` 另有两条 `Molang is not supported: -` 动画值警告，相关值被当前 parser 跳过；
  文件存在/枪械可加载不等于该动画完全受支持，已保留到逐枪审计。
- 未识别字段/API 只是相对当前 upstream/bridge 的静态差异；不能全部称为 Arcana 特有，不能将“可加载”等同功能完整。

## Decisions for the user

1. 保持 TACZ 原始 NBT 和耗弹语义：open bolt 的 raw true 保留，所有有效膛弹/显示分支忽略它。
2. 额外匣是真实 magazine 子树实例，保留零缩放与命名骨边界；不靠猜时间点重建顶点或复制错误父链。
3. nil 的可选 track lookup 为不存在，不伪造轨道、不编辑第三方 Lua；其它脚本错误仍显式 fallback/log。
4. B93R 换弹、HK416D 白字、P320 倾斜无可靠安全修复证据，明确待验收；不以挪面/固定角度掩盖问题。
5. 普通子弹不可见；专用弹体模型和 tracer 单独排队。粒子不是所有 TACZ 特效的共同实现。
6. 只做已验证的 Gson 语法/字段适配，不解密、不安装 Arcana；未知扩展列清单，不假称兼容。
7. 玩家正在游戏中：只构建/测试/提交；关闭游戏后再部署整套 DLL/PDB/manifest/RP/BP。

## 验证

- Server / client Windows x64 release 构建均通过，最终 `bin/tacz/manifest.json` 为 **0.3.19 / client**。
- 全量 **143 tests / 77,719 assertions** 通过，0 failures / 0 skipped；日志中的两条 sol2 异常是原有负向测试。
- 真实 AK47 功能实例后仍通过 35 clips / 2,310 poses / 17,073,424 vertices 比较；最大位置误差
  4.76837e-7 方块，可逆法线 2.38419e-7，所有位置/UV 均测试。13 把默认枪（含 AK47）的 reload/inspect 资源回归通过。
- 生产加载器独立审计 **325/325 data + display + geometry** 成功；80 ammo / 36 logic scripts。
- 已检查 diff 空白错误；没有新 native hook。只保留现有 hook-macro / 测试 u8path 警告。
- **未部署**，未启动部署脚本、未停止/修改运行中的游戏。关闭游戏后再运行 `scripts/deploy-client.sh`，
  必须同时替换 RP（黄方块移除在资源包中），不能只换 DLL。
