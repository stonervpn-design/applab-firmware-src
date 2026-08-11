#pragma once
#include <stdint.h>

/* Known wireless-doorbell codes (Princeton / PT2262-family, 24-bit, OOK).
 * Extracted from the FlipperZero-Subghz-DB "Doorbells" set — each is a real
 * decoded chime code, transmitted with correct Princeton framing (PWM symbols +
 * sync gap) so an actual bell decodes and rings. This is the "universal
 * doorbell" set: any bell whose code is here will chime when the sweep hits it.
 *
 * NOTE the de Bruijn approach this replaced could NOT ring these — Princeton is
 * PWM-framed, not raw NRZ. See doorbell-brute-app memory. */

typedef struct {
    uint32_t freq; /* Hz */
    uint32_t key; /* 24-bit Princeton code */
    uint16_t te; /* short-pulse period, microseconds */
    const char* name; /* short label for the status view */
} DoorbellCode;

/* 38 Princeton doorbell codes (from FlipperZero-Subghz-DB). */
static const DoorbellCode kDoorbellCodes[] = {
    {433920000u, 0x005455u, 355u, "intertechnoMLR7"},
    {433920000u, 0x015455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x045455u, 355u, "intertechnoMLR7"},
    {433920000u, 0x055455u, 355u, "intertechnoMLR7"},
    {433920000u, 0x105455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x115455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x145455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x155455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x172962u, 303u, "Doorbell3"},
    {433920000u, 0x29FDDAu, 323u, "Action"},
    {433920000u, 0x2A6C38u, 203u, "Doorbell4"},
    {433920000u, 0x361F08u, 281u, "ConstructionMar"},
    {433920000u, 0x405455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x415455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x438022u, 303u, "M520WirelessDoo"},
    {433920000u, 0x445455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x455455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x47B132u, 302u, "M520WirelessDoo"},
    {433920000u, 0x505455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x514155u, 300u, "Mydoorway"},
    {433920000u, 0x515455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x545455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x553C08u, 285u, "ConstructionMar"},
    {433920000u, 0x555455u, 356u, "intertechnoMLR7"},
    {433920000u, 0x592A23u, 240u, "ConstructionMar"},
    {433920000u, 0x5D90B8u, 214u, "GE2MelodyDoorbe"},
    {433920000u, 0x619558u, 187u, "HardHead"},
    {433920000u, 0x6859C3u, 196u, "AreTech"},
    {433920000u, 0x7D1504u, 182u, "Doorbell0"},
    {433920000u, 0x82C313u, 230u, "ConstructionMar"},
    {433920000u, 0x919AC3u, 183u, "ConstructionMar"},
    {433920000u, 0x9A25A8u, 203u, "Doorbell1"},
    {433920000u, 0xC7170Cu, 287u, "DollarGeneralPr"},
    {433920000u, 0xDAB1F8u, 193u, "ConstructionMar"},
    {433920000u, 0xE0CEF3u, 241u, "ConstructionMar"},
    {433920000u, 0xEE070Bu, 423u, "Doorbell5"},
    {433920000u, 0xEF3838u, 198u, "AvidsenDoorBell"},
    {433920000u, 0xF15508u, 285u, "ConstructionMar"},
};

#define DOORBELL_CODE_COUNT (sizeof(kDoorbellCodes) / sizeof(kDoorbellCodes[0]))
