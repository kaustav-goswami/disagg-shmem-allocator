/**
 * @file  shm_cap_side.h
 * @brief Variant (a) of the slimmed capability design: side metadata.
 *
 * shm_cap_side_t is a 256-bit (32 B) value stored *next to* a pointer, not
 * merged into it (contrast with the unimplemented shm_cap_fat_t in
 * shm_cap_variants.h). It is software-only — no hardware dependency, works
 * on any host/compiler — and is the reference implementation of the same
 * bearer-key, rotation-based revocation model the CapCXL CEU realizes in
 * hardware via shm_hwcap.h / shm_hwcap_mock.h.
 *
 * Bearer-key model, now identity-scoped for mutation
 * ──────────────────────────────────────────────────────
 * Ordinary access (shm_cap_side_deref()) stays pure bearer: possession of
 * the key is both necessary and sufficient to read/write through it, with
 * no identity check at all. That part is unchanged from the original
 * design and matches the rotating-key hardware model, where "whoever has
 * the key has access."
 *
 * Mutation (shm_cap_side_rotate() / shm_cap_side_free()) is no longer
 * possession-only. Each row now also records the `identity` of whoever
 * minted it, and rotate/free require the caller to present a matching
 * identity *in addition to* the key — presenting the key alone is no
 * longer sufficient to revoke or retire a capability. This reconciles with
 * the owner-gated GRANT/WITHDRAW model in CapCXL's capcxl_context_grants.sv
 * (an independently-built subsystem — see the project's capability design
 * discussion): there, only a record's `owner` may withdraw or destroy it,
 * while any granted recipient may still read/write it. shm_cap_side_t now
 * mirrors that split — identity gates *mutation*, not *use* — rather than
 * the fuller multi-recipient read/write mask system that RTL implements,
 * which is a larger, separately-scoped follow-up, not part of this change.
 *
 * Revocation
 * ───────────
 * Revoking access means rotating the key: shm_cap_side_rotate() overwrites
 * the allocation's current key with a fresh one. Every shm_cap_side_t still
 * carrying the old key value immediately and permanently stops matching —
 * there is no separate "revoked" flag to check, no cache to go stale, and
 * no sweep. This mirrors exactly how the CapCXL CEU's content-addressed
 * (CAM) table realizes revocation in hardware.
 *
 * Where the key table lives
 * ───────────────────────────
 * This reference implementation keeps its key table PROCESS-LOCAL (a static
 * array in shm_cap_side.c), the same way shm_hwcap.c's own token-tracking
 * table is process-local. That is sufficient to exercise and test the
 * mechanism (mint → access → rotate → old key fails) within one process,
 * but it does NOT give multi-host visibility the way the shared region
 * itself does for shm_cap_t, or the way a real device-side table does for
 * the LAM/hardware path (variant c). A deployment that needs the key table
 * itself to be authoritative across hosts should use variant (c); this
 * file is deliberately scoped to the software-only, single-process case.
 *
 * Allocation granularity
 * ────────────────────────
 * One key per allocation, for the allocation's entire lifetime (a resize
 * updates `bound` in place under the same key; it does not require a
 * rotation). There is no delegation and no sub-allocation capability —
 * consistent with the project's "allocation," not "object," terminology.
 */

#ifndef SHM_CAP_SIDE_H
#define SHM_CAP_SIDE_H

#include "shm_alloc.h"   /* shm_region_t, shm_off_t, shm_perm_t */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 256-bit side-metadata capability ────────────────────────────────────── */

/**
 * A software capability stored as side data. Exactly 256 bits (32 B).
 *
 *   addr        : heap-relative payload offset (== base at derive time;
 *                 present separately so the struct's shape mirrors the
 *                 fat-pointer sketch in shm_cap_variants.h).
 *   base        : heap-relative base of the allocation.
 *   bound       : heap-relative exclusive upper bound (base + extent).
 *   identity    : the minting caller's identity (opaque uint32_t; caller
 *                 defines the numbering, same convention as shm_user_id_t).
 *                 Checked only by rotate()/free(), never by deref() -- and
 *                 even there, only as a caller-supplied *parameter* compared
 *                 against the row's internally-stored owner, never trusted
 *                 by reading it back out of a presented capability value
 *                 (that would be trivially forgeable by any local holder).
 *                 This field also replaces what used to be a separate
 *                 `capability` (allocation-id) field -- see the ABA note
 *                 below, which that field used to close.
 *   key_perms   : packed — key in bits [31:2], perms (R/W only) in bits [1:0].
 *                 Packed via explicit shift/mask (SHM_CAP_SIDE_*), not a C
 *                 bitfield, for the same portability reason shm_cap.h's
 *                 meta word is hand-packed.
 *
 * ABA note (regression, not an oversight -- read before relying on this)
 * ──────────────────────────────────────────────────────────────────────
 * The struct used to also carry `capability` (the allocation's monotonic
 * obj_id, which never repeats), checked alongside the key on every lookup.
 * That gave ABA protection: even if a 30-bit key value were ever reused for
 * a *different* allocation after the original was freed, a stale capability
 * from the old allocation couldn't accidentally match the new one, because
 * the obj_id differed. Dropping that field to hit exactly 256 bits removes
 * that protection -- find_by_key() now matches on key alone. The key space
 * is only 2^30; by the birthday bound, a collision between a freed key and
 * a newly-minted one becomes plausible after roughly 2^15 (~33K) mint/free
 * cycles, at which point a long-held stale shm_cap_side_t could, in
 * principle, start matching a live but unrelated allocation. Flag this
 * explicitly if this struct's numbers go into the paper as revocation-safe
 * without qualification.
 */
