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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Software back-invalidate hook (see shm_slim_internal.h) -- weak default,
 *  a strong definition elsewhere in the final link overrides it.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(SHM_SLIM_SOFTWARE_BI)
__attribute__((weak))
void shm_slim_soft_bi(const void *hdr_addr, size_t hdr_len)
{
    (void)hdr_addr;
    (void)hdr_len;
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Backing-fd sizing (POSIX regular files, and DAX character devices)
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t shm_slim_fd_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return 0;
    if (st.st_size > 0)
        return (size_t)st.st_size;

    /* fstat() always reports st_size==0 for a /dev/dax character device --
     * its real size lives in sysfs, keyed by the device's major:minor
     * number (which fstat() DOES report correctly, via st_rdev, since
     * that's just device-node identity, not size). This is the only way
     * to recover it from just an fd: shm_slim_region_open_fd() is never
     * given the original path string, only the fd. */
    if (!S_ISCHR(st.st_mode))
        return 0;

    char sysfs_path[64];
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/dev/char/%u:%u/size",
             major(st.st_rdev), minor(st.st_rdev));

    FILE *f = fopen(sysfs_path, "r");
    if (!f)
        return 0;
    unsigned long long sysfs_size = 0;
    int matched = fscanf(f, "%llu", &sysfs_size);
    fclose(f);
    if (matched != 1)
        return 0;

    return (size_t)sysfs_size;
}

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
 *
 *  Open-addressed hash table keyed by name (FNV-1a + linear probing).
 *  `rh->name_cap` is the physical slot count, inflated ~2x beyond whatever
 *  the caller requested so the load factor stays <= 50% -- see
 *  shm_slim_name_entry_t's own comment and WIP.md for the measured effect.
 *  A slot is EMPTY (name[0]==0, terminates a probe-miss), a TOMBSTONE
 *  (name[0]==SHM_SLIM_NAME_TOMBSTONE, must be probed THROUGH, never
 *  terminates a search), or OCCUPIED (anything else).
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline bool name_slot_empty(const shm_slim_name_entry_t *e)
{
    return e->name[0] == '\0';
}

static inline bool name_slot_tombstone(const shm_slim_name_entry_t *e)
{
    return e->name[0] == SHM_SLIM_NAME_TOMBSTONE;
}

static inline bool name_slot_occupied(const shm_slim_name_entry_t *e)
{
    return !name_slot_empty(e) && !name_slot_tombstone(e);
}

/** FNV-1a. Same algorithm shm_ns.c already uses for cgroup_hash, applied to name strings. */
static uint32_t name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

/** Pure lookup: NULL if not found. Probes through tombstones, stops at the first empty slot. */
static shm_slim_name_entry_t *name_find(const shm_slim_region_t *r, const char *name)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    uint32_t cap = rh->name_cap;
    if (cap == 0) return NULL;
    uint32_t start = name_hash(name) % cap;
    for (uint32_t probe = 0; probe < cap; probe++) {
        shm_slim_name_entry_t *e = shm_slim_name_slot(r, (start + probe) % cap);
        if (name_slot_empty(e))
            return NULL;
        if (name_slot_occupied(e) && strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;   /* table exhausted without hitting an empty slot -- shouldn't happen at our load factor */
}

/**
 * Combined duplicate-check + free-slot search for insertion, in ONE probe
 * pass instead of two separate full scans -- this is the fix for the O(N)
 * per-publish / O(N^2) total cost measured on objstream (see WIP.md).
 * @return NULL + *out_err = EEXIST if `name` is already in use; NULL +
 *         *out_err = ENOSPC if the table is full; otherwise the slot to
 *         write the new entry into.
 */
static shm_slim_name_entry_t *name_insert_slot(const shm_slim_region_t *r, const char *name,
                                                int *out_err)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    uint32_t cap = rh->name_cap;
    if (cap == 0) { *out_err = ENOSPC; return NULL; }
    uint32_t start = name_hash(name) % cap;
    shm_slim_name_entry_t *first_free = NULL;
    for (uint32_t probe = 0; probe < cap; probe++) {
        shm_slim_name_entry_t *e = shm_slim_name_slot(r, (start + probe) % cap);
        if (name_slot_empty(e)) {
            if (!first_free) first_free = e;
            break;   /* genuine empty slot: no duplicate can exist further down this chain */
        }
        if (name_slot_tombstone(e)) {
            if (!first_free) first_free = e;
            continue;
        }
        if (strcmp(e->name, name) == 0) { *out_err = EEXIST; return NULL; }
    }
    if (!first_free) { *out_err = ENOSPC; return NULL; }
    *out_err = 0;
    return first_free;
}

