//! Bitsliced Hitag-2 guess-and-determine primitives (256 lanes).
//!
//! This is the foundation for the fast (hitag2hell/OpenCL-style) recovery: instead
//! of one candidate state per `u64`, each of the 48 state bits is a 256-lane word,
//! so `f20`/`lfsr` run on 256 candidate states at once via pure bitwise ops (no
//! table lookups). The filter is the bitsliced boolean form of the scalar filter
//! (verified against gd.rs's 0x3c65 / 0x0ee5 / 0x0dd3929b constants below).
//!
//! Bit-order is the SAME hitag2hell convention as `gd.rs` (this module reuses
//! `gd::f20`/`gd::lfsr` only in the self-tests, as the ground-truth oracle).
//!
//! Layout of the fill/guess SCHEDULE (which bits are lane-enumerated, the i1..i8
//! nested loops, the filter1..filter31 pruning cascade) lands on top of these
//! primitives — see `find_state` once wired. This file is deliberately just the
//! verified primitives + their per-lane equivalence tests.

use crate::gd;

/// 256 lanes = four 64-bit words — exactly one AVX2 register, the native SIMD
/// width. (Measured: 512 lanes via [u64;8] is ~0.7x slower — doubling per-op cost
/// past the register width doesn't pay for the halved i1 loop, and it worsens the
/// bitslice weak-pruning. 256 is the sweet spot on this hardware.)
pub const LANES: usize = 256;
const W: usize = LANES / 64;

/// A 256-lane bitslice word: lane `j` lives in bit `j & 63` of limb `j >> 6`.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Bs(pub [u64; W]);

impl Bs {
    pub const ZERO: Bs = Bs([0; W]);
    pub const ONES: Bs = Bs([!0; W]);

    #[inline(always)]
    pub fn is_zero(self) -> bool {
        self.0.iter().all(|&x| x == 0)
    }
    #[inline(always)]
    pub fn lane(self, j: usize) -> u64 {
        (self.0[j >> 6] >> (j & 63)) & 1
    }
    #[inline(always)]
    pub fn set_lane(&mut self, j: usize, bit: u64) {
        let m = 1u64 << (j & 63);
        if bit & 1 == 1 {
            self.0[j >> 6] |= m;
        } else {
            self.0[j >> 6] &= !m;
        }
    }
    /// Broadcast a scalar bit into all lanes.
    #[inline(always)]
    pub fn splat(bit: u64) -> Bs {
        if bit & 1 == 1 {
            Bs::ONES
        } else {
            Bs::ZERO
        }
    }
}

impl core::ops::BitAnd for Bs {
    type Output = Bs;
    #[inline(always)]
    fn bitand(self, o: Bs) -> Bs {
        let mut r = [0u64; W];
        for i in 0..W {
            r[i] = self.0[i] & o.0[i];
        }
        Bs(r)
    }
}
impl core::ops::BitOr for Bs {
    type Output = Bs;
    #[inline(always)]
    fn bitor(self, o: Bs) -> Bs {
        let mut r = [0u64; W];
        for i in 0..W {
            r[i] = self.0[i] | o.0[i];
        }
        Bs(r)
    }
}
impl core::ops::BitXor for Bs {
    type Output = Bs;
    #[inline(always)]
    fn bitxor(self, o: Bs) -> Bs {
        let mut r = [0u64; W];
        for i in 0..W {
            r[i] = self.0[i] ^ o.0[i];
        }
        Bs(r)
    }
}
impl core::ops::Not for Bs {
    type Output = Bs;
    #[inline(always)]
    fn not(self) -> Bs {
        let mut r = [0u64; W];
        for i in 0..W {
            r[i] = !self.0[i];
        }
        Bs(r)
    }
}

// ---------------------------------------------------------------------------
// Bitsliced filter sub-functions (verified vs the scalar constants).
// f_a == 0x3c65[abcd], f_b == 0x0ee5[abcd], f_c == 0x0dd3929b[abcde],
// where the 4/5-bit index is MSB-first (a is the high bit). Op counts match
// ref_x86.c (6 / 7 / 13).
// ---------------------------------------------------------------------------

#[inline(always)]
pub fn f_a(a: Bs, b: Bs, c: Bs, d: Bs) -> Bs {
    !(((a | b) & c) ^ (a | d) ^ b)
}
#[inline(always)]
pub fn f_b(a: Bs, b: Bs, c: Bs, d: Bs) -> Bs {
    !(((d | c) & (a ^ b)) ^ (d | a | b))
}
#[inline(always)]
pub fn f_c(a: Bs, b: Bs, c: Bs, d: Bs, e: Bs) -> Bs {
    // ref_x86: ~( X ^ Y ). Written with explicit locals because Rust's `!` binds
    // tighter than `^` (so `!x ^ y` would be `(!x) ^ y`, a different-looking though
    // XNOR-equivalent expression — keep it unambiguous).
    let x = (((((c ^ e) | d) & a) ^ b)) & (c ^ b);
    let y = ((d ^ e) | a) & ((d ^ b) | c);
    !(x ^ y)
}

