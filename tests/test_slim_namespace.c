/**
 * @file  test_slim_namespace.c
 * @brief Tests for optional key->namespace binding and shm_slim_set_key().
 *
 * Namespace enforcement is opt-in: an unbound key is always unrestricted
 * (plain key matching only), matching the "don't force it" requirement.
 * Since shm_slim_namespace_bind()/_check() both read the CALLING process's
 * *real* IPC namespace/cgroup, a single-process test can only exercise the
 * "same namespace" (allowed) path organically. To exercise the mismatch
 * (EPERM) path, this reaches into src/shm_slim_internal.h to corrupt a
 * bound entry's ns_inode/cgroup_hash to a value that cannot match reality,
 * the same technique test_slim_bounds.c uses for header corruption.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_slim.h"
#include "shm_slim_internal.h"

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

typedef struct { char payload[32]; } record_t;

/* Test-only: find the raw namespace-table slot bound to `key`, or NULL. */
static shm_slim_namespace_entry_t *find_ns_slot(shm_slim_region_t *r, shm_slim_key_t key)
{
    shm_slim_region_hdr_t *rh = shm_slim_rh(r);
    for (uint32_t i = 0; i < rh->namespace_cap; i++) {
        shm_slim_namespace_entry_t *e = shm_slim_namespace_slot(r, i);
        if ((e->ns_inode != 0 || e->cgroup_hash != 0) && e->key == key)
            return e;
    }
    return NULL;
}

static void test_unbound_key_is_unrestricted(void)
{
    puts("\n[test_unbound_key_is_unrestricted]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_ns_default", 1 << 20, &opts, &r);

    int rc = shm_slim_namespace_check(r, 0xABCDu);
    CHECK_OK(rc, "a key with no namespace binding is always allowed (opt-in, not forced)");

    shm_off_t off;
    shm_slim_alloc_typed(r, record_t, NULL, 0xABCDu, &off);
    CHECK(shm_slim_ptr(r, off, 0xABCDu) != NULL,
          "plain key access still works with zero namespace involvement");

    shm_slim_region_close(r, true);
}

static void test_bind_check_unbind(void)
{
    puts("\n[test_bind_check_unbind]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = {
        .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE,
        .namespace_capacity = 4,
    };
    shm_slim_region_open("/shm_slim_test_ns_bind", 1 << 20, &opts, &r);

    int rc = shm_slim_namespace_bind(r, 0x1111u);
    CHECK_OK(rc, "binding a fresh key succeeds");

    rc = shm_slim_namespace_bind(r, 0x1111u);
    CHECK_ERR(rc, EEXIST, "binding an already-bound key returns EEXIST");

    rc = shm_slim_namespace_check(r, 0x1111u);
    CHECK_OK(rc, "the binding process is, definitionally, in its own namespace");

    rc = shm_slim_namespace_check(r, 0x2222u);
    CHECK_OK(rc, "a different, never-bound key remains unrestricted");

    shm_slim_namespace_unbind(r, 0x1111u);
    rc = shm_slim_namespace_check(r, 0x1111u);
    CHECK_OK(rc, "after unbind, the key reverts to unrestricted");

    /* Unbinding something never bound is not an error (no-op). */
    shm_slim_namespace_unbind(r, 0x9999u);
    CHECK(1, "unbinding a never-bound key is a harmless no-op");

    shm_slim_region_close(r, true);
}

static void test_bind_capacity_and_slots_are_key_scoped_not_object_scoped(void)
{
    puts("\n[test_bind_capacity_and_slots_are_key_scoped_not_object_scoped]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = {
        .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE,
        .namespace_capacity = 2,
    };
    shm_slim_region_open("/shm_slim_test_ns_capacity", 1 << 20, &opts, &r);

    CHECK_OK(shm_slim_namespace_bind(r, 1u), "bind key 1 (slot 1/2)");
    CHECK_OK(shm_slim_namespace_bind(r, 2u), "bind key 2 (slot 2/2)");
    int rc = shm_slim_namespace_bind(r, 3u);
    CHECK_ERR(rc, ENOSPC, "namespace table exhaustion returns ENOSPC");

    /* Many thousands of objects may share key 1 without costing a single
     * extra namespace-table slot -- the table is keyed by KEY, not by
     * object. */
    for (int i = 0; i < 2000; i++) {
        shm_off_t off;
        int arc = shm_slim_alloc(r, 8, NULL, 1u, &off);
        CHECK(arc == 0, "object under an already-bound key allocates normally");
        if (arc != 0) break;
    }
    CHECK_ERR(shm_slim_namespace_bind(r, 3u), ENOSPC,
              "namespace table still full after allocating many objects under key 1");

    shm_slim_region_close(r, true);
}

