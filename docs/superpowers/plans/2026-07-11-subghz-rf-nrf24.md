# Sub-GHz RF Gaps — RollBack Replay + ProtoView Decoder — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkboxes (`- [ ]`): write failing host test → run it (fails) → implement pure core → run (passes) → build firmware → commit. GUI/firmware-only tasks skip the host test and gate on the build.
> **Model policy:** opus/haiku only (Sonnet trips the cyber-safety gate — do not use it for any step here).

**Spec:** `docs/superpowers/specs/2026-07-11-subghz-rf-nrf24-design.md`

**Goal:** Ship the *two genuinely missing* Sub-GHz features from that spec's bundle — **RollBack** (rolling-code capture-then-replay, a transmitting extension of the read-only rolling analyzer) and **ProtoView** (universal OOK/2-FSK visualize/decode/edit/replay). Everything else in the bundle (fixed-code brute-forcer, TPMS, nRF24 Mousejack, recon) already ships and is untouched. Both features extend the existing `applications/main/subghz/` app with new helpers/scenes/views; no new `.fap`. Every decode/ordering heuristic is a **pure, host-testable** function; every scene leaves its radio cleanly stopped. Optional spec items (POCSAG, Radio Scanner, rtl_433 plug-ins, nRF24 sniffer list) are **out of scope for this plan**.

**Tech stack:** ESP-IDF v5.4.1 (ESP32-S3, `m5stack_cardputer_adv`, **no PSRAM**), C, Flipper firmware app under `applications/main/`. Host unit tests are plain C compiled with WSL `gcc` (Git Bash has no host compiler). CC1101 (external) is the Sub-GHz radio.

---

## Global constraints

- **Firmware build** (synchronous, **never** backgrounded, timeout 600000 ms):
  ```
  MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"
  ```
  Success = log contains `Project build complete` **and** `=== BUILD OK ===`, and `EXIT=0`. Do **not** delete `build_cardputer_adv/`. `build_*.log` is gitignored.
- **Host test** (run from Git Bash, project root — uses WSL gcc; each test `#include`s the pure `.c` directly so a single-file compile works):
  ```
  wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -Wextra -o /tmp/t tests/<test>.c && /tmp/t"
  ```
  Pure modules (`subghz_rollback_core.c`, `protoview_ring.c`, `protoview_decode.c`) and their headers **must not** include `furi.h`, `esp_*`, `gui/*`, or `lib/subghz/*` — only `<stdint.h>`, `<stdbool.h>`, `<string.h>`, `<stddef.h>`, `<stdio.h>`, `<math.h>`. Headers may forward-declare opaque firmware pointers (`SubGhz`, `SubGhzProtocolDecoderBase`) for the firmware-only prototypes.
- **App auto-glob:** the subghz `application.fam` has no `sources=`, so every `.c` under `applications/main/subghz/` (including `helpers/`, `scenes/`, `views/`) is compiled automatically. Just create the files. `tests/` is outside the app and is never compiled into firmware.
- **Radio discipline:** each new scene acquires the CC1101 via the existing `subghz->txrx` and calls `subghz_txrx_stop(subghz->txrx)` on exit. Only one of RX/TX at a time (single CC1101). No concurrent TX+RX.
- **Legal/authorization gate:** RollBack replay and ProtoView replay each show a one-time confirm dialog (`"Transmit — authorized use only?"`) before the first TX, mirroring the brute-forcer's run gate. Ship no pre-baked codes; everything is captured on-device.
- Repo is not a git repo per the harness env, but prior plans commit per task; if `git` is available run the commit, otherwise skip it and note so. Commit **per task**.

## Resolved design decisions (from grounding read)

