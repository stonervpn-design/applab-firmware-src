#include "fg_ai.h"

static uint32_t xorshift(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static uint16_t ai_score(const FgAi* ai, uint8_t ch) {
    return (uint16_t)(3 * ai->ap[ch] + 2 * ai->cl[ch] + 5 * ai->catch_recent[ch] + 1);
}

void fg_ai_init(FgAi* ai, uint32_t seed) {
    for(int i = 0; i < 14; i++) {
        ai->ap[i] = ai->cl[i] = ai->catch_recent[i] = 0;
    }
    ai->rng = seed ? seed : 1;
}

void fg_ai_recount(
    FgAi* ai,
    const FgNetTable* nets,
    const FgClient* clients,
    uint8_t client_count) {
    for(int i = 0; i < 14; i++) {
        ai->ap[i] = 0;
        ai->cl[i] = 0;
    }
    uint16_t nn = nets->count;
    for(uint16_t i = 0; i < nn; i++) {
        uint8_t ch = nets->nets[i].channel;
        if(ch >= 1 && ch <= 13) ai->ap[ch]++;
    }
    for(uint8_t i = 0; i < client_count; i++) {
        uint8_t ch = clients[i].ch;
        if(ch >= 1 && ch <= 13) ai->cl[ch]++;
    }
}

void fg_ai_reward(FgAi* ai, uint8_t channel) {
    if(channel >= 1 && channel <= 13) ai->catch_recent[channel]++;
}

void fg_ai_decay(FgAi* ai) {
    // Subtract at least 1 so a channel's score actually reaches 0 (a plain >>3
    // floors at 7 because 7>>3==0 and the residual would stick forever).
    for(int c = 0; c < 14; c++) {
        if(ai->catch_recent[c] > 0)
            ai->catch_recent[c] -= (ai->catch_recent[c] >> 3) + 1;
    }
}

uint8_t fg_ai_next_channel(FgAi* ai) {
    ai->rng = xorshift(ai->rng);
    if((ai->rng % 8) == 0) { // epsilon: explore uniformly
        ai->rng = xorshift(ai->rng);
        return (uint8_t)(1 + ai->rng % 13);
    }
    uint32_t total = 0;
    for(uint8_t c = 1; c <= 13; c++) total += ai_score(ai, c);
    ai->rng = xorshift(ai->rng);
    uint32_t r = ai->rng % total, acc = 0;
    for(uint8_t c = 1; c <= 13; c++) {
        acc += ai_score(ai, c);
        if(r < acc) return c;
    }
    return 13;
}

uint16_t fg_ai_dwell_ms(const FgAi* ai, uint8_t channel) {
    if(channel < 1 || channel > 13) return 400;
    uint16_t sc = ai_score(ai, channel);
    uint32_t d = 400u + (uint32_t)sc * 100u;
    if(d > 2000) d = 2000;
    if(sc == 1) d = 150; // score floor => dead channel, quick skip
    return (uint16_t)d;
}
