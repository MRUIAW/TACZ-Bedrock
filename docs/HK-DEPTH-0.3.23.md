# 0.3.23 A：HK416D 位置精度与坐标重基

## 实际日志：不能误报half已经确认

输入 `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs/2026-10-26.12.log`。
用户已确认0.3.22 (`0d1be46`) Timeless/block_17滑套贴花正确，cz75/glock_17弹匣、AK47及其他枪回归OK；HK白字仍闪。
枪包保持只读，UV修复仍在本模组的上传兼容层，不要求改任何包。

指定日志的1251–1301行有**13条帧统计**：`meshDiagnostics=true`、`builds=0`、RTX=true。
全文**没有**`[gun-packed]`/`positionSlotBytes`/packed position字段。
0.3.22的诊断仅在mesh构建时运行，用户开mesh1时mesh已经缓存。因此这些行不能证明当前后端是half还是float，
也不能证明inflate是否在这次运行的实际GPU缓冲中丢失。此处更正“那十余行是打包读数”的前提，不填造实际值。

日志另有`[gun-depth]`：moving_block_alpha、test=true、原始func=3、writeMask=1、bias=100、slopeBias=.1；
这些是既有legacy材质描述，不是本版加的bias，也不等于RenderDragon实际GPU状态。
SDK ComparisonFunc本身为空枚举，不能未经映射就把3宣称为某个GL/D3D比较函数；日志没有depth buffer格式，
cameraNear/Far也打印0，不能据此推算实际depth精度。Java的LEQUAL可在源码确认，但本轮未假造两端GPU状态等价。

原生1.26.51确有half4/float3位置编码分支（上一版二进制证据），但分支由后端能力决定。
本版修复诊断：`/tacz rdebug mesh 1`引发renderRevision变化后，**直接检查已缓存mesh**，不需要重建：

- `[gun-packed-cache]`：bone/name、position slot8或12字节、stride、split stream、rawBytes、补偿origin。
- raw还存在、stride/offset/size均通过边界校验时，打印`[gun-packed-position]`首4点的decoded local/restored local、half4/float3。
- raw若已被native清空，明说rawBytes=0，不调用generateHALData、不伪称GPU读回成功。诊断始终只读。
- 仍固定针对1.26.51已核实的110字节layout副本；不构造/突变SDK空VertexLayout，不在菜单查询世界资源。

## 有限的精度修复：等价坐标变换，不抬高贴花

在缺少实际格式读数时，本版采用**同时适用于float与half的坐标精度加固**，不是把half候选升级为已证实根因。
`src/model/LocalMeshFrame.*`仅对远离骨pivot、幅度大而跨度小的轴选局部origin：

1. `abs(center) > .125`，且 `abs(center) > 4*span`；origin取最近的1/16方块（二进制可精确表示）。
2. 逐点模拟half，只有新坐标的最坏还原误差下降超过一半，才启用该轴；其余origin=0保留原路径。
3. 上传缓存中的坐标 `p' = p-origin`；每次draw矩阵 `bone' = bone*T(origin)`。
   因此 `bone' * p' == bone * p`（实数等价），**没有加inflate、没有depth bias、没有改源模型**。
4. 一次上传时计算/缓存，正常帧只增加相应矩阵平移；源rigid group/hash、命名骨动画边界、DFS draw顺序、UV分面、手臂独立路径不改。
   CPU调试path0保留原语义；重点修复默认path1。

安装HK label仍是bone22 `bone`，选出origin **(-.375,0,0)**；其骨矩阵本身含+.375的平移，补偿后不再用-.34附近的half存小字位置。
body21保持origin0。选用float32自定义layout的方案因SDK空placeholder不安全，未采用。

### 生产模型数值（以下half均为离线模拟，不冒充日志实测）

单位为方块（模型像素已/16）。body与decal指实际投影范围重叠的平行面：

| cube | 原decal local X | 原decal世界X | body世界X | 旧half后decal世界X | 重基half后decal世界X | 重基half后间距 |
|---|---:|---:|---:|---:|---:|---:|
| 0 | -.33978125453 | .03521874547 | .03515625 | .03515625 | .03521728515625 | .00006103515625 |
| 2 | -.33196875453 | .04303124547 | .04296875 | .04296875 | .04302978515625 | .00006103515625 |
| 3 | -.41828000546 | -.04328000546 | -.04296875 | -.043212890625 | -.04327392578125 | -.00030517578125 |

cube0/2原间距 `.00006249547`（约.001像素），旧half模拟为0；重基后保留`.00006103515625`。
原局部X的half步长约`.00024414`方块（.00390625像素），比inflate大；新局部X约.035/.043，步长约`.00003052`方块。
cube3原间距`.00031125546`，重基后也更接近原值。body的这些X正好可被half精确表示。

单测在面向贴花侧、距离`.25/.5/1/2`方块、near=.01/far16的示例第一人称投影下，验证cube0/2的贴花深度严格在body前，
包括模拟24位depth量化，不再是同一深度值。另测任意旋转/平移下矩阵补偿的等价性。
这不是当前RTX depth format/实际相机参数的GPU capture，也不保证任意远距/掠射角均有足够深度精度。

Java entityCutout默认LEQUAL、body后child的树序，以及模型inflate证据沿用`UV-PACKING-0.3.22.md`。
本版没有全局bias、强制depth func、清深度缓冲或重新排列draw；若实际为float且白字仍闪，应检查GPU深度/材质pipeline，
不能把本次缺失的packed日志当成排除它们的证据。**HK是否实机修复，仍待验收。**

## 回归和重跑

生产loader审计安装325枪与参考default54枪。全部安装模型的源顶点经过`(p-origin)+origin`回程最大误差为**0**。
Timeless、block_17、AK47、cz75、glock_17的UV审计全部0失败，slot/name有效；不是让325枪都新增可见offset。

```text
xmake build tacz-pack-audit
xmake run tacz-pack-audit <gunpacks> <outside-metadata.json>
python scripts/verify-item-depth-audit.py <outside-metadata.json>
```

本轮metadata为 `C:/Users/MRUIAW/AppData/Local/Temp/opencode/guns-0.3.23.json`，回归脚本通过。
单测涵盖精确两处HK gap、D24示例投影、补偿矩阵、零origin；既有Timeless与新增block_17反向/跨界UV测试继续通过。

实机：持HK后直接mesh1（这次已缓存也应打印），保存新的cache/position日志；观察侧字、检视/换弹、RTX开关，结束mesh0。
再检查Timeless/block_17滑套、cz75/glock_17弹匣、AK47和手臂。没有代理启动游戏来冒充实机回归。
