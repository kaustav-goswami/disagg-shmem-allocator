/**
 * @file  shm_slim.c
 * @brief v0.2 "slim" allocator implementation. See shm_slim.h and WIP.md for
 *        the full design rationale.
 *
 * Free-list discipline
 * ────────────────────
 *  Address-ordered singly-linked list of free blocks, same as v2. Links are
 *  1-based heap-header-offsets. Allocation: first-fit. Free: insertion in
 *  address order + forward/backward coalescing. The one difference from v2:
 *  a free block's "next free" link lives in-band in its own payload area
 *  (shm_slim_set_free_link / shm_slim_get_free_link), not as a header field,
 *  since the header has no room for it.
 *
 * Concurrency
 * ───────────
 *  A single process-shared robust pthread_mutex_t serialises all mutations,
 *  same as v2.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_slim_internal.h"
#include "shm_ns.h"
#include "shm_generic_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Access checking
 * ═══════════════════════════════════════════════════════════════════════════ */

/** True if @p b is a live block (either anonymous or named). */
static bool blk_is_live(const shm_slim_hdr_t *b)
{
    return b && (b->magic == SHM_SLIM_MAGIC_LIVE || b->magic == SHM_SLIM_MAGIC_LIVE_NAMED);
}

/**
 * The entire v0.2 access model: a live block with key == SHM_SLIM_KEY_OPEN
 * grants access to anyone; otherwise the caller's key must match exactly.
 * This replaces v2's five-step owner+perms+admin check with one comparison.
 */
