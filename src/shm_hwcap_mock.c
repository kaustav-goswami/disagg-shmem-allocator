/**
 * @file  shm_hwcap_mock.c
 * @brief Software CEU-like token unit emulator (see shm_hwcap_mock.h).
 */

#include "shm_hwcap_mock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool             used;
    const void      *base;
    size_t           size;
    shm_hwcap_perm_t perms;
} mock_slot_t;

struct shm_hwcap_mock {
    mock_slot_t slots[SHM_HWCAP_MOCK_SLOTS];
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

/* ── shm_hwcap_ops_t callbacks ────────────────────────────────────────────── */

static int mock_register(void *ctx, void *ptr, size_t size,
                          shm_hwcap_perm_t perms, uint32_t *out_token)
{
    shm_hwcap_mock_t *m = (shm_hwcap_mock_t *)ctx;
    if (!m || !ptr || size == 0 || !out_token)
        return EINVAL;

    for (uint32_t i = 0; i < SHM_HWCAP_MOCK_SLOTS; i++) {
        if (!m->slots[i].used) {
            m->slots[i].used  = true;
            m->slots[i].base  = ptr;
            m->slots[i].size  = size;
            m->slots[i].perms = perms;
            *out_token = i;
            return 0;
        }
    }
    return ENOSPC;   /* token table exhausted, mirrors a full CEU shadow file */
}

static int mock_revoke(void *ctx, uint32_t token)
{
    shm_hwcap_mock_t *m = (shm_hwcap_mock_t *)ctx;
    if (!m || token >= SHM_HWCAP_MOCK_SLOTS || !m->slots[token].used)
        return EINVAL;

    memset(&m->slots[token], 0, sizeof(m->slots[token]));
    return 0;
}

static int mock_verify(void *ctx, uint32_t token, const void *ptr,
                        shm_hwcap_perm_t required_perms)
{
    shm_hwcap_mock_t *m = (shm_hwcap_mock_t *)ctx;
    if (!m || token >= SHM_HWCAP_MOCK_SLOTS || !m->slots[token].used)
        return EINVAL;   /* CEU_STS_FAULT equivalent: unknown/revoked token */

    const mock_slot_t *s = &m->slots[token];
    if (ptr < s->base || (const char *)ptr >= (const char *)s->base + s->size)
        return EFAULT;    /* CEU_STS_BOUNDS equivalent */

    if ((s->perms & required_perms) != required_perms)
        return EACCES;    /* CEU_STS_PERM equivalent */

    return 0;             /* CEU_STS_OK equivalent */
}

shm_hwcap_ops_t shm_hwcap_mock_ops(shm_hwcap_mock_t *mock)
{
    shm_hwcap_ops_t ops;
    ops.ctx             = mock;
    ops.register_region = mock_register;
    ops.revoke_region   = mock_revoke;
    ops.verify_region    = mock_verify;
    return ops;
}
