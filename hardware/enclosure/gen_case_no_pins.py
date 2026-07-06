"""
ESP32-C3 Super Mini enclosure generator — SLIM variant, for boards WITHOUT
soldered pin headers (bare through-hole pads only, or headers desoldered).

Identical two-piece base+lid design to gen_case.py (snap-fit rib/groove +
pry slot + open-top USB-C notch + rounded lid corners), but BOTTOM_CLEARANCE
is cut down to 0.8mm — just enough for solder blobs on the bare pads, not
full pin length. Use this variant if your board has no header pins sticking
out the bottom. If your board DOES have pin headers soldered on, use
gen_case.py instead.

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

TOP_CLEARANCE = 4.5       # above PCB top: ESP32-C3 module + shield can
BOTTOM_CLEARANCE = 0.8    # below PCB: bare solder pads only, no header pins
USB_SILL_HEIGHT = 2.0     # how far above the board's underside the solid
                          # part of the front wall stops (below this, wall;
                          # above this, open — see USB opening section below)
FIT_SLACK = 0.5           # per-side slack around the board footprint, all 4 sides
                          # equally — the connector turned out to sit flush with
                          # the PCB edge (no length-wise overhang), so the front
                          # doesn't need extra room beyond this; only its height
                          # (USB_SILL_HEIGHT below) needed correcting.

# ─── Shell parameters ───────────────────────────────────────────────────────
WALL_T = 1.6
PEG_SIZE = 2.5                  # corner support-peg footprint (board rests on these)
PEG_H = BOTTOM_CLEARANCE

USB_W = 10.0                     # width of the open USB-C notch in the front wall

LID_T = 1.6                      # lid plate thickness
LID_LIP_H = 2.0                  # lip depth that drops into the base's rim recess
LID_FIT_SLACK = 0.2              # lip-to-recess slack

# ─── Snap-fit clip (continuous rib on the base + matching groove in the lid) ─
# A full-length ridge (not a small point bump) prints far more reliably at
# this scale and is much more forgiving of alignment/tolerance error.
RIDGE_R = 0.9             # ridge radius, on the base's left/right inner walls
RIDGE_PROTRUSION = 0.5    # how far the ridge pokes into the recess past the wall face
GROOVE_R = 1.3            # matching groove radius cut into the lid's lip (larger for clearance)
GROOVE_DEPTH = 0.65       # how deep the groove cuts into the lip
RIDGE_MARGIN = 2.0        # how much shorter than the full cavity length the ridge is (each end)

# ─── Pry slot (back wall, opposite the USB-C end) ───────────────────────────
PRY_SLOT_W = 6.0   # width (along Y)
PRY_SLOT_D = 3.0   # depth, cut down from the top edge of the wall

# ─── Lid corner rounding ─────────────────────────────────────────────────────
# Sharp rectangular corners are a classic FDM warping/peeling point. Rounding
# the lid plate's outer corners (not the base, not the lip — purely cosmetic,
# doesn't affect the fit) fixes that.
LID_CORNER_R = 2.5

# ─── Optional logo embossing ────────────────────────────────────────────────
# Each entry: (path_to_svg, target_width_mm, x_offset_mm, y_offset_mm)
# Offsets are measured from the lid's center. Leave empty for a plain lid.
LOGO_SVGS = []
LOGO_HEIGHT = 0.6                # how far a logo stands proud of the lid (mm)
SVG_SAMPLES_PER_SEGMENT = 16      # bezier flattening resolution


def cbox(x0, y0, z0, dx, dy, dz):
    """Corner-anchored box: spans [x0, x0+dx] x [y0, y0+dy] x [z0, z0+dz]."""
    return Manifold.cube([dx, dy, dz]).translate([x0, y0, z0])


def x_ridge(length, radius):
    """A cylinder of the given length, centered at the origin, axis along X."""
    return Manifold.cylinder(length, radius, radius, circular_segments=24, center=True).rotate([0, 90, 0])


def rounded_plate(w, h, thickness, r, z0, segments=24):
    """Corner-anchored rounded-rectangle prism: spans [0,w] x [0,h] x [z0,z0+thickness]."""
    core_h = cbox(r, 0, z0, w - 2 * r, h, thickness)
    core_v = cbox(0, r, z0, w, h - 2 * r, thickness)
    plate = core_h + core_v
    for cx, cy in ((r, r), (w - r, r), (r, h - r), (w - r, h - r)):
        plate = plate + Manifold.cylinder(thickness, r, r, circular_segments=segments).translate([cx, cy, z0])
    return plate


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
# Coordinate system: corner-anchored. x=0 is the outer face of the FRONT wall
# (the one with the USB-C opening); x increases towards the back wall.
# FIT_SLACK applies evenly on all 4 sides — the connector sits flush with the
# PCB edge (no length-wise overhang), so the front doesn't need extra room.
cavity_l = BOARD_L + 2 * FIT_SLACK
cavity_w = BOARD_W + 2 * FIT_SLACK
cavity_h = BOTTOM_CLEARANCE + BOARD_T + TOP_CLEARANCE
outer_l = cavity_l + 2 * WALL_T
outer_w = cavity_w + 2 * WALL_T
base_wall_h = cavity_h + LID_LIP_H
total_h = base_wall_h + WALL_T

cavity_x0, cavity_x1 = WALL_T, outer_l - WALL_T
cavity_y0, cavity_y1 = WALL_T, outer_w - WALL_T
cavity_z0 = WALL_T
wall_top_z = WALL_T + base_wall_h

board_x0 = cavity_x0 + FIT_SLACK
board_x1 = board_x0 + BOARD_L

# ─── BASE ───────────────────────────────────────────────────────────────────
floor = cbox(0, 0, 0, outer_l, outer_w, WALL_T)
wall_front = cbox(0, 0, WALL_T, WALL_T, outer_w, base_wall_h)
wall_back = cbox(outer_l - WALL_T, 0, WALL_T, WALL_T, outer_w, base_wall_h)
wall_left = cbox(0, 0, WALL_T, outer_l, WALL_T, base_wall_h)
wall_right = cbox(0, outer_w - WALL_T, WALL_T, outer_l, WALL_T, base_wall_h)
base_shell = floor + wall_front + wall_back + wall_left + wall_right

pegs = Manifold()
for px in (board_x0, board_x1 - PEG_SIZE):
    for py in (cavity_y0, cavity_y1 - PEG_SIZE):
        pegs = pegs + cbox(px, py, WALL_T, PEG_SIZE, PEG_SIZE, PEG_H)

cavity = cbox(cavity_x0, cavity_y0, cavity_z0, cavity_l, cavity_w, cavity_h)
rim_recess = cbox(cavity_x0, cavity_y0, WALL_T + cavity_h, cavity_l, cavity_w, LID_LIP_H + 0.5)

# USB-C opening: solid wall below the sill, fully open above it (all the way
# past the top of the wall) — the lid plate seals the top when assembled.
usb_sill_z = WALL_T + BOTTOM_CLEARANCE + USB_SILL_HEIGHT
usb_notch = cbox(-0.5, (outer_w - USB_W) / 2, usb_sill_z,
                  WALL_T + 1, USB_W, (wall_top_z - usb_sill_z) + 1)

pry_slot = cbox(outer_l - WALL_T - 0.5, (outer_w - PRY_SLOT_W) / 2, wall_top_z - PRY_SLOT_D,
                 WALL_T + 1, PRY_SLOT_W, PRY_SLOT_D + 0.5)

ridge_len = cavity_l - 2 * RIDGE_MARGIN
ridge_z = WALL_T + cavity_h + LID_LIP_H / 2
ridge_x = outer_l / 2
ridge_left = x_ridge(ridge_len, RIDGE_R).translate(
    [ridge_x, cavity_y0 - (RIDGE_R - RIDGE_PROTRUSION), ridge_z])
ridge_right = x_ridge(ridge_len, RIDGE_R).translate(
    [ridge_x, cavity_y1 + (RIDGE_R - RIDGE_PROTRUSION), ridge_z])

base = (base_shell + pegs - cavity - rim_recess - usb_notch - pry_slot
        + ridge_left + ridge_right)

# ─── LID (+ snap groove, rounded corners, + optional logos) ────────────────
# Local z=0 is the lip's bottom tip (first to enter the base); the plate sits
# above it. When assembled, local z=0 aligns with base z = WALL_T + cavity_h.
lip_x0 = cavity_x0 + LID_FIT_SLACK
lip_y0 = cavity_y0 + LID_FIT_SLACK
lip_l = cavity_l - 2 * LID_FIT_SLACK
lip_w = cavity_w - 2 * LID_FIT_SLACK

lid_plate = rounded_plate(outer_l, outer_w, LID_T, LID_CORNER_R, LID_LIP_H)
lid_lip = cbox(lip_x0, lip_y0, 0, lip_l, lip_w, LID_LIP_H)
lid_blank = lid_plate + lid_lip

groove_z = LID_LIP_H / 2  # matches ridge_z's position relative to the rim band
groove_left = x_ridge(ridge_len, GROOVE_R).translate(
    [ridge_x, lip_y0 + (GROOVE_R - GROOVE_DEPTH), groove_z])
groove_right = x_ridge(ridge_len, GROOVE_R).translate(
    [ridge_x, lip_y0 + lip_w - (GROOVE_R - GROOVE_DEPTH), groove_z])

lid = lid_blank - groove_left - groove_right

for svg_path, target_w, off_x, off_y in LOGO_SVGS:
    logo = add_svg_logo(svg_path, target_w).translate(
        [outer_l / 2 + off_x, outer_w / 2 + off_y, LID_LIP_H + LID_T])
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
    print(f"Base: {n1} tris, outer {outer_l:.1f} x {outer_w:.1f} x {total_h:.1f} mm")
    print(f"Lid:  {n2} tris, outer {outer_l:.1f} x {outer_w:.1f} mm")
    print(f"Board slot: {board_x1 - board_x0:.1f}mm long, front edge sits "
          f"{FIT_SLACK:.1f}mm from the USB wall")
    print(f"USB-C opening: sill at {usb_sill_z - (WALL_T + BOTTOM_CLEARANCE):.1f}mm above board bottom, open upward, sealed by lid")
    print(f"Base manifold status: {base.status()}, lid manifold status: {lid.status()}")