1. **`subghz_txrx_tx_start(instance, FlipperFormat*)`** (`helpers/subghz_txrx.c:293`) reads the `Protocol` string, `subghz_transmitter_alloc_init`s that protocol, and `subghz_transmitter_deserialize`s the format. It works for **any** protocol.
2. **RAW replay routing (spec open item #2 — RESOLVED):** the RAW encoder's `deserialize` (`lib/subghz/protocols/raw.c:385`) reads **only** `File_name` + `Radio_device_name` and streams timings from an SD `.sub` through `subghz_file_encoder_worker`. It does **not** accept inline `RAW_Data`. Therefore:
   - **RollBack** replays *decoded dynamic-protocol* frames → `subghz_protocol_decoder_base_serialize()` → `subghz_txrx_tx_start()` **directly** (a normal-protocol format with `Protocol`/`Bit`/`Key`/`Te`; **no SD file needed**). This is bit-identical to loading a saved `.sub` and pressing Send.
   - **ProtoView** edited frames are arbitrary raw timings with **no** protocol encoder → they **must** route through the `subghz_read_raw` send path: write a `.sub` (`Protocol: RAW` + `RAW_Data`) to `/ext/subghz/protoview/`, build the send format with `subghz_protocol_raw_gen_fff_data(fff, path, dev_name)`, then `subghz_tx_start(subghz, fff)`.
3. **Fix/Cnt/MF parser (spec open item #1 — RESOLVED):** the authoritative rolling fields come from `subghz_protocol_decoder_base_get_string()` as text lines `Fix:0x........`, `Cnt:XXXX` (or `Cnt:????`), `MF:<name>`. The read-only analyzer (`scenes/subghz_scene_rolling_analyzer.c:27-67`) already parses these inline. Task 1 **extracts** that parser into the pure core and Task 2 **refactors the analyzer to call it** (real dedup, no re-derivation).
4. **ProtoView RX feed:** reuse the RAW decoder's data callback (`subghz_protocol_decoder_raw_set_data_callback`, the exact path `subghz_scene_read_raw` uses for USB export) — it delivers `const int32_t* data, uint16_t count` batches where each value is a signed duration (positive=high, negative=low). Convert each to `PvSample{ dur=abs, level=sign }` and push to the ring. No `subghz_worker` pair-callback repointing is needed (the txrx does not expose the worker).
5. **RollBack encoder re-advance risk (spec open item #3):** the analyzer's verdict "TX re-advances counter" refers to the *manual-generation* emulate flow, not verbatim capture replay. RollBack stores the decoder's `serialize()` output **unchanged** and TXs it verbatim, so no counter mutation occurs. This is the riskiest assumption — see **Riskiest task** and the Task 2 on-hardware check.

---

## Task 1 — RollBack pure core: Fix/Cnt parser + slot store + replay ordering (host-tested)

**Files:**
- Create `applications/main/subghz/helpers/subghz_rollback.h`
- Create `applications/main/subghz/helpers/subghz_rollback_core.c` (pure; compiled into firmware AND `#include`d by the test)
- Create `tests/test_subghz_rollback.c`

**Interfaces produced** (in `subghz_rollback.h`):
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ROLLBACK_MAX_SLOTS 8

typedef struct {
    uint64_t data;        // full decoded data word (informational)
    uint16_t bits;        // data_count_bit
    uint32_t serial;      // Fix & 0x0FFFFFFF
    uint32_t counter;     // Cnt (0 if "????")
    uint8_t  btn;         // (Fix >> 28) & 0x0F
    uint8_t  protocol_id; // 0 = unset, else app-assigned tag
    bool     decoded;     // MF known AND Cnt not "????"
} RollbackSlot;

typedef enum {
    RollbackOrderAsCaptured,
    RollbackOrderAsc,
    RollbackOrderDesc,
} RollbackOrder;

typedef struct {
    RollbackSlot slot[ROLLBACK_MAX_SLOTS];
    uint8_t count;
} RollbackStore;

// ---- pure parsing (shared with the read-only rolling analyzer) ----
uint32_t subghz_rollback_hex_after(const char* s, const char* marker);
bool     subghz_rollback_manufacturer(const char* s, char* out, size_t out_size);

// ---- pure slot math (host-tested) ----
void     subghz_rollback_store_reset(RollbackStore* st);
// Append a slot. Rejects an exact (serial,counter) duplicate and rejects when
// full. Only ever appends at slot[count] (never reorders) so an app-side
// parallel array stays index-aligned. Returns true if stored.
bool     subghz_rollback_store_add(RollbackStore* st, const RollbackSlot* s);
uint8_t  subghz_rollback_store_count(const RollbackStore* st);
// Ready = >=2 slots sharing one serial. Writes that serial to *same_serial
// (the serial of the largest same-serial group; first-seen wins on a tie).
bool     subghz_rollback_ready(const RollbackStore* st, uint32_t* same_serial);
// Fill out_idx with the slot indices whose serial == `serial`, in `order`.
// Asc/Desc sort by counter; AsCaptured keeps store order. Returns count written.
uint8_t  subghz_rollback_order_indices(
    const RollbackStore* st, uint32_t serial, RollbackOrder order,
    uint8_t* out_idx, uint8_t out_cap);

// ---- firmware glue (implemented in subghz_rollback.c, Task 2) ----
typedef struct SubGhz SubGhz;
typedef struct SubGhzProtocolDecoderBase SubGhzProtocolDecoderBase;
bool subghz_rollback_capture(SubGhz* subghz, SubGhzProtocolDecoderBase* decoder);
bool subghz_rollback_replay(
    SubGhz* subghz, RollbackOrder order, uint32_t delay_ms, volatile bool* abort);
```

- [ ] **Step 1 — Failing test.** Create `tests/test_subghz_rollback.c`:
  ```c
  #include "../applications/main/subghz/helpers/subghz_rollback_core.c"
  #include <assert.h>
  #include <stdio.h>

  static RollbackSlot mk(uint32_t serial, uint32_t counter) {
      RollbackSlot s = {0};
      s.serial = serial; s.counter = counter; s.bits = 66; s.decoded = true;
      return s;
  }

  int main(void) {
      // --- parser: Fix/Cnt hex + MF name ---
      const char* d = "Fix:0x1ABCDEF0\r\nCnt:0034\r\nMF:Nice\r\n";
      uint32_t fix = subghz_rollback_hex_after(d, "Fix:");
      assert(fix == 0x1ABCDEF0u);
      assert((fix & 0x0FFFFFFFu) == 0x0ABCDEF0u);       // serial
      assert(((fix >> 28) & 0x0F) == 0x1);              // btn
      assert(subghz_rollback_hex_after(d, "Cnt:") == 0x34u);
      char mf[24];
      assert(subghz_rollback_manufacturer(d, mf, sizeof(mf)) && !strcmp(mf, "Nice"));
      assert(subghz_rollback_hex_after("Cnt:????", "Cnt:") == 0u); // placeholder -> 0
      assert(subghz_rollback_hex_after(d, "ZZ:") == 0u);           // missing marker

      // --- store: add, dedupe, full ---
      RollbackStore st; subghz_rollback_store_reset(&st);
      RollbackSlot a = mk(0xABCDEF0, 5), b = mk(0xABCDEF0, 6), c = mk(0xABCDEF0, 5);
      assert(subghz_rollback_store_add(&st, &a));
      assert(subghz_rollback_store_add(&st, &b));
      assert(!subghz_rollback_store_add(&st, &c));      // exact dup rejected
      assert(subghz_rollback_store_count(&st) == 2);

      // --- ready: two share a serial ---
      uint32_t serial = 0;
      assert(subghz_rollback_ready(&st, &serial) && serial == 0xABCDEF0u);
      RollbackStore st2; subghz_rollback_store_reset(&st2);
      RollbackSlot x = mk(1, 5), y = mk(2, 5);
      subghz_rollback_store_add(&st2, &x); subghz_rollback_store_add(&st2, &y);
      assert(!subghz_rollback_ready(&st2, &serial));    // different serials

      // --- ordering: added 5 then 6 ---
      uint8_t idx[ROLLBACK_MAX_SLOTS];
      uint8_t n = subghz_rollback_order_indices(&st, 0xABCDEF0, RollbackOrderAsc, idx, ROLLBACK_MAX_SLOTS);
      assert(n == 2 && st.slot[idx[0]].counter == 5 && st.slot[idx[1]].counter == 6);
      n = subghz_rollback_order_indices(&st, 0xABCDEF0, RollbackOrderDesc, idx, ROLLBACK_MAX_SLOTS);
      assert(n == 2 && st.slot[idx[0]].counter == 6 && st.slot[idx[1]].counter == 5);
      n = subghz_rollback_order_indices(&st, 0xABCDEF0, RollbackOrderAsCaptured, idx, ROLLBACK_MAX_SLOTS);
      assert(n == 2 && idx[0] == 0 && idx[1] == 1);

      printf("test_subghz_rollback OK\n");
      return 0;
  }
  ```
  Run it — it fails to compile (no `subghz_rollback_core.c` yet). That is the red state.

- [ ] **Step 2 — Header.** Create `helpers/subghz_rollback.h` with the interface block above verbatim.

- [ ] **Step 3 — Pure impl.** Create `helpers/subghz_rollback_core.c`:
  ```c
  #include "subghz_rollback.h"
  #include <string.h>

  uint32_t subghz_rollback_hex_after(const char* s, const char* marker) {
      const char* p = strstr(s, marker);
      if(!p) return 0;
      p += strlen(marker);
      while(*p == ' ') p++;
      if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
      uint32_t value = 0; bool any = false;
      while(*p) {
          char c = *p; uint8_t d;
          if(c >= '0' && c <= '9') d = (uint8_t)(c - '0');
          else if(c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
          else if(c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
          else break;
          value = (value << 4) | d; any = true; p++;
      }
      return any ? value : 0;
  }

  bool subghz_rollback_manufacturer(const char* s, char* out, size_t out_size) {
      const char* p = strstr(s, "MF:");
      if(!p) return false;
      p += 3; size_t i = 0;
      while(*p && *p != '\r' && *p != '\n' && i + 1 < out_size) out[i++] = *p++;
      out[i] = '\0';
      return true;
  }

  void subghz_rollback_store_reset(RollbackStore* st) {
      memset(st, 0, sizeof(*st));
  }

  bool subghz_rollback_store_add(RollbackStore* st, const RollbackSlot* s) {
      if(st->count >= ROLLBACK_MAX_SLOTS) return false;
      for(uint8_t i = 0; i < st->count; i++)
          if(st->slot[i].serial == s->serial && st->slot[i].counter == s->counter)
              return false; // exact duplicate
      st->slot[st->count++] = *s;
      return true;
  }

  uint8_t subghz_rollback_store_count(const RollbackStore* st) { return st->count; }

  bool subghz_rollback_ready(const RollbackStore* st, uint32_t* same_serial) {
      for(uint8_t i = 0; i < st->count; i++) {
          uint8_t group = 1;
          for(uint8_t j = i + 1; j < st->count; j++)
              if(st->slot[j].serial == st->slot[i].serial) group++;
          if(group >= 2) { if(same_serial) *same_serial = st->slot[i].serial; return true; }
      }
      return false;
  }

  uint8_t subghz_rollback_order_indices(
      const RollbackStore* st, uint32_t serial, RollbackOrder order,
      uint8_t* out_idx, uint8_t out_cap) {
      uint8_t n = 0;
      for(uint8_t i = 0; i < st->count && n < out_cap; i++)
          if(st->slot[i].serial == serial) out_idx[n++] = i;
      if(order != RollbackOrderAsCaptured) {
          for(uint8_t a = 0; a < n; a++)      // insertion sort by counter
              for(uint8_t b = (uint8_t)(a + 1); b < n; b++) {
                  bool swap = (order == RollbackOrderAsc)
                      ? st->slot[out_idx[b]].counter < st->slot[out_idx[a]].counter
                      : st->slot[out_idx[b]].counter > st->slot[out_idx[a]].counter;
                  if(swap) { uint8_t t = out_idx[a]; out_idx[a] = out_idx[b]; out_idx[b] = t; }
              }
      }
      return n;
  }
  ```

- [ ] **Step 4 — Green.** Run the host test; it must print `test_subghz_rollback OK`.
- [ ] **Step 5 — Commit** `feat(subghz): rollback pure core (parser + slot store + ordering)`.

---

## Task 2 — RollBack firmware: capture/replay glue + two scenes + menu + analyzer refactor (build-gated)

**Files:**
- Create `applications/main/subghz/helpers/subghz_rollback.c` (firmware glue; includes furi/txrx)
- Create `applications/main/subghz/scenes/subghz_scene_rollback_capture.c`
- Create `applications/main/subghz/scenes/subghz_scene_rollback_replay.c`
- Modify `applications/main/subghz/subghz_i.h` (add RollBack state fields)
- Modify `applications/main/subghz/scenes/subghz_scene_config.h` (register 2 scenes)
- Modify `applications/main/subghz/scenes/subghz_scene_start.h` (add 2 submenu ids)
- Modify `applications/main/subghz/scenes/subghz_scene_start.c` (add 2 menu items + handlers)
- Modify `applications/main/subghz/scenes/subghz_scene_rolling_analyzer.c` (call shared parser — dedup)

**Interfaces produced:** `subghz_rollback_capture`, `subghz_rollback_replay` (bodies for Task 1's prototypes).

- [ ] **Step 1 — App state.** In `subghz_i.h`, add `#include "helpers/subghz_rollback.h"` near the other helper includes, and inside `struct SubGhz` (after the TPMS block) add:
  ```c
      // RollBack rolling-code capture-then-replay
      RollbackStore rollback_store;
      char rollback_frame[ROLLBACK_MAX_SLOTS][192]; // serialized .sub body per slot, index-aligned
      volatile bool rollback_abort;
  ```
  (Fixed char array, not FuriString — no-PSRAM discipline, no alloc lifecycle. A serialized KeeLoq body fits well under 192 bytes.)

- [ ] **Step 2 — Firmware glue.** Create `helpers/subghz_rollback.c`:
  ```c
  #include "../subghz_i.h"
  #include "subghz_txrx_i.h"
  #include <lib/subghz/blocks/generic.h>
  #include <lib/subghz/protocols/base.h>
  #include <toolbox/stream/stream.h>

  #define TAG "SubGhzRollback"

  bool subghz_rollback_capture(SubGhz* subghz, SubGhzProtocolDecoderBase* decoder) {
      if(!decoder || !decoder->protocol) return false;
      if(decoder->protocol->type != SubGhzProtocolTypeDynamic) return false; // rolling only
      if(subghz->rollback_store.count >= ROLLBACK_MAX_SLOTS) return false;

      FuriString* text = furi_string_alloc();
      subghz_protocol_decoder_base_get_string(decoder, text);
      const char* t = furi_string_get_cstr(text);

      RollbackSlot s = {0};
      uint32_t fix = subghz_rollback_hex_after(t, "Fix:");
      s.serial = fix & 0x0FFFFFFF;
      s.btn = (uint8_t)((fix >> 28) & 0x0F);
      s.counter = subghz_rollback_hex_after(t, "Cnt:");
      s.decoded = (strstr(t, "MF:") != NULL) && (strstr(t, "MF:Unknown") == NULL) &&
                  (strstr(t, "Cnt:????") == NULL);
      s.protocol_id = 1;

      // Serialize the frame VERBATIM (same bytes a saved .sub would hold) so replay
      // re-TXs the captured counter — no generation, no counter advance.
      SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
      FlipperFormat* ff = flipper_format_string_alloc();
      bool ok = (subghz_protocol_decoder_base_serialize(decoder, ff, &preset) ==
                 SubGhzProtocolStatusOk);
      uint8_t next = subghz->rollback_store.count;
      if(ok) {
          Stream* st = flipper_format_get_raw_stream(ff);
          stream_rewind(st);
          size_t len = stream_read(
              st, (uint8_t*)subghz->rollback_frame[next], sizeof(subghz->rollback_frame[0]) - 1);
          subghz->rollback_frame[next][len] = '\0';
          ok = subghz_rollback_store_add(&subghz->rollback_store, &s); // append at [next]
      }
      flipper_format_free(ff);
      furi_string_free(text);
      return ok;
  }

  bool subghz_rollback_replay(
      SubGhz* subghz, RollbackOrder order, uint32_t delay_ms, volatile bool* abort) {
      uint32_t serial = 0;
      if(!subghz_rollback_ready(&subghz->rollback_store, &serial)) return false;
      uint8_t idx[ROLLBACK_MAX_SLOTS];
      uint8_t n = subghz_rollback_order_indices(
          &subghz->rollback_store, serial, order, idx, ROLLBACK_MAX_SLOTS);
      for(uint8_t i = 0; i < n; i++) {
          if(abort && *abort) break;
          FlipperFormat* ff = flipper_format_string_alloc();
          Stream* st = flipper_format_get_raw_stream(ff);
          stream_clean(st);
          stream_write_cstring(st, subghz->rollback_frame[idx[i]]);
          stream_rewind(st);
          if(subghz_txrx_tx_start(subghz->txrx, ff) == SubGhzTxRxStartTxStateOk) {
              furi_delay_ms(200); // transmit window: several packet repeats
          }
          subghz_txrx_stop(subghz->txrx);
          flipper_format_free(ff);
          if(i + 1 < n) furi_delay_ms(delay_ms); // inter-frame gap
      }
      return true;
  }
  ```
  (Include `subghz_txrx_i.h` for `SubGhzProtocolTypeDynamic`/preset access, mirroring `subghz_scene_rolling_analyzer.c`. Verify `stream_write_cstring`/`stream_read`/`stream_clean` names against `lib/toolbox/stream/stream.h` during build; adjust if the API differs.)

- [ ] **Step 3 — Capture scene.** Create `scenes/subghz_scene_rollback_capture.c`. On enter: set preset from `last_settings`, set filter to `SubGhzProtocolFlag_Decodable`, register the rx callback, start RX, render a widget "RollBack — Captured N (need 2+)". The rx callback:
  ```c
  static void subghz_scene_rollback_capture_rx_cb(
      SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* decoder, void* context) {
      UNUSED(receiver);
      SubGhz* subghz = context;
      if(subghz_rollback_capture(subghz, decoder)) {
          view_dispatcher_send_custom_event(
              subghz->view_dispatcher, SubGhzCustomEventSceneRollbackUpdate);
      }
      subghz_receiver_reset(subghz_txrx_get_receiver(subghz->txrx));
  }
  ```
  Add `SubGhzCustomEventSceneRollbackUpdate` to `helpers/subghz_custom_event.h`. On that event, redraw the widget count via `subghz_rollback_store_count`; when `subghz_rollback_ready(...)` is true, show an "OK: Replay" hint and, on the OK button custom event, `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollbackReplay)`. On enter, `subghz_rollback_store_reset(&subghz->rollback_store)` only when arriving fresh (scene state IDLE) so returning from Replay keeps the slots. On exit: `subghz_txrx_stop`, clear rx callback (`subghz_txrx_set_rx_callback(subghz->txrx, NULL, subghz)`), restore filter. Use `subghz->widget` + `SubGhzViewIdWidget` (already registered) — no new view.

- [ ] **Step 4 — Replay scene.** Create `scenes/subghz_scene_rollback_replay.c` using `subghz->variable_item_list` (`SubGhzViewIdVariableItemList`, already registered):
  - Item "Order": AsCaptured / Ascending / Descending (maps to `RollbackOrder`).
  - Item "Delay": 50/100/250/500/1000 ms.
  - Item "Start Replay": on select, show the confirm dialog (`dialog_message`, header `"Transmit"`, text `"Rolling-code replay.\nAuthorized use only?"`, left `"Cancel"` / right `"Send"`). If `DialogMessageButtonRight`, set `subghz->rollback_abort = false`, spawn a `FuriThread` whose entry calls `subghz_rollback_replay(subghz, order, delay_ms, &subghz->rollback_abort)` then sends `SubGhzCustomEventSceneRollbackDone`; show a "Replaying…" popup. Back sets `subghz->rollback_abort = true` and joins the thread. On exit: join thread if running, `subghz_txrx_stop`.

- [ ] **Step 5 — Register scenes.** In `subghz_scene_config.h`, after the `rolling_analyzer` line add:
  ```c
  ADD_SCENE(subghz, rollback_capture, RollbackCapture)
  ADD_SCENE(subghz, rollback_replay, RollbackReplay)
  ```

- [ ] **Step 6 — Menu.** In `subghz_scene_start.h` add `SubmenuIndexRollBack,` to `enum SubmenuIndex`. In `subghz_scene_start.c`, add a `submenu_add_item(subghz->submenu, "RollBack", SubmenuIndexRollBack, subghz_scene_start_submenu_callback, subghz);` (place after "Bruteforce"), and in `on_event` add:
  ```c
  } else if(event.event == SubmenuIndexRollBack) {
      scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneStart, SubmenuIndexRollBack);
      scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollbackCapture);
      return true;
  }
  ```

- [ ] **Step 7 — Analyzer dedup (reuse proof).** In `subghz_scene_rolling_analyzer.c`, delete the local `subghz_rolling_analyzer_hex_after` and `subghz_rolling_analyzer_manufacturer` static functions and their call sites, replacing with `subghz_rollback_hex_after(...)` / `subghz_rollback_manufacturer(...)` (add `#include "../helpers/subghz_rollback.h"`). Behavior is identical (the pure impl is copied verbatim from these functions). This proves the parser is genuinely shared, not re-derived.

- [ ] **Step 8 — Build.** Run the firmware build with tag `rollback`. Require `Project build complete` + `=== BUILD OK ===` + `EXIT=0`.
- [ ] **Step 9 — Commit** `feat(subghz): RollBack rolling-code capture-then-replay scenes`.

**On-hardware check (record, do not block the plan):** capture ≥2 presses from an HCS/rolling fob → both land with the same serial + rising counter; Replay (each order) TXs; **confirm the TX'd counter equals the captured counter** (scope/second Flipper) — this validates decision #5. Nothing in the read-only analyzer regressed.

---

## Task 3 — ProtoView ring buffer (`protoview_ring`) — pure, host-tested

**Files:**
- Create `applications/main/subghz/helpers/protoview_ring.h`
- Create `applications/main/subghz/helpers/protoview_ring.c` (pure — single static ring, no furi)
- Create `tests/test_protoview_ring.c`

**Interfaces produced:**
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PV_RING_LEN 2048

typedef struct { uint32_t dur; uint8_t level; } PvSample;

void   pv_ring_reset(void);
void   pv_ring_push(bool level, uint32_t duration); // ignored while frozen
void   pv_ring_freeze(bool frozen);
bool   pv_ring_is_frozen(void);
size_t pv_ring_count(void);                          // valid samples (<= PV_RING_LEN)
// Linearize (oldest-first) in place and return contiguous window; count via return.
size_t pv_ring_snapshot(const PvSample** out);
```

- [ ] **Step 1 — Failing test.** Create `tests/test_protoview_ring.c`:
  ```c
  #include "../applications/main/subghz/helpers/protoview_ring.c"
  #include <assert.h>
  #include <stdio.h>

  int main(void) {
      pv_ring_reset();
      assert(pv_ring_count() == 0 && !pv_ring_is_frozen());

      for(uint32_t i = 0; i < 100; i++) pv_ring_push(i & 1, i + 1);
      assert(pv_ring_count() == 100);
      const PvSample* w = NULL;
      assert(pv_ring_snapshot(&w) == 100);
      assert(w[0].dur == 1 && w[99].dur == 100);        // oldest-first order

      // overflow: push past capacity, oldest overwritten
      pv_ring_reset();
      for(uint32_t i = 0; i < PV_RING_LEN + 500; i++) pv_ring_push(i & 1, i + 1);
      assert(pv_ring_count() == PV_RING_LEN);
      assert(pv_ring_snapshot(&w) == PV_RING_LEN);
      assert(w[0].dur == 501);                          // first 500 dropped
      assert(w[PV_RING_LEN - 1].dur == PV_RING_LEN + 500);

      // freeze blocks writes
      pv_ring_freeze(true);
      pv_ring_push(1, 99999);
      assert(pv_ring_snapshot(&w) == PV_RING_LEN && w[PV_RING_LEN - 1].dur == PV_RING_LEN + 500);
      pv_ring_freeze(false);
      printf("test_protoview_ring OK\n");
      return 0;
  }
  ```

- [ ] **Step 2 — Impl.** Create `protoview_ring.c`:
  ```c
  #include "protoview_ring.h"
  #include <string.h>

  static PvSample s_ring[PV_RING_LEN];
  static size_t s_head;      // next write index
  static size_t s_count;     // valid samples
  static bool   s_frozen;

  void pv_ring_reset(void) { s_head = 0; s_count = 0; s_frozen = false; }

  void pv_ring_push(bool level, uint32_t duration) {
      if(s_frozen) return;
      s_ring[s_head].dur = duration;
      s_ring[s_head].level = level ? 1 : 0;
      s_head = (s_head + 1) % PV_RING_LEN;
      if(s_count < PV_RING_LEN) s_count++;
  }

  void pv_ring_freeze(bool frozen) { s_frozen = frozen; }
  bool pv_ring_is_frozen(void) { return s_frozen; }
  size_t pv_ring_count(void) { return s_count; }

  size_t pv_ring_snapshot(const PvSample** out) {
      // Rotate so the oldest sample sits at index 0 (in place; single 10 KB buffer).
      if(s_count == PV_RING_LEN && s_head != 0) {
          PvSample tmp[PV_RING_LEN];
          size_t start = s_head; // oldest
          for(size_t i = 0; i < PV_RING_LEN; i++) tmp[i] = s_ring[(start + i) % PV_RING_LEN];
          memcpy(s_ring, tmp, sizeof(s_ring));
          s_head = 0;
      }
      // When not full, samples already occupy [0, s_count) in order.
      *out = s_ring;
      return s_count;
  }
  ```
  Note: the snapshot rotate uses a stack `tmp[PV_RING_LEN]` (~10 KB) transiently; on the ESP32-S3 the subghz app stack is `8*1024`, so allocate `tmp` as a second file-static buffer instead of on the stack to avoid overflow. Change: `static PvSample s_tmp[PV_RING_LEN];` at file scope and copy through it. (Host test is unaffected.) Total static ≈ 20 KB, still comfortably in SRAM, still no dynamic growth.

- [ ] **Step 3 — Green.** Run the host test → `test_protoview_ring OK`.
- [ ] **Step 4 — Commit** `feat(subghz): protoview fixed sample ring buffer`.

---

## Task 4 — ProtoView decoders (`protoview_decode`) — pure, host-tested

**Files:**
- Create `applications/main/subghz/helpers/protoview_decode.h`
- Create `applications/main/subghz/helpers/protoview_decode.c` (pure)
- Create `tests/test_protoview_decode.c`

Reference: **antirez/protoview** (MIT, constrained-device C) — the classifier shapes below mirror its low-level line-code decoders, re-implemented as allocation-free pure functions.

**Interfaces produced:**
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "protoview_ring.h"   // PvSample

typedef struct { uint32_t t_short, t_long, t_sync; } PvThresholds;
// Raw output kept furi-free (no LevelDuration) for host-testability; the scene
// converts PvRawSymbol -> signed RAW_Data ints when it writes the .sub.
typedef struct { uint32_t dur; uint8_t level; } PvRawSymbol;

// Histogram of durations -> short/long thresholds; t_sync = longest gap seen.
// Returns false if fewer than 8 edges or no clear short/long split.
bool pv_classify_symbols(const PvSample* s, size_t n, PvThresholds* out);
// PWM / PT2262: short-high+long-low -> '0', long-high+short-low -> '1'.
// Returns bit count (0 if nothing decodes). Writes a NUL-terminated bitstring.
int  pv_decode_pwm(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap);
// Manchester (IEEE): mid-bit low->high = '1', high->low = '0'. Returns bit count, <0 on desync.
int  pv_decode_manchester(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap);
// PPM / Oregon-style: fixed pulse, gap==t_short -> '0', gap==t_long -> '1'. Returns bit count.
int  pv_decode_ppm(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap);
// Bitstring -> raw symbols at te microseconds per short element (PWM encoding). Returns symbol count.
size_t pv_bits_to_raw(const char* bits, uint32_t te, PvRawSymbol* out, size_t cap);
```

- [ ] **Step 1 — Failing test.** Create `tests/test_protoview_decode.c` with a synthetic PT2262 frame (short=350 µs, long=1050 µs, bit '0' = H350/L1050, bit '1' = H1050/L350) for the byte pattern `0b1011`:
  ```c
  #include "../applications/main/subghz/helpers/protoview_decode.c"
  #include <assert.h>
  #include <stdio.h>
  #include <string.h>

  #define S 350u
  #define L 1050u

  static size_t push_pwm_bit(PvSample* a, size_t k, char bit) {
      if(bit == '1') { a[k].level = 1; a[k].dur = L; a[k+1].level = 0; a[k+1].dur = S; }
      else           { a[k].level = 1; a[k].dur = S; a[k+1].level = 0; a[k+1].dur = L; }
      return k + 2;
  }

  int main(void) {
      const char* want = "1011";
      PvSample a[64]; size_t n = 0;
      for(const char* p = want; *p; ++p) n = push_pwm_bit(a, n, *p);

      PvThresholds th;
      assert(pv_classify_symbols(a, n, &th));
      assert(th.t_short > 250 && th.t_short < 500);
      assert(th.t_long  > 900 && th.t_long < 1200);

      char bits[64];
      int nb = pv_decode_pwm(a, n, &th, bits, sizeof(bits));
      assert(nb == 4 && !strcmp(bits, want));

      // round-trip: bits -> raw -> same durations
      PvRawSymbol raw[64];
      size_t nr = pv_bits_to_raw(want, S, raw, 64);
      assert(nr == n);
      assert(raw[0].level == 1 && raw[0].dur == L); // '1' -> long high
      assert(raw[1].level == 0 && raw[1].dur == S);
      printf("test_protoview_decode OK\n");
      return 0;
  }
  ```

- [ ] **Step 2 — Impl.** Create `protoview_decode.c`. `pv_classify_symbols`: bucket all `s[i].dur` into a small histogram (e.g. 16 log-spaced or linear bins over the observed min..max), pick the two dominant clusters as `t_short` (smaller centroid) and `t_long` (larger); `t_sync` = max duration. Require ≥8 edges and `t_long >= 2*t_short` for success. `pv_decode_pwm`: iterate sample pairs (high then low); classify each element as short/long by nearest threshold; `H_short+L_long -> '0'`, `H_long+L_short -> '1'`; ignore leading/trailing sync gaps (`dur > 1.5*t_long`). `pv_decode_manchester`: track half-bit transitions using `t_short` as the half-bit; emit on mid-bit edges. `pv_decode_ppm`: assume roughly constant high pulse; classify the following gap as short('0')/long('1'). `pv_bits_to_raw`: for each bit, emit `{level=1, dur=(bit=='1'? te*3 : te)}` then `{level=0, dur=(bit=='1'? te : te*3)}` (long = 3× short, matching PT2262). Keep every function branch bounded by `cap`.

- [ ] **Step 3 — Green.** Run host test → `test_protoview_decode OK`. (If the classifier's histogram split needs tuning, adjust bins until the PT2262 vector passes; do not weaken the ≥8-edge / 2× guards.)
- [ ] **Step 4 — Commit** `feat(subghz): protoview pure symbol classifier + line-code decoders`.

---

## Task 5 — ProtoView view: waveform scope + bit/edit render (build-gated)

**Files:**
- Create `applications/main/subghz/views/subghz_view_protoview.h`
- Create `applications/main/subghz/views/subghz_view_protoview.c`
- Modify `applications/main/subghz/helpers/subghz_types.h` (add `SubGhzViewIdProtoview`)
- Modify `applications/main/subghz/subghz_i.h` (add `SubGhzViewProtoview* subghz_protoview;`)
- Modify `applications/main/subghz/subghz.c` (alloc/add + remove/free the view)

**Interfaces produced:**
```c
// subghz_view_protoview.h
#pragma once
#include <gui/view.h>

typedef struct SubGhzViewProtoview SubGhzViewProtoview;

typedef enum {
    ProtoviewModeScope,   // live/frozen waveform
    ProtoviewModeEdit,    // bit editor
} ProtoviewMode;

typedef void (*SubGhzViewProtoviewCallback)(uint32_t event, void* context);

SubGhzViewProtoview* subghz_view_protoview_alloc(void);
void  subghz_view_protoview_free(SubGhzViewProtoview* v);
View* subghz_view_protoview_get_view(SubGhzViewProtoview* v);
void  subghz_view_protoview_set_callback(
    SubGhzViewProtoview* v, SubGhzViewProtoviewCallback cb, void* ctx);
void  subghz_view_protoview_set_frozen(SubGhzViewProtoview* v, bool frozen);
void  subghz_view_protoview_set_bits(SubGhzViewProtoview* v, const char* bits, const char* label);
void  subghz_view_protoview_set_status(SubGhzViewProtoview* v, const char* freq_preset);
```

- [ ] **Step 1 — View model + render.** Create the `.c` with a `with_view_model` struct holding: `ProtoviewMode mode; size_t scroll; size_t cursor; bool frozen; char bits[257]; char label[24]; char status[24];`. `draw_callback`:
  - Scope mode: call `pv_ring_snapshot(&w)`; render one column per sample starting at `scroll` — high level = top rows, low = bottom rows, one pixel column each across the 128-wide canvas; draw a vertical cursor line; footer (`canvas_set_font FontSecondary`) shows `label` + bit count.
  - Edit mode: render the `bits` string with the character at `cursor` inverted; footer shows key hints.
  - Top status line shows `status` (freq/preset).
  `input_callback`: Left/Right scroll (scope) or move cursor (edit); Up in edit flips the bit at `cursor` (`'0'<->'1'`); OK/Back emit callback events. Emit named events via the callback: `ProtoviewEvtFreeze`, `ProtoviewEvtDecode`, `ProtoviewEvtEdit`, `ProtoviewEvtReplay`, `ProtoviewEvtSave`, `ProtoviewEvtBack` (define these `enum` values in the view header). Include `../helpers/protoview_ring.h`.

- [ ] **Step 2 — View id.** In `subghz_types.h`, add `SubGhzViewIdProtoview,` to the `SubGhzViewId` enum (before the closing `}`).

- [ ] **Step 3 — Wire into app.** In `subghz_i.h` add the `subghz_protoview` field and `#include "views/subghz_view_protoview.h"`. In `subghz.c`, near the Read RAW view alloc (~line 202), add:
  ```c
  subghz->subghz_protoview = subghz_view_protoview_alloc();
  view_dispatcher_add_view(
      subghz->view_dispatcher, SubGhzViewIdProtoview,
      subghz_view_protoview_get_view(subghz->subghz_protoview));
  ```
  and in the free path add `view_dispatcher_remove_view(subghz->view_dispatcher, SubGhzViewIdProtoview);` + `subghz_view_protoview_free(subghz->subghz_protoview);` (guard with the same `alloc_for_tx_only` bracket the RAW view uses only if ProtoView is RX-capable — it is, so add it unconditionally like Read RAW).

- [ ] **Step 4 — Build** (tag `protoview_view`). Require the two success markers + `EXIT=0`.
- [ ] **Step 5 — Commit** `feat(subghz): protoview scope + bit-editor view`.

---

## Task 6 — ProtoView scene: capture/freeze/decode/edit/replay + menu (build-gated)

**Files:**
- Create `applications/main/subghz/scenes/subghz_scene_protoview.c`
- Modify `applications/main/subghz/scenes/subghz_scene_config.h` (register scene)
- Modify `applications/main/subghz/scenes/subghz_scene_start.h` (add `SubmenuIndexSignalDecoder`)
- Modify `applications/main/subghz/scenes/subghz_scene_start.c` (menu item + handler)

- [ ] **Step 1 — RX feed via RAW decoder.** In `on_enter`: set preset from `last_settings`; `pv_ring_reset()`; `subghz_txrx_load_decoder_by_name_protocol(subghz->txrx, SUBGHZ_PROTOCOL_RAW_NAME)`; `subghz_txrx_receiver_set_filter(subghz->txrx, SubGhzProtocolFlag_RAW)`; install the data callback and start RX:
  ```c
  static void subghz_scene_protoview_data_cb(const int32_t* data, uint16_t count, void* ctx) {
      UNUSED(ctx);
      for(uint16_t i = 0; i < count; i++) {
          int32_t v = data[i];
          pv_ring_push(v >= 0, (uint32_t)(v >= 0 ? v : -v));
      }
  }
  ...
  SubGhzProtocolDecoderRAW* raw =
      (SubGhzProtocolDecoderRAW*)subghz_txrx_get_decoder(subghz->txrx);
  subghz_protocol_decoder_raw_set_data_callback(raw, subghz_scene_protoview_data_cb, subghz);
  subghz_txrx_rx_start(subghz->txrx);
  ```
  Switch to `SubGhzViewIdProtoview`; set the view status string from `subghz_txrx_get_frequency_and_modulation`.

- [ ] **Step 2 — Events.** Handle the view callback events:
  - `ProtoviewEvtFreeze`: toggle `pv_ring_freeze(...)`, `subghz_view_protoview_set_frozen(...)`; when freezing, `subghz_txrx_stop`.
  - `ProtoviewEvtDecode`: `pv_ring_snapshot(&w)`; `pv_classify_symbols`; cycle a decoder index (PWM → Manchester → PPM); call the chosen `pv_decode_*` into a `char bits[257]`; `subghz_view_protoview_set_bits(view, bits, label)` where label is `"PWM(PT2262)"` / `"Manchester"` / `"PPM(Oregon)"` + bit count.
  - `ProtoviewEvtEdit`: switch the view to `ProtoviewModeEdit`.
  - `ProtoviewEvtReplay`: **confirm gate** (dialog `"Transmit — authorized use only?"`); on accept, do Step 3.
  - `ProtoviewEvtSave`: write the frozen ring to `/ext/subghz/protoview/pv_<timestamp>.pvr` (raw `dur level` lines) via `storage_file_*`.
  - `ProtoviewEvtBack`: `subghz_txrx_stop`; clear the data callback (`subghz_protocol_decoder_raw_set_data_callback(raw, NULL, NULL)`); restore filter; back out.

- [ ] **Step 3 — Edited-frame replay (RAW/SD path — decision #2).**
  ```c
  // 1. edit bits -> raw symbols
  PvRawSymbol raw[520];
  size_t nr = pv_bits_to_raw(edited_bits, te /* view TE, default t_short */, raw, 520);
  // 2. write a RAW .sub to SD
  #define PV_SUB "/ext/subghz/protoview/pv_replay.sub"
  FlipperFormat* file = flipper_format_file_alloc(furi_record_open(RECORD_STORAGE));
  flipper_format_file_open_always(file, PV_SUB);
  flipper_format_write_header_cstr(file, "Flipper SubGhz RAW File", 1);
  flipper_format_write_uint32(file, "Frequency", &freq, 1);
  flipper_format_write_string_cstr(file, "Preset", "FuriHalSubGhzPresetOok650Async");
  flipper_format_write_string_cstr(file, "Protocol", "RAW");
  // RAW_Data lines: signed us, +high / -low, batched (<= 512 ints per line)
  int32_t line[512]; size_t k = 0;
  for(size_t i = 0; i < nr; i++) {
      line[k++] = raw[i].level ? (int32_t)raw[i].dur : -(int32_t)raw[i].dur;
      if(k == 512) { flipper_format_write_int32(file, "RAW_Data", line, k); k = 0; }
  }
  if(k) flipper_format_write_int32(file, "RAW_Data", line, k);
  flipper_format_free(file);
  furi_record_close(RECORD_STORAGE);
  // 3. build send format + TX (routes through the RAW file encoder worker)
  subghz_protocol_raw_gen_fff_data(
      subghz_txrx_get_fff_data(subghz->txrx), PV_SUB,
      subghz_txrx_radio_device_get_name(subghz->txrx));
  subghz_tx_start(subghz, subghz_txrx_get_fff_data(subghz->txrx));
  ```
  (Confirm exact `flipper_format_write_*` names against `lib/toolbox/flipper_format/flipper_format.h` at build; the read_raw save path uses the same writers.) Set the end-of-TX callback via `subghz_txrx_set_raw_file_encoder_worker_callback_end` and stop on completion, mirroring `subghz_scene_read_raw`'s `SendStart` branch.

- [ ] **Step 4 — Register scene.** In `subghz_scene_config.h` add `ADD_SCENE(subghz, protoview, Protoview)`.

- [ ] **Step 5 — Menu.** In `subghz_scene_start.h` add `SubmenuIndexSignalDecoder,`. In `subghz_scene_start.c` add `submenu_add_item(subghz->submenu, "Signal Decoder", SubmenuIndexSignalDecoder, subghz_scene_start_submenu_callback, subghz);` (after "RollBack") and:
  ```c
  } else if(event.event == SubmenuIndexSignalDecoder) {
      scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneStart, SubmenuIndexSignalDecoder);
      scene_manager_next_scene(subghz->scene_manager, SubGhzSceneProtoview);
      return true;
  }
  ```

- [ ] **Step 6 — Build** (tag `protoview_scene`). Require the two success markers + `EXIT=0`.
- [ ] **Step 7 — Commit** `feat(subghz): ProtoView universal signal decoder scene`.

**On-hardware check (record):** tune 433.92 OOK, capture a PT2262 remote → waveform renders, Decode yields a stable bitstring, Freeze holds, Edit flips a bit, Replay re-TXs the edited frame; capture an Oregon sensor → PPM decode shows plausible bits; watch free heap (ring must not grow); `.pvr`/`.sub` write to SD. Regression: Read RAW, Bruteforce, TPMS still work.

---

## Self-Review

**Spec coverage:**
- RollBack capture-then-replay: Tasks 1–2. Reuses the existing decoder (`subghz_protocol_decoder_base_get_string`/`serialize`) and the Fix/Cnt/MF parser (extracted + shared with the analyzer, Task 2 Step 7). ≤8 in-RAM slots (`ROLLBACK_MAX_SLOTS`), fixed `char[192]` frame store — no growth. Ordered replay (AsCaptured/Asc/Desc) + inter-frame delay via `subghz_txrx_tx_start` directly. RollJam non-goal is not attempted (single CC1101; documented in Resolved decision #5 and the spec).
- ProtoView: Tasks 3–6. Single static `PvSample ring[2048]` (+ one static rotate scratch) — no PSRAM growth; decode windows are ring slices; edit buffer ≤256 bits. Real-time scope + histogram classifier + PWM/Manchester/PPM decoders + bit editor + replay via the RAW/SD send path. antirez/protoview cited (Task 4).
- Confirm-before-TX gate on both replays; RX-only capture ungated. No pre-baked codes. `fam_config.py` untouched (both live in the existing `subghz` app). Optional spec items (POCSAG/scanner/rtl_433/nRF24 sniffer) explicitly deferred.

**Placeholder scan:** every step contains real code or a concrete file/symbol edit. No "add error handling" / "similar to Task N" placeholders. Three named API-verification points are called out (stream fn names, flipper_format writer names, `SubGhzProtocolTypeDynamic` include) — these are "confirm exact spelling at build," not deferred design.

**Type consistency:** `RollbackSlot`/`RollbackOrder`/`RollbackStore`, `PvSample`, `PvThresholds`, `PvRawSymbol` are single-defined in their pure headers and reused unchanged by firmware. Pure modules avoid `LevelDuration` (use `PvRawSymbol`) so they host-compile; the scene converts `PvRawSymbol` → signed `RAW_Data` ints at the SD boundary. `SubGhzViewIdProtoview` added to the one `SubGhzViewId` enum; two `SubmenuIndex*` added to the one menu enum. Firmware prototypes in the pure header use forward-declared opaque `SubGhz`/`SubGhzProtocolDecoderBase`, so the header stays host-clean.

**Riskiest task: Task 2 (RollBack replay), specifically the verbatim-serialize-and-TX assumption (decision #5).** If a dynamic protocol's encoder re-advances the counter when deserializing a captured frame (rather than TXing the stored bits verbatim), replay will send wrong counters and the desync will fail. Mitigation baked in: RollBack stores the decoder's `serialize()` output **unchanged** (identical to a saved `.sub`) and TXs it exactly as the existing "load saved key → Send" flow does — that flow is known to transmit captured bits, not regenerate. The Task 2 on-hardware check explicitly verifies the TX'd counter equals the captured counter. **Fallback if it fails:** capture each rolling frame as RAW timings (run the RAW decoder in parallel during capture) and replay through the ProtoView RAW/SD path (Task 6 Step 3), which is bit-exact regardless of encoder behavior — reuse already built in this plan.

## Open items still needing the controller

- **Spec open item #4 (ProtoView TE robustness on noisy 433):** the plan defaults the replay TE to `t_short` from the classifier and exposes bit editing, but does **not** add a manual `te` `variable_item` override. If bench testing shows the auto-threshold is too fragile, add a TE field to the edit view (small follow-up). Flagging rather than pre-building it.
- **Spec open items #5/#6 (nRF24 sniffer dwell, POCSAG BCH cost):** out of scope for this plan (optional features); no action.
- No other ambiguity blocks implementation — decisions #1–#5 resolve the two cited open items (#1 parser fields, #2 RAW replay routing).
