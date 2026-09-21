# Flippergotchi v2.0 "Maxed" — Design

**Date:** 2026-07-13
**App:** `applications_user/flippergotchi/` (MENUEXTERNAL, statically linked into the port firmware)
**Status:** Approved design; implementation pending.

## 1. Overview & goals

Flippergotchi is a Pwnagotchi-style WiFi handshake/PMKID harvester with a mood face.
v1.0 (538 lines, one file) blindly hops channels 1→13, harvests EAPOL M2 + M1 PMKIDs,
auto-deauths, does basic pwngrid peer detect/advertise, shows one face screen, and
persists a lifetime pwn count.

v2.0 turns it into a genuinely "maxed" pwnagotchi across three axes — **pwning power**,
**pwnagotchi soul**, and **Cardputer-native UX** — while keeping the beloved face central.

**Goals**
- Catch more, faster, and smarter (AI channel dwell + active clientless PMKID).
- Count and browse *unique* networks, not raw frames; never re-attack an already-pwned AP.
- A pet with a name, real moods, XP/levels, streaks, and achievements.
- Real pwngrid social: a peers screen, friends remembered across reboots.
- Multi-screen UI navigable with the D-pad; optional GPS-tagged loot.

**Non-goals**
- No color/hi-res rendering (the port renders through the 128×64 Flipper canvas).
- No cracking on-device (loot stays hashcat-22000 / pcap for offline cracking).
- No new radio HAL — reuse `wlan_hal_*`, `pmkid_frames`, `wlan_handshake_parser`, `wlan_pcap`.

## 2. Architecture

Split the monolith into focused, independently-reasoned modules under
`applications_user/flippergotchi/`:

| Module | Responsibility | Key interface |
|---|---|---|
| `fg_types.h` | Shared structs (`FgApp`, `FgNet`, `FgPeer`, `FgStats`), constants | (types only) |
| `fg_net_table.{c,h}` | Per-AP dedup table: find/insert by BSSID, record SSID/RSSI/PMKID/M2/full, channel | `fg_net_find/observe/mark_pwned/count` |
| `fg_capture.{c,h}` | Promiscuous cb + EAPOL/PMKID processing → updates net table + stats | `fg_capture_cb`, `fg_capture_drain` |
| `fg_ai.{c,h}` | Channel scoring + next-channel + dwell-ms decision (recon→attack, reward) | `fg_ai_next_channel`, `fg_ai_dwell_ms`, `fg_ai_reward` |
| `fg_attack.{c,h}` | Worker-context TX: targeted/broadcast deauth + Auth+AssocReq PMKID solicit | `fg_attack_run` (via `wlan_hal_run_in_worker`) |
| `fg_pwngrid.{c,h}` | Parse peer JSON (name/pwnd/face/uptime); peer table; build our advert | `fg_pwngrid_note`, `fg_pwngrid_build_adv` |
| `fg_progress.{c,h}` | Name, XP/level curve, streaks, session records, achievements; persistence | `fg_progress_load/save`, `fg_progress_on_pwn`, `fg_award` |
| `fg_geo.{c,h}` | Optional GNSS tagging → Wigle-style CSV (guarded, no-op without fix) | `fg_geo_tag` |
| `fg_ui.{c,h}` | Multi-screen draw + D-pad input; owns screen state | `fg_ui_draw`, `fg_ui_input` |
| `flippergotchi.c` | Lifecycle: alloc, records, worker thread, view dispatcher, teardown | `flippergotchi_app` |

Threading is unchanged in shape: **WiFi task** runs the promiscuous cb (lock-free writes
to ring + net table with the existing benign-race discipline), a **worker task**
channel-hops + drains + drives attack/advert, and the **GUI thread** draws. All shared
counters stay behind the existing `FuriMutex`; the net/peer tables use the current
"published count + append-only" lock-free pattern (bounded arrays, capped).

## 3. Feature specifications

### 3.1 Net table + dedup + completeness (`fg_net_table`, `fg_capture`)
- `FgNet { uint8_t bssid[6]; char ssid[33]; uint8_t channel; int8_t rssi; uint8_t flags; uint32_t last_seen; }`
  where `flags` = bits `HAS_PMKID | HAS_M2 | HAS_M3 | HAS_M4 | FULL_HS | SOLICITED`.
