/**
 * @file  shm_alloc.c
 * @brief Core shared/disaggregated memory allocator implementation.
 *
 * Internal layout types and accessor helpers live in shm_internal.h so they
 * can be shared with the CHERI-like capability layer (shm_cap.c).
 *
 * Free-list discipline
 * ────────────────────
 *  Address-ordered singly-linked list of free blocks.  Links are 1-based
 *  heap-header-offsets (hoff+1) so 0 is an unambiguous end-of-list sentinel
 *  even when the first block in the heap is at hoff=0.
 *  Allocation: first-fit scan.
 *  Free: insertion in address order + forward and backward coalescing.
 *
 * Concurrency
 * ───────────
 *  A single process-shared robust pthread_mutex_t serialises all mutations.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_internal.h"   /* shared structs, inline helpers */
#include "shm_ns.h"         /* namespace / cgroup fingerprinting */

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
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Permission checking
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Verify that caller may perform an operation requiring @p required bits.
 *
 * Steps (in order):
 *   1. Block magic must match and block must be allocated.
 *   2. SHM_PERM_ADMIN in caller_perms bypasses the owner-id check.
 *   3. Caller must be the block owner OR hold ADMIN.
 *   4. Block's stored perms must contain all required bits.
 *   5. Caller's supplied perms must also contain all required bits.
 *
 * @return 0 on success; EINVAL for a corrupt block; EACCES for denied access.
 */
static int check_block_access(const shm_block_hdr_t *b,
                               shm_user_id_t          caller,
                               shm_perm_t             caller_perms,
                               shm_perm_t             required)
{
    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK)
        return EINVAL;           /* magic mismatch: corruption or wrong offset */
    if (!b->allocated)
        return EINVAL;           /* tag bit is 0: block is on the free list */
    if (caller_perms & SHM_PERM_ADMIN)
        return 0;                /* admin bypasses ownership and perm checks */
    if (caller != b->owner)
        return EACCES;           /* caller does not own this block */
    if ((b->perms & required) != required)
        return EACCES;           /* block does not grant the required bits */
    if ((caller_perms & required) != required)
        return EACCES;           /* caller did not request the required bits */
    return 0;
}

/**
 * Same check but against a directory entry; used before we have the block ptr.
 */
