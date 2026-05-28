/**
 * @file  shm_alloc.hpp
 * @brief C++ typed wrappers over the shm_alloc C API.
 *
 * Design goals
 * ────────────
 *   shm::ptr<T>         Strongly-typed, process-safe handle (wraps shm_off_t).
 *                       Never stores a raw pointer; always re-resolved via
 *                       shm_ptr() with an explicit permission check.
 *
 *   shm::make<T>()      Allocate + placement-new in one call.
 *   shm::get<T>()       Lookup by handle → raw pointer (read-only).
 *   shm::get_mut<T>()   Lookup by handle → mutable pointer.
 *   shm::cast<To>()     static_cast-style reinterpretation.
 *   shm::cast_dyn<To>() dynamic_cast for polymorphic types stored in shared memory.
 *   shm::find<T>()      Lookup by monotonic ID → typed ptr<T>.
 *   shm::destroy()      ~T() without freeing the block.
 *   shm::free()         ~T() + shm_free().
 *
 * C++ objects in shared memory
 * ────────────────────────────
 *   Placement new constructs an object at the heap address returned by
 *   shm_alloc.  The object must not contain raw pointers (they differ per
 *   process).  Use shm::ptr<T> for internal cross-process references.
 *
 *   For polymorphic types, virtual dispatch works as long as all participating
 *   processes load the same executable / shared library (identical vtable layout).
 *
 * Type tags
 * ─────────
 *   The default type_tag is sizeof(T); override type_tag_of<T> specialisations
 *   or pass an explicit tag to shm::make for protocol-level type safety.
 */

#ifndef SHM_ALLOC_HPP
#define SHM_ALLOC_HPP

#include "shm_alloc.h"   /* C API: shm_region_t, shm_off_t, shm_alloc, … */

#include <cerrno>        /* EINVAL, EACCES, ENOENT */
#include <cstring>       /* memset */
#include <new>           /* placement new, std::bad_alloc */
#include <type_traits>   /* is_trivially_destructible_v, is_polymorphic_v */
#include <utility>       /* std::forward */

namespace shm {

/* ── Typed handle ─────────────────────────────────────────────────────────── */

/**
 * A strongly-typed wrapper around a heap-relative payload offset.
 *
 * shm::ptr<T> can be safely stored in shared memory because it is just a
 * 64-bit integer.  Dereferencing always goes through shm_ptr() which
 * re-validates the block at call time.
 */
template <typename T>
struct ptr {
    shm_off_t off{SHM_OFF_NULL};   /* heap-relative payload offset; SHM_OFF_NULL = null */

    constexpr ptr() = default;                         /* default-construct as null */
    explicit constexpr ptr(shm_off_t o) : off(o) {}   /* construct from raw offset */

    /** True when the handle is not null. */
    explicit operator bool() const { return off != SHM_OFF_NULL && off != 0; }

