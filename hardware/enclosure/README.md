# ESP32-C3 Super Mini enclosure

A small two-piece 3D-printable case for the ESP32-C3 Super Mini board this
project runs on. The board (including its pin headers) sits fully enclosed —
only the USB-C port is exposed for power/flashing. Nothing needs live wire
access once assembled, since this project only uses WiFi/BLE.

## Files

- `gen_case.py` — for boards **with** pin headers soldered on (measured:
  9mm total pin length, 6mm below the PCB). Generates
  `esp32c3_supermini_case_base.stl` and `esp32c3_supermini_case_lid.stl`.
  Total assembled height ~16.2mm.
- `gen_case_no_pins.py` — slimmer variant for boards **without** header pins
  (bare through-hole pads, or headers desoldered). Same design, just a
  shallower cavity below the PCB (0.8mm for solder blobs instead of 6.5mm
  for pin tips + margin). Generates `esp32c3_supermini_case_slim_base.stl`
  and `esp32c3_supermini_case_slim_lid.stl`. Total assembled height ~10.5mm.

## Requirements

```
pip install manifold3d numpy-stl
```

Add `svgpathtools` as well if you use the optional logo feature below.

## Usage

```
cd hardware/enclosure
python gen_case.py
```

Slice and print both STLs (no supports needed). The lid snaps onto the
base: two small clip bumps on the base's left/right inner walls seat into
matching dimples in the lid's lip when pressed down. To reopen, insert a
small flat screwdriver into the pry slot cut into the back wall (opposite
the USB-C end) and lever the lid up.

## Dimensions and assumptions

All dimensions are named constants at the top of `gen_case.py`:

| Constant | Value | Meaning |
|---|---|---|
| `BOARD_L` / `BOARD_W` / `BOARD_T` | 22.5 / 18.0 / 1.6 mm | Super Mini PCB footprint |
| `TOP_CLEARANCE` | 4.5 mm | headroom above the PCB for the ESP32-C3 module/shield can |
| `BOTTOM_CLEARANCE` | 6.5 mm (`gen_case.py`) / 0.8 mm (`gen_case_no_pins.py`) | measured 6mm pin protrusion + 0.5mm margin, or just solder blobs on a bare board |
| `FIT_SLACK` | 0.5 mm/side | board-to-cavity clearance |
| `CONNECTOR_OVERHANG` | 2.0 mm | EXTRA clearance at the USB-C end only, since the connector overhangs the PCB edge |
| `CONNECTOR_HEIGHT_ABOVE_BOARD_BOTTOM` | 0.5 mm | measured: how far above the board's underside the USB-C shell starts — sets the cutout's vertical position |
| `USB_W` / `USB_H` | 10.0 / 4.0 mm | USB-C cutout in the front wall |
| `CLIP_BUMP_R` / `CLIP_PROTRUSION` | 0.6 / 0.4 mm | snap-fit bump size on the base's inner walls |
| `CLIP_DIMPLE_R` / `CLIP_DIMPLE_DEPTH` | 0.9 / 0.55 mm | matching recess cut into the lid's lip |
| `PRY_SLOT_W` / `PRY_SLOT_D` | 6.0 / 3.0 mm | screwdriver slot in the back wall |

The board footprint is well documented; component-height clearances, the
USB-C connector's overhang, and the snap-fit bump/dimple sizing are
deliberately generous rather than press-fit-tight, since PCB revisions and
printer tolerances vary. **Test-fit before committing to a full print** and
adjust the constants above if your board variant differs — if the board
still doesn't slide in, increase `CONNECTOR_OVERHANG` first.

## Adding your own logo(s)

The script includes a reusable `add_svg_logo()` helper that traces an SVG
`<path>` and embosses it on the lid. No logo artwork is bundled with this
repo (Shelly and Matter are trademarks of their respective owners) — to
emboss your own, point `LOGO_SVGS` at the top of the script to your own SVG
file(s):

```python
LOGO_SVGS = [
    ("/path/to/your-logo.svg", 9.0, -5.5, 0.0),  # (svg_path, target_width_mm, x_offset, y_offset)
]
```

Each SVG should contain a single `<path>` element (export "as outlines" /
"flatten" from your vector tool of choice if it doesn't already).