static bool key_matches(const shm_slim_hdr_t *b, shm_slim_key_t key)
{
    return b->key == SHM_SLIM_KEY_OPEN || b->key == key;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Name table helpers (called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

static shm_slim_name_entry_t *name_find(const shm_slim_region_t *r, const char *name)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    for (uint32_t i = 0; i < rh->name_cap; i++) {
        shm_slim_name_entry_t *e = shm_slim_name_slot(r, i);
        if (e->name[0] != '\0' && strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static shm_slim_name_entry_t *name_find_by_off(const shm_slim_region_t *r, shm_off_t off)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    for (uint32_t i = 0; i < rh->name_cap; i++) {
        shm_slim_name_entry_t *e = shm_slim_name_slot(r, i);
        if (e->name[0] != '\0' && e->off == off)
            return e;
    }
    return NULL;
}

static shm_slim_name_entry_t *name_find_free_slot(const shm_slim_region_t *r)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    for (uint32_t i = 0; i < rh->name_cap; i++) {
        shm_slim_name_entry_t *e = shm_slim_name_slot(r, i);
        if (e->name[0] == '\0')
            return e;
    }
    return NULL;
}

static uint32_t name_count_used(const shm_slim_region_t *r)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    uint32_t n = 0;
    for (uint32_t i = 0; i < rh->name_cap; i++)
        if (shm_slim_name_slot(r, i)->name[0] != '\0')
            n++;
    return n;
}

static void name_remove_by_off(shm_slim_region_t *r, shm_off_t off)
{
    shm_slim_name_entry_t *e = name_find_by_off(r, off);
    if (e) {
        memset(e, 0, sizeof(*e));
        shm_persist(e, sizeof(*e));
    }
}

static void name_update_off(shm_slim_region_t *r, shm_off_t old_off, shm_off_t new_off)
{
    shm_slim_name_entry_t *e = name_find_by_off(r, old_off);
    if (e) {
        e->off = new_off;
        shm_persist(e, sizeof(*e));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Free-list operations (all called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void freelist_insert(shm_slim_region_t *r, shm_slim_hdr_t *b)
{
    shm_slim_region_hdr_t *rh   = shm_slim_rh(r);
    size_t                  b_ho = shm_slim_hoff_of_blk(r, b);

    b->magic = SHM_SLIM_MAGIC_FREE;
    b->key   = 0;

    uint64_t prev_lnk = 0;
    uint64_t cur_lnk  = rh->free_list;

    while (cur_lnk) {
        shm_slim_hdr_t *cur = shm_slim_blk_of_link(r, cur_lnk);
        if (shm_slim_hoff_of_blk(r, cur) > b_ho)
            break;
        prev_lnk = cur_lnk;
        cur_lnk  = shm_slim_get_free_link(cur);
    }

    shm_slim_set_free_link(b, cur_lnk);
    if (prev_lnk == 0)
        rh->free_list = shm_slim_link_enc(b_ho);
    else
        shm_slim_set_free_link(shm_slim_blk_of_link(r, prev_lnk), shm_slim_link_enc(b_ho));

    /* Coalesce with the successor if physically adjacent. */
    if (cur_lnk) {
        shm_slim_hdr_t *cur = shm_slim_blk_of_link(r, cur_lnk);
        if (b_ho + b->size == shm_slim_hoff_of_blk(r, cur)) {
            b->size = b->size + cur->size;
            shm_slim_set_free_link(b, shm_slim_get_free_link(cur));
            cur->magic = 0;                         /* poison the merged block */
            shm_persist(cur, sizeof(*cur));
        }
    }

    /* Coalesce the predecessor with b if physically adjacent. */
    if (prev_lnk) {
        shm_slim_hdr_t *prev  = shm_slim_blk_of_link(r, prev_lnk);
        size_t           pr_ho = shm_slim_hoff_of_blk(r, prev);
        if (pr_ho + prev->size == b_ho) {
            prev->size = prev->size + b->size;
            shm_slim_set_free_link(prev, shm_slim_get_free_link(b));
            b->magic = 0;                            /* poison the merged block */
            shm_persist(b, sizeof(*b));
            shm_persist(prev, sizeof(*prev));
            return;
        }
        shm_persist(prev, sizeof(*prev));
    } else {
        shm_persist(&rh->free_list, sizeof(rh->free_list));
    }

    shm_persist(b, sizeof(*b));
}

static void freelist_remove(shm_slim_region_t *r, shm_slim_hdr_t *target)
{
    shm_slim_region_hdr_t *rh         = shm_slim_rh(r);
    uint64_t                 target_lnk = shm_slim_link_enc(shm_slim_hoff_of_blk(r, target));
    uint64_t                 prev_lnk   = 0;
    uint64_t                 cur_lnk    = rh->free_list;

    while (cur_lnk && cur_lnk != target_lnk) {
        prev_lnk = cur_lnk;
        cur_lnk  = shm_slim_get_free_link(shm_slim_blk_of_link(r, cur_lnk));
    }
    if (cur_lnk != target_lnk)
        return;   /* not on the free list (should not happen) */

    uint64_t target_next = shm_slim_get_free_link(target);
    if (prev_lnk == 0)
        rh->free_list = target_next;
    else
        shm_slim_set_free_link(shm_slim_blk_of_link(r, prev_lnk), target_next);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal heap allocator (called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

static shm_slim_hdr_t *heap_alloc_block(shm_slim_region_t *r, size_t need_total)
{
    shm_slim_region_hdr_t *rh       = shm_slim_rh(r);
    uint64_t                 prev_lnk = 0;
    uint64_t                 cur_lnk  = rh->free_list;

    while (cur_lnk) {
        shm_slim_hdr_t *b = shm_slim_blk_of_link(r, cur_lnk);

        if (b->size >= need_total) {
            size_t rem = b->size - need_total;
            uint64_t b_next = shm_slim_get_free_link(b);

            if (rem >= SHM_SLIM_BLOCK_MIN) {
                shm_slim_hdr_t *nb = (shm_slim_hdr_t *)((char *)b + need_total);
                nb->magic = SHM_SLIM_MAGIC_FREE;
                nb->key   = 0;
                nb->size  = rem;
                shm_slim_set_free_link(nb, b_next);
                shm_persist(nb, sizeof(*nb));

                b->size = need_total;

                uint64_t nb_lnk = shm_slim_link_enc(shm_slim_hoff_of_blk(r, nb));
                if (prev_lnk == 0)
                    rh->free_list = nb_lnk;
                else
                    shm_slim_set_free_link(shm_slim_blk_of_link(r, prev_lnk), nb_lnk);
            } else {
                /* Absorb the remainder into b (minor internal fragmentation). */
                if (prev_lnk == 0)
                    rh->free_list = b_next;
                else
                    shm_slim_set_free_link(shm_slim_blk_of_link(r, prev_lnk), b_next);
            }

            b->magic = SHM_SLIM_MAGIC_LIVE;   /* caller sets _NAMED / key afterward */
            b->key   = 0;
            rh->used_bytes  += b->size;
            rh->block_count += 1;

            shm_persist(b, sizeof(*b));
            shm_persist(&rh->free_list,
                        sizeof(rh->free_list) + sizeof(rh->used_bytes)
                        + sizeof(rh->block_count));
            return b;
        }

        prev_lnk = cur_lnk;
        cur_lnk  = shm_slim_get_free_link(b);
    }

    return NULL;   /* heap exhausted */
}

static void heap_free_block(shm_slim_region_t *r, shm_slim_hdr_t *b)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    rh->used_bytes  -= b->size;
    rh->block_count -= 1;
    freelist_insert(r, b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Region initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void region_init(shm_slim_region_t *r, size_t total_size,
                         shm_slim_backend_t backend, uint32_t name_cap,
                         const char *name)
{
    size_t hdr_sz   = shm_slim_align_up(sizeof(shm_slim_region_hdr_t), SHM_SLIM_ALIGN);
    size_t name_off = hdr_sz;
    size_t name_sz  = shm_slim_align_up((size_t)name_cap * sizeof(shm_slim_name_entry_t),
                                         SHM_SLIM_ALIGN);
    size_t data_off = name_off + name_sz;

    memset(r->base, 0, total_size);

    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    rh->magic       = SHM_SLIM_MAGIC_REGION;
    rh->version     = SHM_SLIM_VERSION;
    rh->backend     = (uint32_t)backend;
    rh->name_cap    = name_cap;
    rh->region_size = total_size;
    rh->name_offset = name_off;
    rh->data_offset = data_off;
    rh->used_bytes  = 0;
    rh->block_count = 0;
    rh->host_count  = 0;
    rh->ns_inode    = shm_ns_ipc_inode();
    rh->cgroup_hash = shm_ns_cgroup_hash();
    if (name)
        strncpy(rh->shm_name, name, sizeof(rh->shm_name) - 1);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&rh->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    size_t heap_sz = total_size - data_off;
    shm_slim_hdr_t *initial = shm_slim_blk_at_hoff(r, 0);
    initial->magic = SHM_SLIM_MAGIC_FREE;
    initial->key   = 0;
    initial->size  = heap_sz;
    shm_slim_set_free_link(initial, 0);

    rh->free_list = shm_slim_link_enc(0);

    shm_persist(r->base, data_off + sizeof(shm_slim_hdr_t));
    shm_drain();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mutex helpers (handle EOWNERDEAD recovery)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int region_lock(shm_slim_region_t *r)
{
    int rc = pthread_mutex_lock(&shm_slim_rh(r)->mutex);
    if (rc == EOWNERDEAD) {
        pthread_mutex_consistent(&shm_slim_rh(r)->mutex);
        rc = 0;
    }
    return rc;
}

static void region_unlock(shm_slim_region_t *r)
{
    pthread_mutex_unlock(&shm_slim_rh(r)->mutex);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: region lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_slim_region_open(const char                        *name_or_path,
                          size_t                             size,
                          const shm_slim_region_open_opts_t *opts,
                          shm_slim_region_t                **out_region)
{
    if (!name_or_path || !out_region)
        return EINVAL;

    shm_slim_backend_t backend  = opts ? opts->backend        : SHM_SLIM_BACKEND_POSIX;
    uint32_t            flags    = opts ? opts->flags          : 0u;
    uint32_t            name_cap = opts ? opts->name_capacity  : 0u;
    mode_t               mode     = opts ? opts->mode           : 0;
    bool                 creating = (flags & SHM_SLIM_OPEN_CREATE) != 0;

    if (name_cap == 0) name_cap = SHM_SLIM_NAME_CAP_DEFAULT;
    if (mode      == 0) mode     = 0660;

    shm_slim_region_t *reg = calloc(1, sizeof(*reg));
    if (!reg) return ENOMEM;

    reg->name = strdup(name_or_path);
    if (!reg->name) { free(reg); return ENOMEM; }
    reg->backend = backend;
    reg->creator = creating;
    reg->fd      = -1;

    if (backend == SHM_SLIM_BACKEND_GENERIC) {
        /* A generic-allocator backend has no OS-level named object for a
         * second process to attach to -- see shm_generic_backend.h. */
        if (!creating) {
            free(reg->name); free(reg);
            return ENOTSUP;
        }
        if (size < 4096) {
            free(reg->name); free(reg);
            return EINVAL;
        }
        reg->base = shm_generic_backend_alloc(size);
        if (!reg->base) {
            free(reg->name); free(reg);
            return ENOMEM;
        }
        reg->size = size;
        region_init(reg, size, backend, name_cap, name_or_path);

        region_lock(reg);
        shm_slim_rh(reg)->host_count++;
        shm_persist(&shm_slim_rh(reg)->host_count, sizeof(shm_slim_rh(reg)->host_count));
        shm_drain();
        region_unlock(reg);

        *out_region = reg;
        return 0;
    }

    if (backend == SHM_SLIM_BACKEND_POSIX) {
        int oflags = creating ? (O_CREAT | O_RDWR) : O_RDWR;
        reg->fd = shm_open(name_or_path, oflags, mode);
        if (reg->fd < 0) {
            int err = errno;
            free(reg->name); free(reg);
            return err;
        }
        if (creating && size >= 4096) {
            if (ftruncate(reg->fd, (off_t)size) != 0) {
                int err = errno;
                close(reg->fd); shm_unlink(name_or_path);
                free(reg->name); free(reg);
                return err;
            }
        }
        if (!creating) {
            struct stat st;
            if (fstat(reg->fd, &st) != 0) {
                int err = errno;
                close(reg->fd); free(reg->name); free(reg);
                return err;
            }
            size = (size_t)st.st_size;
        }
    } else {
        reg->fd = open(name_or_path, O_RDWR);
        if (reg->fd < 0) {
            int err = errno;
            free(reg->name); free(reg);
            return err;
        }
        if (size < 4096) {
            struct stat st;
            if (fstat(reg->fd, &st) == 0 && st.st_size > 0)
                size = (size_t)st.st_size;
            else
                size = 2u << 20;
        }
    }

    if (size < 4096) {
        close(reg->fd);
        if (backend == SHM_SLIM_BACKEND_POSIX && creating) shm_unlink(name_or_path);
        free(reg->name); free(reg);
        return EINVAL;
    }

    reg->base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, reg->fd, 0);
    if (reg->base == MAP_FAILED) {
        int err = errno;
        close(reg->fd);
        if (backend == SHM_SLIM_BACKEND_POSIX && creating) shm_unlink(name_or_path);
        free(reg->name); free(reg);
        return err;
    }
    reg->size = size;

    shm_slim_region_hdr_t *rh = shm_slim_rh(reg);

    if (creating) {
        region_init(reg, size, backend, name_cap, name_or_path);
    } else {
        /* The version check is what makes this format incompatible with v2:
         * v2 regions store an integer version (2); this format stores the
         * literal ASCII word "slim". The two can never accidentally match. */
        if (rh->magic != SHM_SLIM_MAGIC_REGION || rh->version != SHM_SLIM_VERSION) {
            munmap(reg->base, reg->size);
            close(reg->fd); free(reg->name); free(reg);
            return ENOENT;
        }
        if ((flags & SHM_SLIM_OPEN_ENFORCE_NS) && rh->ns_inode != 0) {
            uint64_t my_ns = shm_ns_ipc_inode();
            if (my_ns != 0 && my_ns != rh->ns_inode) {
                munmap(reg->base, reg->size);
                close(reg->fd); free(reg->name); free(reg);
                return EPERM;
            }
        }
        if ((flags & SHM_SLIM_OPEN_ENFORCE_CGROUP) && rh->cgroup_hash != 0) {
            uint64_t my_cg = shm_ns_cgroup_hash();
            if (my_cg != 0 && my_cg != rh->cgroup_hash) {
                munmap(reg->base, reg->size);
                close(reg->fd); free(reg->name); free(reg);
                return EPERM;
            }
        }
    }

    region_lock(reg);
    shm_slim_rh(reg)->host_count++;
    shm_persist(&shm_slim_rh(reg)->host_count, sizeof(shm_slim_rh(reg)->host_count));
    shm_drain();
    region_unlock(reg);

    *out_region = reg;
    return 0;
}

void shm_slim_region_close(shm_slim_region_t *region, bool unlink_name)
{
    if (!region) return;
    if (region->base) {
        region_lock(region);
        if (shm_slim_rh(region)->host_count > 0) {
            shm_slim_rh(region)->host_count--;
            shm_persist(&shm_slim_rh(region)->host_count,
                        sizeof(shm_slim_rh(region)->host_count));
            shm_drain();
        }
        region_unlock(region);
        if (region->backend == SHM_SLIM_BACKEND_GENERIC)
            shm_generic_backend_free(region->base, region->size);
        else
            munmap(region->base, region->size);
    }
    if (region->fd >= 0)
        close(region->fd);
    if (unlink_name && region->name && region->backend == SHM_SLIM_BACKEND_POSIX)
        shm_unlink(region->name);
    free(region->name);
    free(region);
}

void *shm_slim_region_base(const shm_slim_region_t *region)
{
    return region ? region->base : NULL;
}

size_t shm_slim_region_size(const shm_slim_region_t *region)
{
    return region ? region->size : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: allocation
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_slim_alloc(shm_slim_region_t *region,
                    size_t             payload_size,
                    const char        *name,
                    shm_slim_key_t     key,
                    shm_off_t         *out_off)
{
    if (!region || payload_size == 0 || !out_off)
        return EINVAL;

    bool want_name = name && name[0] != '\0';
    if (want_name && strnlen(name, SHM_SLIM_NAME_MAX) >= SHM_SLIM_NAME_MAX)
        return ENAMETOOLONG;

    size_t need_total = shm_slim_align_up(sizeof(shm_slim_hdr_t) + payload_size, SHM_SLIM_ALIGN);
    if (need_total < SHM_SLIM_BLOCK_MIN)
        need_total = SHM_SLIM_BLOCK_MIN;

    region_lock(region);

    shm_slim_name_entry_t *nslot = NULL;
    if (want_name) {
        if (name_find(region, name)) {
            region_unlock(region);
            return EEXIST;
        }
        nslot = name_find_free_slot(region);
        if (!nslot) {
            region_unlock(region);
            return ENOSPC;
        }
    }

    shm_slim_hdr_t *b = heap_alloc_block(region, need_total);
    if (!b) {
        region_unlock(region);
        return ENOMEM;
    }

    b->key   = key;
    b->magic = want_name ? SHM_SLIM_MAGIC_LIVE_NAMED : SHM_SLIM_MAGIC_LIVE;

    size_t payload_cap = b->size - sizeof(shm_slim_hdr_t);
    memset((char *)b + sizeof(*b), 0, payload_cap);

    /* Persist the zero-initialized payload before exposing the live block. */
    shm_persist((char *)b + sizeof(*b), payload_cap);
    shm_persist(b, sizeof(*b));

    shm_off_t off = shm_slim_poff_of_blk(region, b);

    if (nslot) {
        memset(nslot, 0, sizeof(*nslot));
        strncpy(nslot->name, name, SHM_SLIM_NAME_MAX - 1);
        nslot->off = off;
        shm_persist(nslot, sizeof(*nslot));
    }

    shm_drain();

    *out_off = off;
    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: free
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_slim_free(shm_slim_region_t *region, shm_off_t off, shm_slim_key_t key)
{
    if (!region) return EINVAL;

    region_lock(region);

    shm_slim_hdr_t *b = shm_slim_blk_of_poff_checked(region, off);
    if (!b) {
        region_unlock(region);
        return EFAULT;
    }
    if (!blk_is_live(b)) {
        region_unlock(region);
        return EINVAL;
    }
    if (!key_matches(b, key)) {
        region_unlock(region);
        return EACCES;
    }

    if (b->magic == SHM_SLIM_MAGIC_LIVE_NAMED)
        name_remove_by_off(region, off);

    heap_free_block(region, b);
    shm_drain();

    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: resize (grow / shrink)
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_slim_resize(shm_slim_region_t *region,
                     shm_off_t          off,
                     size_t             new_size,
                     shm_slim_key_t     key,
                     shm_off_t         *out_off)
{
    if (!region || new_size == 0 || !out_off) return EINVAL;

    region_lock(region);

    shm_slim_hdr_t *b = shm_slim_blk_of_poff_checked(region, off);
    if (!b) {
        region_unlock(region);
        return EFAULT;
    }
    if (!blk_is_live(b)) {
        region_unlock(region);
        return EINVAL;
    }
    if (!key_matches(b, key)) {
        region_unlock(region);
        return EACCES;
    }

    bool   named   = (b->magic == SHM_SLIM_MAGIC_LIVE_NAMED);
    size_t new_total = shm_slim_align_up(sizeof(shm_slim_hdr_t) + new_size, SHM_SLIM_ALIGN);
    if (new_total < SHM_SLIM_BLOCK_MIN)
        new_total = SHM_SLIM_BLOCK_MIN;

    shm_off_t final_off = off;

    if (new_total <= b->size) {
        /* ── Shrink ── */
        size_t rem = b->size - new_total;
        if (rem >= SHM_SLIM_BLOCK_MIN) {
            shm_slim_hdr_t *tail = (shm_slim_hdr_t *)((char *)b + new_total);
            tail->magic = SHM_SLIM_MAGIC_FREE;
            tail->key   = 0;
            tail->size  = rem;
            shm_slim_set_free_link(tail, 0);

            shm_slim_region_hdr_t *rh = shm_slim_rh(region);
            rh->used_bytes -= rem;
            b->size = new_total;

            shm_persist(b, sizeof(*b));
            freelist_insert(region, tail);
        }
    } else {
        /* ── Grow ── */
        size_t b_ho    = shm_slim_hoff_of_blk(region, b);
        size_t next_ho = b_ho + b->size;
        bool   grew    = false;

        if (next_ho + sizeof(shm_slim_hdr_t) < shm_slim_heap_size(region)) {
            shm_slim_hdr_t *next = shm_slim_blk_at_hoff(region, next_ho);
            if (next->magic == SHM_SLIM_MAGIC_FREE) {
                size_t combined = b->size + next->size;
                if (combined >= new_total) {
                    freelist_remove(region, next);
                    shm_slim_region_hdr_t *rh = shm_slim_rh(region);
                    rh->used_bytes += next->size;
                    b->size = combined;
                    next->magic = 0;
                    shm_persist(next, sizeof(*next));

                    size_t leftover = combined - new_total;
                    if (leftover >= SHM_SLIM_BLOCK_MIN) {
                        shm_slim_hdr_t *tail = (shm_slim_hdr_t *)((char *)b + new_total);
                        tail->magic = SHM_SLIM_MAGIC_FREE;
                        tail->key   = 0;
                        tail->size  = leftover;
                        shm_slim_set_free_link(tail, 0);
                        rh->used_bytes -= leftover;
                        b->size = new_total;
                        shm_persist(b, sizeof(*b));
                        freelist_insert(region, tail);
                    } else {
                        shm_persist(b, sizeof(*b));
                    }
                    shm_persist(&rh->used_bytes, sizeof(rh->used_bytes));
                    grew = true;
                }
            }
        }

        if (!grew) {
            /* Relocate: allocate a new block, copy, free the old one. */
            shm_slim_hdr_t *nb = heap_alloc_block(region, new_total);
            if (!nb) {
                region_unlock(region);
                return ENOMEM;
            }
            size_t old_cap = b->size - sizeof(shm_slim_hdr_t);
            memcpy((char *)nb + sizeof(*nb), (char *)b + sizeof(*b), old_cap);
            size_t new_cap = nb->size - sizeof(shm_slim_hdr_t);
            if (new_cap > old_cap)
                memset((char *)nb + sizeof(*nb) + old_cap, 0, new_cap - old_cap);

            nb->key   = b->key;
            nb->magic = named ? SHM_SLIM_MAGIC_LIVE_NAMED : SHM_SLIM_MAGIC_LIVE;

            shm_persist((char *)nb + sizeof(*nb), new_cap);
            shm_persist(nb, sizeof(*nb));

            heap_free_block(region, b);
            final_off = shm_slim_poff_of_blk(region, nb);

            if (named)
                name_update_off(region, off, final_off);
        }
    }

    shm_drain();
    *out_off = final_off;
    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: direct pointer access
 * ═══════════════════════════════════════════════════════════════════════════ */

void *shm_slim_ptr(const shm_slim_region_t *region, shm_off_t off, shm_slim_key_t key)
{
    if (!region) return NULL;

    shm_slim_hdr_t *b = shm_slim_blk_of_poff_checked(region, off);
    if (!blk_is_live(b))
        return NULL;
    if (!key_matches(b, key))
        return NULL;

    return (char *)b + sizeof(shm_slim_hdr_t);
}

int shm_slim_block_size(const shm_slim_region_t *region,
                         shm_off_t                off,
                         shm_slim_key_t           key,
                         size_t                  *out_size)
{
    if (!region) return EINVAL;

    shm_slim_hdr_t *b = shm_slim_blk_of_poff_checked(region, off);
    if (!blk_is_live(b))
        return EINVAL;
    if (!key_matches(b, key))
        return EACCES;

    if (out_size) *out_size = b->size - sizeof(shm_slim_hdr_t);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: discovery
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_slim_lookup_by_name(const shm_slim_region_t *region,
                             const char              *name,
                             shm_slim_key_t           key,
                             shm_off_t               *out_off)
{
    if (!region || !name || !out_off) return EINVAL;

    shm_slim_region_t *mr = (shm_slim_region_t *)region;
    region_lock(mr);

    shm_slim_name_entry_t *e = name_find(region, name);
    if (!e) {
        region_unlock(mr);
        return ENOENT;
    }

    shm_slim_hdr_t *b = shm_slim_blk_of_poff_checked(region, e->off);
    if (!blk_is_live(b)) {
        region_unlock(mr);
        return EFAULT;   /* name table entry is stale/corrupted */
    }
    if (!key_matches(b, key)) {
        region_unlock(mr);
        return EACCES;
    }

    *out_off = e->off;
    region_unlock(mr);
    return 0;
}

int shm_slim_walk(const shm_slim_region_t *region, uint64_t *cursor, shm_slim_entry_t *out)
{
    if (!region || !cursor || !out) return EINVAL;

    shm_slim_region_t *r = (shm_slim_region_t *)region;
    size_t heap_sz = shm_slim_heap_size(r);

    while (*cursor < heap_sz) {
        /* A header itself must be fully in-bounds before any of its fields
         * are read -- otherwise a corrupted chain could walk the cursor to
         * within a few bytes of the heap's end and read a "size" field from
         * memory outside the mapping. */
        if (*cursor + sizeof(shm_slim_hdr_t) > heap_sz)
            return EIO;

        shm_slim_hdr_t *b = shm_slim_blk_at_hoff(r, (size_t)*cursor);

        bool live = blk_is_live(b);
        bool free_blk = (b->magic == SHM_SLIM_MAGIC_FREE);
        if (!live && !free_blk)
            return EIO;   /* corrupted implicit block chain: cannot safely continue */

        size_t this_size = b->size;
        if (this_size < sizeof(shm_slim_hdr_t) || *cursor + this_size > heap_sz)
            return EIO;   /* corrupted size field: would run past the heap */

        shm_off_t off = (shm_off_t)(*cursor + sizeof(shm_slim_hdr_t));
        *cursor += this_size;

        if (live) {
            out->off  = off;
            out->size = this_size - sizeof(shm_slim_hdr_t);
            out->name[0] = '\0';
            if (b->magic == SHM_SLIM_MAGIC_LIVE_NAMED) {
                shm_slim_name_entry_t *n = name_find_by_off(r, off);
                if (n) {
                    strncpy(out->name, n->name, SHM_SLIM_NAME_MAX - 1);
                    out->name[SHM_SLIM_NAME_MAX - 1] = '\0';
                }
            }
            return 0;
        }
    }
    return ENOENT;
}

int shm_slim_pool_stats(const shm_slim_region_t *region, shm_slim_pool_stats_t *out)
{
    if (!region || !out) return EINVAL;

    shm_slim_region_hdr_t *rh = shm_slim_rh((shm_slim_region_t *)region);

    out->total_bytes   = region->size - rh->data_offset;
    out->used_bytes    = rh->used_bytes;
    out->free_bytes    = out->total_bytes - rh->used_bytes;
    out->block_count   = rh->block_count;
    out->host_count    = rh->host_count;
    out->name_capacity = rh->name_cap;
    out->name_used     = name_count_used(region);

    return 0;
}
