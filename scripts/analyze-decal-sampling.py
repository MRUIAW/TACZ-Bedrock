"""Read-only installed-pack UV audit and PNG sampling controls (no Pillow).

python scripts/analyze-decal-sampling.py <gunpacks> <output-directory>
Output contains third-party crops: keep it outside this repository. These images
are sampling hypotheses/UV controls, NOT captures of Bedrock's GPU sampler.
"""
import json
import math
from pathlib import Path
import runpy
import struct
import sys
import zlib

png_rgba = runpy.run_path(str(Path(__file__).with_name("analyze-gun-alpha.py")))["png_rgba"]


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def write_png(path, w, h, pixels):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\0" + bytes(v for p in pixels[y*w:(y+1)*w] for v in p) for y in range(h))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def pixel(pixels, w, h, x, y, repeat):
    x = x % w if repeat else min(w-1, max(0, x))
    y = y % h if repeat else min(h-1, max(0, y))
    return pixels[y*w+x]


def sample(pixels, w, h, u, v, repeat, linear):
    if not linear:
        return pixel(pixels, w, h, math.floor(u*w), math.floor(v*h), repeat)
    x, y = u*w-.5, v*h-.5
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x-ix, y-iy
    taps = [pixel(pixels, w, h, ix+dx, iy+dy, repeat) for dy in range(2) for dx in range(2)]
    weights = [(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy]
    return tuple(round(sum(p[k]*a for p, a in zip(taps, weights))) for k in range(4))


def render(pixels, w, h, rect, ow, oh, repeat=True, linear=False):
    u1, v1, u2, v2 = rect
    return [sample(pixels, w, h, u1+(u2-u1)*(x+.5)/ow, v1+(v2-v1)*(y+.5)/oh, repeat, linear)
            for y in range(oh) for x in range(ow)]


def mip(pixels, w, h, levels=2):
    for _ in range(levels):
        nw, nh = max(w//2, 1), max(h//2, 1)
        pixels = [tuple(round(sum(pixel(pixels, w, h, x*2+dx, y*2+dy, True)[k]
                                  for dx in range(2) for dy in range(2))/4) for k in range(4))
                  for y in range(nh) for x in range(nw)]
        w, h = nw, nh
    return pixels, w, h


def main(root, output):
    if output.resolve().is_relative_to(root.resolve()):
        raise ValueError("output must be outside installed gunpacks (read-only audit)")
    output.mkdir(parents=True, exist_ok=True)
    def resource(identifier, kind, ext):
        ns, name = identifier.split(":", 1)
        paths = sorted(root.glob(f"*/assets/{ns}/{kind}/{name}{ext}"))
        if not paths:
            raise FileNotFoundError(identifier)
        return paths[-1]
    report, cards = [], []
    targets = [("ccrp:timeless_50_display", "pattern", 0, "west"),
               ("tacz:hk416d_display", "bone", 0, "west"),
               ("tacz:hk416d_display", "bone", 2, "west"),
               ("tacz:hk416d_display", "bone", 3, "east")]
    for identifier, bone_name, index, side in targets:
        display = json.loads(resource(identifier, "display/guns", ".json").read_text(encoding="utf-8-sig"))
        geo = json.loads(resource(display["model"], "geo_models", ".json").read_text(encoding="utf-8-sig"))["minecraft:geometry"][0]
        bone = next(b for b in geo["bones"] if b["name"] == bone_name)
        cube = bone["cubes"][index]
        face = cube["uv"][side]
        u, v = map(f32, face["uv"])
        du, dv = map(f32, face["uv_size"])
        tw, th = geo["description"]["texture_width"], geo["description"]["texture_height"]
        # BedrockPolygon.java:12-17: zero inset, description dimensions. Native
        # GeoModel.cpp:86-90 uses the same arithmetic; DecalUVTests checks its output.
        java = [f32(u/tw), f32(v/th), f32(f32(u+du)/tw), f32(f32(v+dv)/th)]
        native = [f32(u/tw), f32(v/th), f32(f32(u+du)/tw), f32(f32(v+dv)/th)]
        texture_path = resource(display["texture"], "textures", ".png")
        w, h, pixels = png_rgba(texture_path)
        label = identifier.replace(":", "-") + f"-{bone_name}-{index}-{side}"
        row = {"display": identifier, "bone": bone_name, "cube": index, "face": side,
               "geometry_size": [tw, th], "PNG_size": [w, h], "uv": face,
               "java_normalized_endpoints": java, "native_normalized_endpoints": native,
               "java_texel_endpoints": [java[0]*w, java[1]*h, java[2]*w, java[3]*h],
               "native_texel_endpoints": [native[0]*w, native[1]*h, native[2]*w, native[3]*h],
               "corner_order": "(u2,v1),(u1,v1),(u1,v2),(u2,v2)",
               "physical_size_model_pixels": cube["size"], "origin": cube["origin"],
               "UV_equal": java == native, "actual_native_sampler": "unverified, opaque SDK descriptor"}
        row["adjacent_png_mcmeta_exists"] = texture_path.with_suffix(".png.mcmeta").exists()
        raw_w, raw_h = max(1, math.ceil(abs(du)*w/tw)), max(1, math.ceil(abs(dv)*h/th))
        write_png(output/(label+"-crop.png"), raw_w, raw_h, render(pixels, w, h, java, raw_w, raw_h))
        # Common tiny screen footprint, enlarged without interpolation for comparison.
        ow, oh = 48, 12
        mp, mw, mh = mip(pixels, w, h)
        controls = [("java-point-repeat", render(pixels, w, h, java, ow, oh)),
                    ("native-UV-point-repeat-control", render(pixels, w, h, native, ow, oh)),
                    ("point-clamp-hypothesis", render(pixels, w, h, native, ow, oh, False)),
                    ("linear-clamp-hypothesis", render(pixels, w, h, native, ow, oh, False, True)),
                    ("mip2-linear-repeat-hypothesis", render(mp, mw, mh, native, ow, oh, True, True))]
        row["controls"] = [name for name, _ in controls]
        cards.append(controls)
        for name, image in controls:
            write_png(output/(label+"-"+name+".png"), ow, oh, image)
        report.append(row)
    # Five columns in the order above; four rows in target order. Checkerboard
    # alpha composite is for visibility only, not the actual underlying gun body.
    cw, ch = 200, 80
    sheet = []
    for y in range(ch*len(cards)):
        for x in range(cw*5):
            row, col = y//ch, x//cw
            lx, ly = x % cw, y % ch
            background = 40 if ((lx//8+ly//8) % 2) else 65
            p = (background, background, background, 255)
            if 4 <= lx < 196 and 16 <= ly < 64:
                source = cards[row][col][1][((ly-16)//4)*48+(lx-4)//4]
                a = source[3]/255
                p = (*[round(source[k]*a+background*(1-a)) for k in range(3)], 255)
            sheet.append(p)
    write_png(output/"sampling-controls.png", cw*5, ch*len(cards), sheet)
    (output/"report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
