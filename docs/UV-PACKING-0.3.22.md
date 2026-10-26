# 0.3.22：原生 UV 打包、repeat 分面与 HK416D 深度调查

## 已验收基线

用户已验收0.3.21 (`9431c99`)：HUD很接近Java；fresh闭膛AK47始终正确031；空弹/低弹红色正确；
RTX开关、菜单、换世界稳定。此前030并非HUD缺膛弹bug。Timeless/HK的depth1/2与sampler0/1均无差异；
Timeless贴花像把大部分枪贴图压进小面。保留这些结果，不再重复把阈值、投影深度或mip当作成立根因。

## 1. 原生编码证据（不是猜测half UV）

`GunRenderer::uploadMesh`：`Vertex::uv` (vec2 float32) → `Tessellator::mNextUV[0]` → `vertex(x,y,z)`
→ `MeshData::mTextureUVs[0]` (vector<vec2>, native +0x98) → `end(Buffered)`/Mesh → `generateHALData`。
Mesh保有 `mMeshData`(+0x58)、110字节`mVertexLayout`(+0x1f0)、`mRawData`(+0x260)。
本端未在parser、bone compiler、上传前执行frac/clamp；原生编码才是丢失区间的地方。

对安装的 **1.26.51.01 Minecraft.Windows.exe** 只读分析：
SHA256 `e3bd6b85f5f37c7814b131ba4e0c412957397e2804b71871e6180c773f69605b`，preferred base `0x140000000`。
以下都是RVA，不能直接当成另一版本的地址：

- Mesh生成函数 `0x4afce00`：无forced layout时在`0x4afcebe`调用默认layout生成`0x4affdf0`；
  在`0x4afcf5c`将MeshData、layout和raw vector交给编码器`0x4b000d0`。
- 默认生成器`0x4afff32–0x4afff70`检查UV0 vector，添加field4，offset写入layout+0xa。
- field type查询`0x488de90`，jump table RVA `0xe8ebc60`的第4项指向`0x488df19`：**mov ax,4**。
  所以不是仅找到一个“可能不用”的压缩分支，也不受position half能力标志影响。
- 编码器`0x4b0173f–0x4b017f8`：type4读MeshData+0x98，乘float65535，再ceilf、cvttss2si，
  `mov word [rsi],ax` / `[rsi+2]`。**2×16位归一化UV，范围[0,1]，步长1/65535；没有clamp**。
  UV不是float32或half；type3的32767分支不是默认UV0。
- 常数RVA `0xe61e17c`=float65535；调用`0xe5a68e0`经IAT `0x14f104478`解析为CRT **ceilf**。
  优化packer`0x4b05c10`同样从MeshData+0x98读UV，乘65535、ceilf、低16位写入。
- `0x4b05a80`是**位置**float→half，不是UV。position的half/float选择另受后端能力标志控制，不能把它外推为UV类型。

可重跑（pefile/capstone安装到外部目录，不装入仓库）：

```text
python scripts/inspect-native-uv.py <Minecraft.Windows.exe> --dependency-dir <dependencies> --output <outside-output.json>
```

脚本断言上述分支/常数/导入/低字存储，输出二进制哈希、定点数值、局部反汇编；不启动游戏、不执行原生函数或修改二进制。
本轮输出 `C:/Users/MRUIAW/AppData/Local/Temp/opencode/native-uv-0.3.22.json`。
编码已实证；UNORM解码按65535的归一化格式推算。**没有GPU capture，也没有声称已反编译所有材质shader**；
但CPU缓冲已经丢掉整数区间，无需假设shader再frac就能解释用户观察。

### Timeless 精确数值

原V：`.7890625 → 1.005859375`；description128、PNG256；正确texel `202 → 257.5`。

| 原V | float32(V×65535) | ceil整数 | 低16位 | 归一化解码 |
|---:|---:|---:|---:|---:|
| .7890625 | 51711.2109375 | 51712 | 51712 | .789074540322 |
| 1.005859375 | 65918.9921875 | 65919 | 383 | .005844205386 |

GPU端点插值从约202texel**倒着**扫到1.496texel，而不是202..257.5；压缩大部分纹理的描述吻合。
低16位溢出并非精确数学frac：65535与65536不同（例如-.25编码49153→.7500267）。不能简单在CPU对端点frac。

## 2. 修复：先保留线性区间，边界分面，再整数平移

