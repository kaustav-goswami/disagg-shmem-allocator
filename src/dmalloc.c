/**
 * @file  dmalloc.c
 * @brief v0.3 formal lifecycle API bodies. See include/dmalloc.h for the
 *        full design rationale (the eight resolved deviations from the
 *        source table, plus the namespace-binding addendum).
 *
 * Built entirely on the v0.2 slim engine (shm_slim.h) -- this file never
 * touches shm_slim_internal.h; every operation here is expressible through
 * the slim engine's own public API.
 */

#define _POSIX_C_SOURCE 200809L

#include "dmalloc.h"
#include "shm_slim.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * d_init()'s signature is fixed at (fd, flag, key) by the source table --
 * there is no parameter to size the name/namespace tables when creating a
 * pool. That is a real gap for any workload that publishes more than a
 * handful of named objects (see objstream in test-prog/stream-namebound/,
 * which -- because d_publish() has no anonymous counterpart -- must name
 * every single object). Rather than silently picking one fixed capacity
 * that is wrong for most callers either way, d_init() reads it from the
 * environment on create, defaulting to the slim engine's own defaults (64)
 * when unset. This is a deliberately visible workaround for a signature
 * limitation, not a hidden one -- flagged here and in WIP.md.
 */
static uint32_t env_capacity(const char *var)
{
    const char *s = getenv(var);
    if (!s || !*s) return 0;   /* 0 -> shm_slim engine default */
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0' || v == 0 || v > 0xFFFFFFFFul) return 0;
    return (uint32_t)v;
}

/**
 * shm_slim_fd_size() (fstat(), with a sysfs fallback for DAX character
 * devices) is not reliable for every DAX/CXL device node in the wild --
 * confirmed working for some /dev/daxX.Y nodes, confirmed failing
 * (reports 0) for at least one real /dev/cxl/memN node. Same "signature
 * has no room for this parameter" situation as DMALLOC_NAME_CAPACITY
 * above: d_init(fd, flag, key) can't take an explicit size, so this reads
 * one from the environment as the escape hatch when auto-detection
 * doesn't work for a given device, rather than leaving the caller stuck.
 *
 * DANGEROUS IF WRONG, NOT JUST INACCURATE: this value goes straight into
 * mmap()'s length. If it's larger than what the fd can actually back
 * (device too small, or -- confirmed directly -- a POSIX shm file only
 * ftruncate()'d to a smaller size), touching memory past the real extent
 * is a SIGBUS crash, not a clean error; mmap() itself happily succeeds
 * with an oversized length and only traps on the actual access. There is
 * no way to validate this value against ground truth here -- if
 * auto-detection could confirm it, you wouldn't need this override in
 * the first place. Set it to your device's REAL, KNOWN capacity (or less)
 * -- e.g. `daxctl list` / `cxl list` output -- never a guess.
 */
static size_t env_size(const char *var)
{
    const char *s = getenv(var);
    if (!s || !*s) return 0;   /* 0 -> auto-detect via shm_slim_fd_size() */
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0' || v == 0) return 0;
    return (size_t)v;
}

/**
 * d_init()'s signature also has no backend-selection parameter.
 * DMALLOC_BACKEND=generic makes it source the region's backing memory
 * from whichever generic allocator library shm_slim.c was COMPILED
 * against (GENERIC_ALLOC=jemalloc|cxlalloc at build time -- see the main
 * repo Makefile and src/shm_generic_backend.h) instead of POSIX shm/mmap.
 * This is a runtime on/off switch for a compile-time choice, not a
 * runtime allocator selector: shm_generic_backend_alloc() itself is still
 * chosen entirely at compile time, exactly as it always was -- setting
 * DMALLOC_BACKEND does not and cannot pick jemalloc vs cxlalloc, only
 * whether d_init() uses the generic path at all. Unset (or any value
 * other than "generic") preserves today's exact behavior.
 *
 * KNOWN LIMITATION, inherited, not introduced here:
 * shm_slim_region_open()'s generic-backend branch returns ENOTSUP for
 * anything but D_CREATE -- there is no OS-level named object a second
 * process could attach to (see shm_generic_backend.h). So
 * DMALLOC_BACKEND=generic only ever supports a single process that both
 * publishes and attaches its own objects; `fd` is ignored entirely in
 * this mode (there is nothing to open a generic-backend region from),
 * and DMALLOC_REGION_SIZE (see env_size() above) becomes REQUIRED rather
 * than an optional override, since there is no fd to fstat() a size from
 * either.
 */