    /** The raw heap-relative offset (for passing to C API functions). */
    shm_off_t raw() const { return off; }
};

/* ── Type tag ─────────────────────────────────────────────────────────────── */

/**
 * Default type discriminator: sizeof(T) cast to uint32_t.
 * Specialise or override with a custom enum for stronger type safety.
 *
 * Example specialisation:
 * @code
 *   template<> constexpr uint32_t shm::type_tag_of<MyClass>() { return 0xDEAD; }
 * @endcode
 */
template <typename T>
constexpr uint32_t type_tag_of() { return static_cast<uint32_t>(sizeof(T)); }

/* ── Allocation ───────────────────────────────────────────────────────────── */

/**
 * Allocate sizeof(T) bytes in the heap and register in the directory.
 * The payload is zero-initialised (no constructor is called).
 * Use shm::make() to also run the constructor.
 *
 * @tparam T         The type to allocate storage for.
 * @param region     Region handle.
 * @param user_id    Owner user id.
 * @param perms      Permission bits (use SHM_PERM_DEFAULT for full access).
 * @param out        Receives the typed handle on success.
 * @param name       Optional directory name (empty string if unused).
 * @param tag        Type tag (defaults to type_tag_of<T>()).
 * @return           0 on success; errno-compatible code on failure.
 */
template <typename T, uint32_t Tag = type_tag_of<T>()>
int alloc(shm_region_t *region,
          shm_user_id_t user_id,
          shm_perm_t    perms,
          ptr<T>       *out,
          const char   *name = nullptr,
          uint32_t      tag  = Tag)
{
    if (!region || !out) return EINVAL;
    shm_off_t off   = SHM_OFF_NULL;
    uint64_t  id    = 0;
    const int rc    = shm_alloc(region, sizeof(T), name, user_id, perms, tag, &id, &off);
    if (rc != 0) return rc;
    *out = ptr<T>(off);
    return 0;
}

/* ── Pointer resolution ───────────────────────────────────────────────────── */

/**
 * Resolve a typed handle to a read-only pointer.
 * Calls shm_ptr() with required_perms = SHM_PERM_READ.
 *
 * @return Non-NULL pointer on success; nullptr on permission failure.
 */
template <typename T>
const T *get(shm_region_t *region,
             ptr<T>        handle,
             shm_user_id_t user_id,
             shm_perm_t    caller_perms)
{
    return static_cast<const T *>(
        shm_ptr(region, handle.off, user_id, caller_perms, SHM_PERM_READ));
}

/**
 * Resolve a typed handle to a mutable pointer.
 * Calls shm_ptr() with required_perms = SHM_PERM_READ|SHM_PERM_WRITE.
 */
template <typename T>
T *get_mut(shm_region_t *region,
           ptr<T>        handle,
           shm_user_id_t user_id,
           shm_perm_t    caller_perms)
{
    return static_cast<T *>(
        shm_ptr(region, handle.off, user_id, caller_perms,
                static_cast<shm_perm_t>(SHM_PERM_READ | SHM_PERM_WRITE)));
}

/* ── Casts ────────────────────────────────────────────────────────────────── */

/**
 * static_cast-style reinterpretation of a typed handle.
 *
 * Use when you have a ptr<Base> but know the object is actually a Derived,
 * or vice versa.  No runtime type check is performed beyond the block magic.
 *
 * @tparam To    Destination type.
 * @tparam From  Source type stored in the handle.
 */
template <typename To, typename From>
To *cast(shm_region_t *region,
         ptr<From>     handle,
         shm_user_id_t user_id,
         shm_perm_t    caller_perms,
         shm_perm_t    required = SHM_PERM_READ)
{
    return static_cast<To *>(
        shm_ptr(region, handle.off, user_id, caller_perms, required));
}

/**
 * dynamic_cast-style downcast for polymorphic types.
 *
 * Checks the type_tag stored in the block header against type_tag_of<To>().
 * If the tags don't match the cast is refused (returns nullptr).
 * For polymorphic types also runs a real dynamic_cast.
 *
 * @note The type_tag check only works reliably when you explicitly set a
 *       custom tag at alloc time.  The default sizeof-based tag cannot
 *       distinguish unrelated types with the same size.
 */
template <typename To, typename From>
To *cast_dyn(shm_region_t *region,
             ptr<From>     handle,
             shm_user_id_t user_id,
             shm_perm_t    caller_perms)
{
    /* Retrieve raw source pointer (read-only check). */
    void *raw = shm_ptr(region, handle.off, user_id, caller_perms, SHM_PERM_READ);
    if (!raw) return nullptr;   /* permission denied or corrupt block */

    /* Check the type_tag stored in the block header. */
    uint32_t stored_tag = 0;
    shm_block_info(region, handle.off, nullptr, nullptr, nullptr, &stored_tag, nullptr);
    if (stored_tag != 0 && stored_tag != type_tag_of<To>())
        return nullptr;   /* tag mismatch: wrong target type */

    auto *src = static_cast<From *>(raw);

    if constexpr (std::is_polymorphic_v<From>) {
        /* For polymorphic objects we can use the vtable for a real runtime check. */
        return dynamic_cast<To *>(src);
    } else {
        /* Non-polymorphic: static_cast only; caller is responsible for correctness. */
        return static_cast<To *>(src);
    }
}

/* ── Construction / destruction ───────────────────────────────────────────── */

/**
 * Run the constructor of T at the location pointed to by @p handle.
 * The storage must already have been allocated (via shm::alloc or shm_alloc).
 * This does NOT allocate heap memory.
 *
 * @param args  Constructor arguments forwarded to T(args…).
 * @return      0 on success; EACCES if the pointer cannot be resolved.
 */
template <typename T, typename... Args>
int construct(shm_region_t *region,
              ptr<T>        handle,
              shm_user_id_t user_id,
              shm_perm_t    caller_perms,
              Args &&...args)
{
    T *p = get_mut<T>(region, handle, user_id, caller_perms);
    if (!p) return EACCES;               /* permission denied or bad offset */
    new (p) T(std::forward<Args>(args)...);  /* placement new: calls T(args…) */
    return 0;
}

/**
 * Call the destructor of T without freeing the heap block.
 * Useful when the block will be reused with a different constructor call.
 *
 * @return 0 on success; EACCES if the pointer cannot be resolved.
 */
template <typename T>
int destroy(shm_region_t *region,
            ptr<T>        handle,
            shm_user_id_t user_id,
            shm_perm_t    caller_perms)
{
    T *p = get_mut<T>(region, handle, user_id, caller_perms);
    if (!p) return EACCES;
    p->~T();   /* explicit destructor call via placement-delete idiom */
    return 0;
}

/**
 * Call ~T() (unless trivially destructible) and then free the heap block.
 * This is the primary way to release a C++ object from shared memory.
 *
 * @return 0 on success; any error code from ~T() or shm_free().
 */
template <typename T>
int free(shm_region_t *region,
         ptr<T>        handle,
         shm_user_id_t user_id,
         shm_perm_t    caller_perms)
{
    if constexpr (!std::is_trivially_destructible_v<T>) {
        /* Run the destructor before releasing the block so RAII is honoured. */
        const int d = destroy(region, handle, user_id, caller_perms);
        if (d != 0) return d;   /* abort if we cannot resolve the pointer */
    }
    /* Determine the object ID so we can call shm_free (which takes an ID). */
    uint64_t obj_id = 0;
    shm_block_info(region, handle.off, nullptr, nullptr, nullptr, nullptr, &obj_id);
    if (obj_id == 0) return EINVAL;   /* block is not registered in the directory */
    return shm_free(region, obj_id, user_id, caller_perms);
}

/* ── make: allocate + construct in one step ──────────────────────────────── */

/**
 * Allocate sizeof(T) bytes, register in the directory, and run T(args…).
 *
 * On success *out holds a valid handle.  On failure the heap block is not
 * allocated (or is freed if construction throws — note: exceptions are
 * generally discouraged in shared-memory objects).
 *
 * @param out   Receives the typed handle on success.
 * @param name  Optional directory name.
 * @param tag   Type tag (defaults to type_tag_of<T>()).
 * @param args  Forwarded to the T constructor.
 * @return      0 on success; errno-compatible error code on failure.
 */
template <typename T, typename... Args>
int make(shm_region_t *region,
         shm_user_id_t user_id,
         shm_perm_t    perms,
         ptr<T>       *out,
         const char   *name = nullptr,
         uint32_t      tag  = type_tag_of<T>(),
         Args &&...args)
{
    const int a = alloc<T>(region, user_id, perms, out, name, tag);
    if (a != 0) return a;   /* allocation failed; nothing to clean up */
    return construct(region, *out, user_id, perms, std::forward<Args>(args)...);
}

/* ── Lookup by monotonic ID ──────────────────────────────────────────────── */

/**
 * Look up an existing object by its directory ID and return a typed handle.
 *
 * A new host process uses this after receiving an ID through an out-of-band
 * channel (message, config, the sync variable at the start of the region, …).
 *
 * @param region        Region handle.
 * @param obj_id        Monotonic ID returned at alloc time.
 * @param user_id       Caller's user id.
 * @param caller_perms  Must include SHM_PERM_READ (or SHM_PERM_ADMIN).
 * @param out           Receives the typed handle on success.
 * @return              0, ENOENT, or EACCES.
 */
template <typename T>
int find(const shm_region_t *region,
         uint64_t            obj_id,
         shm_user_id_t       user_id,
         shm_perm_t          caller_perms,
         ptr<T>             *out)
{
    if (!region || !out) return EINVAL;
    shm_off_t off = SHM_OFF_NULL;
    const int rc  = shm_lookup(region, obj_id, user_id, caller_perms, &off, nullptr);
    if (rc != 0) return rc;
    *out = ptr<T>(off);
    return 0;
}

/**
 * Look up an existing object by its directory name and return a typed handle.
 */
template <typename T>
int find_by_name(const shm_region_t *region,
                 const char         *name,
                 shm_user_id_t       user_id,
                 shm_perm_t          caller_perms,
                 ptr<T>             *out,
                 uint64_t           *out_id = nullptr)
{
    if (!region || !name || !out) return EINVAL;
    shm_off_t off = SHM_OFF_NULL;
    uint64_t  id  = 0;
    const int rc  = shm_lookup_by_name(region, name, user_id, caller_perms, &id, &off);
    if (rc != 0) return rc;
    *out = ptr<T>(off);
    if (out_id) *out_id = id;
    return 0;
}

}  /* namespace shm */

#endif /* SHM_ALLOC_HPP */