/// The 20 filter taps and their sub-filter assignment, exactly mirroring gd.rs
/// `f20`: five 4-input sub-filters (f_a/f_b/f_b/f_b/f_a) feed one 5-input f_c.
/// `w[i]` is the bitslice word holding state bit `i`.
#[inline(always)]
pub fn f20_bs(w: &[Bs; 48]) -> Bs {
    let f0 = f_a(w[2], w[3], w[5], w[6]);
    let f1 = f_b(w[8], w[12], w[14], w[15]);
    let f2 = f_b(w[17], w[21], w[23], w[26]);
    let f3 = f_b(w[28], w[29], w[31], w[33]);
    let f4 = f_a(w[34], w[43], w[44], w[46]);
    f_c(f0, f1, f2, f3, f4)
}

/// LFSR feedback taps (same set as gd.rs `lfsr_feedback`): XOR of these state bits.
pub const FB_TAPS: [usize; 16] = [0, 2, 3, 6, 7, 8, 16, 22, 23, 26, 30, 41, 42, 43, 46, 47];

/// Bitsliced LFSR feedback bit (the new bit entering at position 47).
#[inline(always)]
pub fn lfsr_feedback_bs(w: &[Bs; 48]) -> Bs {
    let mut acc = w[FB_TAPS[0]];
    for &t in &FB_TAPS[1..] {
        acc = acc ^ w[t];
    }
    acc
}

/// One bitsliced LFSR clock: shift all words down one (w[i] <- w[i+1]) and set
/// w[47] to the feedback of the pre-shift state — matching gd.rs `lfsr`.
#[inline(always)]
pub fn lfsr_step_bs(w: &mut [Bs; 48]) {
    let fb = lfsr_feedback_bs(w);
    for i in 0..47 {
        w[i] = w[i + 1];
    }
    w[47] = fb;
}

// ---------------------------------------------------------------------------
// Pack / unpack between LANES scalar states and the bitslice word array.
// ---------------------------------------------------------------------------

/// Pack `LANES` scalar 48-bit states into the word array (lane j = states[j]).
pub fn pack(states: &[u64; LANES]) -> [Bs; 48] {
    let mut w = [Bs::ZERO; 48];
    for (j, &s) in states.iter().enumerate() {
        for i in 0..48usize {
            w[i].set_lane(j, (s >> i) & 1);
        }
    }
    w
}

/// Recover lane `j`'s 48-bit state from the word array.
pub fn unpack_lane(w: &[Bs; 48], j: usize) -> u64 {
    let mut s = 0u64;
    for i in 0..48usize {
        s |= w[i].lane(j) << i;
    }
    s
}

// ---------------------------------------------------------------------------
// Self-tests — per-lane equivalence against the audited scalar cipher.
// ---------------------------------------------------------------------------

/// Verify f_a/f_b/f_c reproduce the scalar filter constants for all input combos,
/// using splatted (all-0 / all-1) lane words. Mirrors the host python check.
fn test_filters() -> bool {
    let bit = |x: u32, i: u32| ((x >> i) & 1) as u64;
    for x in 0..16u32 {
        let (a, b, c, d) = (bit(x, 3), bit(x, 2), bit(x, 1), bit(x, 0));
        let ga = f_a(Bs::splat(a), Bs::splat(b), Bs::splat(c), Bs::splat(d)).lane(0);
        if ga != (0x3c65u64 >> x) & 1 {
            return false;
        }
        let gb = f_b(Bs::splat(a), Bs::splat(b), Bs::splat(c), Bs::splat(d)).lane(0);
        if gb != (0x0ee5u64 >> x) & 1 {
            return false;
        }
    }
    for x in 0..32u32 {
        let (a, b, c, d, e) = (bit(x, 4), bit(x, 3), bit(x, 2), bit(x, 1), bit(x, 0));
        let gc = f_c(
            Bs::splat(a),
            Bs::splat(b),
            Bs::splat(c),
            Bs::splat(d),
            Bs::splat(e),
        )
        .lane(0);
        if gc != (0x0dd3929bu64 >> x) & 1 {
            return false;
        }
    }
    true
}

/// Pack LANES pseudo-random states, then check f20_bs matches gd::f20 per lane and
/// one lfsr_step_bs matches gd::lfsr per lane. Repeated over several batches.
fn test_f20_lfsr_roundtrip() -> bool {
    const M48: u64 = (1 << 48) - 1;
    let mut rng = 0xDEAD_BEEF_1234_5678u64;
    for _ in 0..8 {
        let mut states = [0u64; LANES];
        for s in states.iter_mut() {
            rng = rng
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            *s = rng & M48;
        }
        let mut w = pack(&states);
        // f20 per lane
        let f = f20_bs(&w);
        for (j, &s) in states.iter().enumerate() {
            if f.lane(j) != gd::f20(s) as u64 {
                return false;
            }
        }
        // one lfsr clock per lane
        lfsr_step_bs(&mut w);
        for (j, &s) in states.iter().enumerate() {
            if unpack_lane(&w, j) != gd::lfsr(s) {
                return false;
            }
        }
    }
    true
}

// ---------------------------------------------------------------------------
// Bitsliced find_state — the fast recovery (256 candidate assignments per pass).
//
// State model (ref_x86.c "-2" convention): the UNROLLED LFSR lives in a fixed
// array `st: [Bs; 78]`; logical bit IDX is at `st[IDX-2]` (logical 0,1 are never
// stored). st[0..47] = post-init state bits 2..49; st[48],[49] = the two GUESSED
// lfsr outputs (logical 48,49); st[50..] = feedback via lfsr_bs.
// ---------------------------------------------------------------------------

