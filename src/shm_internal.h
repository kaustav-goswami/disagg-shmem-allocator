/**
 * @file  shm_internal.h
 * @brief Shared internal types, constants, and inline helpers used by both
 *        shm_alloc.c (the core allocator) and shm_cap.c (the CHERI-like
 *        capability layer).
 *
 * This header is NOT part of the public API.  It exposes the raw in-memory
 * layout of every structure in a shared region so that both translation units
 * can access block headers and region metadata without going through the
 * public shm_alloc() interface.
 *
 * Do not include this from user code; include shm_alloc.h instead.
 */

#ifndef SHM_INTERNAL_H
#define SHM_INTERNAL_H

#define _POSIX_C_SOURCE 200809L   /* robust mutexes, shm_open */

#include "shm_alloc.h"
#include "shm_persist.h"   /* shm_persist(), shm_drain(), shm_flush_line() */

#include <pthread.h>   /* pthread_mutex_t */
#include <stddef.h>    /* size_t, offsetof */
#include <stdint.h>    /* uint8_t … uint64_t */
#include <string.h>    /* memset, memcpy */

/* ── Internal constants ──────────────────────────────────────────────────── */

/** Every heap block and the region header are rounded to this alignment. */
#define HEAP_ALIGN       8u

/** Minimum useful block size: header + one alignment quantum of payload. */
#define BLOCK_MIN        (sizeof(shm_block_hdr_t) + HEAP_ALIGN)

/** Default directory capacity when the caller does not specify one. */
#define DIR_CAP_DEFAULT  256u

/** Maximum characters in a directory-entry name (includes NUL terminator). */
#define DIR_NAME_MAX     48u

/* ── In-memory struct definitions ───────────────────────────────────────── */

/**
 * Block header — lives immediately before every payload in the heap.
 *
 * Security-relevant fields (`owner`, `perms`, `allocated`) are redundant
 * copies of the directory-entry fields so that shm_ptr() and shm_cap_deref()
 * can validate a raw offset without a directory scan.
 */
typedef struct shm_block_hdr {
    uint32_t      magic;        /* SHM_ALLOC_MAGIC_BLOCK — corruption guard   */
    uint32_t      type_tag;     /* caller-defined type id, copied from dir     */
    shm_user_id_t owner;        /* owning user id                              */
    shm_perm_t    perms;        /* permission bits                             */
    uint8_t       allocated;    /* 1 = live; 0 = on free list (tag bit)        */
    uint8_t       _pad[7];      /* keeps obj_id on an 8-byte boundary          */
    uint64_t      obj_id;       /* directory ID; 0 for free blocks             */
    size_t        payload_cap;  /* payload bytes (not counting this header)    */
    size_t        total_size;   /* sizeof(hdr) + payload_cap; heap unit        */
    uint64_t      next_free;    /* 1-based hoff of next free block; 0 = end   */
} shm_block_hdr_t;

/**
 * Region header — lives at base[0].
 * Contains the layout offsets, the process-shared mutex, and the namespace /
 * cgroup fingerprints used for cross-host access control.
 */
typedef struct shm_region_hdr {
    uint32_t        magic;          /* SHM_ALLOC_MAGIC_REGION                 */
    uint32_t        version;        /* SHM_ALLOC_VERSION                      */
    uint32_t        backend;        /* shm_backend_t cast to uint32           */
    uint32_t        dir_capacity;   /* number of directory slots              */
    pthread_mutex_t mutex;          /* process-shared robust mutex            */
    uint32_t        host_count;     /* attached processes                     */
    uint32_t        _pad0;
    size_t          region_size;    /* total mapping size in bytes            */
    size_t          dir_offset;     /* byte offset of object directory        */
    size_t          data_offset;    /* byte offset of heap start              */
    uint64_t        free_list;      /* 1-based hoff → first free heap block   */
    size_t          used_bytes;     /* bytes committed to live blocks         */
    size_t          block_count;    /* live block count                       */
    uint64_t        next_id;        /* next monotonic object ID (starts at 1) */
    uint64_t        ns_inode;       /* IPC namespace inode of creator         */
    uint64_t        cgroup_hash;    /* FNV-1a hash of /proc/self/cgroup       */
    uint64_t        map_base;       /* creator's mmap VA; attach remaps here  */
    char            shm_name[64];   /* the name/path used at creation         */
} shm_region_hdr_t;

