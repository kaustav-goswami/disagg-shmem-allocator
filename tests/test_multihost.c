/**
 * @file  test_multihost.c
 * @brief Multi-process tests: parent creates objects; child discovers via the
 *        object directory; both sides verify data consistency.
 *
 * The test uses fork() to simulate two independent "hosts" sharing a single
 * POSIX shared-memory region.  This mirrors the disaggregated-memory use case
 * where each host opens the same /dev/dax device (here replaced by shm_open).
 *
 * Synchronisation between parent and child is done through a small "sync"
 * object in the shared region itself:  the parent writes a ready flag and a
 * known object ID; the child spin-waits until the flag is set, then discovers
 * all objects by iterating the directory.
 *
 * Exit codes:
 *   0 = all tests passed
 *   1 = at least one test failed
 */

#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE         /* usleep on older glibc */
#define _DEFAULT_SOURCE     /* usleep on glibc >= 2.19 */

#include "shm_alloc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>   /* waitpid */
#include <unistd.h>     /* fork, sleep */
#include <sched.h>      /* sched_yield */

/* ── Minimal test framework ──────────────────────────────────────────────── */

static int g_failed = 0;

#define CHECK(cond, desc) \
    do { \
        if (cond) { printf("  PASS: %s\n", desc); } \
        else { fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, desc); \
               g_failed = 1; } \
    } while (0)

#define CHECK_OK(rc, desc)       CHECK((rc) == 0, desc)
#define CHECK_ERR(rc, exp, desc) CHECK((rc) == (exp), desc)

/* ── Shared sync object layout ────────────────────────────────────────────── */

/**
 * Lives in shared memory; used as a lightweight hand-shake between parent and
 * child without requiring an external synchronisation mechanism.
 *
 * The parent sets ready = 1 after all work objects are registered.
 * The child spin-waits on ready before starting its discovery pass.
 * The child sets child_done = 1 when it has finished.
 */
typedef struct {
    volatile int ready;       /* 1 once parent has finished setting up objects */
    volatile int child_done;  /* 1 once child has finished reading objects */
    uint64_t work_id_a;       /* ID of the first work object */
    uint64_t work_id_b;       /* ID of the second work object */
} sync_t;

/* ── Work object ──────────────────────────────────────────────────────────── */

typedef struct {
    int    seq;         /* sequential number written by parent */
    double value;       /* floating-point value written by parent */
    char   tag[16];     /* string tag */
} work_t;

/* ── Region name and user IDs ──────────────────────────────────────────────── */

#define REGION_NAME   "/shm_test_multihost"  /* POSIX shm name */
#define UID_PARENT    1u                      /* user id for the parent process */
#define UID_CHILD     2u                      /* user id for the child process */

/* ── Parent logic ─────────────────────────────────────────────────────────── */

/**
 * Parent process:
 *   1. Creates the shared region.
 *   2. Allocates the sync object and two work objects.
 *   3. Writes data into the work objects.
 *   4. Sets sync->ready so the child can start discovering.
 *   5. Waits for child_done.
 *   6. Returns 0 on success, 1 on failure.
 */