static bool use_generic_backend(void)
{
    const char *s = getenv("DMALLOC_BACKEND");
    return s && strcmp(s, "generic") == 0;
}

/**
 * d_init()'s signature has no room for a fixed mmap base address either
 * (see shm_slim.h's shm_slim_region_open_opts_t.fixed_base for what this
 * actually does and why it exists at all -- it's an escape hatch for
 * callers whose OWN design assumes raw pointers embedded in shared data
 * stay valid across processes, which requires every process to map the
 * region at the identical virtual address; ordinary dmalloc use never
 * needs this, since d_attach() is already offset-based). Parsed as a hex
 * address (with or without a leading "0x"); unset or unparsable leaves
 * fixed_base NULL, i.e. today's normal OS/ASLR-chosen mapping.
 */
static void *env_fixed_base(const char *var)
{
    const char *s = getenv(var);
    if (!s || !*s) return NULL;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 16);
    if (!end || *end != '\0' || v == 0) return NULL;
    return (void *)(uintptr_t)v;
}

/**
 * Per-object access control (a tracked table of every attached pointer,
 * checked either by a background thread or on demand -- the earlier
 * design this replaced) turned out not to be worth its overhead: there is
 * no way to check an individual object's key without either touching the
 * data path on every access or maintaining a whole tracked-pointer table
 * and sweeping it. A per-HANDLE time lease is the only version of this
 * that's actually cheap -- one pair of timestamps, no table, no sweep,
 * and d_attach()/d_detach() are completely unaware it exists (see their
 * bodies below, and WIP.md for the fuller rationale).
 *
 * d_init() starts the lease (if DMALLOC_LEASE_DURATION_US is set --
 * see env_lease_duration_ns() below); d_reinit(), already this project's
 * "software forced key rotation" primitive, renews it as a side effect
 * of every successful call. Checking it is d_lease_expired()'s entire
 * job, and this library never calls that function itself, on any
 * schedule -- deliberately lazy, matching every other opt-in knob here:
 * zero cost unless the caller asks for it, and even then, one timestamp
 * comparison.
 */
static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/**
 * Same "signature has no room for this parameter" situation as
 * DMALLOC_NAME_CAPACITY/DMALLOC_REGION_SIZE above: d_init(fd, flag, key)
 * can't take a lease duration argument, so it's read from the
 * environment instead. Unset or 0 disables leasing for this handle
 * entirely -- d_lease_expired() then always reports false, and nothing
 * else changes.
 */
static uint64_t env_lease_duration_ns(void)
{
    const char *s = getenv("DMALLOC_LEASE_DURATION_US");
    if (!s || !*s) return 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0' || v == 0) return 0;
    return (uint64_t)v * 1000ull;
}

struct handle {
    shm_slim_region_t *region;
    uint32_t            key;   /* this handle's own identity, set at d_init() */
    uint64_t             lease_start_ns;    /* monotonic; 0 if leasing disabled */
    uint64_t             lease_duration_ns; /* 0 -> leasing disabled for this handle */
};

uint32_t d_handle_key(const handle_t *handle)
{
    return handle ? handle->key : 0;
}

bool d_namespace_bind(handle_t *handle, uint32_t key)
{
    if (!handle || key != handle->key)
        return false;   /* a handle may only bind its OWN key -- see dmalloc.h */
    return shm_slim_namespace_bind(handle->region, (shm_slim_key_t)key) == 0;
}

uint32_t d_name_capacity(const handle_t *handle)
{
    if (!handle) return 0;
    shm_slim_pool_stats_t stats;
    if (shm_slim_pool_stats(handle->region, &stats) != 0) return 0;
    return stats.name_capacity;
}

