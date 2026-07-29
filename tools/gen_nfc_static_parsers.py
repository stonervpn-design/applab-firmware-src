#!/usr/bin/env python3
"""Generate the static supported-card parser registry.

Scans applications/main/nfc/plugins/supported_cards/*.c for each parser's
`<name>_plugin_ep()` entry point and emits nfc_supported_cards_static.c: a table
mapping the parser's registry name (the file basename, matching the old
`<name>_parser.fal` file name) to its entry point. Lets the parsers resolve
statically (compiled into the binary) instead of via the runtime .fal ELF loader,
which is skipped on no-PSRAM boards. Rerun after adding/removing a parser.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NFC_DIR = os.path.join(ROOT, "applications", "main", "nfc")
PARSER_DIR = os.path.join(NFC_DIR, "plugins", "supported_cards")
HELPERS_DIR = os.path.join(NFC_DIR, "helpers")
OUT_C = os.path.join(HELPERS_DIR, "nfc_supported_cards_static.c")
# Per-instance wrapper TUs for multi-instantiated parsers go here (globbed into nfc).
VARIANT_DIR = os.path.join(HELPERS_DIR, "static_parser_variants")

EP_RE = re.compile(r"\b([a-z0-9_]+_plugin_ep)\s*\(\s*(?:void)?\s*\)")

# Parsers to NOT register statically. The read-success flow tries a supported-card
# parser FIRST and, if it returns true, shows ITS output INSTEAD of the protocol
# renderer (nfc_protocol_support.c). EMV is the one protocol whose built-in renderer
# (helpers/protocol_support/emv/emv_render.c, ~544 lines: AIDs, transactions, all
# fields) is RICHER than the supported-card emv.c parser. On boards where the .fal
# loader was skipped, emv.c never ran and EMV fell back to that full renderer; letting
# it run now truncates EMV. So skip it and keep the full render. (Non-EMV protocols
# have no card-specific renderer, so their parsers add value and stay.)
SKIP_PARSERS = {"emv"}

# Parsers compiled multiple times with a compile-time protocol #define (mirrors the
# per-instance PLUGIN entries in application.fam). Each instance needs its OWN
# translation unit + a unique ep symbol, so we generate a wrapper .c per instance.
# {parser_basename: (define_name, [(instance_registry_name, define_value), ...], [localize_syms])}
# `localize_syms` = the parser's NON-static top-level symbols (functions/globals). Since
# each instance #includes the same .c into a separate TU, those symbols would multiply-
# define at link; the wrapper #define-renames them per instance so each TU is isolated
# (mirrors the old .fal-per-instance isolation) WITHOUT editing the parser source.
MULTI_INSTANCE = {
    "ndef": (
        "NDEF_PROTO",
        [
            ("ndef_ul", "NDEF_PROTO_UL"),
            ("ndef_mfc", "NDEF_PROTO_MFC"),
            ("ndef_slix", "NDEF_PROTO_SLIX"),
            ("ndef_t4t", "NDEF_PROTO_T4T"),
        ],
        ["ndef_parse_record", "ndef_parse_message", "ndef_parse_tlv"],
    ),
    "saflok": (
        "SL_PROTO",
        [
            ("saflok_mfc", "SL_PROTO_MFC"),
            ("saflok_ul", "SL_PROTO_UL"),
        ],
        ["generate_saflok_key", "saflok_parse", "CalculateCheckSum", "DecryptCard", "weekdays"],
    ),
}


def main():
    os.makedirs(VARIANT_DIR, exist_ok=True)
    entries = []  # (name, ep)

    # Multi-instance parsers: generate one wrapper TU per instance.
    for base, (define_name, instances, localize) in sorted(MULTI_INSTANCE.items()):
        with open(os.path.join(PARSER_DIR, base + ".c"), "r", encoding="utf-8", errors="replace") as f:
            eps = sorted(set(EP_RE.findall(f.read())))
        if not eps:
            print(f"ERROR: multi-instance parser {base}.c has no _plugin_ep", file=sys.stderr)
            sys.exit(1)
        base_ep = eps[0]
        for inst_name, define_value in instances:
            inst_ep = f"{inst_name}_plugin_ep"
            renames = "".join(f"#define {sym} {inst_name}_{sym}\n" for sym in localize)
            wrapper = (
                f"// AUTO-GENERATED variant of {base}.c for {define_name}={define_value}.\n"
                f"// One translation unit per instance so each gets unique symbols.\n"
                f"#define {define_name} {define_value}\n"
                f"#define {base_ep} {inst_ep}\n"
                f"{renames}"
                f'#include "../../plugins/supported_cards/{base}.c"\n'
            )
            with open(os.path.join(VARIANT_DIR, inst_name + ".c"), "w", encoding="utf-8", newline="\n") as f:
                f.write(wrapper)
            entries.append((inst_name, inst_ep))

    for fname in sorted(os.listdir(PARSER_DIR)):
        if not fname.endswith(".c"):
            continue
        base = os.path.splitext(fname)[0]
        if base in MULTI_INSTANCE:
            continue  # handled via wrappers above
        if base in SKIP_PARSERS:
            continue  # deliberately not registered (see SKIP_PARSERS)
        path = os.path.join(PARSER_DIR, fname)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        eps = sorted(set(EP_RE.findall(text)))
        if not eps:
            continue  # not a plugin (e.g. shared helper)
        if len(eps) > 1:
            print(f"WARN: {fname} has multiple eps {eps}; using first", file=sys.stderr)
        entries.append((base, eps[0]))

    entries.sort()

    lines = []
    lines.append("// AUTO-GENERATED by tools/gen_nfc_static_parsers.py -- do not edit by hand.")
    lines.append("// Static registry of supported-card parsers compiled into the binary, so they")
    lines.append("// resolve without the runtime .fal ELF loader (which no-PSRAM boards skip).")
    lines.append('#include "nfc_supported_cards_static.h"')
    lines.append('#include "../plugins/supported_cards/nfc_supported_card_plugin.h"')
    lines.append("#include <flipper_application.h>")
    lines.append("#include <furi.h>")
    lines.append("#include <string.h>")
    lines.append("")
    for _, ep in entries:
        lines.append(f"extern const FlipperAppPluginDescriptor* {ep}(void);")
    lines.append("")
    lines.append("typedef const FlipperAppPluginDescriptor* (*NfcStaticPluginEp)(void);")
    lines.append("typedef struct {")
    lines.append("    const char* name;")
    lines.append("    NfcStaticPluginEp ep;")
    lines.append("} NfcStaticParser;")
    lines.append("")
    lines.append("static const NfcStaticParser nfc_static_parsers[] = {")
    for name, ep in entries:
        lines.append(f'    {{"{name}", {ep}}},')
    lines.append("};")
    lines.append("")
    lines.append("static const NfcSupportedCardsPlugin* nfc_static_resolve(NfcStaticPluginEp ep) {")
    lines.append("    const FlipperAppPluginDescriptor* d = ep();")
    lines.append("    if(d == NULL) return NULL;")
    lines.append("    if(strcmp(d->appid, NFC_SUPPORTED_CARD_PLUGIN_APP_ID) != 0) return NULL;")
    lines.append("    if(d->ep_api_version != NFC_SUPPORTED_CARD_PLUGIN_API_VERSION) return NULL;")
    lines.append("    return d->entry_point;")
    lines.append("}")
    lines.append("")
    lines.append("size_t nfc_supported_cards_static_count(void) {")
    lines.append("    return COUNT_OF(nfc_static_parsers);")
    lines.append("}")
    lines.append("")
    lines.append(
        "const NfcSupportedCardsPlugin* nfc_supported_cards_static_get_by_index("
    )
    lines.append("    size_t index,")
    lines.append("    const char** name_out) {")
    lines.append("    if(index >= COUNT_OF(nfc_static_parsers)) return NULL;")
    lines.append("    if(name_out) *name_out = nfc_static_parsers[index].name;")
    lines.append("    return nfc_static_resolve(nfc_static_parsers[index].ep);")
    lines.append("}")
    lines.append("")
    lines.append(
        "const NfcSupportedCardsPlugin* nfc_supported_cards_static_get_by_name(const char* name) {"
    )
    lines.append("    for(size_t i = 0; i < COUNT_OF(nfc_static_parsers); i++) {")
    lines.append("        if(strcmp(nfc_static_parsers[i].name, name) == 0)")
    lines.append("            return nfc_static_resolve(nfc_static_parsers[i].ep);")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")
    lines.append("")

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"Wrote {OUT_C} with {len(entries)} parsers:")
    print("  " + " ".join(n for n, _ in entries))


if __name__ == "__main__":
    main()
