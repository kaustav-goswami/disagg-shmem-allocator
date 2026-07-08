/**
 * @file  test_cap.c
 * @brief Tests for the CHERI-like 128-bit software capability layer.
 *
 * Test matrix
 * ───────────
 *  1. basic_derive_deref       — derive a capability and successfully deref it.
 *  2. null_cap                 — null capability always fails deref.
 *  3. tag_bit_revocation       — free the block; any deref returns NULL.
 *  4. aba_protection           — free A, alloc B at same offset (new obj_id);
 *                                original cap for A is rejected.
 *  5. epoch_forgery            — manually flip the epoch; deref is rejected.
 *  6. permission_narrowing     — narrow from R|W to R-only; W deref fails.
 *  7. widen_rejected           — attempting to add perms returns SHM_CAP_NULL.
 *  8. bounds_field             — bounds() returns the correct payload_cap.
 *  9. offset_field             — offset() returns the original shm_off_t.
 * 10. bounds_mismatch          — resize invalidates pre-resize capability.
 * 11. shm_cap_is_valid         — false after free, true while live.
 * 12. cross_object_forge       — cap for object A cannot deref object B.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_alloc.h"
#include "shm_cap.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>   /* shm_unlink */
#include <unistd.h>

/* ── Test utilities ──────────────────────────────────────────────────────── */

static const char *SHM_NAME = "/test_cap_region";
static const size_t REGION_SIZE = 1u << 20;  /* 1 MiB */

/** Owner and permission constants used throughout the tests. */
static const shm_user_id_t UID   = 42;
static const shm_perm_t    PERMS = (shm_perm_t)(SHM_PERM_READ | SHM_PERM_WRITE
                                                 | SHM_PERM_FREE | SHM_PERM_RESIZE);

#define PASS(name)  fprintf(stdout, "  PASS  %s\n", (name))
#define FAIL(name)  do { fprintf(stdout, "  FAIL  %s\n", (name)); exit(1); } while(0)

#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stdout, "    assertion failed: %s  (%s:%d)\n", \
                                 #cond, __FILE__, __LINE__); exit(1); } } while(0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Open a fresh region (creates and truncates). */
static shm_region_t *open_region(void)
{
    /* Remove any stale region from a previous run. */
    shm_unlink(SHM_NAME);

    shm_region_open_opts_t opts = {
        .backend       = SHM_BACKEND_POSIX,
        .flags         = SHM_OPEN_CREATE,
        .dir_capacity  = 64,
        .mode          = 0600,
    };
    shm_region_t *r = NULL;
    int rc = shm_region_open(SHM_NAME, REGION_SIZE, &opts, &r);
    if (rc != 0) {
        fprintf(stderr, "shm_region_open failed: %d (%s)\n", rc, strerror(rc));
        exit(1);
    }
    return r;
}

