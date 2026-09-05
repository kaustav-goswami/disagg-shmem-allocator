/**
 * @file  test_slim_bounds.c
 * @brief Bounds-checked offset resolution tests for the v0.2 "slim" format,
 *        mirroring tests/test_bounds.c for v2.
 *
 * Unlike v2, shm_slim_free()/shm_slim_resize() take the offset directly from
 * the caller rather than trusting a separate directory entry -- there is no
 * directory to corrupt. So the attack surface collapses to one place: any
 * function that accepts a caller-supplied shm_off_t must never turn a bad
 * one into an out-of-bounds dereference. These tests hit that directly, plus
 * the heap-walk's implicit block chain, which has the same class of risk in
 * the opposite direction (reading a corrupted `size` field could walk the
 * cursor past the end of the heap).
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_slim.h"
#include "shm_slim_internal.h"   /* to poke corrupted values directly, test-only */

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ── Minimal test framework ──────────────────────────────────────────────── */

static int g_failed = 0;

#define CHECK(cond, desc) \
    do { \
        if (cond) { printf("  PASS: %s\n", desc); } \
        else { fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, desc); \
               g_failed = 1; } \
    } while (0)

#define CHECK_OK(rc, desc)        CHECK((rc) == 0, desc)
#define CHECK_ERR(rc, exp, desc)  CHECK((rc) == (exp), desc)

typedef struct { char payload[64]; } record_t;

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_bad_offsets_do_not_crash(void)
{
    puts("\n[test_bad_offsets_do_not_crash]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_bounds_read", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc_typed(r, record_t, NULL, 1u, &off);

    size_t heap_bytes = shm_slim_heap_size(r);

    void *p = shm_slim_ptr(r, SHM_OFF_NULL, 1u);
    CHECK(p == NULL, "shm_slim_ptr(SHM_OFF_NULL) returns NULL");

    p = shm_slim_ptr(r, 0, 1u);
    CHECK(p == NULL, "shm_slim_ptr(0) returns NULL");

    p = shm_slim_ptr(r, (shm_off_t)heap_bytes + 1, 1u);
    CHECK(p == NULL, "shm_slim_ptr(heap_size + 1) returns NULL, does not crash");

    p = shm_slim_ptr(r, (shm_off_t)heap_bytes * 1000000ULL, 1u);
    CHECK(p == NULL, "shm_slim_ptr(huge offset) returns NULL, does not crash");

    size_t sz;
    int rc = shm_slim_block_size(r, (shm_off_t)heap_bytes * 1000000ULL, 1u, &sz);
    CHECK_ERR(rc, EINVAL, "shm_slim_block_size(huge offset) returns EINVAL, does not crash");

    rc = shm_slim_free(r, (shm_off_t)heap_bytes + 1, 1u);
    CHECK_ERR(rc, EFAULT, "shm_slim_free(out-of-range offset) returns EFAULT, does not crash");

    shm_off_t new_off;
    rc = shm_slim_resize(r, (shm_off_t)heap_bytes * 1000000ULL, 128, 1u, &new_off);
    CHECK_ERR(rc, EFAULT, "shm_slim_resize(huge offset) returns EFAULT, does not crash");

    /* The legitimate object is unaffected by all of the above abuse. */
    const record_t *q = shm_slim_cast(r, off, record_t, 1u);
    CHECK(q != NULL, "valid offset still resolves correctly");

    shm_slim_region_close(r, true);
}

static void test_rejects_corrupted_size(void)
{
    puts("\n[test_rejects_corrupted_size]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_bounds_corrupt", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc_typed(r, record_t, NULL, 1u, &off);

    /* Reach past the public API to poison the header directly, simulating
     * corruption / a torn write from another host. */
    shm_slim_hdr_t *b = shm_slim_blk_of_poff(r, off);
    uint64_t original_size = b->size;
    b->size = (uint64_t)shm_slim_heap_size(r) * 2;   /* claims to overrun the heap */

    void *p = shm_slim_ptr(r, off, 1u);
    CHECK(p == NULL, "shm_slim_ptr rejects a block whose size overruns the heap");

    b->size = original_size;   /* restore so the region tears down cleanly */

    shm_slim_region_close(r, true);
}

static void test_walk_rejects_corrupted_chain(void)
{
    puts("\n[test_walk_rejects_corrupted_chain]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_bounds_walk", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc_typed(r, record_t, NULL, 1u, &off);

    shm_slim_hdr_t *b = shm_slim_blk_of_poff(r, off);
    b->size = (uint64_t)shm_slim_heap_size(r) * 2;   /* corrupt the implicit-list size */

    uint64_t cursor = 0;
    shm_slim_entry_t e;
    int rc = shm_slim_walk(r, &cursor, &e);
    CHECK_ERR(rc, EIO, "walk detects a corrupted chain and returns EIO instead of running past the heap");

    /* Restore so the region tears down cleanly (region_close does not walk
     * the heap, but leaving corrupted shared memory around is bad hygiene). */
    b->size = sizeof(*b) + sizeof(record_t);

    shm_slim_region_close(r, true);
}

static void test_key_mismatch_paths(void)
{
    puts("\n[test_key_mismatch_paths]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_bounds_key", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc_typed(r, record_t, NULL, 0x1234u, &off);

    CHECK(shm_slim_ptr(r, off, 0x0000u) == NULL, "shm_slim_ptr denies a wrong key");
    CHECK(shm_slim_ptr(r, off, 0x1234u) != NULL, "shm_slim_ptr accepts the right key");

    shm_off_t new_off;
    int rc = shm_slim_resize(r, off, 128, 0xFFFFu, &new_off);
    CHECK_ERR(rc, EACCES, "resize denies a wrong key instead of silently succeeding");

    rc = shm_slim_free(r, off, 0xFFFFu);
    CHECK_ERR(rc, EACCES, "free denies a wrong key instead of silently succeeding");

    shm_slim_region_close(r, true);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    puts("=== test_slim_bounds ===");
    test_bad_offsets_do_not_crash();
    test_rejects_corrupted_size();
    test_walk_rejects_corrupted_chain();
    test_key_mismatch_paths();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
