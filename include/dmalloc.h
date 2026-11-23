/**
 * @file  dmalloc.h
 * @brief v0.3 formal lifecycle API for the slim allocator.
 *
 * DRAFT -- built on the v0.2 slim engine (shm_slim.h / src/shm_slim.c),
 * which is the ONLY engine left in v0.3 (v2 and the v2-compatible shim are
 * being removed as part of this change; see WIP.md).
 *
 * This implements the eight-primitive lifecycle table (d_init, d_publish,
 * d_lookup, d_attach, d_detach, d_revoke, d_delete, d_deinit), with the
 * following resolved deviations from the literal table -- each one was
 * either a genuine gap (the table couldn't function without it) or a
 * necessary concrete choice where the table used C++ optional-parameter
 * notation that C has no equivalent for. See WIP.md for the full
 * discussion; summary here:
 *
 *   1. d_publish gained an explicit `size_t size` parameter. `void *obj`
 *      alone carries no length -- there is no way to allocate or copy
 *      without being told how much.
 *
 *   2. `key` is uint32_t here, not uint64_t as in the source table. The
 *      engine's on-disk key (shm_slim_key_t, inside shm_slim_hdr_t) is
 *      already 32 bits -- that width is what keeps the header at 16 bytes;
 *      widening it to 64 bits would grow every object's fixed overhead by
 *      50%. Declaring this API's `key` as uint64_t while silently
 *      truncating to 32 bits internally was considered and rejected: it
 *      would let two callers who believe they're using different keys
 *      (differing only in the high 32 bits) silently collide on the same
 *      actual access token. Matching the table's width was never
 *      load-bearing for anything except surface fidelity, so the
 *      parameter type here is narrowed to say what is actually true.
 *
 *   3. handle_t is opaque and stores the key established at d_init(). Every
 *      bracketed-optional [key] in the table becomes a REQUIRED parameter
 *      in this C signature (C has no default arguments) -- pass
 *      d_handle_key(handle) to act as yourself, or a different value to
 *      act as another principal. d_lookup/d_attach take no key parameter
 *      at all (matching the table exactly) and always use the handle's own
 *      key.
 *
 *   4. d_attach/d_detach are LOGICAL operations: d_init() maps the entire
 *      region once, so attach/detach resolve a name to a pointer inside
 *      that existing mapping rather than issuing a fresh mmap/munmap per
 *      object. A literal per-object mmap would require page-aligned,
 *      page-sized objects, which conflicts directly with this engine's
 *      16-byte-header, tightly-packed-heap design.
 *
 *   5. d_revoke() rotates the named object's key; there is no per-host
 *      access-control list and no active cross-host invalidation push.
 *      Anyone holding the OLD key (not just host_id) loses access on their
 *      next call, lazily. `host_id` is accepted for API-shape and logging
 *      purposes only. Authorizing the rotation itself requires presenting
 *      the object's CURRENT key via handle_key -- there is no separate
 *      "owner" identity distinct from "whoever holds the key" in this
 *      engine (that concept was removed from v2 deliberately).
 *
 *   6. d_delete() requires the matching key; there is no separate
 *      administrative "garbage collector" bypass identity (v2's
 *      SHM_PERM_ADMIN equivalent was deliberately dropped).
 *
 *   7. d_deinit() returns void. The table lists `void *d_deinit(...)` but
 *      its own description says "returns nothing" -- treated as a typo.
 *
 *   8. Namespace binding (d_namespace_bind(), a ninth primitive beyond the
 *      source table) lets a key optionally be confined to the Linux IPC
 *      namespace/cgroup its owner was running in when they bound it --
 *      finer-grained than the region-wide D_ENFORCE_NS/D_ENFORCE_CGROUP
 *      above, which only gate opening the pool at all. This is entirely
 *      opt-in: an unbound key is unrestricted, exactly as if this feature
 *      did not exist. Binding costs one fixed-size slot per distinct KEY
 *      (shm_slim_namespace_entry_t, 24 bytes), never per object -- the
 *      per-object header is completely unaffected by whether or how many
 *      keys have namespace bindings. Design:
 *        - Explicit and self-service only: a handle may bind ONLY its own
 *          key (d_namespace_bind(handle, d_handle_key(handle))) -- there is
 *          no privileged call that binds an arbitrary key on someone
 *          else's behalf, so a key's namespace cannot be hijacked or
 *          preemptively locked by a third party.
 *        - One namespace per key (not one namespace shared by many keys):
 *          shm_slim_namespace_bind() rejects a second bind for the same
 *          key with EEXIST.
 *        - d_attach() checks it: after the normal key match succeeds,
 *          d_attach() calls shm_slim_namespace_check(region,
 *          d_handle_key(handle)) and returns NULL on EPERM. d_init(),
 *          d_publish(), d_lookup(), and d_delete() do not additionally
 *          check it -- this keeps the feature's cost exactly where it is
 *          opted into (the one call that actually hands out a pointer),
 *          not smeared across every primitive.
 *        - d_revoke() must carry a binding forward, not drop it: after
 *          shm_slim_set_key() rotates old_key -> new_key, d_revoke() calls
 *          shm_slim_namespace_rebind(region, old_key, new_key) so the
 *          restriction still applies to whoever holds the new key. Without
 *          this, a revoke would silently un-restrict the object.
 *        - d_delete() releases the binding: after shm_slim_free() succeeds,
 *          d_delete() calls shm_slim_namespace_unbind(region, key). Caveat,
 *          documented rather than hidden: if the same key is shared by
 *          several objects (a legitimate pattern -- one key, many objects,
 *          all one "namespace" worth of access), deleting any ONE of them
 *          releases the namespace binding for the key as a whole, even
 *          though the other objects using that key still exist. There is
 *          no reference count. If that matters for a given deployment,
 *          don't delete a shared key's last-standing binding casually, or
 *          re-bind it after the delete.
 *
 *   9. Parallel attach (d_attach_slot() + d_name_capacity(), a tenth and
 *      eleventh primitive beyond the source table) exists because d_attach()
 *      cannot be made to scale by itself: shm_slim_lookup_by_name() holds
 *      the region's one process-shared mutex for its entire linear scan, so
 *      spawning threads that all call d_attach() concurrently buys zero
 *      parallelism -- every call still serialises behind the same lock, on
 *      top of already being O(name table size). Measured directly on
 *      objstream (test-prog/stream-namebound/): attaching 30,001 named
 *      scalars sequentially via d_attach() took 1.22s wall time; see
 *      WIP.md for the full before/after with d_attach_slot().
 *        - d_name_capacity(handle) returns the size of the range to
 *          partition -- callers split [0, d_name_capacity(handle)) across
 *          worker threads/processes, each calling d_attach_slot() over its
 *          own slice.
 *        - d_attach_slot(handle, slot_index, flag, name_out, name_out_cap,
 *          out_ptr) is the slot-oriented counterpart to d_attach(): instead
 *          of searching for a name, it reads one physical name-table slot
 *          directly and tells the caller what name (if any) lives there,
 *          mirroring shm_slim_name_read_slot()/v2's shm_dir_read_slot().
 *        - LOCK-FREE, UNDER A CONTRACT: d_attach_slot() always reads
 *          lock-free (SHM_SLIM_READ_LOCK_FREE) -- safe only while no thread,
 *          in this or any other attached process, is calling
 *          d_publish/d_delete/d_revoke/d_namespace_bind on the same handle's
 *          region. This is the same quiescent-table contract v2's
 *          SHM_DIR_READ_LOCK_FREE already requires (see shm_alloc.h) --
 *          not a new kind of risk this API introduces, the same one v2
 *          already accepted for exactly this use case. Violating it is
 *          undefined; there is no detection of a concurrent writer.
 *          d_attach() remains the safe-under-any-concurrency default; use
 *          d_attach_slot() only for the specific "one process finished
 *          publishing and exited, now a worker pool attaches to read"
 *          pattern it targets.
 *        - This does NOT fix d_publish()'s cost. Inserting a new name still
 *          requires the mutex (to check for a duplicate and claim a free
 *          slot atomically against other publishers) -- parallel attach
 *          only speeds up the read side. See WIP.md.
 */

