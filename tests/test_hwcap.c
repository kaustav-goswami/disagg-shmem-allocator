/**
 * @file  test_hwcap.c
 * @brief Tests for the optional hardware-capability bridge (shm_hwcap.h)
 *        against the software CEU emulator (shm_hwcap_mock.h).
 *
 * Test matrix
 * ───────────
 *  1. perm_translation        — shm_perm_to_hwcap/shm_hwcap_to_perm never let
 *                                SHM_PERM_FREE (0x04) leak into hardware EXEC.
 *  2. software_only_no_backend — with no backend bound, derive/deref behave
 *                                as pure shm_cap_t pass-throughs.
 *  3. basic_bridge_derive_deref — with the mock bound, derive obtains a live
 *                                hardware token and deref succeeds.
 *  4. free_hook_revokes_token  — shm_free() auto-revokes the object's token.
 *  5. explicit_revoke          — shm_hwcap_revoke() clears the token without
 *                                freeing the underlying object.
 *  6. table_exhaustion_degrades — once the mock's 64 slots are full, further
 *                                derives still succeed as software-only caps.
 *  7. hw_denial_fails_deref    — an out-of-band hardware-side revoke is
 *                                caught by shm_hwcap_deref() even though the
 *                                software capability alone still looks valid.
 *  8. clear_backend_reverts_to_sw — after shm_hwcap_clear_backend(), stale
 *                                hw_token values are ignored and only the
 *                                software checks apply.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_alloc.h"
#include "shm_cap.h"
#include "shm_hwcap.h"
#include "shm_hwcap_mock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>   /* shm_unlink */

/* ── Test utilities (mirrors tests/test_cap.c) ───────────────────────────── */

static const char *SHM_NAME = "/test_hwcap_region";
static const size_t REGION_SIZE = 1u << 20;  /* 1 MiB */

static const shm_user_id_t UID   = 42;
static const shm_perm_t    PERMS = (shm_perm_t)(SHM_PERM_READ | SHM_PERM_WRITE
                                                 | SHM_PERM_FREE | SHM_PERM_RESIZE);

#define PASS(name)  fprintf(stdout, "  PASS  %s\n", (name))
#define FAIL(name)  do { fprintf(stdout, "  FAIL  %s\n", (name)); exit(1); } while(0)

#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stdout, "    assertion failed: %s  (%s:%d)\n", \
                                 #cond, __FILE__, __LINE__); exit(1); } } while(0)

static shm_region_t *open_region(void)
{
    shm_unlink(SHM_NAME);

    shm_region_open_opts_t opts = {
        .backend      = SHM_BACKEND_POSIX,
        .flags        = SHM_OPEN_CREATE,
        .dir_capacity = 128,
        .mode         = 0600,
    };
    shm_region_t *r = NULL;
    int rc = shm_region_open(SHM_NAME, REGION_SIZE, &opts, &r);
    if (rc != 0) {
        fprintf(stderr, "shm_region_open failed: %d (%s)\n", rc, strerror(rc));
        exit(1);
    }
    return r;
}

static void alloc_int(shm_region_t *r, uint64_t *out_id, shm_off_t *out_off)
{
    int rc = shm_alloc(r, sizeof(int), NULL, UID, PERMS, 0, out_id, out_off);
    CHECK(rc == 0);
}

/* Every test that binds a backend must clear it again before returning, so
 * tests remain independent regardless of run order. */
