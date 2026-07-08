/**
 * @file  shm_cap.hpp
 * @brief C++ typed wrapper over the CHERI-like shm_cap_t capability API.
 *
 * Usage quick-reference
 * ─────────────────────
 *
 * @code
 * // Derive a capability from a newly allocated object
 * shm::ptr<MyObj> handle = shm::alloc<MyObj>(region, uid, perms);
 * shm::cap<MyObj> c = shm::cap<MyObj>::from_ptr(region, handle,
 *                         uid, perms, SHM_PERM_READ | SHM_PERM_WRITE);
 *
 * // Dereference for read-write access
 * MyObj *p = c.rw(region);
 *
 * // Narrow to read-only and pass to untrusted code
 * shm::cap<MyObj> ro_cap = c.narrow(SHM_PERM_READ);
 *
 * // Check liveness
 * if (!ro_cap.valid(region)) { ... }
 * @endcode
 *
 * Design notes
 * ─────────────
 * shm::cap<T> wraps shm_cap_t and adds:
 *   - Type safety:    the template parameter T annotates what type the
 *                     payload is expected to hold.
 *   - operator bool:  true iff the cap is non-null.
 *   - valid():        true iff the cap passes structural validation (no perm check).
 *   - get() / rw():   typed dereference with explicit permission requirements.
 *   - narrow():       monotonic permission reduction.
 *   - reinterpret():  unsafe cast to shm::cap<U> for low-level interop.
 *   - from_ptr():     derive from an existing shm::ptr<T> handle.
 *   - from_off():     derive from a raw shm_off_t.
 */

#pragma once

#include "shm_alloc.hpp"   /* shm::ptr<T>, type_tag_of<T>() */
#include "shm_cap.h"       /* shm_cap_t, shm_cap_derive(), etc. */

#include <cstddef>         /* size_t */
#include <cstdint>         /* uint16_t */

namespace shm {

/**
 * A CHERI-like 128-bit software capability for shared-memory type @p T.
 *
 * Thin wrapper over shm_cap_t.  All actual security logic is in shm_cap.c.
 * The wrapper is copyable (plain 128-bit value) and safe to store in shared
 * memory (no virtual tables, no raw pointers).
 */
template <typename T>
class cap {
public:

    /* ── Constructors ──────────────────────────────────────────────────── */

    /** Default-construct the null capability. */
    cap() noexcept : c_(SHM_CAP_NULL) {}

    /** Wrap an existing raw shm_cap_t. */
    explicit cap(shm_cap_t raw) noexcept : c_(raw) {}

    /* ── Raw access ────────────────────────────────────────────────────── */

    /** Return the underlying shm_cap_t for use with the C API. */
    shm_cap_t raw() const noexcept { return c_; }

    /* ── Null / validity ───────────────────────────────────────────────── */

    /** True iff this capability is non-null. */
    explicit operator bool() const noexcept { return !shm_cap_is_null(c_); }

    /** True iff the capability passes structural validation (magic, tag, epoch, bounds). */
    bool valid(const shm_region_t *r) const noexcept
    {
        return shm_cap_is_valid(r, c_);
    }

    /* ── Dereference ───────────────────────────────────────────────────── */

    /**
     * Validate and return a const pointer for read-only access.
     * Returns nullptr on any validation failure; check errno for details.
     */
    const T *get(const shm_region_t *r,
                 shm_perm_t req = SHM_PERM_READ) const noexcept
    {
        return static_cast<const T *>(shm_cap_deref(r, c_, req));
    }

    /**
     * Validate and return a mutable pointer for read-write access.
     * Returns nullptr on any validation failure; check errno for details.
     */
    T *rw(shm_region_t *r) const noexcept
    {
        return static_cast<T *>(shm_cap_deref(r, c_,
            static_cast<shm_perm_t>(SHM_PERM_READ | SHM_PERM_WRITE)));
    }

    /**
     * Validate with an explicit required-permissions mask.
     * Returns non-null only when all @p required bits are both sealed in
     * the capability AND still present in the live block header.
     */
    T *deref(shm_region_t *r, shm_perm_t required) const noexcept
    {
        return static_cast<T *>(shm_cap_deref(r, c_, required));
    }

    /* ── Narrowing ─────────────────────────────────────────────────────── */

    /**
     * Return a new capability with fewer permission bits.
     * Returns a null cap<T> if @p new_perms contains bits absent from this cap.
     */
    cap<T> narrow(shm_perm_t new_perms) const noexcept
    {
        return cap<T>(shm_cap_narrow(c_, new_perms));
    }

    /* ── Introspection ─────────────────────────────────────────────────── */

    /** Permission bits sealed in this capability. */
    shm_perm_t perms()  const noexcept { return shm_cap_perms(c_); }

    /** Payload capacity (bounds) sealed in this capability. */
    size_t     bounds() const noexcept { return shm_cap_bounds(c_); }

    /** Heap-relative payload offset sealed in this capability. */
    shm_off_t  offset() const noexcept { return shm_cap_offset(c_); }

    /** ABA-protection epoch sealed in this capability. */
    uint16_t   epoch()  const noexcept { return shm_cap_epoch(c_); }

    /* ── Unsafe reinterpret ────────────────────────────────────────────── */

    /**
     * Reinterpret the capability as a capability for type @p U.
     * The caller is responsible for ensuring the types are compatible.
     * The underlying shm_cap_t is unchanged; only the C++ type annotation
     * changes.
     */
    template <typename U>
    cap<U> reinterpret_as() const noexcept { return cap<U>(c_); }

    /* ── Factory functions ─────────────────────────────────────────────── */

    /**
     * Derive a capability from a typed handle (shm::ptr<T>).
     *
     * @param r             Region handle.
     * @param handle        Typed offset handle from shm::alloc<T>() etc.
     * @param uid           Caller's user id.
     * @param caller_perms  Caller's own permission bits.
     * @param grant_perms   Bits to seal into the capability (⊆ intersection).
     * @return              The new capability, or a null cap on failure.
     */
    static cap<T> from_ptr(const shm_region_t *r,
                           ptr<T>              handle,
                           shm_user_id_t       uid,
                           shm_perm_t          caller_perms,
                           shm_perm_t          grant_perms) noexcept
    {
        return cap<T>(shm_cap_derive(r, handle.off, uid, caller_perms, grant_perms));
    }

    /**
     * Derive a capability from a raw shm_off_t.
     * Convenience form for code that holds a raw offset rather than a typed handle.
     */
    static cap<T> from_off(const shm_region_t *r,
                           shm_off_t           off,
                           shm_user_id_t       uid,
                           shm_perm_t          caller_perms,
                           shm_perm_t          grant_perms) noexcept
    {
        return cap<T>(shm_cap_derive(r, off, uid, caller_perms, grant_perms));
    }

    /** Return the null capability of this type. */
    static cap<T> null() noexcept { return cap<T>(); }

private:
    shm_cap_t c_;   /* underlying 128-bit raw capability */
};

} /* namespace shm */
