# 0.3.33：LL升级与容器飞行图标

目标：Minecraft **1.26.51.01 / LeviLamina 26.51.9**。

## 1. 独立检查点：SDK升级

- xmake的LL精确版本26.51.7→26.51.9；tooth的server/client依赖均锁26.51.9。
- 更新本地levimc-repo后，由上游配方自动选`bedrockdata v26.51.1-server.12/client.12`，不另行锁定旧数据。
- 专门复核InventorySync、StackLimitProbe、AmmoItem、PackItem、GunHud、SkinTextureSource：
  所有原有签名/布局断言保留，双端Release成功，**无需修改既有hook签名**。
  直接包含的头文件差异是CompoundTagVariant新增算术get模板、client Blob等价uint8别名等；
  上游其他容器声明变更未造成这些hook的签名变化。
- ClientSkin/ClientSkinSystem/ActorResourceDefinition已可供后续皮肤阶段调查；本次不接入。
  ItemDescriptor复制/移动构造thunk及Item::buildDescriptor仍未新增，保留描述符桥、enforce和会话/生命周期防护。
- server/client Release及全部**200项／920,410断言**通过后单独提交，包版本暂为0.3.32；
  正式0.3.33在第二检查点同步。未将旧实机记录中的26.51.7改写成新SDK验收。
- **注意**：26.51.9的FlyingItemRenderer头与26.51.7相同；编译断言只证明代码匹配SDK，
  不证明SDK匹配本机二进制。飞行参数必须在下一检查点核对机器码。

## 2. 飞行图标：本机只读反汇编

证据日志：`2026-10-28.4.log`，`small_chest_screen`，AK47的x=275.49625、y=230.58562、scale=3.2e-44、zOrder=0。
这是整数23的float位模式，不是PNG透明、身份丢失或应该放大图标的证据。

二进制：`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/Minecraft.Windows.exe`。
SHA256：`e3bd6b85f5f37c7814b131ba4e0c412957397e2804b71871e6180c773f69605b`。
下列均为RVA，只读调查，**不用于按RVA安装hook**。

### 定位与交叉核对

- 未启动游戏取临时日志。先按完整`ui::FlyingItem`字段的读取组合定位渲染循环：
  ItemStack0x98字节，startTime+0x98、animationTime+0xa0，source/dest x/y+0xa4/+0xa8/+0xac/+0xb0，
  source/dest scale+0xb4/+0xb8，forceFoil+0xbc，数组步长0xc0。
- `0x5568770`循环逐项读这些字段、移除过期ItemStack、对三对source/dest值做插值；
  `0x55689c7`调用`0x5568a30`。后者又访问同一个ItemStack，判断+0xbc的forceFoil，
  调用ItemRenderer的GUI绘制并提交UI mesh。结构、状态流和绘制下游相互吻合。
- `python find_callers.py 5568a30`只找到一个直接调用点`0x55689c7`。
  循环的插值分支与到达终点分支都汇入此点。旧hook的x/y正常、scale恰为层级整数位模式也与此布局完全对应。
- 本机原始反汇编保存在临时目录`tacz-0.3.33-flying-disassembly.txt`；
  用既有`disas_inv.py dis 5568770 1000`和`disas_inv.py dis 5568a30 700`可复查。

### 调用方：没有transparency

```text
0x5568951: movss xmm0, [r15+0xb4]  ; sourceScale
0x556895a: movss xmm1, [r15+0xb8]  ; destScale
0x5568963: movaps xmm2, xmm6       ; elapsed / animationTime
0x5568966: call 0x26400c0          ; native eased scale -> xmm0
0x556896b: mov eax, [rsi+0x64]     ; owner UIControl zOrder
0x556896e: mov [rsp+0x30], eax
0x5568972: movss [rsp+0x28], xmm0  ; scale, not transparency
0x5568978: movss [rsp+0x20], xmm8  ; y
0x556897f: mov rcx, rbx            ; context
0x5568982: mov rdx, rdi            ; client
0x5568985: mov r8, r15             ; ui::FlyingItem&
0x5568988: movaps xmm3, xmm7       ; x, 4th argument
...
0x55689c7: call 0x5568a30
```

终点分支`0x5568990–0x55689c4`以相同三个栈槽传desty/destScale/zOrder、xmm3传destx。
MSVC x64的前四个位置使用rcx/rdx/r8/r9或相应xmm0..3；第5项起上栈，每项占8字节槽。

### 被调方：七参数，原生透明度固定1

入口RSP记作S；压7个寄存器再sub0x160，`rbp = S-0x118`：