#ifndef DMALLOC_H
#define DMALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMALLOC_VERSION_MAJOR 0
#define DMALLOC_VERSION_MINOR 3
#define DMALLOC_VERSION_STRING "0.3"

/* ── Flags ───────────────────────────────────────────────────────────────── */

typedef uint32_t flags_t;

#define D_CREATE          0x01u  /* d_init: initialize a brand-new pool over fd (vs. attach to an existing one) */
#define D_ENFORCE_NS      0x02u  /* d_init: reject attach from a different IPC namespace */
#define D_ENFORCE_CGROUP  0x04u  /* d_init: reject attach from a different cgroup */

/*
 * Reserved for d_lookup()/d_attach(). This engine's access control is
 * all-or-nothing (a matching key, or not) -- there is no read/write
 * distinction to enforce, so these bits are currently accepted but not
 * checked. Kept for signature compatibility with the source table and as
 * a place to land real enforcement later if the key model grows one.
 */
#define D_READ            0x08u
#define D_WRITE           0x10u

/* ── Handle ──────────────────────────────────────────────────────────────── */

/** Opaque; wraps the underlying region plus the key established at d_init(). */
typedef struct handle handle_t;

/** The key this handle acts as by default. Pass this back into any
 *  function below that takes an explicit key to mean "as myself". */
