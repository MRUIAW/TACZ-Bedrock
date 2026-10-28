# 0.3.34：枪包同路径覆盖规则、实现及核验

**用户已批准按§3完整对齐，包括ammo index/上限与TWX共享Lua切换。§1–10保留为实施前调研快照，其中“当前/拟实施”指0.3.33调查时；最终生效规则、Java新实测与核验结果以§11之后为准。**
0.3.33用户已反馈飞行图标正常、未发现问题；本轮不回改该功能。

## 1. 结论先行

1. 确认HK416D声音差异的文件证据：ClassicRCCRP的`hk416d_shoot.ogg`为148254 B，默认包为69695 B。
   当前移植端按大小写敏感的路径排序，默认包最后；所有readFile均倒序取，所以选默认包。
2. **不能仅删除排序，也不能把所有资源统一改成first或last。** Java把全部枪包封在一个DelegatingPackResources里：
   单文件getResource取**第一个实际包含该路径**的委托（不是第一个有该namespace就停止）；
   listResources向同一回调顺序提交所有委托，FallbackResourceManager.listResources对同ID执行put，故**最后一个**委托赢。
3. 模型/动画LazyJsonDataManager先扫描到ID，但实际内容通过getResource重新读取，因此是**first**，不是last。
   声音读取扫描结果缓存的Resource，不重新按getResource选包，因此是**last**。
4. listMatchingResourceStacks保留全部同ID内容；TACZ配件tag、配方filter、loot注入各自合并。
   **Forge47.3.19的getResourceStack另有getChildren补丁**，再经Lists.reverse，语言按key合并时同namespace内反而是**first优先**。
5. 全量扫描5包11589个文件、11101个不同相对路径；429个路径重复，其中**100个路径存在不同SHA256**：
   assets17、data81、非资源根文件2。不是只有17个声音/美术冲突。
6. 对当前已实现的消费路径，按本文规则改后预计**17个原始资源选择改变**（11声音、2弹药display、3弹药index、1 Lua脚本）；
   3个同路径弹药PNG与1个模型仍应选默认包。配方/tag等尚未实现，不能写成“当前已应用默认包”。
   另外存在4个跨namespace语言键冲突，见§6，不能仅靠枪包first/last推断最终Java翻译。

## 2. 环境、次序与证据来源

### 2.1 实际安装包

目录：`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz/gunpacks`，F盘实测为NTFS。
本机Python os.scandir和JDK17的Files.newDirectoryStream各独立只读枚举，结果一致：

| 简称 | Java/原生枚举位置 | 目录 |
| --- | ---: | --- |
| C | 1 | `Cyber Armorer-1.1.5_for114` |
| D | 2 | `tacz_default_gun` |
| T | 3 | `[Tacz1.1.5-]TWX_T1.1.0[无配件]` |
| R | 4 | `[TACZ1.1.8+Arcana]ClassicRCCRP 1.1.6_release_hotfix2` |
| M | 5 | `[Tacz1.1.8+]MS-Mobius Gunspack v1.5.8` |

- Java扫描顺序：**C,D,T,R,M**。当前C++大小写敏感排序：**C,R,T,M,D**。
- `std::filesystem::path`比较不能拿Python WindowsPath的默认排序代替；后者会casefold。本表C++顺序按其大小写敏感比较核对。
- `_reference/TACZ/src/main/java/com/tacz/guns/resource/GunPackLoader.java:230–245`使用newDirectoryStream，无sort；
  `:94–118`顺序追加委托，`:121–134`只注册一个`tacz_resources`、Position.BOTTOM。
- **BOTTOM指外层Pack优先级，不等于内部委托反转。** 外层资源包/datapack可继续覆盖本枪包集合；
  本文“Java选中”限定为当前5包、扫描顺序如上、无额外高优先级同路径资源/过滤器/第三方mixin改规则。

### 2.2 权威实现与行号约定

- TACZ参考树：`_reference/TACZ`，HEAD `b482eff8c94a733ac8d0910193fca3893954027c`；
  `build.gradle:33`为1.1.8-hotfix2，`gradle.properties:3`为Minecraft1.20.1 / Forge47.3.19。
  原有`gradle/wrapper/gradle-wrapper.properties`修改是已有参考树状态，本轮未动。
- Forge源码直接读取本机缓存的`forge-1.20.1-47.3.19-sources.jar`，不是猜测其他版本：
  `C:/Users/MRUIAW/.gradle/caches/forge_gradle/maven_downloader/net/minecraftforge/forge/1.20.1-47.3.19/`。
- Minecraft实现直接从本机`joined-1.20.1-20230612.114412-srg.jar`指定class只读提取后用现有ForgeFlower反编译；
  原jar位于`.gradle/caches/forge_gradle/mcp_repo/net/minecraft/joined/1.20.1-20230612.114412/`。
  **不是把1.20.4/1.21代码当成1.20.1。** 临时研究产物均在
  `C:/Users/MRUIAW/AppData/Local/Temp/opencode/tacz-0.3.34-research/`，未放入项目代码。
- 下文`MC/`行号指该临时目录下`net/minecraft/`的反编译文本；`Forge/`指其中`net/minecraftforge/`的原源码。
  SRG方法对应：`m_213713_=getResource`，`m_213829_=getResourceStack`，
  `m_214159_=listResources`，`m_214160_=listResourceStacks`；
  FileToIdConverter的`m_247457_=listMatchingResources`、`m_246760_=listMatchingResourceStacks`。
  TACZ简写路径均相对`_reference/TACZ/src/main/java/com/tacz/guns/`。

### 2.3 资源管理器逐层核实

| 层/接口 | 文件:行 | 实际行为 |
| --- | --- | --- |
| FileToIdConverter | `MC/resources/FileToIdConverter.java:28–34` | 两种listMatching分别直接转发listResources/listResourceStacks，没有再做first选择 |
| 外层多包管理器 | `MC/server/packs/resources/MultiPackResourceManager.java:24–53,72–101` | 按外层pack顺序构建namespace fallback；各namespace结果putAll，不重新调用getResource读取每个ID |
| 单文件查询 | `MC/.../FallbackResourceManager.java:62–80` | 外层从高到低优先，找到首个非空supplier即返回 |
| 单个TACZ委托包getResource | `Forge/resource/DelegatingPackResources.java:46–57,102–124` | 同namespace候选保持扫描顺序，依次试具体路径，首个非空supplier赢 |
| 单个TACZ委托包listResources | `Forge/resource/DelegatingPackResources.java:69–74` | 顺序遍历所有委托，不去重、不反转 |
| 扫描最高资源 | `MC/.../FallbackResourceManager.java:147–188`，关键`:168` | 外层低→高；每个委托重复提交同ID时map.put直接替换；同一个外层pack/index也照样替换。内部last赢 |
| 扫描所有资源 | `MC/.../FallbackResourceManager.java:254–302`，关键`:270` | 每次回调add Resource，保留所有委托；外层低→高、内部正序 |
| 单路径资源栈 | `MC/.../FallbackResourceManager.java:91–132` + `patches/net/minecraft/server/packs/resources/FallbackResourceManager.java.patch:3–23` | Forge将pack.getChildren按正序展开，外层高→低收集，最后Lists.reverse；TACZ内部顺序因此反转 |

