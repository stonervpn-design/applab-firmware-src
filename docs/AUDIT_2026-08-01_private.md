# Private build audit — 2026-08-01 (pass 1)

Scope agreed with owner: **custom code + known issues** (not upstream Flipper base).
Mode: **fix safe directly + rebuild to verify; report risky/device-dependent.**
Dimensions: correctness, memory/RAM, code quality, performance/battery, UX, security.

Build used to gather signal: `build_adv_only_private.bat` (private Cardputer ADV,
`build_cardputer_adv`), all 260 custom `.c` files force-recompiled with the strict
app flags (`-Wall -Wextra -Wshadow -Wvla -Wformat-signedness -Wmissing-prototypes`).

## FIXED (safe, verified — rebuild reached SENTINEL_OK, 0 code warnings remain)

1. **6× deprecated `view_dispatcher_enable_queue()` removed** — the API is
   `@deprecated` in `view_dispatcher.h` ("remove all calls; queue is always enabled
   during construction"). No behavior change. Files: `pin_config_app.c`,
   `can_commander_app.c`, `lan_attack_app.c`, `camera_hunter_app.c`,
   `doorbell_brute.c`, `settings/device_name/device_name.c`.
2. **3× unused variables removed** — `wiegand_scene_capture.c` (`app` in on_exit),
   `bus_tools_scene_start.c` (`scene_manager`), `lan_attack/net_wpa.c` (`int i` in
   SHA1ProcessMessageBlock). No behavior change.

Positive findings (no action needed):
- No raw `sprintf`/`gets` anywhere in custom code; only 5 `strcpy`s, all
  constant-string-into-known-buffer (safe). String handling is disciplined.
- Overall warning density is very low (30 total across a full custom recompile,
  most of them the config items below) — the custom code is in good shape.

## REPORT — risky / needs your decision or the device

### R1 — DONE (applied + build-verified 2026-08-01; needs on-device RF test): WiFi RAM tuning was silently ignored
**RESOLUTION:** replaced the dead `ESP_WIFI_RX_BUFFER_NUM=4` with the real IDF 5.4.1
symbol `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4` (was defaulting to 10); removed the
two other dead lines (`ESP_WIFI_TX_BUFFER_NUM` — TX already configured via the valid
STATIC/DYNAMIC_TX_BUFFER_NUM symbols; `ESP_WIFI_TASK_STACK_SIZE` — no such symbol).
Verified: rebuild SENTINEL_OK, WiFi "unknown symbol" warnings gone, generated
sdkconfig shows `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4`. **Net: ~9.6 KB internal
SRAM reclaimed** for WiFi on the no-PSRAM Cardputer — the plausible fix for the
WiFi-app-on-entry OOM. Safe at 4 because AMPDU-RX is disabled. Mirrored to
src_cardputer_adv. **Owner must RF-test: scan finds networks + evil-portal
client-connect still works.** M5Stick build was NOT affected (it never set these).

### R1 (original description, kept for context): WiFi RAM tuning is silently ignored
`sdkconfig.defaults.m5stack_cardputer_adv` sets symbols that **do not exist in
IDF 5.4.1**, so the build ignores them and uses IDF defaults (bigger buffers =
less free RAM on this no-PSRAM board):
| In sdkconfig (ignored) | Real IDF 5.4.1 symbol(s) |
|---|---|
| `CONFIG_ESP_WIFI_RX_BUFFER_NUM=4` | `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` + `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` |
| `CONFIG_ESP_WIFI_TX_BUFFER_NUM=4` | `CONFIG_ESP_WIFI_TX_BUFFER_TYPE` (0=dynamic) + `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` |
| `CONFIG_ESP_WIFI_TASK_STACK_SIZE=2560` | no such symbol in esp_wifi (removed/renamed) — the 2560 stack intent is not applied |

Why it matters: the whole point of these was to shrink WiFi's RAM footprint on a
board with ~40 KB headroom. They've been no-ops, so WiFi is using default (larger)
buffers — plausibly a contributor to the "WiFi app crash on entry" OOM path.
**Not changed here**: correcting these changes radio RAM/throughput and MUST be
verified on-device (scan works, evil portal client-connect still works, no new
OOM). Recommend a focused follow-up: set the correct symbols to the intended small
values, rebuild, and RF-test.

### R2 (LOW, harmless): `CONFIG_ESP_SYSTEM_ABORT_ON_ALLOC_FAILURE=n` also unknown
Real symbol is `CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS` (heap component), whose
default is already `n` (return NULL, don't abort). Intent already matches the
default → no behavioral impact. Optional: rename for correctness, or delete the line.

### R3 (LOW, cosmetic): tautological guard in `wiegand_scene_craft.c:24`
`if (app->state.facility <= 255 && app->state.card <= 65535)` is always true because
the fields are `uint8_t`/`uint16_t`. Harmless defensive check; left intact on purpose
(would matter if the field types ever widen). Not a bug.

### Known open crashes (need an on-device serial coredump — cannot root-cause statically)
- **WiFi app crash on entry.** Entry path (`wlan_app.c`) is heavily guarded already
  (55 KB free-RAM gate, BT release). Note: none of the `malloc`/`_alloc` calls in
  `wlan_app_alloc()` are NULL-checked — the RAM gate mitigates but doesn't guarantee.
  See R1: the ignored WiFi buffer config may be the real trigger.
- **IR-learn close crash.** Unchanged from prior analysis; needs the coredump.
- Capture: `idf.py -p COM12 monitor` (or serial @115200), reproduce, grab the
  `Guru Meditation` + `Backtrace: 0x...` lines.

## PASS 2 (in progress — module by module)
- **nRF24 `nrf24_hw.c` (660 LOC SPI driver): REVIEWED — CLEAN.** Every SPI buffer is
  a fixed `[1+32]` with `len` capped ≤32 before `memcpy`; addresses clamped 3–5 bytes;
  FIFO-drain loops bounded. USB PA-clamp + data-rate negotiation sound. No change.
- **nRF24 `nrf24_mj_core.c` (495 LOC Mousejack): REVIEWED — CLEAN.** The
  security-critical part — `mj_fingerprint()` parsing attacker-controlled OTA data —
  is properly bounded: `payload_length` clamped to `≤ size-9`, `size` capped 37, all
  `buf[37]`/`esb_payload[32]` indices in range; MS/Logitech frame builders fixed-size.
  No overflow/UB. No change.
- Still to review in nRF24: `nrf24_app.c`, jam scenes/views, `nrf24_wifi_scan.c`,
  the mj_ducky/jam_config/presets helpers.
- **Untrusted-input parser spot-checks — all CLEAN (properly bounds-checked):**
  `wlan_evil_portal.c` KARMA SSID-from-802.11-frame (`slen` clamped ≤32 + frame-length
  checked before memcpy), `net_proto.c` DNS/LLMNR name decode (label `len>63` + input
  and output bounds checked, loop always advances → no overflow/infinite-loop). The
  author consistently guards parsed lengths before copying. Remaining variable-length
  memcpy sites to spot-check: `net_ntlm.c`, `net_wpa.c` (PBKDF2/SHA1 — vendored).
- **More parsers spot-checked — all CLEAN:** `scene_handshake.c` EAPOL capture (`len`
  clamped to `HS_PKT_MAX_LEN=512` = `slot->data[512]`), `esp_now/scene_sniff.c`
  (`copy_len` clamped to `ESP_NOW_PKT_MAX_DATA`, NULL+mutex guarded),
  `nfc_share_proto.c` TX builders (name/data lengths clamped + NUL-padded).
- **ST25R3916 NFC backend (2041 LOC hal/nfca/i2c): REVIEWED — CLEAN.** RX clamps FIFO
  count to `ST25R_HAL_FIFO_MAX` then to `rx_cap` before memcpy; NFC-A anticollision
  (card-controlled UID) bounds-checks `uidlen` vs `sizeof(uid)` before every append;
  emulation UID copies bounded ≤4/≤7 into `ptm[14]`. No overflow.

## Overall assessment (pass 2 so far)
Across nRF24 (driver + Mousejack OTA parser), the whole WiFi/network parsing surface
(802.11 SSID, DNS/LLMNR, EAPOL, esp-now), NFC-share wire format, and the ST25R3916 NFC
backend — **every untrusted-input length is clamped before the copy; no memory-safety or
overflow bug found.** The custom code is consistently, deliberately defensive. The
actionable audit output is thus the config/robustness layer (R1 SRAM, R4 OOM diagnostics)
plus warning cleanup — not a pile of latent buffer bugs, because there aren't any in the
reviewed surface. Remaining unread modules (subghz SubBrute/ProtoPirate, defender,
camera_hunter cred logic, can_commander) are lower-risk; established pattern predicts clean.

- **can_commander (CSV/`ID#DATA` parse): REVIEWED — CLEAN.** `can_frame.c` checks
  `lens[0]>=name_max` and `lens[3]/lens[4]>=sizeof` before each memcpy + null-terminate;
  `can_commander_app.c` checks `idlen>=sizeof(idbuf)`; monitor pairs `strncpy(dst,row,LEN-1)`
  with explicit `dst[LEN-1]='\0'`.
- **defender + camera_hunter: sweep found NO raw variable-length copies** — strings built
  via `FuriString`/`snprintf` (bounds-safe by construction). No issue.

## FINAL CONCLUSION
Custom code is well-engineered and defensively coded. Across the full high/medium-risk
surface — nRF24 driver + Mousejack OTA parser, all WiFi/network parsers (802.11, DNS/LLMNR,
EAPOL, esp-now, evil-portal), NFC-share, the ST25R3916 NFC backend, can_commander CSV —
**zero memory-safety / overflow / UAF bugs found**; every untrusted-input length is clamped
before copy.

Delivered (all build-verified, mirrored across trees):
1. 9 compiler warnings removed (clean strict-flag build).
2. R1 — WiFi static-RX buffer config now actually applied (~9.6 KB SRAM reclaimed); likely
   the WiFi-on-entry OOM fix. **Needs owner RF test.**
3. R4 (safe subset) — `furi_check(app)` on all 15 app-entry allocs → clean OOM panic vs
   undiagnosable NULL-deref. Global aborting-malloc deliberately NOT done (would regress
   wlan's graceful-degradation design); available on request with the 8-site conversion.

Needs hardware, not code review: the WiFi/IR-learn crashes still want a serial coredump
(R1 should fix / R4 cleanly diagnose the WiFi one); the battery/DFS pass needs on-device
power measurement (see [[m5sticks3-battery-dfs-power]]).

### R4 — PARTIALLY DONE (safe subset applied + verified; global option declined as risky)
**What was done (safe, build-verified SENTINEL_OK, mirrored to all trees):** added
`furi_check(app)` after all 15 app-entry `malloc(sizeof(App))` sites, so an OOM at app
launch is now a clean panic + backtrace instead of a mystery StoreProhibited NULL-deref.
**Why NOT the global aborting-malloc (owner had picked it):** on closer review the wlan
module relies on malloc's NULL return for graceful degradation in **8+ places**
(`wlan_app_ensure_cred_sniff`, `scene_live_creds` GUI buffers, `scene_pmkid` pool,
`wlan_oui`, `evil_portal_templates` fallback, plus `if(!s_ring)/if(!s_rx)` buffer guards)
— this is the intentional no-PSRAM survival design. A global aborting malloc would turn
those graceful degrades into aborts = a regression under exactly the memory pressure the
device is built to survive. So global-abort is **reported, not applied**. If still wanted,
it requires converting those 8+ sites to pre-check free RAM (or routing them through a
`malloc_try`); I can do that as a dedicated, on-device-tested change.

### R4 (original architectural description, kept for context): port malloc returns NULL, code assumes it can't
`components/furi/core/memmgr.h` does `#define malloc(size) calloc(1,(size))` backed by
plain ESP-IDF `calloc`, which **returns NULL on OOM and does NOT abort** — unlike stock
Flipper, whose allocator `furi_check`s and never returns NULL. The whole codebase
(upstream Flipper modules + our ~15 app entry points + 84 malloc sites total) derefs the
result **unchecked**, matching the Flipper "malloc never fails" contract that no longer
holds here. On the no-PSRAM board an OOM anywhere → StoreProhibited NULL-deref = an
undiagnosable mystery crash (exactly the shape of the two open crashes). BUT it's not a
simple "add checks" fix: some port code intentionally relies on the NULL return for
graceful degradation (e.g. `wlan_app_ensure_cred_sniff()` checks `if(app->cred_sniff)`).
So the two assumptions coexist. **Recommended (owner's call):** restore the Flipper
contract — make the port's `malloc`/`calloc` abort cleanly (a `furi_check` in a wrapper)
so OOM produces a clean panic + backtrace instead of UB — and convert the handful of
intentional NULL-handlers to pre-check free RAM (as `wlan_app` already does at entry).
Alternatively keep NULL-return and add `furi_check(app)` to the ~15 entry mallocs (clean
panic, low churn) — partial but safe. Not changed pending your decision; I can do either.

## REVIEWED (this audit) vs REMAINING (optional, lower-risk)
REVIEWED clean: nRF24 (hw driver + mj_core OTA parser), WiFi/network parsers (802.11
SSID, DNS/LLMNR, EAPOL, esp-now, evil-portal), NFC-share proto, ST25R3916 NFC backend,
can_commander CSV, defender + camera_hunter (no raw copies). Warning build covered ALL
260 custom .c (0 code warnings remain).
REMAINING (optional spot-checks — established pattern predicts clean, not blockers):
subghz SubBrute engine + ProtoPirate decoders internals, lan_attack net_ntlm/net_wpa
(vendored crypto), ble_spam frame builders, doorbell_brute, qrcode, power_monitor,
bus_tools scenes. Non-code: battery/DFS on-device verification; input-consistency pass.