Java `entityCutout`禁用blur/mipmap；SimpleTexture没有metadata时clamp=false，NativeImage使用repeat。
本轮已检查安装枪包：没有任何`.png.mcmeta`。公开mapped vanilla来源与上版文档相同：
`RenderType.java` entityCutout、`RenderStateShard.java` TextureStateShard、`texture/SimpleTexture.java`、
`com/mojang/blaze3d/platform/NativeImage.java`；<https://github.com/mahtomedi/minecraft/tree/main/src/main/java>。

`src/model/PackedUV.{h,cpp}`在**枪的上传/缓存边界**处理四边形：

1. 已在[0,1]的面直接原样复制，position/UV/normal不做新运算。
2. 对原UV线性参数s/t求所有跨过的整数边界；分成同一tile内的子quad，逐片减相同整数。
   UV倒序、负值、跨多个tile、零UV面积/整值端点均保留语义，不改变parser的Java UV。
3. 新切点由原quad线性/双线性几何插值产生；原角点原样保留；保持平面法线、绕序、源面顺序；
   相邻片共享同一切点计算，没有深度bias/抬高贴花。输入由parser保证是矩形UV/平行四边形几何。
4. Timeless在`t=36/37`分两片：V `.7890625..1` 和 `0...005859375`；不跨越原生表示范围。
   单端负值或两端同在某tile内只作整数平移。最后的clamp仅消除求切点舍入误差，不改变原UV区间。
5. native上传和draw count改用**分面后的顶点数**，包括双面variant；不再截断新增的第二片。

默认path1仍是缓存一次的mesh + 每帧骨骼矩阵；rigid源顶点与group哈希不改、没有新增每帧顶点重建。
命名骨/匿名helper folding、Java DFS顺序、材质/RTX选择、手臂独立上传、世界/菜单资源生命周期都不改。
调试path0仍按其原有CPU重烘焙语义处理。拒绝非有限/不合矩形UV及超大分片预算，不静默生成错误几何。
没有构造/修改SDK空VertexLayout或共享sampler；未改安装枪包、未访问移除/加密Vanguard内容。

单测：精确原生word、Timeless两片、负值/反向/多tile/零尺寸、面积覆盖/绕序/法线、9点逐片编码插值误差、
in-range保持一致、异常/预算限制。16位正常量化仍存在（约1.526e-5 UV），不宣称GPU浮点精度与Java完全相同。

## 3. 离线复现图片和全枪审计

```text
python scripts/analyze-decal-sampling.py <installed-gunpacks> <outside-output>
python scripts/audit-gunpacks.py <installed-gunpacks> --uv-only --output docs/UV-AUDIT-INSTALLED-0.3.22.md
python scripts/audit-gunpacks.py _reference/TACZ/src/main/resources/assets/tacz/custom --uv-only --metadata <outside-default.json> --output docs/UV-AUDIT-DEFAULT-0.3.22.md
```

图输出 `C:/Users/MRUIAW/AppData/Local/Temp/opencode/sampling-0.3.22/`，未纳入仓库。
`sampling-controls.png`已在Review展示；从左到右：**Java、原生16位坏端点、分面修复、clamp示意、linear示意**。
从上到下：Timeless、HK cube0、cube2、cube3。原始分辨率fixed crop与Java最近邻采样比对：
分别 **0/7672、0/156、0/189、0/196** 像素差异；不是实机投影/过滤/GPU截图。

生产loader/parser全部源面审计：安装 **325/325枪**，1,180,714面；独立参考default **54/54枪**，139,967面。
所有已注册枪的display主geo均解析成功。按枪/model引用计，重复模型不去重，隐藏弹匣/手臂locator也计源面：

| 物理包 | 越界枪/枪数 | 越界面 | 后处理片数 / 通过 |
|---|---:|---:|---:|
| Cyber Armorer | 0/35 | 0 | 0 |
| ClassicRCCRP | 4/164 | 28 | 42/42 |
| TWX | 0/14 | 0 | 0 |
| MS-Mobius | 0/58 | 0 | 0 |
| 安装default | 2/54 | 1223 | 1223/1223 |
| 独立参考default | 2/54 | 1223 | 1223/1223 |

每片9点比较Java原UV与“安全端点→真实16位编码→解码→插值+tile”，全部通过，最大误差`1.51398108e-5`。
这是插值/repeat语义离线验证，**不是325枪全部实机验证**；分片内两轴都是仿射，端点量化误差不会被放大成整张纹理。
明细及最坏例见两份UV-AUDIT文档：

- `ccrp:timeless_50`：pattern，Vmax1.005859375，1面；`ccrp:block_17`：pattern，Vmax1.0126953125，1面。
- `classicr:kar98`：12面、最坏V3；`classicr:spr_15`：14面、最坏V1.5，大多是hand locator，不能说真实手臂此前错贴。
- `tacz:cz75`：731面，minV-.0078125；`tacz:glock_17`：492面，minV-.0009765625。
  这1223面全部为零UV面积，数字大不等于1223个可见贴花错误。

