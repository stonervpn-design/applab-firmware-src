# Flipper Share NFC — Phase 4 (ship as a standalone app) — Design

**Date:** 2026-07-31
**Status:** Approved direction.
**Target:** `NEW_v2/src_m5stick_s3` first, then mirror to `src_cardputer_adv`.
NFC hardware = **M5 Unit NFC (ST25R3916)** over Grove I2C (both ends).
**Predecessor:** `2026-07-31-flipper-share-nfc-port-design.md` (Phases 1–3, all
PASSED on hardware against a real Flipper Zero, both directions).

## Goal

Turn the hardware-proven inline prototype into a shipping app: real file
selection for sending, received files saved to SD, arbitrary file sizes via
SD streaming (no whole-file RAM buffer), the nsh protocol + ST25R workers
extracted into a reusable engine module, a proper standalone `nfc_share` app
with scenes, mirrored into both firmware trees, and an applab catalog entry.

## Current state (what Phase 4 builds on)

All protocol + radio work is done and proven. The logic currently lives inline
in `applications/main/bus_tools/scenes/bus_tools_scene_st25r_nfc.c` (~524 lines):
CRC-16/CCITT-FALSE, ANNOUNCE/REQUEST/DATA build+parse, the listener-sender
worker (`nfc_tp_listener_worker`) and the poller-receiver worker
(`nfc_rx_poller_worker`). Both are byte-exact vs. lomalkin's Flipper Share.

**What is hardcoded / RAM-only today (the Phase 4 gaps):**
- **Send** serves a fixed 240-byte `kFileText` with a precomputed `kFileMd5`;
  single block; no file picker.
- **Receive** buffers the whole file into `malloc`'d RAM (`RX_MAX_FILE` 64 KB
  cap), md5-verifies, but **never writes it to SD**.
- No standalone app; it is a `bus_tools` test scene with stale-labelled UI and
  on-screen hex diagnostics.

Protocol wire format, the ST25R3916 driver, and the "M5 Unit NFC on both ends"
requirement are **unchanged** by Phase 4.

## Architecture

New app directory `applications/main/nfc_share/`:

```
nfc_share/
  application.fam            # App(...) MENUEXTERNAL entry, appid "nfc_share"
  nfc_share.h / nfc_share.c  # app struct, scene manager + view dispatcher setup
  nfc_share_engine.h / .c    # protocol + ST25R workers, file-I/O backed
  nfc_share_i.h              # shared app-internal types (app struct, view ids)
  views/  nfc_share_progress_view.{h,c}   # shared send/receive progress view
  scenes/ nfc_share_scene_start.c         # submenu: Send / Receive
          nfc_share_scene_file_select.c   # send: dialog_file_browser over /ext
          nfc_share_scene_transfer.c      # runs the engine, shows progress
          nfc_share_scene_config.h + generated scene table
```

### Engine module (`nfc_share_engine`)

Owns the protocol constants (from the inline impl, unchanged) and both worker
loops, but takes its file bytes through an explicit I/O boundary instead of the
`s_file_content` / `s_rx_filebuf` globals. One clear job: move a file across the
NFC transport in either role; testable by swapping the I/O callbacks.

Public interface (sketch):

```c
typedef struct NfcShareEngine NfcShareEngine;

typedef enum {
    NfcShareRoleSend,     // we are the listener/card (sender)
    NfcShareRoleReceive,  // we are the poller/reader (receiver)
} NfcShareRole;

typedef enum {
    NfcShareStateInit,      // radio starting / activating
    NfcShareStateWaiting,   // send: announcing; recv: waiting for announce
    NfcShareStateRunning,   // blocks moving
    NfcShareStateDoneOk,    // complete + md5 verified
    NfcShareStateFail,      // md5 mismatch / IO error / radio error
} NfcShareState;

typedef struct {                 // snapshot for the progress view
    NfcShareState state;
    char     name[40];
    uint32_t file_size;
    uint32_t bytes_done;
    uint32_t blocks_total;
    uint32_t blocks_done;
    uint32_t kbps_x10;           // throughput *10, integer math
    uint8_t  err;                // NfcShareErr for the Fail state
} NfcShareStatus;

// Send: source file path on /ext. Receive: destination directory on /ext.
NfcShareEngine* nfc_share_engine_alloc(NfcShareRole role, const char* path);
void  nfc_share_engine_start(NfcShareEngine* e);    // spawns the worker FuriThread
void  nfc_share_engine_stop(NfcShareEngine* e);     // signals + joins the worker
void  nfc_share_engine_status(NfcShareEngine* e, NfcShareStatus* out);  // locked copy
void  nfc_share_engine_free(NfcShareEngine* e);
```

Internally the engine keeps the proven worker bodies (listener/poller loops) and
replaces the hardcoded buffers with a `Storage`/`File` handle:

- **Send:** open source read-only, `storage_file_size()` for size; compute md5 by
  streaming the file in `NSH_DATA_LENGTH`-aligned chunks; `build_data_frame(block)`
  seeks `block*240`, reads ≤240 bytes, zero-pads the tail.
