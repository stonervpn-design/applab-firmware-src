//! Guess-and-determine state recovery for Hitag-2 (the "actual cryptanalysis").
//!
//! This is the reduction that turns a 2^48 key search into a pruned guess-and-
//! determine tree, ported from Aram Verstegen's `hitag2hell` (WOOT'18) — the
//! same algorithm the Python reference in `tools/hitag2/crack.py` +
//! `ref_tabled.py` validates. The heart of the speedup is the **tabled**
//! reduction: for every layer, precompute the filter-tap fills whose `f20`
//! matches the target keystream bit, keyed by the tap bits already determined by
//! earlier layers. Instead of re-scanning 2^20 fills per node, the recursion
//! does a lookup.
//!
//! The lookup structure is a flat `(old_bits, fill)` table **sorted by key** and
//! binary-searched — cache-friendly, no hashing in the hot path. That sort is
//! the piece this module adds on top of the firmware-convention cipher in
//! `main.rs`.
//!
//! Bit-order note: this module deliberately uses the *hitag2hell* convention
//! (matching `hitag2.py`), NOT the firmware convention in `main.rs`. Both are
//! valid Hitag-2; the GD masks/fills constants (`0x5806b4a2d16c`,
//! `0xce0044c101cd`) are only correct in this convention, so the whole reduction
//! + the `invert_init` key step live here self-consistently and are validated by
//! a synthetic round-trip (known key -> keystream -> recovered key).

const MASK48: u64 = (1 << 48) - 1;

/// 20-bit filter-tap mask (the positions `f20` reads), hitag2hell convention.
const FILT_MASK: u64 = 0x5806_b4a2_d16c;
/// LFSR feedback-tap mask; used by `generate_masks` to decide when the incoming
/// bit is determined (clock forward) vs. must be guessed both ways.
const LFSR_MASK: u64 = 0xce00_44c1_01cd;

// ---------------------------------------------------------------------------
// Cipher core — verbatim port of tools/hitag2/hitag2.py (hitag2hell convention)
// ---------------------------------------------------------------------------

#[inline(always)]
fn i4(x: u64, a: u32, b: u32, c: u32, d: u32) -> u32 {
    ((((x >> a) & 1) as u32) << 3)
        | ((((x >> b) & 1) as u32) << 2)
        | ((((x >> c) & 1) as u32) << 1)
        | (((x >> d) & 1) as u32)
}

#[inline(always)]
pub fn f20(s: u64) -> u32 {
    let f0 = (0x3c65u32 >> i4(s, 2, 3, 5, 6)) & 1;
    let f1 = (0x0ee5u32 >> i4(s, 8, 12, 14, 15)) & 1;
    let f2 = (0x0ee5u32 >> i4(s, 17, 21, 23, 26)) & 1;
    let f3 = (0x0ee5u32 >> i4(s, 28, 29, 31, 33)) & 1;
    let f4 = (0x3c65u32 >> i4(s, 34, 43, 44, 46)) & 1;
    (0x0dd3_929bu32 >> ((f0 << 4) | (f1 << 3) | (f2 << 2) | (f3 << 1) | f4)) & 1
}

#[inline(always)]
fn lfsr_feedback(s: u64) -> u64 {
    ((s >> 0)
        ^ (s >> 2)
        ^ (s >> 3)
        ^ (s >> 6)
        ^ (s >> 7)
        ^ (s >> 8)
        ^ (s >> 16)
        ^ (s >> 22)
        ^ (s >> 23)
        ^ (s >> 26)
        ^ (s >> 30)
        ^ (s >> 41)
        ^ (s >> 42)
        ^ (s >> 43)
        ^ (s >> 46)
        ^ (s >> 47))
        & 1
}

#[inline(always)]
pub fn lfsr(s: u64) -> u64 {
    (s >> 1) | (lfsr_feedback(s) << 47)
}

#[inline(always)]
fn lfsr_feedback_inv(s: u64) -> u64 {
    ((s >> 47)
        ^ (s >> 1)
        ^ (s >> 2)
        ^ (s >> 5)
        ^ (s >> 6)
        ^ (s >> 7)
        ^ (s >> 15)
        ^ (s >> 21)
        ^ (s >> 22)
        ^ (s >> 25)
        ^ (s >> 29)
        ^ (s >> 40)
        ^ (s >> 41)
        ^ (s >> 42)
        ^ (s >> 45)
        ^ (s >> 46))
        & 1
}

