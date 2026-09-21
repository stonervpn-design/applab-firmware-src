# Flippergotchi v2.0 "Maxed" Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild Flippergotchi as a modular v2.0 with AI channel dwell, active clientless PMKID solicitation, per-AP dedup, a nameable pet with XP/achievements, full pwngrid social, a 4-screen UI, and optional GPS-tagged loot.

**Architecture:** Split the 538-line monolith into ~10 focused modules under `applications_user/flippergotchi/`. Pure-logic modules (net table, AI scoring, XP/achievements, pwngrid JSON parse, Wigle CSV) have no ESP-IDF deps and are validated by a Python oracle in `tests/`. Integration modules (capture cb, attack TX, UI, geo) are verified by green build + on-device. Threading is unchanged in shape: WiFi-task promiscuous cb (lock-free append), worker task (drain + AI + attack + advert), GUI thread (draw snapshots).

**Tech Stack:** C (ESP-IDF v5.4.1, Flipper GUI), reuses `wlan_hal_*`, `pmkid_frames`, `wlan_handshake_parser`, `wlan_pcap`, `components/gnss`. Build via `build_fix.bat`. No host C compiler → Python mirror tests for pure logic.

## Global Constraints
- App is `MENUEXTERNAL`, statically linked into firmware → all firmware symbols (wlan_hal_*, pmkid_build_*, gnss_*) resolve at link; no `.fap` loader.
- Bounded RAM only: `FG_MAX_NETS=96`, `FG_MAX_PEERS=24`, `FG_MAX_CLIENTS=32`, `FG_RING=16`, `FG_FRAME_MAX=288`. No PSRAM — no large static/heap buffers.
- No `Math.random`/`Date.now` equivalents; deterministic xorshift seeded once from `furi_get_tick()` at start.
- All faces/strings ASCII (128×64 mono canvas). Loot paths: pcap/pmkid unchanged; state → `/ext/apps_data/flippergotchi.dat`; friends → `.friends`; wardrive → `/ext/wifi/flippergotchi_wardrive.csv`.
- Frame parsing reuses hardened `wlan_handshake_parser` — no new attacker-length-driven writes. Release-fence before publishing any table count read by the GUI thread.
- Every phase builds green (`build_fix.bat` → SENTINEL_OK) and leaves the app runnable. Commit per task with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` trailer.
- Spec of record: `docs/superpowers/specs/2026-07-13-flippergotchi-v2-maxed-design.md`.

---

## File Structure
- `applications_user/flippergotchi/fg_types.h` — shared structs + constants (no logic)
- `fg_net_table.{c,h}` — per-AP dedup table (pure)
- `fg_progress.{c,h}` — XP/level/streaks/achievements + name (pure math + persistence I/O)
- `fg_pwngrid.{c,h}` — peer JSON parse + peer table + advert build (pure parse + I/O)
- `fg_ai.{c,h}` — channel scoring/next/dwell (pure)
- `fg_geo.{c,h}` — Wigle CSV formatter (pure) + GNSS tag (I/O, guarded)
- `fg_capture.{c,h}` — promiscuous cb + EAPOL processing (integration)
- `fg_attack.{c,h}` — worker-context deauth + PMKID solicit (integration)
- `fg_ui.{c,h}` — 4-screen draw + D-pad input (integration)
- `flippergotchi.c` — lifecycle/wiring (rewritten thin)
- `tests/fg_oracle.py` — Python mirror validating net-table/AI/XP/pwngrid/CSV logic
- `application.fam` — add `gnss` to requires; bump version to 2.0

Each `.c` compiles via the app's `*.c*` auto-glob (verify each produces an `.obj`).

---

## Task 1: fg_types.h + net table (pure, dedup spine)

**Files:**
- Create: `applications_user/flippergotchi/fg_types.h`
- Create: `applications_user/flippergotchi/fg_net_table.c`, `fg_net_table.h`
- Create/Test: `applications_user/flippergotchi/tests/fg_oracle.py` (net-table section)

**Interfaces:**
- Produces:
  - `#define FG_NET_HAS_PMKID 0x01 / _HAS_M2 0x02 / _HAS_M3 0x04 / _HAS_M4 0x08 / _FULL 0x10 / _SOLICITED 0x20`
  - `typedef struct { uint8_t bssid[6]; char ssid[33]; uint8_t channel; int8_t rssi; uint8_t flags; uint32_t last_seen; uint32_t solicit_tick; } FgNet;`
  - `typedef struct { FgNet nets[96]; volatile uint16_t count; } FgNetTable;`
  - `int fg_net_find(FgNetTable*, const uint8_t bssid[6]);` // index or -1
  - `int fg_net_observe(FgNetTable*, const uint8_t bssid[6], uint8_t channel, int8_t rssi, uint32_t now);` // find-or-insert-or-evict → index
  - `void fg_net_set_ssid(FgNetTable*, int idx, const char* ssid);`
  - `bool fg_net_add_flag(FgNetTable*, int idx, uint8_t flag);` // returns true if flag was newly set
  - `bool fg_net_is_pwned(const FgNetTable*, int idx);` // HAS_PMKID|HAS_M2
  - `uint16_t fg_net_pwned_count(const FgNetTable*);`