以仅有D、R都包含的某路径p为例：

- `getResource(p)` → D。
- `listResources(...)[p]` → R，即使它们属于**同一个**外层Pack也如此。
- `listResourceStacks(...)[p]` → `[D,R]`。
- Forge `getResourceStack(p)` → `[R,D]`，语言随后put同key时D覆盖R。

这四项必须作为不同接口对待；不能用“资源包总是后者覆盖”概括。

## 3. 分类规则确认表

当前移植端共用`src/pack/GunPackLoader.cpp`（下简称Loader）：
`:171–172`收集并排序包；`:76–104` collect对ID后写覆盖；`:210–223` parseAll先collect再解析；
`:354–359` readFile倒序读；`:208` mAssetPaths只做存在性集合、不选内容。

| 资源类别 | Java检索链与文件:行证据 | 同路径冲突规则 | 当前移植端 | 是否不同 |
| --- | --- | --- | --- | --- |
| `assets/*/tacz_sounds/*.ogg` | `mixin/client/SoundManagerPreparationsMixin.java:18,23–27` listMatchingResources→cached Resource；`MC/client/sounds/SoundManager.java:56–59,315–318`缓存交SoundEngine；SoundBufferLibrary`:29–33`读取该provider | **last**；当前HK416D为R | `src-client/client/sound/GunSound.cpp:39`→readFile，全局倒序，当前D | 是：包次序不同；不能改成通用first后漏掉声音 |
| `assets/*/display/{guns,ammo,attachments,blocks}/*.json` | `client/resource/ClientAssetsManager.java:95–98`→DisplayManager`:21–28`→JsonDataManager`:48–49`→ResourceScanner`:34–49`直接解析扫描返回Resource | **last** | 枪display Loader`:263` parseAll；ammo display`:265–270`却走readFile；配件/方块display尚无完整消费者 | 是：目前两条代码路径恰都选D；应统一到扫描last语义 |
| `data/*/data/{guns,attachments,blocks}`及`index/*` | `resource/CommonAssetsManager.java:88–100`→CommonDataManager`:19`→JsonDataManager→ResourceScanner | **last**，服务端权威 | Loader`:229–231` collect/parseAll，枪data/index与ammo index已实现，其余未完整实现 | 规则形式同last，但包次序不同；当前3个ammo index会变 |
| `assets/*/geo_models/*.json` | ClientAssetsManager`:100–101`→LazyJsonDataManager`:65–80,118–123,140–148`：扫描只保存ID/路径，读取时getResource | **first**，eager/lazy都一样 | GunRenderer`:433`、GunAnimation`:248`→readFile，当前D | 一般规则不同；本组1个异内容模型恰仍应为D |
| `assets/*/animations/*.animation.json`、`.gltf` | ClientAssetsManager`:102–104`，GltfManager`:15–20`→同一LazyJsonDataManager；InternalAssetLoader`:68–80`的默认动画/旧内部模型也open→getResource | **first** | `src/pack/GunAnimations.cpp:15`→readFile；gltf尚未实现 | 一般不同；5包没有动画同路径异内容冲突 |
| 纹理PNG（枪UV、槽图、弹壳等） | TACZ `client/renderer/item/AmmoItemRenderer.java:74,88`与ClientAmmoIndex`:160–182`引用ResourceLocation；`MC/client/renderer/texture/SimpleTexture.java:81–88`→ResourceProvider`:15–20`→getResource；不是list结果内容 | **first** | GunRenderer`:435`、GunHud`:169`→readFile，当前D | 一般不同；3个同路径ammo PNG当前D恰正确，应保留D，而非改成R |
| `data/*/scripts/*.lua`、`assets/*/scripts/*.lua` | CommonAssetsManager`:84,95`、ClientAssetsManager`:105`→ScriptManager`:45–58,69–81`直接持有并打开扫描返回Resource | **last** | Loader`:225–227` collect，当前D | 次序不同；真实冲突xmag脚本应选T |
| `assets/*/gunpack_info.json` | `client/resource/manager/PackInfoManager.java:29–33`逐namespace getResource | **first** | Loader`:240–249`逐包读并覆盖mPackDisplayNames，当前排序last | 一般不同；当前5包无此同路径冲突 |
| 语言`assets/*/lang/<locale>.json` | `MC/client/resources/language/ClientLanguage.java:35–41,51–57`→getResourceStack→Forge children补丁；逐文件逐key put。Forge服务器LanguageHook`:74–80`也取资源栈 | **合并所有文件的键；同namespace/locale重复键first优先**。外层pack仍高优先级后写 | Loader`:232–260` builtin打底、逐包逐文件直接后写；PackItems`:102–109`语言归一化/fallback | 是，不能误用listResourceStacks顺序；跨namespace同key另见§6 |
| TACZ配件tag `data/*/tacz_tags/attachments/**` | ResourceScanner`:74–84` listMatchingResourceStacks→AttachmentsTagManager`:39–45,54–80` | **全部正序读取，列表合并后按tag/allow_attachments集合去重**；不是last一份文件 | 没有消费者；原始readFile若有人调用仍只会取一份D，不能等同“已加载” | 功能尚未实现；必须保留all接口语义供后续使用 |
| `data/*/recipe_filters/*.json` | RecipeFilterManager`:38–44,53–65`→scanDirectoryAll→RecipeFilter.merge | **合并全部filter**，非单一winner | 尚未实现 | 同上，不在本轮覆盖修复中顺带新增合成系统 |
| `data/*/tacz_loot_injectors/*.json` | LootInjectionManager`:28–44`→scanDirectoryAll，逐项向目标loot table追加 | **全部注入** | 尚未实现 | 同上；当前无此同路径异内容冲突 |
| Minecraft配方`data/*/recipes/*.json` | `MC/world/item/crafting/RecipeManager.java:34–42`继承SimpleJsonResourceReloadListener；后者`:33–45`直接解析listMatchingResources的Resource | **last**一份，不合并同ID配方；filter是另一机制 | 尚未实现/不消费这些文件 | 当前25个异内容配方不能列成“D已生效” |
| Minecraft原版tag `data/*/tags/**` | `MC/tags/TagLoader.java:39–60` listMatchingResourceStacks，支持TagFile.replace；Forge补丁还支持remove | **所有栈按顺序处理**，replace清前值、remove按Forge规则；不同于TACZ的字符串集合tag | 尚未实现 | 无当前异内容冲突，需避免未来混用last接口 |
| player-animator兼容动画 | `compat/playeranimator/animation/PlayerAnimatorAssetManager.java:60–72`直接解析listMatchingResources结果 | **last**（不能因“动画”二字就套模型动画的first） | 尚未实现该资源系统 | 当前无相关同路径异内容冲突 |
| 根`gunpack.meta.json` | GunPackLoader`:175–190,198–219`各包独立读取，用于准入及依赖检查 | **逐包，非覆盖资源** | Loader`:185–205`逐包；仅日志提示依赖、不做Forge mod版本验证 | 覆盖无关；依赖校验能力本来不同，不应当作同ID冲突去重 |
| 根README等 | 无TACZ资源消费者 | 不参与资源合并 | 不加载 | 无覆盖问题 |

