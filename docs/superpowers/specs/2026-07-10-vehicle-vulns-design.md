# Vehicle Vulnerability (CVE) Lookup — Design Spec

**Date:** 2026-07-10
**Status:** Approved (feature selected from options; design = the option description). Source: KAT `src/vuln_db.rs`.

## Goal

A read-only **Vehicle Vulns** reference in the Sub-GHz app: browse known
keyless-entry / RF CVEs by make → model, showing the affected year range, CVE id,
region, and a plain-language description (replay, RollBack, rolling-code reuse).
Pairs with the rolling-code analyzer + automotive fob library — a red-teamer can
look up whether a target vehicle has a documented RKE weakness.

## Scope

- Port KAT's `VULN_DB` (21 entries, static data) to a C table. No fabrication —
  copy the entries verbatim (CVE, years, makes, models, region, description, url).
- A simple browse UI: make picker → per-make CVE list. Read-only.
- **Non-goals:** no automatic RF-capture→CVE matching (our analyzer decodes RF
  serial/counter, not vehicle make/model); no online CVE fetch; no editing.

## Data model (ported to C)

```c
typedef struct {
    const char*        cve;          // "CVE-2022-38766"
    const char*        year_start;   // "2020" or "ALL"
    const char*        year_end;     // "2022" or "ALL"
    const char* const* makes;        // NULL-terminated, e.g. {"Renault", NULL}
    const char* const* models;       // NULL-terminated, e.g. {"ZOE", NULL}
    const char*        region;       // "ALL"
    const char*        description;
    const char*        url;          // NVD link
} VehicleVuln;

extern const VehicleVuln VEHICLE_VULN_DB[];
extern const size_t VEHICLE_VULN_DB_COUNT;

/* Distinct makes present in the DB, for the picker. */
size_t vehicle_vuln_make_count(void);
const char* vehicle_vuln_make(size_t i);
```
(21 entries transcribed from `KAT/src/vuln_db.rs` `VULN_DB`. Rust `&["A","B"]`
arrays → NULL-terminated C string arrays.)

## UI

Entry point: a **"Vehicle Vulns"** item added to the Sub-GHz start menu
(`subghz_scene_start` / its submenu). Two scenes:

1. **Make picker** (`subghz_scene_vuln_makes`): submenu of the distinct makes in
   the DB (Renault, Honda, Hyundai, Kia, Mazda, …). Select → go to the list scene
   for that make.
2. **Per-make list** (`subghz_scene_vuln_list`): a scrollable widget/text_box
   showing every DB entry whose `makes` includes the chosen make, each rendered as:
   ```
   <Model(s)>  <year_start>-<year_end>
   <CVE>  (<region>)
   <description>
   ```
   Back returns to the make picker.

(Two scenes keep it simple; a per-CVE detail scene is unnecessary for 21 entries.)

## Components

- New: `applications/main/subghz/helpers/vehicle_vuln_db.{c,h}` (the table +
  make accessors).
- New: `applications/main/subghz/scenes/subghz_scene_vuln_makes.c`,
  `subghz_scene_vuln_list.c` (+ scene-config registration).
- Modify: `subghz_scene_start.c` (add the menu entry + route it).

## Testing

- **Compile gate:** `build_ci.bat`.
- **On-hardware checklist:** Sub-GHz → Vehicle Vulns → make picker lists the DB
  makes → pick Honda → list shows Civic/Fit/City/Vezel entries with CVE
  ids/years/descriptions → Back works; nothing else in Sub-GHz regressed.

## Attribution / honesty

The CVE data is public (NVD) and copied verbatim from KAT (which credits
ProtoPirate/Flipper-ARF). Descriptions are the published CVE summaries. No RF
code from KAT is used.

## Open Items (resolved during planning)

1. Exact Sub-GHz start-menu insertion point + submenu/text-box view ids — read
   `subghz_scene_start.c` and a sibling text/widget scene during planning.
2. The full 21-entry data (entries 15-21 not yet transcribed here) — the
   implementer reads `KAT/src/vuln_db.rs` and transcribes all 21 verbatim.
