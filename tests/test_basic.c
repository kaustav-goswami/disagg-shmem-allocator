/**
 * @file  test_basic.c
 * @brief Basic allocator tests: alloc, lookup by ID and name, permissions, stats.
 *
 * All tests use the POSIX shm_open backend.  A unique shm name is used so
 * parallel test runs do not interfere.
 *
 * Exit codes:
 *   0 = all tests passed
 *   1 = at least one test failed
 */

#define _POSIX_C_SOURCE 200809L  /* for shm_open */

#include "shm_alloc.h"

#include <errno.h>     /* EACCES, ENOENT */
#include <stdio.h>     /* printf, fprintf */
#include <string.h>    /* memset, strcmp */
#include <unistd.h>    /* unlink */

/* ── Minimal test framework ──────────────────────────────────────────────── */

/** Running count of failures (set to 1 on the first failure). */
static int g_failed = 0;

/**
 * Check that condition @p cond is true.
 * Prints PASS/FAIL with the condition text and increments g_failed on failure.
 */
#define CHECK(cond, desc)                                               \
    do {                                                                \
        if (cond) {                                                     \
            printf("  PASS: %s\n", desc);                              \
        } else {                                                        \
            fprintf(stderr, "  FAIL [%s:%d]: %s\n",                   \
                    __FILE__, __LINE__, desc);                          \
            g_failed = 1;                                              \
        }                                                               \
    } while (0)

/** Convenience: check that an int rc equals 0. */
#define CHECK_OK(rc, desc)  CHECK((rc) == 0, desc)
/** Convenience: check that an int rc equals an expected errno. */
#define CHECK_ERR(rc, expected, desc)  CHECK((rc) == (expected), desc)

/* ── Test data type ───────────────────────────────────────────────────────── */

/** Simple POD struct used as the shared object payload. */
typedef struct {
    int   id;           /* numeric identifier */
    float value;        /* floating-point field */
    char  label[24];    /* string field */
} record_t;

/* ── Individual tests ────────────────────────────────────────────────────── */

static void test_open_close(void)
{
    puts("\n[test_open_close]");

    shm_region_t *r = NULL;

    /* Create a fresh region with default options (POSIX backend, 256 dir slots). */
    shm_region_open_opts_t opts = {
        .backend      = SHM_BACKEND_POSIX,
        .flags        = SHM_OPEN_CREATE,  /* initialise a new region */
        .dir_capacity = 64,               /* small directory for this test */
        .mode         = 0660,
    };
    int rc = shm_region_open("/shm_test_basic", 1 << 20, &opts, &r);
    CHECK_OK(rc, "shm_region_open create returns 0");
    CHECK(r != NULL, "region handle is non-NULL");

    /* The base pointer must be non-NULL and mapped. */
    CHECK(shm_region_base(r) != NULL, "region base is non-NULL");
    CHECK(shm_region_size(r) == (1u << 20), "region size matches requested size");

    /* Pool starts empty. */
    shm_pool_stats_t st;
    shm_pool_stats(r, &st);
    CHECK(st.block_count == 0,  "initially no live blocks");
    CHECK(st.used_bytes  == 0,  "initially zero used bytes");
    CHECK(st.free_bytes  > 0,   "initially some free bytes");
    CHECK(st.next_id     == 1,  "monotonic ID starts at 1");
    CHECK(st.host_count  == 1,  "one host attached after open");

    /* Open the same region a second time without SHM_OPEN_CREATE. */
    shm_region_open_opts_t opts2 = { .backend = SHM_BACKEND_POSIX, .flags = 0 };
    shm_region_t *r2 = NULL;
    rc = shm_region_open("/shm_test_basic", 0, &opts2, &r2);
    CHECK_OK(rc, "second open (non-create) returns 0");
    /* Both handles see the same host_count increment. */
    shm_pool_stats(r2, &st);
    CHECK(st.host_count == 2, "host_count is 2 after second open");

    shm_region_close(r2, false);  /* close second handle; do not unlink */
    shm_region_close(r,  true);   /* close first handle; unlink the name */
}

