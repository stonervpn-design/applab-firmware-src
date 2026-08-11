# Camera Hunter — scan & connect, no network join (2026-08-10)

PRIVATE-only, authorized-audit tool. Drop the requirement to join a home network.
The app scans the air for cameras, auto-connects to cameras that have their OWN
AP and audits them (default creds / RTSP), and reports cameras it can only detect
(clients on a network it hasn't joined).

## Change from today

Current: two modes — Auto (scan air, auto-join open/cam-style hotspots, probe) and
Join (type SSID/pass → join a network → ARP-sweep the /24). The Join mode requires
joining a network. Remove that requirement.

New: one scan-driven flow, no network join. Enhanced discovery (adds promiscuous
sniff to find cameras that are clients on a network), plus the existing connect-to-
cam-AP + credential audit.

## Flow

1. **Discover (no join).**
   - **AP scan** via `wlan_hal_scan()` → `wifi_ap_record_t[]` (ssid, bssid, rssi,
     primary channel, authmode). Classify each AP as a camera by SSID keyword +
     BSSID OUI.
   - **Promiscuous sniff** via `wlan_hal_set_promiscuous(true, cb)` + channel-hop
     (`wlan_hal_set_channel` 1..13, dwell ~150-250ms). The cb parses each frame's
     addr1/2/3, dedups MACs, classifies by camera-vendor OUI. Catches client cams
     (Wyze/Ring/Reolink on a home router) that never appear as an AP.
2. **Connect + audit (kept).** For camera APs that are OPEN or crack with a
   built-in default WPA key, `wlan_hal_connect(ssid, key, bssid, channel)` to the
   CAMERA's own AP, wait for IP, then run the existing `cam_auth` audit against the
   gateway IP (camera ports 554/8554/80/81/88/8000/8080/8899/37777/34567, vendor
   fingerprint, ~21 default creds over HTTP Basic/Digest + RTSP DESCRIBE).
   Disconnect, move to the next.
3. **Report.** Two finding kinds:
   - **Audited** — connected; cred/RTSP result (as today).
   - **Detected** — found by sniff/scan, not reachable without its network: MAC /
     vendor / SSID (if any) / RSSI / channel, marked "detected".

## Classifier (new: `cam_classify.h`)

- `cam_vendor_from_oui(const uint8_t mac[6]) -> const char*` — table of camera-maker
  OUI prefixes (Hikvision, Dahua, Amcrest, Reolink, Foscam, EZVIZ, Wyze, Ring/
  Amazon, Nest/Google, Arlo/Netgear, Axis, Ubiquiti, TP-Link/Tapo, D-Link, Vivotek,
  Lorex, Swann, Tuya-module …). Returns NULL if not a known camera vendor.
- `cam_ssid_looks_like_cam(const char* ssid) -> bool` — case-insensitive keyword
  match (IPCAM, IP-CAM, CAM, CAMERA, WYZE, REOLINK, RING, FOSCAM, EZVIZ, TAPO,
  AMCREST, DAHUA, HIK, DVR, NVR, GOPRO, INSTA360, YI, V380, ONVIF …).
- A device is a "possible camera" if OUI matches OR SSID matches. Confidence =
  both > OUI > SSID.

## Structural changes

- `cam_hunter.{c,h}`: drop `CamHuntModeJoin`; a single scan flow (mode arg kept as
  a no-op/removed). Add a sniff phase (`CamPhaseSniff`) + a `CamFindingKind`
  (Audited / Detected). Grow findings table to hold detected devices too
  (`CAM_HUNT_MAX_FINDINGS` up as heap allows, e.g. 24). Reuse the existing auto-join
  + audit path for own-AP cams.
- Scenes: remove `camera_hunter_scene_join`; main scene starts the scan directly
  (no SSID entry). Results scene groups Audited vs Detected. Status view shows scan
  + sniff progress + counts.
- Keep `cam_auth.{c,h}` unchanged. Keep CSV output; add a "kind" column.

## Constraints / honesty

- A client camera on a network can be DETECTED but not audited without joining that
  network (which needs the network's key). Do not claim otherwise; mark detected-
  only clearly. Cracking/joining the cam's host network is explicitly out of scope.
- No-PSRAM RAM budget: reuse `wlan_hal_release_bt()` + `hint_skip_netif` for the
  RX-only sniff/scan phase; the netif is only created for the connect+audit of an
  own-AP cam (wlan_hal recreates it on connect).
- PRIVATE-only: stays stripped from public via `fam_config.py`. Mirror to both trees;
  private cardputer bin comes from the SUPERSET (`build_adv_only_private`), not the
  stale src_cardputer_adv mirror.

## Verification

- Builds clean both private trees.
- Classifier host-testable (OUI/SSID → vendor) — quick unit check.
- On-device: scan near known cameras; confirm own-AP cams get audited and client
  cams get detected. (User's RF acceptance test.)
