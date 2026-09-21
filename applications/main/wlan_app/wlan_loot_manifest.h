#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    char timestamp[24];
    char type[24];
    char target[48];
    char detail[80];
} WlanManifestRow;

bool wlan_manifest_parse_line(const char* line, WlanManifestRow* out);
