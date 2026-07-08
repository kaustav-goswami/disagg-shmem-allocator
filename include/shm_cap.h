/**
 * @file  shm_cap.h
 * @brief CHERI-like 128-bit software capability for shared / disaggregated memory.
 *
 * Overview
 * ────────
 * A shm_cap_t is a 128-bit value composed of two 64-bit words:
 *
 *   ┌──────────────────────────────────────────────────────────────────────┐
 *   │  word 0 — addr (64-bit)                                              │
 *   │           heap-relative payload offset (identical to shm_off_t)      │
 *   ├──────────────────────────────────────────────────────────────────────┤
 *   │  word 1 — meta (64-bit), packed from block-header fields at          │
 *   │           shm_cap_derive() time:                                      │
 *   │                                                                      │
 *   │  bits [63:56]  perms     (8 b)  sealed shm_perm_t                   │
 *   │  bits [55:48]  owner_lo  (8 b)  low byte of shm_user_id_t           │
 *   │  bits [47:32]  epoch     (16 b) low 16 bits of block's obj_id       │
 *   │  bits [31: 0]  bounds    (32 b) payload_cap at derive time          │
 *   └──────────────────────────────────────────────────────────────────────┘
 *
 * The **tag bit** (CHERI equivalent) is NOT stored in the capability itself.
 * It lives in the heap block header as `shm_block_hdr_t::allocated`.  Every
 * call to shm_cap_deref() re-reads the live header and checks this bit — a
 * freed block immediately invalidates all outstanding capabilities pointing
 * to it, with no capability revocation scan required.
 *
 * Validation performed by shm_cap_deref()
 * ─────────────────────────────────────────
 *   1. Non-null:         addr != 0 || meta != 0
 *   2. Magic check:      block->magic == SHM_ALLOC_MAGIC_BLOCK
 *   3. Tag bit (liveness): block->allocated == 1
 *   4. Epoch (ABA):      (block->obj_id & 0xFFFF) == epoch-field-in-meta
 *   5. Bounds:           block->payload_cap == bounds-field-in-meta
 *   6. Capability perms: (cap_perms & required) == required
 *   7. Block perms:      (block->perms & required) == required
 *
 * Capability narrowing
 * ─────────────────────
 * shm_cap_narrow() produces a new capability with fewer permission bits.
 * Adding bits is impossible (returns SHM_CAP_NULL).  This mirrors CHERI's
 * capability monotonicity invariant.
 *
 * Optional use
 * ─────────────
 * The capability layer is entirely additive.  Existing code that uses
 * shm_off_t, shm_ptr(), and shm_cast*() continues to work unchanged on the
 * same region.  shm_cap_derive() creates a capability from any valid
 * shm_off_t; the two representations coexist.
 */

#ifndef SHM_CAP_H
#define SHM_CAP_H

#include "shm_alloc.h"   /* shm_region_t, shm_off_t, shm_perm_t, etc. */

#include <stdbool.h>     /* bool */
#include <stddef.h>      /* size_t */
#include <stdint.h>      /* uint8_t, uint16_t, uint32_t, uint64_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 128-bit capability type ─────────────────────────────────────────────── */

/**
 * A 128-bit software capability.
 *
 * Safe to store in shared memory (plain integers, no pointers).
 * May be freely copied between processes — the tag bit lives in the
 * heap block header, not here, so a copy cannot outlive its source.
 */
typedef struct shm_cap {
    uint64_t addr;   /**< word 0: heap-relative payload offset               */
    uint64_t meta;   /**< word 1: sealed bounds + perms + epoch (see above)  */
} shm_cap_t;

/** The null capability: both words zero.  All validation calls reject it. */
#define SHM_CAP_NULL  ((shm_cap_t){ 0u, 0u })

/* ── meta word bit-field positions (for documentation / advanced use) ────── */

#define SHM_CAP_META_PERMS_SHIFT    56u   /**< bits [63:56] — permission mask */
#define SHM_CAP_META_OWNER_SHIFT    48u   /**< bits [55:48] — owner low byte  */
#define SHM_CAP_META_EPOCH_SHIFT    32u   /**< bits [47:32] — ABA epoch       */
#define SHM_CAP_META_BOUNDS_SHIFT    0u   /**< bits [31: 0] — payload_cap     */

#define SHM_CAP_META_PERMS_MASK  (0xFFULL    << SHM_CAP_META_PERMS_SHIFT)
#define SHM_CAP_META_OWNER_MASK  (0xFFULL    << SHM_CAP_META_OWNER_SHIFT)
#define SHM_CAP_META_EPOCH_MASK  (0xFFFFULL  << SHM_CAP_META_EPOCH_SHIFT)
#define SHM_CAP_META_BOUNDS_MASK (0xFFFFFFFFULL)

/* ── Null / validity ─────────────────────────────────────────────────────── */

