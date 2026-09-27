import re
HDR = "components/assets/assets_icons.h"
syms = re.findall(r"extern const Icon ((?:I_|A_)[A-Za-z0-9_]+);", open(HDR, encoding="utf-8").read())
# path = symbol minus the I_/A_ prefix (standard asset-pack filename convention)
rows = "".join('    {{&{s}, "{p}"}},\n'.format(s=s, p=s[2:]) for s in syms)
open("components/gui/asset_packs_icon_paths.h", "w", encoding="utf-8", newline="\n").write(
'''#pragma once
// Generated (T-Embed port): icon -> asset-pack path table for asset_packs.c.
// The base's asset pipeline doesn't emit ICON_PATHS, so we derive it from
// assets_icons.h: path = icon symbol minus its I_/A_ prefix.
#include <gui/icon.h>
#include <stddef.h>

typedef struct {
    const Icon* icon;
    const char* path;
} IconPath;

extern const IconPath ICON_PATHS[];
extern const size_t ICON_PATHS_COUNT;
''')
open("components/gui/asset_packs_icon_paths.c", "w", encoding="utf-8", newline="\n").write(
'''#include "asset_packs_icon_paths.h"
#include <assets_icons.h>
#include <core/core_defines.h>

const IconPath ICON_PATHS[] = {{
{rows}}};
const size_t ICON_PATHS_COUNT = COUNT_OF(ICON_PATHS);
'''.format(rows=rows))
print("generated %d icon path entries" % len(syms))
