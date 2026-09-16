#!/usr/bin/env python3
"""
Signal-ID fingerprint miner + reference classifier + C-table generator.

Ground truth in = decoder source out. For every protocol in lib/subghz/protocols
this mines the (te_short, te_long, bit-count, modulation, bands) straight from the
decoder's SubGhzBlockConst and protocol .flag line, so the fingerprint DB can
never silently drift from the firmware. It then runs a self-test suite over the
mined DB (real execution — the Windows box has no native C host compiler, only the
xtensa cross-compiler, so the *algorithm* is verified here in Python and mirrored
line-for-line in signal_fingerprint.c) and emits the C table include.

Usage:
    python tools/signal_id/build_fingerprints.py            # mine + test
    python tools/signal_id/build_fingerprints.py --emit-c   # + write .inc/.json
"""
import os
import re
import sys
import json

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PROTO_DIR = os.path.join(ROOT, "lib", "subghz", "protocols")

# Makes we recognise from a protocol/name string -> canonical make label.
MAKE_HINTS = [
    # Specific / aftermarket makes FIRST — the match loop breaks on first hit, and a
    # broad hint below would otherwise shadow these ("audi" is a substring of
    # "audiovox"; "star" of "compustar"/"astrostart").
    ("compustar", "Compustar (remote start)"),
    ("astrostart", "Astrostart (remote start)"),
    ("audiovox", "Audiovox (remote start)"),
    ("gm_rke", "GM"), ("continental", "Continental (RKE)"),
    ("nidec", "Nidec (RKE)"), ("alps", "Alps (RKE)"),
    ("ldv", "LDV"), ("jeep", "Jeep"), ("lexus", "Lexus"),
    ("opel", "Opel"), ("peugot", "Peugeot"), ("ducato", "Fiat"),
    ("chrysler", "Chrysler"), ("fiat", "Fiat"), ("ford", "Ford"),
    ("honda", "Honda"), ("kia", "Kia"), ("hyundai", "Hyundai"),
    ("mazda", "Mazda"), ("mitsubishi", "Mitsubishi"), ("porsche", "Porsche"),
    ("psa", "PSA"), ("renault", "Renault"), ("subaru", "Subaru"),
    ("vag", "VAG/VW"), ("audi", "Audi"), ("bmw", "BMW"),
    ("chevrolet", "Chevrolet"), ("land_rover", "Land Rover"),
    ("scher", "Scher-Khan (alarm)"), ("star", "StarLine (alarm)"),
    ("toyota", "Toyota"), ("nissan", "Nissan"), ("suzuki", "Suzuki"),
]

# Rolling / crypto signal families -> a note. Presence of these tokens in the
# decoder marks the frame as carrying a rolling authenticator.
ROLLING_TOKENS = ("hop", "rolling", "keeloq", "hitag", "aut64", "cnt", "counter", "->cnt")

FLAG_BANDS = {  # SubGhzProtocolFlag_* -> nominal centre freq (Hz)
    "315": 315_000_000, "390": 390_000_000, "433": 433_920_000,
    "868": 868_350_000, "915": 915_000_000,
}
BAND_TOL_HZ = 3_000_000  # +/- 3 MHz counts as "in band"


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def resolve_name(macro, texts):
    """Resolve a NAME macro like KIA_PROTOCOL_V7_NAME -> its "Kia V7" string."""
    if macro.startswith('"'):
        return macro.strip('"')
    pat = re.compile(r'#define\s+' + re.escape(macro) + r'\s+"([^"]+)"')
    for t in texts:
        m = pat.search(t)
        if m:
            return m.group(1)
    return None


def parse_const_block(text):
    """Return (te_short, te_long, min_bits) from the first SubGhzBlockConst."""
    m = re.search(r'SubGhzBlockConst\s+\w+\s*=\s*\{(.*?)\};', text, re.DOTALL)
    if not m:
        return None
    body = m.group(1)

    def field(name):
        fm = re.search(r'\.' + name + r'\s*=\s*([A-Za-z0-9_]+)', body)
        if not fm:
            return None
        tok = fm.group(1)
        if tok.isdigit():
            return int(tok)
        # a macro like KIA_V7_KEY_BITS or FIAT_V2_TE_SHORT -> resolve in text
        dm = re.search(r'#define\s+' + re.escape(tok) + r'\s+(\d+)', text)
        return int(dm.group(1)) if dm else None

    return field("te_short"), field("te_long"), field("min_count_bit_for_found")


