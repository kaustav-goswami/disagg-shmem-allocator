/**
 * @file  shm_alloc.c
 * @brief Core implementation of the shared/disaggregated memory allocator.
 *
 * Internal data structures
 * ────────────────────────
 *
 *  shm_region_hdr_t  – lives at base[0]; self-describing layout (stores the
 *                       offsets of the directory and the heap).
 *  shm_obj_entry_t   – one slot in the object directory; indexed 0‥dir_capacity-1.
 *  shm_block_hdr_t   – precedes every heap payload; tracks ownership and links.
 *
 * Free-list discipline
 * ────────────────────
 *  Address-ordered singly-linked list of free blocks.  Links are 1-based
 *  heap-header-offsets (hoff+1), so 0 is an unambiguous end-of-list sentinel
 *  even when the very first block in the heap is at hoff=0.
 *  Allocation: first-fit scan.
 *  Free: insertion in address order followed by forward and backward coalescing.
 *
 * Concurrency
 * ───────────
 *  A single process-shared robust pthread_mutex_t serialises all mutations.
 *  Read-only queries (shm_ptr, shm_block_info) lock briefly to prevent
 *  torn reads; shm_dir_next does not lock (best-effort snapshot).
 */

#define _GNU_SOURCE      /* MAP_FIXED_NOREPLACE, shm_open, strdup */

#include "shm_alloc.h"   /* public API */
#include "shm_ns.h"      /* namespace + cgroup fingerprinting */

#include <errno.h>       /* errno, EINVAL, ENOMEM, EACCES, ENOENT, ENOSPC */
#include <fcntl.h>       /* O_RDWR, O_CREAT for shm_open / open */
#include <pthread.h>     /* pthread_mutex_t, pthread_mutexattr_* */
#include <stdbool.h>     /* bool, true, false */
#include <stddef.h>      /* size_t, NULL */
#include <stdint.h>      /* uint8_t, uint32_t, uint64_t */
#include <stdio.h>       /* snprintf, fprintf */
#include <stdlib.h>      /* malloc, free, calloc */
#include <string.h>      /* memset, memcpy, strncpy, strcmp */
#include <sys/mman.h>    /* mmap, munmap, MAP_SHARED, PROT_READ, PROT_WRITE */
#include <sys/stat.h>    /* fstat, struct stat */
#include <unistd.h>      /* ftruncate, close */

/* POSIX shared-memory API lives in <sys/mman.h> on Linux but needs -lrt. */
#include <sys/mman.h>    /* shm_open, shm_unlink (linked via -lrt) */

/* Older glibc/headers may lack the flag; the numeric value is stable on Linux. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/*
 * Preferred mmap VA for new regions.  Using a stable high address makes
 * cross-process MAP_FIXED attach reliable under ASLR (critical for memcached
 * raw pointers).  Falls back to mmap(NULL) if the hint is unavailable.
 */
#ifndef SHM_PREFERRED_MAP_BASE
#define SHM_PREFERRED_MAP_BASE ((uintptr_t)0x4000000000ULL)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal constants
 * ═══════════════════════════════════════════════════════════════════════════ */

/** All heap blocks and the region header are aligned to this boundary. */
#define HEAP_ALIGN  8u

/** A new free block is only split off when the remainder is at least this big.
 *  Prevents creating blocks too small to be useful.  Value = header + 8 bytes. */
#define BLOCK_MIN   (sizeof(shm_block_hdr_t) + HEAP_ALIGN)

/** Default number of directory slots when caller does not specify. */
#define DIR_CAP_DEFAULT  256u

/** Maximum name length stored in the directory (including NUL terminator). */
#define DIR_NAME_MAX  48u

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal layout types (not visible outside this translation unit)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Block header.  Lives immediately before each payload in the heap.
 * All fields are plain integers so the struct is safe across processes.
 */
typedef struct shm_block_hdr {
    uint32_t      magic;        /* SHM_ALLOC_MAGIC_BLOCK — detects corruption */
    uint32_t      type_tag;     /* copy of the directory entry's type_tag */
    shm_user_id_t owner;        /* copy of the owning user id */
    shm_perm_t    perms;        /* copy of the permission bits */
    uint8_t       allocated;    /* 1 = block is live; 0 = block is on free list */
    uint8_t       _pad[7];      /* keep obj_id on an 8-byte boundary */
    uint64_t      obj_id;       /* directory entry ID; 0 for free blocks */
    size_t        payload_cap;  /* bytes available for payload (not counting header) */
    size_t        total_size;   /* sizeof(header) + payload_cap; unit of accounting */
    uint64_t      next_free;    /* 1-based hoff of next free block; 0 = end of list */
} shm_block_hdr_t;

/**
 * Region header.  Lives at base[0]; the rest of the layout is derived from
 * the dir_offset and data_offset fields stored here.
 */
typedef struct shm_region_hdr {
    uint32_t        magic;          /* SHM_ALLOC_MAGIC_REGION */
    uint32_t        version;        /* SHM_ALLOC_VERSION */
    uint32_t        backend;        /* shm_backend_t cast to uint32 */
    uint32_t        dir_capacity;   /* number of directory slots */
    pthread_mutex_t mutex;          /* process-shared robust mutex */
    uint32_t        host_count;     /* how many processes are attached */
    uint32_t        _pad0;
    size_t          region_size;    /* total mapping size in bytes */
    size_t          dir_offset;     /* byte offset of object directory from base */
    size_t          data_offset;    /* byte offset of heap start from base */
    uint64_t        free_list;      /* 1-based hoff link to first free block */
    size_t          used_bytes;     /* total heap bytes in live blocks */
    size_t          block_count;    /* number of live blocks */
    uint64_t        next_id;        /* next monotonic object ID to assign */
    uint64_t        ns_inode;       /* IPC namespace inode of region creator */
    uint64_t        cgroup_hash;    /* FNV-1a hash of creator's /proc/self/cgroup */
    uint64_t        map_base_addr;  /* creator mmap VA; attachers must match */
    uint32_t        guard_pages;    /* PROT_NONE pages at end of mapped window */
    uint32_t        _pad1;
    char            shm_name[64];   /* the name/path supplied to shm_region_open */
} shm_region_hdr_t;

/**
 * One slot in the object directory.  Scanned linearly for lookup.
 * A slot is free when id == 0 (monotonic IDs start at 1).
 */
typedef struct shm_obj_entry {
    uint64_t        id;              /* monotonic ID; 0 means slot is available */
    char            name[DIR_NAME_MAX];  /* optional human-readable name */
    shm_off_t       off;             /* heap-relative payload offset */
    size_t          capacity;        /* physical payload capacity */
    size_t          used_size;       /* logical used size (set by owner) */
    uint32_t        type_tag;        /* caller type identifier */
    shm_user_id_t   owner;           /* owning user id */
    shm_perm_t      perms;           /* permission bits */
    uint8_t         alive;           /* 1 = live object; 0 = freed (may be reused) */
    uint8_t         _pad[3];
} shm_obj_entry_t;

