# 0.3.27：原版创造拾取政策／槽图后续

## Part1：创造模式恢复原版

用户实测 `9966c25`：**生存部分拾取余量正确，退世界重进仍存在**。
部分拾取无音效与原版泥土等一致，保留原生表现，不补声音。
用户决定创造模式也完全按原版：放不下的余量有意丢弃，不再人为保留。

因此删除Actor::isCreative hook、AmmoTakeContext/RAII scope及preserveAmmoPickupRemainder helper。
Player::take仅观察并原样转发参数/返回值。保留Inventory::add部分写入且remaining>0时返回false，
原生生存false分支写回原actor；无A2、无actor副本、无强制刷新/网络包/拾取后改数。

保留修正日志：rawAfterTake是实际读取的actor count；removed是当前状态；
removalPending代表take返回true、等待caller删除的actor；有效groundAfter排除已移除/待移除actor。
创造模式有意丢弃时conserved可能为false，creativeDiscarded记录推导的丢弃量，accounted纳入该量；
生存conserved/accounted应均为true。isCreative仅为日志查询，不再hook或改变它。
实际add日志不再受canAdd的40条采样上限遮蔽，不需要任何take scope。

纯测试覆盖生存余量、原版创造丢弃政策、完整拾取残留actor与待移除计数；不把纯模型当native实机证明。
复测：生存半满栈+地面60，确认等量入包/地面剩余，重进保留；创造同场景与原版物品对比，允许丢弃放不下的部分。
图标祖先查找另行提交；A1其余set/isFullStack/isStackable、FillingContainer搜索/addResource及漏斗/容器/UI/网络/预测仍未统一。

Part1验证：Server/Client Release均通过；184项测试／917,541条断言通过，diff检查通过。
确认游戏未运行后已部署客户端；DLL/PDB/manifest和RP/BP全文件哈希匹配。
Part1 DLL SHA256：`ba2f217dd89089d57eb2cad7e58bd7a1d243221cc4cd33967cea6432b81f597a`。

## Part2：完成暂存的祖先槽位查找（独立commit，版本仍0.3.27）

0.3.25的update hook安装成功，但hud_screen/inventory_screen/small_chest_screen均无resolve/source；
owner调用findAllCollectionIndices没有提供候选，不能把hook安装当解析到真实槽。
本次重新核对Mojang `ui_common.json`：3935行item_renderer通过collection_details绑定$item_collection_name；
5833/5837行grid_item_for_inventory/grid_item_for_hotbar分别指定inventory_items/hotbar_items，
5848/5866行grid_item_template及collection_name将它们放入对应grid。
这些是**grid/item上下文的静态证据**，不是本机某个祖先control名称或depth已被实测的证据；
该模板未直接写字面collection_index字段，索引由运行时grid实例化提供，不能硬编码其槽序或控件名称。
来源：`https://raw.githubusercontent.com/Mojang/bedrock-samples/main/resource_pack/ui/ui_common.json`。

Lamium `src/features/inspection/hover/HoverTracker.cpp:118,150–158`从controller hover回调接收
已解析collection/index并经_getVisualItemStack读取完整栈；它本身不从renderer owner枚举索引。
本版仅借鉴full-stack resolver，**不借用当前hovered/selected槽给所有renderer配图**。
来源：`https://raw.githubusercontent.com/amatouhake/Lamium/main/src/features/inspection/hover/HoverTracker.cpp`。

沿owner→mParent.lock()最多32级、检测循环，选最近有非空collection且index>=0的层；
仅对该层经container controller或HUD hotbar本地inventory取完整栈，验证base/aux与GunId/AmmoId。
最近层为空/不匹配/歧义则保留原生图，不向外层grid猜身份，不访问兄弟或子树。
父shared_ptr只在本次update短暂持有；sidecar继续仅存身份值及owner/world校验，切同类不同枪使批次失效，退出清理不变。

每screen对普通/pack控制各最多3次输出ancestor(depth/control/collectionCount)、
ancestor-index(collection/index)、ancestor-result(sourceDepth/identity/ambiguous)；保留resolve/source/rendered。
非诊断帧找到最近collection层即停止，不每帧遍历整棵UI树；无world session不执行本模组查找/日志。
**具体哪个实际祖先持有索引尚未实测，必须以新日志的control/depth/collection/index为准**。
纯测试覆盖空owner、两级祖先、index0、非法索引、最近层优先；不代替native UI/像素验收。

复测多枪/多ammo在热栏、背包、箱子互移，快速切同类别不同枪、空槽再放入、关开UI/退菜单/换世界。
若仍占位，回传ancestor链及resolve/source/rendered，禁止据占位结果反猜GunId。

Part2验证：Server/Client Release均通过；185项测试／917,549条断言全部通过，diff检查通过。
再次确认游戏未运行后已部署0.3.27/client（含Part1+Part2），DLL/PDB/manifest与RP/BP全文件哈希匹配；未启动游戏或改枪包。
最终DLL SHA256：`0ec3368ee8a2b5702c0b73fb42b2d44e80a6882580e210aa17919db2b3194702`。
实机图标/具体祖先索引仍待用户验收；本轮未继续安装其余A1 hook，不宣称上限全路径统一。
