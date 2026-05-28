/**
 * @file  shm_alloc.h
 * @brief Shared / disaggregated-memory allocator with typed offset handles,
 *        per-object user-id ownership, permission bits, and an object directory
 *        at the head of every region for multi-host discovery.
 *
 * ── Memory layout ────────────────────────────────────────────────────────────
 *
 *   base[0]            base + dir_offset       base + data_offset
 *     │                     │                       │
 *     ▼                     ▼                       ▼
 *   ┌─────────────────┬─────────────────────────┬──────────────────────────┐
 *   │  region header  │   object directory      │   heap (blocks …)        │
 *   └─────────────────┴─────────────────────────┴──────────────────────────┘
 *
 *   The region header records dir_offset and data_offset so the layout is
 *   self-describing; a new host process only needs to open the same name/device
 *   and read those two fields to find everything else.
 *
 * ── Backends ─────────────────────────────────────────────────────────────────
 *
 *   SHM_BACKEND_POSIX — shm_open("/name") + mmap(MAP_SHARED).
 *                        Standard POSIX IPC, any Linux host.
 *   SHM_BACKEND_DAX   — open("/dev/daxX.Y") + mmap(MAP_SHARED).
 *                        Disaggregated / persistent memory (CXL, PMEM, etc.).
 *                        ftruncate is skipped; region size is fixed by the device.
 *
 * ── Object directory & multi-host discovery ──────────────────────────────────
 *
 *   Every shm_alloc() assigns a monotonically increasing uint64_t ID and
 *   registers the object in the directory (name, offset, capacity, owner …).
 *   A new host calls shm_region_open() with SHM_OPEN_CREATE unset, then uses
 *   shm_lookup(id) or shm_dir_next() to iterate and find existing objects
 *   without relying on hardcoded offsets.
 *
 * ── Dynamic resize ────────────────────────────────────────────────────────────
 *
 *   shm_resize() grows or shrinks an object.  Growth first tries to expand
 *   into the adjacent free block; if that is not possible the object is moved
 *   to a new location and the directory entry is updated.  Callers must
 *   re-call shm_lookup() after a resize that required relocation (the payload
 *   offset in the directory will have changed).
 *
 * ── Namespace / cgroup protection ────────────────────────────────────────────
 *
 *   The creator's IPC-namespace inode and a hash of /proc/self/cgroup are
 *   stored in the region header.  Pass SHM_OPEN_ENFORCE_NS or
 *   SHM_OPEN_ENFORCE_CGROUP to shm_region_open_opts_t to reject processes
 *   that are in a different Linux namespace or cgroup.
 *
 * ── Pointer safety ────────────────────────────────────────────────────────────
 *
 *   All cross-process handles are shm_off_t heap-relative byte offsets.
 *   shm_ptr() converts an offset to a local virtual address after checking
 *   magic, ownership, and permissions.  Never store raw pointers in shared
 *   memory — the mapping base differs between processes.
 */

#ifndef SHM_ALLOC_H
#define SHM_ALLOC_H

#include <stdbool.h>    /* bool, true, false */
#include <stddef.h>     /* size_t */
#include <stdint.h>     /* uint32_t, uint64_t */
#include <sys/types.h>  /* mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Magic identifiers ───────────────────────────────────────────────────── */

/** Stored at byte 0 of the region to guard against corruption / wrong mapping. */
#define SHM_ALLOC_MAGIC_REGION  0x53484D41u  /* "SHMA" in ASCII */
/** Stored at the start of every heap block header. */
#define SHM_ALLOC_MAGIC_BLOCK   0x424C4B21u  /* "BLK!" in ASCII */
/** Incremented every time the on-disk layout changes in a breaking way. */
#define SHM_ALLOC_VERSION       2u

/* ── Heap-relative payload offset ───────────────────────────────────────── */

/**
 * An offset from heap_base (= region_base + data_offset) to the first byte
 * of an object's payload.  Value 0 is a valid offset (first block in the
 * heap).  The null / invalid sentinel is UINT64_MAX.
 */