static void reset_backend(void)
{
    shm_hwcap_clear_backend();
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/** 1. Permission translation must never map SHM_PERM_FREE onto hardware EXEC. */
static void test_perm_translation(void)
{
    /* READ and WRITE cross the bridge unchanged. */
    CHECK(shm_perm_to_hwcap(SHM_PERM_READ)  == SHM_HWCAP_PERM_READ);
    CHECK(shm_perm_to_hwcap(SHM_PERM_WRITE) == SHM_HWCAP_PERM_WRITE);
    CHECK(shm_perm_to_hwcap(SHM_PERM_READ | SHM_PERM_WRITE)
          == (SHM_HWCAP_PERM_READ | SHM_HWCAP_PERM_WRITE));

    /* FREE (0x04) and RESIZE (0x08) have no hardware meaning and must be
     * dropped -- in particular FREE must NOT turn into EXEC (also 0x04). */
    CHECK(shm_perm_to_hwcap(SHM_PERM_FREE)   == 0);
    CHECK(shm_perm_to_hwcap(SHM_PERM_RESIZE) == 0);
    CHECK(shm_perm_to_hwcap(PERMS) == (SHM_HWCAP_PERM_READ | SHM_HWCAP_PERM_WRITE));

    /* ADMIN must not leak through either. */
    CHECK(shm_perm_to_hwcap(SHM_PERM_ADMIN) == 0);

    /* Round trip back: EXEC has no shm_perm_t equivalent and must vanish. */
    CHECK(shm_hwcap_to_perm(SHM_HWCAP_PERM_READ | SHM_HWCAP_PERM_WRITE | SHM_HWCAP_PERM_EXEC)
          == (SHM_PERM_READ | SHM_PERM_WRITE));

    PASS("perm_translation");
}

/** 2. No backend bound: derive/deref behave as pure shm_cap_t pass-throughs. */
static void test_software_only_no_backend(void)
{
    CHECK(!shm_hwcap_backend_bound());

    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_hwcap_t cap = shm_hwcap_derive(r, off, UID, PERMS, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(!shm_cap_is_null(cap.sw));
    CHECK(cap.hw_token == SHM_HWCAP_TOKEN_NONE);
    CHECK(!shm_hwcap_is_bound(cap));

    void *p = shm_hwcap_deref(r, cap, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(p != NULL);

    shm_region_close(r, true);
    PASS("software_only_no_backend");
}

/** 3. Mock bound: derive obtains a live token, deref checks both layers. */
static void test_basic_bridge_derive_deref(void)
{
    shm_hwcap_mock_t *mock = shm_hwcap_mock_create();
    shm_hwcap_ops_t   ops  = shm_hwcap_mock_ops(mock);
    shm_hwcap_set_backend(&ops);
    CHECK(shm_hwcap_backend_bound());

    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_hwcap_t cap = shm_hwcap_derive(r, off, UID, PERMS, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(!shm_cap_is_null(cap.sw));
    CHECK(shm_hwcap_is_bound(cap));
    CHECK(shm_hwcap_mock_active_count(mock) == 1);

    void *p = shm_hwcap_deref(r, cap, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(p != NULL);

    /* Requiring only READ must also succeed (subset of granted perms). */
    p = shm_hwcap_deref(r, cap, SHM_PERM_READ);
    CHECK(p != NULL);

    shm_region_close(r, true);
    reset_backend();
    shm_hwcap_mock_destroy(mock);
    PASS("basic_bridge_derive_deref");
}

/** 4. shm_free() must auto-revoke the object's hardware token. */
static void test_free_hook_revokes_token(void)
{
    shm_hwcap_mock_t *mock = shm_hwcap_mock_create();
    shm_hwcap_ops_t   ops  = shm_hwcap_mock_ops(mock);
    shm_hwcap_set_backend(&ops);

    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_hwcap_t cap = shm_hwcap_derive(r, off, UID, PERMS, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(shm_hwcap_is_bound(cap));
    CHECK(shm_hwcap_mock_active_count(mock) == 1);

    int rc = shm_free(r, id, UID, SHM_PERM_FREE);
    CHECK(rc == 0);

    /* The free hook must have revoked the token in the mock table. */
    CHECK(shm_hwcap_mock_active_count(mock) == 0);

    /* And the bridged deref must fail too (software half already invalid). */
    CHECK(shm_hwcap_deref(r, cap, SHM_PERM_READ) == NULL);

    shm_region_close(r, true);
    reset_backend();
    shm_hwcap_mock_destroy(mock);
    PASS("free_hook_revokes_token");
}

/** 5. Explicit revoke clears the token without freeing the object. */
static void test_explicit_revoke(void)
{
    shm_hwcap_mock_t *mock = shm_hwcap_mock_create();
    shm_hwcap_ops_t   ops  = shm_hwcap_mock_ops(mock);
    shm_hwcap_set_backend(&ops);

    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_hwcap_t cap = shm_hwcap_derive(r, off, UID, PERMS, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(shm_hwcap_is_bound(cap));

    int rc = shm_hwcap_revoke(&cap);
    CHECK(rc == 0);
    CHECK(!shm_hwcap_is_bound(cap));
    CHECK(shm_hwcap_mock_active_count(mock) == 0);

    /* A second revoke on an already-cleared capability is rejected. */
    CHECK(shm_hwcap_revoke(&cap) == EINVAL);

    /* The object itself is still alive: software-only deref still succeeds. */
    void *p = shm_hwcap_deref(r, cap, SHM_PERM_READ);
    CHECK(p != NULL);

    shm_region_close(r, true);
    reset_backend();
    shm_hwcap_mock_destroy(mock);
    PASS("explicit_revoke");
}

/** 6. Once the mock's 64 slots are full, derive still succeeds (sw-only). */
static void test_table_exhaustion_degrades(void)
{
    shm_hwcap_mock_t *mock = shm_hwcap_mock_create();
    shm_hwcap_ops_t   ops  = shm_hwcap_mock_ops(mock);
    shm_hwcap_set_backend(&ops);

    shm_region_t *r = open_region();

    for (unsigned i = 0; i < SHM_HWCAP_MOCK_SLOTS; i++) {
        uint64_t  id  = 0;
        shm_off_t off = 0;
        alloc_int(r, &id, &off);
        shm_hwcap_t cap = shm_hwcap_derive(r, off, UID, PERMS,
                                            SHM_PERM_READ | SHM_PERM_WRITE);
        CHECK(!shm_cap_is_null(cap.sw));
        CHECK(shm_hwcap_is_bound(cap));
    }
    CHECK(shm_hwcap_mock_active_count(mock) == SHM_HWCAP_MOCK_SLOTS);

    /* The table is now full: one more object still gets a valid software
     * capability, just without a hardware token. */
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);
    shm_hwcap_t overflow_cap = shm_hwcap_derive(r, off, UID, PERMS,
                                                 SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(!shm_cap_is_null(overflow_cap.sw));
    CHECK(!shm_hwcap_is_bound(overflow_cap));

    void *p = shm_hwcap_deref(r, overflow_cap, SHM_PERM_READ);
    CHECK(p != NULL);

    shm_region_close(r, true);
    reset_backend();
    shm_hwcap_mock_destroy(mock);
    PASS("table_exhaustion_degrades");
}

/** 7. An out-of-band hardware revoke must be caught on the next deref. */
static void test_hw_denial_fails_deref(void)
{
    shm_hwcap_mock_t *mock = shm_hwcap_mock_create();
    shm_hwcap_ops_t   ops  = shm_hwcap_mock_ops(mock);
    shm_hwcap_set_backend(&ops);

    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_hwcap_t cap = shm_hwcap_derive(r, off, UID, PERMS, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(shm_hwcap_is_bound(cap));

    /* Simulate the hardware revoking this token through some path that
     * bypasses shm_hwcap_revoke() (e.g. an external fault handler). The
     * software capability and cap.hw_token are unaware of this. */
    CHECK(ops.revoke_region(ops.ctx, cap.hw_token) == 0);

    /* Software checks alone would still say this capability is fine... */
    CHECK(shm_cap_deref(r, cap.sw, SHM_PERM_READ) != NULL);

    /* ...but the bridged deref must catch the hardware-side denial. */
    CHECK(shm_hwcap_deref(r, cap, SHM_PERM_READ) == NULL);

    shm_region_close(r, true);
    reset_backend();
    shm_hwcap_mock_destroy(mock);
    PASS("hw_denial_fails_deref");
}

/** 8. Clearing the backend reverts stale hw_token values to sw-only checks. */
static void test_clear_backend_reverts_to_sw(void)
{
    shm_hwcap_mock_t *mock = shm_hwcap_mock_create();
    shm_hwcap_ops_t   ops  = shm_hwcap_mock_ops(mock);
    shm_hwcap_set_backend(&ops);

    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_hwcap_t cap = shm_hwcap_derive(r, off, UID, PERMS, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(shm_hwcap_is_bound(cap));

    shm_hwcap_clear_backend();
    CHECK(!shm_hwcap_backend_bound());

    /* cap.hw_token is still a non-NONE value, but with no backend installed
     * shm_hwcap_deref() must skip the hardware check entirely. */
    void *p = shm_hwcap_deref(r, cap, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(p != NULL);

    shm_region_close(r, true);
    shm_hwcap_mock_destroy(mock);
    PASS("clear_backend_reverts_to_sw");
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_hwcap ===\n");

    test_perm_translation();
    test_software_only_no_backend();
    test_basic_bridge_derive_deref();
    test_free_hook_revokes_token();
    test_explicit_revoke();
    test_table_exhaustion_degrades();
    test_hw_denial_fails_deref();
    test_clear_backend_reverts_to_sw();

    printf("=== all hwcap tests passed ===\n");
    return 0;
}