#[inline(always)]
pub fn lfsr_inv(s: u64) -> u64 {
    ((s << 1) | lfsr_feedback_inv(s)) & MASK48
}

/// key/uid/nonce (48/32/32-bit) -> 48-bit post-init state.
pub fn init(key: u64, uid: u64, nonce: u64) -> u64 {
    let mut state = 0u64;
    for i in 32..48u32 {
        state = (state << 1) | ((key >> i) & 1);
    }
    for i in 0..32u32 {
        state = (state << 1) | ((uid >> i) & 1);
    }
    for i in 0..32u32 {
        let nonce_bit = (f20(state) as u64) ^ ((nonce >> (31 - i)) & 1);
        state = (state >> 1) | (((nonce_bit ^ (key >> (31 - i))) & 1) << 47);
    }
    state
}

/// `length` keystream bits from a post-init state (MSB first).
pub fn keystream(state: u64, length: u32) -> u64 {
    let mut c = 0u64;
    let mut st = state;
    for _ in 0..length {
        c = (c << 1) | (f20(st) as u64);
        st = lfsr(st);
    }
    c
}

/// Recover the 48-bit key from the post-init state + known uid/nonce.
pub fn invert_init(post_state: u64, uid: u64, nonce: u64) -> u64 {
    const LOW47: u64 = (1 << 47) - 1;
    let mut state = post_state;
    let mut key = 0u64;
    for i in (0..32u32).rev() {
        let b = (state >> 47) & 1;
        let dropped = (uid >> (31 - i)) & 1;
        state = (((state & LOW47) << 1) | dropped) & MASK48; // = S_i
        let key_bit = b ^ (f20(state) as u64) ^ ((nonce >> (31 - i)) & 1);
        key |= key_bit << (31 - i);
    }
    for j in 0..16u32 {
        key |= ((state >> (47 - j)) & 1) << (32 + j);
    }
    key & MASK48
}

// ---------------------------------------------------------------------------
// Layer masks + the sorted fill-tables (the reduction)
// ---------------------------------------------------------------------------

/// Per-layer fresh-bit masks and the count of leading layers where the incoming
/// LFSR bit must be guessed both ways (feedback not yet fully determined).
fn generate_masks() -> (Vec<u64>, usize) {
    let mut filt_mask = FILT_MASK;
    let mut masks = Vec::new();
    let mut fill = 0u64;
    let mut last_lfsr_guess = 0usize;
    while fill != 0xffff_ffff_ffff {
        masks.push(filt_mask);
        fill |= filt_mask;
        if (fill & LFSR_MASK) != LFSR_MASK {
            last_lfsr_guess += 1;
        }
        fill >>= 1;
        fill |= 1 << 47;
        filt_mask &= !fill;
    }
    (masks, last_lfsr_guess)
}

/// Scatter the low bits of `x` into the positions selected by `mask`.
#[inline]
fn expand(mut mask: u64, mut x: u64) -> u64 {
    let mut res = 0u64;
    for i in 0..48u32 {
        if mask & 1 == 1 {
            res |= (x & 1) << i;
            x >>= 1;
        }
        mask >>= 1;
    }
    res
}

/// Parallel bit-extract: gather the `mask`-selected bits of `val` into a dense
/// index. BMI2 PEXT on x86_64 (one instruction on Raptor Lake; the crate is
/// built `-C target-cpu=native`, see `.cargo/config.toml`), scalar fallback
/// elsewhere. This is the per-node table key, so it must be ~free.
#[inline(always)]
fn pext(val: u64, mask: u64) -> u64 {
    #[cfg(target_arch = "x86_64")]
    {
        // Runtime-detected, so the binary is CORRECT even without target-cpu=native
        // (a generic build, or an old x86 CPU). PEXT is UB without BMI2, so this
        // guard is a soundness requirement, not merely a perf toggle. The check is
        // a cached atomic read + a predictable branch — negligible on the hot path.
        if std::is_x86_feature_detected!("bmi2") {
            // SAFETY: reached only when the runtime BMI2 check above passed.
            return unsafe { core::arch::x86_64::_pext_u64(val, mask) };
        }
    }
    pext_scalar(val, mask)
}

