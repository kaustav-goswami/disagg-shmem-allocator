/**
 * @file  test_cap_side.c
 * @brief Tests for the side-metadata capability (shm_cap_side.h, variant a).
 *
 * Test matrix
 * ───────────
 *  1. derive_deref_basic     — mint succeeds, deref with granted perms works.
 *  2. deref_is_bearer_only   — deref works regardless of "who" calls it; the
 *                               API has no identity parameter for deref at all.
 *  3. wrong_key_denied       — a forged/guessed key is rejected by deref.
 *  4. rotate_requires_identity — the right key with the WRONG identity is
 *                               rejected; the right key AND identity succeeds.
 *  5. rotate_invalidates_old — after a correctly-authorized rotate, the OLD
 *                               capability fails and the NEW one succeeds,
 *                               immediately, for any identity on deref.
 *  6. free_requires_identity — same identity-gating as rotate, for free.
 *  7. free_then_deref_fails  — after a correctly-authorized free, the key no
 *                               longer matches anything; a second free (even
 *                               with the right identity) is rejected.
 *  8. perm_subset_enforced   — deref requiring WRITE fails on a READ-only cap.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_alloc.h"
#include "shm_cap_side.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static const char  *SHM_NAME   = "/test_cap_side_region";
static const size_t REGION_SIZE = 1u << 20;

static const uint32_t OWNER   = 7;
static const uint32_t STRANGER = 99;

#define PASS(name)  fprintf(stdout, "  PASS  %s\n", (name))
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stdout, "    assertion failed: %s  (%s:%d)\n", \
                                 #cond, __FILE__, __LINE__); exit(1); } } while(0)

static shm_region_t *open_region(void)
{
    shm_unlink(SHM_NAME);
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE,
        .dir_capacity = 64, .mode = 0600,
    };
    shm_region_t *r = NULL;
    int rc = shm_region_open(SHM_NAME, REGION_SIZE, &opts, &r);
    CHECK(rc == 0);
    return r;
}

static shm_off_t alloc_int(shm_region_t *r)
{
    uint64_t id = 0; shm_off_t off = 0;
    int rc = shm_alloc(r, sizeof(int), NULL, 0 /*unused: bearer model*/,
                        SHM_PERM_DEFAULT, 0, &id, &off);
    CHECK(rc == 0);
    return off;
}

