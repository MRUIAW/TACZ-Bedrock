# 0.3.30：物品系统单人收尾

## 0.3.29 实测

用户确认：箱子shift双向（含53+60）、漏斗、站在掉落ammo上射击/换弹后同步均正确。
创造满包却不吸取：`.7.log`的canAdd accepted=0/native=true/adapter=false表明旧适配器拦截了原版创造丢弃分支。
槽图身份已在热栏/背包/箱子/漏斗解析并有rendered，但开界面后空图、透明变黑、拖拽占位仍未修复。

## Part1：删除旧逐路径补丁

删除Inventory::$canAdd、Inventory::$add与Player::take观察hook。容量/插入/地面余量、同步、创造溢出丢弃和音效全由原生处理。
保留桥接enforce，不引入A2，不按拾取路径另打补丁，也不人为补部分拾取声音。
保留/tacz give拆堆/掉脚边（其私有插入helper只被命令调用，不拦截普通操作）。

本机1.26.51.01只读反汇编再次验证ItemStackBase::getMaxStackSize RVA0x247df40：
0x247df6f调用getDescriptor；0x247df77加载Item虚表+0x320；0x247df84调用内层max getter。
因此删除PackStackLimit外层重复hook，让它也通过桥接。未按这些RVA安装新hook。
旧vanilla-base带AmmoId仍可读身份，但不属于仅tacz:ammo的桥过滤器，不承诺其动态上限。

hasSameUserData只留下**至少一侧带StringTag TaczAutoName**的旧自动名兼容；无标记直接origin。
旧标记及恰好等于标记的display.Name可忽略，其他NBT仍完整比较；不绕过native item/aux检查。
若全删，旧自动名栈与新栈的userdata不同，会停止合并；故保留这一窄迁移逻辑，不当作上限适配器。

回归：生存半栈+地面60→槽填满、余量同actor持久化且立即同步（部分拾取无声与原版一致）；
创造满包无可合并槽→仍吸入并丢弃放不下的部分；箱子双向shift、漏斗、拖拽拆堆及满包give溢出。
旧marked-name ammo和新ammo应能合并，真正改名/不同AmmoId/lore不得合并。编译/纯测试不代替实机验收。

Part1验证：双端Release、192项测试／917,613条断言、diff检查通过。
确认游戏关闭后已部署；DLL/PDB/manifest、RP/BP全文件哈希一致。
DLL SHA256：`2d208dacad4e105a5899a6e3c5cccec2ea9ffb94051b2c4af0567f1e7fce5286`。