/// Portable scalar parallel-extract. Always compiled (used on non-x86_64, and as
/// the x86_64 fallback when BMI2 is absent).
#[inline]
fn pext_scalar(val: u64, mask: u64) -> u64 {
    let (mut r, mut b, mut m) = (0u64, 0u32, mask);
    while m != 0 {
        let bit = m & m.wrapping_neg();
        if val & bit != 0 {
            r |= 1 << b;
        }
        b += 1;
        m &= m - 1;
    }
    r
}

/// One layer's fill-table in compressed-sparse-row form. The old (already-
/// determined) tap bits compress via PEXT to a dense `idx`; `fills[starts[idx]
/// ..starts[idx+1]]` are the fresh-tap fills whose `f20` matches this layer's
/// keystream bit. O(1) lookup, ~half the memory of the (old,fill) array, and one
/// cache line per node instead of a ~19-probe binary search into 8 MB.
struct LayerTable {
    old_mask: u64,
    starts: Vec<u32>,
    fills: Vec<u64>,
}

impl LayerTable {
    #[inline(always)]
    fn get(&self, key: u64) -> &[u64] {
        let idx = pext(key, self.old_mask) as usize;
        &self.fills[self.starts[idx] as usize..self.starts[idx + 1] as usize]
    }
}

// ---------------------------------------------------------------------------
// The reduction: build the per-layer compressed fill-tables
// ---------------------------------------------------------------------------

/// For every layer, enumerate all 2^20 fillings of the 20 filter taps, keep
/// those whose `f20` equals this layer's target keystream bit, and bucket them
/// by their PEXT-compressed `old` bits (a counting sort into CSR). Same fills as
/// the sorted-array reduction, laid out for O(1) cache-friendly lookup.
fn build_tables(ks_bits: &[u32], masks: &[u64]) -> Vec<LayerTable> {
    let nlayers = masks.len();
    let old_masks: Vec<u64> = masks.iter().map(|&m| FILT_MASK & !m).collect();
    let nidx: Vec<usize> = old_masks.iter().map(|&m| 1usize << m.count_ones()).collect();

    // Pass 1 — count fills per (layer, idx).
    let mut counts: Vec<Vec<u32>> = nidx.iter().map(|&n| vec![0u32; n]).collect();
    for i in 0..(1u64 << 20) {
        let taps_fill = expand(FILT_MASK, i);
        let out = f20(taps_fill);
        for layer in 0..nlayers {
            if out == ks_bits[layer] {
                let idx = pext(taps_fill, old_masks[layer]) as usize;
                counts[layer][idx] += 1;
            }
        }
    }

    // Prefix-sum each layer's counts into CSR `starts`, allocate `fills`.
    let mut starts: Vec<Vec<u32>> = Vec::with_capacity(nlayers);
    let mut fills: Vec<Vec<u64>> = Vec::with_capacity(nlayers);
    for layer in 0..nlayers {
        let n = nidx[layer];
        let mut st = vec![0u32; n + 1];
        for k in 0..n {
            st[k + 1] = st[k] + counts[layer][k];
        }
        fills.push(vec![0u64; st[n] as usize]);
        starts.push(st);
    }

    // Pass 2 — scatter fills into place (reuse a per-idx cursor).
    let mut cursor: Vec<Vec<u32>> = starts.iter().map(|s| s[..s.len() - 1].to_vec()).collect();
    for i in 0..(1u64 << 20) {
        let taps_fill = expand(FILT_MASK, i);
        let out = f20(taps_fill);
        for layer in 0..nlayers {
            if out == ks_bits[layer] {
                let idx = pext(taps_fill, old_masks[layer]) as usize;
                let pos = cursor[layer][idx];
                cursor[layer][idx] = pos + 1;
                fills[layer][pos as usize] = taps_fill & masks[layer];
            }
        }
    }

    (0..nlayers)
        .map(|layer| LayerTable {
            old_mask: old_masks[layer],
            starts: std::mem::take(&mut starts[layer]),
            fills: std::mem::take(&mut fills[layer]),
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Threaded blind key recovery (parallelize over layer-0 fills + early-stop)
// ---------------------------------------------------------------------------

use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};

/// Shared, read-only search context handed to every worker.
struct Ctx<'a> {
    tables: &'a [LayerTable],
    /// Per layer: the single fresh tap bit if this layer fills exactly one (so we
    /// resolve it with two inline `f20` evals instead of a table probe), else 0.
    single: &'a [u64],
    last_lfsr_guess: usize,
    nlayers: usize,
    ksb: &'a [u32],
    // Two-frame target for disambiguation (a 32-bit ks pins several states).
    uid: u64,
    nonce: u64,
    ks: u64,
    nonce2: u64,
    ks2: u64,
    // Shared coordination.
    stop: &'a AtomicBool,
    found: &'a AtomicU64,
    total_nodes: &'a AtomicU64,
}

/// One worker: its own node counter, flushed to the shared total periodically so
/// the heartbeat sees live progress.
struct Finder<'a> {
    cx: &'a Ctx<'a>,
    nodes: u64,
    flushed: u64,
}