static void test_derive_deref_basic(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t cap = shm_cap_side_derive(r, off, SHM_PERM_READ | SHM_PERM_WRITE, OWNER);
    CHECK(!shm_cap_side_is_null(cap));
    CHECK(shm_cap_side_key(cap) != 0);
    CHECK(shm_cap_side_identity(cap) == OWNER);

    int *p = (int *)shm_cap_side_deref(r, cap, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(p != NULL);
    *p = 42;
    CHECK(*p == 42);

    shm_region_close(r, true);
    PASS("derive_deref_basic");
}

/** deref() has no identity parameter at all: possession of the key is
 * sufficient to read/write, regardless of who's asking. This part of the
 * bearer model is unchanged by adding identity-gated mutation. */
static void test_deref_is_bearer_only(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t cap = shm_cap_side_derive(r, off, SHM_PERM_READ, OWNER);
    CHECK(!shm_cap_side_is_null(cap));

    void *p1 = shm_cap_side_deref(r, cap, SHM_PERM_READ);
    void *p2 = shm_cap_side_deref(r, cap, SHM_PERM_READ);
    CHECK(p1 != NULL && p2 != NULL && p1 == p2);

    shm_region_close(r, true);
    PASS("deref_is_bearer_only");
}

static void test_wrong_key_denied(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t cap = shm_cap_side_derive(r, off, SHM_PERM_READ, OWNER);
    CHECK(!shm_cap_side_is_null(cap));

    shm_cap_side_t forged = cap;
    forged.key_perms ^= (1u << SHM_CAP_SIDE_KEY_SHIFT);   /* flip one key bit */
    CHECK(shm_cap_side_key(forged) != shm_cap_side_key(cap));

    void *p = shm_cap_side_deref(r, forged, SHM_PERM_READ);
    CHECK(p == NULL);
    CHECK(errno == EACCES);

    shm_region_close(r, true);
    PASS("wrong_key_denied");
}

/** The right key with the wrong identity cannot rotate; the right key with
 * the right identity can. Key possession alone is no longer enough. */
static void test_rotate_requires_identity(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t cap = shm_cap_side_derive(r, off, SHM_PERM_READ, OWNER);
    CHECK(!shm_cap_side_is_null(cap));

    /* STRANGER has the real key (it's bearer-readable) but is not the owner. */
    CHECK(shm_cap_side_is_null(shm_cap_side_rotate(cap, STRANGER)));
    /* The attempt must not have mutated anything: the original key still works. */
    CHECK(shm_cap_side_deref(r, cap, SHM_PERM_READ) != NULL);

    shm_cap_side_t rotated = shm_cap_side_rotate(cap, OWNER);
    CHECK(!shm_cap_side_is_null(rotated));
    CHECK(shm_cap_side_identity(rotated) == OWNER);

    shm_region_close(r, true);
    PASS("rotate_requires_identity");
}

static void test_rotate_invalidates_old(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t old_cap = shm_cap_side_derive(r, off, SHM_PERM_READ | SHM_PERM_WRITE, OWNER);
    CHECK(!shm_cap_side_is_null(old_cap));
    CHECK(shm_cap_side_deref(r, old_cap, SHM_PERM_READ) != NULL);

    shm_cap_side_t new_cap = shm_cap_side_rotate(old_cap, OWNER);
    CHECK(!shm_cap_side_is_null(new_cap));
    CHECK(shm_cap_side_key(new_cap) != shm_cap_side_key(old_cap));

    /* Old key: dead immediately, no window -- for anyone, since deref is
     * still pure bearer and doesn't take an identity at all. */
    CHECK(shm_cap_side_deref(r, old_cap, SHM_PERM_READ) == NULL);

    /* New key: works, same allocation. */
    void *p = shm_cap_side_deref(r, new_cap, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(p != NULL);

    /* Rotating the already-stale old_cap again is rejected, not silently
     * reusing whatever is now live -- even with the correct owner identity. */
    CHECK(shm_cap_side_is_null(shm_cap_side_rotate(old_cap, OWNER)));

    shm_region_close(r, true);
    PASS("rotate_invalidates_old");
}

static void test_free_requires_identity(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t cap = shm_cap_side_derive(r, off, SHM_PERM_READ, OWNER);
    CHECK(!shm_cap_side_is_null(cap));

    CHECK(shm_cap_side_free(cap, STRANGER) == EACCES);
    /* Rejected attempt must not have cleared the row. */
    CHECK(shm_cap_side_deref(r, cap, SHM_PERM_READ) != NULL);

    CHECK(shm_cap_side_free(cap, OWNER) == 0);
    CHECK(shm_cap_side_deref(r, cap, SHM_PERM_READ) == NULL);

    shm_region_close(r, true);
    PASS("free_requires_identity");
}

static void test_free_then_deref_fails(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t cap = shm_cap_side_derive(r, off, SHM_PERM_READ, OWNER);
    CHECK(!shm_cap_side_is_null(cap));

    CHECK(shm_cap_side_free(cap, OWNER) == 0);
    CHECK(shm_cap_side_deref(r, cap, SHM_PERM_READ) == NULL);

    /* Double free: the key no longer matches anything -- rejected (ENOENT,
     * not EACCES, since there's no row left to check identity against),
     * not a silent no-op success. */
    CHECK(shm_cap_side_free(cap, OWNER) == ENOENT);

    shm_region_close(r, true);
    PASS("free_then_deref_fails");
}

static void test_perm_subset_enforced(void)
{
    shm_region_t *r = open_region();
    shm_off_t off = alloc_int(r);

    shm_cap_side_t cap = shm_cap_side_derive(r, off, SHM_PERM_READ, OWNER);  /* READ only */
    CHECK(!shm_cap_side_is_null(cap));

    CHECK(shm_cap_side_deref(r, cap, SHM_PERM_READ) != NULL);
    CHECK(shm_cap_side_deref(r, cap, SHM_PERM_WRITE) == NULL);
    CHECK(shm_cap_side_deref(r, cap, SHM_PERM_READ | SHM_PERM_WRITE) == NULL);

    shm_region_close(r, true);
    PASS("perm_subset_enforced");
}

int main(void)
{
    printf("=== test_cap_side ===\n");
    test_derive_deref_basic();
    test_deref_is_bearer_only();
    test_wrong_key_denied();
    test_rotate_requires_identity();
    test_rotate_invalidates_old();
    test_free_requires_identity();
    test_free_then_deref_fails();
    test_perm_subset_enforced();
    printf("=== all cap_side tests passed ===\n");
    return 0;
}
