/**
 * @file  shm_slim.h
 * @brief v0.2 "slim" shared/disaggregated-memory allocator — minimal
 *        per-object metadata, incompatible with the v2 format in
 *        shm_alloc.h.
 *
 * This is a separate, self-contained module: it shares no types with
 * shm_alloc.h and can be included in the same translation unit without
 * collision. It shares only the generic, format-agnostic helpers
 * (shm_persist.h, shm_ns.h) that v2 also uses.
 *
 * ── Why a second format ──────────────────────────────────────────────────────
 *
 *   The v2 format spends 152 bytes of metadata per object (56-byte block
 *   header + 96-byte directory entry). At the scale this allocator targets
 *   — disaggregated memory holding huge numbers of small objects — that
 *   does not scale: 300M objects representing 2.1 GiB of data would need
 *   ~70 GiB of metadata. A typical malloc spends 8-16 bytes per chunk. This
 *   format targets that ballpark for the common (anonymous object) case.
 *
 * ── What changed ──────────────────────────────────────────────────────────────
 *
 *   - The 56+96-byte block-header + directory-entry pair is replaced by a
 *     single 16-byte block header (magic, key, size). No monotonic object
 *     IDs, no owner/permission bitmask, no type tag, no separate physical-
 *     vs-logical size.
 *   - Access control is a single access-control key: an object either has
 *     no protection (key == SHM_SLIM_KEY_OPEN) or requires the caller to
 *     present the exact key it was allocated with. No owner identity, no
 *     per-operation permission bits, no admin bypass.
 *   - The object directory is gone. A tiny, fixed-capacity, OPTIONAL name
 *     table (sized to how many objects you actually want to name for
 *     discovery, not to the total object count) replaces it. Anonymous
 *     objects — expected to be the overwhelming majority at scale — cost
 *     zero directory bytes.
 *   - Namespace / cgroup enforcement is unchanged from v2 (it is O(1) per
 *     region, not O(n) per object, so it was never part of the problem).
 *   - Bounds-checked offset resolution (reject a stale/corrupted/out-of-
 *     range offset with EFAULT/NULL instead of dereferencing outside the
 *     mapped region) is carried forward unchanged in spirit from v2.
 *
 * ── Incompatibility with v2 ───────────────────────────────────────────────────
 *
 *   The on-disk version field is the literal 4-byte ASCII word "slim"
 *   (SHM_SLIM_VERSION), not an incrementing integer. v2's open path rejects
 *   any region whose version isn't its own integer constant; this format's
 *   open path rejects any region whose version isn't "slim". The two can
 *   never be silently cross-opened.
 *
 * ── Memory layout ────────────────────────────────────────────────────────────
 *
 *   base[0]              base + name_offset       base + data_offset
 *     |                        |                         |
 *     v                        v                         v
 *   +------------------+-------------------------+------------------------+
 *   | region header    | name table (optional,    | heap (16-byte-header   |
 *   |                  | fixed small capacity)    | blocks, implicit list) |
 *   +------------------+-------------------------+------------------------+
 *
 * See WIP.md for the full design rationale and the list of what was
 * deliberately dropped (monotonic IDs, owner/perms, type tags, the CHERI-
 * like capability layer, C++ typed wrappers) and why.
 */

#ifndef SHM_SLIM_H
#define SHM_SLIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Magic / version ─────────────────────────────────────────────────────── */

/** Region-family identifier; shared with v2 ("this is an shm_alloc region"). */
#define SHM_SLIM_MAGIC_REGION  0x53484D41u  /* "SHMA" */

/**
 * On-disk format version for this module: the literal ASCII bytes "slim",
 * not an incrementing integer. This is what actually makes v0.2 and v2
 * regions mutually unreadable -- v2 checks for its own integer constant,
 * this module checks for this word, and the two encodings cannot collide.
 */
#define SHM_SLIM_VERSION       0x736C696Du  /* "slim" */

/** Stored in every live/free heap block header. */
#define SHM_SLIM_MAGIC_LIVE        0x534C4C41u  /* "SLLA": SLim Live Anonymous */
#define SHM_SLIM_MAGIC_LIVE_NAMED  0x534C4C4Eu  /* "SLLN": SLim Live Named    */
#define SHM_SLIM_MAGIC_FREE        0x534C4652u  /* "SLFR": SLim FRee          */

/* ── Heap-relative payload offset ───────────────────────────────────────── */

