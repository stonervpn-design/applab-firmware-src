// car_presets.h — vehicle -> capture frequency + modulation reference DB
// Source: github.com/Carlitack/subghz-multitool-expert (adopted as reference metadata).
// Purpose: pick the correct frequency + modulation BEFORE capturing a fob, so FSK fobs are
//          not recorded with an OOK preset (the #1 cause of undecodable car captures).
// NOTE: the `mod` column maps to Flipper presets (AM650=Ook650Async, FM476=2FSKDev476Async,
//       F3/F4/Honda1=custom FSK). The `proto`/`clicks` columns are Carlitack-internal indices
//       and are NOT wired to this port's decoders — treat them as informational only.
// This is a self-contained data header; #include it from a car-picker scene to use it.

// car_presets.h — Database 200+ vehicules + barrieres
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct {
    const char* brand;
    const char* model;
    uint16_t year_start;
    uint16_t year_end;
    uint32_t frequency;
    const char* mod;
    uint8_t proto;
    uint8_t clicks;
} CarPreset;

static const CarPreset CAR_PRESETS[] = {
    {"Volvo", "S60/V70/XC70 (EU)", 2006, 2013, 433670000, "FM476", 0, 0}, // Olsson2013 EX012/2013: 2-GFSK Manchester 5.76kb; also 434.251MHz CH2; preset FSK-approx (dev NOT published, tune on HW)
    {"Volvo", "S80/XC60/XC90 (EU)", 2006, 2013, 433670000, "FM476", 0, 0}, // one-way RFR->CEM rolling-code fob; decoder=Volvo RKE (decode/ID, AES block opaque)
    {"Ford", "Fiesta", 2008, 2012, 433920000, "AM650", 0, 0},
    {"Ford", "Focus", 2004, 2011, 433920000, "AM650", 0, 0},
    {"Ford", "Mondeo", 2006, 2010, 433920000, "AM650", 0, 0},
    {"Ford", "Kuga I", 2008, 2012, 433920000, "AM650", 0, 0},
    {"Ford", "C-Max", 2003, 2010, 433920000, "AM650", 0, 0},
    {"Ford", "Galaxy", 2006, 2010, 433920000, "AM650", 0, 0},
    {"Ford", "Transit", 2006, 2014, 433920000, "AM650", 0, 0},
    {"Ford", "Explorer", 2010, 2015, 315000000, "AM650", 0, 0},
    {"Ford", "Mustang", 2008, 2016, 315000000, "AM650", 0, 0},
    {"Ford", "Focus III", 2011, 2018, 433920000, "F4", 1, 0},
    {"Ford", "Kuga II", 2012, 2019, 433920000, "F4", 1, 0},
    {"Ford", "Mondeo", 2010, 2014, 433920000, "F4", 1, 0},
    {"Ford", "Ranger", 2011, 2015, 433920000, "F4", 1, 0},
    {"Ford", "Edge (US)", 2010, 2014, 315000000, "F4", 0, 0}, // NA-only gen: 315MHz FSK (FCC CWTWB1U793)
    {"Ford", "Fusion", 2002, 2012, 433920000, "AM650", 0, 0},
    {"Kia", "Rio", 2011, 2014, 433920000, "AM650", 2, 0},
    {"Kia", "Sorento I", 2002, 2012, 433920000, "AM650", 2, 0},
    {"Kia", "Sportage", 2008, 2016, 433920000, "FM476", 3, 0},
    {"Kia", "Mohave", 2008, 2016, 433920000, "FM476", 3, 0},
    {"Kia", "Rio", 2015, 2020, 433920000, "FM476", 3, 0},
    {"Kia", "Cerato/Forte", 2003, 2019, 433920000, "FM476", 3, 0},
    {"Kia", "Ceed", 2006, 2018, 433920000, "FM476", 3, 0},
    {"Kia", "Soul", 2008, 2019, 433920000, "FM476", 3, 0},
    {"Kia", "Sorento II", 2009, 2020, 433920000, "FM476", 3, 7},
    {"Kia", "Carnival", 2020, 2021, 433920000, "FM476", 3, 7},
    {"Kia", "Optima/K5", 2010, 2021, 433920000, "FM476", 3, 0},
    {"Kia", "Stonic", 2017, 2020, 433920000, "FM476", 3, 0},
    {"Hyundai", "Solaris", 2011, 2014, 433920000, "AM650", 2, 0},
    {"Hyundai", "Accent", 2011, 2014, 433920000, "AM650", 2, 0},
    {"Hyundai", "Solaris", 2014, 2020, 433920000, "FM476", 3, 0},
    {"Hyundai", "Santa Fe", 2005, 2016, 433920000, "FM476", 3, 0},
    {"Hyundai", "IX35", 2010, 2015, 433920000, "FM476", 3, 0},
    {"Hyundai", "Tucson", 2009, 2019, 433920000, "FM476", 3, 0},
    {"Hyundai", "Elantra", 2006, 2019, 433920000, "FM476", 3, 0},
    {"Hyundai", "Sonata", 2009, 2017, 433920000, "FM476", 3, 0},
    {"Hyundai", "I30", 2007, 2017, 433920000, "FM476", 3, 0},
    {"Hyundai", "Kona", 2018, 2021, 433920000, "FM476", 3, 0},
    {"VAG (VW/Audi)", "Golf 4/5/6", 1999, 2016, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Passat B5/B6/B7", 1997, 2015, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Polo", 2000, 2014, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Jetta", 1999, 2014, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Tiguan", 2008, 2014, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Beetle", 2011, 2019, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Audi A3/S3/RS3", 1997, 2013, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Audi A4/S4/RS4", 2000, 2008, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Audi TT", 1998, 2014, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Audi R8", 2006, 2015, 434420000, "AM650", 4, 0},
    {"VAG (VW/Audi)", "Skoda Octavia", 2008, 2014, 434420000, "AM650", 4, 0},
    {"PSA", "207 Mode 1", 2009, 2015, 433920000, "AM650", 5, 0},
    {"PSA", "307 Mode 1", 2001, 2008, 433920000, "AM650", 5, 0},
    {"PSA", "208 Mode 2", 2012, 2019, 433920000, "F3", 5, 0},
    {"PSA", "308 Mode 2", 2007, 2018, 433920000, "F3", 5, 0},
    {"PSA", "3008 Mode 2", 2009, 2016, 433920000, "F3", 5, 0},
    {"PSA", "C4 Mode 2", 2010, 2018, 433920000, "F3", 5, 0},
    {"PSA", "C3 Mode 2", 2014, 2016, 433920000, "F3", 5, 0},
    {"PSA", "DS3/DS4/DS5", 2010, 2016, 433920000, "F3", 5, 0},
    {"PSA", "Partner/Jumpy", 2009, 2016, 433920000, "F3", 5, 0},
    {"Fiat", "V0 (static)", 2000, 2015, 433920000, "AM650", 6, 0},
    {"Fiat", "V1 (HITAG2)", 2005, 2015, 433920000, "AM650", 6, 0},
    {"Honda", "Accord (EU)", 2003, 2007, 433920000, "Honda1", 7, 0}, // Honda RKE is FSK
    {"Honda", "Civic (EU)", 2006, 2015, 433920000, "Honda1", 7, 0},
    {"Honda", "CR-V (EU)", 2002, 2015, 433920000, "Honda1", 7, 0},
    {"Honda", "Jazz", 2001, 2014, 433920000, "Honda1", 7, 0},
    {"Renault", "Megane III", 2009, 2016, 433920000, "AM650", 8, 0},
    {"Renault", "Laguna III", 2011, 2015, 433920000, "AM650", 8, 0},
    {"Renault", "Duster", 2011, 2014, 433920000, "AM650", 8, 0},
    {"Renault", "Logan", 2010, 2014, 433920000, "AM650", 8, 0},
    {"Renault", "Clio III", 2006, 2010, 433920000, "AM650", 8, 0},
    {"Renault", "Scenic III", 2010, 2016, 433920000, "AM650", 8, 0},
    {"Renault", "Kangoo", 2008, 2017, 433920000, "AM650", 8, 0},
    {"Subaru", "Forester", 2007, 2017, 433920000, "AM650", 9, 0},
    {"Subaru", "Impreza", 2000, 2016, 433920000, "AM650", 9, 0},
    {"Subaru", "Outback", 2000, 2014, 433920000, "AM650", 9, 0},
    {"Mazda", "Tribute", 2008, 2011, 315000000, "AM650", 10, 0},
    {"Mitsubishi", "Pajero", 2006, 2020, 433920000, "FM476", 11, 0},
    {"Mitsubishi", "L200/Triton", 2007, 2020, 433920000, "FM476", 11, 0},
    {"Chrysler", "V0", 2000, 2015, 433920000, "AM650", 12, 0},
    {"VAG (VW/Audi)", "Touareg (EU)", 2003, 2010, 433920000, "AM650", 15, 0}, // Touareg=VW, EU=433.92
    {"Porsche", "Cayenne (EU)", 2003, 2010, 433920000, "AM650", 15, 0},
    {"StarLine", "A2/A4/A6/A8/A9", 2005, 2020, 433920000, "AM650", 16, 0},
    {"StarLine", "B6/B9", 2005, 2020, 433920000, "AM650", 16, 0},
    {"Scher-Khan", "Magicar 7S-12", 2005, 2020, 433920000, "FM476", 17, 0},
    {"Came", "Top/Tam", 2000, 2024, 433920000, "AM650", 18, 0},
    {"Nice", "Flo", 2000, 2024, 433920000, "AM650", 19, 0},
    {"FAAC", "SLH/Spa", 2000, 2024, 433920000, "AM650", 20, 0},
    {"Hormann", "HSM4/Bisecur", 2000, 2024, 868000000, "AM650", 21, 0},
    // === SECURITY+ 2.0 (Chamberlain rolling code — 390 MHz!) ===
    {"Chamberlain", "Security+ 2.0", 2010, 2024, 390000000, "AM650", 0, 0},
    {"LiftMaster", "Security+ 2.0", 2010, 2024, 390000000, "AM650", 0, 0},
    // === VERIFIED from Quantum .sub files ===
    {"Suzuki", "Swift/SX-4/Vitara", 2005, 2015, 433920000, "FM476", 2, 0},  // Uses Kia V0 proto
    {"Land Rover", "Discovery 2 (EU)", 1998, 2004, 433920000, "F4", 7, 0},    // Disco2 ended 2004; Honda V2 proto
    {"Land Rover", "Freelander", 2000, 2010, 433920000, "F4", 7, 0},
    {"Jaguar", "X-Type/S-Type", 2000, 2008, 433920000, "AM650", 0, 0},        // Uses Ford V0 proto
    {"Jaguar", "XJ/XJR/XJ8", 2000, 2008, 433920000, "AM650", 0, 0},
    // Added alarms (KeeLoq-based, works with StarLine proto)
// === QUANTUM CFW 3.3 NEW ===
    // FCA Group (up to 2016)
    {"Chrysler", "300C", 2005, 2010, 315000000, "AM650", 12, 0},
    {"Chrysler", "PT Cruiser", 2000, 2010, 315000000, "AM650", 12, 0},
    {"Fiat", "500", 2007, 2016, 433920000, "AM650", 6, 0},
    {"Fiat", "Panda", 2007, 2016, 433920000, "AM650", 6, 0},
    {"Fiat", "Punto", 2007, 2016, 433920000, "AM650", 6, 0},
    // Honda extended (CVE 2022)
    {"Honda", "Civic CVE", 2016, 2022, 433920000, "Honda1", 7, 0},
    // Hyundai/Kia extended to 2026
    // Ford FM keys up to 2024 + Keyless
    // Mazda Siemens/Visteon keys
    // Opel/Nissan commercial
    // Renault extended
    // Suzuki extended to 2022
    // === US-MARKET 315 MHz VARIANTS (fact-checked 2026-09-05 vs FCC IDs) ===
    // Same models as the EU rows above, but US/JP/KR-domestic fobs transmit at
    // 315 MHz (Honda US = 313.8). Modulation is model-generation-dependent, so it
    // is kept the same as the EU counterpart -- only the region frequency changes.
    // Pick a "(US)" row for a North-American / domestic-market car.
    {"Kia", "Rio (US)", 2006, 2020, 315000000, "FM476", 2, 0},
    {"Kia", "Sportage (US)", 2008, 2016, 315000000, "FM476", 3, 0},
    {"Kia", "Soul (US)", 2010, 2019, 315000000, "FM476", 3, 0},
    {"Kia", "Forte (US)", 2010, 2018, 315000000, "FM476", 3, 0},
    {"Kia", "Optima/K5 (US)", 2011, 2020, 315000000, "FM476", 3, 0},
    {"Kia", "Sorento (US)", 2011, 2020, 315000000, "FM476", 3, 0},
    {"Kia", "Borrego (US)", 2009, 2011, 315000000, "FM476", 3, 0},
    {"Hyundai", "Accent (US)", 2012, 2017, 315000000, "FM476", 2, 0},
    {"Hyundai", "Santa Fe (US)", 2007, 2016, 315000000, "FM476", 3, 0},
    {"Hyundai", "Elantra (US)", 2007, 2016, 315000000, "FM476", 3, 0},
    {"Hyundai", "Sonata (US)", 2009, 2015, 315000000, "FM476", 3, 0},
    {"Hyundai", "Tucson (US)", 2010, 2015, 315000000, "FM476", 3, 0},
    {"Honda", "Accord (US)", 2003, 2015, 313800000, "Honda1", 7, 0},
    {"Honda", "Civic (US)", 2006, 2015, 313800000, "Honda1", 7, 0},
    {"Honda", "CR-V (US)", 2002, 2016, 313800000, "Honda1", 7, 0},
    {"Honda", "Fit (US)", 2007, 2020, 313800000, "Honda1", 7, 0},
    {"Subaru", "Forester (US)", 2007, 2017, 315000000, "AM650", 9, 0},
    {"Subaru", "Impreza (US)", 2000, 2016, 315000000, "AM650", 9, 0},
    {"Subaru", "Outback (US)", 2000, 2014, 315000000, "AM650", 9, 0},
    {"Jaguar", "X/S-Type (US)", 2000, 2008, 315000000, "AM650", 0, 0},
    {"Jaguar", "XJ8 (US)", 2000, 2008, 315000000, "AM650", 0, 0},
    {"Land Rover", "Disco 2 (US)", 1998, 2004, 315000000, "AM650", 7, 0},
    {"Land Rover", "LR2/Freelander (US)", 2008, 2015, 315000000, "AM650", 7, 0},
    {"Suzuki", "Swift/SX-4/Vitara (US)", 2005, 2015, 315000000, "AM650", 2, 0},
    {"VAG (VW/Audi)", "Touareg (US)", 2004, 2010, 315000000, "AM650", 15, 0},
    {"Porsche", "Cayenne (US)", 2003, 2010, 315000000, "AM650", 15, 0},
    {NULL, NULL, 0, 0, 0, NULL, 0, 0},
};

#define CAR_PRESETS_COUNT (sizeof(CAR_PRESETS)/sizeof(CAR_PRESETS[0]) - 1)

static inline uint8_t car_presets_by_brand(const char* brand, const CarPreset** out, uint8_t max) {
    uint8_t count = 0;
    for(uint8_t i = 0; CAR_PRESETS[i].brand && count < max; i++) {
        if(strcmp(CAR_PRESETS[i].brand, brand) == 0) {
            out[count++] = &CAR_PRESETS[i];
        }
    }
    return count;
}

// Unique brand list in table order (de-duplicated). Fills out[] up to `max`, returns count.
static inline uint8_t car_presets_unique_brands(const char** out, uint8_t max) {
    uint8_t count = 0;
    for(uint8_t i = 0; CAR_PRESETS[i].brand && count < max; i++) {
        const char* b = CAR_PRESETS[i].brand;
        uint8_t dup = 0;
        for(uint8_t j = 0; j < count; j++) {
            if(strcmp(out[j], b) == 0) {
                dup = 1;
                break;
            }
        }
        if(!dup) out[count++] = b;
    }
    return count;
}