static shm_slim_name_entry_t *name_find_by_off(const shm_slim_region_t *r, shm_off_t off)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    for (uint32_t i = 0; i < rh->name_cap; i++) {
        shm_slim_name_entry_t *e = shm_slim_name_slot(r, i);
        if (name_slot_occupied(e) && e->off == off)
            return e;
    }
    return NULL;
}

static uint32_t name_count_used(const shm_slim_region_t *r)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    uint32_t n = 0;
    for (uint32_t i = 0; i < rh->name_cap; i++)
        if (name_slot_occupied(shm_slim_name_slot(r, i)))
            n++;
    return n;
}

/** Mark an already-resolved entry as deleted. Tombstone, never a full
 * clear -- zeroing would break probe chains for other entries that
 * collided through this slot (see the type's own comment). */
static void name_tombstone_entry(shm_slim_name_entry_t *e)
{
    e->name[0] = SHM_SLIM_NAME_TOMBSTONE;
    shm_persist(e, sizeof(*e));
}

static void name_remove_by_off(shm_slim_region_t *r, shm_off_t off)
{
    shm_slim_name_entry_t *e = name_find_by_off(r, off);
    if (e) name_tombstone_entry(e);
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
 *  Namespace table helpers (called under the region mutex)
 *
 *  A slot is empty iff ns_inode == 0 && cgroup_hash == 0 -- key == 0
 *  (SHM_SLIM_KEY_OPEN) is itself a legitimate bindable key, so it cannot
 *  double as the "unused slot" sentinel the way name_entry uses name[0].
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool namespace_slot_empty(const shm_slim_namespace_entry_t *e)
{
    return e->ns_inode == 0 && e->cgroup_hash == 0;
}

static shm_slim_namespace_entry_t *namespace_find(const shm_slim_region_t *r,
                                                   shm_slim_key_t key)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    for (uint32_t i = 0; i < rh->namespace_cap; i++) {
        shm_slim_namespace_entry_t *e = shm_slim_namespace_slot(r, i);
        if (!namespace_slot_empty(e) && e->key == key)
            return e;
    }
    return NULL;
}