impl<'a> Finder<'a> {
    #[inline]
    fn flush(&mut self) {
        let delta = self.nodes - self.flushed;
        if delta > 0 {
            self.cx.total_nodes.fetch_add(delta, Ordering::Relaxed);
            self.flushed = self.nodes;
        }
    }

    fn test(&mut self, mut state: u64) {
        for bit in self.cx.nlayers..32usize {
            if f20(state) != self.cx.ksb[bit] {
                return;
            }
            state = lfsr(state);
        }
        for _ in 0..32 {
            state = lfsr_inv(state);
        }
        // `state` is now a post-init state consistent with frame 1. Invert to the
        // key and confirm it reproduces BOTH frames before accepting.
        let key = invert_init(state, self.cx.uid, self.cx.nonce);
        if keystream(init(key, self.cx.uid, self.cx.nonce), 32) == self.cx.ks
            && keystream(init(key, self.cx.uid, self.cx.nonce2), 32) == self.cx.ks2
        {
            self.cx.found.store(key, Ordering::SeqCst);
            self.cx.stop.store(true, Ordering::Relaxed);
        }
    }

    fn recurse(&mut self, state: u64, layer: usize) {
        self.nodes += 1;
        if self.nodes & 0xFFFFF == 0 {
            self.flush();
            if self.cx.stop.load(Ordering::Relaxed) {
                return;
            }
        }
        if layer < self.cx.nlayers {
            let cx = self.cx; // Copy the shared &Ctx out so the fills borrow isn't
                              // tied to `self` (frees the `&mut self` recursion).
            let sb = cx.single[layer];
            if sb != 0 {
                // Single-bit layer: the 19 other filter taps are already in `state`;
                // the fresh bit is `sb`. Compute which of {0, sb} keep f20 == ks
                // inline — no memory probe, which is where the deep tree spends its
                // time.
                let want = cx.ksb[layer];
                for cand in [state, state | sb] {
                    if f20(cand) == want {
                        if layer < cx.last_lfsr_guess {
                            self.recurse(cand >> 1, layer + 1);
                            self.recurse((cand >> 1) | (1 << 47), layer + 1);
                        } else {
                            self.recurse(lfsr(cand), layer + 1);
                        }
                    }
                }
            } else {
                let fills = cx.tables[layer].get(state & FILT_MASK);
                for &efill in fills {
                    let new_state = state | efill;
                    if layer < cx.last_lfsr_guess {
                        self.recurse(new_state >> 1, layer + 1);
                        self.recurse((new_state >> 1) | (1 << 47), layer + 1);
                    } else {
                        self.recurse(lfsr(new_state), layer + 1);
                    }
                }
            }
        } else {
            self.test(state);
        }
    }

    /// Expand one layer-0 seed into its two (guess) or one (determined) subtree.
    fn seed(&mut self, new_state: u64) {
        if self.cx.last_lfsr_guess > 0 {
            self.recurse(new_state >> 1, 1);
            self.recurse((new_state >> 1) | (1 << 47), 1);
        } else {
            self.recurse(lfsr(new_state), 1);
        }
    }
}

