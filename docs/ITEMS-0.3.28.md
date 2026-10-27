# 0.3.28：槽位标量索引／descriptor上限观测

## 0.3.27 实测结果

`caee2ed`：创造拾取符合原版，生存脚本测试通过；部分拾取无声仍按原版，不改音效。
新问题尚未修复：满包站在地面ammo上射击/换弹，客户端计数回跳60，Q/重进才恢复；
有同种ammo时箱子→背包shift失败；箱子53、背包60+60，shift一堆后箱子60+49、背包60+4。
最后一例符合转移按64、槽位限制60的不一致，仍需本轮测量归类，不能声称已统一A1。

## Part1：findCollectionIndex标量索引

`2026-10-27.5.log`327–350等行：hud的item_icon→hotbar_parent→gui_hotbar_grid_item均报告hotbar_items，
背包common.item_renderer→item→item_cell→grid_item_for_inventory报告inventory_items，箱子container_items。
**各槽callback int均1188008220/0x46CF9F9C**，不是合法索引；祖先链不是本次失败点。
SDK完整function_ref及签名能通过编译，仍不能证明native callback参数ABI一致。

不再读取/打印/使用callback的int，只收集collection字符串名称；已有日志中名称与Mojang模板一致，
再检查长度<=128及ASCII名称字符后，调用当前26.51.7 SDK的
`int UIControl::findCollectionIndex(std::string const&) const`，有完整decltype static_assert。
字符串参数的语义证据为正常名称与plain getter的结果，不宣称已证明所有function_ref ABI正确。
标量索引防御范围0..4095，HUD进一步限制0..8；异常超大collection会安全保留占位图。
每screen/collection/index去重输出`[item-icons] slot-index ... via=findCollectionIndex nameValid=true inRange=...`，
祖先日志也只显示getter得到的index。保留nearest scope、完整栈base/aux验证、歧义拒绝、owner/world清理。

实机验收：不同槽应显示不同0..N索引，而非0x46CF9F9C；随后应有resolve/source/rendered。
尚未启动游戏，不把编译/纯测试当成实际索引或像素成功。

Part1验证：双端Release及186项测试／917,560条断言通过，diff检查通过。
游戏未运行时已部署0.3.28/client；DLL/PDB/manifest与RP/BP文件集合/哈希匹配。
Part1 DLL SHA256：`cea0b2d8967be6d3f3db842925b109258268247d78eee63866d394b48cd974bf`。