int d_attach_slot(handle_t *handle, uint32_t slot_index, flags_t flag,
                  char *name_out, size_t name_out_cap, void **out_ptr)
{
    (void)flag;   /* reserved -- see dmalloc.h */
    if (!handle || !out_ptr)
        return EINVAL;

    shm_slim_name_slot_t slot;
    int rc = shm_slim_name_read_slot(handle->region, slot_index,
                                     SHM_SLIM_READ_LOCK_FREE, &slot);
    if (rc != 0)   /* ENOENT (unused slot) or ERANGE, propagated as-is */
        return rc;

    /* Same enforcement point as d_attach() -- see file header, point 8/9. */
    if (shm_slim_namespace_check_ex(handle->region, (shm_slim_key_t)handle->key,
                                    SHM_SLIM_READ_LOCK_FREE) != 0)
        return EPERM;

    void *p = shm_slim_ptr(handle->region, slot.off, (shm_slim_key_t)handle->key);
    if (!p)
        return EACCES;

    if (name_out && name_out_cap > 0) {
        strncpy(name_out, slot.name, name_out_cap - 1);
        name_out[name_out_cap - 1] = '\0';
    }
    *out_ptr = p;
    return 0;
}

handle_t *d_init(int fd, flags_t flag, uint32_t key)
{
    uint32_t slim_flags = 0;
    if (flag & D_CREATE)         slim_flags |= SHM_SLIM_OPEN_CREATE;
    if (flag & D_ENFORCE_NS)     slim_flags |= SHM_SLIM_OPEN_ENFORCE_NS;
    if (flag & D_ENFORCE_CGROUP) slim_flags |= SHM_SLIM_OPEN_ENFORCE_CGROUP;

    shm_slim_region_open_opts_t opts = {0};
    opts.name_capacity      = env_capacity("DMALLOC_NAME_CAPACITY");
    opts.namespace_capacity = env_capacity("DMALLOC_NAMESPACE_CAPACITY");
    opts.size_hint           = env_size("DMALLOC_REGION_SIZE");
    opts.fixed_base          = env_fixed_base("DMALLOC_FIXED_BASE");

    shm_slim_region_t *region = NULL;

    if (use_generic_backend()) {
        /* See use_generic_backend()'s comment: D_CREATE and an explicit
         * size are both hard requirements here, not just recommended --
         * there is no fd to fall back on for either. */
        if (!(flag & D_CREATE) || opts.size_hint == 0)
            return NULL;
        opts.backend = SHM_SLIM_BACKEND_GENERIC;
        opts.flags   = slim_flags;
        if (shm_slim_region_open("dmalloc_generic_backend", opts.size_hint,
                                 &opts, &region) != 0)
            return NULL;
    } else if (shm_slim_region_open_fd(fd, slim_flags, &opts, &region) != 0) {
        return NULL;
    }

    handle_t *h = malloc(sizeof(*h));
    if (!h) {
        shm_slim_region_close(region, false);
        return NULL;
    }
    h->region = region;
    h->key    = key;
    h->lease_duration_ns = env_lease_duration_ns();
    h->lease_start_ns    = h->lease_duration_ns ? monotonic_ns() : 0;
    return h;
}

bool d_publish(handle_t *handle, const char *name, void *obj, size_t size,
               uint32_t key)
{
    if (!handle || !name || !obj || size == 0)
        return false;

    shm_off_t off = SHM_OFF_NULL;
    if (shm_slim_alloc(handle->region, size, name, (shm_slim_key_t)key, &off) != 0)
        return false;

    void *p = shm_slim_ptr(handle->region, off, (shm_slim_key_t)key);
    if (!p)   /* cannot happen right after a successful alloc with this key */
        return false;

    memcpy(p, obj, size);
    return true;
}

bool d_lookup(handle_t *handle, const char *name, flags_t flag)
{
    (void)flag;   /* reserved -- see dmalloc.h */
    if (!handle || !name)
        return false;

    shm_off_t off;
    return shm_slim_lookup_by_name(handle->region, name, (shm_slim_key_t)handle->key,
                                   &off) == 0;
}

