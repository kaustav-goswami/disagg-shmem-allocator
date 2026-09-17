/**
 * @file  shm_hwcap_mock.h
 * @brief Software emulation of a CEU-like token unit, implementing
 *        shm_hwcap_ops_t (see shm_hwcap.h) entirely in userspace.
 *
 * This lets the hardware-capability bridge be exercised end-to-end —
 * register, verify, revoke, table exhaustion — without any CapCXL hardware
 * present. It is deliberately shaped after the real CEU: a fixed-size table
 * of (base, size, perms) slots addressed by a small integer token, mirroring
 * CapCXL's 64-entry shadow register file (see capcxl_pkg.sv,
 * SHADOW_REG_COUNT). A real CapCXL adapter and this mock are interchangeable
 * behind the same shm_hwcap_ops_t interface — swap one for the other by
 * changing which shm_hwcap_set_backend() call an application makes.
 */

#ifndef SHM_HWCAP_MOCK_H
#define SHM_HWCAP_MOCK_H

#include "shm_hwcap.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of emulated token slots; matches CapCXL's SHADOW_REG_COUNT. */
#define SHM_HWCAP_MOCK_SLOTS 64u

/** Opaque emulator instance. */
typedef struct shm_hwcap_mock shm_hwcap_mock_t;

/** Create a fresh emulator with all slots free. */
shm_hwcap_mock_t *shm_hwcap_mock_create(void);

/** Destroy an emulator created by shm_hwcap_mock_create(). */
void shm_hwcap_mock_destroy(shm_hwcap_mock_t *mock);

/**
 * @brief Build an shm_hwcap_ops_t bound to @p mock.
 *
 * Pass the result directly to shm_hwcap_set_backend(). The returned ops
 * table's ctx is @p mock; the mock must outlive the backend registration.
 */
shm_hwcap_ops_t shm_hwcap_mock_ops(shm_hwcap_mock_t *mock);

/** Number of currently occupied token slots (for test assertions). */
size_t shm_hwcap_mock_active_count(const shm_hwcap_mock_t *mock);

#ifdef __cplusplus
}
#endif

#endif /* SHM_HWCAP_MOCK_H */