uint32_t d_handle_key(const handle_t *handle);

/**
 * @brief Confine this handle's own key to the caller's current Linux IPC
 *        namespace/cgroup. Opt-in -- see file header, point 8.
 *
 * @param key  Must equal d_handle_key(handle); a handle can only bind its
 *             own key, never another principal's.
 * @return     true on success; false if key isn't this handle's own key,
 *             the key is already bound (unbind first to rebind), or the
 *             namespace table is full.
 */
bool d_namespace_bind(handle_t *handle, uint32_t key);

/* ── Parallel attach (see file header, point 9) ─────────────────────────── */

/**
 * @brief Size of the range to partition across d_attach_slot() calls.
 * @return Name-table capacity backing this handle's region; 0 on error.
 */
uint32_t d_name_capacity(const handle_t *handle);

/**
 * @brief Slot-oriented, lock-free counterpart to d_attach().
 *
 * Reads name-table slot `slot_index` directly instead of searching by name,
 * so independent callers can partition [0, d_name_capacity(handle)) and
 * attach concurrently without serialising on the engine's one mutex. See
 * file header, point 9, for the quiescent-table contract this requires.
 *
 * @param slot_index    Physical name-table slot in [0, d_name_capacity(handle)).
 * @param flag          Reserved, same meaning as d_attach()'s flag.
 * @param name_out      Receives the object's NUL-terminated name. May be
 *                      NULL if the caller doesn't need it.
 * @param name_out_cap  Capacity of name_out in bytes; ignored if name_out
 *                      is NULL.
 * @param out_ptr       Receives the attached pointer on success.
 * @return              0 on success; ENOENT if the slot is unused; ERANGE
 *                      if slot_index is out of range; EACCES if handle's
 *                      key does not match the object's key; EPERM if
 *                      handle's key is namespace-bound and the caller is
 *                      outside that namespace; EINVAL for invalid arguments.
 */
int d_attach_slot(handle_t *handle, uint32_t slot_index, flags_t flag,
                  char *name_out, size_t name_out_cap, void **out_ptr);

/* ── Lifecycle primitives ────────────────────────────────────────────────── */

/**
 * @brief Initialize a handle over a contiguous range of shared/CXL memory.
 *
 * @param fd     File descriptor already opened by the caller (e.g. via
 *               shm_open() or open() on a DAX device) referring to the
 *               backing memory. d_init() takes over from here: it maps fd
 *               and either initializes (D_CREATE) or validates (attach)
 *               the pool structures within it.
 * @param flag   D_CREATE to initialize a new pool; omit to attach to an
 *               existing one. May be OR'd with D_ENFORCE_NS/D_ENFORCE_CGROUP.
 * @param key    The identity this handle will act as for every subsequent
 *               call that doesn't override it explicitly.
 * @return       A valid handle on success; NULL on failure (bad fd, failed
 *               mmap, or -- when attaching -- a pool that fails validation
 *               or namespace/cgroup enforcement).
 *
 * If the environment variable DMALLOC_LEASE_DURATION_US is set (and
 * non-zero), this handle also starts a time lease of that many
 * microseconds -- see d_lease_expired() and d_reinit(). Unset or 0
 * (the default) leaves this handle unleased, with zero added cost.
 */
handle_t *d_init(int fd, flags_t flag, uint32_t key);

/**
 * @brief Register a new named object, allocating and copying its contents.
 *
 * @param handle  Handle from d_init().
 * @param name    ASCII name to register the object under. Must not already
 *                be in use.
 * @param obj     Local buffer to copy `size` bytes from.
 * @param size    Number of bytes to allocate and copy from `obj`.
 * @param key     Access-control key to seal onto the new object. Pass
 *                d_handle_key(handle) to publish under your own identity.
 * @return        true on success; false on name collision, name-table
 *                exhaustion, or allocation failure.
 */
bool d_publish(handle_t *handle, const char *name, void *obj, size_t size,
               uint32_t key);

/**
 * @brief Check whether a named object exists and this handle can access it,
 *        without mapping it.
 *
 * Deliberately does not return a pointer or any object data -- discovery
 * and mapping are separate operations; call d_attach() to actually get a
 * pointer.
 *
 * @return true if the name exists and handle's key grants access.
 */
bool d_lookup(handle_t *handle, const char *name, flags_t flag);

/**
 * @brief Resolve a named object to a local pointer.
 *
 * If handle's key has an active namespace binding (see d_namespace_bind()),
 * this also requires the caller to currently be in that namespace/cgroup --
 * checked here, after the key match, and nowhere else (see file header,
 * point 8). Unbound keys are unaffected by this at all.
 *
 * @return Pointer to the object's payload on success; NULL if the name
 *         does not exist, handle's key does not grant access, or handle's
 *         key is namespace-bound and the caller is outside that namespace.
 */
