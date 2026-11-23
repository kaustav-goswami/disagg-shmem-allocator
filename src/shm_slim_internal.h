/**
 * @file  shm_slim_internal.h
 * @brief Internal on-disk layout + inline helpers for the v0.2 "slim" format.
 *
 * Not part of the public API; include shm_slim.h from user code instead.
 * Deliberately independent of shm_internal.h (the v2 layout) -- nothing in
 * this file is shared with, or compatible with, the v2 format.
 */

#ifndef SHM_SLIM_INTERNAL_H
#define SHM_SLIM_INTERNAL_H

#define _POSIX_C_SOURCE 200809L   /* robust mutexes, shm_open */

#include "shm_slim.h"
#include "shm_persist.h"   /* shm_persist(), shm_drain(), shm_flush_line() */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── Internal constants ──────────────────────────────────────────────────── */

/** Every heap block and the region header are rounded to this alignment. */
#define SHM_SLIM_ALIGN  8u

/**
 * Minimum physical block size: header + one alignment quantum of payload.
 * The extra 8 bytes beyond the header are not optional padding -- they are
 * where a *freed* block's free-list link lives (see shm_slim_set_free_link
 * below), so every block, however small the requested payload, must have
 * room for it once freed.
 */
#define SHM_SLIM_BLOCK_MIN  (sizeof(shm_slim_hdr_t) + SHM_SLIM_ALIGN)

/* ── In-memory struct definitions ───────────────────────────────────────── */

/**
 * Per-object heap block header. 24 bytes (was 16 before the `timestamp`
 * field below was added -- see SHM_SLIM_VERSION), always immediately
 * preceding the payload -- see shm_slim.h for the full design rationale.
 */
typedef struct shm_slim_hdr {
    uint32_t magic;     /* SHM_SLIM_MAGIC_LIVE / _LIVE_NAMED / _FREE */
    uint32_t key;       /* shm_slim_key_t; access-control token, 0 = open */
    uint64_t size;      /* total block size (header + payload), aligned */
    uint64_t timestamp; /* seconds since epoch, last time this header was
                          * written (publish, revoke, or free) -- a leak
                          * candidate is an object whose timestamp is old
                          * relative to how long its region has existed;
                          * this field only records the timestamp, it does
                          * not itself reclaim anything */
} shm_slim_hdr_t;

/* The whole point of this format: a live object's fixed overhead must not
 * exceed a typical malloc chunk header by much. */
_Static_assert(sizeof(shm_slim_hdr_t) == 24,
               "shm_slim_hdr_t must be exactly 24 bytes");

/**
 * Software-enforced back-invalidate hook (compile-time opt-in via
 * -DSHM_SLIM_SOFTWARE_BI). Called every time a block header is written --
 * shm_slim_alloc() (publish), shm_slim_set_key() (revoke), and
 * freelist_insert() (delete) -- with the address and length of exactly the
 * header bytes that changed.
 *
 * This is a testing hook for running against a simulator that models both
 * CXL back-invalidate modes:
 *   - hardware-enforced: the simulated CXL device snoops every DAX write
 *     automatically. dmalloc needs no code for this at all -- it is
 *     already what happens when SHM_SLIM_SOFTWARE_BI is left undefined,
 *     since the ordinary shm_persist() write is all real hardware would
 *     ever see or need.
 *   - software-enforced: the simulator instead expects software to
 *     explicitly signal which address range just changed, since there is
 *     no local instruction that can invalidate another host's cache --
 *     that is exactly the gap real hardware BISnp closes. This hook is
 *     that signal.
 *
 * shm_slim_soft_bi() itself is a weak no-op (defined in shm_slim.c); a
 * simulator integration overrides it (provide a strong, non-weak
 * definition of the same symbol in the final link, or replace the .o) to
 * actually notify the fabric model. What that override does -- a memory
 * write to a location the simulator watches, a magic/pseudo instruction
 * the simulator's CPU model traps on, or something else entirely -- is
 * intentionally not decided here: it depends on the specific simulator,
 * which is outside this repository.
 */
#if defined(SHM_SLIM_SOFTWARE_BI)
void shm_slim_soft_bi(const void *hdr_addr, size_t hdr_len);
#define SHM_SLIM_BI_NOTIFY(addr, len)  shm_slim_soft_bi((addr), (len))
#else
#define SHM_SLIM_BI_NOTIFY(addr, len)  ((void)0)
#endif

/**
 * Region header -- lives at base[0]. O(1) per region, not per object, so it
 * is not under the same pressure to be minimal as the block header is.
 */