static shm_slim_namespace_entry_t *namespace_find_free_slot(const shm_slim_region_t *r)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    for (uint32_t i = 0; i < rh->namespace_cap; i++) {
        shm_slim_namespace_entry_t *e = shm_slim_namespace_slot(r, i);
        if (namespace_slot_empty(e))
            return e;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Free-list operations (all called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void freelist_insert(shm_slim_region_t *r, shm_slim_hdr_t *b)
{
    shm_slim_region_hdr_t *rh   = shm_slim_rh(r);
    size_t                  b_ho = shm_slim_hoff_of_blk(r, b);

    b->magic     = SHM_SLIM_MAGIC_FREE;
    b->key       = 0;
    b->timestamp = (uint64_t)time(NULL);
    SHM_SLIM_BI_NOTIFY(b, sizeof(*b));

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

/**
 * Open addressing needs headroom to keep probe sequences short. Every
 * caller in this codebase sizes name_capacity to exactly the number of
 * objects it intends to publish (e.g. DMALLOC_NAME_CAPACITY == object
 * count in objstream) -- a table sized exactly to that request would run
 * at 100% load factor, degrading right back to the linear-scan cost
 * hashing exists to avoid. Doubling the requested capacity keeps the load
 * factor <= 50% for any caller. This is a real, disclosed memory cost
 * (the physical name table is ~2x the requested size) traded for O(1)
 * amortized publish/lookup instead of O(name_capacity) per call -- see
 * WIP.md for the measured effect both ways.
 */
static uint32_t name_table_physical_capacity(uint32_t requested)
{
    if (requested > UINT32_MAX / 2) return UINT32_MAX;   /* saturate rather than overflow */
    return requested * 2;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Region initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void region_init(shm_slim_region_t *r, size_t total_size,
                         shm_slim_backend_t backend, uint32_t name_cap,
                         uint32_t namespace_cap, const char *name)
{
    size_t hdr_sz        = shm_slim_align_up(sizeof(shm_slim_region_hdr_t), SHM_SLIM_ALIGN);
    size_t name_off      = hdr_sz;
    size_t name_sz       = shm_slim_align_up((size_t)name_cap * sizeof(shm_slim_name_entry_t),
                                              SHM_SLIM_ALIGN);
    size_t namespace_off = name_off + name_sz;
    size_t namespace_sz  = shm_slim_align_up((size_t)namespace_cap * sizeof(shm_slim_namespace_entry_t),
                                              SHM_SLIM_ALIGN);
    size_t data_off      = namespace_off + namespace_sz;

    memset(r->base, 0, total_size);

    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    rh->magic            = SHM_SLIM_MAGIC_REGION;
    rh->version          = SHM_SLIM_VERSION;
    rh->backend          = (uint32_t)backend;
    rh->name_cap         = name_cap;
    rh->namespace_cap    = namespace_cap;
    rh->region_size      = total_size;
    rh->name_offset      = name_off;
    rh->namespace_offset = namespace_off;
    rh->data_offset      = data_off;
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

    shm_slim_backend_t backend       = opts ? opts->backend            : SHM_SLIM_BACKEND_POSIX;
    uint32_t            flags         = opts ? opts->flags              : 0u;
    uint32_t            name_cap      = opts ? opts->name_capacity      : 0u;
    uint32_t            namespace_cap = opts ? opts->namespace_capacity : 0u;
    mode_t               mode          = opts ? opts->mode               : 0;
    bool                 creating      = (flags & SHM_SLIM_OPEN_CREATE) != 0;

    if (name_cap      == 0) name_cap      = SHM_SLIM_NAME_CAP_DEFAULT;
    if (namespace_cap == 0) namespace_cap = SHM_SLIM_NAMESPACE_CAP_DEFAULT;
    if (mode          == 0) mode          = 0660;
    name_cap = name_table_physical_capacity(name_cap);

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
        region_init(reg, size, backend, name_cap, namespace_cap, name_or_path);

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
            /* fstat().st_size is always 0 for a /dev/dax character device
             * (its size lives in sysfs, not the inode) -- shm_slim_fd_size()
             * handles that; a 2MB fallback here would have silently mapped
             * only a sliver of a real multi-GB DAX device with no warning. */
            size_t detected = shm_slim_fd_size(reg->fd);
            size = (detected > 0) ? detected : (2u << 20);
        }
    }

    if (size < 4096) {
        close(reg->fd);
        if (backend == SHM_SLIM_BACKEND_POSIX && creating) shm_unlink(name_or_path);
        free(reg->name); free(reg);
        return EINVAL;
    }

    reg->base = opts && opts->fixed_base
        ? mmap(opts->fixed_base, size, PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_FIXED_NOREPLACE, reg->fd, 0)
        : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, reg->fd, 0);
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
        region_init(reg, size, backend, name_cap, namespace_cap, name_or_path);
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

int shm_slim_region_open_fd(int fd, uint32_t flags,
                            const shm_slim_region_open_opts_t *opts,
                            shm_slim_region_t                **out_region)
{
    if (fd < 0 || !out_region)
        return EINVAL;

    uint32_t name_cap      = opts ? opts->name_capacity      : 0u;
    uint32_t namespace_cap = opts ? opts->namespace_capacity : 0u;
    if (name_cap      == 0) name_cap      = SHM_SLIM_NAME_CAP_DEFAULT;
    if (namespace_cap == 0) namespace_cap = SHM_SLIM_NAMESPACE_CAP_DEFAULT;
    name_cap = name_table_physical_capacity(name_cap);

    bool creating = (flags & SHM_SLIM_OPEN_CREATE) != 0;

    /* Unlike shm_slim_region_open(), this never calls ftruncate(): the
     * caller already opened (and, for a fresh pool, already sized) fd --
     * "using a file descriptor" is the whole point of this entry point.
     * The size is read back from the fd itself via shm_slim_fd_size()
     * (handles the DAX-character-device case, where plain fstat().st_size
     * is always 0) -- unless opts->size_hint overrides it, for device
     * nodes where even that detection isn't reliable. */
    size_t size = (opts && opts->size_hint) ? opts->size_hint : shm_slim_fd_size(fd);
    if (size < 4096)
        return EINVAL;

    void *base = opts && opts->fixed_base
        ? mmap(opts->fixed_base, size, PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0)
        : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
        return errno;

    shm_slim_region_t *reg = calloc(1, sizeof(*reg));
    if (!reg) {
        munmap(base, size);
        return ENOMEM;
    }
    reg->name    = NULL;   /* no name/path is known at this entry point */
    reg->fd      = -1;     /* caller opened fd and keeps owning its lifecycle */
    reg->base    = base;
    reg->size    = size;
    reg->creator = creating;
    reg->backend = SHM_SLIM_BACKEND_POSIX;   /* nominal; unused for fd-based regions */

    shm_slim_region_hdr_t *rh = shm_slim_rh(reg);

    if (creating) {
        region_init(reg, size, reg->backend, name_cap, namespace_cap, NULL);
    } else {
        if (rh->magic != SHM_SLIM_MAGIC_REGION || rh->version != SHM_SLIM_VERSION) {
            munmap(base, size); free(reg);
            return ENOENT;
        }
        if ((flags & SHM_SLIM_OPEN_ENFORCE_NS) && rh->ns_inode != 0) {
            uint64_t my_ns = shm_ns_ipc_inode();
            if (my_ns != 0 && my_ns != rh->ns_inode) {
                munmap(base, size); free(reg);
                return EPERM;
            }
        }
        if ((flags & SHM_SLIM_OPEN_ENFORCE_CGROUP) && rh->cgroup_hash != 0) {
            uint64_t my_cg = shm_ns_cgroup_hash();
            if (my_cg != 0 && my_cg != rh->cgroup_hash) {
                munmap(base, size); free(reg);
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
    if (want_name && name[0] == SHM_SLIM_NAME_TOMBSTONE)
        return EINVAL;   /* would be indistinguishable from a deleted slot -- see SHM_SLIM_NAME_TOMBSTONE */

    size_t need_total = shm_slim_align_up(sizeof(shm_slim_hdr_t) + payload_size, SHM_SLIM_ALIGN);
    if (need_total < SHM_SLIM_BLOCK_MIN)
        need_total = SHM_SLIM_BLOCK_MIN;

    region_lock(region);

    shm_slim_name_entry_t *nslot = NULL;
    if (want_name) {
        int err = 0;
        nslot = name_insert_slot(region, name, &err);
        if (!nslot) {
            region_unlock(region);
            return err;
        }
    }

    shm_slim_hdr_t *b = heap_alloc_block(region, need_total);
    if (!b) {
        region_unlock(region);
        return ENOMEM;
    }

    b->key       = key;
    b->magic     = want_name ? SHM_SLIM_MAGIC_LIVE_NAMED : SHM_SLIM_MAGIC_LIVE;
    b->timestamp = (uint64_t)time(NULL);

    size_t payload_cap = b->size - sizeof(shm_slim_hdr_t);
    memset((char *)b + sizeof(*b), 0, payload_cap);

    /* Persist the zero-initialized payload before exposing the live block. */
    shm_persist((char *)b + sizeof(*b), payload_cap);
    shm_persist(b, sizeof(*b));
    SHM_SLIM_BI_NOTIFY(b, sizeof(*b));

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

int shm_slim_free_by_name(shm_slim_region_t *region, const char *name,
                          shm_slim_key_t key, shm_off_t *out_off)
{
    if (!region || !name) return EINVAL;

    region_lock(region);

    shm_slim_name_entry_t *e = name_find(region, name);
    if (!e) {
        region_unlock(region);
        return ENOENT;
    }

    shm_slim_hdr_t *b = shm_slim_blk_of_poff_checked(region, e->off);
    if (!blk_is_live(b)) {
        region_unlock(region);
        return EFAULT;   /* name table entry is stale/corrupted */
    }
    if (!key_matches(b, key)) {
        region_unlock(region);
        return EACCES;
    }

    shm_off_t off = e->off;
    name_tombstone_entry(e);
    heap_free_block(region, b);
    shm_drain();

    if (out_off) *out_off = off;
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

int shm_slim_ptr_to_off(const shm_slim_region_t *region, const void *ptr,
                        shm_off_t *out_off)
{
    if (!region || !ptr || !out_off) return EINVAL;

    const char *heap_base = shm_slim_heap_base(region);
    size_t      heap_size = shm_slim_heap_size(region);
    const char *p         = (const char *)ptr;

    if (p < heap_base || (size_t)(p - heap_base) >= heap_size)
        return EINVAL;

    *out_off = (shm_off_t)(p - heap_base);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: key management
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_slim_set_key(shm_slim_region_t *region, shm_off_t off,
                     shm_slim_key_t old_key, shm_slim_key_t new_key)
{
    if (!region) return EINVAL;

    region_lock(region);

    shm_slim_hdr_t *b = shm_slim_blk_of_poff_checked(region, off);
    if (!blk_is_live(b)) {
        region_unlock(region);
        return EINVAL;
    }
    if (!key_matches(b, old_key)) {
        region_unlock(region);
        return EACCES;
    }

    b->key       = new_key;
    b->timestamp = (uint64_t)time(NULL);
    shm_persist(b, sizeof(*b));
    shm_drain();
    SHM_SLIM_BI_NOTIFY(b, sizeof(*b));

    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: namespace binding
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_slim_namespace_bind(shm_slim_region_t *region, shm_slim_key_t key)
{
    if (!region) return EINVAL;

    region_lock(region);

    if (namespace_find(region, key)) {
        region_unlock(region);
        return EEXIST;
    }
    shm_slim_namespace_entry_t *slot = namespace_find_free_slot(region);
    if (!slot) {
        region_unlock(region);
        return ENOSPC;
    }

    uint64_t ns_inode    = shm_ns_ipc_inode();
    uint64_t cgroup_hash = shm_ns_cgroup_hash();
    if (ns_inode == 0 && cgroup_hash == 0) {
        /* Nothing to bind to on this host -- refuse rather than silently
         * create an entry that namespace_slot_empty() would treat as free. */
        region_unlock(region);
        return ENOTSUP;
    }

    slot->key         = key;
    slot->ns_inode    = ns_inode;
    slot->cgroup_hash = cgroup_hash;
    shm_persist(slot, sizeof(*slot));
    shm_drain();

    region_unlock(region);
    return 0;
}

void shm_slim_namespace_unbind(shm_slim_region_t *region, shm_slim_key_t key)
{
    if (!region) return;

    region_lock(region);
    shm_slim_namespace_entry_t *e = namespace_find(region, key);
    if (e) {
        memset(e, 0, sizeof(*e));
        shm_persist(e, sizeof(*e));
        shm_drain();
    }
    region_unlock(region);
}

int shm_slim_namespace_rebind(shm_slim_region_t *region,
                              shm_slim_key_t old_key, shm_slim_key_t new_key)
{
    if (!region) return EINVAL;

    region_lock(region);

    shm_slim_namespace_entry_t *e = namespace_find(region, old_key);
    if (!e) {
        region_unlock(region);
        return 0;   /* nothing bound; not an error */
    }
    if (old_key != new_key && namespace_find(region, new_key)) {
        region_unlock(region);
        return EEXIST;
    }

    e->key = new_key;
    shm_persist(e, sizeof(*e));
    shm_drain();

    region_unlock(region);
    return 0;
}

int shm_slim_namespace_check_ex(const shm_slim_region_t *region, shm_slim_key_t key,
                                shm_slim_read_mode_t mode)
{
    if (!region) return EINVAL;
    if (mode != SHM_SLIM_READ_LOCKED && mode != SHM_SLIM_READ_LOCK_FREE)
        return EINVAL;

    shm_slim_region_t *mr = (shm_slim_region_t *)region;
    if (mode == SHM_SLIM_READ_LOCKED) region_lock(mr);

    const shm_slim_namespace_entry_t *e = namespace_find(region, key);
    int result = 0;
    if (e) {
        if (e->ns_inode != 0 && e->ns_inode != shm_ns_ipc_inode())
            result = EPERM;
        else if (e->cgroup_hash != 0 && e->cgroup_hash != shm_ns_cgroup_hash())
            result = EPERM;
    }

    if (mode == SHM_SLIM_READ_LOCKED) region_unlock(mr);
    return result;
}

int shm_slim_namespace_check(const shm_slim_region_t *region, shm_slim_key_t key)
{
    return shm_slim_namespace_check_ex(region, key, SHM_SLIM_READ_LOCKED);
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

int shm_slim_name_read_slot(const shm_slim_region_t *region, uint32_t slot_index,
                            shm_slim_read_mode_t mode, shm_slim_name_slot_t *out)
{
    if (!region || !out) return EINVAL;
    if (mode != SHM_SLIM_READ_LOCKED && mode != SHM_SLIM_READ_LOCK_FREE)
        return EINVAL;

    shm_slim_region_hdr_t *rh = shm_slim_rh((shm_slim_region_t *)region);
    if (slot_index >= rh->name_cap) return ERANGE;

    shm_slim_region_t *mr = (shm_slim_region_t *)region;
    if (mode == SHM_SLIM_READ_LOCKED) region_lock(mr);

    shm_slim_name_entry_t *e = shm_slim_name_slot(region, slot_index);
    bool occupied = name_slot_occupied(e);   /* excludes both empty and tombstone slots */
    if (occupied) {
        strncpy(out->name, e->name, SHM_SLIM_NAME_MAX - 1);
        out->name[SHM_SLIM_NAME_MAX - 1] = '\0';
        out->off = e->off;
    }

    if (mode == SHM_SLIM_READ_LOCKED) region_unlock(mr);
    return occupied ? 0 : ENOENT;
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
