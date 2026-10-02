/*
 * NocSif — WEP (RC4) key recovery, portable statistics core.
 *
 * Recovers a 40-bit (5-byte) or 104-bit (13-byte) WEP secret key from captured 802.11 data
 * frames, using the published Klein/PTW and FMS correlations between the per-packet RC4 key
 * (IV || secret) and the keystream. This is the classic, well-documented capability behind
 * aircrack-ng; it works only against the long-broken RC4-based WEP scheme (IEEE 802.11-1999).
 * Intended use here is auditing/rejoining the operator's OWN WEP network.
 *
 * This translation unit is PURE COMPUTATION: no platform, OS, radio, RTOS or LVGL dependency,
 * only <stdint.h>/<stdlib.h>/<string.h>. The NocSif firmware does the radio side (promiscuous
 * capture on the target channel, optional interactive ARP replay to farm IVs, deriving the
 * keystream bytes) and feeds samples in here; on RECOVERED it stores the key and rejoins via
 * nocsif_wifi_request_connect(). Because it is self-contained it also builds and self-tests on
 * a host (see WEP_RECOVER_SELFTEST at the bottom of wep_recover.c).
 *
 * How a caller derives the keystream (the bit this module consumes):
 *   WEP encrypts RC4(IV||K) XOR plaintext. LLC/SNAP-encapsulated frames begin with the fixed
 *   plaintext  AA AA 03 00 00 00 <EtherType_hi> <EtherType_lo>  (ARP => 08 06, IPv4 => 08 00),
 *   and ARP request/reply bodies are almost entirely predictable. XOR that known plaintext
 *   against the encrypted bytes to recover keystream[k] = RC4(IV||K)[k]. Each captured frame
 *   thus yields several keystream bytes at fixed offsets. The recovery needs keystream index
 *   (l-1) to solve secret byte l (l = 3..key_len+2), i.e. keystream[2..key_len+1]:
 *     - 40-bit  (5 secret bytes):  keystream[0..6]   (>= 7 bytes)   -> LLC/SNAP alone is enough
 *     - 104-bit (13 secret bytes): keystream[0..14]  (>= 15 bytes)  -> needs the ARP body too
 *   keystream[0] additionally feeds the FMS weak-IV votes and key verification.
 *
 * Threading / real-time: add_sample() is cheap (dedup + copy) and safe to call from the capture
 * path. wep_recover_try() is the CPU-bound recovery pass — run it on a worker (PSRAM stack ok),
 * OFF any real-time/LVGL path. It never blocks and does no allocation (all buffers are claimed
 * once in wep_recover_ctx_init()).
 *
 * Sample counts (unique IVs) — see the note at the top of wep_recover.c for detail:
 *   PTW/Klein votes come from EVERY unique IV. On-device fast path: ~100k for 104-bit and ~40-50k
 *   for 40-bit lock the key in a handful of nodes; fewer may still work but a pass is node-capped
 *   and returns NEED_MORE rather than grinding, so keep farming IVs and retry. FMS only contributes
 *   from weak IVs of form (l,255,*) — rare in random IVs, common in some counters — a complementary
 *   boost, not the primary path.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- key / IV geometry -------------------------------------------------------------------- */
#define WEP_IV_LEN            3     /* per-packet IV = RC4 key bytes 0..2                        */
#define WEP_KEY40_LEN         5     /* 40-bit  WEP secret (RC4 key length 8)                     */
#define WEP_KEY104_LEN        13    /* 104-bit WEP secret (RC4 key length 16)                    */
#define WEP_KEY_MAX_LEN       WEP_KEY104_LEN

/* Keystream bytes kept per sample. Secret byte l (l = 3..key_len+2) is voted from keystream
 * index (l-1); the last 104-bit byte (l=15) needs index 14, so keep indices 0..14. */
#define WEP_SAMPLE_KS_KEEP    (WEP_KEY104_LEN + 2)   /* = 15 (indices 0..14)                     */

/* Result of feeding one packet. */
typedef enum {
    WEP_ADD_ADDED = 0,     /* stored (new unique IV)                                             */
    WEP_ADD_DUP,           /* IV already seen — ignored (dedup by IV)                            */
    WEP_ADD_FULL,          /* sample store is at capacity — ignored                              */
    WEP_ADD_BADARG,        /* NULL / ks_len too small — ignored                                  */
} wep_add_result_t;

/* Result of a recovery pass. */
typedef enum {
    WEP_RECOVER_RECOVERED = 0,  /* out_key/out_len hold a key verified against held-out samples  */
    WEP_RECOVER_NEED_MORE,      /* no key verified yet; collect ~out_need_more more unique IVs    */
    WEP_RECOVER_FAILED,         /* search budget exhausted without a verified key                 */
} wep_recover_status_t;