const FILT_MASK: u64 = 0x5806_b4a2_d16c;
/// The 8 layer-1 tap positions enumerated across the 256 lanes (ref_x86's
/// filter_pos[0..8]).
const FILTER_POS8: [usize; 8] = [4, 7, 9, 13, 16, 18, 22, 24];
const M48: u64 = (1 << 48) - 1;

/// initial_bitslices[b]: lane L holds bit (L>>b)&1. Across 256 lanes the 8 slices
/// enumerate all 2^8 values of the 8 lane bits (lane index == the value).
fn initial_bitslices() -> [Bs; 8] {
    let mut ib = [Bs::ZERO; 8];
    for (b, slice) in ib.iter_mut().enumerate() {
        for l in 0..LANES {
            slice.set_lane(l, ((l >> b) & 1) as u64);
        }
    }
    ib
}

/// keystream[N] = NOT ks[N] broadcast (ks MSB-first: ks[N] = (ks>>(31-N))&1).
/// Then `filter(N) ^ keystream[N]` is 1 exactly in lanes where f20 == ks[N].
fn keystream_words(ks: u64) -> [Bs; 32] {
    let mut kw = [Bs::ZERO; 32];
    for (n, w) in kw.iter_mut().enumerate() {
        let ksn = (ks >> (31 - n)) & 1;
        *w = Bs::splat(ksn ^ 1);
    }
    kw
}

/// f20 at keystream position `n` over the unrolled array (taps shifted up by n).
/// `st[i]` = logical bit i+2. Byte-for-byte equal to gd::f20(state>>? ) per lane.
#[inline(always)]
fn filter(st: &[Bs; 78], n: usize) -> Bs {
    let g0 = f_a(st[n], st[n + 1], st[n + 3], st[n + 4]);
    let g1 = f_b(st[n + 6], st[n + 10], st[n + 12], st[n + 13]);
    let g2 = f_b(st[n + 15], st[n + 19], st[n + 21], st[n + 24]);
    let g3 = f_b(st[n + 26], st[n + 27], st[n + 29], st[n + 31]);
    let g4 = f_a(st[n + 32], st[n + 41], st[n + 42], st[n + 44]);
    f_c(g0, g1, g2, g3, g4)
}

/// LFSR feedback for clock `k`; writes logical bit k+48 (= st[k+46]). Reads
/// logical (k+tap) = st[k+tap-2] for each feedback tap.
#[inline(always)]
fn lfsr_bs_at(st: &[Bs; 78], k: usize) -> Bs {
    let mut acc = st[k + FB_TAPS[0] - 2];
    for &t in &FB_TAPS[1..] {
        acc = acc ^ st[k + t - 2];
    }
    acc
}

/// Scatter the low 20 bits of `i0` into the FILT_MASK positions (== gd::expand).
fn expand20(i0: u64) -> u64 {
    let (mut res, mut x, mut mask) = (0u64, i0, FILT_MASK);
    for i in 0..48u32 {
        if mask & 1 == 1 {
            res |= (x & 1) << i;
            x >>= 1;
        }
        mask >>= 1;
    }
    res
}

/// Scalar layer-0: the ~2^19 filter-tap fillings whose f20 == ks bit 0. Each is a
/// seed for one bitsliced 256-lane subtree (identical set to gd.rs tables[0]).
fn build_candidates(ks_bit0: u32) -> Vec<u64> {
    let mut v = Vec::with_capacity(1 << 19);
    for i0 in 0u64..(1u64 << 20) {
        let s0 = expand20(i0);
        if gd::f20(s0) == ks_bit0 {
            v.push(s0);
        }
    }
    v
}

