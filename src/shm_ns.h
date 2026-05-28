/**
 * @file  shm_ns.h
 * @brief Internal helpers for Linux namespace and cgroup fingerprinting.
 *
 * These are used by shm_region_open() to record and optionally verify that a
 * process opening an existing region is in the same Linux IPC namespace and/or
 * memory cgroup as the process that originally created the region.
 *
 * Neither function is part of the public API.
 */

#ifndef SHM_NS_H
#define SHM_NS_H

#include <stdint.h>   /* uint64_t */

/**
 * @brief Return the inode number of the calling process's IPC namespace.
 *
 * Linux exposes the IPC namespace as a synthetic inode at /proc/self/ns/ipc.
 * All processes in the same namespace see the same inode number.  This is a
 * cheap, kernel-provided identity that does not require any privileged call.
 *
 * @return The inode number, or 0 if /proc/self/ns/ipc is unavailable
 *         (e.g. old kernel, seccomp policy, or non-Linux system).
 */
uint64_t shm_ns_ipc_inode(void);

/**
 * @brief Return an FNV-1a 64-bit hash of the calling process's cgroup path.
 *
 * The content of /proc/self/cgroup is hashed so that processes in different
 * cgroups produce different values.  This is useful as a lightweight guard
 * to prevent accidental shared-memory access across cgroup boundaries.
 *
 * @return A non-zero hash on success, or 0 if /proc/self/cgroup cannot be
 *         read (in which case enforcement is silently skipped).
 */
uint64_t shm_ns_cgroup_hash(void);

#endif /* SHM_NS_H */