**待确认方案边界**：只修现有消费路径的选择与可复用检索规则，不借此实现配件、合成、战利品等未完成系统。
未实现类别在§5“实施后”仍记未加载；其Java目标规则已在本表明确，不能误报为全面功能等价。

## 4. 全包两两冲突检查

SHA256逐字节比较，包含所有namespace、assets及data，未仅筛tacz。表中“共有/不同”均为该对包内的路径数。

| 包对 | assets共有/不同 | data共有/不同 | 其他根文件共有/不同 |
| --- | ---: | ---: | ---: |
| C–D | 0/0 | 1/1 | 1/1 |
| C–T | 0/0 | 1/1 | 1/1 |
| C–R | 0/0 | 1/1 | 1/1 |
| C–M | 0/0 | 1/1 | 1/1 |
| D–T | 0/0 | 229/39 | 1/1 |
| D–R | 190/17 | 53/53 | 1/1 |
| D–M | 0/0 | 10/10 | 2/2 |
| T–R | 0/0 | 45/45 | 1/1 |
| T–M | 0/0 | 10/10 | 1/1 |
| R–M | 0/0 | 10/10 | 1/1 |

多包重复路径在各对中重复计数，不能把此表相加当唯一冲突总数。
唯一重复路径为assets190、data237、根文件2；唯一异内容路径为assets17、data81、根文件2。
另以大小写折叠相对路径检查，当前文件集中没有“只大小写不同”的路径别名。
文档§5的100条路径、所有提供者和每份字节数已由独立只读脚本重新对照实盘检查，无遗漏/多项。
没有`data/*/data/guns`、枪index、assets脚本、模型动画、同路径lang或gunpack_info的异内容冲突；
但共享脚本与ammo index会间接影响枪械行为/显示，不能由“枪data无冲突”推出玩法完全不变。

## 5. 每一个同路径异内容文件

下表列完**100个唯一相对路径**，尺寸为字节。`D+T+R`等表示Java读取全部并按§3消费者合并，不是最后一个包独占。
“实施后”指§3限定的现有功能覆盖修复；`—`为没有消费者，**不是被D覆盖**。
17个“变”是原始资源选择变化；2个display有未实现的3D字段，原始内容变不等于所有字段已有画面表现。

### 5.1 assets：17项

| 相对路径 | 各包尺寸B | Java | 当前 | 实施后 | 选择变化 |
| --- | --- | --- | --- | --- | --- |
| `assets/tacz/display/ammo/545x39_display.json` | D142 / R1157 | R | D | R | 变 |
| `assets/tacz/display/ammo/792x57_display.json` | D146 / R555 | R | D | R | 变 |
| `assets/tacz/geo_models/shell/57x28_shell.json` | D4898 / R4402 | D | D | D | 不变 |
| `assets/tacz/tacz_sounds/ak47/ak47_shoot.ogg` | D154772 / R167715 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/ak47/ak47_shoot_3p.ogg` | D32765 / R33894 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/ak47/ak47_silence.ogg` | D166095 / R171035 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/ak47/ak47_silence_3p.ogg` | D31298 / R31480 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/hk416d/hk416d_reload_end.ogg` | D20716 / R7441 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/hk416d/hk416d_shoot.ogg` | D69695 / R148254 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/hk416d/hk416d_shoot_3p.ogg` | D63305 / R31264 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/hk416d/hk416d_silence.ogg` | D63980 / R115088 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/hk416d/hk416d_silence_3p.ogg` | D55178 / R23956 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/scar_h/p05_ar_schotel_inspect_boltclose.ogg` | D14370 / R18808 | R | D | R | 变 |
| `assets/tacz/tacz_sounds/scar_h/p05_ar_schotel_inspect_magrattle.ogg` | D32469 / R37448 | R | D | R | 变 |
| `assets/tacz/textures/ammo/slot/545x39.png` | D3124 / R539 | D | D | D | 不变 |
| `assets/tacz/textures/ammo/slot/68x51fury.png` | D504 / R754 | D | D | D | 不变 |
| `assets/tacz/textures/ammo/slot/792x57.png` | D830 / R566 | D | D | D | 不变 |

### 5.2 data索引/脚本/filter/配方：30项

| 相对路径 | 各包尺寸B | Java | 当前 | 实施后 | 选择变化 |
| --- | --- | --- | --- | --- | --- |
| `data/tacz/index/ammo/545x39.json` | D130 / R111 | R | D | R | 变 |
| `data/tacz/index/ammo/57x28.json` | D115 / R104 | R | D | R | 变 |
| `data/tacz/index/ammo/792x57.json` | D114 / R103 | R | D | R | 变 |
| `data/tacz/scripts/xmag_reload_logic.lua` | D5685 / T15280 | T | D | T | 变 |
| `data/tacz/recipe_filters/default.json` | C77 / D476 / T43 / R476 / M77 | C+D+T+R+M合并 | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/12g.json` | D475 / T443 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/308.json` | D460 / T422 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/30_06.json` | D379 / T347 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/338.json` | D472 / T440 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/357mag.json` | D380 / T348 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/40mm.json` | D468 / T440 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/45_70.json` | D474 / T442 / R324 | R | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/45acp.json` | D378 / T347 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/46x30.json` | D378 / T347 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/50ae.json` | D473 / T441 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/50bmg.json` | D553 / T521 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/545x39.json` | D386 / R324 | R | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/556x45.json` | D386 / T348 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/57x28.json` | D473 / T442 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/58x42.json` | D385 / T347 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/68x51fury.json` | D389 / T351 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/762x25.json` | D379 / T348 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/762x39.json` | D386 / T348 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/762x54.json` | D386 / T348 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/792x57.json` | D386 / R442 | R | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/9mm.json` | D376 / T345 | T | — | — | 尚未实现 |
| `data/tacz/recipes/ammo/rpg_rocket.json` | D476 / T448 | T | — | — | 尚未实现 |
| `data/tacz/recipes/attachments/laser_compact.json` | D345 / T335 | T | — | — | 尚未实现 |
| `data/tacz/recipes/attachments/laser_lopro.json` | D344 / T245 | T | — | — | 尚未实现 |
| `data/tacz/recipes/attachments/laser_nightstick.json` | D348 / T245 | T | — | — | 尚未实现 |

### 5.3 TACZ配件tag：51项

每行Java列都是**合并集合**，没有单独赢家；当前与拟议现有功能修复后均“未加载”。
重复字节相同的D/T版本仍列出，因合并需知道全部提供者；Java最终集合会去重。

