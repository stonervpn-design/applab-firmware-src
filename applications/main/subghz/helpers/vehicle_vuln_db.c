#include "vehicle_vuln_db.h"

#include <string.h>

// Ported verbatim from KAT/src/vuln_db.rs (VULN_DB, 21 entries).
// Rust &["A","B"] -> {"A","B",NULL}. Descriptions copied verbatim.

static const char* const v1_makes[] = {"Renault", NULL};
static const char* const v1_models[] = {"ZOE", NULL};

static const char* const v2_makes[] = {"Honda", NULL};
static const char* const v2_models[] = {"Civic", NULL};

static const char* const v3_makes[] = {"Honda", NULL};
static const char* const v3_models[] = {"Fit (hybrid)", "Fit Hybrid", NULL};

static const char* const v4_makes[] = {"Honda", NULL};
static const char* const v4_models[] = {"Fit", NULL};

static const char* const v5_makes[] = {"Honda", NULL};
static const char* const v5_models[] = {"City", NULL};

static const char* const v6_makes[] = {"Honda", NULL};
static const char* const v6_models[] = {"Vezel", NULL};

static const char* const v7_makes[] = {"Hyundai", NULL};
static const char* const v7_models[] = {"Elantra", NULL};

static const char* const v8_makes[] = {"Kia", NULL};
static const char* const v8_models[] = {"Cerato", "Forte", "K3", NULL};

static const char* const v9_makes[] = {"Kia", NULL};
static const char* const v9_models[] = {"Cerato", "Forte", "K3", NULL};

static const char* const v10_makes[] = {"Mazda", NULL};
static const char* const v10_models[] = {"3", NULL};

static const char* const v11_makes[] = {"Mazda", NULL};
static const char* const v11_models[] = {"2 Sedan", NULL};

static const char* const v12_makes[] = {"Mazda", NULL};
static const char* const v12_models[] = {"2 HB (facelift)", "2 HB", NULL};

static const char* const v13_makes[] = {"Mazda", NULL};
static const char* const v13_models[] = {"Cx-3", "CX-3", NULL};

static const char* const v14_makes[] = {"Mazda", NULL};
static const char* const v14_models[] = {"Cx-5", "CX-5", NULL};

static const char* const v15_makes[] = {"Nissan", NULL};
static const char* const v15_models[] = {"Latio", NULL};

static const char* const v16_makes[] = {"Nissan", NULL};
static const char* const v16_models[] = {"Sylphy", NULL};

static const char* const v17_makes[] = {"Acura", NULL};
static const char* const v17_models[] = {"TSX", NULL};

static const char* const v18_makes[] = {"Honda", NULL};
static const char* const v18_models[] = {"Accord", NULL};

static const char* const v19_makes[] = {"Honda", NULL};
static const char* const v19_models[] = {"HR-V", NULL};

static const char* const v20_makes[] = {"Honda", NULL};
static const char* const v20_models[] = {"Civic", NULL};

static const char* const v21_makes[] = {"Honda", NULL};
static const char* const v21_models[] = {"Civic", NULL};