typedef uint64_t shm_off_t;
#define SHM_OFF_NULL UINT64_MAX

/* ── Access-control key ──────────────────────────────────────────────────── */

/**
 * A single opaque access-control key, replacing v2's owner-id + permission
 * bitmask. SHM_SLIM_KEY_OPEN means "no protection": any caller, with any
 * key value, may access the object. Any other value must be matched
 * exactly by every subsequent shm_slim_ptr/free/resize/lookup_by_name call.
 */
typedef uint32_t shm_slim_key_t;
#define SHM_SLIM_KEY_OPEN  ((shm_slim_key_t)0)

/* ── Name table ──────────────────────────────────────────────────────────── */

/** Maximum name length including the NUL terminator. */
#define SHM_SLIM_NAME_MAX  24u

/** Default name-table capacity when the caller does not specify one. */
#define SHM_SLIM_NAME_CAP_DEFAULT  64u

/* ── Backend ─────────────────────────────────────────────────────────────── */

typedef enum shm_slim_backend {
    SHM_SLIM_BACKEND_POSIX   = 0,  /* POSIX shm_open IPC shared memory       */
    SHM_SLIM_BACKEND_DAX     = 1,  /* /dev/daxX.Y disaggregated / CXL memory */
    /**
     * Source the region's backing memory from a vendored allocator library
     * (jemalloc / tcmalloc / cxlalloc / ralloc) selected at compile time --
     * see src/shm_generic_backend.h and the "Generic allocator backend"
     * section of README.md. name_or_path is used only as a debug label;
     * `size` must be >= 4096; SHM_SLIM_OPEN_CREATE must be set (there is no
     * OS-level named object for a second process to attach to, so opening
     * without create returns ENOTSUP).
     */
    SHM_SLIM_BACKEND_GENERIC = 2,
} shm_slim_backend_t;

/* ── Open flags ──────────────────────────────────────────────────────────── */

#define SHM_SLIM_OPEN_CREATE          0x01u
#define SHM_SLIM_OPEN_ENFORCE_NS      0x02u
#define SHM_SLIM_OPEN_ENFORCE_CGROUP  0x04u

/* ── Open options ────────────────────────────────────────────────────────── */

typedef struct shm_slim_region_open_opts {
    shm_slim_backend_t backend;         /* which storage backend (default POSIX) */
    uint32_t            flags;          /* bitwise OR of SHM_SLIM_OPEN_* */
    uint32_t            name_capacity;  /* max named objects (0 -> default 64) */
    mode_t               mode;          /* file permission bits (0 -> 0660) */
} shm_slim_region_open_opts_t;

/* ── Opaque region handle ────────────────────────────────────────────────── */

typedef struct shm_slim_region shm_slim_region_t;

/* ── Directory-free enumeration entry ───────────────────────────────────── */

/**
 * A snapshot of one live object, returned by shm_slim_walk(). Enumeration
 * walks the heap's implicit block list directly (there is no separate
 * directory array to scan) so this is O(number of live+free blocks), same
 * asymptotic cost as v2's directory scan, at zero storage cost.
 */
typedef struct shm_slim_entry {
    shm_off_t off;                     /* heap-relative payload offset */
    size_t    size;                    /* payload capacity in bytes */
    char      name[SHM_SLIM_NAME_MAX]; /* empty string if the object is anonymous */
} shm_slim_entry_t;

/* ── Pool statistics ─────────────────────────────────────────────────────── */

typedef struct shm_slim_pool_stats {
    size_t   total_bytes;     /* total heap bytes (region minus headers) */
    size_t   used_bytes;      /* bytes committed to live blocks */
    size_t   free_bytes;      /* bytes currently on the free list */
    size_t   block_count;     /* number of live (allocated) heap blocks */
    uint32_t host_count;      /* number of processes currently attached */
    uint32_t name_capacity;   /* maximum entries in the name table */
    uint32_t name_used;       /* number of live entries in the name table */
} shm_slim_pool_stats_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Region lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Open or create a slim shared-memory / DAX region.
 *
 * @param name_or_path  POSIX name (must start with '/') or DAX device path.
 * @param size           Desired mapping size in bytes (>= 4096). Ignored for
 *                        non-creating opens and for DAX.
 * @param opts           Options; pass NULL for all defaults.
 * @param out_region     Receives the new handle on success.
 * @return               0 on success, errno-compatible code on failure.
 *                        ENOENT if an existing region's magic/version does
 *                        not match this format (including any v2 region).
 */
