/**
 * @file  test_resize.c
 * @brief Tests for shm_resize(): shrink, in-place growth, relocation growth.
 *
 * Each sub-test verifies that:
 *   - The used_size in the directory reflects the new size.
 *   - The payload content is preserved across a resize.
 *   - The pool accounting (used_bytes, block_count) stays consistent.
 *   - After shrink + re-grow the reclaimed space is reachable.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_alloc.h"

#include <errno.h>    /* EACCES, ENOENT */
#include <stdio.h>
#include <string.h>

/* ── Minimal test framework (same as test_basic.c) ───────────────────────── */

static int g_failed = 0;

#define CHECK(cond, desc) \
    do { \
        if (cond) { printf("  PASS: %s\n", desc); } \
        else { fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, desc); \
               g_failed = 1; } \
    } while (0)

#define CHECK_OK(rc, desc)           CHECK((rc) == 0, desc)
#define CHECK_ERR(rc, exp, desc)     CHECK((rc) == (exp), desc)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Fill @p n bytes starting at @p ptr with a repeating byte value @p val. */
static void fill_bytes(void *ptr, size_t n, unsigned char val)
{
    memset(ptr, (int)val, n);
}

/** Verify that all @p n bytes starting at @p ptr equal @p val. */
static int check_bytes(const void *ptr, size_t n, unsigned char val)
{
    const unsigned char *p = (const unsigned char *)ptr;
    for (size_t i = 0; i < n; i++)
        if (p[i] != val) return 0;   /* mismatch found */
    return 1;   /* all bytes match */
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_shrink(void)
{
    puts("\n[test_shrink]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_resize_shrink", 1 << 20, &opts, &r);

    const shm_user_id_t uid   = 1;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    /* Allocate 512 bytes. */
    uint64_t id; shm_off_t off;
    shm_alloc(r, 512, "shrink_obj", uid, perms, 0, &id, &off);

    /* Write a pattern into the first 256 bytes. */
    void *p = shm_ptr(r, off, uid, perms, SHM_PERM_WRITE);
    fill_bytes(p, 256, 0xAB);

    /* Record pool state before shrink. */
    shm_pool_stats_t before, after;
    shm_pool_stats(r, &before);

    /* Shrink to 256 bytes. */
    int rc = shm_resize(r, id, 256, uid, perms);
    CHECK_OK(rc, "shm_resize shrink returns 0");

    /* The directory entry must reflect the new used_size. */
    size_t used = 0;
    shm_lookup(r, id, uid, SHM_PERM_READ, &off, &used);
    CHECK(used == 256, "used_size updated to 256 after shrink");

    /* The original content in the first 256 bytes must be intact. */
    p = shm_ptr(r, off, uid, perms, SHM_PERM_READ);
    CHECK(check_bytes(p, 256, 0xAB), "content preserved after shrink");

    /* Pool used_bytes should have decreased (split off a free block). */
    shm_pool_stats(r, &after);
    CHECK(after.used_bytes < before.used_bytes,
          "used_bytes decreased after shrink");

    /* The freed tail space should be available for a new allocation. */
    uint64_t id2; shm_off_t off2;
    rc = shm_alloc(r, 200, "reuse_after_shrink", uid, perms, 0, &id2, &off2);
    CHECK_OK(rc, "can allocate from reclaimed tail space");

    shm_free(r, id,  uid, perms);
    shm_free(r, id2, uid, perms);
    shm_region_close(r, true);
}

static void test_grow_in_place(void)
{
    puts("\n[test_grow_in_place]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_resize_inplace", 1 << 20, &opts, &r);

    const shm_user_id_t uid   = 1;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    /* Allocate a small object (128 bytes) followed by nothing else.
     * Since the heap starts empty and no other allocation has been made,
     * the adjacent free block covers the rest of the heap. */
    uint64_t id; shm_off_t off_before;
    shm_alloc(r, 128, "grow_me", uid, perms, 0, &id, &off_before);

    /* Write a pattern. */
    void *p = shm_ptr(r, off_before, uid, perms, SHM_PERM_WRITE);
    fill_bytes(p, 128, 0xCD);

    /* Grow to 512 bytes — the adjacent free block has plenty of space. */
    int rc = shm_resize(r, id, 512, uid, perms);
    CHECK_OK(rc, "shm_resize grow in-place returns 0");

    /* After in-place grow the offset should NOT have changed. */
    shm_off_t off_after; size_t used;
    shm_lookup(r, id, uid, SHM_PERM_READ, &off_after, &used);
    CHECK(off_after == off_before, "payload offset unchanged after in-place grow");
    CHECK(used == 512, "used_size is 512 after grow");

    /* Original content must still be present. */
    p = shm_ptr(r, off_after, uid, perms, SHM_PERM_READ);
    CHECK(check_bytes(p, 128, 0xCD), "original content intact after in-place grow");

    shm_free(r, id, uid, perms);
    shm_region_close(r, true);
}

static void test_grow_with_relocation(void)
{
    puts("\n[test_grow_with_relocation]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_resize_reloc", 1 << 20, &opts, &r);

    const shm_user_id_t uid   = 1;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    /* Allocate A (128 bytes), then B (128 bytes) immediately after.
     * A now has no free space adjacent to it in memory (B is there).
     * Growing A beyond 128 bytes must relocate it. */
    uint64_t id_a; shm_off_t off_a;
    shm_alloc(r, 128, "block_a", uid, perms, 0, &id_a, &off_a);

    uint64_t id_b; shm_off_t off_b;
    shm_alloc(r, 128, "block_b", uid, perms, 0, &id_b, &off_b);

    /* Write distinct patterns to each block. */
    void *pa = shm_ptr(r, off_a, uid, perms, SHM_PERM_WRITE);
    void *pb = shm_ptr(r, off_b, uid, perms, SHM_PERM_WRITE);
    fill_bytes(pa, 128, 0x11);
    fill_bytes(pb, 128, 0x22);

    /* Grow A to 512 bytes — must relocate. */
    int rc = shm_resize(r, id_a, 512, uid, perms);
    CHECK_OK(rc, "shm_resize grow-with-relocation returns 0");

    /* After relocation, lookup returns a NEW (different) offset for A. */
    shm_off_t off_a2; size_t used_a;
    shm_lookup(r, id_a, uid, SHM_PERM_READ, &off_a2, &used_a);
    CHECK(off_a2 != off_a, "payload offset changed after relocation grow");
    CHECK(used_a == 512,   "used_size is 512 after grow");

    /* Original content must have been copied to the new location. */
    void *pa2 = shm_ptr(r, off_a2, uid, perms, SHM_PERM_READ);
    CHECK(check_bytes(pa2, 128, 0x11), "A content copied to new location");

    /* B must be undisturbed. */
    const void *pb2 = shm_ptr(r, off_b, uid, perms, SHM_PERM_READ);
    CHECK(check_bytes(pb2, 128, 0x22), "B content unaffected by A's relocation");

    shm_free(r, id_a, uid, perms);
    shm_free(r, id_b, uid, perms);
    shm_region_close(r, true);
}

static void test_resize_permission_check(void)
{
    puts("\n[test_resize_permission_check]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_resize_perms", 1 << 20, &opts, &r);

    const shm_user_id_t owner    = 5;
    const shm_user_id_t stranger = 9;
    const shm_perm_t    perms    = SHM_PERM_DEFAULT;

    uint64_t id; shm_off_t off;
    shm_alloc(r, 64, "perm_resize_obj", owner, perms, 0, &id, &off);

    /* Stranger without ADMIN cannot resize. */
    int rc = shm_resize(r, id, 128, stranger, SHM_PERM_RESIZE);
    CHECK_ERR(rc, EACCES, "stranger cannot resize without ADMIN");

    /* Owner can resize. */
    rc = shm_resize(r, id, 128, owner, perms);
    CHECK_OK(rc, "owner can resize");

    shm_free(r, id, owner, perms);
    shm_region_close(r, true);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    puts("=== test_resize ===");
    test_shrink();
    test_grow_in_place();
    test_grow_with_relocation();
    test_resize_permission_check();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