void *d_attach(handle_t *handle, const char *name, flags_t flag);

/**
 * @brief Release a pointer previously returned by d_attach().
 *
 * Logical, not a real per-object unmap (see file header, point 4): d_init()
 * holds the whole region mapped for the handle's lifetime. This validates
 * that ptr belongs to this handle's region and is (still) live.
 *
 * @return true if ptr was valid for this handle; false otherwise.
 */
bool d_detach(handle_t *handle, void *ptr);

/**
 * @brief Query whether this handle's own time lease has expired.
 *
 * d_attach()/d_detach() only check a key at the moment they're called;
 * every ordinary load/store through the pointer d_attach() returns is an
 * ordinary CPU instruction that never consults this library again, by
 * design (see the control-path/data-path split throughout this file).
 * Checking that on every individual attached OBJECT (an earlier design
 * this replaced) turned out not to be worth it -- there is no way to do
 * that without either a per-object tracked-pointer table and a sweep, or
 * touching the data path directly, and both cost more than this project
 * is willing to spend. A time lease on the HANDLE itself is the cheap
 * version of the same idea: d_init() starts a lease of length
 * DMALLOC_LEASE_DURATION_US (env var, microseconds; unset or 0 disables
 * leasing for that handle entirely -- default behavior, zero added
 * cost), and d_reinit() -- already this project's "software forced key
 * rotation" primitive -- renews it as a side effect of every successful
 * call.
 *
 * This is deliberately lazy: nothing in this library calls
 * d_lease_expired() automatically, on any schedule, and nothing crashes
 * the process on its behalf. It costs one timestamp comparison, and nothing
 * else exists between calls -- no background thread, no tracked table. It
 * is entirely up to the caller to check it (however often fits their own
 * workload) and decide what an expired lease means for them: call
 * d_reinit() to renew it, exit, log, or ignore it.
 *
 * @return true if leasing is enabled for this handle (its lease duration
 *         is non-zero) AND that duration has elapsed since d_init() or
 *         the last successful d_reinit(); false if leasing was never
 *         enabled for this handle, or the lease is still current.
 */
bool d_lease_expired(handle_t *handle);

/**
 * @brief Renew this handle's own (process-level) identity.
 *
 * Distinct from d_revoke(), which rotates a single OBJECT's key: this
 * changes which key THIS handle presents on every subsequent call that
 * doesn't explicitly override it. Existing objects previously published
 * under the handle's OLD key are unaffected by this call alone -- they
 * still require the old key, exactly as if a different handle held it --
 * so a bare d_reinit() does not carry forward access to this handle's own
 * prior objects; a caller that needs continuity must d_revoke() each such
 * object to the new key itself.
 *
 * If the OLD key had an active namespace binding (d_namespace_bind()), it
 * is carried forward to new_key automatically, mirroring d_revoke()'s
 * existing behavior for object-level bindings -- otherwise a reinit would
 * silently and unintentionally lift a namespace restriction the caller
 * never asked to remove.
 *
 * If this handle was leased (see d_lease_expired()), a successful call
 * also renews the lease -- d_reinit() IS this project's "re-lease"
 * action, regardless of how much of the previous lease period had
 * elapsed. A handle that was never leased is unaffected.
 *
 * @param new_key  The key this handle will act as from now on.
 * @return         true on success; false only if handle is NULL.
 */
bool d_reinit(handle_t *handle, uint32_t new_key);

/**
 * @brief Rotate a named object's access key.
 *
 * handle must currently hold the object's key (there is no separate owner
 * identity in this engine -- see file header, point 5). Every other
 * holder of the OLD key, including host_id, loses access on their next
 * call; there is no active push to other hosts.
 *
 * If the current key has a namespace binding, it is carried forward to
 * new_key automatically (see file header, point 8) -- a revoke never
 * silently drops an existing namespace restriction.
 *
 * @param host_id  Accepted for logging/API-shape purposes; this engine has
 *                 no per-host access-control list to selectively revoke.
 * @param new_key  The key to rotate to.
 * @return         true on success; false if handle's key does not match
 *                 the object's current key, or the name does not exist.
 */
bool d_revoke(handle_t *handle, const char *name, int host_id,
              uint32_t new_key);

/**
 * @brief Delete a named object and reclaim its space.
 *
 * Also releases key's namespace binding, if any (see file header, point 8
 * for the caveat if the same key is shared by other, still-live objects).
 *
 * @param key  Must match the object's current key.
 * @return     0 on success; an errno-style code otherwise (ENOENT, EACCES).
 */
int d_delete(handle_t *handle, const char *name, uint32_t key);

/**
 * @brief Tear down a handle: unmap and free it. Does not touch the pool's
 *        contents -- other handles/processes are unaffected.
 */
void d_deinit(handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* DMALLOC_H */
