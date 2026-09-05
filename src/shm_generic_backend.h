/**
 * @file  shm_generic_backend.h
 * @brief Optional, compile-time-selected "generic allocator" backend.
 *
 * By default shm_alloc.c (v2) and shm_slim.c (v0.2) source a region's
 * backing memory from POSIX shm_open()+mmap() or a DAX device. This header
 * adds a third option: source it from a vendored allocator library instead
 * -- select at most one of the following at compile time:
 *
 *   -DSHM_GENERIC_JEMALLOC   jemalloc/          malloc() / free() (overridden)
 *   -DSHM_GENERIC_TCMALLOC   tcmalloc/          TCMallocInternalMalloc/Free()
 *   -DSHM_GENERIC_CXLALLOC   cxlalloc/          cxlalloc_malloc() / _free()
 *   -DSHM_GENERIC_RALLOC     ralloc/            (see limitation below)
 *
 * `make GENERIC_ALLOC=<name>` sets the right macro, include path, and link
 * flags -- see the Makefile and the "Generic allocator backend" section of
 * README.md. None of the vendored sources under jemalloc/, tcmalloc/,
 * cxlalloc/, ralloc/ are modified by this integration; this header only
 * calls each library's own public API.
 *
 * ── Why these four ──────────────────────────────────────────────────────────
 *
 *   jemalloc and tcmalloc are general-purpose allocators; using them here
 *   is mainly useful for comparing dmalloc-slim's own bump/free-list heap
 *   logic against delegating raw buffer acquisition to a mature allocator.
 *   cxlalloc is purpose-built for CXL/disaggregated memory -- the same
 *   problem space this project targets -- and is the most natural fit.
 *   ralloc is Redox's userspace allocator; see the build-status note below.
 *
 * ── Build status in this repository (checked when this was written) ────────
 *
 *   jemalloc  -- builds cleanly with the vendored autoconf build
 *                (`./autogen.sh && make`); fully wired and tested.
 *   cxlalloc  -- builds cleanly with `cargo build --release -p
 *                cxlalloc-static` *using the 1.88.0 Rust toolchain already
 *                pinned by cxlalloc/rust-toolchain.toml* (the default
 *                system `cargo`/`rustc` in this environment is 1.75.0,
 *                which is too old for one of cxlalloc's workspace members).
 *                Also required initializing one nested git submodule
 *                (`cxlalloc/extern/memento`) that a workspace member's
 *                Cargo.toml path-depends on -- a missing vendored
 *                dependency, not a source change. Fully wired and tested.
 *   tcmalloc  -- this vendored copy only ships a Bazel build
 *                (MODULE.bazel) plus a CMakeLists.txt that FetchContents
 *                abseil/protobuf/googletest/RE2 from GitHub at configure
 *                time -- a large, slow build, and Bazel is not installed
 *                in this environment. The real public symbol names
 *                (TCMallocInternalMalloc/Free, confirmed from
 *                tcmalloc/tcmalloc/tcmalloc.h) are wired below, but you
 *                must build libtcmalloc yourself first (see WIP.md).
 *   ralloc    -- this vendored copy is `#![no_std]` and depends on
 *                nightly-only Rust features that were renamed or removed
 *                years ago (`optin_builtin_traits`, feature-gated
 *                `const_fn`, ...). Neither the 1.75.0 nor 1.88.0 toolchain
 *                available here can compile it, and no toolchain old
 *                enough to try is installed. The hook exists below for
 *                completeness, but there is currently no way to actually
 *                exercise it without sourcing a period-correct nightly
 *                compiler -- see WIP.md.
 *
 * ── Important limitation: single-process only ───────────────────────────────
 *
 * A generic-backend region is backed by memory obtained from a library call
 * (malloc-style), not a named, independently-attachable OS object the way a
 * POSIX shm segment or DAX device is. Every one of these four libraries'
 * allocations live in the calling process's own address space with no
 * built-in mechanism (used here) for a second process to attach to the same
 * bytes. shm_region_open()/shm_slim_region_open() with the create flag
 * unset therefore returns ENOTSUP for a generic backend -- this is a real
 * capability gap versus POSIX/DAX, not an oversight.
 */

#ifndef SHM_GENERIC_BACKEND_H
#define SHM_GENERIC_BACKEND_H

#include <stddef.h>

#if (defined(SHM_GENERIC_JEMALLOC) + defined(SHM_GENERIC_TCMALLOC) + \
     defined(SHM_GENERIC_CXLALLOC) + defined(SHM_GENERIC_RALLOC)) > 1
#error "Select at most one of SHM_GENERIC_{JEMALLOC,TCMALLOC,CXLALLOC,RALLOC}."
#endif

