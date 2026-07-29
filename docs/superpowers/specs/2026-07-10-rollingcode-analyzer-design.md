# Rolling-Code Analyzer + KeeLoq Decode Expansion — Design Spec

**Date:** 2026-07-10
**Status:** Approved (design confirmed).

## Goal

Get more out of captured **dynamic (rolling-code)** Sub-GHz signals:
1. **Part A — Rolling-Code Analyzer view:** a structured, read-only breakdown of a
   captured dynamic signal (protocol, manufacturer, serial, button, counter,
   bit-length) with a plain-language verdict, instead of the single decoded text
   line the receiver-info screen shows today.
2. **Part B — KeeLoq decode enablement:** ensure the **plaintext**
   `keeloq_mfcodes_user` (Encryption: 0) manufacturer-key path is fully wired and
   used by the decoder, and make it trivial to drop in a full public key list —
   widening which KeeLoq fobs decode to serial/counter/button (which feeds Part A).

## Context / Constraints (why this shape)

- Our `furi_hal_crypto_decrypt` is a **stub** (`memcpy` passthrough) — the ESP32
  has no Flipper secure-enclave key, so the AES-encrypted `keeloq_mfcodes` is
  unusable, and implementing AES wouldn't help (that file is encrypted with the
  Flipper's per-device secret we don't have). The **only** viable path to wider
  KeeLoq decode is a bigger **plaintext** keystore (`Encryption: 0`), which the
  loader already supports.
- `lib/subghz/protocols/keeloq.c` already iterates keystore manufacturer keys and,
  on a match, decodes serial/counter/button/manufacturer. Today that result is
  rendered as one text string on the receiver-info screen. The receiver list
  already flags dynamic vs static vs raw with distinct icons.
- **Key values are real cryptographic data and will NOT be fabricated.** Part B's
  engineering deliverable is the *path + tooling + docs*; the actual manufacturer
  keys are a user-supplied/public data drop-in.

## Part A — Rolling-Code Analyzer view

### Entry point
From the Sub-GHz **receiver-info** scene (`subghz_scene_receiver_info.c`), which
already shows a captured signal with a widget + button callbacks (Center=TX,
Right=Save). Add an **"Analyze"** affordance available when the captured protocol
is **dynamic** (`SubGhzProtocolTypeDynamic`) — a left/secondary button that opens
the analyzer scene. Static/raw/unknown captures don't show it (nothing rolling to
analyze).

### The analyzer scene/view
A new read-only scene (`subghz_scene_rolling_analyzer.c`) + a widget/text view
showing, for the captured dynamic signal:
- **Protocol** (e.g. KeeLoq, Security+ 2.0, CAME Atomo, …)
- **Manufacturer** (from the matched key, or "unknown key" if none matched)
- **Serial** (hex)
- **Button** (code + name if known)
- **Counter** (hex + decimal)
- **Bits** (min_count_bit_for_found / actual)
- **Verdict** — plain language, derived from decode state:
  - decoded: *"Rolling code, counter 0x00A3. A raw replay is rejected by the
    receiver; retransmit advances the counter (see TX)."*
  - dynamic but no key matched: *"Rolling code — manufacturer key unknown, counter
    not decodable. Fixed/serial portion shown; add the key to keeloq_mfcodes_user."*

### Data source
Pull the fields from the already-decoded protocol instance. The decoder exposes
its decoded data via the protocol's `get_string` / status accessors and the
`SubGhzBlockGeneric` fields (serial, cnt, btn, data_count_bit) that `keeloq.c`
and the other dynamic decoders populate. The analyzer reads those existing fields
— it does NOT re-implement decoding. Where a protocol doesn't expose a needed
field via a clean accessor, parse it from the protocol's `get_string` output
(the same string the info screen already renders) as a fallback.

## Part B — KeeLoq decode enablement

- **Verify + wire the plaintext path:** confirm `subghz_environment_load_keystore`
  loads `keeloq_mfcodes_user` (Encryption: 0) and that `keeloq.c` iterates its
  entries. It does today; the task is to confirm end-to-end (a plaintext key in
  the user file actually decodes a matching capture) and fix any gap if the user
  file isn't being loaded/merged with the (unusable) AES file.
- **Make the drop-in trivial:** expand the `keeloq_mfcodes_user.example` header
  docs (format, encryption-method codes 1–8, where to place the file on SD:
  `/ext/subghz/assets/keeloq_mfcodes_user`), and document that a full public
  plaintext keystore can be dropped there for broad coverage.
- **Optionally seed** the shipped `keeloq_mfcodes_user` with any manufacturer keys
  that are already present *in plaintext in this repo or the cloned upstream*
  (e.g. if upstream ships a populated plaintext file) — copying existing public
  data, not fabricating. If none is available in-repo, leave the file as-is with
  the improved docs and the working path.
- **No AES work** — explicitly out of scope (unusable on this hardware).

## Non-Goals (YAGNI)

- No new transmit behavior — the counter-advance-on-TX already exists.
- No AES keystore decryption (impossible on this hardware).
- No fabricated key values.
- No changes to the receiver list icons (already correct).

## Components

- New: `applications/main/subghz/scenes/subghz_scene_rolling_analyzer.c` (+ scene
  registration in the subghz scene config).
- Modify: `applications/main/subghz/scenes/subghz_scene_receiver_info.c` (add the
  Analyze button/action for dynamic captures + the custom event).
- Possibly new: a small view or reuse `widget` for the analyzer layout.
- Modify: `keeloq_mfcodes_user.example` (docs) and confirm the keystore load path.

## Data Flow

Receive → decode (existing) → receiver-info shows the signal; if dynamic, an
**Analyze** button → analyzer scene reads the decoded protocol fields → renders
structured breakdown + verdict. Part B: more plaintext keys in the keystore →
more captures decode to full fields → richer analyzer output.

## Testing

- **Compile gate:** `build_ci.bat` (`Project build complete` + `=== BUILD OK ===`).
- **On-hardware checklist:** capture a static fob → no Analyze button; capture a
  dynamic fob (e.g. a KeeLoq garage remote) → Analyze button appears → shows
  serial/counter/button/manufacturer (if a matching key is in the plaintext
  keystore) or the "key unknown" verdict otherwise; drop a known plaintext
  manufacturer key into `keeloq_mfcodes_user` → the same fob now decodes counter.

## Open Items (resolved during planning)

1. Exact accessors on `SubGhzProtocolDecoderBase` / `SubGhzBlockGeneric` for
   serial/cnt/btn vs. parsing `get_string` — determine by reading `keeloq.c`
   and `subghz_protocol_decoder_base.h` during planning.
2. Whether the receiver-info widget has a free button slot (Left) or needs a
   different affordance for "Analyze" — confirm from the widget/button layout.
3. Whether the cloned upstream or this repo contains any populated plaintext
   KeeLoq keys to seed with (copy-only, no fabrication) — check during planning.
