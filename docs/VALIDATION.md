# 技术验证记录

环境：LeviLamina 26.51.7，客户端 1.26.51.01（LeviLauncher），单人世界（客户端内置服务端）。

| # | 项目 | 结果 | 实现位置 |
|---|------|------|----------|
| 1 | 自定义包双向收发 | 通过（单人） | `src/net/DebugPackets.h`、`ll::network::PacketBase` / `PacketHandlerBase` |
| 2 | 物品 NBT 读写、同步到客户端、退出重进后保留 | 通过 | `src-server/server/ServerProbe.cpp`：`ItemStack::mUserData` + `setUserData` + `Player::setSelectedItem` + `sendInventory` |
| 3 | 手持枪时拦截原版左键 | 通过（单击和按住都不会挖方块或攻击） | `src-client/client/ClientProbe.cpp`：`MouseInputEvent::cancel()` |
| 4 | 自解析 geo 模型替换第一人称手持物渲染 | 通过（静止姿态，带贴图，跟随视角不抖动，单面无缺面） | `src-client/client/render/GunRenderer.cpp`：hook `ItemInHandRenderer::renderFirstPerson`，Tessellator 建网格，stb 解码 PNG 后 `TextureGroup::uploadTexture` |
| 5 | 播放枪包目录里的 ogg | 通过（枪声、切换模式声，连点无卡顿） | `src-client/client/sound/GunSound.cpp`：读取 `tacz_sounds/<path>.ogg`，`SoundEngine::playFromMemory`（非定位） |
| 6 | C++ 驱动 `tacz:bullet` 实体 | 通过（正常速度可见，命中方块即消失无残留；234 发中 173 命中方块、60 寿命到期） | `src-server/server/BulletSystem.cpp` + `packs/behavior_packs/tacz_bp`：`CommandUtils::spawnEntityAt` 生成，`ServerLevelTickEvent` 每 tick 推进，`BlockSource::clip` 判定方块 |
| - | 客户端 HUD 文字绘制 | 通过 | `AfterUIRenderEvent` + `MinecraftUIRenderContext::drawText`，只画 `.hud_screen` 视图 |
| - | 服务端命令注册（单人） | 通过 | 监听 `ServerCommandRegisterEvent`，每次进世界重新注册 |
| - | 游戏内加载默认枪包（54 把枪） | 通过 | `/tacz packs`、`/tacz setgun` 校验枪 ID；回复改用 `Player::sendMessage` 后聊天栏可见 |

## 注意事项

- 客户端和服务端在同一进程时共享一张包处理器表，所以 C2S 和 S2C 必须用不同的包类型。
- 命令参数结构体不能放在匿名命名空间里（boost::pfr 反射要求外部链接）。
- 单人世界里，自定义命令的 `CommandOutput::success` 文本不会显示在聊天栏（加了前缀也不显示），但命令确实执行了，日志里有输出。原因未查，暂不影响；需要给玩家看的反馈改用 `Player::sendMessage`。
- 单人模式测不到网络延迟，也测不到"服务端装了 mod、客户端没装"的情况，这两项留到有 BDS 时再测。
- 只取消 `MouseInputEvent` 就足以挡住按住左键的连续挖掘和攻击，暂时不需要 hook `InputHandler`。

### 第一人称渲染（0.2.1–0.2.3）

- 材质：`MovingBlockActorRenderer::mBlockMaterials[RenderlayerAlphatest]` 能画出贴图，配合 `ActorShaderManager::setupShaderParameters(screen, blockSource, 全亮 BrightnessPair, ...)`（Lamium 画贴图方块的做法）。`entity_alphatest` 画不出东西；`ItemInHandRenderer::mStaticMaterial` 在第一人称渲染器上未初始化；`debug` 纯色材质可用作排查。
- 摆放：第一人称渲染时世界矩阵栈顶就是相机的反向旋转，把 TACZ 的 `firstPersonIdleTransform` 乘在它上面即可得到正确的视图空间位置。**不能替换视图矩阵**：push 一个单位视图矩阵后渲染器内部空指针崩溃（`+0x30`）。
- UV 和法线：设置 `Tessellator::mNextUV[0]` / `mNextNormal` 后再 `vertex()` 能正确写入（日志 uv0、normals 数量与顶点数一致）。
- 网格只在首次手持时构建一次并缓存（AK47：605 个骨骼，7608 个顶点）。
- 已知缺口：光照固定为全亮；没有手臂和动画；第三人称仍是原物品。

### 音效（0.2.5）

- `SoundEngine::playFromMemory(name, data, size, is3D, ...)` 直接播放 ogg 字节，无需资源包。字节缓冲在进程内一直保留，因为引擎可能在调用返回后继续读取。
- display 的 `sounds` 里有些键在枪包中没有对应文件（如 AK47 的 `reload_tactical`、`inspect`）；这些声音实际写在动画关键帧的 `sound_effects` 里，要随动画系统一起播放。
- 空音效 ID（`""`）在 TACZ 里表示不播放，必须跳过而不是报错，否则 fn_fal、hk_mk23、spas_12 的 display 会整体加载失败。
- 尚未验证：3D 定位播放（其他玩家开枪的声音）、服务端广播。

### 子弹实体与 mod 自带包（0.2.6）

- LeviLamina 会自动加载 `<mod 目录>/resource_packs/*` 和 `<mod 目录>/behavior_packs/*`：资源包在客户端启动时加入全局栈，行为包在每次进世界时加入该世界的栈（单人和 BDS 都一样，见 LL `src-*/ll/core/main_win.cpp`）。不需要在世界设置里勾选。日志里能看到 `Pack Stack - [00] TACZ Bedrock ... @ ./mods/tacz/behavior_packs/tacz_bp`。
- 包只在游戏启动时读取，改了包要重启游戏。包的 UUID 必须固定。
- `Actor::moveTo` 和 `Actor::teleport` 两种同步方式在客户端看起来差别不大，默认用 `moveTo`。
- 250 m/s（每 tick 12.5 格）的子弹在正常速度下肉眼可见。
- 行为包里的 `minecraft:timer` 15 秒保底消失，防止 mod 不再驱动时子弹残留。
- 尚未做：命中实体、伤害、爆头、水中阻力、曳光渲染。
