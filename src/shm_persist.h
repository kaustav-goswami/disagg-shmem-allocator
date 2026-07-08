/**
 * @file  shm_persist.h
 * @brief Cache-line flush / write-back helpers for shared and persistent memory.
 *
 * PURPOSE
 * ───────
 * On conventional DRAM-backed shared memory, CPU caches are kept coherent by
 * hardware and no explicit flushes are needed.  On disaggregated persistent
 * memory (Intel Optane, CXL PMEM, /dev/dax) writes may sit in the CPU cache
 * indefinitely; they must be explicitly flushed or written back to reach the
 * persistent medium and become visible to other hosts after a crash.
 *
 * This header provides two primitives:
 *
 *   shm_persist(addr, len)  – flush / write-back all cache lines in [addr, addr+len)
 *   shm_drain()             – issue a store-fence to order all prior persists
 *
 * CACHE MODE SELECTION  (pick exactly one at compile time)
 * ─────────────────────────────────────────────────────────
 * Pass ONE of the following defines to the compiler.  The recommended way is
 * via the Makefile  CACHE=  variable (e.g.  make CACHE=CLWB):
 *
 *   -DSHM_CACHE_CLFLUSH      x86 / x86-64
 *                            CLFLUSH: flush + invalidate cache line.
 *                            The original instruction; serialising.
 *                            Best for correctness; higher overhead.
 *
 *   -DSHM_CACHE_CLFLUSHOPT   x86 / x86-64  (Broadwell+, CPUID CLFLUSHOPT)
 *                            CLFLUSHOPT: flush + invalidate, non-serialising.
 *                            Requires SFENCE after the last flush in a group.
 *                            Better throughput than CLFLUSH when flushing many
 *                            lines at once.
 *
 *   -DSHM_CACHE_CLWB         x86 / x86-64  (Cannon Lake+, CPUID CLWB)
 *                            CLWB: write-back dirty data; cache line stays
 *                            valid in Modified → Shared state.  Lowest
 *                            latency choice for PMEM workloads; needs SFENCE.
 *
 *   -DSHM_CACHE_CBO_FLUSH    RISC-V  (Zicbom extension)
 *                            cbo.flush: flush + invalidate cache block.
 *                            Semantically equivalent to CLFLUSH.
 *                            Requires FENCE after a group of cbo.flush ops.
 *
 *   -DSHM_CACHE_CBO_CLEAN    RISC-V  (Zicbom extension)
 *                            cbo.clean: write-back without invalidate.
 *                            Semantically equivalent to CLWB.
 *                            Requires FENCE after a group.
 *
 * DEFAULT (none of the above)
 * ────────────────────────────
 * Both shm_persist() and shm_drain() compile to nothing.  The compiler
 * barrier in shm_persist() prevents reordering of stores across the call
 * boundary even in this mode.  Appropriate for ordinary DRAM-backed SHM.
 *
 * ARCHITECTURE GUARD
 * ───────────────────
 * Selecting an x86 mode on a non-x86 host (or a RISC-V mode on a non-RISC-V
 * host) is a compile-time error; this prevents silent bugs where the wrong
 * instruction set is silently ignored.
 *
 * MULTIPLE MODES
 * ───────────────
 * Enabling more than one mode is a compile-time error.
 *
 * CACHE LINE SIZE
 * ────────────────
 * Defaults to 64 bytes.  Override with  -DSHM_CACHE_LINE_BYTES=<n>  for
 * platforms with a different L1 cache line size (e.g. IBM POWER: 128 bytes).
 */

#ifndef SHM_PERSIST_H
#define SHM_PERSIST_H

#include <stddef.h>   /* size_t */

/* ── Cache line size ─────────────────────────────────────────────────────── */

/**
 * Number of bytes in one cache line.  Overridable with -DSHM_CACHE_LINE_BYTES.
 * Defined as size_t so that bitwise operations like ~(SHM_CACHE_LINE_BYTES-1)
 * produce a full 64-bit mask on LP64 targets.  Using an unsigned int here
 * would truncate the mask to 32 bits after integer promotion and corrupt the
 * align-down address calculation in shm_persist().
 */
