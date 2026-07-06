"""
ESP32-C3 Super Mini enclosure generator — SLIM variant, for boards WITHOUT
soldered pin headers (bare through-hole pads only, or headers desoldered).

Identical two-piece base+lid design to gen_case.py, but BOTTOM_CLEARANCE is
cut down from 3.0mm to 0.8mm — just enough for solder blobs on the bare
pads, not full pin length. Use this variant if your board has no header
pins sticking out the bottom. If your board DOES have pin headers soldered
on, use gen_case.py instead.

Requirements:
    pip install manifold3d numpy-stl

Optionally, to emboss your own logo/text on the lid, point LOGO_SVGS below
at your own SVG file(s) (each containing a single <path> element) — see
add_svg_logo() for the mechanics. No logo files are bundled with this
project; the base design works standalone with LOGO_SVGS = [].

Run:
    python gen_case_no_pins.py
Output:
    esp32c3_supermini_case_slim_base.stl
    esp32c3_supermini_case_slim_lid.stl
"""

import numpy as np
import manifold3d as m3d
from manifold3d import Manifold, CrossSection, FillRule
from stl import mesh as stl_mesh

# ─── Board dimensions (mm) ──────────────────────────────────────────────────
BOARD_L = 22.5   # length, along the pin-header rows (X)
BOARD_W = 18.0   # width, across the board, USB-C on one end (Y)
BOARD_T = 1.6    # PCB thickness (standard)

TOP_CLEARANCE = 4.5      # above PCB top: ESP32-C3 module + shield can
BOTTOM_CLEARANCE = 0.8   # below PCB: bare solder pads only, no header pins
FIT_SLACK = 0.5          # per-side slack around the board footprint

# ─── Shell parameters ───────────────────────────────────────────────────────
WALL_T = 1.6
PEG_SIZE = 2.5                  # corner support-peg footprint (board rests on these)
PEG_H = BOTTOM_CLEARANCE

USB_W, USB_H = 10.0, 4.0         # USB-C cutout in the front wall (generous)

LID_T = 1.6                      # lid plate thickness
LID_LIP_H = 2.0                  # lip depth that drops into the base's rim recess
LID_FIT_SLACK = 0.2              # lip-to-recess slack (friction fit)

# ─── Optional logo embossing ────────────────────────────────────────────────
# Each entry: (path_to_svg, target_width_mm, x_offset_mm, y_offset_mm)
# Leave empty for a plain lid.
LOGO_SVGS = []
LOGO_HEIGHT = 0.6                # how far a logo stands proud of the lid (mm)
SVG_SAMPLES_PER_SEGMENT = 16      # bezier flattening resolution


def box(lx, ly, lz, x=0.0, y=0.0, z=0.0):
    return Manifold.cube([lx, ly, lz], center=True).translate([x, y, z])


def add_svg_logo(svg_path, target_width_mm):
    """Parse an SVG file containing a single <path> element and return a
    Manifold solid: the exact traced shape, extruded to LOGO_HEIGHT and
    centered on its own bounding box. Requires `pip install svgpathtools`.
    """
    import re
    from svgpathtools import parse_path

    text = open(svg_path, "r", encoding="utf-8").read()
    vb_match = re.search(r'viewBox="([\d.\-\s]+)"', text)
    if vb_match:
        _, _, vb_w, vb_h = [float(v) for v in vb_match.group(1).split()]
    else:
        vb_w = float(re.search(r'width="([\d.]+)"', text).group(1))
        vb_h = float(re.search(r'height="([\d.]+)"', text).group(1))

    d_match = re.search(r'<path[^>]*\sd="([^"]+)"', text)
    path = parse_path(d_match.group(1))

    contours = []
    scale = target_width_mm / vb_w
    for sub in path.continuous_subpaths():
        pts = []
        for seg in sub:
            n = 2 if seg.__class__.__name__ == "Line" else SVG_SAMPLES_PER_SEGMENT
            for i in range(n):
                p = seg.point(i / n)
                pts.append((p.real * scale, (vb_h - p.imag) * scale))  # flip Y
        contours.append(np.array(pts))

    cross = CrossSection(contours, FillRule.NonZero)
    solid = Manifold.extrude(cross, LOGO_HEIGHT)
    bbox = solid.bounding_box()
    cx, cy = (bbox[0] + bbox[3]) / 2, (bbox[1] + bbox[4]) / 2
    return solid.translate([-cx, -cy, 0])