typedef uint64_t shm_off_t;

/** Sentinel value meaning "no offset / invalid". */
#define SHM_OFF_NULL UINT64_MAX

/* ── Permission bitmask ──────────────────────────────────────────────────── */

/** Type for permission bitmasks attached to every object and to each call. */
typedef uint32_t shm_perm_t;

#define SHM_PERM_NONE    0x00u  /* no permissions */
#define SHM_PERM_READ    0x01u  /* may call shm_ptr() requesting read access */
#define SHM_PERM_WRITE   0x02u  /* may call shm_ptr() requesting write access */
#define SHM_PERM_FREE    0x04u  /* may call shm_free() on the object */
#define SHM_PERM_RESIZE  0x08u  /* may call shm_resize() on the object */
#define SHM_PERM_ADMIN   0x80u  /* bypass owner-id check for all operations */

/** Convenience: all non-admin permissions. */
#define SHM_PERM_DEFAULT \
    (SHM_PERM_READ | SHM_PERM_WRITE | SHM_PERM_FREE | SHM_PERM_RESIZE)

/* ── User / owner identity ───────────────────────────────────────────────── */

/** Numeric user id embedded in every object; callers supply their own scheme. */
typedef uint32_t shm_user_id_t;

/* ── Backends ────────────────────────────────────────────────────────────── */

/** Selects the underlying memory technology used to back the region. */
typedef enum shm_backend {
    SHM_BACKEND_POSIX = 0,  /* POSIX shm_open IPC shared memory        */
    SHM_BACKEND_DAX   = 1,  /* /dev/daxX.Y disaggregated / CXL memory  */
} shm_backend_t;

/* ── Open flags ──────────────────────────────────────────────────────────── */

/** Create a brand-new region; truncates any existing region of the same name. */
#define SHM_OPEN_CREATE          0x01u
/** Reject the open if the caller's IPC namespace inode differs from creator's. */
#define SHM_OPEN_ENFORCE_NS      0x02u
/** Reject the open if the caller's cgroup hash differs from the creator's. */
#define SHM_OPEN_ENFORCE_CGROUP  0x04u

/* ── Open options ────────────────────────────────────────────────────────── */

/**
 * Passed to shm_region_open().  Zero-initialise and fill only the fields you
 * care about; defaults are applied for any field left as zero.
 */
typedef struct shm_region_open_opts {
    shm_backend_t backend;       /* which storage backend to use (default POSIX) */
    uint32_t      flags;         /* bitwise OR of SHM_OPEN_* constants */
    uint32_t      dir_capacity;  /* max objects in directory (0 → default 256) */
    mode_t        mode;          /* file permission bits (0 → 0660) */
} shm_region_open_opts_t;

/* ── Opaque region handle ────────────────────────────────────────────────── */

/** Per-process handle returned by shm_region_open().  Do not copy. */
typedef struct shm_region shm_region_t;

/* ── Object directory public entry ──────────────────────────────────────── */

/**
 * A snapshot of one live directory entry, returned by shm_dir_next().
 * Used by host processes to discover what objects exist in the region.
 */
typedef struct shm_dir_entry {
    uint64_t        id;         /* monotonic ID assigned at alloc time; never reused */
    char            name[48];   /* optional human-readable name (empty string if unused) */
    shm_off_t       off;        /* heap-relative byte offset to the start of payload */
    size_t          capacity;   /* physical payload bytes actually allocated */
    size_t          used_size;  /* logical bytes in use, as last set by the owner */
    uint32_t        type_tag;   /* caller-defined type identifier */
    shm_user_id_t   owner;      /* user id of the allocating process */
    shm_perm_t      perms;      /* permission bits on this object */
} shm_dir_entry_t;

/* ── Pool statistics ─────────────────────────────────────────────────────── */