/**
 * Per-process handle.  Allocated on the C heap; not visible to other hosts.
 */
struct shm_region {
    char          *name;      /* heap copy of the name or device path */
    int            fd;        /* file descriptor from shm_open / open */
    void          *base;      /* mmap base pointer */
    size_t         size;      /* usable+guard mapping length (DAX window) */
    size_t         guard_bytes; /* PROT_NONE bytes at end of mapping + after */
    size_t         post_guard_bytes; /* anonymous PROT_NONE after the VMA */
    size_t         dev_size;  /* DAX device capacity (0 for POSIX) */
    bool           creator;   /* true if this process initialised the region */
    shm_backend_t  backend;   /* which backend was used */
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal utility: arithmetic and accessor helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Round n up to the nearest multiple of a (a must be a power of two). */
static size_t align_up(size_t n, size_t a)
{
    return (n + a - 1u) & ~(a - 1u);  /* standard power-of-two alignment trick */
}

/** Access the region header at the start of the mapping. */
static shm_region_hdr_t *region_hdr(const shm_region_t *r)
{
    return (shm_region_hdr_t *)r->base;  /* header is always at base[0] */
}

/** Pointer to the first byte of the heap (after header + directory). */
static char *heap_base(const shm_region_t *r)
{
    return (char *)r->base + region_hdr(r)->data_offset;
}

/** Total usable heap bytes (excludes in-region PROT_NONE guard pages). */
static size_t heap_size(const shm_region_t *r)
{
    size_t total = r->size - region_hdr(r)->data_offset;
    size_t guard = (size_t)region_hdr(r)->guard_pages * (size_t)sysconf(_SC_PAGESIZE);
    if (guard > total)
        return 0;
    return total - guard;
}

/** Convert a heap-header-offset (hoff) to a block pointer. */
static shm_block_hdr_t *blk_at_hoff(const shm_region_t *r, size_t hoff)
{
    return (shm_block_hdr_t *)(heap_base(r) + hoff);
}

/** Convert a heap-payload-offset (poff) to the block header that precedes it. */
static shm_block_hdr_t *blk_of_poff(const shm_region_t *r, shm_off_t poff)
{
    /* The block header immediately precedes the payload area. */
    return (shm_block_hdr_t *)(heap_base(r) + poff - sizeof(shm_block_hdr_t));
}

/** Convert a block pointer to its heap-header-offset. */
static size_t hoff_of_blk(const shm_region_t *r, const shm_block_hdr_t *b)
{
    return (size_t)((const char *)b - heap_base(r));
}

/** Convert a block pointer to its heap-payload-offset (what callers store). */
static shm_off_t poff_of_blk(const shm_region_t *r, const shm_block_hdr_t *b)
{
    return (shm_off_t)(hoff_of_blk(r, b) + sizeof(shm_block_hdr_t));
}

/* ── Free-list link encoding ─────────────────────────────────────────────── */

/**
 * Encode a heap-header-offset as a free-list link.
 * We add 1 so that hoff=0 (the first block) maps to link=1, leaving 0 as the
 * unambiguous end-of-list sentinel.
 */
static uint64_t link_enc(size_t hoff) { return (uint64_t)hoff + 1u; }

/** Decode a free-list link back to its heap-header-offset. */
static size_t   link_dec(uint64_t lnk) { return (size_t)(lnk - 1u); }

/**
 * Dereference a free-list link to the block pointer it points at.
 * Returns NULL when lnk == 0 (end-of-list sentinel).
 */
static shm_block_hdr_t *blk_of_link(const shm_region_t *r, uint64_t lnk)
{
    return lnk ? blk_at_hoff(r, link_dec(lnk)) : NULL;
}

/* ── Directory helpers ───────────────────────────────────────────────────── */

/** Base pointer of the object directory (directly after the region header). */
static shm_obj_entry_t *dir_base(const shm_region_t *r)
{
    return (shm_obj_entry_t *)((char *)r->base + region_hdr(r)->dir_offset);
}

/** nth slot in the directory (0-based). */
static shm_obj_entry_t *dir_slot(const shm_region_t *r, uint32_t idx)
{
    return &dir_base(r)[idx];  /* plain array indexing */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Permission checking
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Verify that @p caller may perform an operation requiring @p required bits.
 *
 * Rules:
 *   1. The block magic must be valid and the block must be allocated.
 *   2. SHM_PERM_ADMIN in caller_perms bypasses the owner-id check.
 *   3. caller must be the block owner OR caller_perms must contain ADMIN.
 *   4. The block's stored perms must contain all required bits.
 *   5. The caller's own perms must also contain all required bits.
 *
 * @return 0 on success; EINVAL for corrupt block; EACCES for denied access.
 */
static int check_block_access(const shm_block_hdr_t *b,
                               shm_user_id_t          caller,
                               shm_perm_t             caller_perms,
                               shm_perm_t             required)
{
    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK)
        return EINVAL;           /* magic mismatch: corruption or wrong offset */
    if (!b->allocated)
        return EINVAL;           /* dereferencing a free block is a bug */
    if (caller_perms & SHM_PERM_ADMIN)
        return 0;                /* admin bypasses all further checks */
    if (caller != b->owner)
        return EACCES;           /* caller does not own this block */
    if ((b->perms & required) != required)
        return EACCES;           /* block does not grant required permissions */
    if ((caller_perms & required) != required)
        return EACCES;           /* caller did not request the required permissions */
    return 0;
}

/**
 * Same check but against a directory entry (used before we have the block ptr).
 */
static int check_dir_access(const shm_obj_entry_t *e,
                             shm_user_id_t          caller,
                             shm_perm_t             caller_perms,
                             shm_perm_t             required)
{
    if (!e || !e->alive || e->id == 0)
        return ENOENT;           /* slot is empty or freed */
    if (caller_perms & SHM_PERM_ADMIN)
        return 0;                /* admin skips ownership check */
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
 * Insert block @p b into the address-ordered free list and coalesce
 * with adjacent free blocks.
 *
 * The free list is maintained in ascending address order so that coalescing
 * can be done in a single pass without a second scan.
 */
static void freelist_insert(shm_region_t *r, shm_block_hdr_t *b)
{
    shm_region_hdr_t *rh   = region_hdr(r);
    size_t            b_ho = hoff_of_blk(r, b);  /* hoff of the block to insert */

    /* Clear allocation metadata; the block is now free. */
    b->allocated = 0;
    b->obj_id    = 0;
    b->owner     = 0;
    b->perms     = 0;
    b->type_tag  = 0;

    /* Walk the free list to find the insertion point (address order). */
    uint64_t prev_lnk = 0;                  /* link to the predecessor slot */
    uint64_t cur_lnk  = rh->free_list;      /* link to the current slot */

    while (cur_lnk) {
        shm_block_hdr_t *cur = blk_of_link(r, cur_lnk);
        if (hoff_of_blk(r, cur) > b_ho)
            break;                           /* found the first block after b */
        prev_lnk = cur_lnk;
        cur_lnk  = cur->next_free;
    }

    /* Splice b between prev and cur in the linked list. */
    b->next_free = cur_lnk;                  /* b points forward to cur */
    if (prev_lnk == 0)
        rh->free_list = link_enc(b_ho);      /* b becomes the new list head */
    else
        blk_of_link(r, prev_lnk)->next_free = link_enc(b_ho);  /* prev → b */

    /* Coalesce b with its successor (cur) if they are adjacent in memory. */
    if (cur_lnk) {
        shm_block_hdr_t *cur = blk_of_link(r, cur_lnk);
        if (b_ho + b->total_size == hoff_of_blk(r, cur)) {
            /* Blocks touch: merge by adding cur's size to b. */
            b->total_size += cur->total_size;  /* absorb cur's size into b */
            b->next_free   = cur->next_free;   /* skip cur in the list */
            cur->magic     = 0;                /* poison merged block's magic */
        }
    }

    /* Coalesce the predecessor (prev) with b if they are adjacent. */
    if (prev_lnk) {
        shm_block_hdr_t *prev   = blk_of_link(r, prev_lnk);
        size_t           pr_ho  = hoff_of_blk(r, prev);
        if (pr_ho + prev->total_size == b_ho) {
            /* Blocks touch: merge by adding b's size to prev. */
            prev->total_size += b->total_size;  /* absorb b into prev */
            prev->next_free   = b->next_free;   /* skip b in the list */
            b->magic          = 0;              /* poison merged block's magic */
        }
    }
}

/**
 * Remove a specific block from the free list (used during in-place growth).
 *
 * Linear scan is O(free-list length) but acceptable for shared-memory use.
 */
static void freelist_remove(shm_region_t *r, shm_block_hdr_t *target)
{
    shm_region_hdr_t *rh          = region_hdr(r);
    uint64_t          target_lnk  = link_enc(hoff_of_blk(r, target));
    uint64_t          prev_lnk    = 0;
    uint64_t          cur_lnk     = rh->free_list;

    while (cur_lnk && cur_lnk != target_lnk) {
        prev_lnk = cur_lnk;
        cur_lnk  = blk_of_link(r, cur_lnk)->next_free;
    }
    if (cur_lnk != target_lnk)
        return;  /* target not on the free list (should not happen) */

    /* Unlink target by bridging its predecessor to its successor. */
    if (prev_lnk == 0)
        rh->free_list = target->next_free;   /* target was the head */
    else
        blk_of_link(r, prev_lnk)->next_free = target->next_free;

    target->next_free = 0;  /* clear the link now that the block is detached */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal heap allocator  (called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Find the first free block large enough for @p need_total bytes (including
 * the block header), remove it from the free list, optionally split off a
 * remainder free block, and return the new allocated block.
 *
 * @return Pointer to the allocated block, or NULL if the heap is exhausted.
 */
static shm_block_hdr_t *heap_alloc_block(shm_region_t *r, size_t need_total)
{
    shm_region_hdr_t *rh       = region_hdr(r);
    uint64_t          prev_lnk = 0;           /* predecessor in free list */
    uint64_t          cur_lnk  = rh->free_list;  /* current candidate */

    while (cur_lnk) {
        shm_block_hdr_t *b = blk_of_link(r, cur_lnk);

        if (b->total_size >= need_total) {
            /* This block is large enough.  Try to split off the remainder. */
            size_t rem = b->total_size - need_total;

            if (rem >= BLOCK_MIN) {
                /* Remainder is large enough to form a useful free block. */
                shm_block_hdr_t *nb = (shm_block_hdr_t *)((char *)b + need_total);
                nb->magic        = SHM_ALLOC_MAGIC_BLOCK;  /* initialise magic */
                nb->type_tag     = 0;
                nb->owner        = 0;
                nb->perms        = 0;
                nb->allocated    = 0;       /* mark as free */
                nb->obj_id       = 0;
                nb->payload_cap  = rem - sizeof(shm_block_hdr_t);
                nb->total_size   = rem;
                nb->next_free    = b->next_free;  /* inherit b's successor */

                b->total_size    = need_total;    /* trim b to what we need */
                b->payload_cap   = need_total - sizeof(shm_block_hdr_t);

                /* Replace b in the free list with the new split block nb. */
                uint64_t nb_lnk = link_enc(hoff_of_blk(r, nb));
                if (prev_lnk == 0)
                    rh->free_list = nb_lnk;
                else
                    blk_of_link(r, prev_lnk)->next_free = nb_lnk;
            } else {
                /* Remainder is too small; absorb it (internal fragmentation). */
                if (prev_lnk == 0)
                    rh->free_list = b->next_free;
                else
                    blk_of_link(r, prev_lnk)->next_free = b->next_free;
            }

            /* Mark b as allocated and clear its free-list link. */
            b->allocated  = 1;
            b->next_free  = 0;
            /* Accounting: track total heap bytes committed. */
            rh->used_bytes  += b->total_size;
            rh->block_count += 1;
            return b;
        }

        prev_lnk = cur_lnk;
        cur_lnk  = b->next_free;   /* advance to the next candidate */
    }

    return NULL;  /* heap exhausted: no free block is large enough */
}

/**
 * Return block @p b to the free list and update accounting.
 * Does NOT touch the directory; callers must update the directory separately.
 */
static void heap_free_block(shm_region_t *r, shm_block_hdr_t *b)
{
    shm_region_hdr_t *rh = region_hdr(r);
    rh->used_bytes  -= b->total_size;    /* reduce committed byte count */
    rh->block_count -= 1;                /* one fewer live block */
    freelist_insert(r, b);               /* put back into the address-ordered list */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Directory helpers  (called under the region mutex)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Find a live directory entry whose id == @p obj_id.
 * Returns a pointer into the shared directory, or NULL if not found.
 */
static shm_obj_entry_t *dir_find_by_id(const shm_region_t *r, uint64_t obj_id)
{
    shm_region_hdr_t *rh = region_hdr((shm_region_t *)r);
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        shm_obj_entry_t *e = dir_slot(r, i);
        if (e->id == obj_id && e->alive)
            return e;  /* exact id match; alive guard protects reused slots */
    }
    return NULL;  /* not found */
}

/**
 * Find a live directory entry whose name equals @p name (case-sensitive).
 */
static shm_obj_entry_t *dir_find_by_name(const shm_region_t *r, const char *name)
{
    shm_region_hdr_t *rh = region_hdr((shm_region_t *)r);
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        shm_obj_entry_t *e = dir_slot(r, i);
        if (e->alive && strcmp(e->name, name) == 0)
            return e;  /* name match */
    }
    return NULL;
}

/**
 * Find an empty directory slot (id == 0 or alive == 0).
 * Returns a pointer to the slot, or NULL if the directory is full.
 */
static shm_obj_entry_t *dir_find_free_slot(const shm_region_t *r)
{
    shm_region_hdr_t *rh = region_hdr((shm_region_t *)r);
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        shm_obj_entry_t *e = dir_slot(r, i);
        if (e->id == 0 || !e->alive)
            return e;  /* usable slot */
    }
    return NULL;  /* directory is full */
}

/** Count the number of live directory entries (for stats). */
static uint32_t dir_count_live(const shm_region_t *r)
{
    shm_region_hdr_t *rh = region_hdr((shm_region_t *)r);
    uint32_t count = 0;
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        if (dir_slot(r, i)->alive)
            count++;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Region initialisation helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Compute the region layout (dir_offset, data_offset) and initialise the
 * header + directory + initial free block.  Called only by the creating process.
 *
 * @param guard_pages  PROT_NONE pages reserved at the end of the mapping;
 *                     excluded from the initial free-list heap.
 */
static void region_init(shm_region_t *r, size_t total_size,
                        shm_backend_t backend, uint32_t dir_cap,
                        const char *name, uint32_t guard_pages)
{
    size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
    size_t guard_bytes = (size_t)guard_pages * page_sz;

    /* Step 1: compute offsets.
     * The region header is at offset 0; round its size up to HEAP_ALIGN. */
    size_t hdr_sz  = align_up(sizeof(shm_region_hdr_t), HEAP_ALIGN);
    /* The directory follows the header. */
    size_t dir_off = hdr_sz;
    /* Each directory slot is a shm_obj_entry_t; the whole array is pre-sized. */
    size_t dir_sz  = align_up((size_t)dir_cap * sizeof(shm_obj_entry_t), HEAP_ALIGN);
    /* The heap starts right after the directory. */
    size_t data_off = dir_off + dir_sz;

    if (total_size <= data_off + guard_bytes + BLOCK_MIN) {
        /* Caller must have validated size; keep going with empty heap. */
        guard_bytes = 0;
        guard_pages = 0;
    }

    /* Step 2: zero the usable part of the mapping (skip PROT_NONE guard). */
    memset(r->base, 0, total_size - guard_bytes);

    /* Step 3: fill in the region header. */
    shm_region_hdr_t *rh = region_hdr(r);
    rh->magic        = SHM_ALLOC_MAGIC_REGION;
    rh->version      = SHM_ALLOC_VERSION;
    rh->backend      = (uint32_t)backend;
    rh->dir_capacity = dir_cap;
    rh->region_size  = total_size;
    rh->dir_offset   = dir_off;
    rh->data_offset  = data_off;
    rh->used_bytes   = 0;
    rh->block_count  = 0;
    rh->next_id      = 1;   /* IDs start at 1; 0 is the free-slot sentinel */
    rh->host_count   = 0;   /* incremented by shm_region_open after init */
    rh->ns_inode     = shm_ns_ipc_inode();    /* record creator's IPC namespace */
    rh->cgroup_hash  = shm_ns_cgroup_hash();  /* record creator's cgroup path hash */
    /* Raw pointers embedded in the region are only valid if every process
     * maps at this same virtual address (see attach path below). */
    rh->map_base_addr = (uint64_t)(uintptr_t)r->base;
    rh->guard_pages   = guard_pages;

    /* Copy the name / path for debugging and re-identification. */
    if (name)
        strncpy(rh->shm_name, name, sizeof(rh->shm_name) - 1);

    /* Step 4: initialise the process-shared robust mutex. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    /* PTHREAD_PROCESS_SHARED allows the mutex to be used across processes
     * that map the same shared-memory region. */
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    /* PTHREAD_MUTEX_ROBUST causes the mutex to be recoverable if the owner
     * dies while holding it (the next lock attempt returns EOWNERDEAD). */
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&rh->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    /* Step 5: create the initial single free block covering the usable heap
     * (everything except the trailing PROT_NONE guard pages). */
    size_t heap_sz = total_size - data_off - guard_bytes;
    shm_block_hdr_t *initial = blk_at_hoff(r, 0);  /* first block at hoff=0 */
    initial->magic       = SHM_ALLOC_MAGIC_BLOCK;
    initial->type_tag    = 0;
    initial->owner       = 0;
    initial->perms       = 0;
    initial->allocated   = 0;                      /* mark as free */
    initial->obj_id      = 0;
    initial->payload_cap = heap_sz - sizeof(shm_block_hdr_t);
    initial->total_size  = heap_sz;
    initial->next_free   = 0;                      /* only block; no successor */

    /* The free list starts with this single block at hoff=0 → link=1. */
    rh->free_list = link_enc(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mutex helpers  (handle EOWNERDEAD recovery)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Lock the region mutex.  Handles the EOWNERDEAD case where a process died
 * while holding the lock by calling pthread_mutex_consistent() to recover.
 *
 * @return 0 on success; an errno on unrecoverable failure.
 */
static int region_lock(shm_region_t *r)
{
    shm_region_hdr_t *rh = region_hdr(r);
    int rc = pthread_mutex_lock(&rh->mutex);

    if (rc == EOWNERDEAD) {
        /* The previous owner died; mark the mutex as consistent so it can
         * be used again.  The heap may be in an inconsistent state, but for
         * many use-cases partial recovery is better than refusing all access. */
        pthread_mutex_consistent(&rh->mutex);
        rc = 0;  /* treat as successful lock; caller should re-validate state */
    }
    return rc;
}

static void region_unlock(shm_region_t *r)
{
    pthread_mutex_unlock(&region_hdr(r)->mutex);
}

/*
 * Read the capacity of a Linux device-DAX node from sysfs.
 *
 * /dev/daxX.Y is a character device: fstat(2).st_size is not the memory
 * capacity (often 0).  The size in bytes is published as a decimal string at:
 *   /sys/bus/dax/devices/daxX.Y/size   (current kernel)
 *   /sys/class/dax/daxX.Y/size         (legacy compat driver)
 *
 * Returns 0 if the size cannot be determined.
 */
static size_t dax_device_size_bytes(const char *dev_path)
{
    const char *base = strrchr(dev_path, '/');
    base = base ? base + 1 : dev_path;
    if (base[0] == '\0')
        return 0;

    static const char *const patterns[] = {
        "/sys/bus/dax/devices/%s/size",
        "/sys/class/dax/%s/size",
        NULL
    };

    for (int i = 0; patterns[i] != NULL; i++) {
        char sysfs_path[256];
        if (snprintf(sysfs_path, sizeof(sysfs_path), patterns[i], base)
                >= (int)sizeof(sysfs_path))
            continue;

        FILE *fp = fopen(sysfs_path, "r");
        if (!fp)
            continue;

        unsigned long long val = 0;
        int n = fscanf(fp, "%llu", &val);
        fclose(fp);

        if (n == 1 && val >= 4096)
            return (size_t)val;
    }

    return 0;
}

/**
 * Read the VMA that contains @p addr from /proc/self/maps.
 * @return VMA length in bytes, or 0 on failure.
 */
static size_t vma_length_containing(const void *addr)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;

    uintptr_t needle = (uintptr_t)addr;
    char line[256];
    size_t found = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long start = 0, end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2)
            continue;
        if (needle >= start && needle < end) {
            found = (size_t)(end - start);
            break;
        }
    }
    fclose(f);
    return found;
}

/**
 * If a DAX mmap overshot @p want_size (gem5 or kernel quirk mapping the
 * whole device), chop the excess with munmap so offsets past shm_size
 * become unmapped and cannot produce gem5 packets past the window.
 */
static void chop_dax_overmap(void *base, size_t want_size, size_t dev_size)
{
    if (!base || want_size < 4096)
        return;

    size_t vma_len = vma_length_containing(base);
    size_t excess = 0;

    if (vma_len > want_size)
        excess = vma_len - want_size;
    else if (dev_size > want_size)
        excess = dev_size - want_size; /* try anyway; munmap fails if absent */

    if (excess == 0)
        return;

    if (munmap((char *)base + want_size, excess) == 0) {
        fprintf(stderr,
                "shm_alloc: chopped %zu bytes past window (vma was %zu, "
                "want %zu) — OOB past shm_size will now SIGSEGV\n",
                excess, vma_len ? vma_len : want_size + excess, want_size);
    }

    vma_len = vma_length_containing(base);
    if (vma_len != 0 && vma_len != want_size) {
        fprintf(stderr,
                "shm_alloc: WARNING VMA length %zu != requested window %zu "
                "(gem5 OOB risk if VMA is larger)\n",
                vma_len, want_size);
    }
}

/**
 * Install PROT_NONE guard pages:
 *   1) last @p guard_bytes of the DAX/POSIX mapping (in-region)
 *   2) anonymous PROT_NONE immediately after the mapping (post-VMA)
 * Re-applied on every attach (mprotect is per-process).
 */
static int install_guard_pages(shm_region_t *reg, uint32_t guard_pages)
{
    size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
    size_t guard_bytes = (size_t)guard_pages * page_sz;

    reg->guard_bytes = guard_bytes;
    reg->post_guard_bytes = 0;

    if (guard_pages == 0 || guard_bytes == 0)
        return 0;
    if (reg->size <= guard_bytes)
        return EINVAL;

    void *in_guard = (char *)reg->base + reg->size - guard_bytes;
    if (mprotect(in_guard, guard_bytes, PROT_NONE) != 0) {
        int err = errno;
        fprintf(stderr, "shm_alloc: mprotect in-region guard failed: %s\n",
                strerror(err));
        return err;
    }

    void *post = mmap((char *)reg->base + reg->size, guard_bytes,
                      PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
    if (post == MAP_FAILED) {
        /* Non-fatal: in-region guard still catches walks within the window. */
        fprintf(stderr,
                "shm_alloc: WARNING post-VMA guard mmap failed (%s); "
                "in-region guard (%zu pages) is still active at %p\n",
                strerror(errno), (size_t)guard_pages, in_guard);
    } else {
        reg->post_guard_bytes = guard_bytes;
    }

    fprintf(stderr,
            "shm_alloc: guards active — in-region PROT_NONE %zu bytes at %p "
            "(offsets %zu..%zu)%s\n",
            guard_bytes, in_guard,
            reg->size - guard_bytes, reg->size,
            reg->post_guard_bytes
                ? "; post-VMA anonymous PROT_NONE after mapping"
                : "");
    return 0;
}

/* Candidate fixed VAs for create when the preferred base is busy. */
static const uintptr_t shm_fixed_va_candidates[] = {
    (uintptr_t)0x4000000000ULL,
    (uintptr_t)0x5000000000ULL,
    (uintptr_t)0x6000000000ULL,
    (uintptr_t)0x7000000000ULL,
    (uintptr_t)0x8000000000ULL,
};

/**
 * Map @p size bytes at a stable high VA.  With require_fixed, never falls
 * back to mmap(NULL) (raw shared pointers would be unsafe).
 */
static void *mmap_region_fixed(int fd, size_t size, uintptr_t prefer,
                               bool require_fixed, bool *out_fixed)
{
    static const size_t ncands =
        sizeof(shm_fixed_va_candidates) / sizeof(shm_fixed_va_candidates[0]);

    /* Try preferred first, then the candidate list. */
    uintptr_t try_list[6];
    size_t ntry = 0;
    try_list[ntry++] = prefer ? prefer : SHM_PREFERRED_MAP_BASE;
    for (size_t i = 0; i < ncands; i++) {
        if (shm_fixed_va_candidates[i] != try_list[0])
            try_list[ntry++] = shm_fixed_va_candidates[i];
    }

    for (size_t i = 0; i < ntry; i++) {
        void *base = mmap((void *)try_list[i], size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (base != MAP_FAILED) {
            if (out_fixed) *out_fixed = true;
            return base;
        }
    }

    if (require_fixed) {
        if (out_fixed) *out_fixed = false;
        return MAP_FAILED;
    }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (out_fixed) *out_fixed = false;
    return base;
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
        return EINVAL;  /* mandatory arguments must be non-NULL */

    /* ── Apply defaults for any zero-valued option fields. ── */
    shm_backend_t backend     = opts ? opts->backend    : SHM_BACKEND_POSIX;
    uint32_t      flags       = opts ? opts->flags      : 0u;
    uint32_t      dir_cap     = opts ? opts->dir_capacity : 0u;
    mode_t        mode        = opts ? opts->mode        : 0;
    uint32_t      guard_pages = opts ? opts->guard_pages : 0u;
    bool          creating    = (flags & SHM_OPEN_CREATE) != 0;
    bool          require_fixed = (flags & SHM_OPEN_REQUIRE_FIXED) != 0;

    if (dir_cap == 0)  dir_cap = DIR_CAP_DEFAULT;  /* apply default dir size */
    if (mode   == 0)   mode    = 0660;              /* apply default mode */
    if (size   < 4096) size    = 0;                 /* will be detected below */

    /* ── Allocate the handle struct on the caller's C heap. ── */
    shm_region_t *reg = calloc(1, sizeof(*reg));
    if (!reg) return ENOMEM;

    reg->name = strdup(name_or_path);  /* keep a copy for shm_unlink later */
    if (!reg->name) { free(reg); return ENOMEM; }
    reg->backend  = backend;
    reg->creator  = creating;
    reg->fd       = -1;

    /* ── Open the underlying file descriptor. ── */
    if (backend == SHM_BACKEND_POSIX) {
        /* shm_open creates / opens a POSIX shared-memory object. */
        int oflags = creating ? (O_CREAT | O_RDWR) : O_RDWR;
        reg->fd = shm_open(name_or_path, oflags, mode);
        if (reg->fd < 0) {
            int err = errno;
            free(reg->name); free(reg);
            return err;
        }
        if (creating && size >= 4096) {
            /* Extend the shared-memory object to the requested size. */
            if (ftruncate(reg->fd, (off_t)size) != 0) {
                int err = errno;
                close(reg->fd); shm_unlink(name_or_path);
                free(reg->name); free(reg);
                return err;
            }
        }
        if (!creating) {
            /* Read the actual size from the existing object. */
            struct stat st;
            if (fstat(reg->fd, &st) != 0) {
                int err = errno;
                close(reg->fd); free(reg->name); free(reg);
                return err;
            }
            size = (size_t)st.st_size;  /* trust the stored size */
        }
    } else {
        /* DAX / disaggregated memory: open the device file directly. */
        reg->fd = open(name_or_path, O_RDWR);
        if (reg->fd < 0) {
            int err = errno;
            free(reg->name); free(reg);
            return err;
        }
        {
            /* Character device: capacity comes from sysfs, not fstat. */
            size_t dev_size = dax_device_size_bytes(name_or_path);
            if (dev_size < 4096) {
                struct stat st;
                if (fstat(reg->fd, &st) == 0 && (size_t)st.st_size >= 4096)
                    dev_size = (size_t)st.st_size;
            }
            reg->dev_size = dev_size;
            if (size < 4096) {
                /* size=0 / unspecified → map the entire device. */
                if (dev_size < 4096) {
                    fprintf(stderr,
                            "shm_alloc: cannot determine size of DAX device '%s' "
                            "(try: cat /sys/bus/dax/devices/<name>/size)\n",
                            name_or_path);
                    close(reg->fd);
                    free(reg->name); free(reg);
                    return ENODEV;
                }
                size = dev_size;
            } else if (dev_size >= 4096 && size > dev_size) {
                /* Caller asked for more than the device holds. */
                fprintf(stderr,
                        "shm_alloc: requested mapping %zu bytes exceeds DAX "
                        "device '%s' capacity %zu bytes\n",
                        size, name_or_path, dev_size);
                close(reg->fd);
                free(reg->name); free(reg);
                return ENOSPC;
            }
            if (dev_size >= 4096 && size < dev_size) {
                fprintf(stderr,
                        "shm_alloc: DAX device capacity %zu MB, mapping only "
                        "%zu MB window (tail of device must stay unmapped)\n",
                        dev_size / (1024 * 1024), size / (1024 * 1024));
            }
        }
    }

    if (creating && size < 4096) {
        /* Guard: region must be at least one page so the header fits. */
        close(reg->fd);
        if (backend == SHM_BACKEND_POSIX) shm_unlink(name_or_path);
        free(reg->name); free(reg);
        return EINVAL;
    }

    /* In-region guards need room; refuse tiny windows. */
    {
        size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
        size_t need_guard = (size_t)guard_pages * page_sz;
        if (creating && guard_pages > 0 && size <= need_guard + 4096) {
            fprintf(stderr,
                    "shm_alloc: region %zu too small for %u guard pages\n",
                    size, guard_pages);
            close(reg->fd);
            if (backend == SHM_BACKEND_POSIX && creating)
                shm_unlink(name_or_path);
            free(reg->name); free(reg);
            return EINVAL;
        }
    }

    if (creating) {
        /*
         * Creator: map exactly `size` bytes at a stable high VA.
         * DAX + raw pointers (memcached) must use REQUIRE_FIXED — never
         * fall back to an ASLR address near libc (that pattern produced
         * gem5 OOB PAs like 0x1839ADF20 when the VMA overshot shm_size).
         */
        bool fixed = false;
        void *base = mmap_region_fixed(reg->fd, size, SHM_PREFERRED_MAP_BASE,
                                       require_fixed || backend == SHM_BACKEND_DAX,
                                       &fixed);
        if (base == MAP_FAILED) {
            int err = errno ? errno : EBUSY;
            fprintf(stderr,
                    "shm_alloc: failed to map %zu bytes at a fixed VA "
                    "(REQUIRE_FIXED/DAX) — free 0x4000000000+ or disable ASLR\n",
                    size);
            close(reg->fd);
            if (backend == SHM_BACKEND_POSIX) shm_unlink(name_or_path);
            free(reg->name); free(reg);
            return err;
        }
        reg->base = base;
        reg->size = size;

        /* Chop any over-map past the window (gem5 sometimes maps full DAX). */
        if (backend == SHM_BACKEND_DAX)
            chop_dax_overmap(reg->base, size, reg->dev_size);

        region_init(reg, size, backend, dir_cap, name_or_path, guard_pages);

        if (install_guard_pages(reg, guard_pages) != 0) {
            munmap(reg->base, reg->size);
            close(reg->fd);
            if (backend == SHM_BACKEND_POSIX) shm_unlink(name_or_path);
            free(reg->name); free(reg);
            return EFAULT;
        }

        size_t vma_len = vma_length_containing(reg->base);
        fprintf(stderr,
                "shm_alloc: created %s region size=%zu VA=%p map_base_addr=%p "
                "(%s) vma_rw=%zu guard_pages=%u\n",
                backend == SHM_BACKEND_DAX ? "DAX" : "POSIX",
                size, reg->base,
                (void *)(uintptr_t)region_hdr(reg)->map_base_addr,
                fixed ? "fixed" : "relocated",
                vma_len, guard_pages);
    } else {
        /*
         * Attacher: never map the whole DAX device.  Peek one page to read
         * region_size + map_base_addr, then map exactly that window at the
         * creator's VA so raw pointers embedded in the region stay valid and
         * DAX-relative offsets cannot exceed the gem5/shm_size window.
         */
        const size_t peek_sz = 4096;
        void *peek = mmap(NULL, peek_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
                          reg->fd, 0);
        if (peek == MAP_FAILED) {
            int err = errno;
            close(reg->fd);
            free(reg->name); free(reg);
            return err;
        }

        const shm_region_hdr_t *ph = (const shm_region_hdr_t *)peek;
        if (ph->magic != SHM_ALLOC_MAGIC_REGION || ph->version != SHM_ALLOC_VERSION) {
            fprintf(stderr,
                    "shm_alloc: attach refused — bad magic/version "
                    "(got version %u, need %u); wipe DAX and shm_create\n",
                    ph->version, SHM_ALLOC_VERSION);
            munmap(peek, peek_sz);
            close(reg->fd);
            free(reg->name); free(reg);
            return ENOENT;
        }

        size_t    win         = ph->region_size;
        uintptr_t want_va     = (uintptr_t)ph->map_base_addr;
        uint32_t  hdr_guards  = ph->guard_pages;
        uint64_t  ns_inode    = ph->ns_inode;
        uint64_t  cg_hash     = ph->cgroup_hash;
        munmap(peek, peek_sz);

        /* Attacher ignores caller guard_pages; header is authoritative. */
        guard_pages = hdr_guards;

        if (win < 4096 || want_va == 0) {
            fprintf(stderr,
                    "shm_alloc: attach refused — region_size=%zu map_base_addr=%p "
                    "(re-create the region with a current shm_alloc)\n",
                    win, (void *)want_va);
            close(reg->fd);
            free(reg->name); free(reg);
            return EINVAL;
        }

        /* Optional namespace / cgroup checks use values captured from the peek. */
        if ((flags & SHM_OPEN_ENFORCE_NS) && ns_inode != 0) {
            uint64_t my_ns = shm_ns_ipc_inode();
            if (my_ns != 0 && my_ns != ns_inode) {
                close(reg->fd);
                free(reg->name); free(reg);
                return EPERM;
            }
        }
        if ((flags & SHM_OPEN_ENFORCE_CGROUP) && cg_hash != 0) {
            uint64_t my_cg = shm_ns_cgroup_hash();
            if (my_cg != 0 && my_cg != cg_hash) {
                close(reg->fd);
                free(reg->name); free(reg);
                return EPERM;
            }
        }

        /*
         * Must map at the creator VA for raw pointers.  DAX and REQUIRE_FIXED
         * refuse a relocated fallback (that is what produced PA 0x1839ADF20).
         */
        bool require = require_fixed || backend == SHM_BACKEND_DAX;
        void *base = mmap((void *)want_va, win,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED_NOREPLACE,
                          reg->fd, 0);
        bool fixed = (base != MAP_FAILED);
        if (!fixed) {
            if (require) {
                fprintf(stderr,
                        "shm_alloc: attach FAILED — cannot map at creator VA %p "
                        "(size=%zu). Free that address or disable ASLR; "
                        "relocated attach is not allowed for DAX/raw pointers.\n",
                        (void *)want_va, win);
                close(reg->fd);
                free(reg->name); free(reg);
                return EBUSY;
            }
            base = mmap(NULL, win, PROT_READ | PROT_WRITE, MAP_SHARED,
                        reg->fd, 0);
            if (base == MAP_FAILED) {
                int err = errno;
                close(reg->fd);
                free(reg->name); free(reg);
                return err;
            }
            fprintf(stderr,
                    "shm_alloc: WARNING attach VA %p != creator %p "
                    "(size=%zu) — offset API ok; raw shared pointers "
                    "are NOT valid in this process\n",
                    base, (void *)want_va, win);
        }

        reg->base = base;
        reg->size = win;

        if (backend == SHM_BACKEND_DAX)
            chop_dax_overmap(reg->base, win, reg->dev_size);

        shm_region_hdr_t *rh = region_hdr(reg);
        if (rh->magic != SHM_ALLOC_MAGIC_REGION || rh->version != SHM_ALLOC_VERSION
                || rh->region_size != win) {
            munmap(reg->base, reg->size);
            close(reg->fd);
            free(reg->name); free(reg);
            return ENOENT;
        }

        if (install_guard_pages(reg, guard_pages) != 0) {
            munmap(reg->base, reg->size);
            close(reg->fd);
            free(reg->name); free(reg);
            return EFAULT;
        }

        size_t vma_len = vma_length_containing(reg->base);
        fprintf(stderr,
                "shm_alloc: attached %s region size=%zu VA=%p (%s) vma_rw=%zu "
                "guard_pages=%u\n",
                backend == SHM_BACKEND_DAX ? "DAX" : "POSIX",
                win, reg->base, fixed ? "fixed" : "relocated",
                vma_len, guard_pages);
    }

    /* ── Increment the attached host counter atomically under the mutex. ── */
    region_lock(reg);
    region_hdr(reg)->host_count++;
    region_unlock(reg);

    *out_region = reg;
    return 0;
}

void shm_region_close(shm_region_t *region, bool unlink_name)
{
    if (!region) return;  /* safe no-op for NULL handle */

    /* Decrement the host counter while we still hold the mapping. */
    if (region->base) {
        region_lock(region);
        shm_region_hdr_t *rh = region_hdr(region);
        if (rh->host_count > 0)
            rh->host_count--;   /* track departure of this host */
        region_unlock(region);

        /* Drop post-VMA anonymous guard first (it sits at base+size). */
        if (region->post_guard_bytes > 0) {
            munmap((char *)region->base + region->size,
                   region->post_guard_bytes);
        }
        munmap(region->base, region->size);  /* release virtual address space */
    }

    if (region->fd >= 0)
        close(region->fd);   /* release file descriptor */

    /* Only POSIX shm objects can be unlinked; DAX device nodes cannot. */
    if (unlink_name && region->name && region->backend == SHM_BACKEND_POSIX)
        shm_unlink(region->name);  /* remove the /dev/shm entry */

    free(region->name);  /* release name copy */
    free(region);        /* release handle struct */
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

    /* Total block size needed (header + payload, rounded up to alignment). */
    size_t need_total = align_up(sizeof(shm_block_hdr_t) + payload_size, HEAP_ALIGN);

    region_lock(region);

    /* Find an empty directory slot before touching the heap. */
    shm_obj_entry_t *slot = dir_find_free_slot(region);
    if (!slot) {
        region_unlock(region);
        return ENOSPC;   /* directory is full; cannot register the new object */
    }

    /* Allocate a heap block large enough for the payload. */
    shm_block_hdr_t *b = heap_alloc_block(region, need_total);
    if (!b) {
        region_unlock(region);
        return ENOMEM;   /* heap is full */
    }

    /* Assign a new monotonic ID (never reused even after free). */
    shm_region_hdr_t *rh = region_hdr(region);
    uint64_t new_id = rh->next_id++;   /* post-increment for next call */

    /* Fill in the block header with ownership information. */
    b->type_tag  = type_tag;
    b->owner     = user_id;
    b->perms     = perms;
    b->obj_id    = new_id;   /* link block → directory entry */

    /* Zero the payload area so callers get a clean slate. */
    memset((char *)b + sizeof(shm_block_hdr_t), 0, b->payload_cap);

    /* Register in the object directory. */
    memset(slot, 0, sizeof(*slot));              /* clear any stale fields */
    slot->id        = new_id;
    slot->off       = poff_of_blk(region, b);   /* heap-relative payload offset */
    slot->capacity  = b->payload_cap;
    slot->used_size = payload_size;              /* logical = physical at creation */
    slot->type_tag  = type_tag;
    slot->owner     = user_id;
    slot->perms     = perms;
    slot->alive     = 1;                         /* mark as live */

    /* Copy the optional name (truncated if too long). */
    if (name && name[0] != '\0')
        strncpy(slot->name, name, DIR_NAME_MAX - 1);

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

    /* Find the directory entry for this object. */
    shm_obj_entry_t *slot = dir_find_by_id(region, obj_id);
    if (!slot) {
        region_unlock(region);
        return ENOENT;   /* no such object */
    }

    /* Check that the caller has the right to free this object. */
    int acc = check_dir_access(slot, user_id, caller_perms, SHM_PERM_FREE);
    if (acc != 0) {
        region_unlock(region);
        return acc;
    }

    /* Retrieve the block header from the stored payload offset. */
    shm_block_hdr_t *b = blk_of_poff(region, slot->off);

    /* Return the block to the heap free list. */
    heap_free_block(region, b);

    /* Clear the directory entry (id=0 marks it as available for reuse). */
    memset(slot, 0, sizeof(*slot));   /* zero all fields including id and name */

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

    shm_block_hdr_t *b = blk_of_poff(region, slot->off);
    size_t new_total = align_up(sizeof(shm_block_hdr_t) + new_size, HEAP_ALIGN);

    if (new_total <= b->total_size) {
        /* ── Shrink ── */
        size_t rem = b->total_size - new_total;
        if (rem >= BLOCK_MIN) {
            /* Split off the excess as a new free block. */
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

            /* Update accounting for the trimmed main block. */
            shm_region_hdr_t *rh = region_hdr(region);
            rh->used_bytes -= rem;    /* capacity moved from used to free */
            b->total_size   = new_total;
            b->payload_cap  = new_total - sizeof(shm_block_hdr_t);

            /* Insert the freed tail into the address-ordered free list. */
            freelist_insert(region, tail);
        }
        /* In either case update the directory's logical size. */
        slot->capacity  = b->payload_cap;
        slot->used_size = new_size;

    } else {
        /* ── Grow ── */
        size_t b_ho    = hoff_of_blk(region, b);
        size_t next_ho = b_ho + b->total_size;   /* hoff of the next block in memory */
        bool   grew    = false;

        if (next_ho + sizeof(shm_block_hdr_t) < heap_size(region)) {
            shm_block_hdr_t *next = blk_at_hoff(region, next_ho);
            /* Check whether the next physical block is free and large enough. */
            if (next->magic == SHM_ALLOC_MAGIC_BLOCK && !next->allocated) {
                size_t combined = b->total_size + next->total_size;
                if (combined >= new_total) {
                    /* Expand b in-place by absorbing the adjacent free block. */
                    freelist_remove(region, next);      /* detach next from free list */
                    shm_region_hdr_t *rh = region_hdr(region);
                    rh->used_bytes += next->total_size; /* next bytes become used */
                    b->total_size   = combined;
                    b->payload_cap  = combined - sizeof(shm_block_hdr_t);
                    next->magic     = 0;                /* poison the absorbed header */

                    /* If there is leftover space after the new size, split it off. */
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
                        rh->used_bytes   -= leftover;   /* adjust accounting */
                        b->total_size     = new_total;
                        b->payload_cap    = new_total - sizeof(shm_block_hdr_t);
                        freelist_insert(region, tail);   /* add tail to free list */
                    }
                    grew = true;
                }
            }
        }

        if (!grew) {
            /* In-place growth not possible: allocate a new block and relocate. */
            shm_block_hdr_t *nb = heap_alloc_block(region, new_total);
            if (!nb) {
                region_unlock(region);
                return ENOMEM;   /* heap exhausted */
            }
            /* Copy the existing payload to the new location. */
            memcpy((char *)nb + sizeof(shm_block_hdr_t),
                   (char *)b  + sizeof(shm_block_hdr_t),
                   b->payload_cap);   /* copy old capacity (may be < new_size) */
            /* Zero the newly allocated extra space. */
            size_t extra = nb->payload_cap - b->payload_cap;
            if (extra > 0)
                memset((char *)nb + sizeof(shm_block_hdr_t) + b->payload_cap, 0, extra);

            /* Copy ownership and id to the new block. */
            nb->type_tag  = b->type_tag;
            nb->owner     = b->owner;
            nb->perms     = b->perms;
            nb->obj_id    = b->obj_id;

            /* Free the old block (returns it to the free list). */
            heap_free_block(region, b);

            /* Update the directory entry with the new offset. */
            slot->off       = poff_of_blk(region, nb);
            slot->capacity  = nb->payload_cap;
        } else {
            /* Update directory after successful in-place growth. */
            slot->capacity = b->payload_cap;
        }
        slot->used_size = new_size;
    }

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

    /* Brief lock to avoid torn reads on 64-bit fields. */
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

    shm_region_hdr_t *rh = region_hdr((shm_region_t *)region);

    /* Scan forward from *cursor until a live entry is found. */
    while (*cursor < rh->dir_capacity) {
        shm_obj_entry_t *e = dir_slot(region, *cursor);
        (*cursor)++;   /* always advance so the next call continues past here */

        if (e->alive && e->id != 0) {
            /* Snapshot the entry into the caller's struct. */
            out->id        = e->id;
            out->off       = e->off;
            out->capacity  = e->capacity;
            out->used_size = e->used_size;
            out->type_tag  = e->type_tag;
            out->owner     = e->owner;
            out->perms     = e->perms;
            strncpy(out->name, e->name, DIR_NAME_MAX - 1);
            out->name[DIR_NAME_MAX - 1] = '\0';   /* ensure NUL termination */
            return 0;   /* success: entry filled */
        }
    }
    return ENOENT;   /* iteration exhausted: no more live entries */
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

    /* The block header lives sizeof(shm_block_hdr_t) bytes before the payload. */
    shm_block_hdr_t *b = blk_of_poff(region, off);

    /* Validate magic and check ownership + permissions. */
    if (check_block_access(b, user_id, caller_perms, required_perms) != 0)
        return NULL;

    /* Return a pointer to the payload area (immediately after the header). */
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

    shm_block_hdr_t *b = blk_of_poff(region, off);

    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK)
        return EINVAL;   /* block header is corrupt or offset is wrong */

    /* Populate each optional output parameter if the pointer is non-NULL. */
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

    shm_block_hdr_t *b = blk_of_poff(region, off);

    /* Only the owner or an admin may change permissions. */
    int acc = check_block_access(b, user_id, caller_perms, SHM_PERM_WRITE);
    if (acc != 0) {
        region_unlock(region);
        return acc;
    }

    b->perms = new_perms;   /* update block header */

    /* Also update the directory entry so shm_dir_next() reflects the change. */
    shm_obj_entry_t *slot = dir_find_by_id(region, b->obj_id);
    if (slot)
        slot->perms = new_perms;

    region_unlock(region);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: pool statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

int shm_pool_stats(const shm_region_t *region, shm_pool_stats_t *out)
{
    if (!region || !out) return EINVAL;

    shm_region_hdr_t *rh = region_hdr((shm_region_t *)region);

    out->total_bytes  = region->size - rh->data_offset;   /* total heap bytes */
    out->used_bytes   = rh->used_bytes;
    out->free_bytes   = out->total_bytes - rh->used_bytes;
    out->block_count  = rh->block_count;
    out->next_id      = rh->next_id;
    out->host_count   = rh->host_count;
    out->dir_capacity = rh->dir_capacity;
    out->dir_used     = dir_count_live(region);            /* scan the directory */

    return 0;
}
