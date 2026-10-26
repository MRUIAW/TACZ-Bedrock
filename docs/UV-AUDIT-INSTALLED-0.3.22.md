# 0.3.22 UV 越界离线审计

输入：`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz/gunpacks`；325 index / 325 loaded。
生产 GeoModel 的全部源面（包含隐藏弹匣和 hand locator；按枪计，复用模型不去重）。
验证生产 splitRepeatUV + native16位编码后的逐片插值，9个内部采样点/片；不等于实机/GPU/像素完全一致。
[0,1]内部有 ≤1/65535 正常量化；通过阈值2/65535。未修改源纹理或枪包。

| 物理包 | 枪数 | 解析面数 | 越界枪数 | 越界面数 | 其中非零UV面积 / hand locator | 分割/平移后片数 | 通过片数 | 最大UV误差 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Cyber Armorer-1.1.5_for114 | 35 | 57446 | 0 | 0 | 0 / 0 | 0 | 0 | 0 |
| [TACZ1.1.8+Arcana]ClassicRCCRP 1.1.6_release_hotfix2 | 164 | 679001 | 4 | 28 | 28 / 20 | 42 | 42 | 1.44838517e-05 |
| [Tacz1.1.5-]TWX_T1.1.0[无配件] | 14 | 108975 | 0 | 0 | 0 / 0 | 0 | 0 | 0 |
| [Tacz1.1.8+]MS-Mobius Gunspack v1.5.8 | 58 | 195325 | 0 | 0 | 0 / 0 | 0 | 0 | 0 |
| tacz_default_gun | 54 | 139967 | 2 | 1223 | 0 / 0 | 1223 | 1223 | 1.51398108e-05 |

## 每枪最坏源面（UV按description归一化；最多5项）

| GunId / 模型 | 越界/总面 | 骨 / parser quad | min → max | 超界量 |
|---|---:|---|---|---:|
| `ccrp:block_17` / `ccrp:gun/block_17_geo` | 1/1132 | pattern / 6 | [0.0, 0.505859375] → [0.5068359375, 1.0126953125] | 0.0126953125 |
| `ccrp:timeless_50` / `ccrp:gun/timeless_50_long_geo` | 1/1358 | pattern / 0 | [0.0, 0.7890625] → [0.533203125, 1.005859375] | 0.005859375 |
| `classicr:kar98` / `classicr:gun/kar98_geo` | 12/1859 | righthand_pos / 0 | [0.0, 1.5] → [0.5, 3.0] | 2 |
| `classicr:kar98` / `classicr:gun/kar98_geo` | 12/1859 | lefthand_pos / 0 | [0.0, 1.5] → [0.5, 3.0] | 2 |
| `classicr:kar98` / `classicr:gun/kar98_geo` | 12/1859 | righthand_pos / 2 | [1.5, 0.0] → [2.0, 0.5] | 1 |
| `classicr:kar98` / `classicr:gun/kar98_geo` | 12/1859 | righthand_pos / 3 | [0.5, 1.5] → [1.0, 2.0] | 1 |
| `classicr:kar98` / `classicr:gun/kar98_geo` | 12/1859 | lefthand_pos / 2 | [1.5, 0.0] → [2.0, 0.5] | 1 |
| `classicr:spr_15` / `classicr:gun/spr_15_geo` | 14/3435 | righthand_pos / 0 | [0.0, 0.75] → [0.25, 1.5] | 0.5 |
| `classicr:spr_15` / `classicr:gun/spr_15_geo` | 14/3435 | lefthand_pos / 0 | [0.75, 0.75] → [1.0, 1.5] | 0.5 |
| `classicr:spr_15` / `classicr:gun/spr_15_geo` | 14/3435 | lefthand_pos / 1 | [0.25, 0.75] → [0.5, 1.5] | 0.5 |
| `classicr:spr_15` / `classicr:gun/spr_15_geo` | 14/3435 | lefthand_pos / 5 | [0.5, 0.75] → [0.75, 1.5] | 0.5 |
| `classicr:spr_15` / `classicr:gun/spr_15_geo` | 14/3435 | righthand_pos / 2 | [1.0, 0.0] → [1.25, 0.25] | 0.25 |
| `tacz:cz75` / `tacz:gun/cz75_geo` | 731/2676 | mag6 / 4 | [0.0, -0.0078125] → [0.0, -0.0078125] | 0.0078125 |
| `tacz:cz75` / `tacz:gun/cz75_geo` | 731/2676 | mag6 / 5 | [0.0, -0.0078125] → [0.0, -0.0078125] | 0.0078125 |
| `tacz:cz75` / `tacz:gun/cz75_geo` | 731/2676 | mag6 / 11 | [0.0, -0.0078125] → [0.0, -0.0078125] | 0.0078125 |
| `tacz:cz75` / `tacz:gun/cz75_geo` | 731/2676 | mag6 / 14 | [0.0, -0.0078125] → [0.0, -0.0078125] | 0.0078125 |
| `tacz:cz75` / `tacz:gun/cz75_geo` | 731/2676 | mag6 / 15 | [0.0, -0.0078125] → [0.0, -0.0078125] | 0.0078125 |
| `tacz:glock_17` / `tacz:gun/glock_17_geo` | 492/1569 | mag / 4 | [0.0, -0.0009765625] → [0.0, -0.0009765625] | 0.0009765625 |
| `tacz:glock_17` / `tacz:gun/glock_17_geo` | 492/1569 | mag / 8 | [0.0, -0.0009765625] → [0.0, -0.0009765625] | 0.0009765625 |
| `tacz:glock_17` / `tacz:gun/glock_17_geo` | 492/1569 | mag / 9 | [0.0, -0.0009765625] → [0.0, -0.0009765625] | 0.0009765625 |
| `tacz:glock_17` / `tacz:gun/glock_17_geo` | 492/1569 | mag / 10 | [0.0, -0.0009765625] → [0.0, -0.0009765625] | 0.0009765625 |
| `tacz:glock_17` / `tacz:gun/glock_17_geo` | 492/1569 | mag / 11 | [0.0, -0.0009765625] → [0.0, -0.0009765625] | 0.0009765625 |

## 失败

[]

注意：classicr:kar98/spr_15 的最坏面是righthand_pos源占位面；枪上传修复不改原生手臂上传。
cz75/glock_17大部分为零UV面积面；数值越界不代表每个面此前都有可见贴花错误。