| 相对路径 | 各包尺寸B | Java合并提供者 | 当前 / 实施后 |
| --- | --- | --- | --- |
| `data/tacz/tacz_tags/attachments/allow_attachments/aa12.json` | D186 / T120 / R21 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/ak47.json` | D155 / T177 / R24 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/aug.json` | D158 / T140 | D+T | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/db_long.json` | D69 / T30 | D+T | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/db_short.json` | D96 / T57 | D+T | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/glock_17.json` | D137 / T137 / R52 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/hk416d.json` | D203 / T203 / R228 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/m1014.json` | D147 / R27 | D+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/m16a4.json` | D223 / T223 / R25 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/m4a1.json` | D226 / T226 / R25 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/m870.json` | D123 / T55 | D+T | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/p90.json` | D174 / T174 / R27 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/rpk.json` | D126 / T148 | D+T | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/scar_h.json` | D167 / T167 / R26 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/scar_l.json` | D173 / T173 / R26 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/sks_tactical.json` | D175 / T197 / R27 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/spas_12.json` | D191 / R27 | D+R | — / — |
| `data/tacz/tacz_tags/attachments/allow_attachments/vector45.json` | D307 / T272 / R54 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/ammo_mod.json` | D95 / T95 / R47 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/ammo_mod_no_he.json` | D72 / T72 / R47 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/amr_muzzle_silencer.json` | D38 / T38 / R72 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/ar_laser.json` | D49 / T49 / R54 / M130 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/bayonet_ak.json` | D26 / T26 / R71 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/deagle_sight.json` | D23 / T23 / R163 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/extended_mag.json` | D81 / T81 / R27 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/grip.json` | D306 / T306 / R90 / M281 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/grip_afg.json` | D75 / T75 / R182 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/grip_vertical.json` | D214 / T214 / R72 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/hk416d_stock.json` | D140 / T140 / R173 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/light_extended_mag.json` | D99 / T99 / R21 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/m16a1_scope.json` | D29 / T29 / R73 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/m4a1_stock.json` | D170 / T170 / R427 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/muzzle.json` | D277 / T277 / R211 / M206 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/muzzle_brake.json` | D131 / T131 / R117 / M99 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/muzzle_compensator.json` | D41 / T41 / R94 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/muzzle_shotgun.json` | D125 / R54 | D+R | — / — |
| `data/tacz/tacz_tags/attachments/muzzle_silencer.json` | D111 / T111 / R125 / M110 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/okp_scope.json` | D25 / T25 / R24 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/p90_scope.json` | D240 / T240 / R651 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/pistol_laser.json` | D56 / T56 / R26 / M70 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/pistol_muzzle.json` | D114 / T78 / R122 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/pistol_sight.json` | D179 / T179 / R94 / M26 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/pistol_silencer.json` | D114 / T78 / R80 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/scope.json` | D478 / T432 / R214 / M511 | D+T+R+M | — / — |
| `data/tacz/tacz_tags/attachments/scope_scope.json` | D102 / T78 / R318 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/scope_sight.json` | D279 / T257 / R147 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/scope_sniper.json` | D103 / T103 / R230 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/scope_springfield.json` | D28 / T28 / R33 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/sks_tactical_stock.json` | D150 / T150 / R191 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/sniper_extended_mag.json` | D102 / T102 / R21 | D+T+R | — / — |
| `data/tacz/tacz_tags/attachments/stock.json` | D236 / T236 / R197 / M222 | D+T+R+M | — / — |

### 5.4 非覆盖资源的根文件：2项

| 路径 | 各包尺寸B | Java / 当前 / 实施后 |
| --- | --- | --- |
| `gunpack.meta.json` | C36 / D27 / T26 / R25 / M25 | 各包独立读取；无赢家，无覆盖变化 |
| `README.txt` | D385 / M256 | 都不加载；无赢家，无覆盖变化 |

校验样本SHA256：

| 文件 | D | R |
| --- | --- | --- |
| `assets/tacz/tacz_sounds/hk416d/hk416d_shoot.ogg` | `55abaa1c89accb3a0727515e9aff670ac741b340b4732525151afd0d469bedcd` | `99de4c4bcd49e8b17a876eb6c91335c19db2ec67377810dd44ddcd6dd384a7f1` |
| `assets/tacz/textures/ammo/slot/545x39.png` | `7630221e221e1de8d7d09a2f4cdb9547ed857340a3cdf4d36662b60f52529937` | `c0006702fb2d75ed8d4ade6781c65ed1bcef00900ffe8b0138b02bd9bf1bfdb4` |

## 6. 实施风险与尚不能仅由同路径规则推断的结果

### 6.1 三份ammo index会改变实际行为

| ID | 当前D | Java应使用R | 影响 |
| --- | --- | --- | --- |
| `tacz:545x39` | stack60，sort7 | stack60，无sort字段 | 堆叠不变，排序采用缺省值；display变R，但其slot仍引用同一路径PNG，PNG应继续用D |
| `tacz:57x28` | stack60，name=`tacz.ammo.57x28.name`，display=`tacz:57x28_display` | **stack50**，name=`classicr.ammo.57x28.name`，display=`classicr:57x28_display` | 名称/排序/槽图引用/上限都会变；新slot为`classicr:ammo/slot/57x28`，不是3个tacz同路径PNG之一 |
| `tacz:792x57` | stack36，sort13 | **stack30**，无sort字段 | 上限/排序改变；display变R，slot仍为tacz路径，PNG继续D |

对已有60发5.7或36发7.92的旧栈，不应为资源优先级修复偷偷做库存截断、丢弃、改NBT或“迁移”。
以后实施时须测试超新上限旧栈的give/合并/拆堆/箱子/漏斗行为，明确既有桥仍按新配置执行；
若需要存档迁移，应另行得到授权。7.62x39的60上限没有同路径index冲突，0.3.32已验收的同步代码不应因此改写。

### 6.2 Lua选择确实会换，不只日志上的packName变化

`data/tacz/scripts/xmag_reload_logic.lua`：D 5685 B → T 15280 B，SHA256分别为
`5f25d34f7866fa928d2c937d52a2a5e14c47adce1cbf727d5b58114f33755f46`、
`3068616e67a16380a5470748e5b9e2f91082623100028cf37d631372e87d94ec`。

- 两者导出同名start_reload/tick_reload，内容不是单纯格式差异。
  T在start_reload缓存needed_count/is_tactical/reloaded，tick使用cache与不同装填处理；D在阶段切换时读取需要量并装填。
- 扫描到18份D、79份R、8份M的枪data JSON直接引用这个ID，共**105份文件**，包含AK47、HK416D等；
  这是文件引用数，不保证105把都是最终注册可用枪。
- 因此Java兼容的完整修复会切换共享逻辑来源，需要单独跑Lua VM回归：战术/空仓、跨供弹槽、余弹不足、扩容等级、
  取消换弹、切枪，以及已通过的两项ammo sync。不能为规避变化偷偷保留D，同时宣称覆盖顺序已完整匹配Java。
- 这些变化来自所安装包本身的脚本选择，不应改动包原文件，也不应顺便修改已经接受的库存平衡/单槽包修复。

### 6.3 语言存在4个“不同路径、相同键”的冲突

这4项**不计入100个同路径冲突**。标准JSON解析独立扫描所有lang后发现：

| locale/key | D：`assets/tacz/lang/...` | R：`assets/ccrp/lang/...` |
| --- | --- | --- |
| en_us / `tacz.ammo.545x39.name` | `§e5.45x39mm Bullet` | `5.45x39mm Bullet` |
| en_us / `tacz.ammo.792x57.name` | `§b8mm Mauser Bullet` | `7.92x57mm Mauser` |
| zh_cn / `tacz.ammo.545x39.name` | `§e5.45x39mm 步枪弹` | `5.45x39mm步枪弹` |
| zh_cn / `tacz.ammo.792x57.name` | `§b8mm 毛瑟 步枪弹` | `7.92x57mm 毛瑟步枪弹` |

