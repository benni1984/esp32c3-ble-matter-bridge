"""
ESP32-C3 Super Mini enclosure generator — two-piece box (base + lid).

Generates two STL files: a base that fully encloses the board (including its
pin headers — this project only needs WiFi/BLE, no external GPIO wiring
while assembled), and a lid that SNAPS onto the base (a continuous snap rib
on the base's inner side walls seats into a matching groove in the lid's
lip) rather than just resting in a friction-fit recess. A pry slot in the
back wall lets you pop the lid back off with a flat screwdriver.

The USB-C end of the front wall is left OPEN at the top (down to a sill
just below where the connector sits) rather than a closed window — the lid
plate seals the top of that opening when assembled. This is deliberately
more forgiving of small measurement errors in connector height than a
precisely-placed rectangular cutout.

Requirements:
    pip install manifold3d numpy-stl

Optionally, to engrave your own logo/text into the lid, point LOGO_SVGS
below at your own SVG file(s) (each containing a single <path> element) —
see add_svg_logo() for the mechanics. No logo files are bundled with this
project; the base design works standalone with LOGO_SVGS = []. Slice the
lid flipped (logo face down against the bed) for a crisp, overhang-free
print — see ENGRAVE_DEPTH below.

Run:
    python gen_case.py
Output:
    esp32c3_supermini_case_base.stl
    esp32c3_supermini_case_lid.stl

All dimensions are named constants below. The board footprint (22.5 x 18mm),
pin header length (9mm total, 6mm below the PCB), and USB-C connector
position (flush with the PCB edge, no length-wise overhang) were measured
on the actual hardware. USB_SILL_HEIGHT is still an estimate — test-fit
before committing to a full print, and adjust the constants if your board
variant differs.
"""

import numpy as np
import manifold3d as m3d
from manifold3d import Manifold, CrossSection, FillRule
from stl import mesh as stl_mesh

# ─── Board dimensions (mm) ──────────────────────────────────────────────────
BOARD_L = 22.5   # length, along the pin-header rows (X)
BOARD_W = 18.0   # width, across the board, USB-C on one end (Y)
BOARD_T = 1.6    # PCB thickness (standard)

TOP_CLEARANCE = 4.0       # above PCB top: ESP32-C3 module + shield can.
                          # Was 4.5mm; test-fit reported ~1mm of Z play with
                          # the lid on, so trimmed by half that (conservative
                          # — leaves ~0.5mm margin rather than zeroing it out,
                          # since this headroom exists for the tallest point
                          # on the board and ABS shrinks the cavity, not grows
                          # it). Re-check if this ever gets too tight to close.
# Measured on the actual board: pin headers are 9mm long total, 6mm of that
# protrudes below the PCB. BOTTOM_CLEARANCE = that 6mm + 0.5mm margin so the
# pins don't bottom out on the case floor before the board reaches the pegs.
BOTTOM_CLEARANCE = 6.5
USB_SILL_HEIGHT = 2.0     # how far above the board's underside the solid
                          # part of the front wall stops (below this, wall;
                          # above this, open — see USB opening section below)
FIT_SLACK = 0.35          # per-side slack around the board footprint, all 4 sides
                          # equally — the connector turned out to sit flush with
                          # the PCB edge (no length-wise overhang), so the front
                          # doesn't need extra room beyond this; only its height
                          # (USB_SILL_HEIGHT below) needed correcting.
                          # History: 0.5mm printed tight enough along the
                          # board's full insertion depth that it couldn't be
                          # seated at all (ABS shrinks more, and less
                          # predictably, than PLA/PETG). Bumping this to
                          # 0.7mm, then 1.0mm, "fixed" that — but FIT_SLACK
                          # was, at the time, sizing BOTH the board's resting
                          # cavity AND the rim/ridge zone the lid's snap-fit
                          # lives in, so the fix also loosened the resting
                          # board to 1.5-2mm of side-to-side play. Those two
                          # concerns are now split: FIT_SLACK is back to a
                          # snug value for the board's own resting cavity;
                          # RIM_SLACK (below) independently sizes the wider
                          # rim/ridge zone the board must also clear in
                          # passing on its way down to the pegs, without
                          # loosening where it actually comes to rest.
                          # 0.35mm/side = ~0.7mm total play in X and Y —
                          # test-fit before going any tighter.

