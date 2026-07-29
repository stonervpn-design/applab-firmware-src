# WhisperPair Surface Detector — Design Spec

**Date:** 2026-07-21
**Component:** `applications/main/ble_spam` (the BLE app)
**Target hardware:** M5Stack Cardputer ADV (ESP32-S3, native BLE) + StickS3 sibling
**Status:** Approved design — ready for implementation plan

---

## 1. Goal

Add a **detector** for **WhisperPair (CVE-2025-36911)** — the Google Fast Pair
pairing-mode logic flaw disclosed by KU Leuven COSIC (~Jan 2026). The detector
identifies nearby Fast Pair accessories, enumerates their WhisperPair attack
surface over GATT, and reports an exposure verdict — **without firing the
exploit**. It mirrors the existing Airoha RACE detector (`scene_race_detector.c`),
which is likewise detection-only ("no read/write, no memory access").

This is explicitly a **surface + known-model** detector. It is **NOT**:

- the weaponized WhisperPair chain (forced bond + Account-Key implantation for
  covert tracking / eavesdropping) — deliberately out of scope, same line the
  RACE detector holds;
- the *active* pairing-mode-predicate probe (writing an out-of-pairing-mode
  Key-Based Pairing request). That would give a definitive vulnerable/patched
  verdict but requires the KBP AES/ECC handshake and is a future opt-in mode.

Success criteria:

1. Scanning lists nearby BLE devices advertising the Fast Pair service (0xFE2C),
   each tagged *discoverable* (Model ID advertised) or *has-accounts* (account-key
   filter advertised).
2. Selecting a device connects, confirms the 0xFE2C service, confirms the writable
   **Key-Based Pairing** characteristic (`FE2C1234…`) is present, and **reads** the
   Model ID characteristic (`FE2C1233…`) — then disconnects. **No characteristic is
   ever written.**
3. The device shows a verdict: `KBP surface`, `LIKELY VULN` (surface + name on the
   known-vulnerable list), `FP (unverified)`, `Not Fast Pair`, or `Connect fail`.
4. Reachable from a new `"WhisperPair"` menu entry beside `"Airoha RACE"`.
5. Enter/exit leaves the BLE stack cleanly stopped (scan cancelled, disconnected,
   `ble_walk_hal_stop`) — no stuck connection.
6. Both build targets (Cardputer ADV + StickS3) still report `Project build
   complete`.

Non-goals (v1): no SD logging, no active KBP write/probe, no bond, no Account-Key
write, no settings screen (thresholds/lists are compile-time).

---

## 2. Background — what already exists

`applications/main/ble_spam/`:

- **`ble_walk_hal.c/.h`** — a full NimBLE GATT client already used by the RACE
  detector: `start/stop`, `start_scan/stop_scan`, `get_devices` (each
  `BleWalkDevice` carries `addr/addr_type/rssi/name/adv_data[31]/adv_data_len/
  scan_rsp_data[31]/scan_rsp_len`), `connect(dev, abort_flag)/disconnect`,
  `discover_services/services_ready/get_services`, `discover_chars(service)/
  chars_ready/get_chars`, `read_char(handle)/read_ready/get_read_value/
  get_read_status`, and `write_char(...)` (**not used by this feature**).
  `BleWalkService{ uuid, start_handle, end_handle }`, `BleWalkChar{ uuid, handle,
  properties }`; `esp_bt_uuid_t` supports 128-bit UUIDs (`ESP_UUID_LEN_128`).
- **`scenes/scene_race_detector.c`** + **`views/race_detector_view.c/.h`** — the
  structural template: scan → merge into a list model → Up/Down select → OK runs a
  blocking `probe_device()` (connect → discover services → match UUID → disconnect)
  → status per device. `RaceStatus{ Unknown, Probing, Vulnerable, Clean,
  ConnectFail }`. Registered via: view enum + pointer in `ble_spam_app.h/.c`, scene
  enum in `scenes/scenes.h` + `scenes/scene_config.h`, menu entry in
  `scenes/scene_main.c`, `app->race_probe_abort` flag on `BleSpamApp`.