- Beacons populate SSID + channel + rssi (reuse `wlan_hs_extract_beacon_ssid`).
- EAPOL processing sets M1/M2/M3/M4 bits; `FULL_HS` when M1..M4 (or M2+M3) seen.
- **`total_pwns` counts unique nets** that reached PMKID or M2 (dedup), not raw catches.
  A net already `HAS_PMKID|HAS_M2` is not re-counted and is **excluded from attack**.
- Cap `FG_MAX_NETS = 96` (bounded RAM; ~96×48 B ≈ 4.6 KB). Evict least-recently-seen,
  never-pwned entry when full.

### 3.2 Smart AI channel engine (`fg_ai`)
- Maintain `score[14]` per channel = `w_ap*ap_here + w_cl*clients_here + w_catch*recent_catches`.
- `fg_ai_next_channel()`: weighted pick — mostly the top-scoring channels, with an
  ε-exploration chance to visit others (so new APs are still discovered). Empty channels
  get a minimum floor so they're occasionally rechecked.
- `fg_ai_dwell_ms()`: 400 ms base, scaled up to ~2000 ms on hot channels, min ~150 ms on
  dead ones. Replaces the fixed 400 ms hop.
- `fg_ai_reward(delta)`: a catch bumps the current channel's `recent_catches` (decays over
  time) — the pet gravitates to productive channels. Drives the "happy/attacking" mood.
- Deterministic pacing: no `Math.random`; use a small xorshift seeded from `furi_get_tick`
  captured at start (ticks are allowed at runtime).

### 3.3 Active clientless PMKID solicitation (`fg_attack`)
- Each worker "attack pass" (on the current channel), in addition to the existing deauth:
  for up to N (e.g. 3) not-yet-pwned, not-recently-solicited APs on this channel, send
  `pmkid_build_auth_open()` then `pmkid_build_assoc_req(ssid)` via `esp_wifi_80211_tx`
  (worker context = correct channel). Mark `SOLICITED` with a cooldown so we don't spam.
- Deauth stays (broadcast per-AP + targeted bidirectional per-client) but **skips pwned APs**.
- A single global toggle `attack_on` gates all TX; a second toggle `solicit_on` gates just
  the PMKID solicitation (deauth-only mode).

### 3.4 Personality & progression (`fg_progress`)
- **Name:** persisted string (2–12 chars). Default: a random cute name from a small built-in
  list (seeded once). Rename screen uses Up/Down to change the highlighted char, Left/Right
  to move the cursor, OK to confirm (no keyboard dependency).
- **Moods/faces (~10), all ASCII** (mono font): `pwnd (^o^)`, `excited (^O^)`,
  `friend (^3^)`, `cool/attacking (-_o)`, `looking ( o_o)`↔`(o_o )`, `happy (^_^)`,
  `bored (-_-)`, `lonely (._.)`, `sleepy (u_u)`, `sad (T_T)`. Chosen from catch-rate,
  friends-nearby, attack-mode, and uptime-since-catch.
- **XP/level:** XP = `pmkid*3 + handshake*2 + full_hs*5 + friend*4`. Level curve is
  super-linear: `xp_for(level) = 10 * level * (level+1) / 2` (triangular) so leveling slows.
- **Streaks / records:** current catch streak, best streak, best session pwns, best minute.
- **Achievements** (persisted bitmask): First Blood (1st pwn), Handshake x10, PMKID Master
  (25 PMKIDs), Socialite (1st friend / 5 friends), Full House (1st full 4-way), Night Owl
  (session > 30 min), Channel Surfer (caught on ≥8 channels). Toast on unlock.
- **Persistence:** one file `/ext/apps_data/flippergotchi.dat` — a small versioned text/KV
  blob (name, total_pwns, xp, achievements bitmask, best records). Replaces the bare
  `.score` file; migrates it (reads the old integer as total_pwns if present).

### 3.5 pwngrid social (`fg_pwngrid`)
- Parse the peer beacon JSON fully: `name`, `pwnd_tot`, `uptime`, `face` (minimal scanning
  parser, bounds-checked, no allocator). `FgPeer { char name[20]; uint32_t pwnd; char face[8]; uint32_t last_seen; int8_t rssi; }`.
- Peer table cap `FG_MAX_PEERS = 24`, dedup by name, refresh on re-hear.
- **Persisted friends registry:** a `/ext/apps_data/flippergotchi.friends` list (names +
  total times seen) so "friends remembered across reboots" and a lifetime friend count feed
  the Socialite achievement.
