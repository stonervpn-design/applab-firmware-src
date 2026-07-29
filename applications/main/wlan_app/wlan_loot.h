#pragma once

/* Per-boot loot session grouping. Pure-evidence captures (PMKID, portal creds,
 * live creds) write into /ext/loot/<YYYYMMDD_HHMMSS>/ and are logged to
 * <session>/manifest.csv. State-stores (saved passwords, LAN cache) are NOT
 * affected. The session id is fixed at the first wlan_loot_init() of the boot and
 * the folder is created lazily on the first capture. */

/* Fix the session id + alloc the internal lock. Call once at WiFi-app startup
 * (single-threaded). No-op after the first call of the boot (so the session
 * persists across app relaunches until reboot). */
void wlan_loot_init(void);

/* "/ext/loot/<YYYYMMDD_HHMMSS>" (no trailing slash), creating /ext/loot and the
 * session folder on first call. Never NULL. */
const char* wlan_loot_session_dir(void);

/* Append one row to <session>/manifest.csv: timestamp,type,target,detail (creates
 * the manifest + header on first write). target/detail may be "" (not NULL);
 * their separators are neutralized. */
void wlan_loot_record(const char* type, const char* target, const char* detail);
