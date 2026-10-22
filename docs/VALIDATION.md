# 技术验证记录

环境：LeviLamina 26.51.7，客户端 1.26.51.01（LeviLauncher），单人世界（客户端内置服务端）。

| # | 项目 | 结果 | 实现位置 |
|---|------|------|----------|
| 1 | 自定义包双向收发 | 通过（单人） | `src/net/DebugPackets.h`、`ll::network::PacketBase` / `PacketHandlerBase` |
| 2 | 物品 NBT 读写、同步到客户端、退出重进后保留 | 通过 | `src-server/server/ServerProbe.cpp`：`ItemStack::mUserData` + `setUserData` + `Player::setSelectedItem` + `sendInventory` |
| 3 | 手持枪时拦截原版左键 | 通过（单击和按住都不会挖方块或攻击） | `src-client/client/ClientProbe.cpp`：`MouseInputEvent::cancel()` |
| 4 | 自解析 geo 模型替换第一人称手持物渲染 | 未做 | |
| 5 | 播放枪包目录里的 ogg | 未做 | |
| 6 | C++ 驱动 `tacz:bullet` 实体 | 未做 | |
| - | 客户端 HUD 文字绘制 | 通过 | `AfterUIRenderEvent` + `MinecraftUIRenderContext::drawText`，只画 `.hud_screen` 视图 |
| - | 服务端命令注册（单人） | 通过 | 监听 `ServerCommandRegisterEvent`，每次进世界重新注册 |
| - | 游戏内加载默认枪包（54 把枪） | 通过 | `/tacz packs`、`/tacz setgun` 校验枪 ID；回复改用 `Player::sendMessage` 后聊天栏可见 |

## 注意事项

- 客户端和服务端在同一进程时共享一张包处理器表，所以 C2S 和 S2C 必须用不同的包类型。
- 命令参数结构体不能放在匿名命名空间里（boost::pfr 反射要求外部链接）。
- 单人世界里，自定义命令的 `CommandOutput::success` 文本不会显示在聊天栏（加了前缀也不显示），但命令确实执行了，日志里有输出。原因未查，暂不影响；需要给玩家看的反馈改用 `Player::sendMessage`。
- 单人模式测不到网络延迟，也测不到"服务端装了 mod、客户端没装"的情况，这两项留到有 BDS 时再测。
- 只取消 `MouseInputEvent` 就足以挡住按住左键的连续挖掘和攻击，暂时不需要 hook `InputHandler`。
