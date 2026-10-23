/**
 * @file  shm_cap_side.c
 * @brief Implementation of the side-metadata capability (variant a).
 *
 * The key table here is the software analogue of the CapCXL CEU's
 * content-addressed table: one row per live allocation, found by matching
 * the presented key against every live row (a CAM match), not by using the
 * key as an array index. See shm_cap_side.h for why the table is
 * process-local in this reference implementation.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_cap_side.h"
#include "shm_internal.h"  /* shm_block_hdr_t, shm_blk_of_poff */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Process-local key table ─────────────────────────────────────────────── */

#define SIDE_TABLE_MAX 256u

typedef struct {
    bool     valid;
    uint64_t base;
    uint64_t bound;
    uint32_t key;      /* 0 reserved: "no capability" / free-row sentinel */
    uint32_t perms;
    uint32_t identity; /* owner; checked by rotate()/free(), never deref() */
} side_row_t;

static side_row_t g_rows[SIDE_TABLE_MAX];
static bool       g_rng_seeded = false;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void seed_rng_once(void)
{
    if (!g_rng_seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)g_rows);
        g_rng_seeded = true;
    }
}

/** True if @p key is currently held by any live row. */
static bool key_live(uint32_t key)
{
    for (uint32_t i = 0; i < SIDE_TABLE_MAX; i++)
        if (g_rows[i].valid && g_rows[i].key == key)
            return true;
    return false;
}

/**
 * Generate a fresh nonzero 30-bit key not currently held by any live row
 * (and, for rotation, not equal to @p exclude either). Bounded retry over
 * the whole nonzero 30-bit space; returns 0 (the reserved sentinel) if every
 * attempt within the retry budget collides, which the caller treats as
 * ENOSPC-equivalent.
 */
static uint32_t gen_unique_key(uint32_t exclude)
{
    seed_rng_once();
    const uint32_t key_space = SHM_CAP_SIDE_KEY_MASK >> SHM_CAP_SIDE_KEY_SHIFT; /* 0x3FFFFFFF */

    for (int attempt = 0; attempt < 64; attempt++) {
        uint32_t candidate = ((uint32_t)rand() & key_space);
        if (candidate != 0 && candidate != exclude && !key_live(candidate))
            return candidate;
    }
    return 0;
}

/**
 * Find the row index matching @p key alone (a pure CAM match). No longer
 * cross-checked against a separate allocation id -- see the ABA note on
 * shm_cap_side_t in shm_cap_side.h before assuming this is collision-proof
 * across a long-running process with heavy mint/free churn.
 */
static int find_by_key(uint32_t key)
{
    if (key == 0)
        return -1;   /* the null/sentinel key never matches a real row */
    for (uint32_t i = 0; i < SIDE_TABLE_MAX; i++)
        if (g_rows[i].valid && g_rows[i].key == key)
            return (int)i;
    return -1;
}

static int find_free_row(void)
{
    for (uint32_t i = 0; i < SIDE_TABLE_MAX; i++)
        if (!g_rows[i].valid)
            return (int)i;
    return -1;
}

static uint32_t pack_key_perms(uint32_t key, uint32_t perms)
{
    return ((key << SHM_CAP_SIDE_KEY_SHIFT) & SHM_CAP_SIDE_KEY_MASK) |
           (perms & SHM_CAP_SIDE_PERMS_MASK);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

shm_cap_side_t shm_cap_side_derive(const shm_region_t *region,
                                   shm_off_t           off,
                                   shm_perm_t          grant_perms,
                                   uint32_t            identity)
{
    if (!region)
        return SHM_CAP_SIDE_NULL;

    shm_block_hdr_t *b = shm_blk_of_poff(region, off);
    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK || !b->allocated)
        return SHM_CAP_SIDE_NULL;

    uint32_t perms = 0;
    if (grant_perms & SHM_PERM_READ)  perms |= SHM_CAP_SIDE_PERM_READ;
    if (grant_perms & SHM_PERM_WRITE) perms |= SHM_CAP_SIDE_PERM_WRITE;
    if (perms == 0)
        return SHM_CAP_SIDE_NULL;   /* nothing to grant */

    int idx = find_free_row();
    if (idx < 0)
        return SHM_CAP_SIDE_NULL;   /* table full */

    uint32_t key = gen_unique_key(0);
    if (key == 0)
        return SHM_CAP_SIDE_NULL;   /* key space exhausted (should not happen at 256 rows) */

    uint64_t base  = (uint64_t)off;
    uint64_t bound = base + (uint64_t)b->payload_cap;

    g_rows[idx].valid    = true;
    g_rows[idx].base     = base;
    g_rows[idx].bound    = bound;
    g_rows[idx].key      = key;
    g_rows[idx].perms    = perms;
    g_rows[idx].identity = identity;

    shm_cap_side_t cap;
    cap.addr      = base;
    cap.base      = base;
    cap.bound     = bound;
    cap.key_perms = pack_key_perms(key, perms);
    cap.identity  = identity;
    return cap;
}