## 4. HK416D：驳回折叠/共面前提，保留新线索（本版未修复白字）

安装模型`hk416a5_geo`的body是named `gun_body2`(21)，label是named `bone`(22)，pivot分别`[0,8,1]`/`[6,0,0]`，
标签cube0/2/3没有rotation helper。刚性编译只fold**空名**helper：这三面仍属group bone22；相关重叠body面在bone21，
**两侧都没有因0.3.18改成“一个预烘焙helper、一个不烘焙”**。命名骨边界与Java一样保留。

生产审计`decalDepth`记录：三面各1024次、合计3072次不同模拟camera/projection的prepack clip z/w位比较，
相对同源Java式矩阵链全部0差异；另有精简真实数值fixture单测。不是让decal/body强行共用矩阵后的验证，
也不把“本端与参考路径一致”偷换成“贴花与枪身深度位相同”。

Java `RenderType.CompositeStateBuilder`默认`LEQUAL_DEPTH_TEST`，entityCutout不覆盖它；`RenderStateShard`值515 (`GL_LEQUAL`)，
body先compile、再递归child（本地TACZ `BedrockPart.java:67–75`），本端cutout仍相同DFS顺序。
然而**这些面本来并非严格共面**：cube有inflate，白字外置，不能要求Java贴花/枪身bit-identical depth。
任意平面内不同顶点/三角划分的GPU栅格插值也不自动有bit-identical深度。

| 标签cube | prepack世界X（方块） | 近邻body X | 原间距 | 模拟half后间距 |
|---|---:|---:|---:|---:|
| 0 | .03521874547 | .03515625 | .00006249547 | **0** |
| 2 | .04303124547 | .04296875 | .00006249547 | **0** |
| 3 | -.04328000546 | -.04296875 | -.00031125546 | -.000244140625 |

更强的新候选：**位置half量化丢掉作者的微小inflate**。label偏离pivot6，局部X约-.34；
若用原生half位置分支，cube0/2外扩恰好消失，尽管body局部X本身恰可精确表示；之后不同named骨矩阵及栅格深度舍入可能产生冲突。
UV方面三张白字均在[0,1]，没有Timeless式溢出，不能靠本版UV修复保证白字不闪。

**边界：原生确有half位置编码，但当前RTX/普通后端是否选它尚无运行时证据。**
因此本版未把该模拟当成已确认根因，未全面取消group折叠、未改named骨动画边界、未任意偏移几何，也未宣称HK修复。
下一步优先读实际buffer位置精度，若确认再做局部坐标重基或安全float-position布局，而非继续改alpha/mip。

`/tacz rdebug mesh 1`后在**新建mesh时**增加只读`[gun-packed]`：rawBytes、stride、UVoffset、positionSlotBytes、
splitStream及源/上传顶点数；raw仍保留且布局范围校验通过时打印首4点实际UV words和预测值。
读取的是已知1.26.51布局的110字节**副本**，不写opaque布局、不回写GPU、不强制重新generateHALData。
native可能已清空raw；此时只打元数据，不伪称读回成功。8/12字节position slot可辅助判断half4/float3。
命令本身不会使所有已缓存mesh重传；需要首次持枪或新世界/资源重建。当前尚无新实机日志，不能填造。

## 5. 回归与风险

1. 默认path1、blend1、sampler0、depth0：Timeless slide pattern不再大幅拉伸倒扫纹理；检视/换弹跨UV边界没有裂缝。
2. `ccrp:block_17`贴花；`tacz:cz75`/`tacz:glock_17`弹匣；AK47/M4A1等in-range枪外观、动画、弹量HUD无变化。
3. HK白字仍单列观察，不以UV修复成功替代它的验收。新世界持HK前开mesh1，收集`[gun-packed]`/UV日志，结束mesh0。
4. RTX开/关各持上述枪；双面诊断与默认单面draw count正确；菜单/换世界/切枪缓存与手臂仍正常。

风险：仍有UNORM量化误差，exact texel边界可能差一个采样；切点位置经原生位置量化可能有小形变/细缝，需实机检查。
原生实际sampler过滤/寻址仍未GPU capture；分片保证内部插值范围正确，但linear/clamp边界行为不冒充Java nearest/repeat完全一致。
测试/离线图片不等于RTX或325枪实机验收。未来native版本若支持float UV或改布局应重新审计；版本固定的只读诊断也需复核。
服务端BDS、data同步、server-only warning仍按用户决定最后做。
