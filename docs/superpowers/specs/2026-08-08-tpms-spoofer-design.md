# TPMS Spoofer (from-scratch) — Design

**Date:** 2026-08-08 · **Tree:** src_m5stick_s3 · **Guardrail:** PRIVATE-only (`#ifndef PUBLIC_BUILD`)

## Goal
Transmit synthetic TPMS sensor frames (sensor ID + pressure + temperature + battery) for a chosen protocol **without first capturing a real sensor** — an authorized-research spoofer. Today you can only capture→Edit→TX; there is no from-scratch entry.

## What already exists (reuse, don't rebuild)
- **5 TX-capable TPMS protocols** (encoders present): `Ford TPMS`, `Toyota PMV-107J`, `Renault TPMS`, `Citroen TPMS`, `Schrader GG4`. Each `get_upload` builds the RF frame **from the friendly fields** (Id/Pressure/Temperature/Battery in the FFF) — no raw capture needed.
- **TPMS info view** (`views/subghz_view_tpms_info.c`): shows Pressure/Temperature/Id/Battery, with Edit / Send / Save spokes.
- **Edit scene** (`scenes/subghz_scene_tpms_edit.c`): NumberInput for pressure (x10 bar) + temperature (°C), ByteInput for ID (hex); writes back via `subghz_history_replace_tpms_payload`.
- **Burst TX is free**: TxStart sets `subghz_block_generic_global.endless_tx = true` → holding OK transmits continuously, release sends N repeats. That IS the burst/attack mode a car ECU needs.
- Data model `TPMSBlockGeneric { const char* protocol_name; uint8_t data_count_bit; uint32_t id; uint8_t battery_low; float pressure /*bar*/; float temperature /*C*/; }` + `tpms_block_generic_serialize`.
- Seeding APIs: `subghz_history_add_to_history(hist, decoder_ctx, preset)`, `subghz_history_replace_tpms_payload(hist, idx, generic)`.

## Architecture
1. **New scene `subghz_scene_tpms_spoof`** (PRIVATE): a Submenu of the 5 protocols.
2. On pick, **seed a synthetic frame**: load the protocol's decoder (`subghz_txrx_load_decoder_by_name_protocol`), set a per-protocol default preset (freq + modulation), `add_to_history`, then `replace_tpms_payload` with a default `TPMSBlockGeneric` (default ID, pressure ≈ 2.2 bar / ~32 psi, temp 20 °C, battery ok). Set `idx_menu_chosen` to the new item.
3. **Route to the existing TPMS info view** (via the receiver-info TPMS path) — the user Edits ID/pressure/temp/battery and holds OK to transmit (single tap = one burst cycle; hold = continuous). All reuse.
4. **Menu entry** "TPMS Spoofer" in the Sub-Ghz start menu, guarded `#ifndef PUBLIC_BUILD`; `ADD_SCENE(subghz, tpms_spoof, TpmsSpoof)` guarded.

Per-protocol defaults table (`helpers/tpms_spoof_presets`): name + frequency (default 433.92 MHz EU; US 315 MHz is a documented alt) + preset (FSK/2-FSK for Ford/Toyota/Renault/Citroen/Schrader per their decoder flags: all `SubGhzProtocolFlag_FM`) + a plausible default sensor ID + data_count_bit.

## Scope
IN: 5 protocols, from-scratch seed, edit fields, hold-OK burst TX, private-only.
OUT (deferred): multi-ID sweep/rotation, per-wheel 4-sensor sets, region auto-switch (freq is settable via the existing preset flow if needed).

## Testing / gates
- Pure/host: a small `tpms_spoof_default_for(protocol_index)` returning the default `TPMSBlockGeneric`+freq — host-testable table lookup (Python replica + xtensa -fsyntax-only).
- Build: private ELF has `subghz_scene_tpms_spoof_*`; public gate = 0 (gc'd) + car-free PASS + jammer/rollback still 0.
- HW (deferred, needs a car/TPMS tool): confirm a spoofed Ford/Schrader frame decodes back on a second receiver + (authorized) triggers an ECU.

## Safety
TPMS spoofing (false pressure → dashboard warnings / sensor cloning) is authorized-research only; PRIVATE build, and it inherits the existing "hold OK to transmit" explicit-action TX (no silent/auto TX).