Java在每个locale内遍历resourceManager.getNamespaces()，依次把各namespace的语言写进同一全局map。
namespace集合/HashMap遍历**不是枪包优先级协议**，还会受所有外层pack/mod的namespace影响。
因此这里不能简单说“R后于D所以R赢”或“first所以D赢”；实际安装Java结果需按§9读取I18n值确认。
当前移植端逐包合并通常选D（D排序最后），并最终stripFormatting；这不证明与Java原始I18n一致。

建议把两件事分开确认：
1. 同namespace同路径lang：明确按Forge getResourceStack的内部逆序、逐key合并，first重复键优先。
2. 不同namespace重复翻译键：先记录用户Java实际字符串，再决定是否对这类没有稳定优先级约定的情况定义显式移植策略；
   不在覆盖修复里硬编码`ccrp`或模拟依赖所有外部mod容量的Java HashMap。

### 6.4 内置资源与外层包边界

- `src/pack/BuiltinAssets.cpp:6–13`、Loader`:226,232–238,358`使移植端内置动画/状态机/语言/图片作为fallback或打底。
  这不是完整Minecraft外层resource-pack栈；Java的mod资源、玩家选中资源包、服务端datapack仍可能比BOTTOM的tacz_resources优先。
- 额外核对当前包与移植端builtin重叠：default_state_machine.lua仅D提供，31940 B，与内置版本字节相同；
  en_us/zh_cn语言路径虽与D同名，但内置是额外键，两个locale分别**零重复key**；
  没有包与内置rifle/pistol默认动画或内置图片同路径重叠。
  因此不需要为当前案例改变内置fallback层级，也不能以此宣称任意未来builtin覆盖已等同Java。
- 资源来源报告不能只看sourcePackId：listResources产出的Resource常只标外层`tacz_resources`，
  不暴露具体D/R委托；定位应比较文件长度/哈希或选中内容。
- 无效/不可读文件：Java先选supplier，后打开/解析，胜出文件解析失败通常不会自动回退到另一份。
  实施时要选定来源后再解析，避免“坏的最后一份”触发与Java不同的隐性fallback。

## 7. Windows顺序：推荐原生枚举，不再另造排序

1. OpenJDK17 `WindowsDirectoryStream`构造调用FindFirstFile，迭代调用FindNextFile，无Java层排序。
   源码：<https://github.com/openjdk/jdk17u/blob/master/src/java.base/windows/classes/sun/nio/fs/WindowsDirectoryStream.java>。
   本机JDK17/jshell直接对同一个安装目录执行newDirectoryStream，实测C,D,T,R,M，与os.scandir一致。
2. NTFS目录是`$I30`索引/B-tree（小目录可驻留INDEX_ROOT），常规文件名collation基于卷`$UpCase`大写映射，
   **不是普通ASCII大小写敏感排序，也不是用户区域设置的自然排序**。
   <https://flatcap.github.io/linux-ntfs/ntfs/concepts/directory.html>。
   单纯tolower/Unicode casefold/CompareStringOrdinal(ignoreCase)不承诺与每个卷的NTFS表、同折叠名称的细节完全一致。
3. Win32官方也明确：FindFirstFile不排序，FindNextFile顺序由文件系统决定，NTFS通常按名称排序但**API不保证顺序**。
   <https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilew>
   <https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findnextfilew>。
4. 因此本机最接近Java的做法：保留directory_iterator的原生顺序，**只删除包列表的sort**。
   内部各pack.list为稳定输出而做的文件名sort不必删除；同一个委托中不存在跨包优先级，它和扫描包序是两回事。
   后续用JDK枚举与C++发现列表逐项对照，不靠“排序看起来差不多”。
5. Linux BDS以后同样可按原生目录枚举对齐“同机器上的Java”，但不能保证与Windows结果一样。
   若要Windows客户端/Linux服务器跨平台确定性，应另行批准一个显式顺序清单/配置，由两端共享；
   不应伪称按Unicode排序就是Java跨平台规则。不能在本轮偷偷加配置或换成另一套全平台排序。

## 8. 拟实施方案（待用户确认，尚无代码改动）

### 8.1 文件与接口

| 拟改文件 | 目的 |
| --- | --- |
| `src/pack/GunPackLoader.h` / `.cpp` | 保留原生发现顺序；区分直接查询、扫描胜出查询、扫描全部栈、单路径Forge语言栈；修ammo display与pack info，按namespace处理lang；保留来源信息便于诊断 |
| `src-client/client/sound/GunSound.cpp` | 声音显式使用“扫描胜出”的last规则，不跟随纹理的直接first接口 |
| `src/pack/GunAnimations.cpp`、`src-client/client/animation/GunAnimation.cpp`、`src-client/client/render/GunRenderer.cpp`、`src-client/client/ui/GunHud.cpp` | 检查/显式标注已有模型、动画、PNG调用为direct-first；若readFile改为direct默认，这些可能只需审计，不必改渲染逻辑 |
| 新`tests/PackOverrideTests.cpp`，必要时扩充`tests/DefaultPackTests.cpp` / `tests/GunLogicTests.cpp` | 合成最小fixture＋本机真实5包集成用例，分开验证规则与真实变更，不把本机绝对路径写死进通用测试 |
| 以后正式发布的xmake/tooth/BP/RP/版本测试、CHANGELOG/ROADMAP | 确认实施并验证后才同步0.3.34，记录0.3.33用户验收；本轮不改 |

建议接口按**Java检索方式**命名/枚举，例如Direct / Listed / ListedStack / ResourceStack，
而不是按`.json/.ogg/.png`猜规则：同是JSON的display和geo规则不同，同是动画的bedrock和playeranimator也不同。

- Direct：依发现顺序找首个包含路径的源；用于模型/动画/纹理、pack info。
- Listed：依顺序覆盖，返回最后源的内容；用于声音、data/index、display、Lua。
- ListedStack：保留每份、原顺序；为tag/filter/loot等后续消费者提供能力，但本次不新增这些玩法。
- ResourceStack：外层层级低→高、委托内部按Forge getResourceStack的反向顺序；用于语言逐key合并。
  **不是把整个所有外层包全局倒序**，否则玩家资源包优先级也会倒置。
- 修改`readFile`的当前“last universally”注释，更新所有调用点的意图；不要只修GunSound路径或给HK416D特判。
- collect/parseAll继续选定最后源后解析。ammo display从直接readFile改用Listed；否则新的direct默认会错误选择D。
- 语言保留语言fallback/格式处理的既有功能，不因优先级任务擅自改翻译文本、枪包文件或PNG。
- 合成/标签/战利品只留规则与测试，不在0.3.34覆盖修复中擅自实现尚未开发系统。

### 8.2 离线测试设计

1. **最小双包同namespace**：相同路径内容A/B。Direct=A，Listed=B，ListedStack=[A,B]，ResourceStack=[B,A]。
   首包有namespace但没有该文件时Direct必须继续找第二包；不同namespace的文件不能互相屏蔽。
2. 包名含大小写与`[`：发现结果与同目录JDK原生枚举逐项一致；load(vector)保留调用方给定次序。
   不让unit test断言所有OS都固定返回C,D,T,R,M；本机真实夹具才断言当前实测顺序。
