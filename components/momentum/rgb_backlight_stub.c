// T-Embed CC1101 has no SK6805 RGB backlight strip (its TFT backlight is driven
// by the port's notification service). Momentum's settings core calls into the
// rgb_backlight driver on load; provide no-op stubs so the momentum settings
// global links on this board without pulling the SK6805 HAL path.
#include "rgb_backlight.h"

void rgb_backlight_load_settings(bool enabled) {
    UNUSED(enabled);
}
void rgb_backlight_save_settings(void) {
}
void rgb_backlight_set_color(uint8_t index, const RgbColor* color) {
    UNUSED(index);
    UNUSED(color);
}
void rgb_backlight_get_color(uint8_t index, RgbColor* color) {
    UNUSED(index);
    if(color) {
        color->r = 0;
        color->g = 0;
        color->b = 0;
    }
}
void rgb_backlight_set_rainbow_mode(RGBBacklightRainbowMode rainbow_mode) {
    UNUSED(rainbow_mode);
}
RGBBacklightRainbowMode rgb_backlight_get_rainbow_mode(void) {
    return RGBBacklightRainbowModeOff;
}
void rgb_backlight_set_rainbow_speed(uint8_t rainbow_speed) {
    UNUSED(rainbow_speed);
}
uint8_t rgb_backlight_get_rainbow_speed(void) {
    return 0;
}
void rgb_backlight_set_rainbow_interval(uint32_t rainbow_interval) {
    UNUSED(rainbow_interval);
}
uint32_t rgb_backlight_get_rainbow_interval(void) {
    return 0;
}
void rgb_backlight_set_rainbow_saturation(uint8_t rainbow_saturation) {
    UNUSED(rainbow_saturation);
}
uint8_t rgb_backlight_get_rainbow_saturation(void) {
    return 0;
}
void rgb_backlight_reconfigure(bool enabled) {
    UNUSED(enabled);
}
void rgb_backlight_update(uint8_t brightness, bool forced) {
    UNUSED(brightness);
    UNUSED(forced);
}