**Fast Pair facts (from Google's official spec, verified 2026-07-21):**

- Service UUID **0xFE2C** (Bluetooth SIG assigned).
- Characteristics (128-bit, all share suffix `-8366-4814-8EB0-01DE32100BEA`):
  | Name | UUID | Props |
  |---|---|---|
  | Model ID | `FE2C1233-…` | Read |
  | Key-based Pairing | `FE2C1234-…` | Write + Notify |
  | Passkey | `FE2C1235-…` | Write + Notify |
  | Account Key | `FE2C1236-…` | Write |
  | Additional Data | `FE2C1237-…` | Write + Notify |
- Advertisement service data under 0xFE2C: **exactly 3 bytes** = 24-bit Model ID
  → provider is *discoverable / in pairing mode*. **Longer**, leading byte =
  version/flags → Account-Key filter (Bloom) → *non-discoverable*, has bonded
  accounts. Payload length is the reliable discriminator.

WhisperPair vs Airoha RACE (already implemented) are **different CVEs / protocols /
services** — this feature is additive and does not touch the RACE detector.

---

## 3. Design

### 3.1 Fast Pair constants + known-vulnerable list — `whisperpair_fp.h` (new)

Header-only. Holds:

```c
// 0xFE2C characteristic UUIDs as ESP-IDF little-endian 128-bit byte arrays
// (textual UUID reversed), matching the RACE detector's UUID-array convention.
static const uint8_t FP_UUID_MODEL_ID[16]  = { /* FE2C1233-... reversed */ };
static const uint8_t FP_UUID_KBP[16]       = { /* FE2C1234-... reversed */ };
// (Passkey / Account Key / Additional Data optional — only Model ID + KBP are
//  needed for the verdict; add if the view wants to show the full char set.)

#define FP_SERVICE_UUID16 0xFE2C
```

Known-vulnerable **name-substring** table (from whisperpair.eu/vulnerable-devices):

```c
typedef struct { const char* needle; } FpVulnName;
static const FpVulnName FP_VULN_NAMES[] = {
    {"WH-1000XM6"}, {"WH-1000XM5"}, {"WH-1000XM4"}, {"WF-1000XM5"}, {"WH-CH720N"},
    {"Pixel Buds Pro 2"}, {"Nord Buds 3 Pro"}, {"Redmi Buds 5 Pro"},
    {"MOTIF II"}, {"Liberty 4 NC"}, {"Elite 8 Active"}, {"Tune Beam"},
    {"Nothing Ear"}, /* ... */
};
```

Cross-reference is by **device name substring** (case-insensitive `strcasestr`-style
scan) because mapping a raw 24-bit Model ID to a product name needs Google's model
DB, which we don't ship. The raw Model ID is displayed as hex regardless.

Rationale for name-based match: the adv/GAP `name` is what we actually have; the
list is a *hint layer* on top of the surface verdict, never the sole signal.

### 3.2 Fast Pair advertisement parse — `whisperpair_fp.h` inline helper (pure)

```c
typedef enum { FpAdvNone, FpAdvDiscoverable, FpAdvHasAccounts } FpAdvKind;

// Scan an advert TLV buffer for 0xFE2C service data (AD type 0x16, uuid16 0xFE2C).
// 3-byte payload -> FpAdvDiscoverable (+ 24-bit model id out). Longer -> FpAdvHasAccounts.
// Absent -> FpAdvNone. Pure: <stdint.h>/<stdbool.h>/<string.h> only, host-testable.
FpAdvKind fp_adv_parse(const uint8_t* buf, uint8_t len, uint32_t* model_id_out);
```

Bounds-checked TLV walk identical in shape to `classify_buffer()` in
`ble_tracker_hal.c` (field-length guard, no over-read). Also scans the scan-response
buffer (`ble_walk_hal` stores both).

### 3.3 Verdict logic — in `scene_whisperpair_detector.c`

```c
typedef enum {
    WpUnknown,      // not yet probed
    WpProbing,
    WpNotFastPair,  // connected, but no 0xFE2C service
    WpConnectFail,
    WpSurface,      // 0xFE2C + writable KBP char present (exposed surface)
    WpLikelyVuln,   // WpSurface AND name matches FP_VULN_NAMES
} WpStatus;
```

`probe_device()` (cloned from RACE, **read-only**):

1. `ble_walk_hal_connect(&walk_dev, &app->wp_probe_abort)` — fail → `WpConnectFail`.
2. `discover_services()`, poll `services_ready()` up to ~3 s (abortable) — match
   `FP_SERVICE_UUID16` (a 16-bit service UUID; NimBLE may surface it as a 16-bit or
   the SIG 128-bit base UUID — match both forms). Absent → `WpNotFastPair`.
3. `discover_chars(fp_service)`, poll `chars_ready()` — require `FP_UUID_KBP`
   present (writable) → this is the WhisperPair surface. If found, optionally
   `read_char(model_id_handle)` for `FP_UUID_MODEL_ID` to capture the 24-bit Model
   ID (best-effort; a failed read does not change the verdict).
4. `disconnect()`.
5. `WpSurface`, upgraded to `WpLikelyVuln` if the stored device `name` matches
   `FP_VULN_NAMES`.

**No `write_char` call exists anywhere in this file.**

### 3.4 Scene — `scenes/scene_whisperpair_detector.c` (new)

1:1 clone of `scene_race_detector.c`:

- `on_enter`: reset model, switch view, `ble_walk_hal_start()` (→ back out on
  fail), `ble_walk_hal_start_scan()`.
- Tick: `refresh_device_list()` — pull `ble_walk_hal_get_devices()`, keep only
  devices where `fp_adv_parse()` on their `adv_data`/`scan_rsp` != `FpAdvNone`,
  merge into the model (preserve verdict of known MACs, carry `FpAdvKind` + Model
  ID), unless `model->probing`.
- Up/Down: move selection (windowed, as RACE).
- OK: mark `WpProbing`, `stop_scan`, run blocking `probe_device()`, write result
  back by MAC match, `start_scan`.
- Back / `on_exit`: set `wp_probe_abort`, `disconnect`, `stop_scan`, `stop`.

### 3.5 View — `views/whisperpair_detector_view.c/.h` (new)

Clone of `race_detector_view`. Model:

```c
typedef struct {
    uint8_t addr[6]; esp_ble_addr_type_t addr_type;
    char name[32]; int8_t rssi;
    FpAdvKind adv_kind; uint32_t model_id; bool model_id_valid;
    WpStatus status;
} WpDevice;
// + WpDetectorModel{ devices[WP_DETECTOR_MAX_DEVICES], count, selected,
//                    window_offset, probing }
```

Draw: header `"WhisperPair (FE2C)"`; each row = name (or MAC), a small badge for
`adv_kind` (`disc`/`acct`), verdict label, RSSI. A `WpLikelyVuln` row is emphasized
(inverted), matching how RACE emphasizes `Vulnerable`. A footer line states the
scope honestly, e.g. `"surface scan - not a live exploit"`.

### 3.6 Registration (mirror RACE exactly)

- `scenes/scenes.h` + `scenes/scene_config.h`: `ADD_SCENE(ble_spam,
  whisperpair_detector, WhisperpairDetector)`.
- `ble_spam_app.h`: `BleSpamViewWhisperpairDetector` enum + `View*
  view_whisperpair_detector;` + `volatile bool wp_probe_abort;` on `BleSpamApp`.
- `ble_spam_app.c`: alloc the view in app-alloc, add to dispatcher, free in
  app-free (paired with the RACE view lines).
- `scenes/scene_main.c`: one submenu item `"WhisperPair"` → switch to
  `BleSpamSceneWhisperpairDetector`, placed next to `"Airoha RACE"`.

---

## 4. Testing

**Host unit test** (pure, `gcc`/WSL — like `test_tracker_follow.c`):
`tests/test_whisperpair_fp.c` for `fp_adv_parse`:
- 0xFE2C service data, 3-byte payload → `FpAdvDiscoverable` + correct 24-bit
  little-endian Model ID.
- 0xFE2C service data, ≥5-byte payload w/ leading flags byte → `FpAdvHasAccounts`.
- no 0xFE2C service data → `FpAdvNone`.
- malformed/over-long AD field length → no over-read, returns `FpAdvNone`.
- name matcher: `FP_VULN_NAMES` needle present/absent, case-insensitive.

**On-hardware:**
- Put a phone advertising Fast Pair (or Fast Pair earbuds) nearby → appears in the
  list tagged `disc`/`acct`. OK → verdict resolves to `KBP surface` (or `LIKELY
  VULN` for a listed model), Model ID shown; a non-Fast-Pair device selected →
  `Not Fast Pair`.
- Enter/exit + repeated probes: scan stops, connection closes cleanly, no hang.

**Build gate:** `build_fix.bat` (Cardputer ADV) and `build_m5stick_s3.bat` both
reach `Project build complete` / `SENTINEL_OK`.

---

## 5. Risks

1. **16- vs 128-bit service UUID surfacing** — NimBLE may report 0xFE2C as a 16-bit
   UUID or as the SIG 128-bit base UUID (`0000FE2C-0000-1000-8000-00805F9B34FB`).
   Mitigation: the service match checks both forms.
2. **Passive scan omits service data** — some providers put 0xFE2C data only in the
   scan response. `ble_walk_hal` already captures `scan_rsp_data`; `fp_adv_parse`
   scans both buffers. If the walk HAL scans passively and a device only replies
   actively, it may be missed — acceptable for v1 (documented).
3. **Model-ID → product name gap** — can't name a device from its 24-bit Model ID
   without Google's DB; the known-vuln match is name-based and therefore a *hint*,
   not a guarantee. The `FP (unverified)` label communicates this; we never claim
   "patched."
4. **GATT connect contention** — same `ble_walk_hal` the RACE detector drives;
   probe stops the scan first and disconnects after, exactly as RACE does. Only one
   tracker/walk feature runs at a time (existing app invariant).
5. **Scope creep toward the exploit** — the KBP characteristic is *writable*, and
   writing to it is the actual attack. This feature contains **no** `write_char`
   call; the verdict is derived purely from service/characteristic presence + a
   read of Model ID. Enforced by code review of `scene_whisperpair_detector.c`.

---

## 6. File structure

- **New:** `applications/main/ble_spam/whisperpair_fp.h` — 0xFE2C char UUID arrays,
  `FP_VULN_NAMES`, `FpAdvKind` + pure `fp_adv_parse()` + name matcher.
- **New:** `applications/main/ble_spam/scenes/scene_whisperpair_detector.c` —
  scan/select/probe scene (read-only), cloned from the RACE scene.
- **New:** `applications/main/ble_spam/views/whisperpair_detector_view.c` / `.h` —
  list view, cloned from `race_detector_view`.
- **New:** `tests/test_whisperpair_fp.c` — host unit tests for `fp_adv_parse` +
  name matcher.
- **Modify:** `scenes/scenes.h`, `scenes/scene_config.h` — register the scene.
- **Modify:** `ble_spam_app.h`, `ble_spam_app.c` — view enum/pointer/alloc/free +
  `wp_probe_abort`.
- **Modify:** `scenes/scene_main.c` — one `"WhisperPair"` menu item.

---

## Note on version control

Repo `Flipper-Zero-meets-M5Stack-Cardputer-1.15`, branch `nimble-port`. Spec
committed alongside the plan. The tree currently has unrelated uncommitted work
(NimBLE migration in progress) — the spec commit is scoped to this file only.