3. **HK416D真实内容**：Listed选R，长度148254，SHA256=`99de4c4b...384a7f1`；
   Direct同路径则选D69695，用这个对照证明“一个路径也能因接口不同选不同内容”。
4. AK47四音效、HK其余四音效、SCAR两音效均比对R的完整字节；不是只测文件存在或字节数不同。
5. `geo_models/shell/57x28_shell.json` Direct应为D4898；3个ammo PNG Direct应为D（3124/504/830 B），
   加一个反例断言Listed对同路径返回R。防止删除sort后所有readFile继续last导致图片错误变R。
6. 两个ammo display Listed应为R；3个ammo index应为R，并断言57x28 name/display/stack50、792x57 stack30；
   57x28再沿引用链解析classicr display与slot，验证不是只改index来源标签。
7. Lua xmag应为T15280，与完整SHA256核对；用实际AK47/HK416D等script_params驱动GunLogic VM测试
   生存/创造、战术/空仓、供弹不足/跨槽、取消、扩容各等级，记录由源脚本切换导致的行为差异。
   不改原包脚本来让测试“通过”。
8. 同namespace语言双包fixture：独有键都保留，冲突键选first；en_us→选中locale仍正确。
   跨namespace键单独列为无稳定优先级案例，不把固定某个HashMap遍历顺序冒充标准。
9. 标签/filter/loot的资源栈fixture验证所有来源都在；不同文件相同key、后续replace等要由相应消费者测试，
   不用“最后字节来源正确”替代合并正确性。现有功能阶段不启动新的玩法系统。
10. 本机5包遍历所有429个重复路径，按类别逐项核对选中源/栈；100项异内容用§5快照防漏。
    未提交的枪包、zip或参考树不作为CI必需输入；完整本机测试通过可选外部目录参数启用，通用CI使用最小fixtures。

实施后再按正式要求双端构建、全套测试、diff检查；部署仅在游戏关闭时执行，另做实机听感/图标/供弹回归。
**本轮未进行这些实现/构建/部署步骤，没有新DLL。**

## 9. 需要用户核实的Java运行环境与具体步骤

源码链已确定；不确定的是用户实际运行的mod/jar、外层资源包/mixin，以及跨namespace语言最终值。
请使用一个测试世界，不移动/删除原包，不在正式存档里做清包实验。

### 9.1 无开发工具也能做

1. 提供Java客户端TACZ、Forge版本与最新日志的GunPackFinder扫描5包顺序；确认实际也是C,D,T,R,M。
   服务端若为独立服务器，再提供该服务器扫描日志（客户端与服务端的盘/目录可能不一样）。
2. 记下已启用资源包/服务端datapack和任何资源加载类mod，尤其高于TACZ Resources的覆盖包；
   `F3+T`重载客户端资源，在安静处对照HK416D普通/消音枪声、第三人称枪声与换弹尾声；
   AK47四种声音和SCAR检视声音也可顺带核对。已有“HK416D听感为Classic”视为用户提供证据，不要求重复证明耳朵。
3. 语言切到English和简体中文，查看5.45x39、7.92x57弹药名称；报告上表§6.3究竟哪种文字/颜色/空格生效。
   同时对照5.45、6.8、7.92槽图与D/R的原PNG缩略图；本规则预测这三个同路径PNG应为D。
4. 测试世界空背包，依次执行并观察拆堆上限/名称/图标：
   - `/give @s tacz:ammo{AmmoId:"tacz:57x28"} 60` → 预期新栈按50上限，且显示classicr弹药信息。
   - `/give @s tacz:ammo{AmmoId:"tacz:792x57"} 36` → 预期30上限。
   - `/give @s tacz:ammo{AmmoId:"tacz:545x39"} 60` → 预期60上限。
   服务端/give可能按其实现分发多个栈；判断重点是新栈上限及合并上限，不把总给予数量误认为单栈上限。
5. 用当前5包的AK47/HK416D分别做战术换弹、空仓换弹、供弹不足、取消换弹；
   这是将来切换TWX公共Lua前的Java行为基线，不是要求现在修任何同步代码。

### 9.2 若能在Java调试客户端Evaluate Expression：最直接的判定

不需要修改枪包/源码。在资源重载完成且进入世界后暂停客户端，使用IDE表达式求值/代码片段模式：

```java
var rm = net.minecraft.client.Minecraft.getInstance().getResourceManager();
var id = new net.minecraft.resources.ResourceLocation("tacz", "tacz_sounds/hk416d/hk416d_shoot.ogg");
var files = new net.minecraft.resources.FileToIdConverter("tacz_sounds", ".ogg");
```

分别对下表Resource执行`try (var in = resource.open()) { return in.readAllBytes().length; }`；
栈按返回顺序逐个读。不要长期打开InputStream，不要只打印sourcePackId（委托包可能同名）。

| 查询表达式 | 当前5包/无其他覆盖时预期 |
| --- | --- |
| `rm.getResource(id).orElseThrow()` | D，69695 B |
| `files.listMatchingResources(rm).get(id)` | R，148254 B |
| `files.listMatchingResourceStacks(rm).get(id)` | D69695 → R148254 |
| `rm.getResourceStack(id)`（Forge children补丁） | R148254 → D69695 |

然后把id换为`tacz:geo_models/shell/57x28_shell.json`、converter换成`new FileToIdConverter("geo_models", ".json")`：
Direct应4898，Listed应4402；这能直接区分模型的getResource读取与扫描返回资源。

再求值：

```java
net.minecraft.client.resources.language.I18n.get("tacz.ammo.545x39.name")
net.minecraft.client.resources.language.I18n.get("tacz.ammo.792x57.name")
```

分别在两种locale记录原始字符串，以决定§6.3跨namespace冲突政策。
若没有Java调试客户端，上面§9.1的截图/日志即可先确认主要行为；不要为了本次研究擅自安装新诊断mod。

### 9.3 还需明确的范围

- **客户端与服务端**：GunMod`:37–38`按物理side设置packType，CommonRegistry`:39–41`在AddPackFinders两种事件都注册同一loader，
  loader本身不按事件类型另外排序；PathPackResources/DelegatingPackResources按实际PackType读取assets或data。
  集成服务器通常扫描同一FMLPaths.GAMEDIR/tacz；独立服务端扫描自己的该目录。CommonAssetsManager`:234–242,271–280`
  表明枪data/index等以服务器为准并向客户端同步，不能把客户端本地文件优先级当成远端服务器数据权威。
- **zip**：Java GunPackLoader`:198–219,233–240`把zip与目录按同一次枚举顺序插入，之后SecureJar/PathPackResources
  读取；包之间first/last/stack规则不变。移植端Loader`:176–177`目前明确跳过zip，本轮只记录，不实现zip。
- **Linux**：未来原生枚举与NTFS不同是Java原实现本身的非跨平台确定性；若用户要求两端同序，必须另行确认显式顺序策略。
- **实际Java版本**：若用户所用jar不是参考树的1.1.8-hotfix2/Forge47.3.19，尤其模型是否仍使用LazyJsonDataManager、
  Forge是否仍含getChildren补丁，需以其版本重核。不能把本参考版本结论无条件套到所有TACZ版本。

