idf_component_register(
    SRCS "rfid2.c"
    INCLUDE_DIRS "." ".."
    REQUIRES driver furi_hal
)

# furi_hal_nfc.c references rfid2_* via `extern` (no #include) to avoid a component
# dependency CYCLE, relying on the rfid2 archive being anchored by the rfid2_reader
# app. On builds where rfid2_reader is dropped (e.g. lilygo_t_embed public) nothing
# else pulls rfid2.c.obj, so those extern refs go undefined at final link. Force the
# whole archive in so the NFC symbols always resolve (no-op where already linked).
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
