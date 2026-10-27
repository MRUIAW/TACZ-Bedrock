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

## Part2：持续sprite绘制、alpha与拖拽

0.3.29已有正确source及rendered，只证明入口/上传/一次提交，不证明后续帧、offscreen composite或像素。
旧update仅在身份变化时返回changed，而本模块drawImage/flushImages是即时提交，不生成可重播的原生item mesh。
现在已知pack图标的update持续返回dirty；每次slot render使用缓存身份绘制，未知/owner不符时通过当前scene重新解析。
绑定额外验证base/aux；clone仅复制身份值并标为owner未绑定，绘制前必须重新解析当前owner。无world时不做自定义工作。
已知PNG改用单次sprite材质/渲染pass，不让原生占位mesh/glint后续pass覆盖。数量标签仍为UI独立原生控件。
未知身份/资源或绘制失败直接origin，不修改mHidingIcon，不全局隐藏原图；尊重原生hidden状态。

### Alpha证据

本机 `data/resource_packs/vanilla/materials/ui.material`：ui_base第10–14行已有Blending；
ui_textured_and_glcolor第222行alphaDst=DestColor，而ui_textured_and_glcolor_sprite第238–240行改为OneMinusSrcAlpha。
因此不是盲目声称“旧材质完全没开混合”，而是槽图显式使用原版sprite/source-over目标alpha路径与UIMaterialType::TexturedAndGlcolorSprite。
RGBA8上传不删alpha、不预乘；setRawImage后再设置AlphaUsage::Transparent，避免metadata被替换。
上传日志记录alphaZero/alphaPartial/alphaOpaque；HUD及tooltip的材质参数保持原有选择不改。
这是源码材质与提交路径的修复；黑边/透明像素、RTX/offscreen合成仍需实机确认。

### Cursor与flying

Mojang ui_common.json的inventory_selected_icon继承common.item_renderer，但绑定#inventory_selected_item而不是普通collection；
inventory_icon_panel使用selected_item_icon，inventory_selected_icon_button跟随cursor。只对这些明确cursor控件，
且controller::_isCursorSelectedActive时，读mCursorSlotData()，再走_getVisualItemStack。
不使用hovered/当前热栏枪替换cursor，不猜GunId；仍校验base/aux与完整栈身份。
飞行动画另由FlyingItemRenderer::_renderGuiIcon处理，当前26.51.7 SDK有完整ui::FlyingItem（内含ItemStack），
本次typed static hook直接用该栈身份、native x/y/scale/alpha，缺身份/PNG时origin。
所有新签名均static_assert，未构造UIItemRenderInfo等opaque by-value类型。

`[item-icons] redraw`在draws=1/120/1200记录，确认持续提交而非一次性rendered；
cursor-source、flying rendered及alpha上传计数用于区分数据/绘制/透明问题。
目标：热栏、背包、箱子、漏斗公共renderer，光标持有图与飞行动画；特殊资源包UI名称可能不在cursor识别表内，安全回退。
飞行动画定位/缩放与native叠层需复测；手持/地面3D模型明确不在本版实现。

Part2验证：双端Release、195项测试／917,818条断言、diff检查通过；游戏关闭时已部署，两端文件与RP/BP哈希匹配。
DLL SHA256：`ce7bc5c9900558569fe68be9d839fb7869d77c957dc5763a243d6f81c01bb1cf`。
本轮未启动游戏，连续显示/透明/拖拽仍待实机验收，不能据纯测试宣布里程碑已全部通过。