## 10. 原调研审批项（现已批准）

用户批准按§3完整对齐现有消费者，包括ammo index/上限及TWX共享Lua；不只修声音。
随后补充了Java的枚举顺序、50/30/60堆叠上限、4个跨namespace语言键及5.7x28 tooltip实测，记录如下。

## 11. 用户Java结果与最终生效规则

### 11.1 Java结果

- 包顺序确认C,D,T,R,M。
- 57x28给予60→50+10，792x57给予36→30+6，545x39给予60→60。
- 跨namespace键最终采用D的文字：zh_cn为`§e5.45x39mm 步枪弹`、`§b8mm 毛瑟 步枪弹`；英文同样采用D。
- 5.7x28 tooltip为橙色`§65.7x28mm 穿甲弹`，随后`TAC | 弹药`、`TAC Zero 默认枪械包`；
  名称与display来自R的classicr引用，不使物品ID的namespace从tacz变成classicr。

### 11.2 最终规则与实现

- `GunPackLoader::loadFolder`只删除**包列表sort**，保留原生directory_iterator顺序；单包内部文件列表sort不改。
  server/client使用同一实现，没有另造服务器排序。
- `ResourceLookup::Direct`首个实际包含路径的源；`Listed`最后一个；`ListedStack`全部正序；
  `ResourceStack`按Forge children展开再reverse的内部逆序。内置资源在两种栈底部，单文件查询只作fallback。
  选中但打不开的文件不会偷偷改用另一包；选中后再解析，坏JSON不恢复旧版本。
- 枪/弹药display、common data/index、数据/动画Lua脚本使用Listed；声音加载显式Listed。
  模型、Bedrock动画、PNG均显式Direct；gunpack_info使用Direct。
- 语言在每namespace内通过ResourceStack逐key合并，所以重复键first委托赢、独有键保留。
- 没有为包名、路径实例、namespace或ID设赢家特例。标准目录如display/scripts/tacz_sounds是Java消费类别，不是某个包的优先级。
  配件tag/filter/loot/配方仍未新增游戏功能；已有栈接口可以表达其将来规则，核验报告明确标implemented=false。
- 库存extract/writeSelectedGun/InventorySync与描述符桥源码保持不变。57x28与792x57的新上限仅由选中index自然生效。
  **旧超上限栈不截断、不迁移、不改NBT**；新合并不向满/超限栈追加，新增拆堆/give按新上限。

### 11.3 跨namespace语言：明确的移植政策，不伪装Java保证

Java证据链：MultiPackResourceManager构造namespace HashMap（本机反编译`:24–55`），getNamespaces返回其keySet（`:68–69`）；
ClientLanguage按这个集合逐namespace写入同一个map。不同namespace同key的赢家依赖HashMap遍历，不存在资源包优先级协议。
例如Java17常规桶公式`(hash ^ (hash >>> 16)) & (capacity-1)`，在容量16/32时ccrp桶0/16、tacz桶2/18，
因此tacz可后写，符合本次用户实测；容量64时顺序又可能反过来。没有全部外层mod/namespace清单就不能证明用户实例的具体容量。

**采用通用、确定性政策**：
1. 翻译键以`namespace.`或`namespace:`开头时，该namespace拥有此键，其翻译优于其他namespace提供的同名键。
2. 没有拥有者时，按namespace字典序合并，字典序后者胜出。
3. namespace内仍严格使用Forge ResourceStack规则；包序不因该政策重新排序。

因此`<ns>.ammo...`自然优先取`assets/<ns>/lang/...`，无需特判tacz/ccrp或D/R。
合成测试用a/z namespace、反转包次序验证拥有者优先；无归属key也有稳定决胜测试。
这重现用户此次4键结果，但**不是承诺任意第三方Java mod组合的未规定HashMap顺序都相同**。
语言来源API同时遵循locale→en_us的真正选择，某locale命中builtin时不会误报英文外包来源。

### 11.4 5.7x28 tooltip核对

Java `item/AmmoItem.java:52–56`返回翻译Component，名称内部格式应保留；`:91–103`按ammoId查PackInfo，
ClientAssetsManager`:172–176`进一步使用ammoId.getNamespace，而不是index来自哪个包或display指向哪个namespace。
移植端现为：弹药tooltip保留翻译中的§颜色（枪标题和自定义名称策略不改），包名仍按原AmmoId namespace的Direct pack info解析。
合成测试使用home物品ID、remote翻译/display与其他来源的index，验证标题色、foreign槽图以及home包名同时正确，未写死任何真实枪包。

## 12. 生产解析器实盘核验报告

### 12.1 可复现命令

```powershell
xmake build tacz-pack-resolve
xmake run tacz-pack-resolve "F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz/gunpacks" "C:/Users/MRUIAW/AppData/Local/Temp/opencode/tacz-0.3.34-resolution.json" --reload-script tacz:xmag_reload_logic
```

工具源码`scripts/pack-resolve.cpp`直接使用生产GunPackRegistry、buildItemTooltip与GunLogic，不是另写一套猜测排序的脚本。
输入目录、输出路径和可选探测script ID均由调用方传入；没有嵌入真实包名、HK416D路径或预期赢家。
不写输入枪包、库存或存档。每个重复路径给出4种查询结果、实际消费者类型/选用规则、来源包、实际返回字节数与**FNV-1a-64**。
该hash是诊断摘要而非密码学校验；原调查的SHA256样本仍保留在§5。报告另含全部ammo派生上限/槽图/tooltip和语言值/来源。

实跑发现5包、429个重复路径，其中100个异内容，与研究快照一致；没有通过硬编码预期结果“修正”任何不匹配。

| 核验项目 | 生产结果 | Java对照 |
| --- | --- | --- |
| HK416D shoot | R，148254 B，FNV `66ef9b2ddc3f9af6` | 一致 |
| AK47四音效、HK其余四音效、SCAR-H两音效 | 均Listed选R，逐路径尺寸见完整报告 | 与研究的Java检索链一致；新DLL听感仍需用户测试 |
| 545x39 / 68x51fury / 792x57同路径PNG | Direct选D，3124 / 504 / 830 B | 一致 |
| tacz 57x28 shell模型 | Direct选D，4898 B | 一致；57x28新display另引用classicr模型是另一条引用链 |
| 57x28 / 792x57 / 545x39 index上限 | R，50 / 30 / 60 | 一致 |
| xmag_reload_logic.lua | Listed选T，15280 B，FNV `e2770684b076e174` | 一致 |
| zh_cn 5.45 / 7.92名称 | `§e5.45x39mm 步枪弹` / `§b8mm 毛瑟 步枪弹`，来源D的tacz语言文件 | 与用户文字、空格、颜色一致；泛化规则为§11.3移植政策 |
| en_us同两键 | D：`§e5.45x39mm Bullet` / `§b8mm Mauser Bullet` | 与用户确认来源一致 |
| 57x28 tooltip | `§r§f§65.7x28mm 穿甲弹§r` / `§9TAC | 弹药§r` / `§9§oTAC Zero 默认枪械包§r` | 一致，白色初始样式被内部§6覆盖为橙色 |
| 57x28 slot | `assets/classicr/textures/ammo/slot/57x28.png` | 符合R index→classicr display→slot引用链 |

