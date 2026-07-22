# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What This Project Is

An **ESP32-C3** firmware that acts as a Matter Bridge: it polls **any BTHome
v2 BLE device** relayed through one or more **Shelly BLE relays** over HTTP
(devices are never scanned directly) and exposes their measurements as native
Matter endpoints — Apple Home, Home Assistant, or any Matter controller can
pair with it directly, no cloud/gateway required. The Ecowitt WS90 weather
station is the reference device it was originally built for, but the pipeline
is fully generic — any BTHome v2 broadcaster works with no code change (see
`docs/supported_devices.md`).

Read `README.md` first — it's the current, maintained source of truth for
setup, build, and known limitations. `docs/adding_a_sensor.md` and
`docs/supported_devices.md` cover the sensor pipeline in more depth.

## Data Pipeline

```
Shelly HTTP (BLE.CloudRelay.ListInfos)
  → base64-decode the fcd2 service-data field
  → bthome_parse()          (components/bthome/)
  → sensor_registry         (components/sensor_registry/)
  → matter_bridge_update()  (components/matter_bridge/)
```

- `components/shelly_poller/` — finds Shelly relays via a local subnet scan
  (no hardcoded IPs — see README's "Shelly discovery" section) and polls
  **every** known relay every 10s, reporting **every** BLE device present in
  each relay's response (not just one hardcoded device).
- `components/bthome/` — decodes the BTHome v2 payload into typed
  `sensor_reading_t` values, generically for any MAC. The `s_objects[]` table
  in `bthome.cpp` maps BTHome Object IDs to `sensor_type_t` — **never map two
  genuinely different physical quantities to the same `sensor_type_t`**;
  they'll race for the same Matter endpoint and silently overwrite each other
  (this exact bug hit dewpoint-vs-temperature and capacitor-voltage-vs-battery
  once already). MAC addresses throughout this codebase (JSON keys, console
  commands, bind-key storage) are in **natural/display order**
  (`AA:BB:CC:DD:EE:FF`, matching what a human reads off the device) — don't
  reintroduce the reversed "NimBLE LSB-first" order that used to exist in
  `shelly_poller.cpp`/`matter_bridge.cpp` (a leftover from the removed
  direct-BLE-scan codepath, silently wrong because it was never actually
  exercised against an encrypted device).
- `components/sensor_registry/` — tracks, per physical device (keyed by BLE
  MAC): which Matter endpoint ID belongs to which `sensor_type_t` this boot
  (`matter_endpoint_id[]`, rebuilt every boot), and which sensor types have
  *ever* been observed for that device (`known_type_mask`, persisted,
  append-only — drives what gets pre-created at the *next* boot). See the
  gotcha below for why these are two separate fields.
- `components/matter_bridge/` — creates Matter endpoints and pushes live
  readings into them. See the critical gotcha below before touching this.

## The Most Important Gotcha: Two Attribute-Writing APIs

This esp-matter version (pinned commit, see `README.md`/CI) has **two
completely different ways to update a Matter attribute, and only one of them
actually reaches real Matter clients**:

1. **Legacy**: `esp_matter::attribute::set_val()` / `attribute::update()` —
   writes to esp-matter's own in-memory attribute store. This compiles, logs
   success, and does nothing wrong locally — but for measurement clusters in
   this esp-matter version, **nothing reads from that store** for the real
   Matter bootstrap read or live attribute reporting. Several past
   commits chased phantom "type mismatch" bugs here before this was
   diagnosed — see the comment above `find_measurement_cluster()` in
   `matter_bridge.cpp` for the full story.
2. **Real**: measurement clusters (Temperature, Humidity, Pressure,
   Illuminance, Flow) are served by dedicated `chip::app` C++ cluster objects
   registered in a `ServerClusterInterfaceRegistry`. Some
   (Pressure/Humidity/Flow) have a documented free-function API in
   `clusters/*/integration.h` (`SetMeasuredValue()`/`FindClusterOnEndpoint()`);
   others (Temperature/Illuminance) don't in this version, so
   `find_measurement_cluster<T>()` looks them up directly via the registry
   and calls `SetMeasuredValue()`/`GetMeasuredValue()` on the concrete type.

**When adding or changing a measurement type, always use the real API
(#2), never the legacy one (#1).** See `docs/adding_a_sensor.md` for a
current, working example.

## The Second Gotcha: CHIP Stack Lock

The real attribute API (#2 above) **asserts the caller holds the CHIP stack
lock** — unlike the legacy API, which took the lock internally. Code running
on the Matter/CHIP task (e.g. `app_event_cb()`'s device-event handlers) is
already on that task and needs nothing extra. Code running on a **different
FreeRTOS task** — like `shelly_poller`'s own poll task, calling into
`matter_bridge_update()` — **must** wrap the calls in
`esp_matter::lock::ScopedChipStackLock chip_lock(portMAX_DELAY);` first, or
the device aborts on the very first cross-task write with `chip[DL]: Chip
stack locking error ... Code is unsafe/racy`. See `matter_bridge_update()`
for the existing pattern.

## The Third Gotcha: New Devices Need a Reboot to Appear in Matter

Matter endpoints must exist **before** the Matter stack starts
commissioning/accepting connections — HA's matter.js reads
`descriptor.deviceTypeList` during initial attribute enumeration, and an
endpoint added after a controller has already connected crashes it (this
applies on every boot with an established fabric too, not just first-ever
commissioning).

Consequence: `matter_bridge_init()`'s endpoint pre-creation loop only ever
runs once, at boot, before `esp_matter::start()`, over whatever
`known_type_mask` bits are already persisted in `sensor_registry`.
`matter_bridge_update()` (the runtime data-arrival path) calls
`sensor_registry_mark_known()` for every reading it sees — if that's the
first time this (MAC, sensor_type) pair has ever been observed, it gets
persisted and logged (`"New sensor type '...' observed for ... — will be
exposed as a Matter endpoint after next reboot"`), but **deliberately does
not get a live endpoint this session**. It gets one automatically at the
next boot.

**Do not "fix" this by trying to create Matter endpoints from
`matter_bridge_update()`** — that's exactly the crash this design avoids.
If you need a new device/measurement to show up, the answer is "reboot the
device," not a code change.

## Home Assistant Naming (FixedLabel)

Wind speed/direction, rain, UV, and battery all share the generic
`FlowMeasurement` cluster (Matter has no dedicated clusters for these), which
Home Assistant otherwise labels indistinguishably as "Flow (N)". A
`BridgeDeviceInfoProvider` (in `components/matter_bridge/`) implements
`chip::DeviceLayer::DeviceInfoProvider::IterateFixedLabel()` to give each
endpoint a real name via the `ha_entitylabel` Fixed Label key — confirmed
against `home-assistant/core`'s `VENDOR_LABELING_LIST`, which recognizes that
key specifically for this firmware's test VID/PID (`0xFFF1`/`0x8000`).

**HA only applies Fixed Labels to an entity at first creation, not
retroactively via re-interview.** If you change label text or add new
labeled endpoints, existing paired devices need a full remove-and-re-pair in
HA to pick it up — a "re-interview" (device query) alone won't rename
already-existing entities.

Two things about `FlowMeasurement` entities are hardcoded in HA's own
Matter integration and can't be changed from the firmware at all: the
"Flow"/"Fluss" name prefix and the m³/h unit. See the README's Home Assistant
section for the template-sensor workaround.

## Apple Home Can't Direct-Pair This Topology (Use HA's HomeKit Bridge Instead)

`create_sensor_endpoint()` deliberately puts every sensor as a flat,
top-level endpoint (`ENDPOINT_FLAG_NONE, nullptr` — no Aggregator) instead of
the "real" Matter Bridge pattern (Aggregator + `BridgedNode` sub-endpoints).
That was required to fix a Home Assistant bug: HA only creates sensor
entities for endpoints in ep0's own PartsList, never for endpoints nested
under an Aggregator (see the block comment above `create_sensor_endpoint()`).

The cost: Apple Home's controller is much stricter than HA's about a Root
Node exposing 9+ heterogeneous sensor endpoints with no Aggregator
structure. In practice this surfaces as a generic **"Pairing failed"**
during commissioning or HA's "share device to another fabric" flow — it
happens right after CASE session establishment, while Apple's controller is
reading the Descriptor cluster's DeviceTypeList/PartsList for every
endpoint to build its accessory model.

There is no way to satisfy both controllers with one topology — a Matter
node's structure is identical for every fabric that commissions it, it
can't present differently to HA vs. Apple. The practical fix is to not
direct-pair Apple Home to this device at all: pair it with HA's Matter
integration as normal, then add HA's separate **HomeKit Bridge** integration
(classic HomeKit, unrelated to HA's Matter server) and include the WS90
sensor entities in its filter. That re-exposes the already-working HA
entities to Apple Home over the classic HomeKit protocol, which doesn't
care about the underlying Matter Descriptor structure at all.

Note HomeKit itself only has native accessory types for `temperature`,
`humidity`, and `illuminance` sensors — wind speed/direction, rain, UV
index, and pressure have no HomeKit sensor type to map to and won't appear
in Apple Home via any pairing method, direct or via HA's bridge.

## Testing on Real Hardware

There's no unit test suite — this is validated on real hardware via the
serial monitor. The board is normally connected on **COM5**.

### Local build environment (ESP-IDF works, esp-matter host tools don't — yet)

- **ESP-IDF v5.4** (matching CI's pin) is cloned at `C:\esp\v5.4\esp-idf` and
  fully functional. Activate it **from PowerShell only**, never Git Bash:
  `& "C:\esp\v5.4\esp-idf\export.ps1"`. Git Bash is a hard no — its MSYS
  runtime force-injects `MSYSTEM` into every child process's Windows
  environment block (confirmed: `unset`/`env -u` in bash do not remove it
  from a spawned native `.exe`'s env), and `idf_tools.py` hard-fails
  unconditionally whenever `MSYSTEM` is present, with no override flag. Same
  reason `install.bat` refuses to run under Git Bash.
- **esp-matter** is cloned at `C:\Users\bmuel\esp\esp-matter` with the
  `connectedhomeip/connectedhomeip` submodule and its required `third_party/*`
  submodules (`uriparser`, `nlassert`, `nlio`, `nanopb`, `jsoncpp`,
  `pigweed`) checked out. This requires **Windows Developer Mode enabled**
  (Settings → Privacy & Security → For Developers) — without it, git checks
  out connectedhomeip's ~470 real symlinks (e.g. `scripts/bootstrap.sh` →
  `scripts/setup/bootstrap.sh`) as plain text files containing the link
  target, which breaks everything downstream. Clone/update submodules with
  `git -c core.symlinks=true submodule update --init --depth 1 ...` (a
  one-off flag, not a persisted config change).
- Both of CI's connectedhomeip source patches (see `build.yml`'s "Patch
  connectedhomeip" steps) were applied manually to this checkout: stripping
  `integrations/**/setup.cfg` test-tool requirements, and delaying mDNS
  operational advertisement until IPv4 (`PATCHED_IPV6_SKIP` marker in
  `ConnectivityManagerImpl_WiFi.cpp`). Re-apply both if the submodule is ever
  re-cloned — see `build.yml` for the exact patch scripts.
- Git Bash's default PATH resolves bare `python3` to a broken Windows Store
  alias stub, and connectedhomeip's `bootstrap.sh` calls `python3`
  explicitly — a shim exists at `C:\Users\bmuel\esp\bin\python3` that execs
  the real interpreter (`AppData\Local\Programs\Python\Python312\python.exe`).
  Prepend `C:\Users\bmuel\esp\bin` and the real Python dir to `PATH` in Git
  Bash before running any esp-matter/connectedhomeip setup scripts.
- **Known gap, not yet resolved**: `idf.py set-target esp32c3` succeeds
  fully (CMake configure gets all the way through). `idf.py build` fails at
  `The 'gn' command was not found` — connectedhomeip's Pigweed CIPD bootstrap
  (which would fetch `gn`) has a genuine Windows/Git-Bash bug: `bootstrap.sh`
  passes a path as `windows:/c/Users/.../python311.json` to a native Windows
  Python subprocess; Git Bash's automatic POSIX→Windows path translation
  only fires for bare path-like arguments, not ones with a `windows:` prefix
  before the slash, so the path reaches Python un-translated and `open()`
  fails with `FileNotFoundError`. `gn` itself does list `windows-amd64` as a
  supported CIPD platform (confirmed in `pigweed.json`), so this is a
  narrow, well-understood tooling bug, not a fundamental platform gap — just
  not worth chasing further right now. **Until this is fixed, local builds
  aren't possible; keep relying on CI + hardware serial testing**, same as
  before this environment setup. If a future session wants to finish this:
  either fetch the pinned `gn` build
  (`git_revision:97b68a0bb62b7528bc3491c7949d6804223c2b82`) directly from
  Google's public CIPD service and place it on `PATH`, or set up WSL2
  (matches CI's Ubuntu environment exactly, sidesteps all of the above).

**Claude collects the serial log itself** rather than asking the user to
run/paste it: open COM5 (115200 baud) with a short Python/pyserial script
run via Bash in the background (foreground reads block on a live device),
ask the user to flash/reset/trigger whatever action is needed, then read the
captured file back once the window ends. Notes from doing this a lot in one
session:
- Resetting the board briefly drops the USB-CDC port (re-enumeration) —
  wrap the serial open/read in a retry loop, don't treat one dropped read as
  fatal.
- A capture only needs to be long enough to cover the action being tested
  (a boot: ~10–20s; a live Shelly poll cycle: the poller waits 90s after
  WiFi-up before its first poll, so budget several minutes if that matters).
- If the user reports "I flashed"/"reset done" but nothing shows up in the
  capture, the port was very likely held by something else (the web
  installer's browser tab, a previous capture that's still running, etc.) —
  check for that before assuming the firmware is at fault.

Typical workflow when debugging:
1. Build + flash (`idf.py build && idf.py -p COM5 flash`, or the web
   installer).
2. Watch the serial console (115200 baud) through a boot + commissioning (or
   just a reset, if already commissioned — the fabric survives a plain
   reflash).
3. Look for the specific log lines relevant to what changed — e.g.
   `force-init`/`READBACK` (boot-time attribute init),
   `Updated ep .* FAILED` (live update path), `RegisterFixedLabel`/
   `IterateFixedLabel` (naming), `Free heap` (memory pressure), `poll OK`
   /`unreachable` (Shelly discovery/connectivity), `New sensor type`
   (a device/measurement seen for the first time — see the third gotcha
   above), `pre-created N endpoint(s)` (boot-time endpoint creation).
4. A hard crash shows `chip[-]: chipDie` or `abort() was called` with a
   register dump, immediately followed by `Rebooting...` — if the device
   keeps cycling through boot messages every few seconds without ever
   reaching steady state, it's crash-looping, not just slow to connect.

Known noisy-but-harmless things: `ESP_ERR_NOT_FINISHED` from an attribute
write means "value unchanged, no-op", not a real failure. The very first
Shelly poll after boot occasionally fails once or twice with a `select()
timeout` (transient ARP/network settling) before succeeding continuously —
this is normal, not a regression.

## Hardware Enclosure

`hardware/enclosure/` holds a parametric case generator for the ESP32-C3
Super Mini board this firmware runs on — two Python scripts
(`gen_case.py` for boards with pin headers soldered on, `gen_case_no_pins.py`
for bare/desoldered boards), each producing a `_base.stl` + `_lid.stl` pair.
Full parameter table and usage in `hardware/enclosure/README.md` — the
essentials:

- **No CAD file is the source of truth, the Python constants are.** Both
  scripts build the geometry from named constants (board size, wall
  thickness, clearances, snap-fit rib size, lid corner radius, etc.) via
  `manifold3d` boolean ops, then export STL directly. To change the design,
  edit the constants and rerun (`python gen_case.py`) — don't hand-edit an
  STL or try to reverse-engineer one; if you only have an STL and need to
  know what generated it, check whether its bounding box / wall thickness
  matches these scripts' derived dimensions first (it very likely does).
- **The lid closure is already a snap-fit** (a continuous ridge on the
  base's inner walls seats into a matching groove in the lid's lip) — not a
  full-perimeter friction/interference fit. `RIDGE_R`/`GROOVE_R` must stay
  well under `LID_LIP_H / 2` or the ridge/groove band spans past both ends of
  the lip and the snap loses its resist-then-click feel (see the comments
  above `RIDGE_R` in either script for the exact failure mode that happened
  once already).
- **Printed in ABS**, which shrinks more (and less predictably) than
  PLA/PETG. Getting the board's fit right took three iterations — worth
  knowing all three failure modes if it ever needs revisiting:
  1. `FIT_SLACK=0.5mm`: the ABS cavity printed tight enough along the
     board's full insertion depth that it couldn't be seated at all.
  2. `FIT_SLACK=0.7mm`, then `1.0mm`: fixed that, but `FIT_SLACK` was at
     the time sizing *both* the board's resting cavity *and* the rim zone
     the snap-fit ridge lives in — the board's edges sweep past the ridge's
     Z-band while sliding down to the pegs, so the ridge has to clear the
     board, not just the lid's lip, and `FIT_SLACK` had to be
     `>= RIDGE_PROTRUSION` (0.8mm) for that. Fixing the ridge collision this
     way also fixed the board's own resting cavity to the same wide value,
     which meant it fit but rattled — 1.5-2mm of side-to-side play.
  3. **Current design**: `FIT_SLACK` and the ridge/lip zone are decoupled.
     `FIT_SLACK=0.35mm` sizes only the board's own resting cavity (snug,
     ~0.7mm total play). A separate `RIM_SLACK=1.0mm` independently sizes a
     *wider* rim recess (`rim_x0`/`rim_y0`/`rim_l`/`rim_w`) — centered in
     the same outer envelope, but wider than the board cavity — that both
     the lid's lip/groove *and* the base's snap ridge are anchored to
     instead of the tight cavity. This makes the top of the case a funnel:
     wide enough at the ridge's height for the board to pass freely, then
     narrowing to a snug fit lower down where the board actually rests.
     `RIM_SLACK` must stay `>= RIDGE_PROTRUSION` (0.8mm) with margin, same
     constraint as before, just applied to the independent rim dimensions
     instead of to `FIT_SLACK`.
  **Verify any future change to `FIT_SLACK`, `RIM_SLACK`, `RIDGE_R`, or
  `RIDGE_PROTRUSION`** with a `manifold3d` boolean intersection between
  `base` and a probe spanning the *entire* board footprint swept through
  the ridge's Z-band (zero volume = confirmed clear) — don't just eyeball
  it or check one edge point; both this and the original 0.1mm miss were
  caught by that exact check, not by inspection.
- **Z play** (board can shift up/down inside the closed case): governed by
  `TOP_CLEARANCE` (headroom above the board for the tallest component, the
  ESP32-C3 module/shield can) — there's no positive downward retention, the
  board just rests on the corner pegs by gravity. Trimmed from 4.5mm to
  4.0mm after test-fit reported ~1mm of Z play, conservatively (leaves
  ~0.5mm margin rather than zeroing it out — re-check if this ever gets too
  tight to close, and reduce further only after a successful test-fit).
- Current tuned values (both variants): `FIT_SLACK=0.35mm`,
  `RIM_SLACK=1.0mm`, `TOP_CLEARANCE=4.0mm`, `LID_LIP_H=3.0mm`, `LID_T=1.2mm`.
  Outer footprint 26.4 x 21.9mm (plus a ~0.95mm local boss on each side wall
  at the ridge's height, see below). Total assembled height ~16.7mm
  (`gen_case.py`) / ~11.0mm (`gen_case_no_pins.py`).
- **Ridge boss**: widening the rim zone independently (via `RIM_SLACK`, see
  above) shrank the wall thickness available behind the snap ridge at
  `rim_y0`/`rim_y1` — with `RIDGE_R=1.2mm`/`RIDGE_PROTRUSION=0.8mm` the ridge
  needs `2*RIDGE_R - RIDGE_PROTRUSION = 1.6mm` of solid wall to stay fully
  contained, but `rim_y0` alone is only ~0.95mm, so the ridge cylinder used
  to poke ~0.65mm straight through the *outer* wall face (visible/printable
  as an exposed lump on the case exterior). Fixed with a small local boss —
  extra wall material added only on the outside, only across the ridge's own
  Z-band (`ridge_boss_left`/`ridge_boss_right` in both scripts) — rather than
  changing `RIM_SLACK` or the ridge dimensions themselves, since those are
  both load-bearing for other already-verified fixes (board clearance, snap
  engagement feel). Verified with the same `manifold3d` boolean-probe
  approach as the other fixes: zero material beyond the boss's own outer
  face, and zero board/ridge overlap (unchanged from before).
- **Logo engraving**: `add_svg_logo()` traces a single-`<path>` SVG and cuts
  it into the lid top (recess, not raised boss — prints cleaner with the lid
  flipped, logo face down, no overhangs). Configured via the `LOGO_SVGS` list
  at the top of each script; `ENGRAVE_DEPTH` must stay less than `LID_T`.
  `matter-logo.svg` (Matter smart-home logo, engraved 18mm wide/~17.6mm
  tall, centered, 0.6mm deep — sized against the lid's tighter Y dimension
  with ~2mm margin before the rounded corners) is wired in as of the current
  version — for personal/non-commercial use on this owner's own hardware.

## Git & CI Workflow

- **Push and tag freely without asking first** — `git push origin master`
  then `git tag vX.Y.Z && git push origin vX.Y.Z` to trigger a release build.
  Master has PR-required branch protection that the repo owner bypasses
  directly; this is the established, expected workflow here (unlike other
  repos this owner works in, which use PRs).
- Every real firmware change bumps the version in **both**
  `CMakeLists.txt` (`project(ble_matter_bridge VERSION x.y.z)`) **and**
  `main/CHIPProjectConfig.h` (`CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION` +
  `_STRING`) — CI also independently patches these from the git tag at build
  time, so the committed values mostly matter for local builds and for
  `git diff` readability between tags.
- CI (`.github/workflows/build.yml`) has two jobs: `build` (full firmware
  build + GitHub Release) and `deploy-pages` (publishes the web installer,
  `needs: build`). A flaky Pages deploy can be retried with
  `gh run rerun --failed` in seconds, without rebuilding firmware.
- CI also **skips the ~17-minute firmware build entirely** if nothing under
  `main/`, `components/`, `CMakeLists.txt`, `partitions.csv`, or
  `sdkconfig.defaults` changed since the previous tag (ignoring the two
  auto-injected version lines) — it re-publishes the previous release's
  binaries under the new tag instead. Useful for tags pushed only to pick up
  a CI/docs change.
- Splitting a change into a "safe" commit/tag and a "risky" commit/tag (e.g.
  a well-documented API vs. one relying on unverified internals) has been a
  useful pattern here when a build failure in one shouldn't block the other.

## Session Discipline

This is a single-firmware embedded project (no separate backend/iOS/Android
components like some of this user's other repos) — no special
component-isolation discipline is needed here.
