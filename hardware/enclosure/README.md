# ESP32-C3 Super Mini enclosure

A small two-piece 3D-printable case for the ESP32-C3 Super Mini board this
project runs on. The board (including its pin headers) sits fully enclosed —
only the USB-C port is exposed for power/flashing. Nothing needs live wire
access once assembled, since this project only uses WiFi/BLE.

## Files

- `gen_case.py` — generates `esp32c3_supermini_case_base.stl` and
  `esp32c3_supermini_case_lid.stl`.

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

Slice and print both STLs (no supports needed). The lid's lip friction-fits
into a recess at the top of the base — no screws or glue required, though a
drop of super glue makes it permanent if you don't need to reopen it.

## Dimensions and assumptions

All dimensions are named constants at the top of `gen_case.py`:

| Constant | Value | Meaning |
|---|---|---|
| `BOARD_L` / `BOARD_W` / `BOARD_T` | 22.5 / 18.0 / 1.6 mm | Super Mini PCB footprint |
| `TOP_CLEARANCE` | 4.5 mm | headroom above the PCB for the ESP32-C3 module/shield can |
| `BOTTOM_CLEARANCE` | 3.0 mm | headroom below the PCB for solder joints / pin tips |
| `FIT_SLACK` | 0.5 mm/side | board-to-cavity clearance |
| `USB_W` / `USB_H` | 10.0 / 4.0 mm | USB-C cutout in the front wall |

The board footprint is well documented; component-height clearances are
deliberately generous rather than press-fit-tight, since PCB revisions vary
slightly between vendors. **Test-fit before committing to a full print** and
adjust the constants above if your board variant differs.

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