- Advert carries our **real name + current face + total/run pwns + uptime** (fixes v1's
  hardcoded "Flippergotchi"/"(^o^)").

### 3.6 Multi-screen UI (`fg_ui`)
- Screen enum: `FACE (default) | NETS | PEERS | STATS`. **Left/Right cycles** screens
  (wraps). A 1-line footer shows the screen name + hints.
- **FACE:** big face centered; name + Lv + mood + uptime; PWN/HS/KID + AP/CL/FR/attack line
  (v1 layout, enriched). **OK toggles attack**; **hold OK / Up on FACE** could toggle solicit.
- **NETS:** scrollable list of captured nets — `SSID  CHt  [P][H][F]` flags, RSSI. Up/Down
  scroll. Shows dedup count.
- **PEERS:** scrollable friends list — `name  pwnd  age`. Up/Down scroll.
- **STATS:** level/XP bar, streak + records, achievements grid (unlocked vs locked).
- Rename is reachable from STATS (OK on a "Rename" row) or FACE long-press — TBD-free: put
  a "Rename" action on STATS via OK.

### 3.7 GPS-tagged loot (`fg_geo`, optional/guarded)
- If `gnss_is_running()` (start it lazily at app launch if the component is present) and
  `gnss_get_fix()` returns valid, each *new* pwn appends a Wigle-style row to
  `/ext/wifi/flippergotchi_wardrive.csv` (BSSID, SSID, auth, first-seen, channel, RSSI,
  lat, lon). No fix → no-op (the harvester is unaffected). A small "GPS" indicator shows on
  FACE when a fix is present.
- `fam` gains `"gnss"` in `requires` (or a soft link if the symbol is weak); guarded so the
  app still runs if GNSS hardware is absent.

## 4. Persistence formats (all under `/ext/`)
- `apps_data/flippergotchi.dat` — versioned KV: `v=2;name=...;pwn=..;xp=..;ach=<hex>;bestses=..;beststreak=..`
- `apps_data/flippergotchi.friends` — `name,seen_count` lines
- `wifi/flippergotchi.pcap` — unchanged (all EAPOL frames)
- `wifi/pmkid.22000` — unchanged (hashcat 22000)
- `wifi/flippergotchi_wardrive.csv` — new, optional, Wigle-style

## 5. Threading & safety
- Promiscuous cb (WiFi task): append-only writes to net table + EAPOL ring, `volatile`
  published counts, no locks (matches v1 + the reviewed wlan_app rings). Add a release
  fence before publishing any table count.
- Worker task: drains ring → `fg_capture` → net table + stats (under mutex for stats),
  runs `fg_ai` + `fg_attack` + advert.
- GUI thread: draws snapshots under the mutex; list screens read the append-only tables by
  a fenced count (benign torn-name worst case, never OOB — bounded arrays).
- All new buffers are fixed-size / bounded; no attacker-length-driven writes (frame parsing
  reuses the already-hardened `wlan_handshake_parser`).

## 6. Testing
- **Host-testable pure logic** (no IDF): `fg_net_table` (dedup/evict/flags), `fg_ai` scoring
  + dwell math, the XP/level curve + achievement thresholds, the pwngrid JSON parser, and
  the Wigle CSV formatter — validated via a small `tests/` mirror or a Python oracle (no
  host C compiler on this box, per project notes → Python mirror for the math/parse).
- **On-device**: build + flash; verify capture against a known AP (handshake on a phone
  connect, PMKID on a solicit), the dedup count, mood transitions, screen navigation,
  rename persistence across reboot, and (if GPS present) a wardrive row.

## 7. Phasing (build order)
1. `fg_types` + `fg_net_table` + `fg_capture` (dedup/completeness) — the data spine.
2. `fg_ai` (smart dwell) + wire into worker.
3. `fg_attack` (deauth-skip-pwned + PMKID solicit).
4. `fg_progress` (name, XP, achievements, persistence + migration).
5. `fg_pwngrid` (full parse + peers + persisted friends + real advert).
6. `fg_ui` (multi-screen + rename).
7. `fg_geo` (optional GPS) — stretch, last.

Each phase builds green and is independently reviewable; the app stays runnable after each.
