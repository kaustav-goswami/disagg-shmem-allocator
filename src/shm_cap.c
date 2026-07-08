/**
 * @file  shm_cap.c
 * @brief CHERI-like 128-bit software capability implementation.
 *
 * Depends on shm_internal.h for direct access to shm_block_hdr_t fields;
 * this allows the capability checks to read the live block header on every
 * deref without going through the public shm_block_info() API, which would
 * require a separate lock acquisition.
 *
 * Meta word layout (recap, see shm_cap.h for the authoritative description):
 *
 *   bits [63:56]  perms     (8b)   — sealed shm_perm_t permission mask
 *   bits [55:48]  owner_lo  (8b)   — low byte of shm_user_id_t
 *   bits [47:32]  epoch     (16b)  — low 16 bits of block->obj_id at derive time
 *   bits [31: 0]  bounds    (32b)  — block->payload_cap at derive time
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_cap.h"       /* public capability API */
#include "shm_internal.h"  /* shm_block_hdr_t, shm_blk_of_poff, etc. */

#include <errno.h>         /* EINVAL, EACCES */
#include <stddef.h>        /* size_t, NULL */
#include <stdint.h>        /* uint8_t, uint16_t, uint32_t, uint64_t */

/* ── Internal: pack / unpack the meta word ───────────────────────────────── */

/**
 * Pack capability metadata into the 64-bit meta word.
 *
 * @param perms     shm_perm_t bits to seal (only low 8 bits used).
 * @param owner_lo  Low byte of the owning user id.
 * @param epoch     Low 16 bits of the block's obj_id (anti-ABA token).
 * @param bounds    payload_cap at derive time (32-bit; up to 4 GB objects).
 */
static uint64_t cap_meta_pack(uint8_t  perms,
                               uint8_t  owner_lo,
                               uint16_t epoch,
                               uint32_t bounds)
{
    return ((uint64_t)perms    << SHM_CAP_META_PERMS_SHIFT)   /* bits [63:56] */
         | ((uint64_t)owner_lo << SHM_CAP_META_OWNER_SHIFT)   /* bits [55:48] */
         | ((uint64_t)epoch    << SHM_CAP_META_EPOCH_SHIFT)   /* bits [47:32] */
         | ((uint64_t)bounds   << SHM_CAP_META_BOUNDS_SHIFT); /* bits [31: 0] */
}

/** Extract the permission byte from a meta word. */
static uint8_t  meta_perms(uint64_t meta)
{
    return (uint8_t)((meta & SHM_CAP_META_PERMS_MASK) >> SHM_CAP_META_PERMS_SHIFT);
}

/** Extract the epoch (ABA token) from a meta word. */
static uint16_t meta_epoch(uint64_t meta)
{
    return (uint16_t)((meta & SHM_CAP_META_EPOCH_MASK) >> SHM_CAP_META_EPOCH_SHIFT);
}

/** Extract the bounds (payload_cap) from a meta word. */
static uint32_t meta_bounds(uint64_t meta)
{
    return (uint32_t)(meta & SHM_CAP_META_BOUNDS_MASK);
}

/* ── Public API implementation ───────────────────────────────────────────── */

shm_cap_t shm_cap_derive(const shm_region_t *region,
                          shm_off_t           off,
                          shm_user_id_t       user_id,
                          shm_perm_t          caller_perms,
                          shm_perm_t          grant_perms)
{
    if (!region)
        return SHM_CAP_NULL;   /* null region is always an error */

    /* Retrieve the live block header for this offset. */
    shm_block_hdr_t *b = shm_blk_of_poff(region, off);

    /* Validate the block header magic before reading any other field. */
    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK)
        return SHM_CAP_NULL;   /* corrupt or invalid offset */

    /* The tag bit must be set: the block must be live, not freed. */
    if (!b->allocated)
        return SHM_CAP_NULL;   /* use-after-free: block is on the free list */

    /* Ownership check: caller must be the block owner or hold ADMIN. */
    if (!(caller_perms & SHM_PERM_ADMIN) && user_id != b->owner)
        return SHM_CAP_NULL;   /* caller does not own this block */

    /* Permission narrowing: grant_perms must be a subset of what is available.
     * "Available" = intersection of (block's stored perms) and (caller's perms). */
    shm_perm_t available = (shm_perm_t)(b->perms & caller_perms);
    if ((grant_perms & available) != grant_perms)
        return SHM_CAP_NULL;   /* caller is requesting perms that are not available */

    /* Clamp bounds to 32 bits.  Objects larger than 4 GB cannot be represented
     * in the current meta word layout; return null rather than silently truncate. */
    if (b->payload_cap > 0xFFFFFFFFu)
        return SHM_CAP_NULL;   /* object too large for 32-bit bounds field */

    /* Pack the meta word from live block-header fields. */
    uint64_t meta = cap_meta_pack(
        (uint8_t)(grant_perms & 0xFF),   /* only the low 8 perm bits are stored */
        (uint8_t)(b->owner    & 0xFF),   /* low byte of owner id */
        (uint16_t)(b->obj_id  & 0xFFFF), /* low 16 bits of obj_id = epoch token */
        (uint32_t)(b->payload_cap)       /* payload capacity = bounds */
    );

    shm_cap_t cap;
    cap.addr = off;    /* word 0: the heap-relative payload offset */
    cap.meta = meta;   /* word 1: sealed security metadata */
    return cap;
}