static void test_alloc_and_lookup(void)
{
    puts("\n[test_alloc_and_lookup]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_lookup", 1 << 20, &opts, &r);

    const shm_user_id_t uid   = 42;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    /* Allocate a record_t under the name "my_record". */
    uint64_t  id  = 0;
    shm_off_t off = SHM_OFF_NULL;
    int rc = shm_alloc_typed(r, record_t, "my_record", uid, perms, &id, &off);
    CHECK_OK(rc,          "shm_alloc_typed returns 0");
    CHECK(id  != 0,       "assigned ID is non-zero");
    CHECK(off != SHM_OFF_NULL, "assigned offset is valid");

    /* Write data through a mutable pointer. */
    record_t *p = shm_cast_mut(r, off, record_t, uid, perms);
    CHECK(p != NULL, "shm_cast_mut returns non-NULL");
    p->id    = 100;
    p->value = 3.14f;
    snprintf(p->label, sizeof(p->label), "hello-shm");

    /* Read it back through a const pointer. */
    const record_t *q = shm_cast(r, off, record_t, uid, SHM_PERM_READ);
    CHECK(q != NULL,           "shm_cast returns non-NULL");
    CHECK(q->id == 100,        "id field matches");
    CHECK(q->value == 3.14f,   "value field matches");
    CHECK(strcmp(q->label, "hello-shm") == 0, "label field matches");

    /* Lookup by ID returns the same offset. */
    shm_off_t lookup_off = SHM_OFF_NULL;
    rc = shm_lookup(r, id, uid, SHM_PERM_READ, &lookup_off, NULL);
    CHECK_OK(rc, "shm_lookup by id returns 0");
    CHECK(lookup_off == off, "lookup by id returns same offset");

    /* Lookup by name. */
    uint64_t  found_id  = 0;
    shm_off_t found_off = SHM_OFF_NULL;
    rc = shm_lookup_by_name(r, "my_record", uid, SHM_PERM_READ, &found_id, &found_off);
    CHECK_OK(rc, "shm_lookup_by_name returns 0");
    CHECK(found_id  == id,  "lookup by name returns same id");
    CHECK(found_off == off, "lookup by name returns same offset");

    /* Iterate directory: should see exactly 1 live entry. */
    uint32_t cursor = 0;
    shm_dir_entry_t entry;
    rc = shm_dir_next(r, &cursor, &entry);
    CHECK_OK(rc, "shm_dir_next finds first entry");
    CHECK(entry.id == id,                           "dir entry has correct id");
    CHECK(strcmp(entry.name, "my_record") == 0,     "dir entry has correct name");
    CHECK(entry.owner == uid,                       "dir entry has correct owner");
    CHECK(entry.capacity == sizeof(record_t),       "dir entry capacity == sizeof(record_t)");
    rc = shm_dir_next(r, &cursor, &entry);
    CHECK_ERR(rc, ENOENT, "shm_dir_next returns ENOENT after last entry");

    /* Stats: one live block. */
    shm_pool_stats_t st;
    shm_pool_stats(r, &st);
    CHECK(st.block_count == 1, "pool has one live block");
    CHECK(st.dir_used    == 1, "directory has one live entry");

    /* Free the object. */
    rc = shm_free_id(r, id, uid, perms);
    CHECK_OK(rc, "shm_free_id returns 0");

    shm_pool_stats(r, &st);
    CHECK(st.block_count == 0, "pool has no blocks after free");
    CHECK(st.dir_used    == 0, "directory is empty after free");

    /* Lookup after free must return ENOENT. */
    rc = shm_lookup(r, id, uid, SHM_PERM_READ, NULL, NULL);
    CHECK_ERR(rc, ENOENT, "lookup after free returns ENOENT");

    shm_region_close(r, true);
}