### 12.2 实际TWX公共脚本的Lua VM探测

研究中的105是原始JSON引用数；实际注册可用、且引用该脚本的枪为**102把**。
对每把运行战术、空仓、战术供弹不足、空仓供弹不足、装填前取消、装填后取消，共**612次**，均成功载入/启动，0条Lua警告。
408个完成场景全部结束，204个取消场景按指定阶段停止；未连接Minecraft/网络，也没有声称实机已验收。

| 枪 | 战术（初始15+膛1） | 空仓 | 仅2发库存 | 取消 |
| --- | --- | --- | --- | --- |
| AK47 | 弹匣30+膛1，2000ms结束，消耗15 | 弹匣29+膛1，2600ms结束，消耗30 | 战术17+膛1；空仓1+膛1，库存归零 | 前取消不消耗；1550ms装填后取消保留装入量，不退款/重复扣 |
| HK416D | 弹匣30+膛1，2000ms结束，消耗15 | 弹匣29+膛1，2300ms结束，消耗30 | 同上 | 前取消不消耗；1300ms装填后取消保留装入量 |

探测每次创建隔离VM，传真实选中脚本和各枪script_params；库存只是计数回调，取消用现有interruptReload/clearScriptData且不再tick。
因此证明选中脚本在现有VM可执行、这些计数路径正常，**不证明世界中的切枪/动画/拾取时序已无回归**。
不修改TWX原脚本、不改玩法API或已接受的平衡库存写回。

### 12.3 单元测试与实盘报告的边界

新增规则测试只使用小型内存/临时目录fixture：顺序、同namespace缺路径继续查、不可读胜出文件不回退、
内置fallback、4种检索、跨namespace名称/display/pack info、缩小上限后的新拆堆与旧超限不变、语言政策和按ID加载Lua。
不要求用户安装这5包，不断言“Classic赢HK”；真实5包仅由上述独立命令审计。

## 13. 用户实机回归清单

1. HK416D、AK47普通枪声、换弹／SCAR-H检视声音与Java对照。消音/3p源文件虽能正确选中，
   **消音器触发、3p网络广播、每声音并发限制、0.8²音量曲线仍是后续音效任务**，本版不冒称完成。
2. 空背包give：57x28给予60→50+10；792x57给予36→30+6；545x39给予60→60。
   查看名称颜色、57x28 classicr槽图与“默认枪械包”footer。旧60/36超限栈不应被截断或改NBT；核对合并、拆堆、箱子/漏斗。
3. AK47/HK416D战术、空仓、仅少量弹药、取消（装填前/后）、切枪；新选中TWX脚本是批准的Java对齐，不恢复旧D脚本掩盖差异。
4. 已接受的无地面开火30后换弹扣弹、满包地面弹药连续换弹补满；必要时`/tacz rdebug ammosync 1`取日志，0关闭。
5. 快测实时开火弹量、丢枪/拾回、热栏/背包/箱子持续及shift飞行图标、tooltip、give和漏斗；退出/换世界基本回归。

## 14. Step B verification：d2efba6 与用户Java证据逐项对照

仅执行实盘核验及文档提交，不改代码行为、语言政策、版本、CHANGELOG/ROADMAP，不部署。
对原安装目录重新运行生产工具（不附加Lua探测参数）：

```powershell
xmake run tacz-pack-resolve "F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz/gunpacks" "C:/Users/MRUIAW/AppData/Local/Temp/opencode/tacz-0.3.34-stepB-resolution.json"
```

工具成功加载5包，返回429个重复路径、100个异内容冲突。按原字段重新序列化工具结果，
与现有[`PACK-OVERRIDE-0.3.34-verification.csv`](PACK-OVERRIDE-0.3.34-verification.csv)**逐字节相同**，故CSV未重写。
CSV保留全部429项、四种查询、消费者选择、字节数及FNV-1a-64；未实现的消费者仍标false，不能误读为已经生效。
下表index、语言、tooltip及slot均读取工具中**已加载registry的派生结果**，不是仅按目录顺序推测文件赢家。

| # | 结果 | 用户Java证据与本次生产结果 |
| --- | --- | --- |
| 1 | **PASS** | 顺序C→D→T→R→M，即Cyber Armorer→tacz_default_gun→TWX→ClassicRCCRP→MS-Mobius。 |
| 2 | **PASS** | `assets/tacz/tacz_sounds/hk416d/hk416d_shoot.ogg`由Listed选ClassicRCCRP，148254 B。 |
| 3 | **PASS** | 3个同路径ammo PNG（545x39、68x51fury、792x57）由Direct选默认包，分别3124/504/830 B；`assets/tacz/geo_models/shell/57x28_shell.json`亦为默认包，4898 B。 |
| 4 | **PASS** | 已加载57x28 index：stack50、name=`classicr.ammo.57x28.name`、display=`classicr:57x28_display`；792x57 stack30、545x39 stack60。 |
| 5 | **PASS** | `data/tacz/scripts/xmag_reload_logic.lua`由Listed选TWX，15280 B。 |
| 6 | **PASS** | 已加载zh_cn为默认包的`§e5.45x39mm 步枪弹`、`§b8mm 毛瑟 步枪弹`；en_us为默认包的`§e5.45x39mm Bullet`、`§b8mm Mauser Bullet`。d2efba6的“键前缀namespace拥有者优先、否则namespace字典序后者优先”政策确实得出这些值；本步未修改政策，也不把它宣称为Java HashMap的保证。 |
| 7 | **PASS** | ItemTooltip实际三行为`§r§f§65.7x28mm 穿甲弹§r`、`§9TAC \| 弹药§r`、`§9§oTAC Zero 默认枪械包§r`，颜色/文字/包名均PASS。registry的slot路径为`assets/classicr/textures/ammo/slot/57x28.png`，HUD按Direct读取该classicr路径，不是tacz路径。该文件实盘为**3210 B**，tacz默认PNG为**636 B**。（初次核对时标为MISMATCH，原因是主agent的核对请求把636/3210两个尺寸写反了，见下文更正。） |

第7项额外逐文件读取确认（两文件均32×32，不修改任何文件）：

| 包与路径 | 当前实盘字节数 | SHA256前16位 |
| --- | ---: | --- |
| ClassicRCCRP：`assets/classicr/textures/ammo/slot/57x28.png` | **3210** | `dcb73be4d5df24b2` |
| tacz_default_gun：`assets/tacz/textures/ammo/slot/57x28.png` | **636** | `4123cb0d0f473728` |

R的display原文明确写`"slot": "classicr:ammo/slot/57x28"`，工具派生slot与其一致；
当前安装目录仅R提供此classicr PNG，因此HUD的Direct查询读R的3210 B文件。

**更正（0.3.34收尾）**：主agent发给核对子任务的请求中把两个文件的字节数写反了（写成classicr 636 B、tacz 3210 B）。
实盘为classicr 3210 B、tacz 636 B，HUD加载的正是display指定的classicr文件，第7项判为PASS；游戏内图标与Java截图的对比留待实机验收。
不修改包内容或应用代码，工具无需修复。
以上是离线生产解析/tooltip构建结果，不代表新DLL已部署或完成游戏内显示、听感验收。