def parse_protocol_struct(text):
    """Return (name_macro, flag_line) for the const SubGhzProtocol definition."""
    m = re.search(
        r'const\s+SubGhzProtocol\s+\w+\s*=\s*\{(.*?)\};', text, re.DOTALL)
    if not m:
        return None, None
    body = m.group(1)
    nm = re.search(r'\.name\s*=\s*([A-Za-z0-9_"]+)', body)
    fm = re.search(r'\.flag\s*=\s*([^\;]*?)(?:,\s*)?\n\s*(?:#if|\.type|\.decoder|\.encoder)',
                   body, re.DOTALL)
    if not fm:
        fm = re.search(r'\.flag\s*=\s*([^;]*)', body)
    return (nm.group(1) if nm else None,
            fm.group(1) if fm else "")


def mine_file(path):
    text = read(path)
    base = os.path.basename(path)
    stem = base[:-2]
    hdr = os.path.join(PROTO_DIR, stem + ".h")
    texts = [text] + ([read(hdr)] if os.path.exists(hdr) else [])

    name_macro, flag = parse_protocol_struct(text)
    if not name_macro:
        return None
    name = resolve_name(name_macro, texts)
    if not name:
        return None

    const = parse_const_block(text)
    if not const or const[0] is None or const[1] is None:
        return None
    te_short, te_long, min_bits = const

    mod = "FM" if "SubGhzProtocolFlag_FM" in flag else (
        "AM" if "SubGhzProtocolFlag_AM" in flag else "UNK")
    bands = [hz for tok, hz in FLAG_BANDS.items()
             if f"SubGhzProtocolFlag_{tok}" in flag]
    tx = bool(re.search(
        r'\.alloc\s*=\s*(?:subghz|kia|honda)_protocol_encoder_\w+', text))
    low = text.lower()
    rolling = ("SubGhzProtocolTypeDynamic" in text) or any(
        tok in low for tok in ROLLING_TOKENS)

    make = None
    key = (name + " " + stem).lower()
    for hint, label in MAKE_HINTS:
        if hint in key:
            make = label
            break

    return {
        "name": name, "file": base, "mod": mod, "bands": bands or [433_920_000],
        "te_short": te_short, "te_long": te_long,
        "bits": min_bits or 0, "tx": tx, "rolling": rolling,
        "make": make, "automotive": make is not None,
    }


def mine_all():
    db = []
    skipped = []
    for f in sorted(os.listdir(PROTO_DIR)):
        if not f.endswith(".c"):
            continue
        try:
            entry = mine_file(os.path.join(PROTO_DIR, f))
        except Exception as e:  # pragma: no cover
            skipped.append((f, repr(e)))
            continue
        if entry:
            db.append(entry)
        else:
            skipped.append((f, "no parseable protocol/const"))
    return db, skipped


# ---------------------------------------------------------------------------
# Reference classifier — mirrored line-for-line in signal_fingerprint.c
# ---------------------------------------------------------------------------
def in_any_band(freq, bands):
    return any(abs(freq - b) <= BAND_TOL_HZ for b in bands)


def ratio_score(obs, ref):
    if obs <= 0 or ref <= 0:
        return 0.0
    r = obs / ref if obs < ref else ref / obs   # 0..1, 1 == identical
    # within 8% -> ~full, degrade to 0 by 40% off
    if r >= 0.92:
        return 1.0
    if r <= 0.60:
        return 0.0
    return (r - 0.60) / (0.92 - 0.60)


def bits_score(obs, ref):
    if ref <= 0 or obs <= 0:
        return 0.5   # neutral when unknown
    d = abs(obs - ref)
    if d == 0:
        return 1.0
    if d <= 2:
        return 0.8
    if d <= 6:
        return 0.5
    if d <= 12:
        return 0.2
    return 0.0


def action_for(entry, confident):
    if not confident:
        return "capture"
    if entry.get("make") and ("alarm" in (entry["make"] or "").lower()):
        return "defend"          # alarm systems -> often a nuisance/jam target
    if entry["rolling"] and entry["tx"]:
        return "attack"          # rolling + we can TX -> red-team surface
    return "explain"