typedef struct shm_cap_side {
    uint64_t addr;
    uint64_t base;
    uint64_t bound;
    uint32_t identity;
    uint32_t key_perms;
} shm_cap_side_t;

#define SHM_CAP_SIDE_NULL  ((shm_cap_side_t){ 0, 0, 0, 0, 0 })

#define SHM_CAP_SIDE_KEY_SHIFT    2u
#define SHM_CAP_SIDE_KEY_MASK     (0x3FFFFFFFu << SHM_CAP_SIDE_KEY_SHIFT)   /* 30 bits */
#define SHM_CAP_SIDE_PERMS_MASK   0x3u                                      /* 2 bits: R, W */

#define SHM_CAP_SIDE_PERM_READ    0x1u
#define SHM_CAP_SIDE_PERM_WRITE   0x2u

/** Return true if @p cap is the null capability. */
static inline bool shm_cap_side_is_null(shm_cap_side_t cap)
{
    return cap.addr == 0 && cap.base == 0 && cap.bound == 0 &&
           cap.identity == 0 && cap.key_perms == 0;
}

/** Extract the identity that minted this capability (for introspection). */
static inline uint32_t shm_cap_side_identity(shm_cap_side_t cap)
{
    return cap.identity;
}

/** Extract the 30-bit key from a capability's packed key_perms word. */
static inline uint32_t shm_cap_side_key(shm_cap_side_t cap)
{
    return (cap.key_perms & SHM_CAP_SIDE_KEY_MASK) >> SHM_CAP_SIDE_KEY_SHIFT;
}

/** Extract the 2-bit (R/W) permission mask from a capability. */
static inline uint32_t shm_cap_side_perms(shm_cap_side_t cap)
{
    return cap.key_perms & SHM_CAP_SIDE_PERMS_MASK;
}

/* ── Derive / dereference / rotate / free ────────────────────────────────── */

/**
 * @brief Mint a new side-metadata capability for a live allocation.
 *
 * Reads the allocation's live base/bound from its block header and commits
 * a fresh row — base, bound, a freshly generated nonzero key, the requested
 * permissions (R/W only; bits outside SHM_CAP_SIDE_PERM_* are dropped), and
 * @p identity as the row's recorded owner — into the process-local key
 * table. No check against @p identity at mint time (any caller may mint);
 * it only matters later, to shm_cap_side_rotate()/shm_cap_side_free().
 *
 * @return A valid capability, or SHM_CAP_SIDE_NULL if @p off does not name
 *         a live block, @p grant_perms is empty, or the key table is full.
 */
shm_cap_side_t shm_cap_side_derive(const shm_region_t *region,
                                   shm_off_t           off,
                                   shm_perm_t          grant_perms,
                                   uint32_t            identity);

/**
 * @brief Validate a capability and return a pointer to its payload.
 *
 * Content-addressed: looks up the row whose key matches shm_cap_side_key(cap)
 * alone (see the ABA note on shm_cap_side_t), independent of any identity
 * of the caller. Re-checks the row's base/bound against the
 * allocation's *live* block header (catches resize and free/reuse) before
 * returning a pointer. Fails if the key does not match any live row (never
 * minted, already rotated away, or already freed), if bounds no longer
 * match, or if @p required_perms is not a subset of the row's permissions.
 *
 * @return Non-NULL payload pointer on success; NULL on any check failure.
 */
void *shm_cap_side_deref(const shm_region_t *region,
                         shm_cap_side_t      cap,
                         shm_perm_t          required_perms);

/**
 * @brief Revoke @p cap by rotating its key.
 *
 * Looks up the row by @p cap's current key, then requires @p identity to
 * match the row's recorded owner — key possession alone is no longer
 * sufficient to rotate. On success, overwrites the row's key with a fresh
 * one and returns a new capability carrying that key (identity unchanged).
 * Every other shm_cap_side_t still holding the old key value fails
 * shm_cap_side_deref() from this point on, unconditionally.
 *
 * @return The new capability, or SHM_CAP_SIDE_NULL if @p cap's key does not
 *         match any live row, or @p identity does not match that row's
 *         recorded owner.
 */
shm_cap_side_t shm_cap_side_rotate(shm_cap_side_t cap, uint32_t identity);

/**
 * @brief Free the allocation's side-metadata row.
 *
 * Looks up the row by @p cap's key, requires @p identity to match the
 * row's recorded owner (same rule as rotate), and clears it. Does not free
 * the underlying shm_alloc() allocation itself — call shm_free() for that;
 * this only retires the capability-side bookkeeping. A second free on an
 * already-cleared key is rejected (the row no longer matches anything),
 * not a no-op success.
 *
 * @return 0 on success; ENOENT if @p cap's key does not match any live row;
 *         EACCES if it does but @p identity does not match the owner.
 */
int shm_cap_side_free(shm_cap_side_t cap, uint32_t identity);

#ifdef __cplusplus
}
#endif

#endif /* SHM_CAP_SIDE_H */