const VehicleVuln VEHICLE_VULN_DB[] = {
    {
        .cve = "CVE-2022-38766",
        .year_start = "2020",
        .year_end = "2022",
        .makes = v1_makes,
        .models = v1_models,
        .region = "ALL",
        .description =
            "The remote keyless system on Renault ZOE 2021 vehicles sends 433.92 MHz RF signals "
            "from the same Rolling Codes set for each door-open request, which allows for a replay "
            "attack.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-38766",
    },
    {
        .cve = "CVE-2022-27254",
        .year_start = "2016",
        .year_end = "2019",
        .makes = v2_makes,
        .models = v2_models,
        .region = "ALL",
        .description =
            "The remote keyless system on Honda Civic 2018 vehicles sends the same RF signal for "
            "each door-open request, which allows for a replay attack, a related issue to "
            "CVE-2019-20626.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-27254",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2016",
        .year_end = "2018",
        .makes = v3_makes,
        .models = v3_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2018",
        .year_end = "2018",
        .makes = v4_makes,
        .models = v4_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2017",
        .year_end = "2017",
        .makes = v5_makes,
        .models = v5_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2016",
        .year_end = "2022",
        .makes = v6_makes,
        .models = v6_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2013",
        .year_end = "2015",
        .makes = v7_makes,
        .models = v7_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2016",
        .year_end = "2018",
        .makes = v8_makes,
        .models = v8_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2012",
        .year_end = "2018",
        .makes = v9_makes,
        .models = v9_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-36945",
        .year_start = "2018",
        .year_end = "2018",
        .makes = v10_makes,
        .models = v10_models,
        .region = "ALL",
        .description =
            "The RKE receiving unit on certain Mazda vehicles through 2020 allows remote attackers "
            "to perform unlock operations and force a resynchronization after capturing three "
            "consecutive valid key-fob signals over the radio, aka a RollBack attack. The attacker "
            "retains the ability to unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-36945",
    },
    {
        .cve = "CVE-2022-36945",
        .year_start = "2018",
        .year_end = "2018",
        .makes = v11_makes,
        .models = v11_models,
        .region = "ALL",
        .description =
            "The RKE receiving unit on certain Mazda vehicles through 2020 allows remote attackers "
            "to perform unlock operations and force a resynchronization after capturing three "
            "consecutive valid key-fob signals over the radio, aka a RollBack attack. The attacker "
            "retains the ability to unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-36945",
    },
    {
        .cve = "CVE-2022-36945",
        .year_start = "2020",
        .year_end = "2020",
        .makes = v12_makes,
        .models = v12_models,
        .region = "ALL",
        .description =
            "The RKE receiving unit on certain Mazda vehicles through 2020 allows remote attackers "
            "to perform unlock operations and force a resynchronization after capturing three "
            "consecutive valid key-fob signals over the radio, aka a RollBack attack. The attacker "
            "retains the ability to unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-36945",
    },
    {
        .cve = "CVE-2022-36945",
        .year_start = "2019",
        .year_end = "2019",
        .makes = v13_makes,
        .models = v13_models,
        .region = "ALL",
        .description =
            "The RKE receiving unit on certain Mazda vehicles through 2020 allows remote attackers "
            "to perform unlock operations and force a resynchronization after capturing three "
            "consecutive valid key-fob signals over the radio, aka a RollBack attack. The attacker "
            "retains the ability to unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-36945",
    },
    {
        .cve = "CVE-2022-36945",
        .year_start = "2018",
        .year_end = "2018",
        .makes = v14_makes,
        .models = v14_models,
        .region = "ALL",
        .description =
            "The RKE receiving unit on certain Mazda vehicles through 2020 allows remote attackers "
            "to perform unlock operations and force a resynchronization after capturing three "
            "consecutive valid key-fob signals over the radio, aka a RollBack attack. The attacker "
            "retains the ability to unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-36945",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2007",
        .year_end = "2012",
        .makes = v15_makes,
        .models = v15_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2022-37418",
        .year_start = "2012",
        .year_end = "2019",
        .makes = v16_makes,
        .models = v16_models,
        .region = "ALL",
        .description =
            "RKE RollBack: unlock/resync after capturing two consecutive key fob signals. Attacker "
            "can unlock indefinitely.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2022-37418",
    },
    {
        .cve = "CVE-2019-20626",
        .year_start = "2009",
        .year_end = "2009",
        .makes = v17_makes,
        .models = v17_models,
        .region = "ALL",
        .description =
            "The remote keyless system sends the same RF signal for each door-open request, which "
            "might allow a replay attack. Honda/Acura use static codes (no rolling code).",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2019-20626",
    },
    {
        .cve = "CVE-2019-20626",
        .year_start = "2016",
        .year_end = "2016",
        .makes = v18_makes,
        .models = v18_models,
        .region = "ALL",
        .description =
            "The remote keyless system sends the same RF signal for each door-open request, which "
            "might allow a replay attack. Honda/Acura use static codes (no rolling code).",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2019-20626",
    },
    {
        .cve = "CVE-2019-20626",
        .year_start = "2017",
        .year_end = "2017",
        .makes = v19_makes,
        .models = v19_models,
        .region = "ALL",
        .description =
            "The remote keyless system on Honda HR-V 2017 vehicles sends the same RF signal for "
            "each door-open request, which might allow a replay attack.",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2019-20626",
    },
    {
        .cve = "CVE-2019-20626",
        .year_start = "2018",
        .year_end = "2018",
        .makes = v20_makes,
        .models = v20_models,
        .region = "ALL",
        .description =
            "The remote keyless system sends the same RF signal for each door-open request, which "
            "might allow a replay attack. Honda/Acura use static codes (no rolling code).",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2019-20626",
    },
    {
        .cve = "CVE-2019-20626",
        .year_start = "2020",
        .year_end = "2020",
        .makes = v21_makes,
        .models = v21_models,
        .region = "ALL",
        .description =
            "The remote keyless system sends the same RF signal for each door-open request, which "
            "might allow a replay attack. Honda/Acura use static codes (no rolling code).",
        .url = "https://nvd.nist.gov/vuln/detail/CVE-2019-20626",
    },
};

const size_t VEHICLE_VULN_DB_COUNT = sizeof(VEHICLE_VULN_DB) / sizeof(VEHICLE_VULN_DB[0]);

// --- Distinct makes accessors -------------------------------------------------
// Built lazily on first call; skips the wildcard "ALL", dedupes case-sensitively.

#define VV_MAX_DISTINCT_MAKES 32

static const char* s_distinct_makes[VV_MAX_DISTINCT_MAKES];
static size_t s_distinct_count = 0;
static int s_distinct_built = 0;

static void vv_build_distinct_makes(void) {
    if(s_distinct_built) return;
    s_distinct_count = 0;
    for(size_t e = 0; e < VEHICLE_VULN_DB_COUNT; e++) {
        const char* const* makes = VEHICLE_VULN_DB[e].makes;
        if(makes == NULL) continue;
        for(size_t m = 0; makes[m] != NULL; m++) {
            const char* make = makes[m];
            if(strcmp(make, "ALL") == 0) continue;
            int found = 0;
            for(size_t d = 0; d < s_distinct_count; d++) {
                if(strcmp(s_distinct_makes[d], make) == 0) {
                    found = 1;
                    break;
                }
            }
            if(!found && s_distinct_count < VV_MAX_DISTINCT_MAKES) {
                s_distinct_makes[s_distinct_count++] = make;
            }
        }
    }
    s_distinct_built = 1;
}

size_t vehicle_vuln_make_count(void) {
    vv_build_distinct_makes();
    return s_distinct_count;
}

const char* vehicle_vuln_make(size_t i) {
    vv_build_distinct_makes();
    if(i >= s_distinct_count) return NULL;
    return s_distinct_makes[i];
}