static void test_namespace_mismatch_denies_access(void)
{
    puts("\n[test_namespace_mismatch_denies_access]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_ns_mismatch", 1 << 20, &opts, &r);

    shm_slim_namespace_bind(r, 0x4242u);

    /* Corrupt the bound entry to a namespace this process cannot possibly
     * be in, simulating "key presented by a process outside the bound
     * namespace" without needing to actually fork into a new namespace. */
    shm_slim_namespace_entry_t *slot = find_ns_slot(r, 0x4242u);
    CHECK(slot != NULL, "test can locate the bound namespace slot");
    uint64_t real_ns_inode = slot->ns_inode;
    slot->ns_inode = real_ns_inode ^ 0xFFFFFFFFFFFFFFFFULL;   /* guaranteed mismatch */

    int rc = shm_slim_namespace_check(r, 0x4242u);
    CHECK_ERR(rc, EPERM, "a namespace-bound key denies a caller outside that namespace");

    slot->ns_inode = real_ns_inode;   /* restore for clean teardown */
    rc = shm_slim_namespace_check(r, 0x4242u);
    CHECK_OK(rc, "restoring the real namespace value allows access again");

    shm_slim_region_close(r, true);
}

static void test_set_key(void)
{
    puts("\n[test_set_key]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_set_key", 1 << 20, &opts, &r);

    shm_off_t off;
    shm_slim_alloc_typed(r, record_t, NULL, 100u, &off);

    int rc = shm_slim_set_key(r, off, 999u /* wrong */, 200u);
    CHECK_ERR(rc, EACCES, "rotating a key requires presenting the CURRENT key");

    rc = shm_slim_set_key(r, off, 100u, 200u);
    CHECK_OK(rc, "rotating with the correct current key succeeds");

    CHECK(shm_slim_ptr(r, off, 100u) == NULL, "the old key no longer works");
    CHECK(shm_slim_ptr(r, off, 200u) != NULL, "the new key works");

    shm_slim_region_close(r, true);
}

static void test_revoke_style_rebind_carries_namespace_forward(void)
{
    puts("\n[test_revoke_style_rebind_carries_namespace_forward]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_ns_rebind", 1 << 20, &opts, &r);

    shm_off_t off;
    shm_slim_alloc_typed(r, record_t, NULL, 10u, &off);
    CHECK_OK(shm_slim_namespace_bind(r, 10u), "bind the object's initial key to a namespace");

    /* This is exactly what a d_revoke(old=10, new=20) implementation must
     * do: rotate the object's key, then carry the namespace binding along
     * so the restriction doesn't silently lapse for whoever gets the new
     * key. */
    CHECK_OK(shm_slim_set_key(r, off, 10u, 20u), "rotate the object's key (revoke)");
    CHECK_OK(shm_slim_namespace_rebind(r, 10u, 20u), "carry the namespace binding to the new key");

    shm_slim_namespace_entry_t *stale = find_ns_slot(r, 10u);
    CHECK(stale == NULL, "the old key's namespace slot no longer exists after rebind");
    shm_slim_namespace_entry_t *moved = find_ns_slot(r, 20u);
    CHECK(moved != NULL, "the binding now lives under the new key");

    int rc = shm_slim_namespace_rebind(r, 999u /* never bound */, 30u);
    CHECK_OK(rc, "rebinding a key with no existing binding is a harmless no-op");
    CHECK(find_ns_slot(r, 30u) == NULL, "the no-op does not fabricate a new binding");

    shm_slim_namespace_bind(r, 40u);
    rc = shm_slim_namespace_rebind(r, 20u, 40u);
    CHECK_ERR(rc, EEXIST, "rebinding onto a key that already has its own distinct binding fails");

    shm_slim_region_close(r, true);
}

/*
 * shm_slim_name_read_slot()/shm_slim_namespace_check_ex() exist so a
 * parallel-attach caller (see dmalloc.h's d_attach_slot()) can partition a
 * table across threads instead of every reader serialising on the region's
 * one mutex. A single-threaded test can't demonstrate the wall-clock win
 * (see WIP.md for that), but it can and must prove LOCKED and LOCK_FREE
 * return identical, correct results -- if they ever disagreed, the
 * "faster" mode would just be silently wrong.
 */
