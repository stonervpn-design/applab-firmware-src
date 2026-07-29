#include "../applications/main/pin_config/pin_config_store.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // defaults match the board
    pin_config_reset_defaults();
    assert(pin_config_get(PinFuncIrTx) == 44);
    assert(pin_config_get(PinFuncIrRx) == 1);
    assert(pin_config_get(PinFuncSpiSck) == 40);
    assert(pin_config_get(PinFuncSpiMiso) == 39);
    assert(pin_config_default(PinFuncSpiCs) == 13);

    // names present
    assert(strcmp(pin_config_func_name(PinFuncIrTx), "IR TX") == 0);
    for(int f = 0; f < PinFuncCount; f++) {
        const char* n = pin_config_func_name((PinFunc)f);
        assert(n && n[0] && strcmp(n, "?") != 0);
    }

    // choices sane
    assert(pin_config_choices_count > 10);

    // set + serialize + parse round-trip
    pin_config_set(PinFuncIrTx, 10);
    pin_config_set(PinFuncSpiSck, 12);
    char buf[512];
    size_t n = pin_config_serialize(buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "ir_tx=10\n") != NULL);
    assert(strstr(buf, "spi_sck=12\n") != NULL);

    // parse from scratch restores the values (missing -> default)
    pin_config_parse(buf);
    assert(pin_config_get(PinFuncIrTx) == 10);
    assert(pin_config_get(PinFuncSpiSck) == 12);
    assert(pin_config_get(PinFuncIrRx) == 1); // was serialized as default 1

    // partial config: only one key; rest fall back to defaults
    pin_config_parse("spi_cs=7\n");
    assert(pin_config_get(PinFuncSpiCs) == 7);
    assert(pin_config_get(PinFuncIrTx) == 44); // default

    // unknown keys and junk ignored; out-of-range rejected
    pin_config_parse("garbage_line\nbogus=5\nir_tx=200\nir_rx=9\n");
    assert(pin_config_get(PinFuncIrRx) == 9);
    assert(pin_config_get(PinFuncIrTx) == 44); // 200 > 48 rejected -> stays default

    printf("test_pin_config OK\n");
    return 0;
}