#if defined(SHM_GENERIC_JEMALLOC)

#define SHM_GENERIC_BACKEND_NAME "jemalloc"

/* The vendored jemalloc/ is built here with the default (unprefixed)
 * configure options, which makes it a drop-in replacement for libc's
 * malloc/free rather than exposing separate je_malloc()/je_free() names
 * (confirmed with `nm jemalloc/lib/libjemalloc.a`: it exports plain
 * `malloc`/`free`, not `je_malloc`/`je_free`). Linking libjemalloc.a ahead
 * of libc resolves these calls to jemalloc's implementation. */
#include <stdlib.h>

static inline void *shm_generic_backend_alloc(size_t size)
{
    return malloc(size);
}

static inline void shm_generic_backend_free(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}

#elif defined(SHM_GENERIC_TCMALLOC)

#define SHM_GENERIC_BACKEND_NAME "tcmalloc"

/* Real exported names, confirmed from tcmalloc/tcmalloc/tcmalloc.h -- this
 * build overrides libc's malloc/free via a linker section trick rather
 * than exposing a "tc_malloc"-style name, so we call the internal symbols
 * directly instead of relying on the override taking effect. */
extern void *TCMallocInternalMalloc(size_t size);
extern void  TCMallocInternalFree(void *ptr);

static inline void *shm_generic_backend_alloc(size_t size)
{
    return TCMallocInternalMalloc(size);
}

static inline void shm_generic_backend_free(void *ptr, size_t size)
{
    (void)size;
    TCMallocInternalFree(ptr);
}

#elif defined(SHM_GENERIC_CXLALLOC)

#define SHM_GENERIC_BACKEND_NAME "cxlalloc"

#include "cxlalloc.h"
#include <pthread.h>

/* cxlalloc manages one fixed-size heap per process, sized once at
 * cxlalloc_init_process() time. We size it off the *first* request this
 * process makes, padded generously, since later requests cannot resize it.
 * Per-translation-unit static state: if both shm_alloc.c and shm_slim.c
 * are compiled with the same SHM_GENERIC_CXLALLOC in one program, each
 * gets its own independent init-once guard and will call
 * cxlalloc_init_process() separately -- use only one module's generic
 * backend per process until this is unified. */
static pthread_once_t   shm_cxlalloc_once            = PTHREAD_ONCE_INIT;
static size_t           shm_cxlalloc_heap_size_hint  = 0;

static void shm_cxlalloc_do_init(void)
{
    /* heap_backend="mmap": a private, process-local heap. This matches the
     * single-process-only scope of the generic backend in this version;
     * cxlalloc also supports "shm"/"ivshmem" heap_backend values for real
     * cross-process sharing via heap_id, which a future version could use
     * to lift the single-process limitation. */
    cxlalloc_init_process("shm_generic_backend", -1, "mmap",
                          shm_cxlalloc_heap_size_hint, 1, 0);
}

static inline void *shm_generic_backend_alloc(size_t size)
{
    size_t padded = size * 4 + (64u << 20);   /* room for more allocations later */
    if (padded > shm_cxlalloc_heap_size_hint)
        shm_cxlalloc_heap_size_hint = padded;
    pthread_once(&shm_cxlalloc_once, shm_cxlalloc_do_init);
    return cxlalloc_malloc(size);
}

static inline void shm_generic_backend_free(void *ptr, size_t size)
{
    (void)size;
    cxlalloc_free(ptr);
}

#elif defined(SHM_GENERIC_RALLOC)

#define SHM_GENERIC_BACKEND_NAME "ralloc"

/* ralloc itself exposes no extern "C" API and cannot be built with any
 * toolchain available in this environment (see the build-status note
 * above) -- these are the symbols a small `extern "C"` wrapper crate
 * around ralloc::allocator::{alloc,free} would need to export. Declared
 * here for documentation; linking this backend will fail until such a
 * wrapper exists and both it and ralloc itself can actually be compiled. */
extern void *shm_ralloc_shim_alloc(size_t size);
extern void  shm_ralloc_shim_free(void *ptr, size_t size);

static inline void *shm_generic_backend_alloc(size_t size)
{
    return shm_ralloc_shim_alloc(size);
}

static inline void shm_generic_backend_free(void *ptr, size_t size)
{
    shm_ralloc_shim_free(ptr, size);
}

#else

#define SHM_GENERIC_BACKEND_NAME "(none)"

static inline void *shm_generic_backend_alloc(size_t size)
{
    (void)size;
    return NULL;
}

static inline void shm_generic_backend_free(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
}

#endif

#endif /* SHM_GENERIC_BACKEND_H */
