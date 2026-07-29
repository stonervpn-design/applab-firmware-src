# PMKID Clientless Capture — Design Spec

**Date:** 2026-07-08
**Component:** `applications/main/wlan_app`
**Target hardware:** M5Stack Cardputer ADV (ESP32-S3, native WiFi)
**Status:** Approved design — ready for implementation plan

---

## 1. Goal

Add a **clientless WPA/WPA2 PMKID capture** attack to the WiFi app: obtain a
crackable PMKID from a chosen access point **without any client present**, with
a **deauth-assisted passive capture** as an automatic backstop.

Output is appended to the existing `/ext/wifi/pmkid.22000` file in hashcat
`-m 22000` format (`WPA*01*<pmkid>*<ap_mac>*<sta_mac>*<essid_hex>***`).

Success criteria:

1. Against a WPA2 AP that supports PMKID caching, the app solicits and captures
   a non-zero PMKID **with no client associated to that AP**.
2. The captured line cracks against the known PSK with `hashcat -m 22000`.
3. If manual association yields no M1 within the timeout, the deauth fallback
   still captures a PMKID from a reconnecting real client (when one exists).
4. App enters/exits cleanly with no promiscuous-mode or thread leaks (mirrors
   the lifecycle discipline of `scene_handshake.c`).

Non-goals (v1, explicitly deferred):

- Multi-AP sweep / attack-all. One target at a time.
- A dedicated settings screen. Channel is taken from the selected target.
- A new output file or format. Reuse `/ext/wifi/pmkid.22000` and its writer.

---

## 2. Background — what already exists

The handshake scene (`scenes/scene_handshake.c`) already:

- Runs a promiscuous RX callback into a ring buffer (`HsPkt` pool).
- Parses 802.11 → EAPOL via `wlan_handshake_parser` helpers
  (`wlan_hs_parse_addresses`, `wlan_hs_is_eapol`, `wlan_hs_get_eapol_msg_num`).
- **Extracts a PMKID from an EAPOL M1** via `wlan_hs_extract_pmkid()`
  (parser already implemented; PMKID KDE `DD 14 00 0F AC 04 <16 bytes>` at
  key-data offset 99).
- **Writes hashcat-22000** via `hs_save_pmkid()` to `/ext/wifi/pmkid.22000`.
- Fires deauth+disassoc bursts (`hs_deauth_burst`) with a quiet window so
  clients reconnect and complete the 4-way handshake.

**The gap:** all of the above is *passive* — a PMKID only lands when a **real
client** associates (or is deauthed and reconnects). There is no path where the
Cardputer itself solicits the M1. That "clientless" solicitation is the new
capability this spec adds.

---

## 3. Approach

**A (primary) — manual association / true clientless.**
The app injects its own management frames at the target BSSID:

1. **Authentication** (open system, seq 1), source = our STA MAC, dest = BSSID.
2. **Association Request** carrying an **RSN Information Element** that advertises
   PMK caching support, so the AP includes an RSN PMKID KDE in EAPOL M1.

The AP replies (Auth success → Assoc Response) and sends **EAPOL-Key M1
addressed to our STA MAC**, which contains the PMKID. We capture M1 through the
existing promiscuous path, run `wlan_hs_extract_pmkid()`, and save. No client
required.

**C (fallback) — deauth-assisted passive.**
If manual association produces no M1 within a timeout, start a deauth burst
against the target to knock real clients off; their reconnection M1s are
harvested through the same capture path. This is the backstop for APs that
ignore injected association frames.

Both paths live in **one scene** and share the capture pipeline. Approach B
(`esp_wifi_connect` with a dummy PSK) is rejected: this IDF version does not
cleanly surface the raw M1 to the app layer.

---

## 4. Architecture

New scene `WlanAppScenePmkid` + one view, plus a small pure-logic frame-builder
module and one HAL change.

