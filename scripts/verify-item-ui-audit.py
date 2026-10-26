"""Read-only 0.3.24 item metadata/tooltip checks; not a native UI/pickup test."""
import json
from pathlib import Path
import sys

report = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
guns = {g["id"]: g for g in report["guns"]}
ammo = {a["id"]: a for a in report["ammoItems"]}
for identifier, gun in guns.items():
    assert gun["loaded"] and gun["item"].get("slot"), identifier
    for locale in ("en_us", "zh_cn"):
        assert gun["item"][locale] and "§" not in gun["item"][locale], identifier
        lines = gun["item"]["tooltip"][locale]
        assert lines[0] == "§r§f" + gun["item"][locale] + "§r", identifier
for identifier, item in ammo.items():
    assert item.get("slot"), identifier
    assert sum(item["give120"]) == 120, identifier
    assert all(0 < n <= item["stackBedrock"] <= 64 for n in item["give120"]), identifier
    assert "§" not in item["zh_cn"] and "§" not in item["en_us"], identifier
    assert item["tooltip_zh_cn"][0] == "§r§f" + item["zh_cn"] + "§r", identifier

ak = guns["tacz:ak47"]["item"]["tooltip"]["zh_cn"]
for line in ("§8经验等级：§50 (MAX)§r", "§8伤害: §b9§r",
             "§625% 原版护甲穿透§r", "§6150% 爆头伤害§r", "§c移动速度：-5.2%§r"):
    assert line in ak, line
assert any("31/31" in line for line in ak)
assert ammo["tacz:762x39"]["give120"] == [60, 60]
print(f"PASS {len(guns)} gun / {len(ammo)} ammo: slots, white localized titles, give120 conservation; AK47 Java-default lines")
print("Native screen coverage, font/image positioning and partial ground pickup remain unverified.")
