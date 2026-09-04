/**
 * @file  test_bounds.c
 * @brief Tests for the bounds-checked offset -> block-header conversion
 *        (shm_blk_of_poff_checked) used by shm_free, shm_resize, shm_ptr,
 *        shm_block_info, and shm_block_set_perms.
 *
 * Every shm_off_t that reaches these functions is either supplied directly
 * by a caller (shm_ptr, shm_block_info, shm_block_set_perms) or read back
 * out of a directory entry (shm_free, shm_resize).  In both cases the value
 * may be stale, corrupted, or simply wrong.  Before the bounds check was
 * added, converting such an offset into a block-header pointer was raw
 * arithmetic that could land outside the mapped heap and get dereferenced
 * immediately -- a segfault/SIGBUS rather than a clean error return.  These
 * tests exercise that boundary directly: they use the internal region layout
 * (src/shm_internal.h) to construct offsets and directory entries that used
 * to be unsafe, and confirm the allocator now fails cleanly with EFAULT /
 * EINVAL / NULL instead.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_alloc.h"
#include "shm_internal.h"   /* shm_rh, shm_dir_slot, shm_heap_size, shm_blk_of_poff */

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ── Minimal test framework (same as the other test files) ──────────────── */

static int g_failed = 0;

#define CHECK(cond, desc) \
    do { \
        if (cond) { printf("  PASS: %s\n", desc); } \
        else { fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, desc); \
               g_failed = 1; } \
    } while (0)

#define CHECK_OK(rc, desc)        CHECK((rc) == 0, desc)
#define CHECK_ERR(rc, exp, desc)  CHECK((rc) == (exp), desc)

typedef struct {
    int  id;
    char payload[64];
} record_t;