```
 target selection (existing)          scene_pmkid.c                 wlan_hal.c
   app->target_ap  ───────────────▶  state machine        ┌─▶ wlan_hal_set_channel
                                       │                    ├─▶ wlan_hal_send_raw (cap 64→128)
                          ┌── ASSOCIATING (A) ──────────────┤   (auth + assoc-req frames)
                          │            │                    └─▶ wlan_hal_set_promiscuous(cb)
                          │            ▼
                          │      WAITING_M1  ◀── promiscuous cb ─── EAPOL M1 (to our MAC)
                          │            │                             │
                          │            │        wlan_handshake_parser (existing)
                          │            │         wlan_hs_extract_pmkid()
                          │            ▼                             │
                          │       CAPTURED ──▶ hs_save_pmkid() ──▶ /ext/wifi/pmkid.22000
                          │            ▲
                          └── DEAUTH_FALLBACK (C) ── hs_deauth_burst pattern
```

### 4.1 New files

- `pmkid_frames.c` / `pmkid_frames.h` — **pure functions**, no ESP/furi deps,
  host-testable:
  - `size_t pmkid_build_auth_open(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6]);`
  - `size_t pmkid_build_assoc_req(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6], const char* ssid);`
    (channel is not part of the assoc-req body; it is set on the radio via `wlan_hal_set_channel`.)
    - Emits: frame control (mgmt, subtype assoc-req), addresses, capability +
      listen-interval, SSID tag, supported-rates tag, and an **RSN IE** with
      group/pairwise cipher = CCMP, AKM = PSK, and RSN capabilities set so the
      AP returns a PMKID in M1.
  - Return value = bytes written, or 0 on `cap` overflow.
- `scenes/scene_pmkid.c` — the scene (on_enter/on_event/on_exit).
- `views/wlan_pmkid_view.c` / `.h` — the status view.

### 4.2 Modified files

- `scenes/scene_config.h` — add `ADD_SCENE(wlan_app, pmkid, Pmkid)`.
- `wlan_hal.c` / `wlan_hal.h` — **lift the raw-TX cap**: `WlanCmd.send_raw.buf`
  grows from 64 to 128 bytes and `wlan_hal_send_raw()` rejects `len > 128`.
  (An Assoc Request with an RSN IE is ~55–80 bytes; the current `len > 64`
  reject would drop it.) Deauth (26 B) is unaffected.
- `application.fam` (metapackage or app list) — no new appid; scene lives in the
  existing `wlan_app`. Add the menu entry in the WiFi target/attack menu that
  routes to `WlanAppScenePmkid`.
- `wlan_app.h` — add any scene-scoped state fields to `WlanApp` if needed
  (prefer scene-local statics as `scene_handshake.c` does; add to the struct
  only for cross-scene/view-model data).

### 4.3 Reused as-is

- Promiscuous RX + ring buffer pattern (copy the guarded-callback idiom from
  `scene_handshake.c`; do **not** share its statics — keep a separate small
  pool to avoid cross-scene coupling).
- `wlan_handshake_parser` (all EAPOL/PMKID parsing).
- `hs_save_pmkid()` writer — **refactor**: it is currently `static` in
  `scene_handshake.c`. Extract it to a shared spot (e.g. a small
  `wlan_pmkid_out.c/.h`, or make it non-static and declare in a header) so both
  scenes call one implementation. No behavior change.
- Deauth burst pattern (`deauth_tmpl`, burst + quiet window) — replicate the
  minimal burst logic in the fallback; do not depend on handshake-scene statics.

---

## 5. State machine (scene)

States: `ASSOCIATING → WAITING_M1 → (CAPTURED | DEAUTH_FALLBACK → CAPTURED)`.

| State | Action | Transition |
|---|---|---|
| `ASSOCIATING` | set target channel; enable promiscuous with PMKID cb; inject Auth then Assoc-Req; increment attempt counter | after each attempt → `WAITING_M1` |
| `WAITING_M1` | drain ring buffer; on EAPOL M1 from BSSID→our MAC with non-zero PMKID, save → `CAPTURED`; else after `ASSOC_RETRY_MS` with attempts left → `ASSOCIATING`; after `FALLBACK_AFTER_MS` → `DEAUTH_FALLBACK` | see cells |
| `DEAUTH_FALLBACK` | run deauth bursts at target + quiet windows; keep draining for any client M1 | on PMKID → `CAPTURED`; user Back → exit |
| `CAPTURED` | stop injection/deauth; show PMKID hex + save path; keep promiscuous off | user Back → exit |