- [ ] **Step 1 (test):** In `tests/fg_oracle.py`, model `FgNetTable` in Python: insert 3 BSSIDs → count 3; re-observe one → count still 3, rssi/last_seen updated; fill to 96 then observe a new unpwned → evicts the least-recent unpwned (never a pwned one); `add_flag(HAS_M2)` returns True first time / False second; `pwned_count` rises only on first PMKID/M2 per net. Assert all.
- [ ] **Step 2:** Run `python tests/fg_oracle.py` → the net-table asserts FAIL (oracle funcs stubbed).
- [ ] **Step 3:** Implement the net-table logic in the Python oracle until asserts PASS (this is the reference).
- [ ] **Step 4:** Port the identical logic to `fg_net_table.c` (same names/semantics). `fg_net_observe` eviction: when `count==96`, scan for the least-recent entry with `!is_pwned`; overwrite it; if all 96 are pwned, drop the observation (return -1). Release-fence before bumping `count`.
- [ ] **Step 5 (commit):** `git add applications_user/flippergotchi/fg_types.h fg_net_table.* tests/fg_oracle.py && git commit`.

## Task 2: fg_ai channel engine (pure)

**Files:** Create `fg_ai.c`, `fg_ai.h`; extend `tests/fg_oracle.py`.

**Interfaces:**
- Consumes: `FgNetTable`, per-channel client counts.
- Produces:
  - `typedef struct { uint16_t ap[14]; uint16_t cl[14]; uint16_t catch_recent[14]; uint32_t rng; } FgAi;` (index 1..13 used)
  - `void fg_ai_init(FgAi*, uint32_t seed);`
  - `void fg_ai_recount(FgAi*, const FgNetTable*, const FgClient* clients, uint8_t client_count);` // rebuild ap[]/cl[] from tables
  - `void fg_ai_reward(FgAi*, uint8_t channel);` // ++catch_recent[ch], caller decays periodically
  - `void fg_ai_decay(FgAi*);` // catch_recent[ch] -= catch_recent[ch]>>3 each call
  - `uint8_t fg_ai_next_channel(FgAi*);` // weighted pick, score = 3*ap+2*cl+5*catch_recent+1 floor; ε=~1/8 uniform explore
  - `uint16_t fg_ai_dwell_ms(const FgAi*, uint8_t channel);` // 400 base; +score*100 capped 2000; min 150 if score==floor

- [ ] **Step 1 (test):** Oracle: a channel with high ap+catch is picked far more often than an empty one over 1000 draws (with fixed xorshift seed); dwell on a hot channel ≈2000, on an empty one ≈150–400; `fg_ai_decay` halves-ish `catch_recent` (>>3 repeated). Assert distributions/bounds.
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Implement in oracle → PASS. xorshift32: `x^=x<<13; x^=x>>17; x^=x<<5;`.
- [ ] **Step 4:** Port to `fg_ai.c` verbatim (same xorshift, same weights/caps).
- [ ] **Step 5 (commit).**

