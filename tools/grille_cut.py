"""Cut a hex-packed speaker grille into a shell STL using a robust manifold
mesh boolean (trimesh + manifold3d). Output is watertight-guaranteed.

Why this and not FreeCAD: FreeCAD's mesh boolean mangles thin curved meshes
(leaves cylinder-wall spikes) and its solid boolean times out on the ~78k-facet
dome. trimesh's manifold engine handles dense meshes cleanly and fast.

Setup (once):  python -m pip install trimesh manifold3d numpy

Usage:
    python tools/grille_cut.py                     # defaults below
    python tools/grille_cut.py --hole-dia 2.5 --pitch 4.0
    python tools/grille_cut.py --cx 104 --cy 136 --area-r 17 -o assets/out.stl

Coordinate note: params are in the STL's own frame. For assets/Back-shell.stl
the grille sits over a magnet post at (104, 136); that frame is offset (+79,+83)
from the copy loaded inside the FreeCAD document.
"""
import argparse
import math
import os

import trimesh


def build_hole_centers(cx, cy, area_r, pitch):
    """Hex-packed hole centers within a circle of radius `area_r`."""
    row_h = pitch * math.sqrt(3) / 2.0
    nr = int(area_r / row_h) + 2
    nc = int(area_r / pitch) + 2
    centers = []
    for j in range(-nr, nr + 1):
        y = cy + j * row_h
        xoff = (pitch / 2.0) if (j % 2) else 0.0
        for i in range(-nc, nc + 1):
            x = cx + i * pitch + xoff
            if (x - cx) ** 2 + (y - cy) ** 2 <= area_r ** 2:
                centers.append((x, y))
    return centers


def cut_grille(src, out, cx, cy, area_r, hole_dia, pitch, z0, z1):
    base = trimesh.load(src)          # process=True merges duplicate STL verts
    base.merge_vertices()
    trimesh.repair.fix_normals(base)
    trimesh.repair.fill_holes(base)
    print("base:   faces=%d watertight=%s volume=%.1f"
          % (len(base.faces), base.is_watertight, base.volume))
    if not base.is_watertight:
        print("WARNING: base mesh is not watertight; boolean may be unreliable.")

    centers = build_hole_centers(cx, cy, area_r, pitch)
    print("holes: ", len(centers))

    # one clean closed-volume cylinder per hole, spanning through the wall
    r = hole_dia / 2.0
    h = z1 - z0
    cyls = []
    for (x, y) in centers:
        c = trimesh.creation.cylinder(radius=r, height=h, sections=48)
        c.apply_translation([x, y, (z0 + z1) / 2.0])
        cyls.append(c)

    # robust boolean: base minus all cutters, via the manifold engine
    result = trimesh.boolean.difference([base] + cyls, engine="manifold")
    print("result: faces=%d watertight=%s volume=%.1f"
          % (len(result.faces), result.is_watertight, result.volume))

    result.export(out)
    print("wrote:  %s (%.2f MB)" % (out, os.path.getsize(out) / 1e6))


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-i", "--src", default=os.path.join(repo, "assets", "Back-shell.stl"),
                   help="input STL (default: assets/Back-shell.stl)")
    p.add_argument("-o", "--out", default=os.path.join(repo, "assets", "Back-shell-grille.stl"),
                   help="output STL (default: assets/Back-shell-grille.stl)")
    p.add_argument("--cx", type=float, default=104.0, help="grille center X (STL frame)")
    p.add_argument("--cy", type=float, default=136.0, help="grille center Y (STL frame)")
    p.add_argument("--area-r", type=float, default=17.0, help="grille circle radius (mm)")
    p.add_argument("--hole-dia", type=float, default=2.0, help="hole diameter (mm)")
    p.add_argument("--pitch", type=float, default=3.5, help="hex spacing between holes (mm)")
    p.add_argument("--z0", type=float, default=5.0, help="cutter start Z (below inner wall)")
    p.add_argument("--z1", type=float, default=32.0, help="cutter end Z (above outer wall)")
    a = p.parse_args()
    cut_grille(a.src, a.out, a.cx, a.cy, a.area_r, a.hole_dia, a.pitch, a.z0, a.z1)


if __name__ == "__main__":
    main()