Tunables (constants): `ASSOC_RETRY_MS ≈ 800`, `FALLBACK_AFTER_MS ≈ 8000–10000`,
`MAX_ASSOC_ATTEMPTS ≈ 10`. Driven from the scene-manager **Tick** handler (no
`furi_delay_ms` in the dispatcher path — same rule the handshake scene follows).

---

## 6. Capture details

- **Channel:** `wlan_hal_set_channel(target->channel ? target->channel : 1)`
  before injecting.
- **Own MAC:** `wlan_hal_get_own_mac()`; used as source of injected frames and
  as the expected M1 destination for the filter.
- **M1 filter:** frame type = data, EAPOL LLC/SNAP (0xAA AA … 88 8E), BSSID =
  target, and (for the clientless path) destination = our MAC. In fallback,
  accept M1 for the target BSSID regardless of station (real clients).
- **PMKID guard:** `wlan_hs_extract_pmkid()` already rejects an all-zero PMKID;
  only a non-zero PMKID counts as success.

---

## 7. Lifecycle & safety

The port has a documented history of task/promiscuous races (`BUGFIXES.md`).
Treat teardown as first-class:

- `on_exit` must: disable promiscuous, stop the deauth burst/thread and **join**
  it before freeing, drain+free the ring pool, close storage, and clear all
  scene statics — regardless of which state the scene was in.
- The promiscuous callback must guard against a freed/NULL pool (copy the
  `if(!s_pkt_pool) return;` idiom).
- No dynamic allocation inside the RX callback; copy into the ring only.

---

## 8. Testing

**Host unit tests** (pure logic, no hardware) — matches the existing host-test
harness pattern:

- `pmkid_build_auth_open()` — assert frame-control byte, addresses at correct
  offsets, total length.
- `pmkid_build_assoc_req()` — assert SSID/rates/RSN tags present, RSN IE cipher
  (CCMP) / AKM (PSK) / capability bytes correct, `cap` overflow returns 0,
  resulting length ≤ 128.
- Feed a canned EAPOL M1 byte blob (with a known PMKID KDE) through
  `wlan_hs_extract_pmkid()` and assert the 16-byte output.

**On-hardware:**

- Against a WPA2 AP known to support PMKID caching (most home routers), with **no
  client** on that AP: confirm M1 arrives addressed to our MAC and PMKID is
  non-zero; verify `/ext/wifi/pmkid.22000` line cracks with `hashcat -m 22000`
  against the known PSK.
- Against an AP that ignores injected assoc frames: confirm the deauth fallback
  engages after the timeout and captures a reconnecting client's PMKID.
- Enter/exit the scene 10× rapidly; confirm no crash, no stuck promiscuous, no
  leaked deauth thread (watch heap + logs).

---

## 9. Risks

1. **RSN IE correctness** — some APs only emit a PMKID when the assoc-req RSN
   capabilities request it. The builder must set these bytes precisely; the host
   test pins the layout, hardware validates behavior.
2. **Manual assoc injection tuning** — injected Auth/Assoc may need per-AP
   retries/timing. The deauth fallback (C) is the approved backstop for APs that
   won't answer.
3. **Raw-TX cap change blast radius** — growing `send_raw.buf` to 128 slightly
   enlarges each queued `WlanCmd`; verify the command queue depth/stack budget
   is unaffected (it is small; confirm on build).

---

## 10. Open items to confirm during implementation

- Exact WiFi menu entry point / scene name used to route into
  `WlanAppScenePmkid` (read `scene_main.c` / attack-target menu wiring).
- Whether `hs_save_pmkid()` is best extracted to a shared file vs. exposed via a
  header (pick the smaller diff that removes duplication).
- `WlanCmd` queue item size after the buffer grows — confirm no queue-size
  assumptions break.

---

## Note on version control

This project directory is **not** a git repository (downloaded release
`Flipper-Zero-meets-M5Stack-Cardputer-1.15`), so the brainstorming "commit the
design doc" step does not apply. The spec lives at
`docs/superpowers/specs/2026-07-08-pmkid-clientless-capture-design.md`.