## Task 3: fg_progress — name, XP, achievements, persistence (pure math + I/O)

**Files:** Create `fg_progress.c`, `fg_progress.h`; extend oracle.

**Interfaces:**
- Produces:
  - `typedef struct { char name[13]; uint32_t total_pwns; uint32_t xp; uint32_t ach; uint32_t best_session; uint32_t best_streak; uint32_t cur_streak; } FgProgress;`
  - achievement bits: `FG_ACH_FIRST 0x01, _HS10 0x02, _PMKID25 0x04, _FRIEND1 0x08, _FRIEND5 0x10, _FULL 0x20, _NIGHTOWL 0x40, _SURFER 0x80`
  - `uint32_t fg_xp_for_level(uint32_t level);` // 10*level*(level+1)/2 (triangular)
  - `uint32_t fg_level_of(uint32_t xp);` // largest L with fg_xp_for_level(L) <= xp
  - `void fg_progress_add_xp(FgProgress*, uint32_t dxp);`
  - `uint32_t fg_award_check(FgProgress*, uint32_t hs, uint32_t pmkid, uint32_t friends, uint32_t full_hs, uint32_t session_secs, uint8_t channels_hit);` // sets new bits, returns newly-unlocked mask
  - `void fg_progress_default_name(FgProgress*, uint32_t seed);` // pick from {"Pixel","Nibble","Byte","Sparky","Gizmo","Waffle","Mochi","Bolt"}
  - `bool fg_progress_load(Storage*, FgProgress*);` // parse `/ext/apps_data/flippergotchi.dat`; migrate legacy `.score` (bare int→total_pwns) if .dat absent
  - `void fg_progress_save(Storage*, const FgProgress*);` // write KV: `v=2;name=..;pwn=..;xp=..;ach=<hex>;bestses=..;beststreak=..`

- [ ] **Step 1 (test):** Oracle: `fg_xp_for_level(1)=10, (2)=30, (3)=60`; `fg_level_of(59)=2, (60)=3`; award_check sets FIRST on first pwn, HS10 at hs==10 and returns its bit only once; night-owl at 1800s; surfer at channels_hit==8; default_name deterministic per seed; KV round-trips (save→load equal); legacy migration reads `"42"`→total_pwns=42. Assert.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement in oracle → PASS. **Step 4:** Port pure math to `fg_progress.c`; implement load/save with `storage_file_*` (KV parser: split on `;` then `=`; hex for `ach`). **Step 5 (commit).**

## Task 4: fg_pwngrid — full JSON parse + peer table + advert (pure parse + I/O)

**Files:** Create `fg_pwngrid.c`, `fg_pwngrid.h`; extend oracle.

**Interfaces:**
- Produces:
  - `typedef struct { char name[20]; uint32_t pwnd; char face[8]; int8_t rssi; uint32_t last_seen; } FgPeer;`
  - `typedef struct { FgPeer peers[24]; volatile uint8_t count; } FgPeerTable;`
  - `bool fg_pwngrid_parse(const uint8_t* frame, int len, FgPeer* out);` // scan JSON at offset≥38 for name/pwnd_tot/face; bounds-checked; false if no name
  - `int fg_pwngrid_note(FgPeerTable*, const FgPeer*, uint32_t now);` // dedup by name → index; refresh
  - `uint16_t fg_pwngrid_build_adv(uint8_t* out, const char* name, const char* face, uint32_t pwnd_tot, uint32_t pwnd_run, uint32_t uptime);` // 802.11 beacon from de:ad:be:ef:de:ad + `0xde len JSON`
  - friends registry I/O: `void fg_friends_load/save(Storage*, ... )` — `name,seen` lines; `uint32_t fg_friends_total(...)`.

