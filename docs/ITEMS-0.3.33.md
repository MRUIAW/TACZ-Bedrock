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