/// Blind RKE key recovery from two frames of the SAME fob (same uid).
///
/// Parallel over layer-0 fills with **dynamic work-stealing** (an atomic seed
/// index — GD subtrees are wildly uneven, so static chunking starves cores) and
/// early-stop the instant any worker inverts a state to a key that reproduces
/// both frames. A heartbeat thread reports live node rate to stderr every
/// `heartbeat_secs` (0 = silent). Returns the recovered key (or None if the tree
/// is fully exhausted) and total tree nodes visited.
#[allow(clippy::too_many_arguments)]
pub fn recover_key_mt(
    uid: u64,
    nonce: u64,
    ks: u64,
    nonce2: u64,
    ks2: u64,
    cores: usize,
    heartbeat_secs: u64,
) -> (Option<u64>, u64) {
    let (masks, last_lfsr_guess) = generate_masks();
    let nlayers = masks.len();
    let ksb: Vec<u32> = (0..32u32).map(|i| ((ks >> (31 - i)) & 1) as u32).collect();
    let tables = build_tables(&ksb, &masks);
    // Layers that fill exactly one tap bit are resolved inline (see recurse).
    let single: Vec<u64> = masks
        .iter()
        .map(|&m| if m.count_ones() == 1 { m } else { 0 })
        .collect();

    let stop = AtomicBool::new(false);
    let found = AtomicU64::new(u64::MAX);
    let total_nodes = AtomicU64::new(0);
    // Dynamic seed dispenser: layer 0's old_mask is empty (idx 0 only), so
    // tables[0].fills IS the full set of layer-0 seeds.
    let next = AtomicUsize::new(0);
    let active = AtomicUsize::new(cores.max(1));
    let n0 = tables[0].fills.len();

    let cx = Ctx {
        tables: &tables,
        single: &single,
        last_lfsr_guess,
        nlayers,
        ksb: &ksb,
        uid,
        nonce,
        ks,
        nonce2,
        ks2,
        stop: &stop,
        found: &found,
        total_nodes: &total_nodes,
    };

    std::thread::scope(|s| {
        // Heartbeat.
        if heartbeat_secs > 0 {
            let (stop, active, total_nodes, next) = (&stop, &active, &total_nodes, &next);
            s.spawn(move || {
                let t0 = std::time::Instant::now();
                let mut acc_ms = 0u64;
                // Poll in short slices so the scope isn't held open for a full
                // interval after the workers finish (which would inflate the timed
                // wall-clock main.rs measures around this call).
                while active.load(Ordering::Relaxed) > 0 && !stop.load(Ordering::Relaxed) {
                    std::thread::sleep(std::time::Duration::from_millis(200));
                    acc_ms += 200;
                    if acc_ms >= heartbeat_secs * 1000 {
                        acc_ms = 0;
                        let n = total_nodes.load(Ordering::Relaxed);
                        let el = t0.elapsed().as_secs_f64().max(1e-6);
                        let seed = next.load(Ordering::Relaxed).min(n0);
                        eprintln!(
                            "  [gd] {el:6.0}s  {n:>14} nodes  ~{:.2e}/s  seeds {seed}/{n0}",
                            n as f64 / el
                        );
                    }
                }
            });
        }
        // Workers.
        for _ in 0..cores.max(1) {
            let (cx, next, active) = (&cx, &next, &active);
            s.spawn(move || {
                let mut f = Finder { cx, nodes: 0, flushed: 0 };
                loop {
                    if cx.stop.load(Ordering::Relaxed) {
                        break;
                    }
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    if i >= n0 {
                        break;
                    }
                    f.seed(cx.tables[0].fills[i]); // state at layer 0 is 0
                }
                f.flush();
                active.fetch_sub(1, Ordering::Relaxed);
            });
        }
    });

    let k = found.load(Ordering::SeqCst);
    (
        if k == u64::MAX { None } else { Some(k) },
        total_nodes.load(Ordering::Relaxed),
    )
}