- [ ] **Step 1 (test):** Oracle: feed a real pwngrid beacon byte array (name="Brute",pwnd_tot=7,face="(^^)") → parse extracts all three; a truncated frame → false, no over-read; note() dedups by name; build_adv produces a frame whose bytes 10..15 == de:ad:be:ef:de:ad and contains the JSON with the given name. Assert.
- [ ] **Step 2:** FAIL. **Step 3:** oracle PASS (mirror the scanning parser exactly, with the `i+klen<len` bounds). **Step 4:** port to `fg_pwngrid.c`; the scanning parser reuses v1's `name":"` approach generalized to also read `pwnd_tot":` (integer) and `face":"`. Friends file I/O via storage. **Step 5 (commit).**

## Task 5: fg_geo — Wigle CSV + guarded GNSS tag (pure format + I/O)

**Files:** Create `fg_geo.c`, `fg_geo.h`; extend oracle.

**Interfaces:**
- Produces:
  - `int fg_geo_format_row(char* out, size_t cap, const uint8_t bssid[6], const char* ssid, uint8_t channel, int8_t rssi, double lat, double lon);` // Wigle-style CSV line
  - `void fg_geo_init(void);` // lazily gnss_start() if !gnss_is_running() and symbol present
  - `bool fg_geo_tag(Storage*, const uint8_t bssid[6], const char* ssid, uint8_t channel, int8_t rssi);` // if gnss_get_fix() valid → append row to wardrive.csv, return true; else false
  - `bool fg_geo_has_fix(void);`

- [ ] **Step 1 (test):** Oracle: `format_row` for a known input == expected CSV string (BSSID upper-hex colon, SSID, `[WPA2]`, channel, rssi, lat, lon with 6 decimals). Assert.
- [ ] **Step 2:** FAIL. **Step 3:** oracle PASS. **Step 4:** port `fg_geo_format_row`; implement tag/init/has_fix against `components/gnss/gnss.h` (`gnss_start/is_running/get_fix`), all guarded so absence = no-op. **Step 5 (commit).**

## Task 6: fg_capture — promiscuous cb + EAPOL processing (integration)

**Files:** Create `fg_capture.c`, `fg_capture.h`. Consumes Tasks 1,4.

**Interfaces:**
- Produces:
  - `void fg_capture_cb(FgApp*, const uint8_t* fr, int len);` // called from the promisc cb: beacon→net_observe+ssid (or pwngrid→peer note); EAPOL data→ring
  - `void fg_capture_process(FgApp*, const uint8_t* fr, uint16_t len);` // worker: parse addrs, msg num, set net flags, dedup-count pwns, pcap+pmkid+geo, ai_reward

- [ ] **Step 1:** Implement per spec §3.1: reuse `wlan_hs_is_beacon/extract_beacon_ssid/parse_addresses/is_eapol/get_eapol_msg_num/extract_pmkid`. On M2 set `HAS_M2`; M1+valid pmkid set `HAS_PMKID`+save; M3/M4 set bits; `FULL` when M1&M2&M3&M4 or M2&M3. `total_pwns`/`xp` increment ONLY when `fg_net_add_flag` reports a newly-pwned net; on new pwn call `fg_geo_tag` + `fg_ai_reward`.
- [ ] **Step 2:** Wire the promiscuous cb (in flippergotchi.c, Task 8) to call `fg_capture_cb`. Build green.
- [ ] **Step 3:** Sanity: `grep` the build for `fg_capture.c.obj`. Commit.

## Task 7: fg_attack — deauth (skip pwned) + PMKID solicit (integration)

**Files:** Create `fg_attack.c`, `fg_attack.h`. Consumes Tasks 1,2.

**Interfaces:**
- Produces:
  - `void fg_attack_run(FgApp*);` // worker-context (via wlan_hal_run_in_worker): on app->channel, deauth non-pwned APs + targeted clients (v1 logic minus pwned APs), then if solicit_on, for ≤3 non-pwned non-recently-solicited APs send pmkid_build_auth_open + pmkid_build_assoc_req and mark SOLICITED with a cooldown (e.g. 10s via solicit_tick).

