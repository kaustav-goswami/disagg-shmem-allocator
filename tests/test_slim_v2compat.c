/**
 * @file  test_slim_v2compat.c
 * @brief Exercises the v2-compatible API (bottom of include/shm_slim.h,
 *        bodies in src/shm_slim_compat.c) directly -- proves that code
 *        written against v2's exact names/signatures (shm_region_t,
 *        shm_alloc, shm_ptr, shm_free, shm_resize, shm_pool_stats, ...)
 *        compiles and runs correctly when it is the slim engine underneath.
 *
 * Deliberately links shm_slim.o + shm_slim_compat.o + shm_ns.o -- NOT
 * shm_alloc.o -- see the file header comment in shm_slim_compat.c for why
 * those two must never be linked into the same binary.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_slim.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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
    int   id;
    float value;
    char  label[24];
} record_t;

static void test_v2_names_on_the_slim_engine(void)
{
    puts("\n[test_v2_names_on_the_slim_engine]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {0};
    opts.backend      = SHM_BACKEND_POSIX;
    opts.flags        = SHM_OPEN_CREATE;
    opts.dir_capacity = 999999;   /* must be accepted and ignored, not sized to */

    int rc = shm_region_open("/shm_slim_v2compat_test", 1 << 20, &opts, &r);
    CHECK_OK(rc, "shm_region_open (v2 name) succeeds against the slim engine");
    CHECK(r != NULL, "region handle is non-NULL");
    CHECK(shm_region_base(r) != NULL, "shm_region_base returns non-NULL");
    CHECK(shm_region_size(r) == (1u << 20), "shm_region_size matches the request");

    const shm_user_id_t uid   = 42;
    const shm_perm_t    perms = SHM_PERM_DEFAULT;

    uint64_t  id  = 0;
    shm_off_t off = SHM_OFF_NULL;
    rc = shm_alloc_typed(r, record_t, "my_record", uid, perms, &id, &off);
    CHECK_OK(rc, "shm_alloc_typed (v2 macro) succeeds");
    CHECK(off != SHM_OFF_NULL, "assigned offset is valid");
    CHECK(id == (uint64_t)off, "synthesized id equals the offset (documented mapping)");

    record_t *p = shm_cast_mut(r, off, record_t, uid, perms);
    CHECK(p != NULL, "shm_cast_mut (v2 macro) resolves a mutable pointer");
    p->id = 7;
    p->value = 3.5f;
    snprintf(p->label, sizeof(p->label), "hello");

    const record_t *q = shm_cast(r, off, record_t, uid, SHM_PERM_READ);
    CHECK(q != NULL && q->id == 7 && q->value == 3.5f &&
          strcmp(q->label, "hello") == 0,
          "shm_cast (v2 macro) reads back the same data");

    void *wrong = shm_ptr(r, off, uid + 1, perms, SHM_PERM_READ);
    CHECK(wrong == NULL, "a different user_id (-> different key) is denied");

    shm_pool_stats_t stats = {0};
    rc = shm_pool_stats(r, &stats);
    CHECK_OK(rc, "shm_pool_stats (v2 name/struct) succeeds");
    CHECK(stats.block_count == 1, "pool_stats reports the one live block");
    CHECK(stats.total_bytes > 0 && stats.total_bytes < shm_region_size(r),
          "pool_stats.total_bytes is populated and smaller than the full mapping");

    /* Shrink in place must succeed and report success (id unchanged). */
    rc = shm_resize(r, id, 8, uid, perms);
    CHECK_OK(rc, "shm_resize shrink-in-place succeeds");

    rc = shm_free_id(r, id, uid, perms);
    CHECK_OK(rc, "shm_free_id (v2 macro) succeeds");

    void *gone = shm_ptr(r, off, uid, perms, SHM_PERM_READ);
    CHECK(gone == NULL, "freed object is no longer accessible");

    shm_region_close(r, true);
}

static void test_resize_relocation_is_reported_not_hidden(void)
{
    puts("\n[test_resize_relocation_is_reported_not_hidden]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {0};
    opts.backend = SHM_BACKEND_POSIX;
    opts.flags   = SHM_OPEN_CREATE;
    shm_region_open("/shm_slim_v2compat_resize", 1 << 20, &opts, &r);

    const shm_user_id_t uid = 1;
    uint64_t  id  = 0;
    shm_off_t off = SHM_OFF_NULL;
    shm_alloc(r, 32, NULL, uid, SHM_PERM_DEFAULT, 0, &id, &off);

    /* Consume the neighboring free space so growth cannot happen in place. */
    uint64_t  filler_id;
    shm_off_t filler_off;
    shm_alloc(r, 128, NULL, uid, SHM_PERM_DEFAULT, 0, &filler_id, &filler_off);

    int rc = shm_resize(r, id, 4096, uid, SHM_PERM_DEFAULT);
    CHECK_ERR(rc, ENOTSUP,
              "a relocating grow is reported as ENOTSUP instead of silently "
              "returning a stale id (documented compat-layer limitation)");

    shm_region_close(r, true);
}

int main(void)
{
    puts("=== test_slim_v2compat ===");
    test_v2_names_on_the_slim_engine();
    test_resize_relocation_is_reported_not_hidden();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