/** Aggregate statistics about the allocator pool. */
typedef struct shm_pool_stats {
    size_t   total_bytes;    /* total heap bytes (region minus headers) */
    size_t   used_bytes;     /* bytes committed to live blocks */
    size_t   free_bytes;     /* bytes currently on the free list */
    size_t   block_count;    /* number of live (allocated) heap blocks */
    uint64_t next_id;        /* monotonic ID that will be assigned to the next alloc */
    uint32_t host_count;     /* number of processes currently attached */
    uint32_t dir_capacity;   /* maximum entries in the object directory */
    uint32_t dir_used;       /* number of live entries in the object directory */
} shm_pool_stats_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Region lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Open or create a shared-memory / DAX region.
 *
 * For the POSIX backend the name must start with '/' (e.g. "/mypool").
 * For the DAX backend supply a device path (e.g. "/dev/dax0.0").
 *
 * When SHM_OPEN_CREATE is set in opts->flags the region is (re-)initialised
 * and the object directory is cleared.  When the flag is absent the existing
 * region is opened: the header is validated and, if enforcement flags are set,
 * namespace / cgroup fingerprints are checked.
 *
 * @param name_or_path  Name for POSIX or device path for DAX.
 * @param size          Desired mapping size (bytes, must be ≥ 4096).
 *                      Ignored for non-creating opens (size is read from header).
 *                      Ignored for DAX (device size is used).
 * @param opts          Options; pass NULL for all defaults.
 * @param out_region    Receives the newly allocated handle on success.
 * @return              0 on success, errno-compatible code on failure.
 */
int shm_region_open(const char                   *name_or_path,
                    size_t                        size,
                    const shm_region_open_opts_t *opts,
                    shm_region_t                **out_region);

/**
 * @brief Close and optionally unlink a region.
 *
 * Decrements the attached host count, unmaps the memory, and closes the
 * file descriptor.  If unlink_name is true and the backend is POSIX,
 * shm_unlink() is called to remove the name from the namespace.
 * (DAX device nodes cannot be unlinked; the flag is ignored for DAX.)
 *
 * @param region       Handle to close (NULL is a safe no-op).
 * @param unlink_name  If true, remove the POSIX shm name.
 */
void shm_region_close(shm_region_t *region, bool unlink_name);

/** Return the base virtual address of the mapping (for debugging). */
void  *shm_region_base(const shm_region_t *region);

/** Return the total mapping size in bytes. */
size_t shm_region_size(const shm_region_t *region);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Allocation
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Allocate @p payload_size bytes in the heap and register the object
 *        in the directory with a new monotonic ID.
 *
 * On return, other processes that have the same region open can discover the
 * object by calling shm_lookup(*out_id) or shm_dir_next().
 *
 * @param region        Region handle.
 * @param payload_size  Bytes to allocate (> 0).
 * @param name          Optional human-readable name stored in the directory
 *                      (NULL or "" for anonymous).  Truncated to 47 chars.
 * @param user_id       Owner user id stored in both the block and directory.
 * @param perms         Permission bits granted on this object.
 * @param type_tag      Caller-defined type identifier; 0 if unused.
 * @param out_id        Receives the monotonic object ID on success.
 * @param out_off       Receives the heap-relative payload offset on success.
 * @return              0, EINVAL, ENOMEM, or ENOSPC (dir full).
 */
int shm_alloc(shm_region_t  *region,
              size_t         payload_size,
              const char    *name,
              shm_user_id_t  user_id,
              shm_perm_t     perms,
              uint32_t       type_tag,
              uint64_t      *out_id,
              shm_off_t     *out_off);

/**
 * @brief Free an object by its monotonic ID.
 *
 * Marks the directory entry as dead and returns the heap block to the free
 * list (coalescing with adjacent free blocks).
 *
 * The caller must be the owner of the object, or must supply SHM_PERM_ADMIN
 * in caller_perms to bypass the ownership check.
 *
 * @param region        Region handle.
 * @param obj_id        Monotonic ID returned by shm_alloc().
 * @param user_id       Caller's user id.
 * @param caller_perms  Must contain SHM_PERM_FREE (or SHM_PERM_ADMIN).
 * @return              0, ENOENT (id not found), EACCES (permission denied).
 */
