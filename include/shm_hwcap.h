/**
 * @file  shm_hwcap.h
 * @brief Optional bridge between shm_alloc's software capabilities
 *        (shm_cap_t, see shm_cap.h) and a hardware capability/token unit —
 *        concretely, the CapCXL Capability Extension Unit (CEU) that sits on
 *        the CXL Type2 HDM path of the CapCXL FPGA project.
 *
 * Why this layer exists
 * ──────────────────────
 * shm_cap_t is checked entirely in software: every shm_cap_deref() re-reads
 * the live block header and re-validates tag/epoch/bounds/perms. That is
 * sufficient when the only path to the payload is through this library, but
 * CapCXL's Type2 path additionally enforces a *hardware* token check on
 * every AXI access to CXL HDM memory, independent of whatever userspace
 * code is doing. shm_hwcap.h lets a capability optionally carry a hardware
 * token alongside its software half, so both layers are checked and a
 * dereference only succeeds if hardware and software agree.
 *
 * This layer is entirely additive and optional, exactly like shm_cap.h is
 * additive over shm_off_t: with no backend registered, shm_hwcap_derive()
 * and shm_hwcap_deref() behave as thin pass-throughs to shm_cap_derive() /
 * shm_cap_deref() and hw_token is always SHM_HWCAP_TOKEN_NONE. Code that
 * links this file but never calls shm_hwcap_set_backend() pays no behavior
 * change at all.
 *
 * Pluggable backend
 * ──────────────────
 * shm_hwcap.h does not talk to any hardware or to CapCXL's MMIO runtime
 * directly — it has no dependency on that repository. Instead it defines a
 * small ops-table (shm_hwcap_ops_t) that a real adapter implements on top of
 * CapCXL's __capcxl_register_region() / CEU MMIO doorbell, and that tests
 * (or any environment without the FPGA present) can satisfy with the
 * software CEU emulator in shm_hwcap_mock.h. See shm_hwcap_mock.h for that
 * emulator, and the CapCXL adapter contract in this file's
 * "Writing a real adapter" section.
 *
 * Permission mapping — read this before touching perms
 * ──────────────────────────────────────────────────────
 * shm_perm_t (SHM_PERM_READ=0x01, WRITE=0x02, FREE=0x04, RESIZE=0x08,
 * ADMIN=0x80) and CapCXL's hardware permission byte (READ=0x1, WRITE=0x2,
 * EXEC=0x4) are DIFFERENT bit-assignments that happen to collide at 0x04:
 * in shm_perm_t that bit means "may free this object"; on the hardware side
 * the same bit means "may execute this memory as code". A raw integer cast
 * between the two would silently grant execute permission to any capability
 * that merely grants FREE. shm_perm_to_hwcap() / shm_hwcap_to_perm() exist
 * so nobody ever needs to do that cast by hand; always go through them.
 *
 * Process-local, not shared-memory state
 * ─────────────────────────────────────────
 * Unlike shm_cap_t (safe to store in shared memory and hand to another
 * host), shm_hwcap_t::hw_token names an entry in ONE host's hardware token
 * table (or, for the mock backend, one process's simulated table). Never
 * write a shm_hwcap_t into the shared region or send it to another host —
 * store/exchange the plain shm_off_t or shm_cap_t instead, and have each
 * host derive its own hardware token locally if it has a backend bound.
 *
 * Writing a real adapter
 * ────────────────────────
 * A CapCXL-side adapter implements shm_hwcap_ops_t roughly as:
 *
 *   register_region(ctx, ptr, size, perms, &token)
 *       -> __capcxl_register_region(ptr, size, perms) [capcxl_runtime.h],
 *          which internally issues CEU_CMD_GEN_TOKEN over MMIO and returns
 *          the LAM-encoded pointer; derive the token from that pointer via
 *          CAPCXL_LAM_MASK / CAPCXL_LAM_BIT_LO and hand it back as *token.
 *   revoke_region(ctx, token)
 *       -> issue CEU_CMD_REVOKE for that token (MMIO_CEU_CMD/TOKEN_IN).
 *   verify_region(ctx, token, ptr, required_perms)
 *       -> issue CEU_CMD_VERIFY, or simply trust the hardware's own AXI-path
 *          enforcement and return 0 (the hardware already faults illegal
 *          accesses; this hook exists for backends that only offer a
 *          software-visible status register).
 *
 * That adapter is out of scope for this repository (it depends on
 * CapCXL's runtime headers) — see the CapCXL integration section of the
 * top-level README for the exact wiring.
 */