static int parent_main(void)
{
    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend      = SHM_BACKEND_POSIX,
        .flags        = SHM_OPEN_CREATE,  /* parent always creates fresh */
        .dir_capacity = 32,
    };
    int rc = shm_region_open(REGION_NAME, 1 << 20, &opts, &r);
    if (rc != 0) { fprintf(stderr, "parent: shm_region_open failed: %d\n", rc); return 1; }

    /* Allocate sync object; grant ADMIN so the child (different uid) can read. */
    uint64_t sync_id; shm_off_t sync_off;
    rc = shm_alloc(r, sizeof(sync_t), "sync",
                   UID_PARENT, SHM_PERM_DEFAULT | SHM_PERM_ADMIN,
                   0, &sync_id, &sync_off);
    if (rc != 0) { fprintf(stderr, "parent: alloc sync failed: %d\n", rc); return 1; }

    sync_t *sync = shm_cast_mut(r, sync_off, sync_t, UID_PARENT, SHM_PERM_DEFAULT | SHM_PERM_ADMIN);
    sync->ready      = 0;
    sync->child_done = 0;

    /* Allocate work object A (parent-owned; child reads via ADMIN on the block). */
    uint64_t id_a; shm_off_t off_a;
    shm_alloc(r, sizeof(work_t), "work_a",
              UID_PARENT, SHM_PERM_DEFAULT,
              0xAAAA, &id_a, &off_a);

    work_t *wa = shm_cast_mut(r, off_a, work_t, UID_PARENT, SHM_PERM_DEFAULT);
    wa->seq   = 1;
    wa->value = 2.718;
    strncpy(wa->tag, "euler", sizeof(wa->tag) - 1);

    /* Allocate work object B. */
    uint64_t id_b; shm_off_t off_b;
    shm_alloc(r, sizeof(work_t), "work_b",
              UID_PARENT, SHM_PERM_DEFAULT,
              0xBBBB, &id_b, &off_b);

    work_t *wb = shm_cast_mut(r, off_b, work_t, UID_PARENT, SHM_PERM_DEFAULT);
    wb->seq   = 2;
    wb->value = 3.14159;
    strncpy(wb->tag, "pi", sizeof(wb->tag) - 1);

    /* Store IDs so the child knows which objects to verify. */
    sync->work_id_a = id_a;
    sync->work_id_b = id_b;

    /* Signal child that setup is complete. */
    __sync_synchronize();   /* memory barrier: all writes above are visible */
    sync->ready = 1;

    /* Spin-wait for child to finish (up to ~5 s). */
    for (int i = 0; i < 50000 && !sync->child_done; i++) {
        usleep(100);   /* yield 100 µs per iteration */
    }

    printf("  parent: child_done=%d\n", sync->child_done);
    shm_region_close(r, false);   /* do not unlink; child may still be alive */
    return 0;
}

/* ── Child logic ──────────────────────────────────────────────────────────── */

/**
 * Child process:
 *   1. Opens the existing shared region (no SHM_OPEN_CREATE).
 *   2. Finds the sync object by name.
 *   3. Spin-waits on sync->ready.
 *   4. Iterates the directory and verifies work objects A and B by ID.
 *   5. Sets sync->child_done.
 *   6. Returns 0 on success, 1 on failure.
 */
static int child_main(void)
{
    /* Give parent a moment to create the region before opening it. */
    usleep(5000);

    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = {
        .backend = SHM_BACKEND_POSIX,
        .flags   = 0,   /* open existing; no create */
    };
    int rc = shm_region_open(REGION_NAME, 0, &opts, &r);
    if (rc != 0) { fprintf(stderr, "child: shm_region_open failed: %d\n", rc); return 1; }

    /* Locate the sync object by name.  Grant ourselves ADMIN to read
     * parent-owned objects.  In a real system this would be controlled by
     * cgroup/namespace policy. */
    shm_off_t sync_off;
    uint64_t  sync_id;
    for (int retry = 0; retry < 200; retry++) {
        rc = shm_lookup_by_name(r, "sync", UID_CHILD,
                                SHM_PERM_READ | SHM_PERM_ADMIN,
                                &sync_id, &sync_off);
        if (rc == 0) break;
        usleep(1000);   /* wait 1 ms and retry */
    }
    if (rc != 0) {
        fprintf(stderr, "child: could not find sync object: %d\n", rc);
        shm_region_close(r, false);
        return 1;
    }

    /* Resolve pointer (ADMIN permission bypasses owner check). */
    sync_t *sync = (sync_t *)shm_ptr(r, sync_off, UID_CHILD,
                                     SHM_PERM_READ | SHM_PERM_WRITE | SHM_PERM_ADMIN,
                                     SHM_PERM_READ);

    /* Spin until parent marks setup as ready. */
    for (int i = 0; i < 50000 && !sync->ready; i++)
        usleep(100);

    if (!sync->ready) {
        fprintf(stderr, "child: timeout waiting for parent ready flag\n");
        shm_region_close(r, false);
        return 1;
    }

    /* Verify work objects by iterating the directory. */
    int found_a = 0, found_b = 0;
    uint32_t cursor = 0;
    shm_dir_entry_t entry;
    while (shm_dir_next(r, &cursor, &entry) == 0) {
        if (entry.id == sync->work_id_a) {
            /* Look up and read work object A. */
            shm_off_t off;
            shm_lookup(r, entry.id, UID_CHILD,
                       SHM_PERM_READ | SHM_PERM_ADMIN, &off, NULL);
            const work_t *wa = (const work_t *)shm_ptr(
                r, off, UID_CHILD,
                SHM_PERM_READ | SHM_PERM_ADMIN, SHM_PERM_READ);
            if (wa && wa->seq == 1 && wa->value == 2.718)
                found_a = 1;
        }
        if (entry.id == sync->work_id_b) {
            shm_off_t off;
            shm_lookup(r, entry.id, UID_CHILD,
                       SHM_PERM_READ | SHM_PERM_ADMIN, &off, NULL);
            const work_t *wb = (const work_t *)shm_ptr(
                r, off, UID_CHILD,
                SHM_PERM_READ | SHM_PERM_ADMIN, SHM_PERM_READ);
            if (wb && wb->seq == 2 && wb->value == 3.14159)
                found_b = 1;
        }
    }

    /* Write results to stdout from the child so the parent test suite sees them. */
    printf("  child: found_a=%d found_b=%d\n", found_a, found_b);

    /* Signal parent that we are done. */
    __sync_synchronize();
    sync->child_done = 1;

    shm_region_close(r, false);
    return (found_a && found_b) ? 0 : 1;
}