static void test_permissions(void)
{
    puts("\n[test_permissions]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE
    };
    shm_region_open("/shm_test_perms", 1 << 20, &opts, &r);

    const shm_user_id_t owner   = 10;    /* owning user */
    const shm_user_id_t stranger = 99;   /* different user with no admin bit */

    uint64_t id; shm_off_t off;
    shm_alloc_typed(r, record_t, "perms_obj", owner, SHM_PERM_DEFAULT, &id, &off);

    /* Stranger cannot read without SHM_PERM_ADMIN. */
    void *p = shm_ptr(r, off, stranger, SHM_PERM_READ, SHM_PERM_READ);
    CHECK(p == NULL, "stranger cannot read owner's block (EACCES expected)");

    /* Stranger with ADMIN can read. */
    p = shm_ptr(r, off, stranger, SHM_PERM_READ | SHM_PERM_ADMIN, SHM_PERM_READ);
    CHECK(p != NULL, "admin-privileged stranger can read the block");

    /* Owner without FREE bit in caller_perms cannot free. */
    int rc = shm_free(r, id, owner, SHM_PERM_READ);  /* missing FREE bit */
    CHECK_ERR(rc, EACCES, "owner without FREE caller bit cannot free");

    /* Owner with correct perms can free. */
    rc = shm_free(r, id, owner, SHM_PERM_DEFAULT);
    CHECK_OK(rc, "owner with SHM_PERM_DEFAULT can free");

    shm_region_close(r, true);
}

static void test_multiple_objects(void)
{
    puts("\n[test_multiple_objects]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX, .flags = SHM_OPEN_CREATE,
        .dir_capacity = 16  /* intentionally small to test fill */
    };
    shm_region_open("/shm_test_multi", 1 << 20, &opts, &r);

    const shm_user_id_t uid = 1;

    /* Allocate 5 different objects with distinct IDs. */
    uint64_t  ids[5];
    shm_off_t offs[5];
    for (int i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "obj_%d", i);
        int rc = shm_alloc_typed(r, record_t, name, uid, SHM_PERM_DEFAULT,
                                 &ids[i], &offs[i]);
        CHECK_OK(rc, "alloc object in loop");

        /* Write a distinguishing value. */
        record_t *p = shm_cast_mut(r, offs[i], record_t, uid, SHM_PERM_DEFAULT);
        p->id = i * 10;
    }

    /* Verify IDs are strictly monotonically increasing. */
    for (int i = 1; i < 5; i++)
        CHECK(ids[i] == ids[i-1] + 1, "IDs are monotonically increasing");

    /* All 5 objects readable by ID. */
    for (int i = 0; i < 5; i++) {
        shm_off_t o;
        int rc = shm_lookup(r, ids[i], uid, SHM_PERM_READ, &o, NULL);
        CHECK_OK(rc, "lookup each allocated object");
        const record_t *q = shm_cast(r, o, record_t, uid, SHM_PERM_READ);
        CHECK(q->id == i * 10, "object contains correct written value");
    }

    /* Free every other object; confirm stats. */
    for (int i = 0; i < 5; i += 2)
        shm_free(r, ids[i], uid, SHM_PERM_DEFAULT);

    shm_pool_stats_t st;
    shm_pool_stats(r, &st);
    CHECK(st.block_count == 2, "2 blocks remain after freeing 3 of 5");

    /* Remaining objects still readable. */
    for (int i = 1; i < 5; i += 2) {
        shm_off_t o;
        int rc = shm_lookup(r, ids[i], uid, SHM_PERM_READ, &o, NULL);
        CHECK_OK(rc, "odd-indexed object still accessible after freeing evens");
    }

    /* Free remaining objects. */
    for (int i = 1; i < 5; i += 2)
        shm_free(r, ids[i], uid, SHM_PERM_DEFAULT);

    shm_pool_stats(r, &st);
    CHECK(st.block_count == 0, "all blocks freed; pool empty");

    /* After all frees, the pool should have coalesced back to a single block
     * with the full heap available (modulo block header overhead). */
    CHECK(st.used_bytes == 0, "used_bytes is 0 after all frees");

    shm_region_close(r, true);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    puts("=== test_basic ===");
    test_open_close();
    test_alloc_and_lookup();
    test_permissions();
    test_multiple_objects();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