int shm_slim_region_open(const char                        *name_or_path,
                          size_t                             size,
                          const shm_slim_region_open_opts_t *opts,
                          shm_slim_region_t                **out_region);

/** Close and optionally unlink a region. NULL region is a safe no-op. */
void shm_slim_region_close(shm_slim_region_t *region, bool unlink_name);

/** Return the base virtual address of the mapping (for debugging). */
void  *shm_slim_region_base(const shm_slim_region_t *region);

/** Return the total mapping size in bytes. */
size_t shm_slim_region_size(const shm_slim_region_t *region);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Allocation
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Allocate @p payload_size bytes.
 *
 * @param region        Region handle.
 * @param payload_size  Bytes to allocate (> 0).
 * @param name          Optional name for discovery (NULL or "" for
 *                      anonymous). Anonymous objects cost 16 bytes of
 *                      metadata total; named objects additionally consume
 *                      one name-table slot. Truncated at SHM_SLIM_NAME_MAX-1
 *                      chars; EEXIST if the name is already in use.
 * @param key           Access-control key to seal onto the object;
 *                      SHM_SLIM_KEY_OPEN for no protection.
 * @param out_off       Receives the heap-relative payload offset -- this is
 *                      the handle used by every other function.
 * @return              0, EINVAL, ENOMEM (heap exhausted), ENOSPC (name
 *                      table full), EEXIST (name already used), or
 *                      ENAMETOOLONG.
 */
int shm_slim_alloc(shm_slim_region_t *region,
                    size_t             payload_size,
                    const char        *name,
                    shm_slim_key_t     key,
                    shm_off_t         *out_off);

/**
 * @brief Free an object.
 *
 * @param region  Region handle.
 * @param off     Offset returned by shm_slim_alloc() / shm_slim_resize().
 * @param key     Must match the object's stored key (or the object must be
 *                SHM_SLIM_KEY_OPEN).
 * @return        0, EINVAL (not a live block), EACCES (key mismatch), or
 *                EFAULT (offset does not resolve to a valid block).
 */
int shm_slim_free(shm_slim_region_t *region,
                   shm_off_t          off,
                   shm_slim_key_t     key);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Resize (grow / shrink)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Change the capacity of an object; may relocate it.
 *
 * Unlike v2 (which required re-calling shm_lookup() after a relocating
 * grow, because the directory entry -- not the caller -- tracked the
 * current offset), this format has no per-object directory entry for
 * anonymous objects, so the (possibly new) offset is always returned
 * directly.
 *
 * @param region    Region handle.
 * @param off       Current offset.
 * @param new_size  New capacity in bytes (> 0).
 * @param key       Must match the object's stored key.
 * @param out_off   Receives the offset to use from now on (unchanged unless
 *                  a relocating grow occurred).
 * @return          0, EINVAL, EACCES, EFAULT, or ENOMEM (heap exhausted on grow).
 */
int shm_slim_resize(shm_slim_region_t *region,
                     shm_off_t          off,
                     size_t             new_size,
                     shm_slim_key_t     key,
                     shm_off_t         *out_off);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Direct pointer access
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Resolve an offset to a local virtual pointer.
 *
 * Validates that off resolves to a live block fully inside the mapped heap
 * and that key matches (or the object is unprotected). There is no
 * separate read/write distinction: a matching key grants full access, same
 * as a bearer token.
 *
 * @return Non-NULL payload pointer on success; NULL on any check failure.
 */
void *shm_slim_ptr(const shm_slim_region_t *region,
                    shm_off_t                off,
                    shm_slim_key_t           key);

/**
 * @brief Read a live block's payload capacity without exposing a pointer.
 * @return 0 on success; EINVAL if off is not a valid live block; EACCES on
 *         key mismatch; EFAULT on an out-of-range offset.
 */
int shm_slim_block_size(const shm_slim_region_t *region,
                         shm_off_t                off,
                         shm_slim_key_t           key,
                         size_t                  *out_size);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Discovery
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Look up a named object.
 * @return 0 with *out_off filled; ENOENT if no such name; EACCES if key
 *         does not match the object's stored key.
 */
int shm_slim_lookup_by_name(const shm_slim_region_t *region,
                             const char              *name,
                             shm_slim_key_t           key,
                             shm_off_t               *out_off);