/* Tuning. Pass NULL to wep_recover_ctx_init_cfg (or use wep_recover_ctx_init) for the defaults. */
typedef struct {
    uint8_t  key_len;       /* WEP_KEY40_LEN, WEP_KEY104_LEN, or 0 = auto (try 104 then 40)      */
    uint32_t max_samples;   /* unique-IV capacity; 0 => WEP_RECOVER_DEFAULT_MAX_SAMPLES          */
    uint8_t  fudge;         /* brute-force breadth: candidates tried per key byte; 0 => default  */
    uint32_t max_verify;    /* cap on full-key verifications per pass; 0 => default              */
} wep_recover_cfg_t;

#define WEP_RECOVER_DEFAULT_MAX_SAMPLES  120000u
#define WEP_RECOVER_DEFAULT_FUDGE        8u
#define WEP_RECOVER_DEFAULT_MAX_VERIFY   20000u

/* Context. Treat the fields as opaque; place the struct wherever you like (it is small — the
 * large buffers are claimed on the heap by ctx_init and released by ctx_free). */
typedef struct {
    wep_recover_cfg_t cfg;
    uint8_t  *samples;      /* max_samples * WEP_SAMPLE_STRIDE bytes  (iv[3] + ks[KS_KEEP])      */
    uint32_t  n_samples;    /* unique IVs stored                                                 */
    uint32_t  cap_samples;
    uint32_t *seen;         /* open-addressing hash set of 24-bit IVs (dedup)                    */
    uint32_t  seen_mask;    /* hash capacity - 1 (power of two)                                  */
    uint32_t  seen_count;
    uint64_t  budget_used;  /* verifications spent in the last pass (diagnostic)                 */
    void    (*yield_fn)(void *);  /* optional cooperative-yield hook, called during a try() pass */
    void     *yield_arg;
    uint32_t  yield_ctr;    /* internal: samples processed since the last yield                  */
    bool      inited;
} wep_recover_ctx_t;

/* ---- lifecycle ---------------------------------------------------------------------------- */

/* Initialise with defaults. Allocates the sample store + dedup set. Returns false on OOM. */
bool wep_recover_ctx_init(wep_recover_ctx_t *ctx);

/* Initialise with an explicit config (any zero field falls back to its default). */
bool wep_recover_ctx_init_cfg(wep_recover_ctx_t *ctx, const wep_recover_cfg_t *cfg);

/* Release the buffers. Safe to call on a zeroed / already-freed ctx. */
void wep_recover_ctx_free(wep_recover_ctx_t *ctx);

/* Drop all collected samples (keeps the allocation); start a fresh collection. */
void wep_recover_reset(wep_recover_ctx_t *ctx);

/* Optional cooperative-yield hook. wep_recover_try() is a multi-second CPU-bound pass; on an RTOS
 * it must periodically let lower-priority / idle tasks run (feed the task watchdog). Set `fn` to a
 * callback that yields (e.g. vTaskDelay(1)); it is invoked every ~64k samples voted and once per
 * search node. Pass NULL (the default) for a pure-compute build (the host self-test). */
void wep_recover_set_yield(wep_recover_ctx_t *ctx, void (*fn)(void *), void *arg);

/* ---- feeding samples ---------------------------------------------------------------------- */

/* Feed one captured frame's IV + derived keystream bytes. Deduplicated by IV. keystream must
 * hold ks_len bytes (>= 1); more is better (see the header note). Cheap; capture-path safe. */
wep_add_result_t wep_recover_add_sample(wep_recover_ctx_t *ctx,
                                        const uint8_t iv[WEP_IV_LEN],
                                        const uint8_t *keystream, uint8_t ks_len);

/* Unique-IV count collected so far (progress readout). */
uint32_t wep_recover_unique_ivs(const wep_recover_ctx_t *ctx);

/* Rough 0..100 "confidence" that a recovery pass would now succeed, from the unique-IV count
 * against the count the configured key length typically needs. For a "collecting… N" UI. */
uint8_t wep_recover_progress(const wep_recover_ctx_t *ctx);

/* ---- recovery ----------------------------------------------------------------------------- */

/* Run one recovery pass (CPU-bound; call on a worker, off the hot path).
 *  - out_key must have room for WEP_KEY_MAX_LEN (13) bytes; on RECOVERED, out_len is 5 or 13.
 *  - on NEED_MORE, *out_need_more (if non-NULL) gets a rough count of additional unique IVs to
 *    collect before trying again.
 * out_key / out_len / out_need_more may each be NULL if not wanted. */
wep_recover_status_t wep_recover_try(wep_recover_ctx_t *ctx,
                                     uint8_t *out_key, uint8_t *out_len,
                                     uint32_t *out_need_more);

#ifdef __cplusplus
}
#endif