void *d_attach(handle_t *handle, const char *name, flags_t flag)
{
    (void)flag;   /* reserved -- see dmalloc.h */
    if (!handle || !name)
        return NULL;

    shm_off_t off;
    if (shm_slim_lookup_by_name(handle->region, name, (shm_slim_key_t)handle->key,
                                &off) != 0)
        return NULL;

    /* Namespace check happens here and only here -- see dmalloc.h point 8. */
    if (shm_slim_namespace_check(handle->region, (shm_slim_key_t)handle->key) != 0)
        return NULL;

    return shm_slim_ptr(handle->region, off, (shm_slim_key_t)handle->key);
}

bool d_detach(handle_t *handle, void *ptr)
{
    if (!handle || !ptr)
        return false;

    shm_off_t off;
    if (shm_slim_ptr_to_off(handle->region, ptr, &off) != 0)
        return false;

    size_t size;
    return shm_slim_block_size(handle->region, off, (shm_slim_key_t)handle->key,
                               &size) == 0;
}

bool d_reinit(handle_t *handle, uint32_t new_key)
{
    if (!handle)
        return false;

    /* Carry any namespace binding on the OLD key forward -- see dmalloc.h.
     * Not fatal if this fails (e.g. new_key already independently bound
     * to something else): the handle's own key still gets updated below,
     * which is this call's primary effect. */
    shm_slim_namespace_rebind(handle->region, (shm_slim_key_t)handle->key,
                              (shm_slim_key_t)new_key);

    handle->key = new_key;

    /* Every successful reinit re-leases the handle, regardless of how
     * much (or how little) of the previous lease had elapsed -- see the
     * comment above env_lease_duration_ns(). A no-op when this handle
     * was never leased in the first place (lease_duration_ns == 0). */
    if (handle->lease_duration_ns)
        handle->lease_start_ns = monotonic_ns();

    return true;
}

bool d_lease_expired(handle_t *handle)
{
    if (!handle || !handle->lease_duration_ns)
        return false;   /* leasing disabled for this handle -- never expires */
    return monotonic_ns() - handle->lease_start_ns > handle->lease_duration_ns;
}

bool d_revoke(handle_t *handle, const char *name, int host_id, uint32_t new_key)
{
    (void)host_id;   /* accepted for API-shape/logging only -- see dmalloc.h */
    if (!handle || !name)
        return false;

    shm_off_t off;
    if (shm_slim_lookup_by_name(handle->region, name, (shm_slim_key_t)handle->key,
                                &off) != 0)
        return false;

    uint32_t old_key = handle->key;
    if (shm_slim_set_key(handle->region, off, (shm_slim_key_t)old_key,
                         (shm_slim_key_t)new_key) != 0)
        return false;

    /* Carry any namespace binding forward -- see dmalloc.h point 8. Not
     * fatal if this fails (e.g. new_key already independently bound):
     * the key rotation itself already succeeded and is the primary effect
     * of a revoke. */
    shm_slim_namespace_rebind(handle->region, (shm_slim_key_t)old_key,
                              (shm_slim_key_t)new_key);

    /* handle->key is deliberately NOT updated to new_key: this handle's
     * identity is independent of any one object it happens to have just
     * revoked -- see dmalloc.h. Callers that want to keep using this
     * specific object under its new key need a handle whose own key is
     * new_key. */
    return true;
}

int d_delete(handle_t *handle, const char *name, uint32_t key)
{
    if (!handle || !name)
        return EINVAL;

    /* shm_slim_free_by_name() resolves the name-table entry with the same
     * hashed lookup used everywhere else and clears it directly, instead
     * of a separate shm_slim_lookup_by_name() + shm_slim_free() pair (the
     * latter still has to find the entry via an offset-keyed linear scan
     * to remove it). See WIP.md. */
    int rc = shm_slim_free_by_name(handle->region, name, (shm_slim_key_t)key, NULL);
    if (rc != 0)
        return rc;

    shm_slim_namespace_unbind(handle->region, (shm_slim_key_t)key);
    return 0;
}

void d_deinit(handle_t *handle)
{
    if (!handle)
        return;

    /* Never unlinks -- "does not touch the pool's contents; other
     * handles/processes are unaffected" (see dmalloc.h). */
    shm_slim_region_close(handle->region, false);
    free(handle);
}