static int check_dir_access(const shm_obj_entry_t *e,
                             shm_user_id_t          caller,
                             shm_perm_t             caller_perms,
                             shm_perm_t             required)
{
    if (!e || !e->alive || e->id == 0)
        return ENOENT;           /* slot is empty or has been freed */
    if (caller_perms & SHM_PERM_ADMIN)
        return 0;                /* admin skips owner check */
    if (caller != e->owner)
        return EACCES;
    if ((e->perms & required) != required)
        return EACCES;
    if ((caller_perms & required) != required)
        return EACCES;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Free-list operations  (all called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Insert block @p b into the address-ordered free list and coalesce with
 * adjacent free blocks.  Maintains ascending address order so coalescing can
 * be done in a single pass.
 *
 * Every struct field modified here is persisted before returning so that other
 * hosts see a consistent free list even after a crash mid-operation.
 */
static void freelist_insert(shm_region_t *r, shm_block_hdr_t *b)
{
    shm_region_hdr_t *rh   = shm_rh(r);
    size_t            b_ho = shm_hoff_of_blk(r, b);

    /* Clear ownership metadata now that the block is free. */
    b->allocated = 0;
    b->obj_id    = 0;
    b->owner     = 0;
    b->perms     = 0;
    b->type_tag  = 0;

    /* Walk to find the insertion point (first block with hoff > b_ho). */
    uint64_t prev_lnk = 0;
    uint64_t cur_lnk  = rh->free_list;

    while (cur_lnk) {
        shm_block_hdr_t *cur = shm_blk_of_link(r, cur_lnk);
        if (shm_hoff_of_blk(r, cur) > b_ho)
            break;
        prev_lnk = cur_lnk;
        cur_lnk  = cur->next_free;
    }

    /* Splice b between prev and cur. */
    b->next_free = cur_lnk;
    if (prev_lnk == 0)
        rh->free_list = shm_link_enc(b_ho);
    else
        shm_blk_of_link(r, prev_lnk)->next_free = shm_link_enc(b_ho);

    /* Coalesce b with its successor (cur) if they are physically adjacent. */
    if (cur_lnk) {
        shm_block_hdr_t *cur = shm_blk_of_link(r, cur_lnk);
        if (b_ho + b->total_size == shm_hoff_of_blk(r, cur)) {
            b->total_size += cur->total_size;  /* absorb cur's size into b */
            b->next_free   = cur->next_free;
            cur->magic     = 0;                /* poison the merged block */
            shm_persist(cur, sizeof(*cur));    /* persist poisoned successor */
        }
    }

    /* Coalesce the predecessor (prev) with b if they are physically adjacent. */
    if (prev_lnk) {
        shm_block_hdr_t *prev  = shm_blk_of_link(r, prev_lnk);
        size_t           pr_ho = shm_hoff_of_blk(r, prev);
        if (pr_ho + prev->total_size == b_ho) {
            prev->total_size += b->total_size;
            prev->next_free   = b->next_free;
            b->magic          = 0;             /* poison the merged block */
            shm_persist(b, sizeof(*b));        /* persist poisoned b before prev */
            shm_persist(prev, sizeof(*prev));  /* persist coalesced predecessor */
            return;                            /* b is now part of prev; done */
        }
        /* Predecessor was not merged; persist its updated next_free link. */
        shm_persist(prev, sizeof(*prev));
    } else {
        /* free_list head was updated; persist the region header field. */
        shm_persist(&rh->free_list, sizeof(rh->free_list));
    }

    /* Persist the newly inserted (and possibly forward-coalesced) block b. */
    shm_persist(b, sizeof(*b));
}

/**
 * Remove a specific block from the free list.  Linear scan O(free-list length).
 * Used during in-place growth of an object whose successor is free.
 */
static void freelist_remove(shm_region_t *r, shm_block_hdr_t *target)
{
    shm_region_hdr_t *rh         = shm_rh(r);
    uint64_t          target_lnk = shm_link_enc(shm_hoff_of_blk(r, target));
    uint64_t          prev_lnk   = 0;
    uint64_t          cur_lnk    = rh->free_list;

    while (cur_lnk && cur_lnk != target_lnk) {
        prev_lnk = cur_lnk;
        cur_lnk  = shm_blk_of_link(r, cur_lnk)->next_free;
    }
    if (cur_lnk != target_lnk)
        return;   /* target is not on the free list (should not happen) */

    /* Bridge predecessor to successor, bypassing target. */
    if (prev_lnk == 0)
        rh->free_list = target->next_free;
    else
        shm_blk_of_link(r, prev_lnk)->next_free = target->next_free;

    target->next_free = 0;   /* clear the stale link */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal heap allocator  (called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Find the first free block large enough for @p need_total bytes (including
 * the block header), remove it from the free list, optionally split the
 * remainder into a new free block, and return the now-allocated block.
 *
 * @return Pointer to the allocated block, or NULL if the heap is exhausted.
 */
static shm_block_hdr_t *heap_alloc_block(shm_region_t *r, size_t need_total)
{
    shm_region_hdr_t *rh       = shm_rh(r);
    uint64_t          prev_lnk = 0;
    uint64_t          cur_lnk  = rh->free_list;

    while (cur_lnk) {
        shm_block_hdr_t *b = shm_blk_of_link(r, cur_lnk);

        if (b->total_size >= need_total) {
            size_t rem = b->total_size - need_total;

            if (rem >= BLOCK_MIN) {
                /* Split off the remainder as a new free block. */
                shm_block_hdr_t *nb = (shm_block_hdr_t *)((char *)b + need_total);
                nb->magic        = SHM_ALLOC_MAGIC_BLOCK;
                nb->type_tag     = 0;
                nb->owner        = 0;
                nb->perms        = 0;
                nb->allocated    = 0;                   /* mark as free */
                nb->obj_id       = 0;
                nb->payload_cap  = rem - sizeof(shm_block_hdr_t);
                nb->total_size   = rem;
                nb->next_free    = b->next_free;

                b->total_size    = need_total;
                b->payload_cap   = need_total - sizeof(shm_block_hdr_t);

                /* Persist the split remainder before linking it into the list. */
                shm_persist(nb, sizeof(*nb));

                /* Replace b in the free list with the new split block. */
                uint64_t nb_lnk = shm_link_enc(shm_hoff_of_blk(r, nb));
                if (prev_lnk == 0)
                    rh->free_list = nb_lnk;
                else
                    shm_blk_of_link(r, prev_lnk)->next_free = nb_lnk;
            } else {
                /* Absorb the remainder into b (minor internal fragmentation). */
                if (prev_lnk == 0)
                    rh->free_list = b->next_free;
                else
                    shm_blk_of_link(r, prev_lnk)->next_free = b->next_free;
            }

            b->allocated  = 1;     /* set the tag bit: block is live */
            b->next_free  = 0;
            rh->used_bytes  += b->total_size;
            rh->block_count += 1;

            /* Persist the allocated block header and the updated region counters
             * and free-list head before returning to the caller. */
            shm_persist(b, sizeof(*b));
            shm_persist(&rh->free_list,
                        sizeof(rh->free_list) + sizeof(rh->used_bytes)
                        + sizeof(rh->block_count));
            return b;
        }

        prev_lnk = cur_lnk;
        cur_lnk  = b->next_free;
    }

    return NULL;   /* heap exhausted */
}

/**
 * Return block @p b to the free list and update accounting.
 * Does NOT touch the directory; callers update the directory separately.
 */
static void heap_free_block(shm_region_t *r, shm_block_hdr_t *b)
{
    shm_region_hdr_t *rh = shm_rh(r);
    rh->used_bytes  -= b->total_size;
    rh->block_count -= 1;
    freelist_insert(r, b);   /* returns to the address-ordered free list */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Directory helpers  (called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

static shm_obj_entry_t *dir_find_by_id(const shm_region_t *r, uint64_t obj_id)
{
    shm_region_hdr_t *rh = shm_rh((shm_region_t *)r);
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        shm_obj_entry_t *e = shm_dir_slot(r, i);
        if (e->id == obj_id && e->alive)
            return e;
    }
    return NULL;
}

static shm_obj_entry_t *dir_find_by_name(const shm_region_t *r, const char *name)
{
    shm_region_hdr_t *rh = shm_rh((shm_region_t *)r);
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        shm_obj_entry_t *e = shm_dir_slot(r, i);
        if (e->alive && strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static shm_obj_entry_t *dir_find_free_slot(const shm_region_t *r)
{
    shm_region_hdr_t *rh = shm_rh((shm_region_t *)r);
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        shm_obj_entry_t *e = shm_dir_slot(r, i);
        if (e->id == 0 || !e->alive)
            return e;
    }
    return NULL;
}

static uint32_t dir_count_live(const shm_region_t *r)
{
    shm_region_hdr_t *rh = shm_rh((shm_region_t *)r);
    uint32_t count = 0;
    for (uint32_t i = 0; i < rh->dir_capacity; i++)
        if (shm_dir_slot(r, i)->alive)
            count++;
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Region initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void region_init(shm_region_t *r, size_t total_size,
                        shm_backend_t backend, uint32_t dir_cap,
                        const char *name)
{
    /* Compute the layout offsets. */
    size_t hdr_sz   = shm_align_up(sizeof(shm_region_hdr_t), HEAP_ALIGN);
    size_t dir_off  = hdr_sz;
    size_t dir_sz   = shm_align_up((size_t)dir_cap * sizeof(shm_obj_entry_t), HEAP_ALIGN);
    size_t data_off = dir_off + dir_sz;

    memset(r->base, 0, total_size);   /* clean slate */

    shm_region_hdr_t *rh = shm_rh(r);
    rh->magic        = SHM_ALLOC_MAGIC_REGION;
    rh->version      = SHM_ALLOC_VERSION;
    rh->backend      = (uint32_t)backend;
    rh->dir_capacity = dir_cap;
    rh->region_size  = total_size;
    rh->dir_offset   = dir_off;
    rh->data_offset  = data_off;
    rh->used_bytes   = 0;
    rh->block_count  = 0;
    rh->next_id      = 1;           /* IDs start at 1; 0 is the free-slot sentinel */
    rh->host_count   = 0;           /* incremented by shm_region_open after init */
    rh->ns_inode     = shm_ns_ipc_inode();    /* creator's IPC namespace inode */
    rh->cgroup_hash  = shm_ns_cgroup_hash();  /* creator's cgroup fingerprint */
    if (name)
        strncpy(rh->shm_name, name, sizeof(rh->shm_name) - 1);

    /* Initialise the process-shared robust mutex. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&rh->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    /* Create the single initial free block covering the entire heap. */
    size_t heap_sz = total_size - data_off;
    shm_block_hdr_t *initial = shm_blk_at_hoff(r, 0);
    initial->magic       = SHM_ALLOC_MAGIC_BLOCK;
    initial->type_tag    = 0;
    initial->owner       = 0;
    initial->perms       = 0;
    initial->allocated   = 0;          /* free (tag bit is 0) */
    initial->obj_id      = 0;
    initial->payload_cap = heap_sz - sizeof(shm_block_hdr_t);
    initial->total_size  = heap_sz;
    initial->next_free   = 0;          /* only block; no successor */

    rh->free_list = shm_link_enc(0);   /* hoff=0 → link=1 */

    /* Persist the entire initialized region header, directory (already zeroed
     * by memset, but the persist makes the zeroes visible past the cache), and
     * the first block header so other hosts see a complete initial state.      */
    shm_persist(r->base, data_off + sizeof(shm_block_hdr_t));
    shm_drain();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mutex helpers (handle EOWNERDEAD recovery)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int region_lock(shm_region_t *r)
{
    int rc = pthread_mutex_lock(&shm_rh(r)->mutex);
    if (rc == EOWNERDEAD) {
        /* A process died holding the lock; recover and continue. */
        pthread_mutex_consistent(&shm_rh(r)->mutex);
        rc = 0;
    }
    return rc;
}

static void region_unlock(shm_region_t *r)
{
    pthread_mutex_unlock(&shm_rh(r)->mutex);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: region lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_region_open(const char                   *name_or_path,
                    size_t                        size,
                    const shm_region_open_opts_t *opts,
                    shm_region_t                **out_region)
{
    if (!name_or_path || !out_region)
        return EINVAL;

    shm_backend_t backend  = opts ? opts->backend     : SHM_BACKEND_POSIX;
    uint32_t      flags    = opts ? opts->flags       : 0u;
    uint32_t      dir_cap  = opts ? opts->dir_capacity : 0u;
    mode_t        mode     = opts ? opts->mode        : 0;
    bool          creating = (flags & SHM_OPEN_CREATE) != 0;

    if (dir_cap == 0) dir_cap = DIR_CAP_DEFAULT;
    if (mode    == 0) mode    = 0660;

    shm_region_t *reg = calloc(1, sizeof(*reg));
    if (!reg) return ENOMEM;

    reg->name = strdup(name_or_path);
    if (!reg->name) { free(reg); return ENOMEM; }
    reg->backend = backend;
    reg->creator = creating;
    reg->fd      = -1;

    /* Open the backing file descriptor. */
    if (backend == SHM_BACKEND_POSIX) {
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
        /* DAX backend: open the device file directly. */
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
        if (backend == SHM_BACKEND_POSIX && creating) shm_unlink(name_or_path);
        free(reg->name); free(reg);
        return EINVAL;
    }

    reg->base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, reg->fd, 0);
    if (reg->base == MAP_FAILED) {
        int err = errno;
        close(reg->fd);
        if (backend == SHM_BACKEND_POSIX && creating) shm_unlink(name_or_path);
        free(reg->name); free(reg);
        return err;
    }
    reg->size = size;

    shm_region_hdr_t *rh = shm_rh(reg);

    if (creating) {
        region_init(reg, size, backend, dir_cap, name_or_path);
    } else {
        if (rh->magic != SHM_ALLOC_MAGIC_REGION || rh->version != SHM_ALLOC_VERSION) {
            munmap(reg->base, reg->size);
            close(reg->fd); free(reg->name); free(reg);
            return ENOENT;
        }
        if ((flags & SHM_OPEN_ENFORCE_NS) && rh->ns_inode != 0) {
            uint64_t my_ns = shm_ns_ipc_inode();
            if (my_ns != 0 && my_ns != rh->ns_inode) {
                munmap(reg->base, reg->size);
                close(reg->fd); free(reg->name); free(reg);
                return EPERM;
            }
        }
        if ((flags & SHM_OPEN_ENFORCE_CGROUP) && rh->cgroup_hash != 0) {
            uint64_t my_cg = shm_ns_cgroup_hash();
            if (my_cg != 0 && my_cg != rh->cgroup_hash) {
                munmap(reg->base, reg->size);
                close(reg->fd); free(reg->name); free(reg);
                return EPERM;
            }
        }
    }

    region_lock(reg);
    shm_rh(reg)->host_count++;
    shm_persist(&shm_rh(reg)->host_count, sizeof(shm_rh(reg)->host_count));
    shm_drain();
    region_unlock(reg);

    *out_region = reg;
    return 0;
}

void shm_region_close(shm_region_t *region, bool unlink_name)
{
    if (!region) return;
    if (region->base) {
        region_lock(region);
        if (shm_rh(region)->host_count > 0) {
            shm_rh(region)->host_count--;
            shm_persist(&shm_rh(region)->host_count,
                        sizeof(shm_rh(region)->host_count));
            shm_drain();
        }
        region_unlock(region);
        munmap(region->base, region->size);
    }
    if (region->fd >= 0)
        close(region->fd);
    if (unlink_name && region->name && region->backend == SHM_BACKEND_POSIX)
        shm_unlink(region->name);
    free(region->name);
    free(region);
}

void *shm_region_base(const shm_region_t *region)
{
    return region ? region->base : NULL;
}

size_t shm_region_size(const shm_region_t *region)
{
    return region ? region->size : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: allocation
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_alloc(shm_region_t  *region,
              size_t         payload_size,
              const char    *name,
              shm_user_id_t  user_id,
              shm_perm_t     perms,
              uint32_t       type_tag,
              uint64_t      *out_id,
              shm_off_t     *out_off)
{
    if (!region || payload_size == 0 || !out_id || !out_off)
        return EINVAL;

    size_t need_total = shm_align_up(sizeof(shm_block_hdr_t) + payload_size, HEAP_ALIGN);

    region_lock(region);

    shm_obj_entry_t *slot = dir_find_free_slot(region);
    if (!slot) {
        region_unlock(region);
        return ENOSPC;
    }

    shm_block_hdr_t *b = heap_alloc_block(region, need_total);
    if (!b) {
        region_unlock(region);
        return ENOMEM;
    }

    shm_region_hdr_t *rh = shm_rh(region);
    uint64_t new_id = rh->next_id++;

    b->type_tag = type_tag;
    b->owner    = user_id;
    b->perms    = perms;
    b->obj_id   = new_id;

    memset((char *)b + sizeof(shm_block_hdr_t), 0, b->payload_cap);

    /* Persist the zero-initialized payload before exposing the live block.
     * Other hosts must not see uninitialized bytes through a capability or ptr. */
    shm_persist((char *)b + sizeof(shm_block_hdr_t), b->payload_cap);

    /* Persist the completed block header (includes the allocated/tag bit). */
    shm_persist(b, sizeof(*b));

    memset(slot, 0, sizeof(*slot));
    slot->id        = new_id;
    slot->off       = shm_poff_of_blk(region, b);
    slot->capacity  = b->payload_cap;
    slot->used_size = payload_size;
    slot->type_tag  = type_tag;
    slot->owner     = user_id;
    slot->perms     = perms;
    slot->alive     = 1;
    if (name && name[0] != '\0')
        strncpy(slot->name, name, DIR_NAME_MAX - 1);

    /* Persist the directory entry so other hosts can discover the object. */
    shm_persist(slot, sizeof(*slot));

    /* Persist the region header field next_id (advanced by this allocation). */
    shm_persist(&shm_rh(region)->next_id, sizeof(shm_rh(region)->next_id));

    /* Issue the store fence; all prior persists are now ordered. */
    shm_drain();

    *out_id  = new_id;
    *out_off = slot->off;

    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: free
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_free(shm_region_t *region,
             uint64_t      obj_id,
             shm_user_id_t user_id,
             shm_perm_t    caller_perms)
{
    if (!region || obj_id == 0) return EINVAL;

    region_lock(region);

    shm_obj_entry_t *slot = dir_find_by_id(region, obj_id);
    if (!slot) {
        region_unlock(region);
        return ENOENT;
    }

    int acc = check_dir_access(slot, user_id, caller_perms, SHM_PERM_FREE);
    if (acc != 0) {
        region_unlock(region);
        return acc;
    }

    shm_block_hdr_t *b = shm_blk_of_poff(region, slot->off);
    heap_free_block(region, b);        /* clears allocated (tag bit), links, persists */

    memset(slot, 0, sizeof(*slot));    /* clear directory entry */
    shm_persist(slot, sizeof(*slot));  /* persist the cleared slot */
    shm_drain();                       /* ordering fence */

    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: resize (grow / shrink)
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_resize(shm_region_t *region,
               uint64_t      obj_id,
               size_t        new_size,
               shm_user_id_t user_id,
               shm_perm_t    caller_perms)
{
    if (!region || obj_id == 0 || new_size == 0) return EINVAL;

    region_lock(region);

    shm_obj_entry_t *slot = dir_find_by_id(region, obj_id);
    if (!slot) {
        region_unlock(region);
        return ENOENT;
    }

    int acc = check_dir_access(slot, user_id, caller_perms, SHM_PERM_RESIZE);
    if (acc != 0) {
        region_unlock(region);
        return acc;
    }

    shm_block_hdr_t *b = shm_blk_of_poff(region, slot->off);
    size_t new_total = shm_align_up(sizeof(shm_block_hdr_t) + new_size, HEAP_ALIGN);

    if (new_total <= b->total_size) {
        /* ── Shrink ── */
        size_t rem = b->total_size - new_total;
        if (rem >= BLOCK_MIN) {
            shm_block_hdr_t *tail = (shm_block_hdr_t *)((char *)b + new_total);
            tail->magic       = SHM_ALLOC_MAGIC_BLOCK;
            tail->allocated   = 0;
            tail->obj_id      = 0;
            tail->owner       = 0;
            tail->perms       = 0;
            tail->type_tag    = 0;
            tail->total_size  = rem;
            tail->payload_cap = rem - sizeof(shm_block_hdr_t);
            tail->next_free   = 0;

            shm_region_hdr_t *rh = shm_rh(region);
            rh->used_bytes -= rem;
            b->total_size   = new_total;
            b->payload_cap  = new_total - sizeof(shm_block_hdr_t);

            /* Persist the resized block header before inserting the tail fragment
             * into the free list so the new bounds are visible atomically.      */
            shm_persist(b, sizeof(*b));
            freelist_insert(region, tail);   /* freelist_insert persists tail + rh */
        }
        slot->capacity  = b->payload_cap;
        slot->used_size = new_size;
        shm_persist(slot, sizeof(*slot));   /* persist updated directory entry */

    } else {
        /* ── Grow ── */
        size_t b_ho    = shm_hoff_of_blk(region, b);
        size_t next_ho = b_ho + b->total_size;
        bool   grew    = false;

        if (next_ho + sizeof(shm_block_hdr_t) < shm_heap_size(region)) {
            shm_block_hdr_t *next = shm_blk_at_hoff(region, next_ho);
            if (next->magic == SHM_ALLOC_MAGIC_BLOCK && !next->allocated) {
                size_t combined = b->total_size + next->total_size;
                if (combined >= new_total) {
                    freelist_remove(region, next);
                    shm_region_hdr_t *rh = shm_rh(region);
                    rh->used_bytes += next->total_size;
                    b->total_size   = combined;
                    b->payload_cap  = combined - sizeof(shm_block_hdr_t);
                    next->magic     = 0;
                    shm_persist(next, sizeof(*next));    /* persist poisoned successor */

                    size_t leftover = combined - new_total;
                    if (leftover >= BLOCK_MIN) {
                        shm_block_hdr_t *tail = (shm_block_hdr_t *)((char *)b + new_total);
                        tail->magic       = SHM_ALLOC_MAGIC_BLOCK;
                        tail->allocated   = 0;
                        tail->obj_id      = 0;
                        tail->owner       = 0;
                        tail->perms       = 0;
                        tail->type_tag    = 0;
                        tail->total_size  = leftover;
                        tail->payload_cap = leftover - sizeof(shm_block_hdr_t);
                        tail->next_free   = 0;
                        rh->used_bytes   -= leftover;
                        b->total_size     = new_total;
                        b->payload_cap    = new_total - sizeof(shm_block_hdr_t);
                        /* Persist b before inserting tail; callers see new bounds. */
                        shm_persist(b, sizeof(*b));
                        freelist_insert(region, tail);
                    } else {
                        shm_persist(b, sizeof(*b));      /* persist grown block header */
                    }
                    shm_persist(&rh->used_bytes, sizeof(rh->used_bytes));
                    grew = true;
                }
            }
        }

        if (!grew) {
            /* Allocate a new block and copy the payload. */
            shm_block_hdr_t *nb = heap_alloc_block(region, new_total);
            if (!nb) {
                region_unlock(region);
                return ENOMEM;
            }
            memcpy((char *)nb + sizeof(shm_block_hdr_t),
                   (char *)b  + sizeof(shm_block_hdr_t),
                   b->payload_cap);
            size_t extra = nb->payload_cap - b->payload_cap;
            if (extra > 0)
                memset((char *)nb + sizeof(shm_block_hdr_t) + b->payload_cap, 0, extra);

            nb->type_tag = b->type_tag;
            nb->owner    = b->owner;
            nb->perms    = b->perms;
            nb->obj_id   = b->obj_id;

            /* Persist the new block's copied payload + header before updating
             * the directory, so a reader cannot see the new offset before the
             * data is durable.                                                */
            shm_persist((char *)nb + sizeof(shm_block_hdr_t), nb->payload_cap);
            shm_persist(nb, sizeof(*nb));

            heap_free_block(region, b);     /* frees old block; persists its header */
            slot->off      = shm_poff_of_blk(region, nb);
            slot->capacity = nb->payload_cap;
        } else {
            slot->capacity = b->payload_cap;
        }
        slot->used_size = new_size;
        shm_persist(slot, sizeof(*slot));   /* persist updated directory entry */
    }

    shm_drain();   /* ordering fence: all block and directory writes are durable */
    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: lookup and discovery
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_lookup(const shm_region_t *region,
               uint64_t            obj_id,
               shm_user_id_t       user_id,
               shm_perm_t          caller_perms,
               shm_off_t          *out_off,
               size_t             *out_used_size)
{
    if (!region || obj_id == 0) return EINVAL;

    region_lock((shm_region_t *)region);

    shm_obj_entry_t *slot = dir_find_by_id(region, obj_id);
    if (!slot) {
        region_unlock((shm_region_t *)region);
        return ENOENT;
    }

    int acc = check_dir_access(slot, user_id, caller_perms, SHM_PERM_READ);
    if (acc != 0) {
        region_unlock((shm_region_t *)region);
        return acc;
    }

    if (out_off)       *out_off       = slot->off;
    if (out_used_size) *out_used_size = slot->used_size;

    region_unlock((shm_region_t *)region);
    return 0;
}

int shm_lookup_by_name(const shm_region_t *region,
                       const char         *name,
                       shm_user_id_t       user_id,
                       shm_perm_t          caller_perms,
                       uint64_t           *out_id,
                       shm_off_t          *out_off)
{
    if (!region || !name) return EINVAL;

    region_lock((shm_region_t *)region);

    shm_obj_entry_t *slot = dir_find_by_name(region, name);
    if (!slot) {
        region_unlock((shm_region_t *)region);
        return ENOENT;
    }

    int acc = check_dir_access(slot, user_id, caller_perms, SHM_PERM_READ);
    if (acc != 0) {
        region_unlock((shm_region_t *)region);
        return acc;
    }

    if (out_id)  *out_id  = slot->id;
    if (out_off) *out_off = slot->off;

    region_unlock((shm_region_t *)region);
    return 0;
}

int shm_dir_next(const shm_region_t *region,
                 uint32_t           *cursor,
                 shm_dir_entry_t    *out)
{
    if (!region || !cursor || !out) return EINVAL;

    shm_region_hdr_t *rh = shm_rh((shm_region_t *)region);

    while (*cursor < rh->dir_capacity) {
        shm_obj_entry_t *e = shm_dir_slot(region, *cursor);
        (*cursor)++;

        if (e->alive && e->id != 0) {
            out->id        = e->id;
            out->off       = e->off;
            out->capacity  = e->capacity;
            out->used_size = e->used_size;
            out->type_tag  = e->type_tag;
            out->owner     = e->owner;
            out->perms     = e->perms;
            strncpy(out->name, e->name, DIR_NAME_MAX - 1);
            out->name[DIR_NAME_MAX - 1] = '\0';
            return 0;
        }
    }
    return ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: direct pointer access
 * ═══════════════════════════════════════════════════════════════════════════ */

void *shm_ptr(const shm_region_t *region,
              shm_off_t           off,
              shm_user_id_t       user_id,
              shm_perm_t          caller_perms,
              shm_perm_t          required_perms)
{
    if (!region) return NULL;

    shm_block_hdr_t *b = shm_blk_of_poff(region, off);

    if (check_block_access(b, user_id, caller_perms, required_perms) != 0)
        return NULL;

    return (char *)b + sizeof(shm_block_hdr_t);
}

int shm_block_info(const shm_region_t *region,
                   shm_off_t           off,
                   size_t             *out_payload_cap,
                   shm_user_id_t      *out_owner,
                   shm_perm_t         *out_perms,
                   uint32_t           *out_type_tag,
                   uint64_t           *out_obj_id)
{
    if (!region) return EINVAL;

    shm_block_hdr_t *b = shm_blk_of_poff(region, off);

    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK)
        return EINVAL;

    if (out_payload_cap) *out_payload_cap = b->payload_cap;
    if (out_owner)       *out_owner       = b->owner;
    if (out_perms)       *out_perms       = b->perms;
    if (out_type_tag)    *out_type_tag    = b->type_tag;
    if (out_obj_id)      *out_obj_id      = b->obj_id;

    return 0;
}

int shm_block_set_perms(shm_region_t *region,
                        shm_off_t     off,
                        shm_user_id_t user_id,
                        shm_perm_t    caller_perms,
                        shm_perm_t    new_perms)
{
    if (!region) return EINVAL;

    region_lock(region);

    shm_block_hdr_t *b = shm_blk_of_poff(region, off);

    int acc = check_block_access(b, user_id, caller_perms, SHM_PERM_WRITE);
    if (acc != 0) {
        region_unlock(region);
        return acc;
    }

    b->perms = new_perms;
    shm_persist(b, sizeof(*b));                /* persist the updated block perms */

    shm_obj_entry_t *slot = dir_find_by_id(region, b->obj_id);
    if (slot) {
        slot->perms = new_perms;
        shm_persist(slot, sizeof(*slot));      /* persist the updated directory perms */
    }

    shm_drain();   /* ordering fence */
    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: pool statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_pool_stats(const shm_region_t *region, shm_pool_stats_t *out)
{
    if (!region || !out) return EINVAL;

    shm_region_hdr_t *rh = shm_rh((shm_region_t *)region);

    out->total_bytes  = region->size - rh->data_offset;
    out->used_bytes   = rh->used_bytes;
    out->free_bytes   = out->total_bytes - rh->used_bytes;
    out->block_count  = rh->block_count;
    out->next_id      = rh->next_id;
    out->host_count   = rh->host_count;
    out->dir_capacity = rh->dir_capacity;
    out->dir_used     = dir_count_live(region);

    return 0;
}
