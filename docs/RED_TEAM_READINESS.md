# Red-Team Readiness Roadmap

**Date:** 2026-07-08 · Evidence-based audit of the port-specific code across four dimensions.
Shareable visual version published as a Claude Artifact.

**Verdict:** The problem is not breadth (this port has more offensive surface than most). It's
*trust* — a crash mid-op, your real MAC in a target's logs, or loot overwritten on a re-run.
Close those before adding features.

**Scorecard:** Reliability = Needs work · OPSEC = Weak · Loot/Output = Needs work · Workflow = Functional.

---

## P0 — Field-blocking (fix before the next engagement; all small)

> **Status 2026-07-08: P0 pass DONE** (commits `aa54235`, `0afd2da`, `711f2ba`; final review READY TO MERGE; on-hardware validation pending). #1 and #2 landed as-is. #3 ("overwrite") was found on investigation to be a non-defect — real captures already append/auto-number and the two `CREATE_ALWAYS` files are latest-state stores — so it was replaced with **timestamped LAN host snapshots** (`/ext/wifi/lan/<ssid>_<time>.csv`).

1. ~~**Lock the SD ↔ CC1101 shared SPI3 bus**~~ ✅ DONE — Reliability / Critical / S
   `components/furi_hal/furi_hal_sd.c:573` and `:757` call `sd_release_host()` without the SPI bus
   lock every other SD path holds. SD + Sub-GHz share SPI3 → interleaved transactions corrupt both /
   hang. Highest crash risk. Wrap in `furi_hal_spi_bus_lock()/unlock()`.

2. ~~**Randomize the transmit source MAC**~~ ✅ DONE — OPSEC / High / S
   `esp_wifi_set_mac()` was called nowhere. PMKID/assoc injection (`scene_pmkid.c:184,126`) and the Evil
   Portal SoftAP (`wlan_evil_portal.c:942`) transmit the real factory MAC (Espressif OUI) — logged by
   targets, constant across engagements, even written into `pmkid.22000`. Reuse the LAA generator at
   `wlan_hal.c:653`.

3. ~~**Stop overwriting loot on re-run**~~ → replaced with **Timestamped LAN snapshots** ✅ DONE — Loot / S
   Investigation showed no real loot-loss bug: handshake PCAPs already auto-number, PMKID/creds/BLE-walk
   append. The two `CREATE_ALWAYS` files (`wlan_passwords.c:50` password store, `wlan_lan_cache.c:81`
   host cache) are latest-state stores keyed by SSID where overwrite is correct. Instead, `wlan_lan_cache_save`
   now also writes an additive dated snapshot (`<ssid>_<YYYYMMDD_HHMMSS>.csv`) so network re-scans are
   preserved as evidence.

## P1 — Pro-grade hardening

4. ~~**Fix BadUSB device tells**~~ ✅ DONE (`912c74f`) — OPSEC / High / S
   HID descriptor default strings changed from "Flipper Devices Inc." / "Flipper Zero" / "FZESP32" to
   a Logitech-consistent identity ("Logitech" / "USB Receiver" / neutral serial), matching the existing
   Logitech VID/PID. Per-payload override preserved.

5. ~~**Harden the accepted concurrency residuals**~~ ✅ DONE (P5 pass) — Reliability / Medium / M
   PMKID/handshake teardown UAF window; unsynchronized device counts (`ble_walk_hal.c:28`,
   `ble_tracker_hal.c:16`); Evil Portal bridge flags (`wlan_evil_portal_bridge.c:16`). Add in-callback
   active-gate + `volatile`/critical-sections.
   Fixed: PMKID + handshake capture-pool teardown UAF (portMUX free-vs-write guard); BLE walk/tracker device count now acquire/release published; Evil Portal bridge gate flags serialized under one portMUX (incl. two WiFi-event-task accesses caught in review — bridge_try_fallback attempting-write and the ip-handler pending read). Cosmetic torn-field residual on already-published BLE entries accepted. **Not yet hardware-validated.**

6. **Session-based loot layout + timestamps + manifest** — Loot / Medium / M
   Scattered flat files, no dates on PMKID/passwords/LAN, no per-engagement grouping. Add
   `/ext/loot/<date-time>/`, timestamp the untimed outputs, write a target→capture index.

7. ~~**Randomize the BLE identity**~~ ✅ DONE (`832891d`, `30a6dbb`, `ddff3ab`) — OPSEC / Medium / S
   Added an opt-in **BLE Stealth** lock-menu toggle: when ON, `furi_hal_version_init` advertises a random
   static MAC (`esp_random`) + generic name "BLE Device" instead of the factory MAC + "mob"; default OFF
   keeps the "mob" identity + bonding. Setting persists in **NVS** (`opsec/ble_stealth`) — the initial
   RTC-flag approach was caught in final review as RAM-only in this port (would silently no-op after the
   apply-reboot) and corrected to NVS. Applies on next boot. **Not yet hardware-validated.**

## P2 — Polish & reach

8. **Gate verbose serial logging & kill "Flipper" tells** — OPSEC / Low / S
   Victim MACs (`wlan_evil_portal.c:106`), target BSSIDs (`scene_handshake.c:200`), netcut MACs at
   INFO; beacon fallback SSID literal "Flipper" (`wlan_hal.c:650`). Gate behind a debug flag.

9. **TX-power control & flood throttle** — OPSEC / Low / S
   Everything TXes at ~19.5 dBm (no `esp_wifi_set_max_tx_power`); Karma is a continuous flood
   (`wlan_evil_portal.c:1018`). Add a low-power setting + Karma cadence throttle.

10. **Workflow: saved attack presets + recent targets** — Workflow / Low / M
    Most attacks re-select target + config each run. Generalize the NRF24 jammer's per-source
    persistence into saved presets + a recent-targets list.

11. **Capability reach** — Features / Low / L
    Genuine gaps: WPS/Pixie-Dust (most notable missing WiFi attack), PMKID multi-AP sweep, a deauth
    detector (defensive), the queued BLE radar. Standard-Cardputer keyboard is dead (6 keys) — only
    relevant if targeting that board; the ADV is fine.

---

## Already solid — don't "fix"

- Deauth frames spoof the target's BSSID (not your MAC) — attribution-safe.
- Beacon spam & BLE spam randomize MACs; Karma raw beacons use stable LAA BSSIDs.
- Captured-cred serial logging is compiled out by default (`WLAN_CRED_DEBUG_POST 0`).
- No attack auto-transmits on scene entry (deauth needs a keypress; only passive capture auto-starts).
- Loot formats are tool-ready: hashcat `-m 22000`, PCAP, Wigle CSV.
