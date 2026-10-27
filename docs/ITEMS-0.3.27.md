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