/** Find the live directory slot for obj_id (internal, test-only helper). */
static shm_obj_entry_t *find_slot(shm_region_t *r, uint64_t obj_id)
{
    shm_region_hdr_t *rh = shm_rh(r);
    for (uint32_t i = 0; i < rh->dir_capacity; i++) {
        shm_obj_entry_t *e = shm_dir_slot(r, i);
        if (e->alive && e->id == obj_id)
            return e;
    }
    return NULL;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/**
 * shm_ptr / shm_block_info / shm_block_set_perms all take a caller-supplied
 * shm_off_t directly.  None of these should ever crash, no matter how far
 * out of range the offset is.
 */
static void test_read_path_rejects_bad_offsets(void)
{
    puts("\n[test_read_path_rejects_bad_offsets]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_bounds_read", 1 << 20, &opts, &r);

    const shm_user_id_t uid   = 1;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    uint64_t  id  = 0;
    shm_off_t off = SHM_OFF_NULL;
    shm_alloc_typed(r, record_t, "obj", uid, perms, &id, &off);

    size_t heap_bytes = shm_heap_size(r);

    /* The null sentinel. */
    void *p = shm_ptr(r, SHM_OFF_NULL, uid, perms, SHM_PERM_READ);
    CHECK(p == NULL, "shm_ptr(SHM_OFF_NULL) returns NULL, does not crash");

    /* Smaller than a block header: cannot possibly precede a real block. */
    p = shm_ptr(r, 0, uid, perms, SHM_PERM_READ);
    CHECK(p == NULL, "shm_ptr(0) returns NULL");

    /* Just past the end of the mapped heap. */
    p = shm_ptr(r, (shm_off_t)heap_bytes + 1, uid, perms, SHM_PERM_READ);
    CHECK(p == NULL, "shm_ptr(heap_size + 1) returns NULL, does not crash");

    /* Wildly out of range -- larger than the entire address space could
     * plausibly map for this region. */
    p = shm_ptr(r, (shm_off_t)heap_bytes * 1000000ULL, uid, perms, SHM_PERM_READ);
    CHECK(p == NULL, "shm_ptr(huge offset) returns NULL, does not crash");

    /* Same battery against shm_block_info(). */
    size_t cap;
    int rc = shm_block_info(r, SHM_OFF_NULL, &cap, NULL, NULL, NULL, NULL);
    CHECK_ERR(rc, EINVAL, "shm_block_info(SHM_OFF_NULL) returns EINVAL");
    rc = shm_block_info(r, (shm_off_t)heap_bytes * 1000000ULL, &cap, NULL, NULL, NULL, NULL);
    CHECK_ERR(rc, EINVAL, "shm_block_info(huge offset) returns EINVAL, does not crash");

    /* Same battery against shm_block_set_perms(). */
    rc = shm_block_set_perms(r, (shm_off_t)heap_bytes + 1, uid, perms, SHM_PERM_READ);
    CHECK_ERR(rc, EINVAL, "shm_block_set_perms(out-of-range offset) returns EINVAL");

    /* The legitimate offset still works after all of the above abuse. */
    const record_t *q = shm_cast(r, off, record_t, uid, SHM_PERM_READ);
    CHECK(q != NULL, "valid offset still resolves correctly");

    shm_region_close(r, true);
}

/**
 * A corrupted header that claims a total_size larger than the remaining
 * heap must be rejected even though its magic and immediate bounds look
 * fine -- this is the "poisoned header" branch of the bounds check.
 */
static void test_rejects_corrupted_total_size(void)
{
    puts("\n[test_rejects_corrupted_total_size]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_bounds_corrupt", 1 << 20, &opts, &r);

    const shm_user_id_t uid   = 1;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    uint64_t  id  = 0;
    shm_off_t off = SHM_OFF_NULL;
    shm_alloc_typed(r, record_t, "obj", uid, perms, &id, &off);

    /* Reach past the public API to poison the block header directly --
     * this simulates memory corruption / a torn write from another host. */
    shm_block_hdr_t *b = shm_blk_of_poff(r, off);
    size_t original_total_size = b->total_size;
    b->total_size = shm_heap_size(r) * 2;   /* claims to overrun the heap */

    void *p = shm_ptr(r, off, uid, perms, SHM_PERM_READ);
    CHECK(p == NULL, "shm_ptr rejects a block whose total_size overruns the heap");

    /* Restore the header so the region can be torn down cleanly. */
    b->total_size = original_total_size;

    shm_region_close(r, true);
}

/**
 * shm_free()/shm_resize() trust the directory entry's stored offset.  If
 * that offset has been corrupted (bad metadata write, torn read on a
 * disaggregated/DAX host, etc.) they must fail with EFAULT instead of
 * turning the bad offset into a wild pointer.
 */
static void test_free_and_resize_reject_corrupted_directory_offset(void)
{
    puts("\n[test_free_and_resize_reject_corrupted_directory_offset]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_bounds_dir", 1 << 20, &opts, &r);

    const shm_user_id_t uid   = 1;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    uint64_t  id  = 0;
    shm_off_t off = SHM_OFF_NULL;
    shm_alloc_typed(r, record_t, "obj", uid, perms, &id, &off);

    shm_obj_entry_t *slot = find_slot(r, id);
    CHECK(slot != NULL, "test can locate the directory slot for the new object");

    shm_off_t original_off = slot->off;
    slot->off = (shm_off_t)shm_heap_size(r) * 1000000ULL;  /* corrupt it */

    int rc = shm_free(r, id, uid, perms);
    CHECK_ERR(rc, EFAULT, "shm_free rejects a directory entry with a corrupted offset");

    /* The slot must still be intact (shm_free returned before clearing it),
     * so restoring the offset and freeing again must succeed normally. */
    slot->off = original_off;
    rc = shm_free(r, id, uid, perms);
    CHECK_OK(rc, "shm_free succeeds once the offset is restored");

    /* Same battery against shm_resize(). */
    shm_alloc_typed(r, record_t, "obj2", uid, perms, &id, &off);
    slot = find_slot(r, id);
    original_off = slot->off;
    slot->off = (shm_off_t)shm_heap_size(r) * 1000000ULL;

    rc = shm_resize(r, id, 128, uid, perms);
    CHECK_ERR(rc, EFAULT, "shm_resize rejects a directory entry with a corrupted offset");

    slot->off = original_off;
    rc = shm_resize(r, id, 128, uid, perms);
    CHECK_OK(rc, "shm_resize succeeds once the offset is restored");

    shm_region_close(r, true);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    puts("=== test_bounds ===");
    test_read_path_rejects_bad_offsets();
    test_rejects_corrupted_total_size();
    test_free_and_resize_reject_corrupted_directory_offset();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