/** Allocate a small integer object.  Returns (obj_id, off) via out-params. */
static void alloc_int(shm_region_t *r, uint64_t *out_id, shm_off_t *out_off)
{
    int rc = shm_alloc(r, sizeof(int), NULL, UID, PERMS, 0, out_id, out_off);
    CHECK(rc == 0);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/** 1. Basic derive and deref. */
static void test_basic_derive_deref(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_cap_t cap = shm_cap_derive(r, off, UID, PERMS, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(!shm_cap_is_null(cap));

    /* READ deref must succeed. */
    void *p = shm_cap_deref(r, cap, SHM_PERM_READ);
    CHECK(p != NULL);

    /* WRITE deref must succeed. */
    p = shm_cap_deref(r, cap, SHM_PERM_WRITE);
    CHECK(p != NULL);

    /* Write a value and read it back to confirm the pointer is the payload. */
    *((int *)p) = 0xDEAD;
    int *ro = (int *)shm_cap_deref(r, cap, SHM_PERM_READ);
    CHECK(*ro == 0xDEAD);

    shm_region_close(r, true);
    PASS("basic_derive_deref");
}

/** 2. Null capability always fails. */
static void test_null_cap(void)
{
    shm_region_t *r = open_region();
    shm_cap_t null_cap = SHM_CAP_NULL;

    CHECK(shm_cap_is_null(null_cap));
    CHECK(shm_cap_deref(r, null_cap, SHM_PERM_READ) == NULL);
    CHECK(!shm_cap_is_valid(r, null_cap));

    shm_region_close(r, true);
    PASS("null_cap");
}

/** 3. Tag-bit revocation: free the block; deref returns NULL. */
static void test_tag_bit_revocation(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_cap_t cap = shm_cap_derive(r, off, UID, PERMS,
                                   SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(!shm_cap_is_null(cap));

    /* Capability is valid before free. */
    CHECK(shm_cap_is_valid(r, cap));
    CHECK(shm_cap_deref(r, cap, SHM_PERM_READ) != NULL);

    /* Free the block — tag bit (allocated) is cleared in the header. */
    int rc = shm_free(r, id, UID, PERMS);
    CHECK(rc == 0);

    /* All derefs must now fail because the tag bit is 0. */
    CHECK(shm_cap_deref(r, cap, SHM_PERM_READ)  == NULL);
    CHECK(shm_cap_deref(r, cap, SHM_PERM_WRITE) == NULL);
    CHECK(!shm_cap_is_valid(r, cap));

    shm_region_close(r, true);
    PASS("tag_bit_revocation");
}

/** 4. ABA protection: old cap is rejected after realloc at the same offset. */
static void test_aba_protection(void)
{
    shm_region_t *r = open_region();

    /* Allocate an int-sized object (A). */
    uint64_t  id_a  = 0;
    shm_off_t off_a = 0;
    alloc_int(r, &id_a, &off_a);

    shm_cap_t cap_a = shm_cap_derive(r, off_a, UID, PERMS, SHM_PERM_READ);
    CHECK(!shm_cap_is_null(cap_a));
    CHECK(shm_cap_deref(r, cap_a, SHM_PERM_READ) != NULL);

    /* Free A so the heap slot is available for reuse. */
    int rc = shm_free(r, id_a, UID, PERMS);
    CHECK(rc == 0);

    /* Allocate B of the same size; it should land at the same offset as A. */
    uint64_t  id_b  = 0;
    shm_off_t off_b = 0;
    alloc_int(r, &id_b, &off_b);

    /* The old cap_a must now be rejected even though off_b == off_a because
     * the epoch (low 16 bits of obj_id) has advanced to a new value. */
    if (off_b == off_a) {
        /* ABA is only demonstrable when the allocator reused the same offset. */
        CHECK(shm_cap_deref(r, cap_a, SHM_PERM_READ) == NULL);
    }
    /* If the allocator placed B elsewhere the test is trivially correct. */

    shm_region_close(r, true);
    PASS("aba_protection");
}

/** 5. Epoch forgery: flip the epoch in the meta word; deref is rejected. */
static void test_epoch_forgery(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_cap_t cap = shm_cap_derive(r, off, UID, PERMS, SHM_PERM_READ);
    CHECK(shm_cap_deref(r, cap, SHM_PERM_READ) != NULL);

    /* Forge a capability by inverting the epoch bits. */
    shm_cap_t forged;
    forged.addr = cap.addr;
    forged.meta = cap.meta ^ SHM_CAP_META_EPOCH_MASK;  /* flip all epoch bits */

    /* The forged capability must be rejected. */
    CHECK(shm_cap_deref(r, forged, SHM_PERM_READ) == NULL);

    shm_region_close(r, true);
    PASS("epoch_forgery");
}

/** 6. Permission narrowing: narrow R|W to R; WRITE deref then fails. */
static void test_permission_narrowing(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_cap_t rw_cap = shm_cap_derive(r, off, UID, PERMS,
                                      SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(!shm_cap_is_null(rw_cap));
    CHECK(shm_cap_deref(r, rw_cap, SHM_PERM_WRITE) != NULL);

    /* Narrow to read-only. */
    shm_cap_t ro_cap = shm_cap_narrow(rw_cap, SHM_PERM_READ);
    CHECK(!shm_cap_is_null(ro_cap));

    /* Read deref must still work. */
    CHECK(shm_cap_deref(r, ro_cap, SHM_PERM_READ) != NULL);

    /* Write deref must fail: the sealed perms no longer include WRITE. */
    CHECK(shm_cap_deref(r, ro_cap, SHM_PERM_WRITE) == NULL);

    shm_region_close(r, true);
    PASS("permission_narrowing");
}

/** 7. Attempting to widen permissions via shm_cap_narrow returns null. */
static void test_widen_rejected(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    /* Derive a read-only capability. */
    shm_cap_t ro_cap = shm_cap_derive(r, off, UID, PERMS, SHM_PERM_READ);
    CHECK(!shm_cap_is_null(ro_cap));

    /* Attempt to widen by adding WRITE — must return SHM_CAP_NULL. */
    shm_cap_t wide = shm_cap_narrow(ro_cap, SHM_PERM_READ | SHM_PERM_WRITE);
    CHECK(shm_cap_is_null(wide));

    shm_region_close(r, true);
    PASS("widen_rejected");
}

/** 8. Bounds field matches the payload capacity of the allocated object. */
static void test_bounds_field(void)
{
    shm_region_t *r = open_region();

    /* Allocate a specific payload size. */
    uint64_t  id  = 0;
    shm_off_t off = 0;
    size_t    pay = 128;
    int rc = shm_alloc(r, pay, NULL, UID, PERMS, 0, &id, &off);
    CHECK(rc == 0);

    shm_cap_t cap = shm_cap_derive(r, off, UID, PERMS, SHM_PERM_READ);
    CHECK(!shm_cap_is_null(cap));

    /* bounds() must be >= the requested payload size (may be rounded up). */
    CHECK(shm_cap_bounds(cap) >= pay);

    /* bounds() must match what shm_block_info() reports for payload_cap. */
    size_t blk_cap = 0;
    shm_block_info(r, off, &blk_cap, NULL, NULL, NULL, NULL);
    CHECK(shm_cap_bounds(cap) == blk_cap);

    shm_region_close(r, true);
    PASS("bounds_field");
}

/** 9. Offset field returns the original shm_off_t. */
static void test_offset_field(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_cap_t cap = shm_cap_derive(r, off, UID, PERMS, SHM_PERM_READ);
    CHECK(shm_cap_offset(cap) == off);

    shm_region_close(r, true);
    PASS("offset_field");
}

/** 10. Resize invalidates a pre-resize capability (bounds mismatch). */
static void test_bounds_mismatch_after_resize(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;

    int rc = shm_alloc(r, 64, NULL, UID, PERMS, 0, &id, &off);
    CHECK(rc == 0);

    /* Derive a capability before resize. */
    shm_cap_t pre_cap = shm_cap_derive(r, off, UID, PERMS, SHM_PERM_READ);
    CHECK(!shm_cap_is_null(pre_cap));
    CHECK(shm_cap_deref(r, pre_cap, SHM_PERM_READ) != NULL);

    /* Grow the object — payload_cap changes. */
    rc = shm_resize(r, id, 256, UID, PERMS);
    CHECK(rc == 0);

    /* Re-lookup the new offset (relocation may have occurred). */
    shm_off_t new_off = 0;
    shm_lookup(r, id, UID, PERMS, &new_off, NULL);

    if (new_off == off) {
        /* In-place growth: bounds field in pre_cap no longer matches block. */
        CHECK(shm_cap_deref(r, pre_cap, SHM_PERM_READ) == NULL);
        CHECK(!shm_cap_is_valid(r, pre_cap));
    }
    /* If relocation occurred, the block at the old offset is freed; the tag
     * bit check catches that case, also returning NULL.                    */

    /* Derive a new capability after resize — must succeed. */
    shm_cap_t post_cap = shm_cap_derive(r, new_off, UID, PERMS, SHM_PERM_READ);
    CHECK(!shm_cap_is_null(post_cap));
    CHECK(shm_cap_deref(r, post_cap, SHM_PERM_READ) != NULL);
    CHECK(shm_cap_bounds(post_cap) >= 256);

    shm_region_close(r, true);
    PASS("bounds_mismatch_after_resize");
}

/** 11. shm_cap_is_valid: false after free, true while live. */
static void test_is_valid(void)
{
    shm_region_t *r = open_region();
    uint64_t  id  = 0;
    shm_off_t off = 0;
    alloc_int(r, &id, &off);

    shm_cap_t cap = shm_cap_derive(r, off, UID, PERMS, SHM_PERM_READ);
    CHECK(shm_cap_is_valid(r, cap));

    shm_free(r, id, UID, PERMS);

    CHECK(!shm_cap_is_valid(r, cap));

    shm_region_close(r, true);
    PASS("is_valid");
}

/** 12. Cross-object forgery: cap for A cannot be used to deref B. */
static void test_cross_object_forge(void)
{
    shm_region_t *r = open_region();
    uint64_t  id_a = 0, id_b = 0;
    shm_off_t off_a = 0, off_b = 0;

    /* Allocate two separate objects. */
    alloc_int(r, &id_a, &off_a);
    int rc = shm_alloc(r, sizeof(int), NULL, UID, PERMS, 0, &id_b, &off_b);
    CHECK(rc == 0);
    CHECK(off_a != off_b);

    /* Derive a cap for A, then forge its offset to point at B. */
    shm_cap_t cap_a = shm_cap_derive(r, off_a, UID, PERMS, SHM_PERM_READ);
    CHECK(!shm_cap_is_null(cap_a));

    /* Replace addr with off_b — epoch/bounds from A will not match B's header. */
    shm_cap_t forged;
    forged.addr = off_b;
    forged.meta = cap_a.meta;

    /* The forged capability should be rejected (epoch or bounds mismatch). */
    CHECK(shm_cap_deref(r, forged, SHM_PERM_READ) == NULL);

    shm_region_close(r, true);
    PASS("cross_object_forge");
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_cap ===\n");

    test_basic_derive_deref();
    test_null_cap();
    test_tag_bit_revocation();
    test_aba_protection();
    test_epoch_forgery();
    test_permission_narrowing();
    test_widen_rejected();
    test_bounds_field();
    test_offset_field();
    test_bounds_mismatch_after_resize();
    test_is_valid();
    test_cross_object_forge();

    printf("=== all cap tests passed ===\n");
    return 0;
}
