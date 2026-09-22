/**
 * @file  test_slim_basic.c
 * @brief Basic tests for the v0.2 "slim" allocator: alloc/free/resize,
 *        named vs. anonymous objects, key-based access control, heap-walk
 *        enumeration, pool stats.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_slim.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ── Minimal test framework (same shape as the v2 tests) ─────────────────── */

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

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_open_close(void)
{
    puts("\n[test_open_close]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = {
        .backend = SHM_SLIM_BACKEND_POSIX,
        .flags   = SHM_SLIM_OPEN_CREATE,
        .mode    = 0660,
    };
    int rc = shm_slim_region_open("/shm_slim_test_basic", 1 << 20, &opts, &r);
    CHECK_OK(rc, "shm_slim_region_open create returns 0");
    CHECK(r != NULL, "region handle is non-NULL");
    CHECK(shm_slim_region_base(r) != NULL, "region base is non-NULL");
    CHECK(shm_slim_region_size(r) == (1u << 20), "region size matches requested size");

    shm_slim_pool_stats_t st;
    shm_slim_pool_stats(r, &st);
    CHECK(st.block_count == 0, "initially no live blocks");
    CHECK(st.used_bytes  == 0, "initially zero used bytes");
    CHECK(st.free_bytes  > 0, "initially some free bytes");
    CHECK(st.host_count  == 1, "one host attached after open");
    CHECK(st.name_used   == 0, "initially no named objects");

    shm_slim_region_open_opts_t opts2 = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = 0 };
    shm_slim_region_t *r2 = NULL;
    rc = shm_slim_region_open("/shm_slim_test_basic", 0, &opts2, &r2);
    CHECK_OK(rc, "second open (non-create) returns 0");
    shm_slim_pool_stats(r2, &st);
    CHECK(st.host_count == 2, "host_count is 2 after second open");

    shm_slim_region_close(r2, false);
    shm_slim_region_close(r, true);
}

