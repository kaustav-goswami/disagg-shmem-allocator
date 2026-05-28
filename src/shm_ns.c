/**
 * @file  shm_ns.c
 * @brief Linux namespace and cgroup fingerprint helpers.
 */

#define _POSIX_C_SOURCE 200809L  /* for open(), read(), stat() POSIX 2008 */

#include "shm_ns.h"

#include <fcntl.h>      /* open(), O_RDONLY */
#include <stdint.h>     /* uint8_t, uint64_t */
#include <sys/stat.h>   /* stat(), struct stat */
#include <unistd.h>     /* read(), close() */

/* ── IPC namespace fingerprint ─────────────────────────────────────────── */

uint64_t shm_ns_ipc_inode(void)
{
    struct stat st;  /* will receive filesystem metadata for the ns symlink */

    /* /proc/self/ns/ipc is a magic symlink whose inode encodes the namespace.
     * All threads/processes in the same IPC namespace share the same inode. */
    if (stat("/proc/self/ns/ipc", &st) != 0)
        return 0;   /* stat failed; kernel too old or seccomp blocks it */

    return (uint64_t)st.st_ino;  /* inode number is the unique namespace token */
}

/* ── cgroup hash ─────────────────────────────────────────────────────────── */

/**
 * FNV-1a 64-bit hash algorithm constants.
 * Reference: http://www.isthe.com/chongo/tech/comp/fnv/
 */
#define FNV_OFFSET_BASIS  14695981039346656037ULL  /* recommended initial value */
#define FNV_PRIME         1099511628211ULL          /* large prime for diffusion */

uint64_t shm_ns_cgroup_hash(void)
{
    /* Open the cgroup membership file exposed by the kernel for this process. */
    int fd = open("/proc/self/cgroup", O_RDONLY);
    if (fd < 0)
        return 0;  /* cgroups not available or read blocked by policy */

    uint64_t hash = FNV_OFFSET_BASIS;  /* start with the FNV offset basis */
    char     buf[256];                 /* small read buffer; file is typically tiny */
    ssize_t  n;

    /* Read the entire file in chunks and mix each byte into the hash. */
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            hash ^= (uint8_t)buf[i];   /* XOR the byte into the low 8 bits */
            hash *= FNV_PRIME;         /* multiply to diffuse the bit change */
        }
    }

    close(fd);  /* release the file descriptor regardless of read result */

    /* A hash of 0 is a valid FNV output but we reserved 0 as "unavailable".
     * Flip the last bit to ensure we never accidentally suppress enforcement. */
    return hash ? hash : 1u;
}
