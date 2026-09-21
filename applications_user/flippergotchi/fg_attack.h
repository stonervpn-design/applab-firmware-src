// Worker-context attack pass on the current channel: targeted/broadcast deauth of
// not-yet-pwned APs + active clientless PMKID solicitation. Dispatched via
// wlan_hal_run_in_worker so frames go out on the correct radio channel.
#pragma once

#include "fg_app.h"

void fg_attack_run(FgApp* app); // call inside the wlan worker