typedef struct shm_slim_region_hdr {
    uint32_t        magic;         /* SHM_SLIM_MAGIC_REGION ("SHMA") */
    uint32_t        version;       /* SHM_SLIM_VERSION ("slim") */
    uint32_t        backend;       /* shm_slim_backend_t cast to uint32 */
    uint32_t        name_cap;      /* PHYSICAL capacity of the name table (hash table array size; see shm_slim_name_entry_t) */
    pthread_mutex_t mutex;         /* process-shared robust mutex */
    uint32_t        host_count;    /* attached processes */
    uint32_t        namespace_cap; /* capacity of the key->namespace table */
    size_t          region_size;   /* total mapping size in bytes */
    size_t          name_offset;   /* byte offset of the name table */
    size_t          namespace_offset; /* byte offset of the key->namespace table */
    size_t          data_offset;   /* byte offset of the heap */
    uint64_t        free_list;     /* 1-based hoff -> first free block */
    size_t          used_bytes;    /* bytes committed to live blocks */
    size_t          block_count;   /* live block count */
    uint64_t        ns_inode;      /* IPC namespace inode of creator (region-wide) */
    uint64_t        cgroup_hash;   /* FNV-1a hash of creator's cgroup (region-wide) */
    char            shm_name[64];  /* the name/path used at creation */
} shm_slim_region_hdr_t;

/**
 * Name table entry -- one per named object. Sized to how many objects the
 * application actually wants to name, not to the total object count.
 *
 * Open-addressed hash table (FNV-1a of the name, linear probing) as of
 * the parallel-lookup/hash-index work -- see WIP.md. `name_cap` in the
 * region header is the PHYSICAL slot count (inflated ~2x beyond whatever
 * capacity the caller requested, to keep the load factor <= 50% and
 * probe sequences short); `shm_slim_pool_stats()`/`d_name_capacity()`
 * report this physical count, since that's the range a slot-based scan
 * (`shm_slim_name_read_slot()` / `d_attach_slot()`) actually needs to
 * cover.
 */
typedef struct shm_slim_name_entry {
    char      name[SHM_SLIM_NAME_MAX];  /* NUL-padded; name[0]==0 -> empty */
    shm_off_t off;                       /* payload offset of the object */
} shm_slim_name_entry_t;

/**
 * Sentinel marking a deleted ("tombstone") name-table slot -- distinct
 * from both a genuinely-never-used slot (name[0]==0, which safely
 * terminates a probe-miss) and a live entry (any other first byte).
 * Open-addressing probe sequences must continue THROUGH a tombstone
 * rather than stopping at it, or entries placed further down the same
 * probe chain would become permanently unreachable. A non-printable
 * control byte that no name published through the public API can begin
 * with in practice (every name in this codebase is NUL-terminated ASCII
 * from snprintf) -- shm_slim_alloc() rejects any caller-supplied name
 * that actually begins with this byte outright (EINVAL), rather than
 * leaving it as an unenforced assumption that could silently corrupt the
 * table's probe-chain invariants.
 */
#define SHM_SLIM_NAME_TOMBSTONE ((char)0x01)

/**
 * Key -> namespace binding. Optional and separate from every object: an
 * access key can be confined to a specific Linux IPC namespace / cgroup
 * (finer-grained than the region-wide ns_inode/cgroup_hash above, which
 * only gate opening the region at all). Sized to how many distinct keys a
 * deployment actually binds, never to the object count -- binding a key
 * costs one 24-byte slot here regardless of how many objects use that key.
 *
 * A slot is empty iff ns_inode == 0 && cgroup_hash == 0 (key == 0 is
 * SHM_SLIM_KEY_OPEN, itself a legitimate bindable key, so it cannot double
 * as the "unused slot" sentinel).
 */
typedef struct shm_slim_namespace_entry {
    uint32_t key;          /* shm_slim_key_t this entry restricts */
    uint32_t _pad;
    uint64_t ns_inode;     /* required IPC namespace inode; 0 = not checked */
    uint64_t cgroup_hash;  /* required cgroup fingerprint; 0 = not checked */
} shm_slim_namespace_entry_t;

/** Per-process region handle. Not visible to other host processes. */
struct shm_slim_region {
    char                *name;     /* heap copy of the shm name / dax path */
    int                  fd;       /* file descriptor */
    void                *base;     /* mmap base pointer */
    size_t               size;     /* total mapping length in bytes */
    bool                 creator;  /* true if this process created the region */
    shm_slim_backend_t   backend;
};

/* ── Inline accessor helpers ─────────────────────────────────────────────── */

static inline size_t shm_slim_align_up(size_t n, size_t a)
{
    return (n + a - 1u) & ~(a - 1u);
}

static inline shm_slim_region_hdr_t *shm_slim_rh(const shm_slim_region_t *r)
{
    return (shm_slim_region_hdr_t *)r->base;
}

static inline shm_slim_name_entry_t *shm_slim_name_base(const shm_slim_region_t *r)
{
    return (shm_slim_name_entry_t *)((char *)r->base + shm_slim_rh(r)->name_offset);
}

static inline shm_slim_name_entry_t *shm_slim_name_slot(const shm_slim_region_t *r, uint32_t idx)
{
    return &shm_slim_name_base(r)[idx];
}