TIE_DELTA = 4       # candidates within this many points of the top are "tied"


def classify(obs, db, top_n=3):
    """obs = dict(freq, mod, bits, te_short, te_long[, name]).

    Many car fobs share identical timing, so a signature can legitimately match
    several protocols equally well. When it does we return them all, tagged
    `ambiguous` — a decoded protocol name (authoritative) or more capture breaks
    the tie. We never silently pick one of an equal set and call it certain.
    """
    # Authoritative path: a decoder already matched -> that name is ground truth.
    # Fingerprinting only enriches it (make/model/action) and lists neighbours;
    # it must not "outvote" a real decode with a same-timing rival.
    if obs.get("name"):
        auth = next((e for e in db if e["name"] == obs["name"]), None)
        if auth:
            head = {"name": auth["name"], "make": auth["make"], "score": 100,
                    "rolling": auth["rolling"], "tx": auth["tx"],
                    "action": action_for(auth, True), "confident": True,
                    "ambiguous": False}
            return [head]

    scored = []
    for e in db:
        if obs["mod"] != "UNK" and e["mod"] != "UNK" and obs["mod"] != e["mod"]:
            continue
        if not in_any_band(obs["freq"], e["bands"]):
            continue
        s = (0.45 * ratio_score(obs["te_short"], e["te_short"]) +
             0.25 * ratio_score(obs["te_long"], e["te_long"]) +
             0.30 * bits_score(obs["bits"], e["bits"]))
        scored.append((round(s * 100), e))
    scored.sort(key=lambda x: -x[0])
    if not scored or scored[0][0] < 40:
        return [{"name": "UNKNOWN", "make": None,
                 "score": scored[0][0] if scored else 0, "rolling": False,
                 "tx": False, "action": "capture", "confident": False,
                 "ambiguous": False}]
    top = scored[0][0]
    tied = [(sc, e) for sc, e in scored if top - sc <= TIE_DELTA]
    ambiguous = len(tied) > 1
    out = tied[:top_n] if ambiguous else scored[:top_n]
    results = []
    for score, e in out:
        confident = score >= 55 and not ambiguous
        results.append({
            "name": e["name"], "make": e["make"], "score": score,
            "rolling": e["rolling"], "tx": e["tx"],
            "action": action_for(e, confident),
            "confident": confident, "ambiguous": ambiguous,
        })
    return results


# ---------------------------------------------------------------------------
# Self-tests (real execution)
# ---------------------------------------------------------------------------
def run_tests(db):
    fails = []
    auto = [e for e in db if e["automotive"] and e["bits"] > 0]

    # (a) each protocol's own signature returns it in the top tied group (some
    #     share identical timing -> legitimately ambiguous, which is correct).
    for e in auto:
        obs = {"freq": e["bands"][0], "mod": e["mod"], "bits": e["bits"],
               "te_short": e["te_short"], "te_long": e["te_long"]}
        res = classify(obs, db, top_n=12)   # wide, so genuine >3-way ties show
        top = res[0]["score"]
        names = {r["name"] for r in res if r["score"] == top}
        if e["name"] not in names:
            fails.append(f"[self] {e['name']} not in top group {sorted(names)} (score {top})")

    # (a2) with the decoded name supplied, the tie must collapse to a confident
    #      unique answer (the authoritative-name path).
    for e in auto:
        obs = {"freq": e["bands"][0], "mod": e["mod"], "bits": e["bits"],
               "te_short": e["te_short"], "te_long": e["te_long"], "name": e["name"]}
        res = classify(obs, db)
        if res[0]["name"] != e["name"] or not res[0]["confident"]:
            fails.append(f"[named] {e['name']} -> {res[0]['name']} conf={res[0]['confident']}")

    # (b) perturbed (+/-8% timing, +/-2 bits) still ranks the right make in the
    #     top tied group.
    for e in auto:
        obs = {"freq": e["bands"][0], "mod": e["mod"], "bits": e["bits"] + 2,
               "te_short": int(e["te_short"] * 1.08),
               "te_long": int(e["te_long"] * 0.92)}
        res = classify(obs, db, top_n=12)
        top = res[0]["score"]
        top_makes = {r.get("make") for r in res if r["score"] == top}
        top_names = {r["name"] for r in res if r["score"] == top}
        if e["make"] and e["make"] not in top_makes and e["name"] not in top_names:
            fails.append(f"[pert] {e['name']} ({e['make']}) -> top makes {sorted(m for m in top_makes if m)}")

    # (c) off-band / wrong-modulation -> UNKNOWN, no false make
    for e in auto[:15]:
        obs = {"freq": 100_000_000, "mod": e["mod"], "bits": e["bits"],
               "te_short": e["te_short"], "te_long": e["te_long"]}
        res = classify(obs, db)
        if res[0]["name"] != "UNKNOWN":
            fails.append(f"[oob] off-band {e['name']} -> {res[0]['name']} (should be UNKNOWN)")
    return fails


