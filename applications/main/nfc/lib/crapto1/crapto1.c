/*
 * crapto1.c — MIFARE Classic crypto1 key recovery (mfkey32 core)
 *
 * Vendored from the Flipper Zero "Mfkey32" application:
 *     https://github.com/noproto/FlipperMfkey  (fap/mfkey32.c)
 * which is itself derived from the crapto1 / Craptev1 library by bla and the
 * nfc-tools / Proxmark project. Licensed under the GNU General Public License
 * version 3 (see the LICENSE file in that repository).
 *
 * Only the RAM-bounded recovery core has been extracted; the GUI, storage and
 * dictionary code was removed and progress/cancellation is exposed through a
 * callback. The recovery mathematics are unchanged from the upstream source.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "crapto1.h"

#include <stdlib.h>
#include <string.h>

#pragma GCC optimize("O3")
#pragma GCC optimize("-funroll-all-loops")

#define LF_POLY_ODD  (0x29CE5C)
#define LF_POLY_EVEN (0x870804)
#define CONST_M1_1   (LF_POLY_EVEN << 1 | 1)
#define CONST_M2_1   (LF_POLY_ODD << 1)
#define CONST_M1_2   (LF_POLY_ODD)
#define CONST_M2_2   (LF_POLY_EVEN << 1 | 1)
#define BIT(x, n)    ((x) >> (n) & 1)
#define BEBIT(x, n)  BIT(x, (n) ^ 24)
#define SWAPENDIAN(x) \
    ((x) = ((x) >> 8 & 0xff00ff) | ((x)&0xff00ff) << 8, (x) = (x) >> 16 | (x) << 16)

// MSB_LIMIT: chunk size (out of 256). 16 keeps peak heap at ~115 KB, which fits
// on an ESP32-S3 without PSRAM.
#define MSB_LIMIT 16

struct Crypto1State {
    uint32_t odd, even;
};

struct Msb {
    int tail;
    uint32_t states[768];
};

// clang-format off
static const uint8_t table[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3,
    4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4,
    4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4,
    5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5,
    4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2,
    3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5,
    5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4,
    5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 3, 4, 4, 5, 4, 5, 5, 6,
    4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};
static const uint8_t lookup1[256] = {
    0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16,
    8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24, 8, 8,  24, 24, 8,  24, 8,  8,
    8, 24, 8,  8,  24, 24, 24, 24, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24};
static const uint8_t lookup2[256] = {
    0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4,
    4, 4, 4, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6,
    2, 2, 6, 6, 6, 6, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 2, 2, 6, 6, 2, 6, 2,
    2, 2, 6, 2, 2, 6, 6, 6, 6, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 0, 0, 4, 4,
    0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 2,
    2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4,
    4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2,
    2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2,
    2, 6, 2, 2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6};
// clang-format on

uint32_t crapto1_prng_successor(uint32_t x, uint32_t n) {
    SWAPENDIAN(x);
    while(n--) x = x >> 1 | (x >> 16 ^ x >> 18 ^ x >> 19 ^ x >> 21) << 31;
    return SWAPENDIAN(x);
}

static inline int filter(uint32_t const x) {
    uint32_t f;
    f = lookup1[x & 0xff] | lookup2[(x >> 8) & 0xff];
    f |= 0x0d938 >> (x >> 16 & 0xf) & 1;
    return BIT(0xEC57E80A, f);
}

static inline uint8_t evenparity32(uint32_t x) {
    if((table[x & 0xff] + table[(x >> 8) & 0xff] + table[(x >> 16) & 0xff] + table[x >> 24]) % 2 ==
       0) {
        return 0;
    } else {
        return 1;
    }
}

static inline void update_contribution(unsigned int data[], int item, int mask1, int mask2) {
    int p = data[item] >> 25;
    p = p << 1 | evenparity32(data[item] & mask1);
    p = p << 1 | evenparity32(data[item] & mask2);
    data[item] = p << 24 | (data[item] & 0xffffff);
}

static void crypto1_get_lfsr(struct Crypto1State* state, uint64_t* lfsr) {
    int i;
    for(*lfsr = 0, i = 23; i >= 0; --i) {
        *lfsr = *lfsr << 1 | BIT(state->odd, i ^ 3);
        *lfsr = *lfsr << 1 | BIT(state->even, i ^ 3);
    }
}

static inline uint32_t crypt_word(struct Crypto1State* s) {
    // "in" and "x" are always 0 (last iteration)
    uint32_t res_ret = 0;
    uint32_t feedin, t;
    for(int i = 0; i <= 31; i++) {
        res_ret |= (filter(s->odd) << (24 ^ i));
        feedin = LF_POLY_EVEN & s->even;
        feedin ^= LF_POLY_ODD & s->odd;
        s->even = s->even << 1 | (evenparity32(feedin));
        t = s->odd, s->odd = s->even, s->even = t;
    }
    return res_ret;
}

static inline void crypt_word_noret(struct Crypto1State* s, uint32_t in, int x) {
    uint8_t ret;
    uint32_t feedin, t, next_in;
    for(int i = 0; i <= 31; i++) {
        next_in = BEBIT(in, i);
        ret = filter(s->odd);
        feedin = ret & (!!x);
        feedin ^= LF_POLY_EVEN & s->even;
        feedin ^= LF_POLY_ODD & s->odd;
        feedin ^= !!next_in;
        s->even = s->even << 1 | (evenparity32(feedin));
        t = s->odd, s->odd = s->even, s->even = t;
    }
    return;
}

static inline void rollback_word_noret(struct Crypto1State* s, uint32_t in, int x) {
    uint8_t ret;
    uint32_t feedin, t, next_in;
    for(int i = 31; i >= 0; i--) {
        next_in = BEBIT(in, i);
        s->odd &= 0xffffff;
        t = s->odd, s->odd = s->even, s->even = t;
        ret = filter(s->odd);
        feedin = ret & (!!x);
        feedin ^= s->even & 1;
        feedin ^= LF_POLY_EVEN & (s->even >>= 1);
        feedin ^= LF_POLY_ODD & s->odd;
        feedin ^= !!next_in;
        s->even |= (evenparity32(feedin)) << 23;
    }
    return;
}

static int check_state(struct Crypto1State* t, Crapto1Params* p) {
    if(!(t->odd | t->even)) return 0;
    rollback_word_noret(t, 0, 0);
    rollback_word_noret(t, p->nr0_enc, 1);
    rollback_word_noret(t, p->uid_xor_nt0, 0);
    struct Crypto1State temp = {t->odd, t->even};
    crypt_word_noret(t, p->uid_xor_nt1, 0);
    crypt_word_noret(t, p->nr1_enc, 1);
    if(p->ar1_enc == (crypt_word(t) ^ p->p64b)) {
        crypto1_get_lfsr(&temp, &(p->key));
        return 1;
    }
    return 0;
}

static inline int state_loop(unsigned int* states_buffer, int xks, int m1, int m2) {
    int states_tail = 0;
    int round = 0, s = 0, xks_bit = 0;

    for(round = 1; round <= 12; round++) {
        xks_bit = BIT(xks, round);

        for(s = 0; s <= states_tail; s++) {
            states_buffer[s] <<= 1;

            if((filter(states_buffer[s]) ^ filter(states_buffer[s] | 1)) != 0) {
                states_buffer[s] |= filter(states_buffer[s]) ^ xks_bit;
                if(round > 4) {
                    update_contribution(states_buffer, s, m1, m2);
                }
            } else if(filter(states_buffer[s]) == xks_bit) {
                if(round > 4) {
                    states_buffer[++states_tail] = states_buffer[s + 1];
                    states_buffer[s + 1] = states_buffer[s] | 1;
                    update_contribution(states_buffer, s, m1, m2);
                    s++;
                    update_contribution(states_buffer, s, m1, m2);
                } else {
                    states_buffer[++states_tail] = states_buffer[++s];
                    states_buffer[s] = states_buffer[s - 1] | 1;
                }
            } else {
                states_buffer[s--] = states_buffer[states_tail--];
            }
        }
    }

    return states_tail;
}

static int binsearch(unsigned int data[], int start, int stop) {
    int mid, val = data[stop] & 0xff000000;
    while(start != stop) {
        mid = (stop - start) >> 1;
        if((data[start + mid] ^ 0x80000000) > (val ^ 0x80000000))
            stop = start + mid;
        else
            start += mid + 1;
    }
    return start;
}

static void quicksort(unsigned int array[], int low, int high) {
    if(low >= high) return;
    int middle = low + (high - low) / 2;
    unsigned int pivot = array[middle];
    int i = low, j = high;
    while(i <= j) {
        while(array[i] < pivot) {
            i++;
        }
        while(array[j] > pivot) {
            j--;
        }
        if(i <= j) { // swap
            int temp = array[i];
            array[i] = array[j];
            array[j] = temp;
            i++;
            j--;
        }
    }
    if(low < j) {
        quicksort(array, low, j);
    }
    if(high > i) {
        quicksort(array, i, high);
    }
}

static int extend_table(unsigned int data[], int tbl, int end, int bit, int m1, int m2) {
    for(data[tbl] <<= 1; tbl <= end; data[++tbl] <<= 1) {
        if((filter(data[tbl]) ^ filter(data[tbl] | 1)) != 0) {
            data[tbl] |= filter(data[tbl]) ^ bit;
            update_contribution(data, tbl, m1, m2);
        } else if(filter(data[tbl]) == bit) {
            data[++end] = data[tbl + 1];
            data[tbl + 1] = data[tbl] | 1;
            update_contribution(data, tbl, m1, m2);
            tbl++;
            update_contribution(data, tbl, m1, m2);
        } else {
            data[tbl--] = data[end--];
        }
    }
    return end;
}

static int old_recover(
    unsigned int odd[],
    int o_head,
    int o_tail,
    int oks,
    unsigned int even[],
    int e_head,
    int e_tail,
    int eks,
    int rem,
    int s,
    Crapto1Params* p,
    int first_run) {
    int o, e, i;
    if(rem == -1) {
        for(e = e_head; e <= e_tail; ++e) {
            even[e] = (even[e] << 1) ^ evenparity32(even[e] & LF_POLY_EVEN);
            for(o = o_head; o <= o_tail; ++o, ++s) {
                struct Crypto1State temp = {0, 0};
                temp.even = odd[o];
                temp.odd = even[e] ^ evenparity32(odd[o] & LF_POLY_ODD);
                if(check_state(&temp, p)) {
                    return -1;
                }
            }
        }
        return s;
    }
    if(first_run == 0) {
        for(i = 0; (i < 4) && (rem-- != 0); i++) {
            oks >>= 1;
            eks >>= 1;
            o_tail = extend_table(
                odd, o_head, o_tail, oks & 1, LF_POLY_EVEN << 1 | 1, LF_POLY_ODD << 1);
            if(o_head > o_tail) return s;
            e_tail =
                extend_table(even, e_head, e_tail, eks & 1, LF_POLY_ODD, LF_POLY_EVEN << 1 | 1);
            if(e_head > e_tail) return s;
        }
    }
    first_run = 0;
    quicksort(odd, o_head, o_tail);
    quicksort(even, e_head, e_tail);
    while(o_tail >= o_head && e_tail >= e_head) {
        if(((odd[o_tail] ^ even[e_tail]) >> 24) == 0) {
            o_tail = binsearch(odd, o_head, o = o_tail);
            e_tail = binsearch(even, e_head, e = e_tail);
            s = old_recover(odd, o_tail--, o, oks, even, e_tail--, e, eks, rem, s, p, first_run);
            if(s == -1) {
                break;
            }
        } else if((odd[o_tail] ^ 0x80000000) > (even[e_tail] ^ 0x80000000)) {
            o_tail = binsearch(odd, o_head, o_tail) - 1;
        } else {
            e_tail = binsearch(even, e_head, e_tail) - 1;
        }
    }
    return s;
}

static int calculate_msb_tables(
    int oks,
    int eks,
    int msb_round,
    Crapto1Params* p,
    unsigned int* states_buffer,
    struct Msb* odd_msbs,
    struct Msb* even_msbs,
    unsigned int* temp_states_odd,
    unsigned int* temp_states_even,
    Crapto1CancelFn cancel,
    void* cancel_ctx) {
    unsigned int msb_head = (MSB_LIMIT * msb_round);
    unsigned int msb_tail = (MSB_LIMIT * (msb_round + 1));
    int states_tail = 0, tail = 0;
    int i = 0, j = 0, semi_state = 0, found = 0;
    unsigned int msb = 0;
    memset(odd_msbs, 0, MSB_LIMIT * sizeof(struct Msb));
    memset(even_msbs, 0, MSB_LIMIT * sizeof(struct Msb));

    for(semi_state = 1 << 20; semi_state >= 0; semi_state--) {
        if(semi_state % 32768 == 0) {
            if(cancel && cancel(cancel_ctx)) {
                return 0;
            }
        }

        if(filter(semi_state) == (oks & 1)) {
            states_buffer[0] = semi_state;
            states_tail = state_loop(states_buffer, oks, CONST_M1_1, CONST_M2_1);

            for(i = states_tail; i >= 0; i--) {
                msb = states_buffer[i] >> 24;
                if((msb >= msb_head) && (msb < msb_tail)) {
                    found = 0;
                    for(j = 0; j < odd_msbs[msb - msb_head].tail - 1; j++) {
                        if(odd_msbs[msb - msb_head].states[j] == states_buffer[i]) {
                            found = 1;
                            break;
                        }
                    }

                    if(!found) {
                        tail = odd_msbs[msb - msb_head].tail++;
                        odd_msbs[msb - msb_head].states[tail] = states_buffer[i];
                    }
                }
            }
        }

        if(filter(semi_state) == (eks & 1)) {
            states_buffer[0] = semi_state;
            states_tail = state_loop(states_buffer, eks, CONST_M1_2, CONST_M2_2);

            for(i = 0; i <= states_tail; i++) {
                msb = states_buffer[i] >> 24;
                if((msb >= msb_head) && (msb < msb_tail)) {
                    found = 0;

                    for(j = 0; j < even_msbs[msb - msb_head].tail; j++) {
                        if(even_msbs[msb - msb_head].states[j] == states_buffer[i]) {
                            found = 1;
                            break;
                        }
                    }

                    if(!found) {
                        tail = even_msbs[msb - msb_head].tail++;
                        even_msbs[msb - msb_head].states[tail] = states_buffer[i];
                    }
                }
            }
        }
    }

    oks >>= 12;
    eks >>= 12;

    for(i = 0; i < MSB_LIMIT; i++) {
        if(cancel && cancel(cancel_ctx)) {
            return 0;
        }
        memset(temp_states_even, 0, sizeof(unsigned int) * (1280));
        memset(temp_states_odd, 0, sizeof(unsigned int) * (1280));
        memcpy(temp_states_odd, odd_msbs[i].states, odd_msbs[i].tail * sizeof(unsigned int));
        memcpy(temp_states_even, even_msbs[i].states, even_msbs[i].tail * sizeof(unsigned int));
        int res = old_recover(
            temp_states_odd,
            0,
            odd_msbs[i].tail,
            oks,
            temp_states_even,
            0,
            even_msbs[i].tail,
            eks,
            3,
            0,
            p,
            1);
        if(res == -1) {
            return 1;
        }
    }

    return 0;
}

bool crapto1_recover(Crapto1Params* p, uint32_t ks2, Crapto1CancelFn cancel, void* cancel_ctx) {
    bool found = false;
    unsigned int* states_buffer = malloc(sizeof(unsigned int) * (2 << 9));
    struct Msb* odd_msbs = (struct Msb*)malloc(MSB_LIMIT * sizeof(struct Msb));
    struct Msb* even_msbs = (struct Msb*)malloc(MSB_LIMIT * sizeof(struct Msb));
    unsigned int* temp_states_odd = malloc(sizeof(unsigned int) * (1280));
    unsigned int* temp_states_even = malloc(sizeof(unsigned int) * (1280));

    if(!states_buffer || !odd_msbs || !even_msbs || !temp_states_odd || !temp_states_even) {
        // Out of memory (e.g. no PSRAM and heap exhausted) - fail cleanly.
        free(states_buffer);
        free(odd_msbs);
        free(even_msbs);
        free(temp_states_odd);
        free(temp_states_even);
        return false;
    }

    int oks = 0, eks = 0;
    int i = 0, msb = 0;
    for(i = 31; i >= 0; i -= 2) {
        oks = oks << 1 | BEBIT(ks2, i);
    }
    for(i = 30; i >= 0; i -= 2) {
        eks = eks << 1 | BEBIT(ks2, i);
    }

    for(msb = 0; msb <= ((256 / MSB_LIMIT) - 1); msb++) {
        if(cancel && cancel(cancel_ctx)) {
            break;
        }
        if(calculate_msb_tables(
               oks,
               eks,
               msb,
               p,
               states_buffer,
               odd_msbs,
               even_msbs,
               temp_states_odd,
               temp_states_even,
               cancel,
               cancel_ctx)) {
            found = true;
            break;
        }
    }

    free(states_buffer);
    free(odd_msbs);
    free(even_msbs);
    free(temp_states_odd);
    free(temp_states_even);
    return found;
}