/**
 * Object directory entry — one per live allocation.
 * Scanned linearly for lookup by ID or name.  id == 0 means the slot is free.
 */
typedef struct shm_obj_entry {
    uint64_t        id;                  /* monotonic ID; 0 = slot available  */
    char            name[DIR_NAME_MAX];  /* optional name (NUL-padded)        */
    shm_off_t       off;                 /* heap-relative payload offset      */
    size_t          capacity;            /* physical payload capacity         */
    size_t          used_size;           /* logical used size                 */
    uint32_t        type_tag;            /* caller type identifier            */
    shm_user_id_t   owner;               /* owning user id                   */
    shm_perm_t      perms;               /* permission bits                  */
    uint8_t         alive;               /* 1 = live; 0 = freed              */
    uint8_t         _pad[3];
} shm_obj_entry_t;

/**
 * Per-process region handle — allocated on the local C heap.
 * Not visible to other host processes; they each have their own copy.
 */
struct shm_region {
    char          *name;      /* heap copy of the shm name / dax path         */
    int            fd;        /* file descriptor from shm_open or open        */
    void          *base;      /* mmap base pointer                            */
    size_t         size;      /* total mapping length in bytes                */
    bool           creator;   /* true if this process created the region      */
    shm_backend_t  backend;   /* which backend was used                       */
};

/* ── Inline accessor helpers ─────────────────────────────────────────────── */

/** Round n up to the nearest multiple of a (a must be a power of two). */
static inline size_t shm_align_up(size_t n, size_t a)
{
    return (n + a - 1u) & ~(a - 1u);
}

/** Access the region header at the very start of the mapping. */
static inline shm_region_hdr_t *shm_rh(const shm_region_t *r)
{
    return (shm_region_hdr_t *)r->base;
}

/** Pointer to the first byte of the heap (after header + directory). */
static inline char *shm_heap_base(const shm_region_t *r)
{
    return (char *)r->base + shm_rh(r)->data_offset;
}

/** Total heap bytes available. */
static inline size_t shm_heap_size(const shm_region_t *r)
{
    return r->size - shm_rh(r)->data_offset;
}

/** Heap-header-offset (hoff) → block header pointer. */
static inline shm_block_hdr_t *shm_blk_at_hoff(const shm_region_t *r, size_t hoff)
{
    return (shm_block_hdr_t *)(shm_heap_base(r) + hoff);
}

/**
 * Heap-payload-offset (poff) → block header pointer.
 * The block header immediately precedes the payload area.
 */
static inline shm_block_hdr_t *shm_blk_of_poff(const shm_region_t *r, shm_off_t poff)
{
    return (shm_block_hdr_t *)(shm_heap_base(r) + poff - sizeof(shm_block_hdr_t));
}

/** Block header pointer → heap-header-offset. */
static inline size_t shm_hoff_of_blk(const shm_region_t *r, const shm_block_hdr_t *b)
{
    return (size_t)((const char *)b - shm_heap_base(r));
}

/** Block header pointer → heap-payload-offset (what callers store). */
static inline shm_off_t shm_poff_of_blk(const shm_region_t *r, const shm_block_hdr_t *b)
{
    return (shm_off_t)(shm_hoff_of_blk(r, b) + sizeof(shm_block_hdr_t));
}

/** Encode a heap-header-offset as a 1-based free-list link (0 = end of list). */
static inline uint64_t shm_link_enc(size_t hoff) { return (uint64_t)hoff + 1u; }

/** Decode a free-list link back to its heap-header-offset. */
static inline size_t   shm_link_dec(uint64_t lnk) { return (size_t)(lnk - 1u); }

/** Dereference a free-list link; returns NULL when lnk == 0. */
static inline shm_block_hdr_t *shm_blk_of_link(const shm_region_t *r, uint64_t lnk)
{
    return lnk ? shm_blk_at_hoff(r, shm_link_dec(lnk)) : NULL;
}

/** Base pointer of the object directory (right after the region header). */
static inline shm_obj_entry_t *shm_dir_base(const shm_region_t *r)
{
    return (shm_obj_entry_t *)((char *)r->base + shm_rh(r)->dir_offset);
}

/** Return the nth directory slot (0-based). */
static inline shm_obj_entry_t *shm_dir_slot(const shm_region_t *r, uint32_t idx)
{
    return &shm_dir_base(r)[idx];
}

#endif /* SHM_INTERNAL_H */