#ifndef SHM_CACHE_LINE_BYTES
#  define SHM_CACHE_LINE_BYTES  ((size_t)64)
#endif

/* ── Mutual-exclusion guard ──────────────────────────────────────────────── */

#if defined(SHM_CACHE_CLFLUSH) && defined(SHM_CACHE_CLFLUSHOPT)
#  error "SHM_CACHE_CLFLUSH and SHM_CACHE_CLFLUSHOPT are mutually exclusive"
#endif
#if defined(SHM_CACHE_CLFLUSH) && defined(SHM_CACHE_CLWB)
#  error "SHM_CACHE_CLFLUSH and SHM_CACHE_CLWB are mutually exclusive"
#endif
#if defined(SHM_CACHE_CLFLUSHOPT) && defined(SHM_CACHE_CLWB)
#  error "SHM_CACHE_CLFLUSHOPT and SHM_CACHE_CLWB are mutually exclusive"
#endif
#if defined(SHM_CACHE_CBO_FLUSH) && defined(SHM_CACHE_CBO_CLEAN)
#  error "SHM_CACHE_CBO_FLUSH and SHM_CACHE_CBO_CLEAN are mutually exclusive"
#endif
#if (defined(SHM_CACHE_CLFLUSH)    || \
     defined(SHM_CACHE_CLFLUSHOPT) || \
     defined(SHM_CACHE_CLWB))      && \
    (defined(SHM_CACHE_CBO_FLUSH)  || defined(SHM_CACHE_CBO_CLEAN))
#  error "Cannot mix x86 and RISC-V cache modes"
#endif

/* ── Architecture guards ─────────────────────────────────────────────────── */

#if (defined(SHM_CACHE_CLFLUSH) || defined(SHM_CACHE_CLFLUSHOPT) || defined(SHM_CACHE_CLWB))
#  if !defined(__x86_64__) && !defined(__i386__)
#    error "CLFLUSH / CLFLUSHOPT / CLWB require an x86 or x86-64 target"
#  endif
#endif

#if (defined(SHM_CACHE_CBO_FLUSH) || defined(SHM_CACHE_CBO_CLEAN))
#  if !defined(__riscv)
#    error "cbo.flush / cbo.clean require a RISC-V target (Zicbom extension)"
#  endif
#endif

/* ══════════════════════════════════════════════════════════════════════════
 *  Per-mode implementation
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── x86 CLFLUSH ─────────────────────────────────────────────────────────── */
#if defined(SHM_CACHE_CLFLUSH)

/**
 * Flush and invalidate the single cache line containing @p p.
 * CLFLUSH is serialising; no extra fence is needed for ordering, but we still
 * emit SFENCE in shm_drain() for uniformity.
 */
static inline void shm_flush_line(const void *p)
{
    /* volatile ensures the asm is not deleted; "memory" prevents store hoisting. */
    __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
}

/**
 * Issue an SFENCE to ensure all preceding CLFLUSH operations and stores are
 * globally visible before any subsequent stores.
 */
static inline void shm_drain(void)
{
    __asm__ volatile("sfence" : : : "memory");
}

/* ── x86 CLFLUSHOPT ──────────────────────────────────────────────────────── */
#elif defined(SHM_CACHE_CLFLUSHOPT)

/**
 * Flush and invalidate the cache line containing @p p using CLFLUSHOPT.
 * CLFLUSHOPT is non-serialising (weakly ordered); call shm_drain() after the
 * last flush in a sequence to enforce ordering.
 * Requires Broadwell or newer (CPUID.07H.EBX.CLFLUSHOPT).
 */
static inline void shm_flush_line(const void *p)
{
    __asm__ volatile("clflushopt (%0)" : : "r"(p) : "memory");
}

/** SFENCE — orders all preceding CLFLUSHOPT operations. */
static inline void shm_drain(void)
{
    __asm__ volatile("sfence" : : : "memory");
}

/* ── x86 CLWB ────────────────────────────────────────────────────────────── */
#elif defined(SHM_CACHE_CLWB)

