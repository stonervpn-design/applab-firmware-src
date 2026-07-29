// Smart channel engine: score channels by AP/client/recent-catch, dwell longer on
// hot ones, epsilon-explore the rest. Pure logic — mirrored by tests/fg_oracle.py.
#pragma once

#include "fg_types.h"

void fg_ai_init(FgAi* ai, uint32_t seed);
// Rebuild ap[]/cl[] counts from the current net + client tables.
void fg_ai_recount(FgAi* ai, const FgNetTable* nets, const FgClient* clients, uint8_t client_count);
void fg_ai_reward(FgAi* ai, uint8_t channel); // a catch on this channel
void fg_ai_decay(FgAi* ai); // periodic decay of recent-catch scores
uint8_t fg_ai_next_channel(FgAi* ai); // weighted pick 1..13
uint16_t fg_ai_dwell_ms(const FgAi* ai, uint8_t channel);