# ---------------------------------------------------------------------------
# C table emitter
# ---------------------------------------------------------------------------
def emit_c(db):
    auto = [e for e in db if e["automotive"] and e["bits"] > 0]
    auto.sort(key=lambda e: e["name"])
    lines = [
        "// AUTO-GENERATED by tools/signal_id/build_fingerprints.py — do not edit.",
        "// Mined from lib/subghz/protocols decoder constants (ground truth).",
        f"// {len(auto)} automotive fingerprints.",
        "",
        "static const SignalFingerprint k_signal_fingerprints[] = {",
    ]
    for e in auto:
        bands = e["bands"] + [0, 0, 0]
        b0, b1, b2 = bands[0], bands[1], bands[2]
        mod = {"AM": "SIG_MOD_AM", "FM": "SIG_MOD_FM"}.get(e["mod"], "SIG_MOD_UNK")
        make = ('"%s"' % e["make"]) if e["make"] else "NULL"
        flags = []
        if e["rolling"]:
            flags.append("SIG_F_ROLLING")
        if e["tx"]:
            flags.append("SIG_F_TX")
        flagstr = " | ".join(flags) if flags else "0"
        lines.append(
            '    {"%s", %s, %s, %d, %d, %d, %uU, %uU, %uU, %s},' % (
                e["name"], make, mod, e["bits"], e["te_short"], e["te_long"],
                b0, b1, b2, flagstr))
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    db, skipped = mine_all()
    auto = [e for e in db if e["automotive"]]
    print(f"Mined {len(db)} protocols total, {len(auto)} automotive.")
    fails = run_tests(db)
    if fails:
        print(f"\nSELF-TEST FAILURES ({len(fails)}):")
        for f in fails:
            print("  " + f)
    else:
        print("SELF-TESTS PASSED (self / perturbed / off-band).")

    if "--demo" in sys.argv:
        print("\n=== DEMO: what the engine returns ===")
        demos = [
            ("Raw 433 AM, 64-bit, 250/500us (no decode yet)",
             {"freq": 433_920_000, "mod": "AM", "bits": 64, "te_short": 250, "te_long": 500}),
            ("Same capture, decoder already said 'Kia V7'",
             {"freq": 433_920_000, "mod": "FM", "bits": 64, "te_short": 250, "te_long": 500, "name": "Kia V7"}),
            ("Raw 315 AM, 18-bit, 600/4000us",
             {"freq": 315_000_000, "mod": "AM", "bits": 18, "te_short": 600, "te_long": 4000}),
            ("Off-band 100 MHz noise",
             {"freq": 100_000_000, "mod": "AM", "bits": 64, "te_short": 250, "te_long": 500}),
        ]
        for label, obs in demos:
            res = classify(obs, db, top_n=4)
            print(f"\n> {label}")
            for r in res:
                tag = "AMBIGUOUS" if r.get("ambiguous") else ("confident" if r["confident"] else "guess")
                print(f"    {r['score']:3d}  {r['name']:<16} {str(r['make'] or '-'):<18}"
                      f" [{r['action']}] {tag}"
                      f"{' rolling' if r['rolling'] else ''}{' tx' if r['tx'] else ''}")

    if "--emit-c" in sys.argv:
        with open(os.path.join(HERE, "fingerprints.json"), "w") as fh:
            json.dump(auto, fh, indent=2)
        # Emit into tools/signal_id (NOT the firmware source glob) so the prototype
        # stays out of the shipping build until reviewed. Move to lib/subghz when wiring.
        inc = os.path.join(HERE, "signal_fingerprint_db.inc")
        with open(inc, "w") as fh:
            fh.write(emit_c(db))
        print(f"Wrote fingerprints.json and {inc}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