/** Return true if @p cap is the null capability (both words zero). */
static inline bool shm_cap_is_null(shm_cap_t cap)
{
    return cap.addr == 0u && cap.meta == 0u;
}

/* ── Derive ──────────────────────────────────────────────────────────────── */

/**
 * @brief Create a capability from an existing allocation.
 *
 * Reads the block header at @p off and packs its security fields into the
 * 128-bit capability.  The tag bit (block->allocated) is NOT copied into the
 * capability; it is always read live from the header on every deref call.
 *
 * Permission narrowing: the capability seals only the bits in
 * (@p block->perms & @p caller_perms & @p grant_perms).  If grant_perms
 * contains a bit that is not in block->perms or caller_perms, the derive
 * fails and SHM_CAP_NULL is returned.
 *
 * @param region        Region handle.
 * @param off           Heap-relative payload offset returned by shm_alloc().
 * @param user_id       Caller's user id (must match block owner or be ADMIN).
 * @param caller_perms  Caller's own permission bits.
 * @param grant_perms   Permission bits to seal into the capability (⊆ intersection).
 * @return              The new capability, or SHM_CAP_NULL on any failure.
 */
shm_cap_t shm_cap_derive(const shm_region_t *region,
                          shm_off_t           off,
                          shm_user_id_t       user_id,
                          shm_perm_t          caller_perms,
                          shm_perm_t          grant_perms);

/* ── Dereference ─────────────────────────────────────────────────────────── */

/**
 * @brief Validate a capability and return a pointer to its payload.
 *
 * Performs all seven checks listed in the file header comment in order.
 * If any check fails, NULL is returned and the caller can inspect errno
 * for the reason:
 *   EINVAL  — null cap, corrupt magic, or ABA / bounds mismatch
 *   EACCES  — tag bit clear (block freed) or insufficient permissions
 *
 * @param region          Region handle (provides the heap base address).
 * @param cap             The capability to dereference.
 * @param required_perms  Bits the capability must include for this access.
 * @return                Non-NULL payload pointer on success; NULL on failure.
 */
void *shm_cap_deref(const shm_region_t *region,
                    shm_cap_t           cap,
                    shm_perm_t          required_perms);

/* ── Narrowing ───────────────────────────────────────────────────────────── */

/**
 * @brief Produce a new capability with a subset of the current permissions.
 *
 * Implements CHERI-style monotonic permission narrowing: bits can only be
 * removed, never added.  If @p new_perms contains any bit absent from the
 * current capability, SHM_CAP_NULL is returned.
 *
 * @param cap       Source capability.
 * @param new_perms Desired permission mask (must be ⊆ shm_cap_perms(cap)).
 * @return          Narrowed capability, or SHM_CAP_NULL if widening attempted.
 */
shm_cap_t shm_cap_narrow(shm_cap_t cap, shm_perm_t new_perms);

/* ── Introspection ───────────────────────────────────────────────────────── */

/** Return true if the capability passes basic header validation (no perm check). */
bool shm_cap_is_valid(const shm_region_t *region, shm_cap_t cap);

/** Extract the permission bits sealed in this capability. */
shm_perm_t shm_cap_perms(shm_cap_t cap);

/** Extract the payload capacity (bounds) sealed in this capability. */
size_t shm_cap_bounds(shm_cap_t cap);

/** Extract the heap-relative payload offset from this capability. */
shm_off_t shm_cap_offset(shm_cap_t cap);

/** Extract the ABA epoch sealed in this capability (low 16 bits of obj_id). */
uint16_t shm_cap_epoch(shm_cap_t cap);

/* ── Typed C macros ──────────────────────────────────────────────────────── */

/**
 * Dereference a capability and cast to @p type * in one step.
 * Returns NULL on any validation failure (no exception / abort).
 */
#define shm_cap_deref_as(region, cap, type, required_perms) \
    ((type *)shm_cap_deref((region), (cap), (required_perms)))

/** Dereference for read-only access (requires SHM_PERM_READ). */
#define shm_cap_ro(region, cap, type) \
    ((const type *)shm_cap_deref((region), (cap), SHM_PERM_READ))

/** Dereference for read-write access (requires SHM_PERM_READ | SHM_PERM_WRITE). */
#define shm_cap_rw(region, cap, type) \
    ((type *)shm_cap_deref((region), (cap), \
                            (shm_perm_t)(SHM_PERM_READ | SHM_PERM_WRITE)))

/**
 * Derive a capability for type @p type from a payload offset.
 * Grants SHM_PERM_DEFAULT unless @p grant_perms is specified explicitly.
 */
#define shm_cap_derive_typed(region, off, type, user_id, caller_perms, grant_perms) \
    shm_cap_derive((region), (off), (user_id), (caller_perms), (grant_perms))

#ifdef __cplusplus
}
#endif

#endif /* SHM_CAP_H */