/* ── Test orchestration ───────────────────────────────────────────────────── */

static void test_multihost_fork(void)
{
    puts("\n[test_multihost_fork]");

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %d\n", errno);
        g_failed = 1;
        return;
    }

    if (pid == 0) {
        /* ── Child process ── */
        int child_rc = child_main();
        _exit(child_rc);   /* _exit: do not flush parent's stdio buffers */
    }

    /* ── Parent process ── */
    int parent_rc = parent_main();

    /* Wait for child and collect its exit code. */
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    int child_rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;

    CHECK(parent_rc  == 0, "parent finished successfully");
    CHECK(child_rc   == 0, "child discovered and verified both objects");

    /* Clean up the shared region now that both sides are done. */
    shm_region_t *r = NULL;
    shm_region_open_opts_t opts = { .backend = SHM_BACKEND_POSIX, .flags = 0 };
    if (shm_region_open(REGION_NAME, 0, &opts, &r) == 0)
        shm_region_close(r, true);   /* unlink on the final close */
}

static void test_ns_enforcement(void)
{
    puts("\n[test_ns_enforcement]");

    /* Create a region that records the creator's IPC namespace. */
    shm_region_t *creator = NULL;
    shm_region_open_opts_t create_opts = {
        .backend = SHM_BACKEND_POSIX,
        .flags   = SHM_OPEN_CREATE,
    };
    int rc = shm_region_open("/shm_test_ns_enforce", 1 << 20, &create_opts, &creator);
    CHECK_OK(rc, "create region for NS enforcement test");

    /* Open again with enforcement enabled.  Same process → same namespace → OK. */
    shm_region_t *same_ns = NULL;
    shm_region_open_opts_t enforce_opts = {
        .backend = SHM_BACKEND_POSIX,
        .flags   = SHM_OPEN_ENFORCE_NS,  /* enforce namespace match */
    };
    rc = shm_region_open("/shm_test_ns_enforce", 0, &enforce_opts, &same_ns);
    CHECK_OK(rc, "same-namespace open with enforcement succeeds");

    if (same_ns)  shm_region_close(same_ns,  false);
    if (creator)  shm_region_close(creator,  true);   /* unlink */
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    puts("=== test_multihost ===");
    test_multihost_fork();
    test_ns_enforcement();

    if (g_failed) {
        fprintf(stderr, "\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll tests PASSED.\n");
    return 0;
}
