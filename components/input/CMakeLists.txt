# Target-specific input driver selection
if(NOT DEFINED FLIPPER_BOARD)
    set(FLIPPER_BOARD "m5stack_cardputer_adv" CACHE STRING "Target board")
endif()

set(TARGET_INPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../targets/${FLIPPER_BOARD}")

if(NOT EXISTS "${TARGET_INPUT_DIR}/target_input.c")
    message(FATAL_ERROR "No target_input.c found for board '${FLIPPER_BOARD}' at ${TARGET_INPUT_DIR}")
endif()

set(INPUT_SRCS "input.c" "${TARGET_INPUT_DIR}/target_input.c")

# Optional per-board tilt-nav (BMI270 IMU). Only boards that ship these files
# (currently m5stick_s3) get them; other boards build unchanged.
if(EXISTS "${TARGET_INPUT_DIR}/bmi270.c")
    list(APPEND INPUT_SRCS
        "${TARGET_INPUT_DIR}/bmi270.c"
        "${TARGET_INPUT_DIR}/tilt_nav.c")
endif()

idf_component_register(
    SRCS ${INPUT_SRCS}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../targets"
    REQUIRES furi
    PRIV_REQUIRES furi_hal driver log nvs_flash
)
