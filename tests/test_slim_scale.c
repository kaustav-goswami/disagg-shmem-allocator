/**
 * @file  test_slim_scale.c
 * @brief Quantitatively proves the metadata-scaling claim behind v0.2:
 *        that per-object overhead is now in the same ballpark as a typical
 *        malloc chunk header, not the 152 bytes/object v2 spent.
 *
 * Two checks:
 *   1. An empirical one: really allocate a few hundred thousand tiny
 *      anonymous objects in a real region and confirm used_bytes matches
 *      exactly N * (header + aligned payload) -- no hidden overhead crept
 *      in anywhere.
 *   2. A projection using the *actual* sizeof() values compiled into this
 *      binary (not hand-copied numbers) at the scale the redesign was
 *      motivated by: 300M objects representing 2.1 GiB of data, which
 *      needed ~70 GiB of metadata under v2.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_slim.h"
#include "shm_slim_internal.h"   /* sizeof(shm_slim_hdr_t) for the projection */

#include <inttypes.h>
#include <stdio.h>

static int g_failed = 0;

#define CHECK(cond, desc) \
    do { \
        if (cond) { printf("  PASS: %s\n", desc); } \
        else { fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, desc); \
               g_failed = 1; } \
    } while (0)

/* v2's per-object metadata cost, from shm_internal.h: a 56-byte block
 * header (shm_block_hdr_t) plus a 96-byte directory entry (shm_obj_entry_t)
 * for every single allocated object, named or not. */
#define V2_BLOCK_HDR_BYTES  56u
#define V2_DIR_ENTRY_BYTES  96u
#define V2_BYTES_PER_OBJECT (V2_BLOCK_HDR_BYTES + V2_DIR_ENTRY_BYTES)

static void test_empirical_overhead(void)
{
    puts("\n[test_empirical_overhead]");

    const size_t   n_objects     = 200000;
    const size_t   payload_bytes = 8;   /* tiny objects, the case that matters at scale */

    shm_slim_region_t *r = NULL;
    shm_slim_region_open_opts_t opts = {
        .backend = SHM_SLIM_BACKEND_POSIX, .flags = SHM_SLIM_OPEN_CREATE,
    };
    int rc = shm_slim_region_open("/shm_slim_test_scale", 1 << 23 /* 8 MiB */, &opts, &r);
    CHECK(rc == 0, "region opens for the empirical scale test");

    for (size_t i = 0; i < n_objects; i++) {
        shm_off_t off;
        rc = shm_slim_alloc(r, payload_bytes, NULL, SHM_SLIM_KEY_OPEN, &off);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: alloc %zu failed with errno %d\n", i, rc);
            g_failed = 1;
            break;
        }
    }

    shm_slim_pool_stats_t st;
    shm_slim_pool_stats(r, &st);
    CHECK(st.block_count == n_objects, "all requested objects were allocated");

    /* Every block is header(16) + payload(8) = 24 bytes, already 8-aligned:
     * exactly SHM_SLIM_BLOCK_MIN, with zero hidden padding or bookkeeping. */
    size_t expected_used = n_objects * (sizeof(shm_slim_hdr_t) + payload_bytes);
    CHECK(st.used_bytes == expected_used,
          "used_bytes == N * (header + payload) exactly, no hidden per-object cost");

    double bytes_per_object = (double)st.used_bytes / (double)n_objects;
    printf("  -> %zu objects, %zu-byte payload each: %.1f bytes/object total "
           "(%.0f bytes of that is header)\n",
           n_objects, payload_bytes, bytes_per_object, (double)sizeof(shm_slim_hdr_t));
    CHECK(bytes_per_object <= 32.0,
          "empirical total bytes/object is within the 'typical malloc' 8-32 byte ballpark");

    CHECK(st.name_used == 0, "name table stays empty: these are all anonymous objects");
    /* Physical capacity is inflated 2x beyond SHM_SLIM_NAME_CAP_DEFAULT to
     * keep the hash table's load factor low (see shm_slim_name_entry_t's
     * comment) -- the point of this check is unaffected: it's still a
     * fixed constant, not something that scales with n_objects. */
    CHECK(st.name_capacity == SHM_SLIM_NAME_CAP_DEFAULT * 2,
          "name table capacity is independent of how many objects were allocated");

    shm_slim_region_close(r, true);
}

static void test_projection_at_motivating_scale(void)
{
    puts("\n[test_projection_at_motivating_scale]");

    /* The scenario that motivated this redesign. */
    const uint64_t n_objects   = 300000000ULL;             /* 300M objects   */
    const uint64_t data_bytes  = 2ULL * 1024 * 1024 * 1024 + 100ULL * 1024 * 1024; /* ~2.1 GiB */

    uint64_t v2_metadata_bytes   = n_objects * (uint64_t)V2_BYTES_PER_OBJECT;
    uint64_t slim_metadata_bytes = n_objects * (uint64_t)sizeof(shm_slim_hdr_t);

    double v2_gib   = (double)v2_metadata_bytes   / (1024.0 * 1024.0 * 1024.0);
    double slim_gib = (double)slim_metadata_bytes / (1024.0 * 1024.0 * 1024.0);
    double data_gib = (double)data_bytes / (1024.0 * 1024.0 * 1024.0);
    double reduction = (double)v2_metadata_bytes / (double)slim_metadata_bytes;

    printf("  -> %" PRIu64 " objects, ~%.2f GiB data\n", n_objects, data_gib);
    printf("     v2   metadata (56+96 B/obj): ~%.2f GiB (%.1fx the data)\n",
           v2_gib, v2_gib / data_gib);
    printf("     slim metadata (%zu B/obj):    ~%.2f GiB (%.1fx the data)\n",
           sizeof(shm_slim_hdr_t), slim_gib, slim_gib / data_gib);
    printf("     reduction: %.1fx\n", reduction);

    /* Using the *actual* struct sizes compiled into this binary (not the
     * user's original back-of-envelope "~70 GiB" figure, which this
     * doesn't need to match exactly to make the point): v2's fixed 152
     * bytes/object is tens of GiB at this object count, dwarfing the 2.1
     * GiB of real data by an order of magnitude. */
    CHECK(v2_gib > 30.0, "v2 metadata at this scale is tens of GiB, the scale that motivated this redesign");
    CHECK(v2_gib / data_gib > 15.0, "v2 metadata is over an order of magnitude larger than the data it describes");

    /* slim's 16-byte header is still larger than a ~7-byte average payload
     * at this scale, so metadata does not shrink below the data itself --
     * but it lands within about 2x of the data instead of ~20x. That is
     * the honest, defensible claim: a huge reduction, not "solved". */
    CHECK(slim_gib / data_gib < 3.0, "slim metadata is within ~2-3x of the data size, not ~20x");
    CHECK(reduction > 8.0, "slim format cuts per-object metadata by at least 8x versus v2");
}

int main(void)
{
    puts("=== test_slim_scale ===");
    test_empirical_overhead();
    test_projection_at_motivating_scale();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
