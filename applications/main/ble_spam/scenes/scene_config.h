ADD_SCENE(ble_spam, main, Main)
ADD_SCENE(ble_spam, spam_menu, SpamMenu)
ADD_SCENE(ble_spam, pair_spam_custom, PairSpamCustom)
ADD_SCENE(ble_spam, running, Running)
ADD_SCENE(ble_spam, walk_scan, WalkScan)
ADD_SCENE(ble_spam, walk_services, WalkServices)
ADD_SCENE(ble_spam, walk_chars, WalkChars)
ADD_SCENE(ble_spam, walk_char_detail, WalkCharDetail)
#ifndef PUBLIC_BUILD
// BLE GATT Fuzzer + connection-flood DoS (private-only). Reachable from
// WalkScan (Right -> Target Info -> Arm DoS) and WalkCharDetail (Right ->
// Fuzz, Sweep/Stack against the connected char).
ADD_SCENE(ble_spam, walk_target_info, WalkTargetInfo)
ADD_SCENE(ble_spam, walk_fuzz_config, WalkFuzzConfig)
ADD_SCENE(ble_spam, walk_fuzz_run, WalkFuzzRun)
#endif
ADD_SCENE(ble_spam, auto_walk, AutoWalk)
ADD_SCENE(ble_spam, tracker_pick, TrackerPick)
ADD_SCENE(ble_spam, tracker_scan, TrackerScan)
ADD_SCENE(ble_spam, tracker_geiger, TrackerGeiger)
ADD_SCENE(ble_spam, race_detector, RaceDetector)
ADD_SCENE(ble_spam, whisperpair_detector, WhisperpairDetector)