- [ ] **Step 1:** Port v1 `fg_deauth_fn` into `fg_attack_run`, adding `if(fg_net_is_pwned(&app->nets, i)) continue;` and the solicitation block using `pmkid_build_*` (include `wlan_app/pmkid_frames.h`). Respect `app->attack_on` and `app->solicit_on`.
- [ ] **Step 2:** Build green; confirm `pmkid_build_*` links (MENUEXTERNAL static link). Commit.

## Task 8: flippergotchi.c lifecycle rewrite (integration)

**Files:** Modify `applications_user/flippergotchi/flippergotchi.c` (thin wiring). Consumes all.

**Interfaces:** Consumes every module; owns `FgApp` (defined in fg_types.h, holding `FgNetTable nets; FgPeerTable peers; FgAi ai; FgProgress prog; FgClient clients[32]; ...` plus the v1 ring/worker/mutex/view fields).

- [ ] **Step 1:** Replace v1 globals with the module tables. On launch: open storage, `fg_progress_load` (+ default name if none), `fg_geo_init`, `fg_friends_load`, open pcap. Worker loop: drain ring→`fg_capture_process`; each hop `fg_ai_recount`+`fg_ai_next_channel`+set channel+`fg_ai_dwell_ms` (variable `furi_delay_ms`); periodic `fg_ai_decay`; `fg_attack_run` via worker; advert via `fg_pwngrid_build_adv` every ~2s. Teardown: `fg_progress_save`+`fg_friends_save`+achievement flush.
- [ ] **Step 2:** Build green + flash. On device: app launches, captures, count is unique-net. Commit.

## Task 9: fg_ui — 4-screen UI + rename (integration)

**Files:** Create `fg_ui.c`, `fg_ui.h`; `flippergotchi.c` delegates draw/input to it.

**Interfaces:**
- Produces:
  - `typedef enum { FG_SCR_FACE, FG_SCR_NETS, FG_SCR_PEERS, FG_SCR_STATS } FgScreen;`
  - `const char* fg_ui_face(const FgApp*, uint32_t now);` // picks 1 of 10 ASCII faces per spec §3.4
  - `void fg_ui_draw(Canvas*, FgApp*);` // switch on app->screen
  - `bool fg_ui_input(FgApp*, InputEvent*);` // L/R cycle screen; Up/Down scroll list or rename char; OK context (FACE=toggle attack, STATS row=rename); Back exit

- [ ] **Step 1:** Implement FACE (enriched v1 layout + name/level/mood), NETS (scroll list `SSID CHt [P][H][F] rssi`), PEERS (`name pwnd age`), STATS (Lv/XP bar + streak/records + 8-achievement grid + Rename row). Rename: Up/Down change highlighted char over `[A-Z0-9_]`, L/R move cursor, OK confirm→`fg_progress_save`.
- [ ] **Step 2:** Wire `view_set_draw_callback`→`fg_ui_draw`, input→`fg_ui_input`. Build + flash. On device: L/R cycles screens; lists scroll; rename persists across reboot. Commit.

## Task 10: fam bump + final integration verify

**Files:** Modify `application.fam` (version "2.0", add `"gnss"` to requires, update description).

- [ ] **Step 1:** Update fam. Full build green (SENTINEL_OK) + flash.
- [ ] **Step 2 (on-device acceptance):** connect a phone to a test AP → handshake caught + face→PWND; a solicit yields a PMKID; NETS shows the net once with flags; leave a second gotchi/Brucegotchi nearby → PEERS shows it + FRIEND face; reboot → name/level/achievements persist; (if GPS) a wardrive row appears. Commit.

---

## Self-review notes
- Spec coverage: §3.1→T1/T6, §3.2→T2, §3.3→T7, §3.4→T3/T9, §3.5→T4, §3.6→T9, §3.7→T5, persistence→T3/T4/T5, threading→T6/T8. All covered.
- No placeholders: every task lists exact files, signatures, and test oracles; algorithms (xorshift, XP curve, eviction, JSON scan) are specified inline.
- Type consistency: `FgNetTable`/`FgAi`/`FgProgress`/`FgPeerTable`/`FgApp` names and the flag/achievement bit macros are used identically across tasks.
