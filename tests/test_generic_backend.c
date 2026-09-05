/**
 * @file  test_generic_backend.c
 * @brief Exercises SHM_BACKEND_GENERIC / SHM_SLIM_BACKEND_GENERIC for both
 *        v2 and v0.2 "slim".
 *
 * This binary is built and run unconditionally (no GENERIC_ALLOC needed):
 *
 *   - Built with no GENERIC_ALLOC selected, it proves the generic backend
 *     fails *cleanly* (ENOMEM from the no-op stub in shm_generic_backend.h)
 *     rather than crashing or silently succeeding with garbage memory.
 *   - Built with `make GENERIC_ALLOC=jemalloc` (or cxlalloc/tcmalloc/
 *     ralloc), it proves the region is actually usable end to end --
 *     alloc, write, read back, free -- backed by that real allocator, and
 *     that attaching a second process to it correctly returns ENOTSUP
 *     (see shm_generic_backend.h for why that's a real limitation, not a
 *     missing feature).
 */

#define _POSIX_C_SOURCE 200809L

/* This file includes both engines' headers side by side; shm_slim.h's
 * v2-compatible API section (shm_alloc()/shm_ptr()/etc. with v2's exact
 * names) would otherwise collide with shm_alloc.h's real declarations of
 * those same names. See the comment above SHM_SLIM_NO_V2_COMPAT in
 * shm_slim.h. */
#define SHM_SLIM_NO_V2_COMPAT
#include "shm_alloc.h"
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

#if defined(SHM_GENERIC_JEMALLOC) || defined(SHM_GENERIC_TCMALLOC) || \
    defined(SHM_GENERIC_CXLALLOC) || defined(SHM_GENERIC_RALLOC)
#define A_BACKEND_IS_SELECTED 1
#else
#define A_BACKEND_IS_SELECTED 0
#endif

typedef struct { char payload[64]; } record_t;

static void test_v2_generic_backend(void)
{
    puts("\n[test_v2_generic_backend]");

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = { .backend = SHM_BACKEND_GENERIC, .flags = SHM_OPEN_CREATE };
    int rc = shm_region_open("v2-generic-backend-test", 1 << 20, &opts, &r);

#if A_BACKEND_IS_SELECTED
    CHECK_OK(rc, "v2 region opens against the selected generic backend");
    CHECK(shm_region_base(r) != NULL, "region base is non-NULL");

    uint64_t id; shm_off_t off;
    rc = shm_alloc_typed(r, record_t, NULL, 1u, SHM_PERM_DEFAULT, &id, &off);
    CHECK_OK(rc, "alloc succeeds against the generic backend");
    record_t *p = shm_cast_mut(r, off, record_t, 1u, SHM_PERM_DEFAULT);
    CHECK(p != NULL, "shm_cast_mut resolves a pointer into generic-backend memory");
    memset(p->payload, 0x5A, sizeof(p->payload));
    const record_t *q = shm_cast(r, off, record_t, 1u, SHM_PERM_READ);
    CHECK(q != NULL && (unsigned char)q->payload[0] == 0x5A,
          "data written through the generic backend reads back correctly");
    rc = shm_free_id(r, id, 1u, SHM_PERM_DEFAULT);
    CHECK_OK(rc, "free succeeds against the generic backend");

    shm_region_t *r2 = NULL;
    shm_region_open_opts_t attach_opts = { .backend = SHM_BACKEND_GENERIC };
    rc = shm_region_open("v2-generic-backend-test", 0, &attach_opts, &r2);
    CHECK_ERR(rc, ENOTSUP, "attaching (create unset) to a generic backend returns ENOTSUP");

    shm_region_close(r, true);
#else
    CHECK_ERR(rc, ENOMEM, "with no generic backend selected, open fails cleanly with ENOMEM");
#endif
}

static void test_slim_generic_backend(void)
{
    puts("\n[test_slim_generic_backend]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_GENERIC, .flags = SHM_SLIM_OPEN_CREATE };
    int rc = shm_slim_region_open("slim-generic-backend-test", 1 << 20, &opts, &r);

#if A_BACKEND_IS_SELECTED
    CHECK_OK(rc, "slim region opens against the selected generic backend");
    CHECK(shm_slim_region_base(r) != NULL, "region base is non-NULL");

    shm_off_t off;
    rc = shm_slim_alloc_typed(r, record_t, NULL, 0xCAFEu, &off);
    CHECK_OK(rc, "alloc succeeds against the generic backend");
    record_t *p = shm_slim_cast_mut(r, off, record_t, 0xCAFEu);
    CHECK(p != NULL, "shm_slim_ptr resolves a pointer into generic-backend memory");
    memset(p->payload, 0xA5, sizeof(p->payload));
    const record_t *q = shm_slim_cast(r, off, record_t, 0xCAFEu);
    CHECK(q != NULL && (unsigned char)q->payload[0] == 0xA5,
          "data written through the generic backend reads back correctly");
    rc = shm_slim_free(r, off, 0xCAFEu);
    CHECK_OK(rc, "free succeeds against the generic backend");

    shm_slim_region_t *r2 = NULL;
    shm_slim_region_open_opts_t attach_opts = { .backend = SHM_SLIM_BACKEND_GENERIC };
    rc = shm_slim_region_open("slim-generic-backend-test", 0, &attach_opts, &r2);
    CHECK_ERR(rc, ENOTSUP, "attaching (create unset) to a generic backend returns ENOTSUP");

    shm_slim_region_close(r, true);
#else
    CHECK_ERR(rc, ENOMEM, "with no generic backend selected, open fails cleanly with ENOMEM");
#endif
}

int main(void)
{
    puts("=== test_generic_backend ===");
#if A_BACKEND_IS_SELECTED
    puts("(a generic allocator backend is selected for this build)");
#else
    puts("(no generic allocator backend selected -- verifying the clean-failure path)");
#endif
    test_v2_generic_backend();
    test_slim_generic_backend();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
