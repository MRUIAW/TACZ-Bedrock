"""Assert0.3.23 offline regression against production tacz-pack-audit metadata."""
import json
from pathlib import Path
import sys

report = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
guns = {g["id"]: g for g in report["guns"]}
for identifier in ["ccrp:timeless_50", "ccrp:block_17", "tacz:ak47", "tacz:cz75", "tacz:glock_17"]:
    gun = guns[identifier]
    assert gun["loaded"] and not gun["geometry"]["uv"]["failures"], identifier
    assert gun["positionFrames"]["maxLocalRoundtrip"] == 0, identifier
    assert gun["item"]["slot"] and gun["item"]["en_us"] and gun["item"]["zh_cn"], identifier
    print(identifier, "UV/position frame/icon/lang OK")
for gun in guns.values():
    assert gun["positionFrames"]["maxLocalRoundtrip"] == 0, gun["id"]
hk = guns["tacz:hk416d"]["decalDepth"]["targets"]
for target in hk:
    assert target["origin"] == [-.375, 0, 0]
    nearest = target["nearbyParallelOverlaps"][0]
    gap = nearest["rebasedHalfGapBlocks"]
    assert gap != 0 and gap * nearest["gapBlocks"] > 0
    print("HK cube", target["cube"], "original/half/rebased-half gap:", nearest["gapBlocks"], nearest["halfGapBlocks"], gap)
print("PASS", len(guns), "guns: source positions survive origin roundtrip exactly; half is simulated, not runtime-proven")
