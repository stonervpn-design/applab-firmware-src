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
| CAN1 | CAN | CAN fuzzer engine (extend can_commander: mutate ID/DLC/payload, abort+rate) | IN PROGRESS (map done; spec drafting). NOTE: can_commander already private via fam_config.py APPS-exclusion (no in-source gate). can_hal_stats() gives REAL TWAI bus-off/error state for honest indicator. |
| USB1 | BadUSB | OS-fingerprint conditional autorun (payload branches on host OS) | TODO |
| WF2 | WiFi | WPS PIN / Pixie-Dust attack | TODO (ESP32-injection-limited; scope carefully) |
| ND1 | nRF24 | Drone/RC hijack Bayang/Syma/ESB/V2x2 from PUBLIC formats, labeled untested-vs-vectors | TODO (user opted into no-reference build) |
| NF1b | NFC | iCLASS/Picopass legacy crack | TODO (lowest priority; heavy, HW-dependent) |

## Declared INFEASIBLE on this hardware (won't fake)
- Zigbee/ANT+ sniffing on nRF24L01+ (can't do 802.15.4)
- BLE pairing/bonding MITM (single radio can't sit in the middle)
- JTAG/SWD glitching / fault injection (needs dedicated glitch hardware/timing)

## Branch plan
One feature branch per sub-project (or a shared `feat/redteam-gapfill` — decide per feature). PRIVATE, unpushed, kept-as-is unless owner says otherwise.
