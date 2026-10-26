"""Read-only PNG/face alpha audit; Python standard library, no pack modifications.
python scripts/analyze-gun-alpha.py <gunpacks-directory>
Reports alpha ranges, model UV vs PNG dimensions, and uv_rotation declarations.
"""
import collections
import json
import math
from pathlib import Path
import struct
import sys
import zlib


def png_rgba(path):
    raw = path.read_bytes()
    if raw[:8] != b"\x89PNG\r\n\x1a\n": raise ValueError("not PNG")
    at, chunks, palette, transparency = 8, [], b"", b""
    while at < len(raw):
        size = struct.unpack_from(">I", raw, at)[0]
        kind, data = raw[at+4:at+8], raw[at+8:at+8+size]
        at += size+12
        if kind == b"IHDR": w, h, depth, mode, compression, filtering, interlace = struct.unpack(">IIBBBBB", data)
        elif kind == b"IDAT": chunks.append(data)
        elif kind == b"PLTE": palette = data
        elif kind == b"tRNS": transparency = data
    if depth != 8 or interlace: raise ValueError("audit supports non-interlaced 8-bit PNG only")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[mode]
    packed = zlib.decompress(b"".join(chunks))
    rows, previous, at = [], bytearray(w*channels), 0
    for y in range(h):
        f, row = packed[at], bytearray(packed[at+1:at+1+w*channels]); at += 1+w*channels
        for x in range(len(row)):
            a = row[x-channels] if x >= channels else 0
            b = previous[x]
            c = previous[x-channels] if x >= channels else 0
            if f == 1: predictor = a
            elif f == 2: predictor = b
            elif f == 3: predictor = (a+b)//2
            elif f == 4:
                p = a+b-c; pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
            elif f == 0: predictor = 0
            else: raise ValueError("invalid PNG filter")
            row[x] = (row[x]+predictor) & 255
        rows.append(row); previous = row
    rgba = []
    for row in rows:
        for x in range(w):
            p = row[x*channels:(x+1)*channels]
            if mode == 6: color = tuple(p)
            elif mode == 2: color = (*p, 255)
            elif mode == 3:
                i = p[0]; color = (*palette[i*3:i*3+3], transparency[i] if i < len(transparency) else 255)
            elif mode == 4: color = (p[0], p[0], p[0], p[1])
            else: color = (p[0], p[0], p[0], 255)
            rgba.append(color)
    return w, h, rgba


def audit(root, namespace, display_name):
    matches = sorted(root.glob(f"*/assets/{namespace}/display/guns/{display_name}.json"))
    if not matches: return {"missing": f"{namespace}:{display_name}"}
    path = matches[-1]
    display = json.loads(path.read_text(encoding="utf-8-sig"))
    assets = path.parents[3]
    def resource(id, directory, ext):
        ns, relative = id.split(":", 1)
        # Resolve across installed packs, same last-file-wins as the registry.
        candidates = sorted(root.glob(f"*/assets/{ns}/{directory}/{relative}{ext}"))
        if not candidates: raise FileNotFoundError(id)
        return candidates[-1]
    geo = json.loads(resource(display["model"], "geo_models", ".json").read_text(encoding="utf-8-sig"))["minecraft:geometry"][0]
    w, h, pixels = png_rgba(resource(display["texture"], "textures", ".png"))
    tw, th = geo["description"]["texture_width"], geo["description"]["texture_height"]
    total, rotated, partial = 0, 0, []
    for bone in geo["bones"]:
        for cube in bone.get("cubes", []):
            faces = cube.get("uv", {})
            if not isinstance(faces, dict): continue
            for name, face in faces.items():
                total += 1
                rotated += "uv_rotation" in face
                u, v = face["uv"]; du, dv = face["uv_size"]
                if du == 0 or dv == 0: continue
                x0, x1 = math.floor(min(u,u+du)/tw*w), math.ceil(max(u,u+du)/tw*w)
                y0, y1 = math.floor(min(v,v+dv)/th*h), math.ceil(max(v,v+dv)/th*h)
                colors = collections.Counter(pixels[(y % h)*w+(x % w)] for y in range(y0,y1) for x in range(x0,x1))
                alpha = collections.Counter()
                for color, count in colors.items(): alpha[color[3]] += count
                if bone["name"] == "pattern" or any(0 < a < 255 for a in alpha):
                    partial.append({"bone": bone["name"], "parent": bone.get("parent"), "face": name,
                                    "uv": [u,v], "uv_size": [du,dv], "alpha": dict(sorted(alpha.items())),
                                    "top_rgba": colors.most_common(4)})
    return {"display": f"{namespace}:{display_name}", "enable_transparency": display.get("enable_transparency", False),
            "model": display["model"], "geometry_uv_size": [tw,th], "PNG": [w,h],
            "per_face_entries": total, "uv_rotation_entries": rotated, "fractional_or_pattern_faces": partial}


if __name__ == "__main__":
    root = Path(sys.argv[1])
    print(json.dumps([audit(root,"ccrp","timeless_50_display"), audit(root,"tacz","hk416d_display")], ensure_ascii=False, indent=2))
