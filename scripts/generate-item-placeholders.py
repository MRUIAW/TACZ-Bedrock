"""Generate original schematic placeholder icons; no third-party source images."""
from pathlib import Path
import runpy
ROOT = Path(__file__).resolve().parents[1]
write_png = runpy.run_path(str(ROOT / "scripts/analyze-decal-sampling.py"))["write_png"]
out = ROOT / "packs/resource_packs/tacz_rp/textures/items"
out.mkdir(parents=True, exist_ok=True)
for name in ("gun", "ammo", "attachment"):
    pixels = []
    for y in range(32):
        for x in range(32):
            ink = (name == "gun" and ((4 <= x < 29 and 11 <= y < 16) or (10 <= x < 15 and 16 <= y < 25)))
            ink |= name == "ammo" and (9 <= x < 23 and 9 <= y < 27 or 12 <= x < 20 and 5 <= y < 9)
            ink |= name == "attachment" and (5 <= x < 27 and 11 <= y < 20 or 10 <= x < 14 and 20 <= y < 24 or 19 <= x < 23 and 20 <= y < 24)
            color = (205, 210, 220, 255) if name == "gun" else (230, 180, 80, 255) if name == "ammo" else (100, 190, 210, 255)
            pixels.append(color if ink else (0, 0, 0, 0))
    write_png(out / f"tacz_{name}.png", 32, 32, pixels)
