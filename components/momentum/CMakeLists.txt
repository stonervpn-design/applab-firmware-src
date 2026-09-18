# Firmware-side Momentum core (settings global). Makes `momentum_settings`
# a firmware-wide global so GUI services (menu style, etc.) can read it, loaded
# from the same settings file the momentum_app FAP writes.
#   <momentum/momentum.h> -> momentum/momentum.h   (INCLUDE_DIRS ".")
#   <rgb_backlight.h>      -> rgb_backlight.h       (stubbed on this board)
idf_component_register(
    SRCS
        "momentum/settings.c"
        "rgb_backlight_stub.c"
    INCLUDE_DIRS
        "."
        "momentum"
    REQUIRES
        furi
        furi_hal
        storage
        flipper_format
        toolbox
    # momentum_settings is a firmware-wide global read by the GUI menu
    # (components/gui/modules/menu.c). menu_alloc is pulled into the link late,
    # after the last libmomentum.a occurrence, so the default 3x link-multiplicity
    # can't resolve it (no --start-group). WHOLE_ARCHIVE forces settings.c into the
    # image unconditionally, so the global links regardless of order / app set.
    WHOLE_ARCHIVE
)
