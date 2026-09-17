/**
 * @file  shm_hwcap.c
 * @brief Implementation of the optional hardware-capability bridge.
 *
 * Uses shm_internal.h to read the live block header directly (same reason
 * shm_cap.c does: avoids a second lock acquisition through the public
 * shm_block_info() API on every derive/deref).
 *
 * Token tracking
 * ──────────────
 * shm_free() knows an object's id/offset/owner/perms but nothing about
 * capabilities layered on top of shm_alloc — by design, shm_alloc.c does not
 * depend on shm_cap.c or this file. To still revoke hardware tokens the
 * instant an object is freed, this file keeps a small process-local
 * (obj_id -> token) table and registers itself as shm_alloc's free hook
 * (shm_set_free_hook()) the first time a backend is installed.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_hwcap.h"
#include "shm_internal.h"  /* shm_block_hdr_t, shm_blk_of_poff */

#include <errno.h>
#include <string.h>

/* ── Process-local backend state ─────────────────────────────────────────── */

static shm_hwcap_ops_t g_ops;
static bool            g_bound = false;

/* ── Process-local token registry (for free-hook auto-revocation) ───────── */

#define HWCAP_TRACK_MAX 256u

typedef struct {
    bool     used;
    uint64_t obj_id;
    uint32_t token;
} hwcap_track_entry_t;

static hwcap_track_entry_t g_track[HWCAP_TRACK_MAX];

static void track_add(uint64_t obj_id, uint32_t token)
{
    for (uint32_t i = 0; i < HWCAP_TRACK_MAX; i++) {
        if (!g_track[i].used) {
            g_track[i].used   = true;
            g_track[i].obj_id = obj_id;
            g_track[i].token  = token;
            return;
        }
    }
    /* Tracking table full: the token still works, but shm_free() will not
     * be able to auto-revoke it. Best-effort tracking only. */
}

static void track_remove_token(uint32_t token)
{
    for (uint32_t i = 0; i < HWCAP_TRACK_MAX; i++) {
        if (g_track[i].used && g_track[i].token == token) {
            g_track[i].used = false;
            return;
        }
    }
}

static void track_clear_all(void)
{
    memset(g_track, 0, sizeof(g_track));
}

/** Free hook: revoke every hardware token tracked against obj_id. */
static void hwcap_on_free(const shm_region_t *region, uint64_t obj_id,
                          shm_off_t off, shm_user_id_t owner, shm_perm_t perms)
{
    (void)region; (void)off; (void)owner; (void)perms;

    if (!g_bound || !g_ops.revoke_region)
        return;

    for (uint32_t i = 0; i < HWCAP_TRACK_MAX; i++) {
        if (g_track[i].used && g_track[i].obj_id == obj_id) {
            g_ops.revoke_region(g_ops.ctx, g_track[i].token);
            g_track[i].used = false;
        }
    }
}

/* ── Backend registration ────────────────────────────────────────────────── */

void shm_hwcap_set_backend(const shm_hwcap_ops_t *ops)
{
    if (!ops) {
        shm_hwcap_clear_backend();
        return;
    }
    g_ops   = *ops;
    g_bound = true;
    track_clear_all();
    shm_set_free_hook(hwcap_on_free);
}

void shm_hwcap_clear_backend(void)
{
    shm_set_free_hook(NULL);
    memset(&g_ops, 0, sizeof(g_ops));
    g_bound = false;
    track_clear_all();
}

bool shm_hwcap_backend_bound(void)
{
    return g_bound;
}

/* ── Derive / deref / revoke ──────────────────────────────────────────────── */

shm_hwcap_t shm_hwcap_derive(const shm_region_t *region,
                             shm_off_t           off,
                             shm_user_id_t       user_id,
                             shm_perm_t          caller_perms,
                             shm_perm_t          grant_perms)
{
    shm_hwcap_t out;
    out.sw       = shm_cap_derive(region, off, user_id, caller_perms, grant_perms);
    out.hw_token = SHM_HWCAP_TOKEN_NONE;

    if (shm_cap_is_null(out.sw))
        return out;   /* software layer already rejected this derive */

    if (!g_bound || !g_ops.register_region)
        return out;   /* no backend installed: software-only capability */

    /* Read the live block header directly (already validated non-NULL and
     * live by the successful shm_cap_derive() call above). */
    shm_block_hdr_t *b = shm_blk_of_poff(region, off);
    void  *ptr  = (char *)b + sizeof(shm_block_hdr_t);
    size_t size = b->payload_cap;

    uint32_t token = SHM_HWCAP_TOKEN_NONE;
    int rc = g_ops.register_region(g_ops.ctx, ptr, size,
                                    shm_perm_to_hwcap(grant_perms), &token);
    if (rc != 0)
        return out;   /* hardware table full or backend error: degrade gracefully */

    out.hw_token = token;
    track_add(b->obj_id, token);
    return out;
}

void *shm_hwcap_deref(const shm_region_t *region,
                      shm_hwcap_t         cap,
                      shm_perm_t          required_perms)
{
    void *p = shm_cap_deref(region, cap.sw, required_perms);
    if (!p)
        return NULL;   /* software checks already failed */

    if (cap.hw_token == SHM_HWCAP_TOKEN_NONE)
        return p;       /* capability was never hardware-backed */

    if (!g_bound || !g_ops.verify_region)
        return p;       /* no backend (or backend offers no verify hook) */

    int rc = g_ops.verify_region(g_ops.ctx, cap.hw_token, p,
                                  shm_perm_to_hwcap(required_perms));
    if (rc != 0) {
        errno = EACCES;   /* hardware denies even though software allowed it */
        return NULL;
    }
    return p;
}

int shm_hwcap_revoke(shm_hwcap_t *cap)
{
    if (!cap || cap->hw_token == SHM_HWCAP_TOKEN_NONE)
        return EINVAL;

    if (!g_bound || !g_ops.revoke_region)
        return ENOSYS;

    int rc = g_ops.revoke_region(g_ops.ctx, cap->hw_token);
    if (rc == 0) {
        track_remove_token(cap->hw_token);
        cap->hw_token = SHM_HWCAP_TOKEN_NONE;
    }
    return rc;
}
