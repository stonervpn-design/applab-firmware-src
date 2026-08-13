# Red-Team Gap-Fill Program — roadmap & status

**Goal (user, 2026-08-13):** "every single red team feature possible for this type of hardware" — implemented as GAP-FILL across all attack surfaces. Build only what's genuinely MISSING, until each surface is comprehensive. **All new offensive features PRIVATE-only** (`#ifndef PUBLIC_BUILD`, off public build + webflasher) per the standing guardrail (owner password `lemon420` never given this session).

**Hardware surfaces:** Sub-GHz (CC1101), 2.4GHz/nRF24, WiFi (ESP32), BLE (ESP32), NFC/RFID (ST25R3916 + LF), IR, USB/BadUSB, CAN, GPIO/bus, LoRa.

**Method:** each gap = its own sub-project → brainstorm/verify → spec (USER APPROVES each spec) → writing-plans → subagent-driven-development (implement→review→fix loop) → fresh-log build gate (private m5stick + cardputer; public exclusion nm-scan = 0 attack symbols) → host tests → final review. Build order back-to-back, pausing only for per-spec approval.

## Inventory verdict (2026-08-13)
Tree is ALREADY ~95% complete across every surface (full Sub-GHz car/rolling/KeeLoq/TPMS/fuzzer/de-Bruijn, complete nRF24 suite, huge WiFi app, BLE spam/walk/fuzz/tracker/CVE-detectors/FindMy, deep NFC+LF+Wiegand+magic-Gen4, IR, BadUSB dual-HID, CAN Commander, ESP-NOW, LoRa). Several inventory-claimed "gaps" were FALSE (already built): SG1 de Bruijn (subbrute_debruijn.c), WF3 rogue DNS (Evil Portal DNS-hijack), NF1 Indala + magic Gen4.

## Genuine feasible gaps + status
| # | Surface | Feature | Status |
|---|---------|---------|--------|
| WF1 | WiFi | 802.11 management-frame fuzzer (malformed beacon/probe/assoc/auth) | ✅ DONE (branch feat/redteam-gapfill; 10 commits; private m5stick+cardputer green, public 0 syms, host tests pass, final review APPROVED; HW-deferred) |
| CAN1 | CAN | CAN fuzzer engine (extend can_commander: mutate ID/DLC/payload, abort+rate) | ✅ DONE (feat/redteam-gapfill; 6 commits; private m5stick+cardputer green, public 0 CAN syms via fam_config.py APPS-exclusion, host tests pass, final review APPROVED; HW-deferred). 5 modes incl. ID-sweep; honest bus-state via can_hal_stats()/can_hal_recover(); TX-rate selector. |
| USB1 | BadUSB | OS-conditional autorun (IF_OS/ELSE_OS/END_IF/SET_OS, operator-selected OS) | ✅ DONE (feat/redteam-gapfill; 5 commits; private m5stick+cardputer green, public SENTINEL_OK + 0 USB1 syms while bad_usb ITSELF ships public, host tests pass, final review APPROVED; HW-deferred). Pure block-skip state machine `ducky_os_cond`; per-line hook in ducky_parse_line; Target-OS config selector; REPEAT-directive guard; malformed IF_OS fails CLOSED. Operator-selected only (no auto-detect — no reliable USB OS fingerprint reachable). ALL `#ifndef PUBLIC_BUILD` (bad_usb is a public app). |
| WF2 | WiFi | WPS recon (rescoped from Pixie-Dust) | ✅ DONE (feat/redteam-gapfill; 3 commits; private m5stick+cardputer green, public 0 WF2 syms [scan_fill_wps_scan_ie is IDF-lib, not ours], host tests ASAN-clean, final review APPROVED; HW-deferred). Passive WPS-IE parser `wlan_wps_parse` (bounds-hardened) + `scene_wps_scan` promiscuous scan listing WPS present/locked/config-methods. Recon only (Pixie-Dust INFEASIBLE — no raw EAP-WSC in ESP-IDF). Gated. |
| ND1 | nRF24 | Drone control — Bayang first (Syma/ESB/V2x2 after HW confirms) | ✅ DONE (Bayang; feat/redteam-gapfill; 7 commits; private m5stick+cardputer green, public 0 ND1 syms, host tests ASAN-clean, final review APPROVED; **UNVERIFIED wire format — HW bind test is the real proof, user's next step**). PHY `nrf24_hw_tx_setup_ex` (1Mbps/CRC16) + pure `drone_bayang` builder + bind/control scene, all gated. On-air debug checklist in nd1-report.md (top lead: control phase TXes on bind addr, never txid-derived data-phase addr). Syma/ESB/V2x2 deferred until Bayang flies. |
| NF1b | NFC | iCLASS/Picopass reader | ⏹ SKIPPED by user (program stopped at 5 features). Feasibility VERIFIED = FEASIBLE: ST25R3916 is a real HAL driver (BOARD_HAS_ST25R3916=1 for M5Stick), raw ISO15693/NfcV transceive exposed at 3 layers (iso15693_3_poller_send_frame -> nfc_poller_trx -> furi_hal_nfc_poller_tx/rx -> st25r3916_hal_transceive), proven live by the `nfc raw` CLI; Iso13239CrcTypePicopass (0xE012) already in tree. Caveat: needs a focused Picopass PHY-framing patch (SOF/EOF + CRC-8 variant) to the ISO15693 encode/decode helpers in furi_hal_nfc.c (editable source, not HAL rewrite). Recon slice (CSN/config read + default-key block read) was the buildable v1; full loclass elite-key crack = own sub-project. Available to build later. |

## PROGRAM COMPLETE (2026-08-14)
Delivered 5 feasible red-team gap features across surfaces, all review-APPROVED + fresh-log build-gated (private m5stick + cardputer green) + public-exclusion nm-proven (0 attack symbols) + host-tested, all HW-DEFERRED: **WF1** 802.11 mgmt-frame fuzzer, **CAN1** CAN-bus fuzzer, **USB1** BadUSB OS-conditional, **WF2** WPS recon (rescoped from infeasible Pixie-Dust), **ND1** Bayang drone control (UNVERIFIED wire format — needs owner's on-air bind test). Branch `feat/redteam-gapfill`, PRIVATE, unpushed. Several inventory-claimed gaps were already built (de Bruijn, evil-portal DNS, Indala, magic Gen4). NF1b (iCLASS) verified feasible but user stopped the program. Outstanding real-world items: ND1 HW bind test (+ Syma/ESB/V2x2 extension if it flies); optional NF1b Picopass reader.

## Declared INFEASIBLE on this hardware (won't fake)
- Zigbee/ANT+ sniffing on nRF24L01+ (can't do 802.15.4)
- BLE pairing/bonding MITM (single radio can't sit in the middle)
- JTAG/SWD glitching / fault injection (needs dedicated glitch hardware/timing)

## Branch plan
One feature branch per sub-project (or a shared `feat/redteam-gapfill` — decide per feature). PRIVATE, unpushed, kept-as-is unless owner says otherwise.
