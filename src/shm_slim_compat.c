/**
 * @file  shm_slim_compat.c
 * @brief v2-compatible API bodies for the slim engine. Declarations live at
 *        the bottom of include/shm_slim.h; see that file's comment block
 *        for exactly what is and is not faithfully preserved.
 *
 * Deliberately a SEPARATE translation unit from shm_slim.c: nothing in the
 * main project links this alongside shm_alloc.o (that would be a duplicate
 * definition of shm_alloc()/shm_ptr()/etc., since both provide those exact
 * names). Only code that wants v2-shaped calls running on the slim engine
 * -- instead of v2's real engine -- should link this in, alongside
 * shm_slim.o and shm_ns.o.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_slim.h"

#include <errno.h>

int shm_region_open(const char                    *name_or_path,
                    size_t                          size,
                    const shm_region_open_opts_t   *opts,
                    shm_region_t                  **out_region)
{
    shm_slim_region_open_opts_t slim_opts = {0};
    const shm_slim_region_open_opts_t *slim_opts_ptr = NULL;

    if (opts) {
        slim_opts.name_capacity = 0;   /* see shm_region_open_opts_t.dir_capacity */
        slim_opts.mode          = opts->mode;
        slim_opts.flags         = 0;
        if (opts->flags & SHM_OPEN_CREATE)
            slim_opts.flags |= SHM_SLIM_OPEN_CREATE;
        if (opts->flags & SHM_OPEN_ENFORCE_NS)
            slim_opts.flags |= SHM_SLIM_OPEN_ENFORCE_NS;
        if (opts->flags & SHM_OPEN_ENFORCE_CGROUP)
            slim_opts.flags |= SHM_SLIM_OPEN_ENFORCE_CGROUP;

        switch (opts->backend) {
        case SHM_BACKEND_POSIX:   slim_opts.backend = SHM_SLIM_BACKEND_POSIX;   break;
        case SHM_BACKEND_DAX:     slim_opts.backend = SHM_SLIM_BACKEND_DAX;     break;
        case SHM_BACKEND_GENERIC: slim_opts.backend = SHM_SLIM_BACKEND_GENERIC; break;
        default: return EINVAL;
        }
        slim_opts_ptr = &slim_opts;
    }

    return shm_slim_region_open(name_or_path, size, slim_opts_ptr, out_region);
}

void shm_region_close(shm_region_t *region, bool unlink_name)
{
    shm_slim_region_close(region, unlink_name);
}

void *shm_region_base(const shm_region_t *region)
{
    return shm_slim_region_base(region);
}

size_t shm_region_size(const shm_region_t *region)
{
    return shm_slim_region_size(region);
}

int shm_alloc(shm_region_t  *region,
              size_t         payload_size,
              const char    *name,
              shm_user_id_t  user_id,
              shm_perm_t     perms,
              uint32_t       type_tag,
              uint64_t      *out_id,
              shm_off_t     *out_off)
{
    (void)perms;      /* not enforced bit-by-bit; see shm_slim.h */
    (void)type_tag;   /* accepted, not persisted; see shm_slim.h */

    if (!out_id || !out_off)
        return EINVAL;

    shm_off_t off = SHM_OFF_NULL;
    int rc = shm_slim_alloc(region, payload_size, name,
                            (shm_slim_key_t)user_id, &off);
    if (rc != 0)
        return rc;

    *out_off = off;
    *out_id  = (uint64_t)off;   /* id == off; see shm_slim.h */
    return 0;
}

int shm_free(shm_region_t *region,
             uint64_t      obj_id,
             shm_user_id_t user_id,
             shm_perm_t    caller_perms)
{
    (void)caller_perms;
    return shm_slim_free(region, (shm_off_t)obj_id, (shm_slim_key_t)user_id);
}

int shm_resize(shm_region_t *region,
               uint64_t      obj_id,
               size_t        new_size,
               shm_user_id_t user_id,
               shm_perm_t    caller_perms)
{
    (void)caller_perms;

    shm_off_t old_off = (shm_off_t)obj_id;
    shm_off_t new_off = SHM_OFF_NULL;
    int rc = shm_slim_resize(region, old_off, new_size,
                             (shm_slim_key_t)user_id, &new_off);
    if (rc != 0)
        return rc;

    if (new_off != old_off) {
        /* Relocated: this compat layer has no id-to-offset directory to
         * keep that transparent, unlike v2. Report it rather than return
         * a caller-visible id that now silently points at a freed block. */
        return ENOTSUP;
    }
    return 0;
}

void *shm_ptr(const shm_region_t *region,
              shm_off_t           off,
              shm_user_id_t       user_id,
              shm_perm_t          caller_perms,
              shm_perm_t          required_perms)
{
    (void)caller_perms;
    (void)required_perms;
    return shm_slim_ptr(region, off, (shm_slim_key_t)user_id);
}

int shm_pool_stats(const shm_region_t *region, shm_pool_stats_t *out)
{
    if (!out)
        return EINVAL;

    shm_slim_pool_stats_t s;
    int rc = shm_slim_pool_stats(region, &s);
    if (rc != 0)
        return rc;

    out->total_bytes  = s.total_bytes;
    out->used_bytes   = s.used_bytes;
    out->free_bytes   = s.free_bytes;
    out->block_count  = s.block_count;
    out->next_id      = 0;
    out->host_count   = s.host_count;
    out->dir_capacity = s.name_capacity;
    out->dir_used     = s.name_used;
    return 0;
}