void *shm_cap_side_deref(const shm_region_t *region,
                         shm_cap_side_t      cap,
                         shm_perm_t          required_perms)
{
    if (!region || shm_cap_side_is_null(cap))
        return NULL;

    uint32_t key = shm_cap_side_key(cap);
    int idx = find_by_key(key);
    if (idx < 0) {
        errno = EACCES;   /* unknown / rotated-away / forged key */
        return NULL;
    }

    /* Re-validate against the *live* block header -- catches resize (bound
     * moved in place). Does NOT catch free+reuse landing a new, unrelated
     * allocation at the same key via a birthday collision -- see the ABA
     * note on shm_cap_side_t; that cross-check was removed along with the
     * `capability` field. */
    shm_block_hdr_t *b = shm_blk_of_poff(region, (shm_off_t)g_rows[idx].base);
    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK || !b->allocated) {
        errno = EACCES;
        return NULL;
    }
    uint64_t live_bound = g_rows[idx].base + (uint64_t)b->payload_cap;
    if (live_bound != g_rows[idx].bound) {
        errno = EACCES;   /* resized since this capability was derived */
        return NULL;
    }

    uint32_t need = 0;
    if (required_perms & SHM_PERM_READ)  need |= SHM_CAP_SIDE_PERM_READ;
    if (required_perms & SHM_PERM_WRITE) need |= SHM_CAP_SIDE_PERM_WRITE;
    if ((g_rows[idx].perms & need) != need) {
        errno = EACCES;
        return NULL;
    }

    return (char *)b + sizeof(shm_block_hdr_t);
}

shm_cap_side_t shm_cap_side_rotate(shm_cap_side_t cap, uint32_t identity)
{
    if (shm_cap_side_is_null(cap))
        return SHM_CAP_SIDE_NULL;

    uint32_t old_key = shm_cap_side_key(cap);
    int idx = find_by_key(old_key);
    if (idx < 0)
        return SHM_CAP_SIDE_NULL;   /* stale / unknown / already-rotated key */

    if (g_rows[idx].identity != identity)
        return SHM_CAP_SIDE_NULL;   /* key matched, but caller is not the owner */

    uint32_t new_key = gen_unique_key(old_key);
    if (new_key == 0)
        return SHM_CAP_SIDE_NULL;   /* key space exhausted */

    g_rows[idx].key = new_key;   /* old_key stops matching everywhere, instantly */

    shm_cap_side_t out;
    out.addr      = g_rows[idx].base;
    out.base      = g_rows[idx].base;
    out.bound     = g_rows[idx].bound;
    out.key_perms = pack_key_perms(new_key, g_rows[idx].perms);
    out.identity  = g_rows[idx].identity;
    return out;
}

int shm_cap_side_free(shm_cap_side_t cap, uint32_t identity)
{
    if (shm_cap_side_is_null(cap))
        return ENOENT;

    int idx = find_by_key(shm_cap_side_key(cap));
    if (idx < 0)
        return ENOENT;   /* already freed, or never valid */

    if (g_rows[idx].identity != identity)
        return EACCES;   /* key matched, but caller is not the owner */

    memset(&g_rows[idx], 0, sizeof(g_rows[idx]));
    return 0;
}