int shm_free(shm_region_t *region,
             uint64_t      obj_id,
             shm_user_id_t user_id,
             shm_perm_t    caller_perms);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Resize (grow / shrink)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Change the physical capacity and logical used_size of an object.
 *
 * Shrink (new_size ≤ current capacity):
 *   Trims the heap block, returning any freed capacity to the pool.
 *
 * Grow in-place (new_size > capacity, adjacent block is free with enough space):
 *   Expands the block into the next free block; the payload offset is unchanged.
 *
 * Grow with relocation (in-place not possible):
 *   Allocates a new heap block, copies the existing payload, frees the old
 *   block, and updates the directory entry.  The payload offset in the directory
 *   WILL CHANGE.  Callers must re-call shm_lookup() to obtain the new offset.
 *
 * @param region        Region handle.
 * @param obj_id        Object to resize.
 * @param new_size      New logical (and physical) size in bytes.
 * @param user_id       Caller's user id.
 * @param caller_perms  Must contain SHM_PERM_RESIZE (or SHM_PERM_ADMIN).
 * @return              0, ENOENT, EACCES, or ENOMEM (heap exhausted on grow).
 */
int shm_resize(shm_region_t *region,
               uint64_t      obj_id,
               size_t        new_size,
               shm_user_id_t user_id,
               shm_perm_t    caller_perms);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Lookup and discovery
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Look up an object by its monotonic ID.
 *
 * Intended for host processes that receive an ID through an out-of-band
 * channel (e.g. a message, a config file, or the initial sync variable).
 *
 * @param region         Region handle.
 * @param obj_id         Monotonic ID to look up.
 * @param user_id        Caller's user id.
 * @param caller_perms   Must contain SHM_PERM_READ (or SHM_PERM_ADMIN).
 * @param out_off        If non-NULL, receives the heap-relative payload offset.
 * @param out_used_size  If non-NULL, receives the current logical used_size.
 * @return               0, ENOENT (id not in directory), EACCES.
 */
int shm_lookup(const shm_region_t *region,
               uint64_t            obj_id,
               shm_user_id_t       user_id,
               shm_perm_t          caller_perms,
               shm_off_t          *out_off,
               size_t             *out_used_size);

/**
 * @brief Look up an object by its human-readable name (case-sensitive).
 *
 * Scans the directory for the first live entry whose name equals @p name.
 * Returns ENOENT if no match is found.
 *
 * @param region       Region handle.
 * @param name         Exact name string.
 * @param user_id      Caller's user id.
 * @param caller_perms Must contain SHM_PERM_READ (or SHM_PERM_ADMIN).
 * @param out_id       If non-NULL, receives the monotonic ID.
 * @param out_off      If non-NULL, receives the heap-relative payload offset.
 * @return             0, ENOENT, EACCES.
 */
int shm_lookup_by_name(const shm_region_t *region,
                       const char         *name,
                       shm_user_id_t       user_id,
                       shm_perm_t          caller_perms,
                       uint64_t           *out_id,
                       shm_off_t          *out_off);

/**
 * @brief Iterate all live objects in the directory.
 *
 * Typical usage:
 * @code
 *   uint32_t cursor = 0;
 *   shm_dir_entry_t e;
 *   while (shm_dir_next(region, &cursor, &e) == 0) {
 *       printf("id=%llu name=%s\n", e.id, e.name);
 *   }
 * @endcode
 *
 * @param region  Region handle.
 * @param cursor  Iteration state; must be initialised to 0 before the first call.
 *                Updated to point past the returned entry each call.
 * @param out     Receives a copy of the directory entry (not permission-checked;
 *                the caller still needs shm_ptr() for actual data access).
 * @return        0 with *out filled; ENOENT when iteration is exhausted.
 */
