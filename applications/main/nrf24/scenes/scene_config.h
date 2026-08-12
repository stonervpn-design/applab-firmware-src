ADD_SCENE(nrf24_app, menu, Menu)
ADD_SCENE(nrf24_app, spectrum, Spectrum)
ADD_SCENE(nrf24_app, jam, Jam)
ADD_SCENE(nrf24_app, mj_menu, MjMenu)
ADD_SCENE(nrf24_app, mj_scan, MjScan)
ADD_SCENE(nrf24_app, mj_target_list, MjTargetList)
ADD_SCENE(nrf24_app, mj_script_select, MjScriptSelect)
ADD_SCENE(nrf24_app, mj_attack, MjAttack)
ADD_SCENE(nrf24_app, mj_auto, MjAuto)
#ifndef PUBLIC_BUILD
// Sniffer + Replay -- attack tooling (nrf24_capture store), private only.
ADD_SCENE(nrf24_app, sniffer, Sniffer)
#endif
