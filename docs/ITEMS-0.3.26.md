# 0.3.26：拾取余量丢失／槽图后续

## Part1：0.3.25真实结果与修复

`f7bd259`用户确认tooltip弹数正确，未观察到超过60；图标仍占位。
满包仅余半栈时，多余地面弹药真实删除，重进也不存在。

证据：`2026-10-27.1.log`487–498行（04:45:01/04:45:07）明确同时记录
`[ammo-stack] add ... remaining=15/7 complete=false`与随后`native=true`。
04:46:06/32缺少add日志不能推断绕过Inventory::$add：canAdd/add共享40条采样额度，之前已耗尽。

### 实际native链（1.26.51.01 RVA；不是硬编码hook地址）

`Player::take 0x1f6c60` → `0x1f741e–0x1f742c`调用Inventory虚表45/add → 当前typed适配器。
native FillingContainer::add亦允许partial mutation但返回false。false进入`0x1f74e1`，
`0x1f750b`起按差额提交事务，`0x1f75ce–0x1f75df`将余量写回原ItemActor。
**之前漏看了紧接的分支**：`0x1f75f0`调用Actor::isCreative（函数`0x1a2a3d0`按gametype1／default5+世界模式判断）；
true时`0x1f75f9–0x1f7607`将本地source赋为空。`0x1f760c`遂看到0并进入成功分支，
take返回true，外层调用者删除actor，即使actor里刚写回的余量仍非零。
这解释了“add=false、groundAfterTake=30但take=true”。不是已证明的64/60读取分歧。

修复：typed Actor::isCreative hook仅在同步AmmoTakeContext中、this为当前玩家、
Inventory::add刚留下余量时返回false，令native保留false/余量分支；零进展有余量也保护。
不改gametype/世界设置，不影响scope外的创造模式；不拆actor、不生成副本、不补造包、不在take后修数量。
scope RAII恢复嵌套上下文，无跨tick引用；保持原生取消和完整拾取路径。
该predicate作用域也包含add之后的同步native事务回调；与其他模组在此回调内查询创造模式的共存需回归。

日志：take内add不再受采样上限遮蔽；记录addCalls/addRemaining/creativeDiscardSuppressed。
完整拾取的actor可能在take返回后才由caller删除，不能只看mRemoved或残存mCount：
`rawAfterTake`为实际读取，`removed`为当前移除状态，`removalPending`明确标出true返回等待caller移除；
`groundAfter`是排除已移除/待移除actor的有效数量，conserved据此计算，不假称已跨tick观察。
部分拾取预期native=false、rawAfterTake=groundAfter=剩余量、conserved=true，原actor继续存在。

### 验收与边界

纯测试覆盖创造/生存、满包零空间/部分/完整、有效ground与pending removal计数。
**编译和纯测试不能证明native hook执行、保存持久性或客户端同步**；请创造/生存分别测
30/60背包+地面60→背包60、原地面30，反复踩无增加，退世界重进地面30仍在；
再腾空一槽收走余量；完整拾取groundAfter=0、conserved=true。查看creativeDiscardSuppressed及addCalls。
其余A1的set/isFullStack/isStackable/descriptor、漏斗/容器/UI/网络/预测归类仍未完成，沿用0.3.25风险。
本部分不宣称图标已修复，不改变行距、枪包或已验收UV/HK修复。

Part1验证：Client/Server Release均通过，全部184项测试／918,572条断言通过；
`git diff --check`通过。检查游戏未运行后已部署客户端；DLL/PDB/manifest与RP/BP全文件哈希一致。
Part1 DLL SHA256：`8d9f45a23b0b4ab9f86ae9e94ec342a263e453569e714a3157631dc11f7dcc07`。
未进行实机持久性验收。
