"""Read-only TACZ cube/face audit; indices are zero-based. No asset modifications."""
import json
import math
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "_reference/TACZ/src/main/resources/assets/tacz/custom/tacz_default_gun/assets/tacz/geo_models/gun/ak47_geo.json"


def sub(a, b):
    return tuple(x-y for x, y in zip(a, b))


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def rotate(p, r):
    x, y, z = p
    a, b, c = map(math.radians, r)
    y, z = y*math.cos(a)-z*math.sin(a), y*math.sin(a)+z*math.cos(a)
    x, z = x*math.cos(b)+z*math.sin(b), -x*math.sin(b)+z*math.cos(b)
    return (x*math.cos(c)-y*math.sin(c), x*math.sin(c)+y*math.cos(c), z)


def faces(c):
    o, s, p = c['origin'], c['size'], c.get('pivot', [0, 0, 0])
    d = c.get('inflate', 0)
    lo = [o[0]-p[0]-d, p[1]-o[1]-s[1]-d, o[2]-p[2]-d]
    hi = [lo[i]+s[i]+2*d for i in range(3)]
    points = [(lo[0], lo[1], lo[2]), (hi[0], lo[1], lo[2]), (hi[0], hi[1], lo[2]), (lo[0], hi[1], lo[2]),
              (lo[0], lo[1], hi[2]), (hi[0], lo[1], hi[2]), (hi[0], hi[1], hi[2]), (lo[0], hi[1], hi[2])]
    # BedrockCubePerFace directions, after FaceUVsItem.getFace remapping.
    indices = {'west': (5, 1, 2, 6), 'east': (0, 4, 7, 3), 'up': (5, 4, 0, 1),
               'down': (2, 3, 7, 6), 'north': (1, 0, 3, 2), 'south': (4, 5, 6, 7)}
    points = [tuple(v[i]+(p[0], 24-p[1], p[2])[i] for i in range(3)) for v in
              (rotate(v, c.get('rotation', [0, 0, 0])) for v in points)]
    for name, order in indices.items():
        if isinstance(c['uv'], list) or name in c['uv']:
            yield name, [points[i] for i in order]


def overlap(a, b, normal):
    # Separating axes of convex coplanar rectangles. Require AREA overlap, not a shared edge.
    for polygon in (a, b):
        for i in range(4):
            axis = cross(normal, sub(polygon[(i+1) % 4], polygon[i]))
            length = math.sqrt(dot(axis, axis))
            if length < 1e-12:
                continue
            aa, bb = [dot(p, axis)/length for p in a], [dot(p, axis)/length for p in b]
            if min(max(aa), max(bb))-max(min(aa), min(bb)) < 1e-6:
                return False
    return True


def main():
    bones = json.loads(SOURCE.read_text(encoding='utf-8'))['minecraft:geometry'][0]['bones']
    for bone in bones:
        if bone['name'] not in ('cover2', 'bolt', 'group3', 'group7', 'group6', 'lower2'):
            continue
        quads = [(i, name, q) for i, c in enumerate(bone.get('cubes', [])) for name, q in faces(c)]
        pairs = []
        for index, (i, name, a) in enumerate(quads):
            n = cross(sub(a[1], a[0]), sub(a[2], a[0]))
            length = math.sqrt(dot(n, n))
            if length < 1e-12:
                continue
            n = tuple(v/length for v in n)
            for j, other, b in quads[index+1:]:
                if i == j:
                    continue
                distances = [abs(dot(sub(v, a[0]), n)) for v in b]
                if max(distances)-min(distances) > 1e-5 or max(distances) > .002:
                    continue
                if overlap(a, b, n):
                    pairs.append((max(distances), i, name, j, other))
        print(bone['name'], 'area-overlapping face pairs within .002 pixels:', len(pairs))
        for distance, i, face, j, other in sorted(pairs)[:20]:
            print(f'  cube {i}.{face} / {j}.{other}: plane separation <= {distance:.9g} pixels ({distance/16:.9g} blocks)')


if __name__ == '__main__':
    main()