static void test_anonymous_alloc_and_access(void)
{
    puts("\n[test_anonymous_alloc_and_access]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_anon", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    int rc = shm_slim_alloc_typed(r, record_t, NULL, 0xCAFEu, &off);
    CHECK_OK(rc, "anonymous alloc returns 0");
    CHECK(off != SHM_OFF_NULL, "assigned offset is valid");

    record_t *p = shm_slim_cast_mut(r, off, record_t, 0xCAFEu);
    CHECK(p != NULL, "correct key resolves a mutable pointer");
    p->id = 7;
    p->value = 2.5f;
    snprintf(p->label, sizeof(p->label), "hi");

    const record_t *q = shm_slim_cast(r, off, record_t, 0xCAFEu);
    CHECK(q != NULL && q->id == 7 && q->value == 2.5f && strcmp(q->label, "hi") == 0,
          "read-back through the same key sees written data");

    void *wrong = shm_slim_ptr(r, off, 0xDEADu);
    CHECK(wrong == NULL, "wrong key is denied access");

    size_t sz = 0;
    rc = shm_slim_block_size(r, off, 0xCAFEu, &sz);
    CHECK_OK(rc, "shm_slim_block_size succeeds with correct key");
    CHECK(sz >= sizeof(record_t), "reported size is at least the requested payload");
    rc = shm_slim_block_size(r, off, 0xDEADu, &sz);
    CHECK_ERR(rc, EACCES, "shm_slim_block_size rejects wrong key");

    shm_slim_region_close(r, true);
}

static void test_open_key_has_no_protection(void)
{
    puts("\n[test_open_key_has_no_protection]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_openkey", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc_typed(r, record_t, NULL, SHM_SLIM_KEY_OPEN, &off);

    void *p1 = shm_slim_ptr(r, off, 0x1111u);
    void *p2 = shm_slim_ptr(r, off, 0x2222u);
    CHECK(p1 != NULL && p2 != NULL, "SHM_SLIM_KEY_OPEN grants access regardless of presented key");

    int rc = shm_slim_free(r, off, 0x9999u);
    CHECK_OK(rc, "any key can free an SHM_SLIM_KEY_OPEN object");

    shm_slim_region_close(r, true);
}

static void test_free_and_use_after_free(void)
{
    puts("\n[test_free_and_use_after_free]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_free", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc_typed(r, record_t, NULL, 42u, &off);

    int rc = shm_slim_free(r, off, 41u);
    CHECK_ERR(rc, EACCES, "free with the wrong key is denied");

    rc = shm_slim_free(r, off, 42u);
    CHECK_OK(rc, "free with the correct key succeeds");

    void *p = shm_slim_ptr(r, off, 42u);
    CHECK(p == NULL, "use-after-free is denied (block is no longer live)");

    rc = shm_slim_free(r, off, 42u);
    CHECK_ERR(rc, EINVAL, "double free returns EINVAL (block is not live)");

    shm_slim_region_close(r, true);
}

static void test_named_objects(void)
{
    puts("\n[test_named_objects]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = {
        .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE,
        .name_capacity = 4,
    };
    shm_slim_region_open("/shm_slim_test_names", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    int rc = shm_slim_alloc_typed(r, record_t, "root", 55u, &off);
    CHECK_OK(rc, "named alloc returns 0");

    shm_off_t found = SHM_OFF_NULL;
    rc = shm_slim_lookup_by_name(r, "root", 55u, &found);
    CHECK_OK(rc, "lookup_by_name finds the object with the right key");
    CHECK(found == off, "lookup_by_name returns the same offset alloc gave back");

    rc = shm_slim_lookup_by_name(r, "root", 56u, &found);
    CHECK_ERR(rc, EACCES, "lookup_by_name with the wrong key is denied");

    rc = shm_slim_lookup_by_name(r, "nope", 55u, &found);
    CHECK_ERR(rc, ENOENT, "lookup_by_name for a nonexistent name returns ENOENT");

    /* Duplicate name is rejected. */
    shm_off_t off2 = SHM_OFF_NULL;
    rc = shm_slim_alloc_typed(r, record_t, "root", 55u, &off2);
    CHECK_ERR(rc, EEXIST, "allocating a duplicate name returns EEXIST");

    /* Name table exhaustion. Requested capacity was 4, but the physical
     * table is inflated beyond that to keep the hash table's load factor
     * low (see shm_slim_name_entry_t's comment in shm_slim_internal.h) --
     * ask the region itself how big it actually is rather than assuming
     * physical == requested. */
    shm_slim_pool_stats_t stats;
    shm_slim_pool_stats(r, &stats);
    char name[SHM_SLIM_NAME_MAX];
    int last_rc = 0;
    for (uint32_t i = 0; i < stats.name_capacity + 4; i++) {
        snprintf(name, sizeof(name), "n%u", i);
        shm_off_t o;
        last_rc = shm_slim_alloc_typed(r, record_t, name, 1u, &o);
        if (last_rc != 0) break;
    }
    CHECK_ERR(last_rc, ENOSPC, "name table exhaustion returns ENOSPC");

    /* Name too long. */
    shm_off_t o;
    rc = shm_slim_alloc_typed(r, record_t, "this-name-is-definitely-too-long-for-the-slot", 1u, &o);
    CHECK_ERR(rc, ENAMETOOLONG, "an over-length name is rejected");

    /* Freeing a named object clears its name-table slot. */
    rc = shm_slim_free(r, off, 55u);
    CHECK_OK(rc, "free of a named object succeeds");
    rc = shm_slim_lookup_by_name(r, "root", 55u, &found);
    CHECK_ERR(rc, ENOENT, "the name is no longer resolvable after free");

    shm_slim_region_close(r, true);
}

static void test_resize(void)
{
    puts("\n[test_resize]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_resize", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc(r, 64, "obj", 7u, &off);

    char *p = shm_slim_cast_mut(r, off, char, 7u);
    memset(p, 0xAB, 64);

    /* Shrink. */
    shm_off_t new_off = SHM_OFF_NULL;
    int rc = shm_slim_resize(r, off, 16, 7u, &new_off);
    CHECK_OK(rc, "shrink returns 0");
    CHECK(new_off == off, "shrink does not relocate");
    size_t sz;
    shm_slim_block_size(r, new_off, 7u, &sz);
    CHECK(sz >= 16 && sz < 64, "capacity reflects the shrink");

    /* Grow back in place (adjacent free space from the shrink should be there). */
    rc = shm_slim_resize(r, new_off, 48, 7u, &new_off);
    CHECK_OK(rc, "grow-in-place returns 0");
    p = shm_slim_cast_mut(r, new_off, char, 7u);
    CHECK((unsigned char)p[0] == 0xAB, "original content survives an in-place grow");

    /* Force relocation by growing past what any in-place neighbor can offer. */
    shm_off_t big_off;
    shm_slim_alloc(r, 128, NULL, 1u, &big_off);  /* consume the neighboring free space */
    rc = shm_slim_resize(r, new_off, 4096, 7u, &new_off);
    CHECK_OK(rc, "relocating grow returns 0");
    p = shm_slim_cast_mut(r, new_off, char, 7u);
    CHECK((unsigned char)p[0] == 0xAB, "original content survives a relocating grow");

    /* Wrong key is rejected. */
    rc = shm_slim_resize(r, new_off, 8, 99u, &new_off);
    CHECK_ERR(rc, EACCES, "resize with the wrong key is denied");

    shm_slim_region_close(r, true);
}

static void test_resize_updates_name_table(void)
{
    puts("\n[test_resize_updates_name_table]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_resize_name", 1 << 20, &opts, &r);

    shm_off_t off = SHM_OFF_NULL;
    shm_slim_alloc(r, 32, "movable", 3u, &off);

    /* Eat the neighboring free space so the next grow must relocate. */
    shm_off_t filler;
    shm_slim_alloc(r, 128, NULL, 1u, &filler);

    shm_off_t new_off = SHM_OFF_NULL;
    int rc = shm_slim_resize(r, off, 4096, 3u, &new_off);
    CHECK_OK(rc, "relocating resize of a named object succeeds");
    CHECK(new_off != off, "the object actually moved");

    shm_off_t found = SHM_OFF_NULL;
    rc = shm_slim_lookup_by_name(r, "movable", 3u, &found);
    CHECK_OK(rc, "the name still resolves after relocation");
    CHECK(found == new_off, "the name now points at the new offset");

    shm_slim_region_close(r, true);
}

static void test_walk(void)
{
    puts("\n[test_walk]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_walk", 1 << 20, &opts, &r);

    shm_off_t off_a, off_b, off_c;
    shm_slim_alloc(r, 16, "alpha", 1u, &off_a);
    shm_slim_alloc(r, 32, NULL, 1u, &off_b);
    shm_slim_alloc(r, 8,  "gamma", 1u, &off_c);

    int named_seen = 0, anon_seen = 0, total_seen = 0;
    uint64_t cursor = 0;
    shm_slim_entry_t e;
    while (shm_slim_walk(r, &cursor, &e) == 0) {
        total_seen++;
        if (e.name[0] != '\0') named_seen++;
        else anon_seen++;
    }
    CHECK(total_seen == 3, "walk visits exactly the 3 live objects");
    CHECK(named_seen == 2, "walk reports names for the 2 named objects");
    CHECK(anon_seen == 1, "walk reports an empty name for the anonymous object");

    /* Free the middle one; walk must skip it without breaking the chain. */
    shm_slim_free(r, off_b, 1u);
    total_seen = 0;
    cursor = 0;
    while (shm_slim_walk(r, &cursor, &e) == 0)
        total_seen++;
    CHECK(total_seen == 2, "walk skips freed blocks but still traverses past them");

    shm_slim_region_close(r, true);
}

static void test_pool_stats(void)
{
    puts("\n[test_pool_stats]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = { .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE };
    shm_slim_region_open("/shm_slim_test_stats", 1 << 20, &opts, &r);

    shm_off_t offs[5];
    for (int i = 0; i < 5; i++)
        shm_slim_alloc(r, 32, NULL, 1u, &offs[i]);

    shm_slim_pool_stats_t st;
    shm_slim_pool_stats(r, &st);
    CHECK(st.block_count == 5, "pool_stats reports 5 live blocks");
    /* Each block is 16-byte header + 32-byte payload = 48 bytes, already
     * 8-byte aligned, so 5 objects should account for exactly 240 bytes. */
    CHECK(st.used_bytes == 5 * 48u, "used_bytes matches header+payload exactly, no hidden overhead");

    for (int i = 0; i < 5; i++)
        shm_slim_free(r, offs[i], 1u);
    shm_slim_pool_stats(r, &st);
    CHECK(st.block_count == 0, "pool_stats reports 0 live blocks after freeing all");
    CHECK(st.used_bytes == 0, "used_bytes returns to 0 after freeing all");

    shm_slim_region_close(r, true);
}

/*
 * The name table became an open-addressed hash table (FNV-1a + linear
 * probing + tombstone deletion) to fix the O(name_capacity) per-call cost
 * of the old linear scan -- see WIP.md. That's a correctness-sensitive
 * change: a probe sequence that stops too early at a tombstone, or a
 * delete that clobbers a neighboring entry's reachability, would silently
 * lose or misresolve names. This stress test churns a fixed set of
 * logical slots through many delete+republish-under-a-new-name cycles
 * (the same pattern sh6bench.c's ring buffer produces, at a similarly
 * high load factor relative to the *requested* capacity) and verifies
 * every name's liveness matches what the test itself tracked -- no
 * false hits on deleted names, no lost live ones.
 */
static void test_name_table_hash_stress(void)
{
    puts("\n[test_name_table_hash_stress]");

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = {
        .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE,
        .name_capacity = 32,
    };
    shm_slim_region_open("/shm_slim_test_hash_stress", 4 << 20, &opts, &r);

    const int kSlots = 32;
    char names[32][SHM_SLIM_NAME_MAX];
    bool alive[32];

    int initial_ok = 1;
    for (int i = 0; i < kSlots; i++) {
        snprintf(names[i], sizeof(names[i]), "s%d", i);
        shm_off_t off;
        alive[i] = (shm_slim_alloc_typed(r, record_t, names[i], 1u, &off) == 0);
        if (!alive[i]) initial_ok = 0;
    }
    CHECK(initial_ok, "initial fill: all 32 requested slots published successfully");

    /* Deterministic PRNG (fixed seed) -- reproducible, no real randomness dependency. */
    unsigned seed = 12345u;
    const int kCycles = 500;
    int op_failed = 0;
    for (int cycle = 0; cycle < kCycles && !op_failed; cycle++) {
        for (int i = 0; i < kSlots; i++) {
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) % 3 == 0) {
                if (alive[i]) {
                    if (shm_slim_free_by_name(r, names[i], 1u, NULL) != 0) { op_failed = 1; break; }
                    alive[i] = false;
                }
            } else if (!alive[i]) {
                snprintf(names[i], sizeof(names[i]), "s%d_c%d", i, cycle);
                shm_off_t off;
                if (shm_slim_alloc_typed(r, record_t, names[i], 1u, &off) != 0) { op_failed = 1; break; }
                alive[i] = true;
            }
        }
    }
    CHECK(!op_failed, "500 churn cycles of delete+republish complete without a spurious failure");

    int mismatches = 0;
    for (int i = 0; i < kSlots; i++) {
        shm_off_t off;
        bool found = (shm_slim_lookup_by_name(r, names[i], 1u, &off) == 0);
        if (found != alive[i]) mismatches++;
    }
    CHECK(mismatches == 0,
          "after churn, every tracked name's liveness matches this test's own bookkeeping exactly");

    shm_slim_region_close(r, true);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    puts("=== test_slim_basic ===");
    test_open_close();
    test_anonymous_alloc_and_access();
    test_open_key_has_no_protection();
    test_free_and_use_after_free();
    test_named_objects();
    test_resize();
    test_resize_updates_name_table();
    test_walk();
    test_pool_stats();
    test_name_table_hash_stress();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