/**
 * @brief Iterate every live object by walking the heap's implicit block
 *        list (there is no separate directory to scan).
 *
 * Typical usage:
 * @code
 *   uint64_t cursor = 0;
 *   shm_slim_entry_t e;
 *   while (shm_slim_walk(region, &cursor, &e) == 0) { ... }
 * @endcode
 *
 * This does not check the access-control key -- it reports which objects
 * exist, not their contents; use shm_slim_ptr() with the right key to
 * actually read one.
 *
 * @param cursor  Byte offset into the heap; must be 0 on the first call.
 * @return        0 with *out filled; ENOENT when iteration is exhausted;
 *                EIO if the heap's implicit block chain is corrupted
 *                (a block's size would run past the end of the heap).
 */
int shm_slim_walk(const shm_slim_region_t *region,
                   uint64_t                *cursor,
                   shm_slim_entry_t        *out);

/**
 * @brief Return aggregate pool statistics.
 * @return 0 on success; EINVAL if region or out is NULL.
 */
int shm_slim_pool_stats(const shm_slim_region_t *region, shm_slim_pool_stats_t *out);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Typed C convenience macros (compile-time only -- no runtime type tag
 *  is stored, unlike v2's type_tag + dynamic_cast-style capability layer)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define shm_slim_alloc_typed(region, type, name, key, out_off) \
    shm_slim_alloc((region), sizeof(type), (name), (key), (out_off))

#define shm_slim_cast(region, off, type, key) \
    ((const type *)shm_slim_ptr((region), (off), (key)))

#define shm_slim_cast_mut(region, off, type, key) \
    ((type *)shm_slim_ptr((region), (off), (key)))

/**
 * Define SHM_SLIM_NO_V2_COMPAT before including this header to skip the
 * v2-compatible API section below entirely. Only needed by code that
 * includes BOTH shm_alloc.h and shm_slim.h in the same translation unit
 * (e.g. a test exercising both engines side by side) -- shm_alloc.h already
 * declares shm_region_t/shm_alloc()/shm_ptr()/etc. with the exact same
 * names, and two conflicting declarations of the same name is a compile
 * error, not a silent problem. Ordinary code that includes only
 * shm_slim.h -- the common case this compatibility layer exists for --
 * never needs to touch this macro.
 */
#ifndef SHM_SLIM_NO_V2_COMPAT

/* ═══════════════════════════════════════════════════════════════════════════
 *  v2-compatible API (bodies in src/shm_slim_compat.c, NOT src/shm_slim.c)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Everything below mirrors include/shm_alloc.h's public names and signatures
 * exactly, so source written against the v2 API compiles unchanged against
 * this header instead: swap `#include "shm_alloc.h"` for
 * `#include "shm_slim.h"`, link shm_slim.o + shm_slim_compat.o + shm_ns.o
 * instead of shm_alloc.o + shm_ns.o, and it runs on the slim engine.
 *
 * This is a *compatibility* layer over the native slim API above, not a
 * second implementation of v2's full feature set -- see WIP.md for the
 * full rationale. In short:
 *
 *   - user_id and perms collapse into the slim engine's single access key
 *     (key := user_id); perms is accepted for signature compatibility but
 *     not enforced bit-by-bit -- see shm_slim_key_t in the native API above.
 *   - type_tag is accepted but not persisted (the whole point of the 16-byte
 *     header is not carrying a type_tag on every object).
 *   - obj_id is synthesized as the offset itself (id == off), since the
 *     slim engine assigns no monotonic IDs. shm_free()/shm_resize() below
 *     therefore expect the value shm_alloc() wrote into *out_id, which
 *     happens to equal the *out_off value from the same call.
 *   - There is NO compatibility shim for shm_lookup(id) / shm_dir_next() /
 *     shm_dir_read_slot() / shm_block_info() / shm_block_set_perms().
 *     Replicating those faithfully would require storing a per-object
 *     id/type_tag/capacity/used_size directory again -- exactly the
 *     per-object cost this format exists to eliminate. Use the native
 *     shm_slim_walk() / shm_slim_lookup_by_name() / shm_slim_block_size()
 *     for discovery/introspection, or stay on v2 if you need those.
 *   - shm_resize() only supports a shrink or a grow that fits in place;
 *     a grow requiring relocation returns ENOTSUP, because there is no
 *     id-to-offset directory here to keep it transparent the way v2's
 *     shm_resize() does (v2 callers already have to re-`shm_lookup()`
 *     after a relocating grow -- this compat layer has no equivalent to
 *     re-look-up *through*, so it reports the failure instead of silently
 *     returning a now-stale offset).
 */

typedef shm_slim_region_t shm_region_t;

typedef uint32_t shm_user_id_t;
typedef uint32_t shm_perm_t;

#define SHM_PERM_NONE    0x00u
#define SHM_PERM_READ    0x01u
#define SHM_PERM_WRITE   0x02u
#define SHM_PERM_FREE    0x04u
#define SHM_PERM_RESIZE  0x08u
#define SHM_PERM_ADMIN   0x80u
#define SHM_PERM_DEFAULT \
    (SHM_PERM_READ | SHM_PERM_WRITE | SHM_PERM_FREE | SHM_PERM_RESIZE)

typedef enum shm_backend {
    SHM_BACKEND_POSIX   = 0,
    SHM_BACKEND_DAX     = 1,
    SHM_BACKEND_GENERIC = 2,
} shm_backend_t;

#define SHM_OPEN_CREATE          0x01u
#define SHM_OPEN_ENFORCE_NS      0x02u
#define SHM_OPEN_ENFORCE_CGROUP  0x04u

typedef struct shm_region_open_opts {
    shm_backend_t backend;
    uint32_t      flags;
    /**
     * Accepted for signature compatibility only. The slim engine has no
     * directory sized to the object count, so this is ignored; the name
     * table (for named objects only) keeps using SHM_SLIM_NAME_CAP_DEFAULT.
     * Use shm_slim_region_open_opts_t.name_capacity via the native API if
     * you need to size it explicitly.
     */
    uint32_t      dir_capacity;
    mode_t        mode;
} shm_region_open_opts_t;

typedef struct shm_pool_stats {
    size_t   total_bytes;
    size_t   used_bytes;
    size_t   free_bytes;
    size_t   block_count;
    uint64_t next_id;       /* always 0: the slim engine assigns no IDs */
    uint32_t host_count;
    uint32_t dir_capacity;  /* mirrors the slim engine's name-table capacity */
    uint32_t dir_used;      /* mirrors the slim engine's name-table usage */
} shm_pool_stats_t;

int shm_region_open(const char             *name_or_path,
                    size_t                  size,
                    const shm_region_open_opts_t *opts,
                    shm_region_t          **out_region);

void   shm_region_close(shm_region_t *region, bool unlink_name);
void  *shm_region_base(const shm_region_t *region);
size_t shm_region_size(const shm_region_t *region);

int shm_alloc(shm_region_t  *region,
              size_t         payload_size,
              const char    *name,
              shm_user_id_t  user_id,
              shm_perm_t     perms,
              uint32_t       type_tag,
              uint64_t      *out_id,
              shm_off_t     *out_off);

int shm_free(shm_region_t *region,
             uint64_t      obj_id,
             shm_user_id_t user_id,
             shm_perm_t    caller_perms);

int shm_resize(shm_region_t *region,
               uint64_t      obj_id,
               size_t        new_size,
               shm_user_id_t user_id,
               shm_perm_t    caller_perms);

void *shm_ptr(const shm_region_t *region,
              shm_off_t           off,
              shm_user_id_t       user_id,
              shm_perm_t          caller_perms,
              shm_perm_t          required_perms);

int shm_pool_stats(const shm_region_t *region, shm_pool_stats_t *out);

#define shm_alloc_typed(region, type, name, user_id, perms, out_id, out_off) \
    shm_alloc((region), sizeof(type), (name), (user_id), (perms),            \
              (uint32_t)sizeof(type), (out_id), (out_off))

#define shm_cast(region, off, type, user_id, caller_perms)                   \
    ((const type *)shm_ptr((region), (off), (user_id), (caller_perms),       \
                           SHM_PERM_READ))

#define shm_cast_mut(region, off, type, user_id, caller_perms)               \
    ((type *)shm_ptr((region), (off), (user_id), (caller_perms),             \
                     SHM_PERM_READ | SHM_PERM_WRITE))

#define shm_free_id(region, obj_id, user_id, caller_perms)                   \
    shm_free((region), (obj_id), (user_id), (caller_perms))

#endif /* !SHM_SLIM_NO_V2_COMPAT */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SHM_SLIM_H */