static inline shm_slim_namespace_entry_t *shm_slim_namespace_base(const shm_slim_region_t *r)
{
    return (shm_slim_namespace_entry_t *)((char *)r->base + shm_slim_rh(r)->namespace_offset);
}

static inline shm_slim_namespace_entry_t *shm_slim_namespace_slot(const shm_slim_region_t *r, uint32_t idx)
{
    return &shm_slim_namespace_base(r)[idx];
}

static inline char *shm_slim_heap_base(const shm_slim_region_t *r)
{
    return (char *)r->base + shm_slim_rh(r)->data_offset;
}

static inline size_t shm_slim_heap_size(const shm_slim_region_t *r)
{
    return r->size - shm_slim_rh(r)->data_offset;
}

/** Heap-header-offset (hoff) -> block header pointer. Unchecked. */
static inline shm_slim_hdr_t *shm_slim_blk_at_hoff(const shm_slim_region_t *r, size_t hoff)
{
    return (shm_slim_hdr_t *)(shm_slim_heap_base(r) + hoff);
}

/** Block header pointer -> heap-header-offset. */
static inline size_t shm_slim_hoff_of_blk(const shm_slim_region_t *r, const shm_slim_hdr_t *b)
{
    return (size_t)((const char *)b - shm_slim_heap_base(r));
}

/** Block header pointer -> heap-payload-offset (what callers store as shm_off_t). */
static inline shm_off_t shm_slim_poff_of_blk(const shm_slim_region_t *r, const shm_slim_hdr_t *b)
{
    return (shm_off_t)(shm_slim_hoff_of_blk(r, b) + sizeof(shm_slim_hdr_t));
}

/** Heap-payload-offset (poff) -> block header pointer. Unchecked. */
static inline shm_slim_hdr_t *shm_slim_blk_of_poff(const shm_slim_region_t *r, shm_off_t poff)
{
    return (shm_slim_hdr_t *)(shm_slim_heap_base(r) + poff - sizeof(shm_slim_hdr_t));
}

/**
 * Bounds-checked heap-payload-offset (poff) -> block header pointer.
 *
 * Carried forward from the v2 fix: a stale, corrupted, or out-of-range poff
 * must never turn into a pointer that lands outside
 * [heap_base, heap_base + heap_size) and gets dereferenced immediately.
 *
 * @return Pointer to the block header on success; NULL if poff is out of
 *         range, or the block's own `size` claims to extend past the end
 *         of the heap (a corrupted header).
 */
static inline shm_slim_hdr_t *shm_slim_blk_of_poff_checked(const shm_slim_region_t *r, shm_off_t poff)
{
    if (poff == SHM_OFF_NULL || poff < sizeof(shm_slim_hdr_t))
        return NULL;

    size_t hsz  = shm_slim_heap_size(r);
    size_t hoff = (size_t)poff - sizeof(shm_slim_hdr_t);

    if (hoff >= hsz || hoff + sizeof(shm_slim_hdr_t) > hsz)
        return NULL;

    shm_slim_hdr_t *b = shm_slim_blk_at_hoff(r, hoff);

    if ((b->magic == SHM_SLIM_MAGIC_LIVE ||
         b->magic == SHM_SLIM_MAGIC_LIVE_NAMED ||
         b->magic == SHM_SLIM_MAGIC_FREE) &&
        hoff + b->size > hsz)
        return NULL;   /* corrupted header: refuse it */

    return b;
}

/** Encode a heap-header-offset as a 1-based free-list link (0 = end of list). */
static inline uint64_t shm_slim_link_enc(size_t hoff) { return (uint64_t)hoff + 1u; }

/** Decode a free-list link back to its heap-header-offset. */
static inline size_t shm_slim_link_dec(uint64_t lnk) { return (size_t)(lnk - 1u); }

/** Dereference a free-list link; returns NULL when lnk == 0. */
static inline shm_slim_hdr_t *shm_slim_blk_of_link(const shm_slim_region_t *r, uint64_t lnk)
{
    return lnk ? shm_slim_blk_at_hoff(r, shm_slim_link_dec(lnk)) : NULL;
}

/**
 * Free-list link storage for a FREE block. Overlaid on the block's own
 * payload area (the first 8 bytes right after the header) rather than kept
 * as a header field -- this is why a live block costs nothing for it, and
 * why SHM_SLIM_BLOCK_MIN reserves 8 extra bytes beyond the header.
 */
static inline void shm_slim_set_free_link(shm_slim_hdr_t *b, uint64_t link)
{
    memcpy((char *)b + sizeof(*b), &link, sizeof(link));
}

static inline uint64_t shm_slim_get_free_link(const shm_slim_hdr_t *b)
{
    uint64_t link;
    memcpy(&link, (const char *)b + sizeof(*b), sizeof(link));
    return link;
}

#endif /* SHM_SLIM_INTERNAL_H */
