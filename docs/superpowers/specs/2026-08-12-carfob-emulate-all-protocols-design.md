# Extend multi-button rolling fob emulate to all car protocols (main subghz app)

**Date:** 2026-08-12
**Branch:** feat/keeloq-no-desync-clone
**App:** applications/main/subghz (the real car subsystem — NOT proto_pirate, which is excluded from the build)

## Goal
Today the multi-button rolling fob view (`views/subghz_keeloq_fob`, scene `subghz_scene_keeloq_fob.c`)
only handles `SubGhzFobKindKeeloq` (4 tiles Lock/Unlock/Trunk/Panic) and `SubGhzFobKindVag`
(3 tiles) — keeloq-family only. Extend it so **every car protocol** with an encoder + button
map (Kia V0-V7, Ford V0-V3, Honda v2-v10, Mazda, Subaru, etc. in `lib/subghz/protocols/`)
opens the same multi-button screen: title, `SN: CNT:xxxx` live counter, a button grid built
from that protocol's own buttons, each press synthesizing that button at the next counter,
transmitting via the protocol's own encoder, and advancing+persisting the rolling counter
(the no-desync jump that already works for VAG/KeeLoq).

## Approach (owner: "do it however")
**Generalize the existing view**, do not fork it. VAG/KeeLoq keep working as instances of the
general path — zero regression to the proven behavior.

## Architecture
### 1. Generic button-table provider (`helpers/subghz_fob_buttons.{c,h}`)
`typedef struct { uint8_t code; const char* name; } SubGhzFobButton;`
`const SubGhzFobButton* subghz_fob_buttons(const char* protocol_name, size_t* n);`
`bool subghz_fob_protocol_supported(const char* protocol_name);`
Backed by per-protocol tables reusing each protocol's existing button-name map
(`honda_get_button_name`, kia/ford/etc.). Host-testable (self-contained, no furi). Keeps the
existing VAG (3-tile) and KeeLoq (4-tile) layouts as two of its entries.

### 2. Generalize the view (`views/subghz_keeloq_fob.{c,h}`)
Add `subghz_keeloq_fob_set_table(instance, const SubGhzFobButton* buttons, size_t n, ...)`
alongside the existing `set_state(kind,...)`. Internally the two hardcoded `SubGhzFobKind`
layouts become table lookups via the provider, so the render/nav path is one code path.
`subghz_fob_nav` already handles N-tile navigation. Keep `set_sent` (counter refresh).

### 3. Routing (`subghz_scene_keeloq_fob.c` + whatever reaches it)
When a captured/saved car fob's protocol is `subghz_fob_protocol_supported`, route it into the
view with its button table (read Protocol/Serial/Btn/Cnt/Freq/Preset from `fff_data`). The
existing VAG/KeeLoq branches stay; a new generic branch covers the rest. Non-supported
protocols keep today's plain transmitter.

### 4. Synthesize + TX per press
On a button press: write `Btn` + the next `Cnt` into `fff_data`
(`flipper_format_update_uint32`), then TX through the standard txrx path so the protocol's
own encoder builds the frame (same as receiver_info/transmitter today — NOT the KeeLoq BFT
generator). Reuse the existing counter-jump (`cnt_start = cnt + jump`) so the first press is a
future frame the car accepts.

### 5. Counter advance + persist
After a successful TX, advance the counter and persist `Cnt` back to `fff_data`/the saved
`.sub` — exactly as the VAG/KeeLoq path already does ("the encoder advances AND persists the
counter into fff_data during subghz_tx_start"). Guard: no advance on failed TX.

## Scope / classification
Supported = car protocols in `lib/subghz/protocols/` that (a) have a real encoder that reads
an independent button field, and (b) expose an enumerable button set. A build-time audit
(Task 1) produces the authoritative supported list; protocols without a button field or with
replay-only encoders keep the plain transmitter (documented, not forced into the grid).

## Testing
- Host (`tools/decoder_host_test/` harness, kept from the reverted work): per-protocol per-button
  synthesize→decode round-trip — encode (serial, cnt, btn) via the real encoder, decode, assert
  btn matches + counter advances. Plus `subghz_fob_buttons` lookup unit test.
- Build: private m5stick + cardputer green (PowerShell); VAG + generic KeeLoq still emulate
  exactly as before (no regression); public build unaffected (car gating unchanged).

## Non-goals
- No new radio/TX stack; no key changes (216 plaintext keeloq master keys already present).
- Not fixing partial-capture protocols; not touching proto_pirate.