#ifndef SHM_HWCAP_H
#define SHM_HWCAP_H

#include "shm_alloc.h"   /* shm_region_t, shm_off_t, shm_perm_t, shm_user_id_t */
#include "shm_cap.h"     /* shm_cap_t and the software capability layer */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Hardware permission bits (independent numbering from shm_perm_t) ────── */

#define SHM_HWCAP_PERM_READ   0x1u
#define SHM_HWCAP_PERM_WRITE  0x2u
#define SHM_HWCAP_PERM_EXEC   0x4u   /* no shm_perm_t equivalent; never set by shm_perm_to_hwcap() */

typedef uint8_t shm_hwcap_perm_t;

/**
 * Translate a shm_perm_t mask to the hardware permission byte.
 * Only READ and WRITE carry across; FREE, RESIZE and ADMIN have no hardware
 * meaning and are dropped (NOT reinterpreted as EXEC — see file header).
 */
static inline shm_hwcap_perm_t shm_perm_to_hwcap(shm_perm_t p)
{
    shm_hwcap_perm_t hw = 0;
    if (p & SHM_PERM_READ)  hw |= SHM_HWCAP_PERM_READ;
    if (p & SHM_PERM_WRITE) hw |= SHM_HWCAP_PERM_WRITE;
    return hw;
}

/**
 * Translate a hardware permission byte back to a shm_perm_t mask.
 * EXEC has no shm_perm_t equivalent and is dropped.
 */
static inline shm_perm_t shm_hwcap_to_perm(shm_hwcap_perm_t hw)
{
    shm_perm_t p = SHM_PERM_NONE;
    if (hw & SHM_HWCAP_PERM_READ)  p |= SHM_PERM_READ;
    if (hw & SHM_HWCAP_PERM_WRITE) p |= SHM_PERM_WRITE;
    return p;
}

/* ── Backend ops table ────────────────────────────────────────────────────── */

/**
 * Backend implementing the actual hardware (or emulated) capability unit.
 * All three callbacks receive the opaque @c ctx pointer supplied at
 * shm_hwcap_set_backend() time. Any callback may be NULL, in which case the
 * corresponding shm_hwcap_* operation is skipped (treated as "unsupported by
 * this backend", not as an error).
 */
typedef struct shm_hwcap_ops {
    void *ctx;

    /**
     * Register [ptr, ptr+size) with the backend and hand back an opaque
     * token identifying the registration.
     * @return 0 on success (*out_token valid); ENOSPC if the backend's
     *         token table is full; other errno-style codes on failure.
     */
    int (*register_region)(void *ctx, void *ptr, size_t size,
                            shm_hwcap_perm_t perms, uint32_t *out_token);

    /** Revoke a previously registered token. 0 on success, EINVAL if unknown. */
    int (*revoke_region)(void *ctx, uint32_t token);

    /**
     * Re-verify that @p token still authorizes @p required_perms access to
     * @p ptr. 0 if allowed; EACCES/EFAULT/EINVAL on any failure. May be NULL
     * if the backend only enforces access on the hardware data path itself
     * and offers no separate software-visible check.
     */
    int (*verify_region)(void *ctx, uint32_t token, const void *ptr,
                         shm_hwcap_perm_t required_perms);
} shm_hwcap_ops_t;

/**
 * @brief Install the process-local hardware-capability backend.
 *
 * Also registers this module's internal free hook via shm_set_free_hook()
 * so that shm_free() automatically revokes any hardware tokens bound to the
 * object being freed. Calling this again replaces both the ops table and
 * the previous token-tracking state.
 *
 * @param ops  Copied by value; must remain valid (ctx and callbacks) for as
 *             long as the backend stays installed.
 */