/// Guided recovery — the fast correctness gate (mirrors `crack.py`
/// `_guided_recover`). Prunes the search to the *true* state's tap bits so it
/// runs in O(layers), proving the reduction reproduces `true_state`.
pub fn recover_states_guided(ks32: u64, true_state: u64, ksbits: u32) -> Vec<u64> {
    let (masks, last_lfsr_guess) = generate_masks();
    let nlayers = masks.len();
    let ksb: Vec<u32> = (0..ksbits)
        .map(|i| ((ks32 >> (ksbits - 1 - i)) & 1) as u32)
        .collect();
    // Advance the true state one clock per layer so we can match its filter bits.
    let mut test_states = Vec::with_capacity(nlayers);
    {
        let mut s = true_state;
        for _ in 0..nlayers {
            test_states.push(s);
            s = lfsr(s);
        }
    }
    let nbits: Vec<u32> = masks.iter().map(|m| m.count_ones()).collect();
    let mut results = Vec::new();

    fn test(mut state: u64, nlayers: usize, ksbits: u32, ksb: &[u32], out: &mut Vec<u64>) {
        for bit in nlayers..(ksbits as usize) {
            if f20(state) != ksb[bit] {
                return;
            }
            state = lfsr(state);
        }
        for _ in 0..ksbits {
            state = lfsr_inv(state);
        }
        out.push(state);
    }

    #[allow(clippy::too_many_arguments)]
    fn fill_layer(
        state: u64,
        layer: usize,
        masks: &[u64],
        nbits: &[u32],
        test_states: &[u64],
        last_lfsr_guess: usize,
        nlayers: usize,
        ksbits: u32,
        ksb: &[u32],
        out: &mut Vec<u64>,
    ) {
        if layer < nlayers {
            for fill in 0..(1u64 << nbits[layer]) {
                let new_state = state | expand(masks[layer], fill);
                if (new_state & FILT_MASK) != (test_states[layer] & FILT_MASK) {
                    continue;
                }
                if f20(new_state) != ksb[layer] {
                    continue;
                }
                if layer < last_lfsr_guess {
                    fill_layer(new_state >> 1, layer + 1, masks, nbits, test_states, last_lfsr_guess, nlayers, ksbits, ksb, out);
                    fill_layer((new_state >> 1) | (1 << 47), layer + 1, masks, nbits, test_states, last_lfsr_guess, nlayers, ksbits, ksb, out);
                } else {
                    fill_layer(lfsr(new_state), layer + 1, masks, nbits, test_states, last_lfsr_guess, nlayers, ksbits, ksb, out);
                }
            }
        } else {
            test(state, nlayers, ksbits, ksb, out);
        }
    }

    fill_layer(0, 0, &masks, &nbits, &test_states, last_lfsr_guess, nlayers, ksbits, &ksb, &mut results);
    results
}

/// Verify the fill-tables directly: walk the true state's path and assert that
/// every layer's table, looked up by the determined tap bits, contains the fresh
/// tap-bit fill the true state requires. Proves the *reduction* (compress +
/// lookup) is correct without a full blind run. Returns Ok, or the failing layer.
pub fn verify_tables_along_path(ks32: u64, true_state: u64, ksbits: u32) -> Result<usize, usize> {
    let (masks, last_lfsr_guess) = generate_masks();
    let nlayers = masks.len();
    let ksb: Vec<u32> = (0..ksbits)
        .map(|i| ((ks32 >> (ksbits - 1 - i)) & 1) as u32)
        .collect();
    let tables = build_tables(&ksb, &masks);

    // Reconstruct the true per-layer `state` the blind recursion would hold.
    let mut state = 0u64;
    let mut true_at_layer = true_state; // true state clocked forward per layer
    for layer in 0..nlayers {
        let key = state & FILT_MASK;
        let want_fill = true_at_layer & masks[layer]; // fresh tap bits this layer
        let hits = tables[layer].get(key);
        if !hits.iter().any(|&n| n == want_fill) {
            return Err(layer);
        }
        let new_state = state | want_fill;
        // Follow the branch the true state actually takes.
        state = if layer < last_lfsr_guess {
            // Choose the guess matching the true incoming bit.
            let next_true = lfsr(true_at_layer);
            let bit47 = (next_true >> 47) & 1;
            (new_state >> 1) | (bit47 << 47)
        } else {
            lfsr(new_state)
        };
        true_at_layer = lfsr(true_at_layer);
    }
    Ok(nlayers)
}

/// Differential check of the PRODUCTION inline single-bit path against the
/// validated table. Finder::recurse resolves single-bit layers with
/// `for cand in [state, state|sb] { if f20(cand)==ks ... }` instead of a table
/// probe; gates 4/5 never run that branch. For every single-bit layer and many
/// pseudo-random assignments of the already-determined taps, the fresh fills the
/// inline test accepts must equal the table's bucket for that key. Returns
/// Ok(#single-bit layers checked) or Err(failing layer).
pub fn verify_inline_equiv(ks32: u64) -> Result<usize, usize> {
    let (masks, _) = generate_masks();
    let ksb: Vec<u32> = (0..32u32).map(|i| ((ks32 >> (31 - i)) & 1) as u32).collect();
    let tables = build_tables(&ksb, &masks);
    let mut rng = 0x9E37_79B9_7F4A_7C15u64;
    let mut checked = 0usize;
    for (layer, &m) in masks.iter().enumerate() {
        if m.count_ones() != 1 {
            continue; // multi-bit layers still use the table (covered by gate 4)
        }
        checked += 1;
        let sb = m;
        let old_mask = FILT_MASK & !m;
        let want = ksb[layer];
        for _ in 0..8192 {
            // Deterministic pseudo-random assignment of the already-fixed taps; the
            // fresh bit `sb` stays 0, mirroring layer entry.
            rng = rng
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            let state = expand(old_mask, rng);
            // Inline path — byte-for-byte what Finder::recurse's single-bit branch does.
            let mut inline: Vec<u64> = [state, state | sb]
                .iter()
                .filter(|&&c| f20(c) == want)
                .map(|&c| c & sb) // fresh-bit fill: 0 or sb
                .collect();
            // Table path.
            let mut table: Vec<u64> = tables[layer].get(state & FILT_MASK).to_vec();
            inline.sort_unstable();
            table.sort_unstable();
            if inline != table {
                return Err(layer);
            }
        }
    }
    Ok(checked)
}