# ─── Shell parameters ───────────────────────────────────────────────────────
WALL_T = 1.6
PEG_SIZE = 2.5                  # corner support-peg footprint (board rests on these)
PEG_H = BOTTOM_CLEARANCE

USB_W = 10.0                     # width of the open USB-C notch in the front wall

LID_T = 1.2                      # lid plate thickness — was 1.6mm; the printed
                                  # lid felt over-built, and this is still plenty
                                  # for a flat unsupported ABS plate at this span.
LID_LIP_H = 3.0                  # lip depth that drops into the base's rim recess.
                                  # Was 4.0mm ("taller than strictly needed"); 3.0
                                  # is the smallest that still keeps the ridge/groove
                                  # band (see below) inside the lip instead of
                                  # spanning past both ends — verified against the
                                  # GROOVE_R=1.4 band math (0.1-2.9mm, within 0-3.0).
LID_FIT_SLACK = 0.2              # lip-to-recess slack

# ─── Snap-fit clip (continuous rib on the base + matching groove in the lid) ─
# A full-length ridge (not a small point bump) prints far more reliably at
# this scale. v1 (0.9/0.5mm) printed with essentially zero engagement — FDM
# printers wash out protrusions that fine. These are deliberately bold.
# Both radii must stay well under LID_LIP_H/2, or the ridge/groove band spans
# almost the entire lip height instead of a localized notch — that happened
# with GROOVE_R=1.7 against LID_LIP_H=3.0 (band ran -0.2 to 3.2mm, i.e. past
# both ends of the lip), giving the ridge free passage almost everywhere
# instead of a resist-then-release click.
RIDGE_R = 1.2             # ridge radius, on the base's left/right inner walls
RIDGE_PROTRUSION = 0.8    # how far the ridge pokes into the recess past the wall face
GROOVE_R = 1.4            # matching groove radius cut into the lid's lip (larger for clearance)
GROOVE_DEPTH = 1.0        # how deep the groove cuts into the lip
RIDGE_MARGIN = 2.0        # how much shorter than the full cavity length the ridge is (each end)

# The rim recess (where the lid's lip + this ridge live, see "Derived
# dimensions" below) is deliberately WIDER than the board's own snug cavity
# — RIM_SLACK, not FIT_SLACK, sizes it. This is what lets the board pass the
# ridge on its way down to the pegs without the board's resting fit (sized
# by FIT_SLACK) having to be loose too. RIM_SLACK must stay
# >= RIDGE_PROTRUSION (0.8mm) with some margin, independent of FIT_SLACK —
# verified with a manifold3d boolean intersection between `base` and a probe
# at the board's actual resting edge position, across the ridge's Z-band
# (zero volume = confirmed clear). Re-verify the same way before changing
# either constant.
RIM_SLACK = 1.0

# ─── Pry slot (back wall, opposite the USB-C end) ───────────────────────────
PRY_SLOT_W = 6.0   # width (along Y)
PRY_SLOT_D = 3.0   # depth, cut down from the top edge of the wall

# ─── Lid corner rounding ─────────────────────────────────────────────────────
# Sharp rectangular corners are a classic FDM warping/peeling point. Rounding
# the lid plate's outer corners (not the base, not the lip — purely cosmetic,
# doesn't affect the fit) fixes that.
LID_CORNER_R = 2.5