/**
 * Write-back the cache line containing @p p to the persistent medium without
 * invalidating it.  The line transitions from Modified → Shared; subsequent
 * reads do not incur a reload penalty.
 * Requires Cannon Lake or newer (CPUID.07H.EBX.CLWB).
 * Must be followed by shm_drain() (SFENCE) before ordering is guaranteed.
 */
static inline void shm_flush_line(const void *p)
{
    __asm__ volatile("clwb (%0)" : : "r"(p) : "memory");
}

/** SFENCE — ensures all preceding CLWB operations and stores are persistent. */
static inline void shm_drain(void)
{
    __asm__ volatile("sfence" : : : "memory");
}

/* ── RISC-V cbo.flush ────────────────────────────────────────────────────── */
#elif defined(SHM_CACHE_CBO_FLUSH)

/**
 * Flush and invalidate the cache block containing @p p.
 * Uses the RISC-V Zicbom CBO instruction  cbo.flush.
 * Semantically equivalent to CLFLUSH.
 * Requires the Zicbom extension; check the hart's ISA string before use.
 * Must be followed by shm_drain() (FENCE) for ordering.
 */
static inline void shm_flush_line(const void *p)
{
    /* cbo.flush is encoded as: opcode=MISC-MEM, funct3=CBO, rs1=<addr>, rs2/rd=0 */
    __asm__ volatile("cbo.flush (%0)" : : "r"(p) : "memory");
}

/**
 * FENCE — ensures all preceding cbo.flush operations and stores are ordered.
 * The "rw,rw" variant orders both loads and stores in both directions.
 */
static inline void shm_drain(void)
{
    __asm__ volatile("fence rw, rw" : : : "memory");
}

/* ── RISC-V cbo.clean ────────────────────────────────────────────────────── */
#elif defined(SHM_CACHE_CBO_CLEAN)

/**
 * Write-back the cache block containing @p p without invalidating it.
 * Uses the RISC-V Zicbom CBO instruction  cbo.clean.
 * Semantically equivalent to CLWB.
 * Must be followed by shm_drain() (FENCE) for ordering.
 */
static inline void shm_flush_line(const void *p)
{
    __asm__ volatile("cbo.clean (%0)" : : "r"(p) : "memory");
}

/** FENCE — ensures all preceding cbo.clean operations and stores are ordered. */
static inline void shm_drain(void)
{
    __asm__ volatile("fence rw, rw" : : : "memory");
}

/* ── Default: no-ops ─────────────────────────────────────────────────────── */
#else

/**
 * No cache mode selected: shm_flush_line() is a no-op.
 * A compiler barrier prevents the compiler from reordering stores across the
 * call, which is still useful for correctness on weakly-ordered hardware.
 */
static inline void shm_flush_line(const void *p)
{
    (void)p;
    /* Compiler barrier only: prevents store reordering without issuing HW ops. */
    __asm__ volatile("" : : : "memory");
}

/** No cache mode selected: shm_drain() is a compiler barrier only. */
static inline void shm_drain(void)
{
    __asm__ volatile("" : : : "memory");
}

#endif /* mode selection */

/* ══════════════════════════════════════════════════════════════════════════
 *  High-level range persist (calls shm_flush_line on each cache line)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Flush / write-back all cache lines that overlap [addr, addr + len).
 *
 * Walks the address range in SHM_CACHE_LINE_BYTES steps, issuing one
 * shm_flush_line() per cache line.  Does NOT issue a drain fence; call
 * shm_drain() after all the persist calls for a logical transaction.
 *
 * @param addr  Start of the byte range to persist.
 * @param len   Number of bytes to cover (rounded up to a whole cache line).
 */
static inline void shm_persist(const void *addr, size_t len)
{
    /* Align down to the cache-line boundary to ensure we cover the first line. */
    const char *p   = (const char *)((size_t)addr & ~(SHM_CACHE_LINE_BYTES - 1u));
    const char *end = (const char *)addr + len;

    /* Issue one flush per cache line until we have covered the entire range. */
    while (p < end) {
        shm_flush_line(p);
        p += SHM_CACHE_LINE_BYTES;
    }
}

#endif /* SHM_PERSIST_H */