# ─── Derived cavity + outer dimensions ──────────────────────────────────────
cavity_l = BOARD_L + 2 * FIT_SLACK
cavity_w = BOARD_W + 2 * FIT_SLACK
cavity_h = BOTTOM_CLEARANCE + BOARD_T + TOP_CLEARANCE
outer_l = cavity_l + 2 * WALL_T
outer_w = cavity_w + 2 * WALL_T
base_wall_h = cavity_h + LID_LIP_H

# ─── BASE ───────────────────────────────────────────────────────────────────
floor = box(outer_l, outer_w, WALL_T, z=WALL_T / 2)
wall_front = box(WALL_T, outer_w, base_wall_h, x=-(outer_l / 2 - WALL_T / 2), z=base_wall_h / 2)
wall_back  = box(WALL_T, outer_w, base_wall_h, x= (outer_l / 2 - WALL_T / 2), z=base_wall_h / 2)
wall_left  = box(outer_l, WALL_T, base_wall_h, y=-(outer_w / 2 - WALL_T / 2), z=base_wall_h / 2)
wall_right = box(outer_l, WALL_T, base_wall_h, y= (outer_w / 2 - WALL_T / 2), z=base_wall_h / 2)
base_shell = floor + wall_front + wall_back + wall_left + wall_right

peg_x, peg_y = cavity_l / 2 - PEG_SIZE / 2, cavity_w / 2 - PEG_SIZE / 2
peg_z = WALL_T + PEG_H / 2
pegs = Manifold()
for sx in (-1, 1):
    for sy in (-1, 1):
        pegs = pegs + box(PEG_SIZE, PEG_SIZE, PEG_H, x=sx * peg_x, y=sy * peg_y, z=peg_z)

cavity = box(cavity_l, cavity_w, cavity_h, z=WALL_T + cavity_h / 2)
rim_recess = box(cavity_l, cavity_w, LID_LIP_H + 1, z=WALL_T + cavity_h - LID_LIP_H / 2 + 0.5)
usb_cutout_z = WALL_T + BOTTOM_CLEARANCE + BOARD_T / 2 + 0.5
usb_cutout = box(WALL_T + 2, USB_W, USB_H, x=-(outer_l / 2 - WALL_T / 2), z=usb_cutout_z)

base = base_shell + pegs - cavity - rim_recess - usb_cutout

# ─── LID (+ optional logos) ─────────────────────────────────────────────────
lid_plate = box(outer_l, outer_w, LID_T, z=LID_T / 2)
lip_l, lip_w = cavity_l - 2 * LID_FIT_SLACK, cavity_w - 2 * LID_FIT_SLACK
lid_lip = box(lip_l, lip_w, LID_LIP_H, z=-(LID_LIP_H / 2))
lid = lid_plate + lid_lip

for svg_path, target_w, off_x, off_y in LOGO_SVGS:
    logo = add_svg_logo(svg_path, target_w).translate([off_x, off_y, LID_T])
    lid = lid + logo


# ─── Export ─────────────────────────────────────────────────────────────────
def export(manifold_obj, path):
    mesh = manifold_obj.to_mesh()
    tris = mesh.tri_verts
    verts = mesh.vert_properties[:, :3]
    out = stl_mesh.Mesh(np.zeros(tris.shape[0], dtype=stl_mesh.Mesh.dtype))
    for i, tri in enumerate(tris):
        for j in range(3):
            out.vectors[i][j] = verts[tri[j]]
    out.save(path)
    return tris.shape[0]


if __name__ == "__main__":
    n1 = export(base, "esp32c3_supermini_case_slim_base.stl")
    n2 = export(lid, "esp32c3_supermini_case_slim_lid.stl")
    print(f"Base: {n1} tris, outer {outer_l:.1f} x {outer_w:.1f} x {base_wall_h + WALL_T:.1f} mm")
    print(f"Lid:  {n2} tris, outer {outer_l:.1f} x {outer_w:.1f} mm")
    print(f"Assembled total height: {base_wall_h + WALL_T:.1f} mm "
          f"(lid lip seats {LID_LIP_H:.1f}mm into the base)")