- **Receive:** on ANNOUNCE, resolve a non-clobbering destination path (below),
  `storage_file_open(... WRITE|CREATE)`; each accepted DATA block seeks
  `block*240` and writes its valid bytes; the block bitmap (RAM,
  `ceil(blocks/8)` bytes) still drives completeness + gap re-requests; on the
  last block, close, reopen read-only, stream-md5, compare to the announced hash.

The worker still runs in a dedicated `furi_thread_alloc_ex(...,4096,...)` — the
FWT window (~74 ms) is far tighter than any GUI tick. The engine exposes only a
locked status snapshot to the GUI; no view-model coupling inside the engine.

### Scenes / views

- **start:** `Submenu` — "Send a file" → file_select; "Receive a file" →
  transfer (receive role).
- **file_select:** `dialog_file_browser_show` rooted at `/ext`, no extension
  filter (share anything: `.sub`, `.nfc`, `.txt`, images, …). On pick → transfer
  (send role, chosen path). Pattern mirrors `bad_usb_scene_file_select.c`.
- **transfer:** allocates + starts the engine for the role, polls
  `nfc_share_engine_status()` on the GUI tick to update the progress view; Back
  stops+joins the engine and returns. Shared progress view renders both roles
  (role-appropriate labels), a progress bar, `bytes/size`, `KB/s`, and a final
  "Done — md5 OK" / "Failed" line. A small debug line (last frame hdr/len) stays
  behind a compile flag, replacing the current always-on hex dump.

## Received-file destination

`/ext/nfc_share/` (created if absent). Sanitize the announced name to a leaf
filename (strip any path separators). If `<name>` exists, append `_1`, `_2`, …
before the extension until free — never overwrite existing SD data.

## Larger files

Streaming both directions removes the 64 KB RAM cap. Per-transfer RAM is just the
block bitmap: `ceil(file_size / 240 / 8)` bytes (≈550 B for 1 MB, ≈5.5 KB for
10 MB) plus small fixed frame buffers — safe on the no-PSRAM Cardputer. Practical
ceiling is SD space and patience (throughput is well under the Flipper's 7 KB/s),
not RAM. A defensive upper bound (e.g. reject announced sizes > 16 MB) guards the
bitmap allocation.

## Protocol (reference — unchanged from Phase 3)

- Transport frame `[tp_hdr(1)][nsh packet]`; `tp_hdr` 0x00 keep-alive, 0x01 packet.
- nsh packet `[version=2][tx_id][type][payload][crc16 LE]`; crc = CCITT-FALSE
  (0x1021, init 0xFFFF) over `[version..payload]`.
- ANNOUNCE (type 1) / REQUEST (type 2) are **control packets, 56-byte payload**
  (parse length by TYPE, not frame length). ANNOUNCE = name[36]+size(u32 LE)+
  md5[16]. REQUEST = range_start(u32)+range_end(u32) in **bytes**, rest padded.
- DATA (type 3) = block_number(u32 LE)+data[240]; offset = block*240.
- RX frames arrive with the 2-byte transport CRC-A still attached — strip before
  nsh parse. `listen_tx` TXE wait stays at 50 ms (needed for the 250 B DATA frame).
- Receiver requests periodically (not every exchange); the sender streams DATA on
  polls, re-arms range on REQUEST.

## Ship

- `application.fam` in both trees: `App(appid="nfc_share",
  apptype=FlipperAppType.MENUEXTERNAL, entry_point="nfc_share_app", ...)`.
- Mirror the whole `nfc_share/` dir into `src_cardputer_adv` (identical sources;
  the ST25R3916 backend + Grove pinout already exist in both trees).
- applab catalog entry (`const APPS` in the applab `index.html`) after the app
  passes on hardware.

## Non-goals

- No protocol/wire changes; no ST25R3916 driver changes.
- Still **M5 Unit NFC (ST25R3916) on both ends** (RFID2/WS1850S is reader-only).
- No resume-across-sessions, no multi-file batch, no encryption — single file,
  single session, matching lomalkin's app.
- The `bus_tools` ST25R test scene is left intact as a dev harness (not the
  shipping surface); retiring it is a later optional cleanup.

## Acceptance tests (hardware-gated)

1. **Send a real file:** pick an arbitrary `/ext` file (a few KB, non-240-aligned
   size) on the M5; a Flipper running Flipper Share (receive) gets it, 100%,
   md5 OK, correct name + byte-identical content.
2. **Receive to SD:** M5 in receive pulls a multi-block file from a Flipper
   (send); file lands at `/ext/nfc_share/<name>`, md5 OK, re-open + byte-compare
   matches the source.
3. **Larger file:** a ≥100 KB file completes both directions with md5 OK
   (proves streaming + no RAM-cap regression), including on the Cardputer.
4. **Name collision:** receiving a name that already exists writes `<name>_1`
   without touching the original.
5. Builds clean and flashes on both M5Stick S3 and Cardputer ADV.
