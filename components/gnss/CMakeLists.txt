idf_component_register(
    SRCS
        "nmea.c"
        "gnss.c"
    INCLUDE_DIRS "." ".."
    REQUIRES driver
)
