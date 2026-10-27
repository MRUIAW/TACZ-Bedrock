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

## Part2：descriptor bridge，严格 observe only

本轮不修复上面三个ammo问题，不统一返回值。已有ItemStackBase::getMaxStackSize AmmoId hook及Inventory适配器不变；
新增Item::$getMaxStackSize、ComponentItem::$getMaxStackSize仅统计并**原样return native**，不会返回would，
不写Item::mMaxStackSize、不更换descriptor/vtable、不改NBT/count/事务/预测，也不恢复创造模式覆盖或A2。
两getter地址折叠相同时只装一次；安装日志的folded=true说明共享入口，不能将ComponentItem=false误判为漏装。
小getter还可能与其他类的机器码折叠：为避免把陌生this/参数解释为Item/descriptor，先在world tick用真实临时ItemStack
解析tacz:ammo（不入包、不掉落、不生成actor），以及从真实完整ammo栈生产处登记Item地址；最多8个地址，兼容client/内置server独立registry。
内层hook先做已验证地址的整数比较，只有匹配才读取descriptor。首次登记之前的调用不计入测量；登记失败时不能声称没有上限读取。
`[stack-probe] registered ... source=temporary-stack/full-stack`用于确认过滤器已准备好。

### 桥键、复制及可信度

- ItemStackBase::getDescriptor返回后，仅当完整栈是tacz:ammo、AmmoId有效且枪包存在，记录
  `{GetCurrentThreadId(), uintptr_t(descriptor.mImpl.get())}`→AmmoId、limit、Item指针数值、aux、时间。
  **不以wrapper地址或类别ID猜AmmoId**。mImpl分配随wrapper移动保持不变，所以同线程移动可继续命中。
- 当前26.51.7 SDK的InternalItemDescriptor::$clone与ItemDescriptor::operator=(const&)观测到的复制会传播候选，
  保留原始时间，不能用不断复制延长候选寿命。ItemDescriptor的Item+aux/name+aux ctor和getDescriptor生产会覆盖/清除旧键。
- 只读当前调用者传入的live descriptor。缓存中的指针仅为整数比较键，**绝不解引用或持有栈/descriptor原生引用**。
  验证Item+aux、同线程、同world session；250ms过期；4096条缓存满时清空而非猜身份。
- SDK析构为inline/default，没有本轮可安全签名检查的独立清理入口；stream/JSON/其他构造或inlined复制可能绕过探针。
  因此同线程、同Item/aux、250ms内的未观测地址复用仍可能形成假命中，跨线程/过期/未观测复制则可能漏命中。
  **resolvedCandidate不是“已证明生命周期正确”**，不得据此直接开启统一上限；后续须先解决这些边界。
- 老26.20.7源码提示有更多move/copy API，但当前26.51.7没有同样ctor thunk声明，不照抄旧签名。
  所有本轮hook均使用当前SDK成员及static_assert；ItemDescriptor16字节、InternalItemDescriptor24字节来自当前SDK，
  无opaque/by-value placeholder。ItemDescriptor按真实完整类型返回，clone用真实unique_ptr<BaseDescriptor>。

### 日志与会话

`[stack-probe] install ... mode=observe candidateOnly=true`显示实际hook安装及folded状态。
进入world后的level tick开启观测；无session只转发origin，不做NBT/descriptor检查或累计。
`[stack-probe] summary`每秒只打印有调用的窗口；键为caller/thread/route：

- `caller=0x... nativeImage=true`是主游戏exe的**return-address RVA（调用指令之后）**，可与反汇编对照。
  nativeImage=false则字段为明确的绝对地址（可能是其他模组/LL跳板），不能伪装成游戏RVA。
- `thread`为Win32线程ID；route区分Item/ComponentItem。若ComponentItem又调用Item，两个native入口分别计数，不能将其总数当独立事务数。
- `calls/resolvedCandidate/unresolved/different/copyHits`及`native=[64=次数] would=[60=次数] ammo=[tacz:762x39=次数]`；
  would仅对候选可读，没有AmmoId时不猜60。`resolution=[candidate/missing/expired/fingerprint-mismatch=次数]`解释漏命中。
  would沿用本模组的Bedrock1..64归一化（大于64的枪包声明仍按64），不是本轮放宽堆叠声明。
  ammo直方图保留最多8个具体ID，其余归other；最多1024个caller/thread/route桶，溢出计droppedCallers。
- `[stack-probe] total`为退出world时各caller的累计，`world-exit`报告生产/复制/构造、容量清理、错误及丢弃桶数，然后清空缓存/累计。
  ClientShooter关闭world token后同步清理，BDS由ServerStopping清理，disable也清理；不跨世界复用指针键。

`[stack-probe] bridge`在有生产/采样活动的秒窗口报告本world累计producer/published/copies/invalidated及缓存大小，
即便没有inner getter命中也能区分“producer在运行但getter没覆盖”。producer包括非ammo描述符；published包括观测复制产生的候选发布，均不是独立游戏事务数。

未额外扫描/改写机器码追踪所有direct/inlined mMaxStackSize读取；若某路径完全没有getter日志，不能据此说它没读上限。
日志是覆盖率/候选命中测量，不是行为验收；探针会增加锁、分配和计数开销，可能影响时序。

对照0.3.25中同一1.26.51.01二进制的已知call-site，可预期这些**调用后return RVA**：
set `0x24870f1`，isFullStack `0x2488c01`，isStackable `0x2488ce8`，
_getSlotWithRemainingSpace `0x25a4835`，_addResource source/target `0x25a3ebb/0x25a4051`。
这是已有反汇编的核对表，不是本轮新运行日志，也没有用这些地址安装hook。

### 用户测试顺序（两模式分别做，记录动作时间）

1. **图标**：给几把不同枪/两种ammo，热栏0..8、背包、箱子互移、同类别不同枪快速切换。
   看slot-index的via=findCollectionIndex、inRange=true与逐槽变化的0..N；再看resolve/source/rendered。
2. **生存拾取/换弹**：满包仅留半满762x39槽，地面60；先部分拾取，再站在余量上射击/换弹。
   记录槽位回跳/迟同步，Q前后数量，必要时重进；测量版不保证该新问题已修复。
3. **箱子shift双向**：背包已有同种ammo时箱子→背包；再用箱子53、背包60+60复现反向shift。
   截取前后数量，核对是否再次出现箱子60+49、背包60+4。
4. **手动拖拽/拆分**：左右键拆堆、合并到部分堆；**漏斗**在两个容器间转移同种ammo，等待至少2秒。
5. **创造模式**重复拾取、shift和拖拽；按原版允许丢弃放不下的余量，不将其单独算为生存丢弹。
6. 每个动作后等1–2秒，让summary输出；最后退世界回主菜单取得total/world-exit，再进另一个世界看是否新累计。
   回传完整新日志，重点关键词：`slot-index`、`resolve/source/rendered`、`ammo-pickup`、`stack-probe`。

Part2验证：双端Release及191项测试／917,607条断言通过，diff检查通过；包括桥键/线程/TTL/复制/复用失效/容量、窗口与累计统计纯测试。
确认游戏未运行后已部署0.3.28/client（含两部分），DLL/PDB/manifest与RP/BP全文件哈希匹配。
最终DLL SHA256：`d69869789a5d8f13c212f94ade12994e2aa0581f952dc4c0d08b8eaab8aba435`。
本轮未启动游戏，安装状态、实际索引、命中率及summary/total均待新日志验证；不把编译通过当成A1覆盖证据。