int shm_dir_next(const shm_region_t *region,
                 uint32_t           *cursor,
                 shm_dir_entry_t    *out);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Direct pointer access
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Resolve a heap-relative offset to a local virtual pointer.
 *
 * Validates the block magic, checks ownership (unless ADMIN), and verifies
 * that the block's stored permission bits include all bits in required_perms.
 *
 * @param region          Region handle.
 * @param off             Heap-relative payload offset.
 * @param user_id         Caller's user id.
 * @param caller_perms    Caller's own permission bits.
 * @param required_perms  Bits that must be set on the block (e.g. SHM_PERM_READ).
 * @return                Non-NULL pointer on success; NULL on any check failure.
 */
void *shm_ptr(const shm_region_t *region,
              shm_off_t           off,
              shm_user_id_t       user_id,
              shm_perm_t          caller_perms,
              shm_perm_t          required_perms);

/**
 * @brief Read low-level block metadata directly from a heap offset.
 *
 * All out-parameters are optional (pass NULL to skip).
 *
 * @return 0 on success; EINVAL if the block magic is bad or off is out of range.
 */
int shm_block_info(const shm_region_t *region,
                   shm_off_t           off,
                   size_t             *out_payload_cap,
                   shm_user_id_t      *out_owner,
                   shm_perm_t         *out_perms,
                   uint32_t           *out_type_tag,
                   uint64_t           *out_obj_id);

/**
 * @brief Update the permission bits on an object (owner or ADMIN only).
 *
 * Changes take effect atomically under the region mutex.
 * Both the heap block header and the directory entry are updated.
 */
int shm_block_set_perms(shm_region_t *region,
                        shm_off_t     off,
                        shm_user_id_t user_id,
                        shm_perm_t    caller_perms,
                        shm_perm_t    new_perms);

/**
 * @brief Return aggregate pool statistics (does not require a lock).
 *
 * @return 0 on success; EINVAL if region or out is NULL.
 */
int shm_pool_stats(const shm_region_t *region, shm_pool_stats_t *out);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Typed C convenience macros
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Default type_tag derived from sizeof(type).  Serves as a rough
 * discriminator; for protocol-level type safety supply an explicit tag.
 */
#define SHM_TYPE_TAG_OF(type)  ((uint32_t)sizeof(type))

/**
 * Allocate sizeof(type) bytes and register in the directory.
 * Sets type_tag to SHM_TYPE_TAG_OF(type).
 *
 * @param region   shm_region_t *
 * @param type     C type name (e.g. MyStruct)
 * @param name     const char * directory name (or NULL)
 * @param user_id  shm_user_id_t owner
 * @param perms    shm_perm_t
 * @param out_id   uint64_t * receives monotonic ID
 * @param out_off  shm_off_t * receives payload offset
 */
#define shm_alloc_typed(region, type, name, user_id, perms, out_id, out_off) \
    shm_alloc((region), sizeof(type), (name), (user_id), (perms),            \
              SHM_TYPE_TAG_OF(type), (out_id), (out_off))

/**
 * Resolve a heap offset to a const typed pointer (checks SHM_PERM_READ).
 *
 * @param region        shm_region_t *
 * @param off           shm_off_t payload offset
 * @param type          C type name
 * @param user_id       shm_user_id_t caller
 * @param caller_perms  shm_perm_t caller permissions
 */
#define shm_cast(region, off, type, user_id, caller_perms)                   \
    ((const type *)shm_ptr((region), (off), (user_id), (caller_perms),       \
                           SHM_PERM_READ))

/**
 * Resolve a heap offset to a mutable typed pointer (checks READ|WRITE).
 */
#define shm_cast_mut(region, off, type, user_id, caller_perms)               \
    ((type *)shm_ptr((region), (off), (user_id), (caller_perms),             \
                     SHM_PERM_READ | SHM_PERM_WRITE))

/**
 * Free an object by ID; thin wrapper for readability.
 */
#define shm_free_id(region, obj_id, user_id, caller_perms)                   \
    shm_free((region), (obj_id), (user_id), (caller_perms))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SHM_ALLOC_H */
