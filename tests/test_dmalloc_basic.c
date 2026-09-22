/**
 * @file  test_dmalloc_basic.c
 * @brief Sanity test for the v0.3 formal API (src/dmalloc.c) directly,
 *        ahead of building the larger STREAM programs on it.
 *
 * d_init() takes a caller-opened fd, not a name -- this test does the
 * shm_open()+ftruncate() itself (exactly what a real caller must do
 * before calling d_init()) and hands the fd over.
 */

#define _POSIX_C_SOURCE 200809L

#include "dmalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_failed = 0;

#define CHECK(cond, desc) \
    do { \
        if (cond) { printf("  PASS: %s\n", desc); } \
        else { fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, desc); \
               g_failed = 1; } \
    } while (0)

#define CHECK_OK(rc, desc)   CHECK((rc) == 0, desc)

typedef struct { int id; char label[16]; } record_t;

int main(void)
{
    puts("=== test_dmalloc_basic ===");

    const char *shm_name = "/dmalloc_test_basic";
    shm_unlink(shm_name);

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0660);
    CHECK(fd >= 0, "test opens its own POSIX shm fd (d_init's caller-owns-the-fd contract)");
    CHECK(ftruncate(fd, 1 << 20) == 0, "test sizes the fd itself before calling d_init");

    handle_t *h = d_init(fd, D_CREATE, 42u);
    CHECK(h != NULL, "d_init creates a pool over the caller-provided fd");
    CHECK(d_handle_key(h) == 42u, "d_handle_key reports back the key given to d_init");

    record_t local = { 7, "hello" };
    bool ok = d_publish(h, "obj1", &local, sizeof(local), d_handle_key(h));
    CHECK(ok, "d_publish registers a new named object and copies its contents");

    ok = d_publish(h, "obj1", &local, sizeof(local), d_handle_key(h));
    CHECK(!ok, "publishing a duplicate name fails");

    CHECK(d_lookup(h, "obj1", 0), "d_lookup finds the published name");
    CHECK(!d_lookup(h, "nope", 0), "d_lookup correctly reports a nonexistent name");

    record_t *p = (record_t *)d_attach(h, "obj1", 0);
    CHECK(p != NULL, "d_attach resolves the name to a pointer");
    CHECK(p && p->id == 7 && strcmp(p->label, "hello") == 0,
          "the attached pointer sees exactly what was published");

    CHECK(d_attach(h, "nope", 0) == NULL, "d_attach on a nonexistent name returns NULL");

    CHECK(d_detach(h, p), "d_detach validates a pointer this handle actually attached");
    CHECK(!d_detach(h, (void *)0x1), "d_detach rejects a pointer outside the mapped heap");

    /* Parallel-lookup addendum (dmalloc.h, point 9): d_attach_slot() /
     * d_name_capacity() let a caller rediscover every named object by
     * scanning physical name-table slots instead of searching by name --
     * this is what lets independent threads partition the table instead of
     * every d_attach() call serialising behind the engine's one mutex. */
    record_t local2 = { 8, "obj2" };
    record_t local3 = { 9, "obj3" };
    CHECK(d_publish(h, "obj2", &local2, sizeof(local2), d_handle_key(h)),
          "publish a second named object ahead of the slot-scan test");
    CHECK(d_publish(h, "obj3", &local3, sizeof(local3), d_handle_key(h)),
          "publish a third named object ahead of the slot-scan test");

    uint32_t cap = d_name_capacity(h);
    CHECK(cap > 0, "d_name_capacity reports a nonzero name-table capacity");

    void *dummy_ptr = NULL;
    int rc_out_of_range = d_attach_slot(h, cap, 0, NULL, 0, &dummy_ptr);
    CHECK(rc_out_of_range == ERANGE,
          "d_attach_slot rejects a slot_index at or beyond d_name_capacity");

    int found_obj1 = 0, found_obj2 = 0, found_obj3 = 0, found_other = 0, empty_slots = 0;
    for (uint32_t i = 0; i < cap; ++i) {
        char name[32];
        void *ptr = NULL;
        int rc = d_attach_slot(h, i, 0, name, sizeof(name), &ptr);
        if (rc == ENOENT) { empty_slots++; continue; }
        CHECK_OK(rc, "d_attach_slot succeeds on every occupied slot it reports");
        if (strcmp(name, "obj1") == 0) {
            found_obj1++;
            CHECK(((record_t *)ptr)->id == 7, "slot-scanned obj1 sees its published contents");
        } else if (strcmp(name, "obj2") == 0) {
            found_obj2++;
            CHECK(((record_t *)ptr)->id == 8, "slot-scanned obj2 sees its published contents");
        } else if (strcmp(name, "obj3") == 0) {
            found_obj3++;
            CHECK(((record_t *)ptr)->id == 9, "slot-scanned obj3 sees its published contents");
        } else {
            found_other++;
        }
    }
    CHECK(found_obj1 == 1 && found_obj2 == 1 && found_obj3 == 1 && found_other == 0,
          "scanning every slot rediscovers exactly the three published objects, once each");
    CHECK((uint32_t)empty_slots == cap - 3, "every other slot in the table reports ENOENT (unused)");

    CHECK(d_delete(h, "obj2", d_handle_key(h)) == 0, "cleanup: delete obj2 before the rest of the test");
    CHECK(d_delete(h, "obj3", d_handle_key(h)) == 0, "cleanup: delete obj3 before the rest of the test");

    ok = d_namespace_bind(h, d_handle_key(h));
    CHECK(ok, "d_namespace_bind confines this handle's own key to the caller's namespace");
    ok = d_namespace_bind(h, 999u);
    CHECK(!ok, "d_namespace_bind refuses to bind a key that isn't this handle's own");
    /* Same-namespace process (this one) must still be able to attach. */
    CHECK(d_attach(h, "obj1", 0) != NULL,
          "namespace-bound key still resolves from within the bound namespace");

    ok = d_revoke(h, "obj1", 7 /* host_id, logging only */, 99u);
    CHECK(ok, "d_revoke rotates the object's key");
    CHECK(d_attach(h, "obj1", 0) == NULL,
          "the revoking handle's own (now-stale) key no longer attaches");
    CHECK(d_handle_key(h) == 42u,
          "revoke does not silently change the handle's own identity (see dmalloc.h)");

    /* A second handle, attached (not created) over the same fd, acting as
     * the new key -- this is how you'd continue using the object post-revoke. */
    handle_t *h2 = d_init(fd, 0 /* attach */, 99u);
    CHECK(h2 != NULL, "a second handle can attach to the same fd as a different key");
    void *p2 = d_attach(h2, "obj1", 0);
    CHECK(p2 != NULL, "the new key attaches successfully after revoke");

    int rc = d_delete(h2, "obj1", 99u);
    CHECK_OK(rc, "d_delete removes the object under its current (post-revoke) key");
    CHECK(d_lookup(h2, "obj1", 0) == false, "the name is gone after delete");

    d_deinit(h2);
    d_deinit(h);
    close(fd);
    shm_unlink(shm_name);

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
