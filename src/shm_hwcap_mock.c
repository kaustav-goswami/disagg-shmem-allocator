/**
 * @file  shm_hwcap_mock.c
 * @brief Software CEU-like token unit emulator (see shm_hwcap_mock.h).
 *
 * Content-addressed, matching the CapCXL CEU's rotating-key design: a
 * presented token is matched against every live slot's stored key (a CAM
 * match), not used as a direct array index. register_region() picks a
 * free slot and mints a fresh random nonzero key for it; revoke_region()
 * and verify_region() find a slot by searching for a matching key, not by
 * indexing; rotate_region() finds a slot the same way and overwrites its
 * key in place, so the old key stops matching every slot, instantly.
 */

#include "shm_hwcap_mock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    bool             used;
    const void      *base;
    size_t           size;
    shm_hwcap_perm_t perms;
    uint32_t         key;     /* 0 reserved: "no capability" / free-slot sentinel */
} mock_slot_t;

struct shm_hwcap_mock {
    mock_slot_t slots[SHM_HWCAP_MOCK_SLOTS];
    bool        rng_seeded;
};

shm_hwcap_mock_t *shm_hwcap_mock_create(void)
{
    shm_hwcap_mock_t *m = calloc(1, sizeof(*m));
    return m;
}

void shm_hwcap_mock_destroy(shm_hwcap_mock_t *mock)
{
    free(mock);
}

size_t shm_hwcap_mock_active_count(const shm_hwcap_mock_t *mock)
{
    if (!mock) return 0;
    size_t n = 0;
    for (size_t i = 0; i < SHM_HWCAP_MOCK_SLOTS; i++)
        if (mock->slots[i].used) n++;
    return n;
}

/* ── Content-addressed lookup helpers ────────────────────────────────────── */

static bool key_live(shm_hwcap_mock_t *m, uint32_t key)
{
    for (uint32_t i = 0; i < SHM_HWCAP_MOCK_SLOTS; i++)
        if (m->slots[i].used && m->slots[i].key == key)
            return true;
    return false;
}

/** CAM match: find the slot index currently holding @p key, or -1. */
static int find_by_key(shm_hwcap_mock_t *m, uint32_t key)
{
    if (key == 0)
        return -1;
    for (uint32_t i = 0; i < SHM_HWCAP_MOCK_SLOTS; i++)
        if (m->slots[i].used && m->slots[i].key == key)
            return (int)i;
    return -1;
}

static int find_free_slot(shm_hwcap_mock_t *m)
{
    for (uint32_t i = 0; i < SHM_HWCAP_MOCK_SLOTS; i++)
        if (!m->slots[i].used)
            return (int)i;
    return -1;
}

/** Fresh nonzero key, not currently live, not equal to @p exclude. Bounded retry. */
static uint32_t gen_unique_key(shm_hwcap_mock_t *m, uint32_t exclude)
{
    if (!m->rng_seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)m);
        m->rng_seeded = true;
    }
    for (int attempt = 0; attempt < 256; attempt++) {
        uint32_t candidate = (uint32_t)rand();
        if (candidate != 0 && candidate != exclude && !key_live(m, candidate))
            return candidate;
    }
    return 0;   /* key space exhausted within retry budget */
}

/* ── shm_hwcap_ops_t callbacks ────────────────────────────────────────────── */

static int mock_register(void *ctx, void *ptr, size_t size,
                          shm_hwcap_perm_t perms, uint32_t *out_token)
{
    shm_hwcap_mock_t *m = (shm_hwcap_mock_t *)ctx;
    if (!m || !ptr || size == 0 || !out_token)
        return EINVAL;

    int idx = find_free_slot(m);
    if (idx < 0)
        return ENOSPC;   /* token table exhausted, mirrors a full CEU shadow file */

    uint32_t key = gen_unique_key(m, 0);
    if (key == 0)
        return ENOSPC;   /* key space exhausted (should not happen at 64 slots) */

    m->slots[idx].used  = true;
    m->slots[idx].base  = ptr;
    m->slots[idx].size  = size;
    m->slots[idx].perms = perms;
    m->slots[idx].key   = key;
    *out_token = key;
    return 0;
}

static int mock_revoke(void *ctx, uint32_t token)
{
    shm_hwcap_mock_t *m = (shm_hwcap_mock_t *)ctx;
    if (!m)
        return EINVAL;

    int idx = find_by_key(m, token);
    if (idx < 0)
        return EINVAL;   /* unknown / already revoked / already rotated-away */

    memset(&m->slots[idx], 0, sizeof(m->slots[idx]));
    return 0;
}

static int mock_verify(void *ctx, uint32_t token, const void *ptr,
                        shm_hwcap_perm_t required_perms)
{
    shm_hwcap_mock_t *m = (shm_hwcap_mock_t *)ctx;
    if (!m)
        return EINVAL;

    int idx = find_by_key(m, token);
    if (idx < 0)
        return EINVAL;   /* CEU_STS_FAULT equivalent: unknown/revoked/rotated-away token */

    const mock_slot_t *s = &m->slots[idx];
    if (ptr < s->base || (const char *)ptr >= (const char *)s->base + s->size)
        return EFAULT;    /* CEU_STS_BOUNDS equivalent */

    if ((s->perms & required_perms) != required_perms)
        return EACCES;    /* CEU_STS_PERM equivalent */

    return 0;             /* CEU_STS_OK equivalent */
}

static int mock_rotate(void *ctx, uint32_t old_token, uint32_t *out_token)
{
    shm_hwcap_mock_t *m = (shm_hwcap_mock_t *)ctx;
    if (!m || !out_token)
        return EINVAL;

    int idx = find_by_key(m, old_token);
    if (idx < 0)
        return EINVAL;   /* stale / unknown / already-rotated token */

    uint32_t new_key = gen_unique_key(m, old_token);
    if (new_key == 0)
        return ENOSPC;

    m->slots[idx].key = new_key;   /* old_token stops matching everywhere, instantly */
    *out_token = new_key;
    return 0;
}

shm_hwcap_ops_t shm_hwcap_mock_ops(shm_hwcap_mock_t *mock)
{
    shm_hwcap_ops_t ops;
    ops.ctx             = mock;
    ops.register_region = mock_register;
    ops.revoke_region   = mock_revoke;
    ops.verify_region   = mock_verify;
    ops.rotate_region   = mock_rotate;
    return ops;
}