# ─── Optional logo engraving ────────────────────────────────────────────────
# Each entry: (path_to_svg, target_width_mm, x_offset_mm, y_offset_mm)
# Offsets are measured from the lid's center. Leave empty for a plain lid.
# The logo is engraved (cut INTO the lid), not embossed — uses less material
# and prints cleaner: slice with the lid flipped so this face is DOWN against
# the bed (the lip then simply stands up with no overhangs).
LOGO_SVGS = [
    ("matter-logo.svg", 18.0, 0.0, 0.0),
]
# 18mm wide -> ~17.6mm tall (matter-logo.svg's viewBox is 512x500, nearly
# square). Sized against the lid's Y extent (outer_w ~21.9mm), the tighter
# of the two dimensions, leaving ~2mm margin top/bottom before the rounded
# corners (LID_CORNER_R) start cutting in. Recompute if BOARD_W or FIT_SLACK
# changes outer_w.
ENGRAVE_DEPTH = 0.6               # how deep the logo cuts into the lid (mm) — must be < LID_T
                                   # (LID_T is 1.2mm, so this leaves 0.6mm solid behind it)
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


def add_svg_logo(svg_path, target_width_mm, depth_mm):
    """Parse an SVG file containing a single <path> element and return a
    Manifold solid: the exact traced shape, extruded to depth_mm and
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
    solid = Manifold.extrude(cross, depth_mm)
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

# Rim zone (lid lip + snap ridge/groove) — independently sized via RIM_SLACK,
# centered in the same outer envelope as the (snugger) board cavity above.
# Wider than the board cavity by design: this is the funnel the board passes
# through on the way down, before landing in its snug home lower in the case.
rim_l = BOARD_L + 2 * RIM_SLACK
rim_w = BOARD_W + 2 * RIM_SLACK
rim_x0 = (outer_l - rim_l) / 2
rim_y0 = (outer_w - rim_w) / 2
rim_x1 = rim_x0 + rim_l
rim_y1 = rim_y0 + rim_w

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
rim_recess = cbox(rim_x0, rim_y0, WALL_T + cavity_h, rim_l, rim_w, LID_LIP_H + 0.5)

# USB-C opening: solid wall below the sill, fully open above it (all the way
# past the top of the wall) — the lid plate seals the top when assembled.
usb_sill_z = WALL_T + BOTTOM_CLEARANCE + USB_SILL_HEIGHT
usb_notch = cbox(-0.5, (outer_w - USB_W) / 2, usb_sill_z,
                  WALL_T + 1, USB_W, (wall_top_z - usb_sill_z) + 1)

pry_slot = cbox(outer_l - WALL_T - 0.5, (outer_w - PRY_SLOT_W) / 2, wall_top_z - PRY_SLOT_D,
                 WALL_T + 1, PRY_SLOT_W, PRY_SLOT_D + 0.5)

ridge_len = rim_l - 2 * RIDGE_MARGIN
ridge_z = WALL_T + cavity_h + LID_LIP_H / 2
ridge_x = outer_l / 2
ridge_left = x_ridge(ridge_len, RIDGE_R).translate(
    [ridge_x, rim_y0 - (RIDGE_R - RIDGE_PROTRUSION), ridge_z])
ridge_right = x_ridge(ridge_len, RIDGE_R).translate(
    [ridge_x, rim_y1 + (RIDGE_R - RIDGE_PROTRUSION), ridge_z])

# The ridge cylinder needs (2*RIDGE_R - RIDGE_PROTRUSION) of solid wall behind
# it to stay fully contained -- with RIM_SLACK sized generously for board
# clearance (see above), the wall left at the rim height (rim_y0) is thinner
# than that, so the ridge would otherwise poke straight through the outer
# wall face. Fix: a local boss on the OUTSIDE of the wall, only across the
# ridge's own Z-band, that adds back exactly the missing material. Doesn't
# touch FIT_SLACK/RIM_SLACK/RIDGE_R/RIDGE_PROTRUSION, so neither the board
# clearance fix nor the tuned snap-engagement feel are affected.
_ridge_needed_backing = (2 * RIDGE_R - RIDGE_PROTRUSION) - rim_y0
if _ridge_needed_backing > 0:
    _boss_margin = 0.3  # a bit of print-tolerance headroom beyond the bare minimum
    _boss_t = _ridge_needed_backing + _boss_margin
    _boss_z0 = ridge_z - RIDGE_R - 0.2
    _boss_h = 2 * RIDGE_R + 0.4
    ridge_boss_left = cbox(ridge_x - ridge_len / 2, -_boss_t, _boss_z0, ridge_len, _boss_t, _boss_h)
    ridge_boss_right = cbox(ridge_x - ridge_len / 2, outer_w, _boss_z0, ridge_len, _boss_t, _boss_h)
else:
    ridge_boss_left = Manifold()
    ridge_boss_right = Manifold()

base = (base_shell + pegs - cavity - rim_recess - usb_notch - pry_slot
        + ridge_left + ridge_right + ridge_boss_left + ridge_boss_right)

# ─── LID (+ snap groove, rounded corners, + optional logos) ────────────────
# Local z=0 is the lip's bottom tip (first to enter the base); the plate sits
# above it. When assembled, local z=0 aligns with base z = WALL_T + cavity_h.
lip_x0 = rim_x0 + LID_FIT_SLACK
lip_y0 = rim_y0 + LID_FIT_SLACK
lip_l = rim_l - 2 * LID_FIT_SLACK
lip_w = rim_w - 2 * LID_FIT_SLACK

lid_plate = rounded_plate(outer_l, outer_w, LID_T, LID_CORNER_R, LID_LIP_H)
lid_lip = cbox(lip_x0, lip_y0, 0, lip_l, lip_w, LID_LIP_H)
lid_blank = lid_plate + lid_lip

groove_z = LID_LIP_H / 2  # matches ridge_z's position relative to the rim band
# Sign matters here: the cut must start at the lip's outer face and go
# GROOVE_DEPTH deep INTO it, not the other way around (that bug made the
# groove ~2.4mm deep instead of ~1.0mm, and the ridge never touched a wall).
groove_left = x_ridge(ridge_len, GROOVE_R).translate(
    [ridge_x, lip_y0 + (GROOVE_DEPTH - GROOVE_R), groove_z])
groove_right = x_ridge(ridge_len, GROOVE_R).translate(
    [ridge_x, lip_y0 + lip_w + (GROOVE_R - GROOVE_DEPTH), groove_z])

lid = lid_blank - groove_left - groove_right

for svg_path, target_w, off_x, off_y in LOGO_SVGS:
    # Extrude a bit deeper than ENGRAVE_DEPTH and position it so the cut
    # starts slightly above the top face — guarantees a clean boolean
    # through the surface instead of an exactly-coplanar (degenerate) cut.
    logo = add_svg_logo(svg_path, target_w, ENGRAVE_DEPTH + 0.5).translate(
        [outer_l / 2 + off_x, outer_w / 2 + off_y, LID_LIP_H + LID_T - ENGRAVE_DEPTH])
    lid = lid - logo


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
    n1 = export(base, "esp32c3_supermini_case_base.stl")
    n2 = export(lid, "esp32c3_supermini_case_lid.stl")
    print(f"Base: {n1} tris, outer {outer_l:.1f} x {outer_w:.1f} x {total_h:.1f} mm")
    print(f"Lid:  {n2} tris, outer {outer_l:.1f} x {outer_w:.1f} mm")
    print(f"Board slot: {board_x1 - board_x0:.1f}mm long, front edge sits "
          f"{FIT_SLACK:.1f}mm from the USB wall")
    print(f"USB-C opening: sill at {usb_sill_z - (WALL_T + BOTTOM_CLEARANCE):.1f}mm above board bottom, open upward, sealed by lid")
    print(f"Base manifold status: {base.status()}, lid manifold status: {lid.status()}")
