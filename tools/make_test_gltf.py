#!/usr/bin/env python3
"""Generate content/models/test_room.gltf: a 20 x 12 m floor plus a pillar and a
low wall, with node names that exercise the glTF tag conventions:

  Floor        -> walkable (name contains "floor")
  Pillar       -> obstacle (name contains "pillar")
  Bench        -> obstacle via extras {"crowdsim": {"tags": ["obstacle"]}}

Coordinates are glTF's (right-handed, +Y up); the importer flips Z.
"""

import base64
import json
import struct
import sys
from pathlib import Path


def box(min_xyz, max_xyz):
    (x0, y0, z0), (x1, y1, z1) = min_xyz, max_xyz
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    # Outward CCW faces (glTF convention).
    f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (3, 6, 2), (3, 7, 6), (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5)]
    return v, f


def quad(x0, z0, x1, z1, y):
    return [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)], [(0, 2, 1), (0, 3, 2)]


def main(out_path: Path) -> None:
    meshes = {
        "Floor": quad(-10, -6, 10, 6, 0.0),
        "Pillar": box((-0.4, 0, -0.4), (0.4, 3, 0.4)),
        "Bench": box((-2, 0, -0.4), (2, 0.5, 0.4)),
    }
    placements = {"Floor": (0, 0, 0), "Pillar": (5, 0, 0), "Bench": (-5, 0, 3)}

    blob = bytearray()
    accessors, views, gltf_meshes, nodes = [], [], [], []
    for name, (verts, faces) in meshes.items():
        # Positions.
        pos_offset = len(blob)
        for v in verts:
            blob += struct.pack("<3f", *v)
        views.append({"buffer": 0, "byteOffset": pos_offset, "byteLength": len(verts) * 12})
        mins = [min(v[i] for v in verts) for i in range(3)]
        maxs = [max(v[i] for v in verts) for i in range(3)]
        accessors.append({"bufferView": len(views) - 1, "componentType": 5126, "count": len(verts),
                          "type": "VEC3", "min": mins, "max": maxs})
        pos_acc = len(accessors) - 1
        # Indices.
        idx_offset = len(blob)
        for f in faces:
            blob += struct.pack("<3H", *f)
        while len(blob) % 4:
            blob += b"\0"
        views.append({"buffer": 0, "byteOffset": idx_offset, "byteLength": len(faces) * 6})
        accessors.append({"bufferView": len(views) - 1, "componentType": 5123, "count": len(faces) * 3,
                          "type": "SCALAR"})
        gltf_meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": pos_acc},
                                                          "indices": len(accessors) - 1}]})
        node = {"name": name, "mesh": len(gltf_meshes) - 1, "translation": list(placements[name])}
        if name == "Bench":
            node["extras"] = {"crowdsim": {"tags": ["obstacle"]}}
        nodes.append(node)

    doc = {
        "asset": {"version": "2.0", "generator": "crowdsim tools/make_test_gltf.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": gltf_meshes,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()}],
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else Path("content/models/test_room.gltf"))