void shm_hwcap_set_backend(const shm_hwcap_ops_t *ops);

/**
 * @brief Remove the process-local hardware-capability backend.
 *
 * After this call shm_hwcap_derive()/shm_hwcap_deref() behave as pure
 * software pass-throughs (as if no backend had ever been installed).
 * Does not revoke any still-outstanding hardware tokens; call
 * shm_hwcap_revoke() on them first if that matters for your backend.
 */
void shm_hwcap_clear_backend(void);

/** True if a backend is currently installed. */
bool shm_hwcap_backend_bound(void);

/* ── Bridged capability type ─────────────────────────────────────────────── */

/** Sentinel meaning "no hardware token bound to this capability". */
#define SHM_HWCAP_TOKEN_NONE  UINT32_MAX

/**
 * A software capability optionally paired with a hardware token.
 *
 * Process-local only — see the file header's "Process-local, not
 * shared-memory state" note before storing or transmitting one of these.
 */
typedef struct shm_hwcap {
    shm_cap_t sw;         /**< underlying software capability (shm_cap.h)   */
    uint32_t  hw_token;   /**< opaque hw token, or SHM_HWCAP_TOKEN_NONE     */
} shm_hwcap_t;

/**
 * @brief Derive a bridged capability from an existing allocation.
 *
 * Always performs the full software derivation (shm_cap_derive()); if that
 * fails, returns a capability whose .sw is SHM_CAP_NULL and whose hw_token
 * is SHM_HWCAP_TOKEN_NONE (mirrors shm_cap_derive()'s failure contract).
 *
 * If a backend is installed, additionally asks it to register the payload
 * range and grant_perms (translated via shm_perm_to_hwcap()). If hardware
 * registration fails for any reason (including a full token table), this
 * function does NOT fail the overall derive — it degrades gracefully to a
 * software-only capability (hw_token == SHM_HWCAP_TOKEN_NONE), consistent
 * with this layer being optional and additive.
 */
shm_hwcap_t shm_hwcap_derive(const shm_region_t *region,
                             shm_off_t           off,
                             shm_user_id_t       user_id,
                             shm_perm_t          caller_perms,
                             shm_perm_t          grant_perms);

/**
 * @brief Validate a bridged capability and return a pointer to its payload.
 *
 * First runs shm_cap_deref() on cap.sw; if that fails, returns NULL
 * immediately (errno set by shm_cap_deref()'s convention). If it succeeds
 * and cap.hw_token != SHM_HWCAP_TOKEN_NONE and a backend with a
 * verify_region callback is installed, additionally asks the backend to
 * re-verify required_perms against the live token; a hardware denial fails
 * the whole dereference (errno = EACCES) even though the software checks
 * passed. If no backend is installed, or the capability was derived without
 * a hardware token, only the software checks apply.
 */
void *shm_hwcap_deref(const shm_region_t *region,
                      shm_hwcap_t         cap,
                      shm_perm_t          required_perms);

/**
 * @brief Explicitly revoke the hardware token bound to @p cap, if any.
 *
 * Does not touch the software half or free the underlying object — use
 * shm_free() for that (which revokes automatically via the free hook).
 * This is for revoking hardware access ahead of a free, e.g. to temporarily
 * degrade a capability to software-only enforcement.
 *
 * @return 0 on success (cap->hw_token is reset to SHM_HWCAP_TOKEN_NONE);
 *         EINVAL if cap already has no token; ENOSYS if no backend with a
 *         revoke_region callback is installed; the backend's own error
 *         code otherwise.
 */
int shm_hwcap_revoke(shm_hwcap_t *cap);

/** True if @p cap carries a live hardware token (not just a software cap). */
static inline bool shm_hwcap_is_bound(shm_hwcap_t cap)
{
    return cap.hw_token != SHM_HWCAP_TOKEN_NONE;
}

#ifdef __cplusplus
}
#endif

#endif /* SHM_HWCAP_H */