// ---------------------------------------------------------------------------
// Self-tests
// ---------------------------------------------------------------------------

/// The correctness gate: known key/uid/nonce -> keystream -> recover the state
/// (guided) -> invert_init -> key, and prove the sorted tables carry the true
/// path. Mirrors `tools/hitag2/crack.py` `_selftest`. Returns true on PASS.
pub fn selftest() -> bool {
    let mut ok = true;

    // 1. cipher determinism + hitag2hell reference vector.
    let (key, uid, nonce) = (0x4141_4141_4141u64, 0x4242_4242u64, 0x4343_4343u64);
    let true_state = init(key, uid, nonce);
    let ks = keystream(true_state, 32);
    println!("[gd 1] init state = {true_state:012x}  ks32 = {ks:08x}");

    // 2. lfsr / lfsr_inv round-trip.
    let mut adv = true_state;
    for _ in 0..32 {
        adv = lfsr(adv);
    }
    for _ in 0..32 {
        adv = lfsr_inv(adv);
    }
    let r2 = adv == true_state;
    println!("[gd 2] lfsr/lfsr_inv round-trip: {}", if r2 { "PASS" } else { "FAIL" });
    ok &= r2;

    // 3. invert_init round-trip on independent vector.
    let (k3, u3, n3) = (0x0123_4567_89abu64, 0xdead_beefu64, 0x0bad_f00du64);
    let rec3 = invert_init(init(k3, u3, n3), u3, n3);
    let r3 = rec3 == k3;
    println!("[gd 3] invert_init: key={k3:012x} recovered={rec3:012x} {}", if r3 { "PASS" } else { "FAIL" });
    ok &= r3;

    // 4. the reduction: sorted tables must carry the true path.
    let r4 = matches!(verify_tables_along_path(ks, true_state, 32), Ok(_));
    println!("[gd 4] sorted fill-tables carry true path: {}", if r4 { "PASS" } else { "FAIL" });
    ok &= r4;

    // 5. guided recovery must yield the true state, and invert_init -> key with a
    //    second frame disambiguating.
    let states = recover_states_guided(ks, true_state, 32);
    let has_true = states.contains(&true_state);
    let nonce2 = 0x4444_4444u64;
    let ks2 = keystream(init(key, uid, nonce2), 32);
    let mut recovered = None;
    for &st in &states {
        let cand = invert_init(st, uid, nonce);
        if keystream(init(cand, uid, nonce), 32) == ks
            && keystream(init(cand, uid, nonce2), 32) == ks2
        {
            recovered = Some(cand);
            break;
        }
    }
    let r5 = has_true && recovered == Some(key);
    println!(
        "[gd 5] guided recovered {} state(s); true present={has_true}; key={} {}",
        states.len(),
        recovered.map(|k| format!("{k:012x}")).unwrap_or_else(|| "None".into()),
        if r5 { "PASS" } else { "FAIL" }
    );
    ok &= r5;

    // 6. the PRODUCTION inline single-bit path must equal the validated table
    //    (the blind recovery runs the inline branch; gates 4/5 do not).
    let r6 = match verify_inline_equiv(ks) {
        Ok(n) => {
            println!("[gd 6] inline single-bit path == table over {n} layers x8192: PASS");
            true
        }
        Err(layer) => {
            println!("[gd 6] inline single-bit path DIVERGES from table at layer {layer}: FAIL");
            false
        }
    };
    ok &= r6;

    ok
}