void *shm_cap_deref(const shm_region_t *region,
                    shm_cap_t           cap,
                    shm_perm_t          required_perms)
{
    /* Check 1: null capability is always invalid. */
    if (shm_cap_is_null(cap))
        return NULL;

    if (!region)
        return NULL;

    /* Load the live block header for this capability's offset. */
    shm_block_hdr_t *b = shm_blk_of_poff(region, cap.addr);

    /* Check 2: magic must match — detects corruption or a fabricated address. */
    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK)
        return NULL;

    /* Check 3: tag bit — the block must be live (allocated == 1).
     * A freed block immediately invalidates all capabilities pointing to it. */
    if (!b->allocated)
        return NULL;

    /* Check 4: epoch (ABA protection).
     * If the block was freed and a new object was allocated at the same heap
     * offset, the new allocation gets a new obj_id.  Its low 16 bits (epoch)
     * will differ from the epoch sealed in this capability, catching the ABA. */
    uint16_t live_epoch = (uint16_t)(b->obj_id & 0xFFFF);
    if (live_epoch != meta_epoch(cap.meta))
        return NULL;

    /* Check 5: bounds.
     * The block's current payload_cap must match what was sealed at derive
     * time.  A resize that changes capacity invalidates capabilities derived
     * before the resize (they should be re-derived after resize). */
    if ((uint32_t)b->payload_cap != meta_bounds(cap.meta))
        return NULL;

    /* Check 6: capability permission bits must include all required bits.
     * This is the "sealed perms" check — only the bits that were granted at
     * derive time can ever be used through this capability. */
    shm_perm_t cap_perms = (shm_perm_t)meta_perms(cap.meta);
    if ((cap_perms & required_perms) != required_perms)
        return NULL;

    /* Check 7: live block permission bits must still include all required bits.
     * A block's perms can be narrowed after a capability is derived via
     * shm_block_set_perms(); this check catches that. */
    if ((b->perms & required_perms) != required_perms)
        return NULL;

    /* All checks passed: return the payload pointer. */
    return (char *)b + sizeof(shm_block_hdr_t);
}

shm_cap_t shm_cap_narrow(shm_cap_t cap, shm_perm_t new_perms)
{
    if (shm_cap_is_null(cap))
        return SHM_CAP_NULL;   /* cannot narrow a null capability */

    shm_perm_t current = (shm_perm_t)meta_perms(cap.meta);

    /* Monotonicity: new_perms must be a strict subset of current perms. */
    if ((new_perms & current) != new_perms)
        return SHM_CAP_NULL;   /* attempted widening — refuse */

    /* Rebuild the meta word with the reduced permission bits. */
    uint64_t new_meta = (cap.meta & ~SHM_CAP_META_PERMS_MASK)
                      | ((uint64_t)(new_perms & 0xFF) << SHM_CAP_META_PERMS_SHIFT);

    shm_cap_t narrowed;
    narrowed.addr = cap.addr;
    narrowed.meta = new_meta;
    return narrowed;
}

bool shm_cap_is_valid(const shm_region_t *region, shm_cap_t cap)
{
    /* Perform structural validation only — does not check permissions. */
    if (shm_cap_is_null(cap) || !region)
        return false;

    shm_block_hdr_t *b = shm_blk_of_poff(region, cap.addr);

    if (!b || b->magic != SHM_ALLOC_MAGIC_BLOCK)
        return false;

    if (!b->allocated)
        return false;   /* tag bit cleared: block has been freed */

    if ((uint16_t)(b->obj_id & 0xFFFF) != meta_epoch(cap.meta))
        return false;   /* ABA: same address, different object */

    if ((uint32_t)b->payload_cap != meta_bounds(cap.meta))
        return false;   /* bounds mismatch (e.g. resize occurred) */

    return true;
}

shm_perm_t shm_cap_perms(shm_cap_t cap)
{
    return (shm_perm_t)meta_perms(cap.meta);
}

size_t shm_cap_bounds(shm_cap_t cap)
{
    return (size_t)meta_bounds(cap.meta);
}

shm_off_t shm_cap_offset(shm_cap_t cap)
{
    return cap.addr;
}

uint16_t shm_cap_epoch(shm_cap_t cap)
{
    return meta_epoch(cap.meta);
}