static void test_name_read_slot_matches_locked_and_lock_free(void)
{
    puts("\n[test_name_read_slot_matches_locked_and_lock_free]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = {
        .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE,
        .name_capacity = 8,
    };
    shm_slim_region_open("/shm_slim_test_name_slot", 1 << 20, &opts, &r);

    shm_off_t off_a, off_b;
    shm_slim_alloc_typed(r, record_t, "alpha", 1u, &off_a);
    shm_slim_alloc_typed(r, record_t, "beta", 1u, &off_b);

    shm_off_t ground_truth_a, ground_truth_b;
    CHECK_OK(shm_slim_lookup_by_name(r, "alpha", 1u, &ground_truth_a),
             "sanity: lookup_by_name finds alpha");
    CHECK_OK(shm_slim_lookup_by_name(r, "beta", 1u, &ground_truth_b),
             "sanity: lookup_by_name finds beta");

    /* Requested capacity was 8, but the physical table is inflated beyond
     * that for the hash table's load factor (see shm_slim_name_entry_t's
     * comment) -- ask the region for its actual size instead of assuming
     * physical == requested. */
    shm_slim_pool_stats_t stats;
    shm_slim_pool_stats(r, &stats);
    uint32_t cap = stats.name_capacity;

    int found_alpha = 0, found_beta = 0, found_other = 0, empty = 0;
    for (uint32_t i = 0; i < cap; i++) {
        shm_slim_name_slot_t locked, lock_free;
        int rc_locked = shm_slim_name_read_slot(r, i, SHM_SLIM_READ_LOCKED, &locked);
        int rc_lockfree = shm_slim_name_read_slot(r, i, SHM_SLIM_READ_LOCK_FREE, &lock_free);
        CHECK(rc_locked == rc_lockfree, "LOCKED and LOCK_FREE agree on slot occupancy");

        if (rc_locked == ENOENT) { empty++; continue; }

        CHECK_OK(rc_locked, "an occupied slot reads back 0 in both modes");
        CHECK(strcmp(locked.name, lock_free.name) == 0 && locked.off == lock_free.off,
              "LOCKED and LOCK_FREE agree on the slot's contents");

        if (strcmp(locked.name, "alpha") == 0) {
            found_alpha++;
            CHECK(locked.off == ground_truth_a, "slot-read alpha's offset matches lookup_by_name");
        } else if (strcmp(locked.name, "beta") == 0) {
            found_beta++;
            CHECK(locked.off == ground_truth_b, "slot-read beta's offset matches lookup_by_name");
        } else {
            found_other++;
        }
    }
    CHECK(found_alpha == 1 && found_beta == 1 && found_other == 0,
          "scanning every slot finds exactly alpha and beta, once each");
    CHECK((uint32_t)empty == cap - 2, "every other slot reports ENOENT");

    shm_slim_name_slot_t out;
    CHECK_ERR(shm_slim_name_read_slot(r, cap, SHM_SLIM_READ_LOCKED, &out), ERANGE,
              "slot_index == name_capacity is out of range");
    CHECK_ERR(shm_slim_name_read_slot(r, cap + 1000, SHM_SLIM_READ_LOCK_FREE, &out), ERANGE,
              "a wildly out-of-range slot_index is also rejected, even lock-free");
    CHECK_ERR(shm_slim_name_read_slot(r, 0, (shm_slim_read_mode_t)99, &out), EINVAL,
              "an invalid read mode is rejected");

    shm_slim_region_close(r, true);
}

static void test_namespace_check_ex_matches_locked_and_lock_free(void)
{
    puts("\n[test_namespace_check_ex_matches_locked_and_lock_free]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_ns_check_ex", 1 << 20, &opts, &r);

    CHECK_OK(shm_slim_namespace_check_ex(r, 0x77u, SHM_SLIM_READ_LOCKED),
             "an unbound key is allowed under LOCKED, via the _ex entry point");
    CHECK_OK(shm_slim_namespace_check_ex(r, 0x77u, SHM_SLIM_READ_LOCK_FREE),
             "an unbound key is allowed under LOCK_FREE too");

    shm_slim_namespace_bind(r, 0x77u);
    CHECK_OK(shm_slim_namespace_check_ex(r, 0x77u, SHM_SLIM_READ_LOCKED),
             "a same-namespace caller is allowed under LOCKED");
    CHECK_OK(shm_slim_namespace_check_ex(r, 0x77u, SHM_SLIM_READ_LOCK_FREE),
             "a same-namespace caller is allowed under LOCK_FREE");
    CHECK_OK(shm_slim_namespace_check(r, 0x77u),
             "shm_slim_namespace_check() still works unchanged (thin wrapper over _ex LOCKED)");

    shm_slim_namespace_entry_t *slot = find_ns_slot(r, 0x77u);
    CHECK(slot != NULL, "test can locate the bound namespace slot");
    uint64_t real_ns_inode = slot->ns_inode;
    slot->ns_inode = real_ns_inode ^ 0xFFFFFFFFFFFFFFFFULL;

    CHECK_ERR(shm_slim_namespace_check_ex(r, 0x77u, SHM_SLIM_READ_LOCKED), EPERM,
              "a mismatched namespace denies access under LOCKED");
    CHECK_ERR(shm_slim_namespace_check_ex(r, 0x77u, SHM_SLIM_READ_LOCK_FREE), EPERM,
              "a mismatched namespace denies access under LOCK_FREE too");

    slot->ns_inode = real_ns_inode;

    int rc;
    rc = shm_slim_namespace_check_ex(r, 0x77u, (shm_slim_read_mode_t)99);
    CHECK_ERR(rc, EINVAL, "an invalid read mode is rejected");

    shm_slim_region_close(r, true);
}

int main(void)
{
    puts("=== test_slim_namespace ===");
    test_unbound_key_is_unrestricted();
    test_bind_check_unbind();
    test_bind_capacity_and_slots_are_key_scoped_not_object_scoped();
    test_namespace_mismatch_denies_access();
    test_set_key();
    test_revoke_style_rebind_carries_namespace_forward();
    test_name_read_slot_matches_locked_and_lock_free();
    test_namespace_check_ex_matches_locked_and_lock_free();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
