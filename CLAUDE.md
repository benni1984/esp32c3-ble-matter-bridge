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
serial monitor. ESP-IDF + esp-matter are installed locally on this machine
(not just in CI/Docker), and the board is normally connected on **COM5**.

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