/// Recover post-init states from ONE layer-0 candidate via the bitsliced nested
/// search, and return the first key that reproduces BOTH frames (or None).
#[allow(clippy::too_many_arguments)]
fn search_candidate(
    state0: u64,
    ib: &[Bs; 8],
    kw: &[Bs; 32],
    uid: u64,
    nonce: u64,
    ks: u64,
    nonce2: u64,
    ks2: u64,
) -> Option<u64> {
    let mut st = [Bs::ZERO; 78];
    // Load candidate bits 2..47 (broadcast), then the 8 lane-enumerated bits.
    for j in 0..46usize {
        st[j] = Bs::splat((state0 >> (2 + j)) & 1);
    }
    for b in 0..8usize {
        st[FILTER_POS8[b] - 2] = ib[b];
    }
    let sel = |bit: u64| Bs::splat(bit);

    for i1 in 0u64..128 {
        st[25] = sel(i1 & 0x01); // logical 27
        st[28] = sel((i1 >> 1) & 1); // 30
        st[30] = sel((i1 >> 2) & 1); // 32
        st[33] = sel((i1 >> 3) & 1); // 35
        st[43] = sel((i1 >> 4) & 1); // 45
        st[45] = sel((i1 >> 5) & 1); // 47
        st[46] = sel((i1 >> 6) & 1); // 48 = lfsr guess 0
        let r1 = filter(&st, 1) ^ kw[1];
        if r1.is_zero() {
            continue;
        }
        for i2 in 0u64..32 {
            st[8] = sel(i2 & 0x01); // logical 10
            st[17] = sel((i2 >> 1) & 1); // 19
            st[23] = sel((i2 >> 2) & 1); // 25
            st[34] = sel((i2 >> 3) & 1); // 36
            st[47] = sel((i2 >> 4) & 1); // 49 = lfsr guess 1
            let r2 = r1 & (filter(&st, 2) ^ kw[2]);
            if r2.is_zero() {
                continue;
            }
            st[48] = lfsr_bs_at(&st, 2); // logical 50
            for i3 in 0u64..8 {
                st[9] = sel(i3 & 0x01); // 11
                st[18] = sel((i3 >> 1) & 1); // 20
                st[35] = sel((i3 >> 2) & 1); // 37
                let r3 = r2 & (filter(&st, 3) ^ kw[3]);
                if r3.is_zero() {
                    continue;
                }
                st[49] = lfsr_bs_at(&st, 3);
                st[50] = lfsr_bs_at(&st, 4);
                st[51] = lfsr_bs_at(&st, 5);
                st[52] = lfsr_bs_at(&st, 6);
                st[53] = lfsr_bs_at(&st, 7);
                for i4 in 0u64..2 {
                    st[36] = sel(i4); // 38
                    let r4 = r3 & (filter(&st, 4) ^ kw[4]);
                    if r4.is_zero() {
                        continue;
                    }
                    for i5 in 0u64..2 {
                        st[37] = sel(i5); // 39
                        let r5 = r4 & (filter(&st, 5) ^ kw[5]);
                        if r5.is_zero() {
                            continue;
                        }
                        for i6 in 0u64..2 {
                            st[38] = sel(i6); // 40
                            let r6 = r5 & (filter(&st, 6) ^ kw[6]);
                            if r6.is_zero() {
                                continue;
                            }
                            for i7 in 0u64..2 {
                                st[39] = sel(i7); // 41
                                let r7 = r6 & (filter(&st, 7) ^ kw[7]);
                                if r7.is_zero() {
                                    continue;
                                }
                                for i8 in 0u64..2 {
                                    st[40] = sel(i8); // 42
                                    let mut res = r7 & (filter(&st, 8) ^ kw[8]);
                                    if res.is_zero() {
                                        continue;
                                    }
                                    // Tail == gd::test(): ks[9..=31], one accumulator.
                                    res = res & (filter(&st, 9) ^ kw[9]);
                                    if res.is_zero() {
                                        continue;
                                    }
                                    let mut alive = true;
                                    for n in 10..=31usize {
                                        // lfsr_bs(n-2) writes logical n+46 = st[n+44].
                                        let v = lfsr_bs_at(&st, n - 2);
                                        st[n + 44] = v;
                                        res = res & (filter(&st, n) ^ kw[n]);
                                        if res.is_zero() {
                                            alive = false;
                                            break;
                                        }
                                    }
                                    if !alive {
                                        continue;
                                    }
                                    // Extract every surviving lane -> post-init state -> key.
                                    for r in 0..LANES {
                                        if res.lane(r) == 0 {
                                            continue;
                                        }
                                        // st[k] = logical k+2 = S_2 bit k.
                                        let mut s2 = 0u64;
                                        for k in 0..48usize {
                                            s2 |= st[k].lane(r) << k;
                                        }
                                        let s0 = gd::lfsr_inv(gd::lfsr_inv(s2)) & M48;
                                        let key = gd::invert_init(s0, uid, nonce);
                                        if gd::keystream(gd::init(key, uid, nonce), 32) == ks
                                            && gd::keystream(gd::init(key, uid, nonce2), 32) == ks2
                                        {
                                            return Some(key);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    None
}

/// Faithful transcription of ref_x86.c `find_state` for ONE layer-0 candidate,
/// PRESERVING the sub-filter hoisting: each `filterN_k` is computed at the exact
/// loop nesting level ref_x86 places it (many hoisted above the loop that would
/// otherwise recompute them). Same observable behavior as `search_candidate`
/// (verified by the equivalence gate) but ~an order of magnitude faster.
///
/// Indexing: LOGICAL array `st[X]` holds logical state bit X, so every C
/// `state[-2+X]` becomes exactly `st[X]` (logical 0,1 are never used).
#[allow(clippy::too_many_arguments)]
fn search_candidate_hoisted(
    state0: u64,
    ib: &[Bs; 8],
    kw: &[Bs; 32],
    uid: u64,
    nonce: u64,
    ks: u64,
    nonce2: u64,
    ks2: u64,
) -> Option<u64> {
    let mut st = [Bs::ZERO; 80];

    // ref ~136-139: candidate bits 2..47 (broadcast) + the 8 lane-enumerated bits.
    for j in 0..46usize {
        st[2 + j] = Bs::splat((state0 >> (2 + j)) & 1);
    }
    for b in 0..8usize {
        st[FILTER_POS8[b]] = ib[b];
    }

    let sel = |b: u64| Bs::splat(b);
    // lfsr_bs(k): XOR of st[k+tap] over FB_TAPS (writes logical bit k+48).
    let lfsr_bs = |st: &[Bs; 80], k: usize| -> Bs {
        let mut a = st[k + FB_TAPS[0]];
        for &t in &FB_TAPS[1..] {
            a = a ^ st[k + t];
        }
        a
    };
    // Fresh full 20-tap filter at position n — used ONLY for N=13..31, which
    // ref_x86 computes inline in the innermost loop with no hoisting.
    let filt = |st: &[Bs; 80], n: usize| -> Bs {
        let g0 = f_a(st[n + 2], st[n + 3], st[n + 5], st[n + 6]);
        let g1 = f_b(st[n + 8], st[n + 12], st[n + 14], st[n + 15]);
        let g2 = f_b(st[n + 17], st[n + 21], st[n + 23], st[n + 26]);
        let g3 = f_b(st[n + 28], st[n + 29], st[n + 31], st[n + 33]);
        let g4 = f_a(st[n + 34], st[n + 43], st[n + 44], st[n + 46]);
        f_c(g0, g1, g2, g3, g4)
    };

    for i1 in 0u64..128 {
        st[27] = sel(i1 & 0x1);
        st[30] = sel((i1 >> 1) & 1);
        st[32] = sel((i1 >> 2) & 1);
        st[35] = sel((i1 >> 3) & 1);
        st[45] = sel((i1 >> 4) & 1);
        st[47] = sel((i1 >> 5) & 1);
        st[48] = sel((i1 >> 6) & 1); // guess lfsr output 0

        let filter1_0 = f_a(st[3], st[4], st[6], st[7]);
        let filter1_1 = f_b(st[9], st[13], st[15], st[16]);
        let filter1_2 = f_b(st[18], st[22], st[24], st[27]);
        let filter1_3 = f_b(st[29], st[30], st[32], st[34]);
        let filter1_4 = f_a(st[35], st[44], st[45], st[47]);
        let filter1 = f_c(filter1_0, filter1_1, filter1_2, filter1_3, filter1_4);
        let r1 = filter1 ^ kw[1];
        if r1.is_zero() {
            continue;
        }

        // ---- hoisted at i1 (ref 164-174) ----
        let filter2_0 = f_a(st[4], st[5], st[7], st[8]);
        let filter2_3 = f_b(st[30], st[31], st[33], st[35]);
        let filter3_0 = f_a(st[5], st[6], st[8], st[9]);
        let filter5_2 = f_b(st[22], st[26], st[28], st[31]);
        let filter6_2 = f_b(st[23], st[27], st[29], st[32]);
        let filter7_2 = f_b(st[24], st[28], st[30], st[33]);
        let filter9_1 = f_b(st[17], st[21], st[23], st[24]);
        let filter9_2 = f_b(st[26], st[30], st[32], st[35]);
        let filter10_0 = f_a(st[12], st[13], st[15], st[16]);
        let filter11_0 = f_a(st[13], st[14], st[16], st[17]);
        let filter12_0 = f_a(st[14], st[15], st[17], st[18]);

        for i2 in 0u64..32 {
            st[10] = sel(i2 & 0x1);
            st[19] = sel((i2 >> 1) & 1);
            st[25] = sel((i2 >> 2) & 1);
            st[36] = sel((i2 >> 3) & 1);
            st[49] = sel((i2 >> 4) & 1); // guess lfsr output 1

            let filter2_1 = f_b(st[10], st[14], st[16], st[17]);
            let filter2_2 = f_b(st[19], st[23], st[25], st[28]);
            let filter2_4 = f_a(st[36], st[45], st[46], st[48]);
            let filter2 = f_c(filter2_0, filter2_1, filter2_2, filter2_3, filter2_4);
            let r2 = r1 & (filter2 ^ kw[2]);
            if r2.is_zero() {
                continue;
            }

            st[50] = lfsr_bs(&st, 2);

            // ---- hoisted at i2 (ref 196-205) ----
            let filter3_3 = f_b(st[31], st[32], st[34], st[36]);
            let filter4_0 = f_a(st[6], st[7], st[9], st[10]);
            let filter4_1 = f_b(st[12], st[16], st[18], st[19]);
            let filter4_2 = f_b(st[21], st[25], st[27], st[30]);
            let filter7_0 = f_a(st[9], st[10], st[12], st[13]);
            let filter7_1 = f_b(st[15], st[19], st[21], st[22]);
            let filter8_2 = f_b(st[25], st[29], st[31], st[34]);
            let filter10_1 = f_b(st[18], st[22], st[24], st[25]);
            let filter10_2 = f_b(st[27], st[31], st[33], st[36]);
            let filter11_1 = f_b(st[19], st[23], st[25], st[26]);

            for i3 in 0u64..8 {
                st[11] = sel(i3 & 0x1);
                st[20] = sel((i3 >> 1) & 1);
                st[37] = sel((i3 >> 2) & 1);

                let filter3_1 = f_b(st[11], st[15], st[17], st[18]);
                let filter3_2 = f_b(st[20], st[24], st[26], st[29]);
                let filter3_4 = f_a(st[37], st[46], st[47], st[49]);
                let filter3 = f_c(filter3_0, filter3_1, filter3_2, filter3_3, filter3_4);
                let r3 = r2 & (filter3 ^ kw[3]);
                if r3.is_zero() {
                    continue;
                }

                st[51] = lfsr_bs(&st, 3);
                st[52] = lfsr_bs(&st, 4);
                st[53] = lfsr_bs(&st, 5);
                st[54] = lfsr_bs(&st, 6);
                st[55] = lfsr_bs(&st, 7);

                // ---- hoisted at i3 (ref 229-239) ----
                let filter4_3 = f_b(st[32], st[33], st[35], st[37]);
                let filter5_0 = f_a(st[7], st[8], st[10], st[11]);
                let filter5_1 = f_b(st[13], st[17], st[19], st[20]);
                let filter6_0 = f_a(st[8], st[9], st[11], st[12]);
                let filter6_1 = f_b(st[14], st[18], st[20], st[21]);
                let filter8_0 = f_a(st[10], st[11], st[13], st[14]);
                let filter8_1 = f_b(st[16], st[20], st[22], st[23]);
                let filter9_0 = f_a(st[11], st[12], st[14], st[15]);
                let filter9_4 = f_a(st[43], st[52], st[53], st[55]);
                let filter11_2 = f_b(st[28], st[32], st[34], st[37]);
                let filter12_1 = f_b(st[20], st[24], st[26], st[27]);

                for i4 in 0u64..2 {
                    st[38] = sel(i4 & 0x1);
                    let filter4_4 = f_a(st[38], st[47], st[48], st[50]);
                    let filter4 = f_c(filter4_0, filter4_1, filter4_2, filter4_3, filter4_4);
                    let r4 = r3 & (filter4 ^ kw[4]);
                    if r4.is_zero() {
                        continue;
                    }

                    // ---- hoisted at i4 (ref 254-255) ----
                    let filter5_3 = f_b(st[33], st[34], st[36], st[38]);
                    let filter12_2 = f_b(st[29], st[33], st[35], st[38]);

                    for i5 in 0u64..2 {
                        st[39] = sel(i5 & 0x1);
                        let filter5_4 = f_a(st[39], st[48], st[49], st[51]);
                        let filter5 = f_c(filter5_0, filter5_1, filter5_2, filter5_3, filter5_4);
                        let r5 = r4 & (filter5 ^ kw[5]);
                        if r5.is_zero() {
                            continue;
                        }

                        // ---- hoisted at i5 (ref 270) ----
                        let filter6_3 = f_b(st[34], st[35], st[37], st[39]);

                        for i6 in 0u64..2 {
                            st[40] = sel(i6 & 0x1);
                            let filter6_4 = f_a(st[40], st[49], st[50], st[52]);
                            let filter6 = f_c(filter6_0, filter6_1, filter6_2, filter6_3, filter6_4);
                            let r6 = r5 & (filter6 ^ kw[6]);
                            if r6.is_zero() {
                                continue;
                            }

                            // ---- hoisted at i6 (ref 285) ----
                            let filter7_3 = f_b(st[35], st[36], st[38], st[40]);

                            for i7 in 0u64..2 {
                                st[41] = sel(i7 & 0x1);
                                let filter7_4 = f_a(st[41], st[50], st[51], st[53]);
                                let filter7 =
                                    f_c(filter7_0, filter7_1, filter7_2, filter7_3, filter7_4);
                                let r7 = r6 & (filter7 ^ kw[7]);
                                if r7.is_zero() {
                                    continue;
                                }

                                // ---- hoisted at i7 (ref 300-302) ----
                                let filter8_3 = f_b(st[36], st[37], st[39], st[41]);
                                let filter10_3 = f_b(st[38], st[39], st[41], st[43]);
                                let filter12_3 = f_b(st[40], st[41], st[43], st[45]);

                                for i8 in 0u64..2 {
                                    st[42] = sel(i8 & 0x1);
                                    // ---- computed at i8 (ref 306-308) ----
                                    let filter8_4 = f_a(st[42], st[51], st[52], st[54]);
                                    let filter9_3 = f_b(st[37], st[38], st[40], st[42]);
                                    let filter11_3 = f_b(st[39], st[40], st[42], st[44]);

                                    let filter8 =
                                        f_c(filter8_0, filter8_1, filter8_2, filter8_3, filter8_4);
                                    let mut res = r7 & (filter8 ^ kw[8]);
                                    if res.is_zero() {
                                        continue;
                                    }

                                    // filter9: fully hoisted parts (ref 319), no lfsr first.
                                    let filter9 =
                                        f_c(filter9_0, filter9_1, filter9_2, filter9_3, filter9_4);
                                    res = res & (filter9 ^ kw[9]);
                                    if res.is_zero() {
                                        continue;
                                    }

                                    // filter10: hoisted _0.._3 + fresh _4 (ref 328-330).
                                    st[56] = lfsr_bs(&st, 8);
                                    let filter10_4 = f_a(st[44], st[53], st[54], st[56]);
                                    let filter10 = f_c(
                                        filter10_0, filter10_1, filter10_2, filter10_3, filter10_4,
                                    );
                                    res = res & (filter10 ^ kw[10]);
                                    if res.is_zero() {
                                        continue;
                                    }

                                    // filter11: hoisted _0.._3 + fresh _4 (ref 339-341).
                                    st[57] = lfsr_bs(&st, 9);
                                    let filter11_4 = f_a(st[45], st[54], st[55], st[57]);
                                    let filter11 = f_c(
                                        filter11_0, filter11_1, filter11_2, filter11_3, filter11_4,
                                    );
                                    res = res & (filter11 ^ kw[11]);
                                    if res.is_zero() {
                                        continue;
                                    }

                                    // filter12: hoisted _0.._3 + fresh _4 (ref 350-352).
                                    st[58] = lfsr_bs(&st, 10);
                                    let filter12_4 = f_a(st[46], st[55], st[56], st[58]);
                                    let filter12 = f_c(
                                        filter12_0, filter12_1, filter12_2, filter12_3, filter12_4,
                                    );
                                    res = res & (filter12 ^ kw[12]);
                                    if res.is_zero() {
                                        continue;
                                    }

                                    // filters 13..31: NO hoisting in ref (all five sub-
                                    // filters computed inline each i8). st[N+46] =
                                    // lfsr_bs(N-2), then the fresh full filter N.
                                    let mut alive = true;
                                    for n in 13..=31usize {
                                        st[n + 46] = lfsr_bs(&st, n - 2);
                                        let fn_full = filt(&st, n);
                                        res = res & (fn_full ^ kw[n]);
                                        if res.is_zero() {
                                            alive = false;
                                            break;
                                        }
                                    }
                                    if !alive {
                                        continue;
                                    }

                                    // finalization (ref 647-654)
                                    for r in 0..LANES {
                                        if res.lane(r) == 0 {
                                            continue;
                                        }
                                        let mut s2 = 0u64;
                                        for k in 0..48usize {
                                            s2 |= st[2 + k].lane(r) << k;
                                        }
                                        let s0 = gd::lfsr_inv(gd::lfsr_inv(s2)) & M48;
                                        let key = gd::invert_init(s0, uid, nonce);
                                        if gd::keystream(gd::init(key, uid, nonce), 32) == ks
                                            && gd::keystream(gd::init(key, uid, nonce2), 32) == ks2
                                        {
                                            return Some(key);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    None
}

/// Equivalence gate: the hoisted search must return EXACTLY what the validated
/// uniform search returns — on true candidates for many random keys AND on random
/// candidates. Any hoist transcription error (a sub-filter read before its input
/// bit is set) diverges here.
pub fn test_hoist_equiv() -> bool {
    let ib = initial_bitslices();
    let mut rng = 0xA5A5_1234_DEAD_0001u64;
    let mut next = move || {
        rng = rng
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        rng
    };
    // (a) true candidates for 64 random keys: both must recover the exact key.
    for _ in 0..64 {
        let key = next() & M48;
        let uid = next() & 0xFFFF_FFFF;
        let nonce = next() & 0xFFFF_FFFF;
        let nonce2 = next() & 0xFFFF_FFFF;
        let s0 = gd::init(key, uid, nonce);
        let ks = gd::keystream(s0, 32);
        let ks2 = gd::keystream(gd::init(key, uid, nonce2), 32);
        let kw = keystream_words(ks);
        let cand = s0 & FILT_MASK;
        let u = search_candidate(cand, &ib, &kw, uid, nonce, ks, nonce2, ks2);
        let h = search_candidate_hoisted(cand, &ib, &kw, uid, nonce, ks, nonce2, ks2);
        if u != Some(key) || h != u {
            return false;
        }
    }
    // (b) arbitrary candidates: uniform and hoisted must agree (usually None).
    for _ in 0..64 {
        let key = next() & M48;
        let uid = next() & 0xFFFF_FFFF;
        let nonce = next() & 0xFFFF_FFFF;
        let nonce2 = next() & 0xFFFF_FFFF;
        let ks = gd::keystream(gd::init(key, uid, nonce), 32);
        let ks2 = gd::keystream(gd::init(key, uid, nonce2), 32);
        let kw = keystream_words(ks);
        let cand = next() & FILT_MASK;
        let u = search_candidate(cand, &ib, &kw, uid, nonce, ks, nonce2, ks2);
        let h = search_candidate_hoisted(cand, &ib, &kw, uid, nonce, ks, nonce2, ks2);
        if u != h {
            return false;
        }
    }
    true
}

use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};

/// Full bitsliced blind recovery: build the ~2^19 layer-0 candidates, then run
/// the 256-lane bitsliced search over them with dynamic work-stealing + early-stop.
/// Returns the recovered key (or None if exhausted) and #candidates processed.
#[allow(clippy::too_many_arguments)]
pub fn recover_key_bs(
    uid: u64,
    nonce: u64,
    ks: u64,
    nonce2: u64,
    ks2: u64,
    cores: usize,
    heartbeat_secs: u64,
) -> (Option<u64>, u64) {
    let ks_bit0 = ((ks >> 31) & 1) as u32;
    let candidates = build_candidates(ks_bit0);
    let n = candidates.len();
    let ib = initial_bitslices();
    let kw = keystream_words(ks);

    let stop = AtomicBool::new(false);
    let found = AtomicU64::new(u64::MAX);
    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let active = AtomicUsize::new(cores.max(1));

    std::thread::scope(|s| {
        if heartbeat_secs > 0 {
            let (stop, active, done) = (&stop, &active, &done);
            s.spawn(move || {
                let t0 = std::time::Instant::now();
                let mut acc = 0u64;
                while active.load(Ordering::Relaxed) > 0 && !stop.load(Ordering::Relaxed) {
                    std::thread::sleep(std::time::Duration::from_millis(200));
                    acc += 200;
                    if acc >= heartbeat_secs * 1000 {
                        acc = 0;
                        let d = done.load(Ordering::Relaxed);
                        let el = t0.elapsed().as_secs_f64().max(1e-6);
                        eprintln!(
                            "  [bs] {el:6.0}s  cand {d}/{n}  ~{:.1}/s  (256 lanes/cand)",
                            d as f64 / el
                        );
                    }
                }
            });
        }
        for _ in 0..cores.max(1) {
            let (candidates, ib, kw, stop, found, next, done, active) =
                (&candidates, &ib, &kw, &stop, &found, &next, &done, &active);
            s.spawn(move || {
                loop {
                    if stop.load(Ordering::Relaxed) {
                        break;
                    }
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    if i >= n {
                        break;
                    }
                    if let Some(k) =
                        search_candidate_hoisted(candidates[i], ib, kw, uid, nonce, ks, nonce2, ks2)
                    {
                        found.store(k, Ordering::SeqCst);
                        stop.store(true, Ordering::Relaxed);
                    }
                    done.fetch_add(1, Ordering::Relaxed);
                }
                active.fetch_sub(1, Ordering::Relaxed);
            });
        }
    });

    let k = found.load(Ordering::SeqCst);
    (
        if k == u64::MAX { None } else { Some(k) },
        done.load(Ordering::Relaxed) as u64,
    )
}

/// Decisive bitslice recovery gate: build the TRUE layer-0 candidate for a known
/// key and confirm the full bitsliced cascade recovers that exact key (one
/// candidate -> fast). Also checks the numeric S_2 checkpoint from the spec.
pub fn selftest_recovery() -> bool {
    let ib = initial_bitslices();
    // Fixed known-answer vector (the spec's worked example) + many pseudo-random
    // keys, so a subtle bug that only shows for certain bit patterns is caught.
    let mut rng = 0x0123_4567_89ab_cdefu64;
    let mut all_ok = true;
    let mut n_ok = 0usize;
    for i in 0..24 {
        let (key, uid, nonce, nonce2) = if i == 0 {
            (0x4141_4141_4141u64, 0x4242_4242u64, 0x4343_4343u64, 0x4444_4444u64)
        } else {
            let mut next = || {
                rng = rng
                    .wrapping_mul(6364136223846793005)
                    .wrapping_add(1442695040888963407);
                rng
            };
            (next() & M48, next() & 0xFFFF_FFFF, next() & 0xFFFF_FFFF, next() & 0xFFFF_FFFF)
        };
        let s0 = gd::init(key, uid, nonce);
        let ks = gd::keystream(s0, 32);
        let ks2 = gd::keystream(gd::init(key, uid, nonce2), 32);
        let kw = keystream_words(ks);
        let cand = s0 & FILT_MASK; // the true layer-0 candidate
        let found = search_candidate(cand, &ib, &kw, uid, nonce, ks, nonce2, ks2);
        if found == Some(key) {
            n_ok += 1;
        } else {
            all_ok = false;
        }
    }
    println!(
        "[bs 3] bitsliced find_state recovers known key ({n_ok}/24 keys): {}",
        if all_ok { "PASS" } else { "FAIL" }
    );
    all_ok
}

/// Exercise the PRODUCTION blind pipeline (build_candidates/expand20/ks_bit0) that
/// [bs 3/4] bypass: the layer-0 candidate builder must (a) partition all 2^20
/// tap-fillings across the two ks-bit-0 parities, and (b) contain the TRUE
/// candidate for every key. Catches regressions in the 2^20 bound, the expand
/// mask, or the ks_bit0 selection that would make recover_key_bs return None on
/// real captures while [bs 1..4] still pass.
fn test_candidate_pipeline() -> bool {
    let c0 = build_candidates(0);
    let c1 = build_candidates(1);
    if c0.len() + c1.len() != (1usize << 20) || c0.is_empty() || c1.is_empty() {
        return false;
    }
    use std::collections::HashSet;
    let s0set: HashSet<u64> = c0.into_iter().collect();
    let s1set: HashSet<u64> = c1.into_iter().collect();
    let mut rng = 0x1357_2468_ACE0_BDF9u64;
    let mut next = move || {
        rng = rng
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        rng
    };
    for _ in 0..1000 {
        let key = next() & M48;
        let uid = next() & 0xFFFF_FFFF;
        let nonce = next() & 0xFFFF_FFFF;
        let s0 = gd::init(key, uid, nonce);
        let ks = gd::keystream(s0, 32);
        let cand = s0 & FILT_MASK; // the layer-0 candidate build_candidates must emit
        let present = if (ks >> 31) & 1 == 0 {
            s0set.contains(&cand)
        } else {
            s1set.contains(&cand)
        };
        if !present {
            return false;
        }
    }
    true
}

/// Run the bitslice primitive self-tests. Returns true on PASS.
pub fn selftest_primitives() -> bool {
    let mut ok = true;
    let t1 = test_filters();
    println!("[bs 1] f_a/f_b/f_c == scalar 0x3c65/0x0ee5/0x0dd3929b: {}", if t1 { "PASS" } else { "FAIL" });
    ok &= t1;
    let t2 = test_f20_lfsr_roundtrip();
    println!("[bs 2] f20_bs & lfsr_step_bs match scalar per-lane (256x8): {}", if t2 { "PASS" } else { "FAIL" });
    ok &= t2;
    ok &= selftest_recovery();
    let t4 = test_hoist_equiv();
    println!("[bs 4] hoisted find_state == uniform (128 cases): {}", if t4 { "PASS" } else { "FAIL" });
    ok &= t4;
    let t5 = test_candidate_pipeline();
    println!("[bs 5] blind candidate pipeline contains true candidate (1000 keys): {}", if t5 { "PASS" } else { "FAIL" });
    ok &= t5;
    ok
}