| 读取 | 实际参数 | 旧hook误读 |
| --- | --- | --- |
| xmm3 | x（第4项） | x正确 |
| `[rbp+0x140] = [S+0x28]`，`0x5568a9c` | y（第5项） | y正确 |
| `[rbp+0x148] = [S+0x30]`，`0x5568a93` | scale（第6项） | alpha误读为scale |
| `[rbp+0x150] = [S+0x38]`，`0x5568a8c` | **int zOrder**（第7项） | scale误读为整数23位模式 |
| 没有读取`[S+0x40]` | 没有第8项 | 旧zOrder读到无关栈值0 |

`0x5568b20–0x5568b56`向`ItemRenderer::renderGuiItemNew`（`0x55ac4e0`）传：
原x/y、renderEnchantmentFoil=false、**transparency=1.0 / lightMultiplier=1.0**、原scale、原zOrder。
forceFoil第二遍也是相同透明度/缩放。不存在需要自己猜测或重建的alpha动画参数。

原版flat-item下游`0x55adcd9`把scale乘常量16，随后`0x55add1a/0x55add21`用作宽高。
所以自绘仍为`16*scale`，**错误是取参位置，不是16这个基数太小**。

### 曲线与生命周期不改写

原版三次调用`0x26400c0`，分别插值x、y、scale。此函数把t减1，连乘为五次方再加1，
即`ease(t)=1+(t-1)^5`，返回`source+(dest-source)*ease(t)`；终点分支直接使用dest值。
hook在原生计算后接收结果，不重新取时钟、插值、夹取进度，也不添加淡入淡出。
玩家背包不创建原版飞行动画时，不人为合成动画。

## 3. 修复与安全边界

- SDK原8参数`static_assert`**保留**，用于将来上游修正时强制重新审查。
  单独声明已证实的7参数`NativeFlyingDraw`；仅将SDK符号**地址**转为此类型，
  detour与origin/trampoline均为7参数，并新增detour类型断言。绝不通过错误SDK声明发起调用。
- 自绘使用原x/y、16*scale、White/alpha1，不再把层级当浮点，不放大错误值、不使用私造alpha曲线。
  原zOrder保留供日志与原生fallback；自绘仍走既有UI即时drawImage/flushImages路径，
  其实际遮挡/图层效果需与箱子原版飞行图标实机对照，不能仅凭数值正确宣称完全一致。
- 非有限x/y/scale、scale不在[0.1,16]时不自绘；非本mod、无图标、纹理/绘制失败、
  禁用或无所属world/client时也执行**正确七参数的origin**。
- 新增纯逻辑测试覆盖16/18像素槽尺寸、中间缩放不变、边界与NaN/Inf/负数/零/subnormal，
  包括日志中整数23位模式；这些测试不代替真实容器/渲染集成验收。
- 不改正常热栏/背包/箱子、cursor/tooltip身份、PNG alpha、材质或枪包；
  不改0.3.32已验收的枪写回/实时单槽包、弹药提取、原生拾取、give、漏斗和丢枪逻辑。

## 4. 实机回归清单（待用户执行）

1. 箱子中放AK47和7.62x39，shift快速转移**双向**；与原版物品并排比较源→目标位置、
   16/18像素槽尺寸及缩放缓动，移动中不消失、不闪双图、不被槽背景遮住；不同GUI缩放也核对。
   日志`[item-icons] flying rendered`应得到正常scale与实际zOrder，不再是3.2e-44/0。
2. 玩家背包shift没有原版飞行动画是预期，不算回归；拖拽预览及重开界面图标仍应正常。
3. 已通过0.3.32的两项各快测：无地面60发开火30后换弹立即30；满包地面弹药连续换弹立即补满、地面扣量正确。
   如异常，用`/tacz rdebug ammosync 1`取新日志（重复1重置预算，0关闭）。不重做已接受的修复。
4. 连射实时枪弹量、Q丢枪/拾回；热栏/背包/箱子持续图标；枪/弹药tooltip；give与满包溢出；漏斗及箱子shift堆叠。
5. 主菜单、退出世界、换世界后再开箱子；非mod原版物品飞行仍正常。未启动游戏冒充验收。

## 5. 离线验证

- 第一检查点`ea8c127`：LL升级，双端Release、200项／920,410断言通过。
- 第二检查点：正式0.3.33同步xmake/tooth、BP/RP（包括BP对RP依赖）、版本测试，无BOM。
  server/client Release及全套**202项／920,529断言**通过，`git diff --check`通过。
- 新飞行效果、UI图层和26.51.9游戏内回归仍未验证；离线断言与机器码证据不是实机验收。
  部署仅在游戏关闭时通过原`scripts/deploy-client.sh`进行，不停止/启动游戏、不改已安装枪包。
