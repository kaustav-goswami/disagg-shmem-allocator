# ─────────────────────────────────────────────────────────────────────────────
# Makefile for shm_alloc — shared / disaggregated memory allocator
# Includes the optional CHERI-like capability layer (shm_cap).
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CXX     = g++

# ── Cache-persistence mode (optional, pick at most one) ───────────────────────
#
#   make CACHE=CLFLUSH      x86 / x86-64  — clflush (flush + invalidate)
#   make CACHE=CLFLUSHOPT   x86 / x86-64  — clflushopt (Broadwell+)
#   make CACHE=CLWB         x86 / x86-64  — clwb write-back (Cannon Lake+)
#   make CACHE=CBO_FLUSH    RISC-V Zicbom — cbo.flush (flush + invalidate)
#   make CACHE=CBO_CLEAN    RISC-V Zicbom — cbo.clean (write-back ≈ clwb)
#   make                    (no CACHE)    — no-op persist, compiler barrier only
#
# The selected mode is passed as -DSHM_CACHE_<MODE> and enforced by a compile-
# time architecture guard in src/shm_persist.h.  CPU feature availability is
# NOT checked at compile time; CLWB and CLFLUSHOPT require compatible hardware.
CACHE ?=
ifneq ($(CACHE),)
  CACHE_FLAG = -DSHM_CACHE_$(CACHE)
else
  CACHE_FLAG =
endif

# ── Back-invalidate mode (optional, for testing against a CXL simulator) ──────
#
#   make BACK_INVALIDATE=software   dmalloc calls shm_slim_soft_bi() (weak,
#                                    no-op by default -- a simulator
#                                    integration overrides it) on every
#                                    block-header write, requesting an
#                                    explicit invalidate for that address
#                                    range.
#   make                 (default)  hardware mode: no extra code path.
#                                    Every header write is an ordinary
#                                    shm_persist() and nothing else --
#                                    correct as long as the backing CXL
#                                    device snoops writes and issues
#                                    BISnp/back-invalidate automatically.
#
# See src/shm_slim_internal.h (SHM_SLIM_BI_NOTIFY) for the exact call sites.
BACK_INVALIDATE ?=
ifeq ($(BACK_INVALIDATE),software)
  BI_FLAG = -DSHM_SLIM_SOFTWARE_BI
else
  BI_FLAG =
endif

# Handle-level access lease (d_lease_expired()/d_reinit()) needs no build
# flag at all -- like DMALLOC_FIXED_BASE below, it's two timestamps on the
# process-local handle_t, opt in purely via the DMALLOC_LEASE_DURATION_US
# environment variable (see src/dmalloc.c and WIP.md). d_attach()/d_detach()
# are completely unaware it exists; an earlier per-object design that DID
# need a build flag (a background sweep over every tracked attached
# pointer) was replaced by this because it cost too much for what it
# bought -- see WIP.md.

# ── Generic allocator backend (optional, pick at most one) ────────────────────
#
#   make GENERIC_ALLOC=jemalloc   SHM_BACKEND_GENERIC sources region memory
#   make GENERIC_ALLOC=cxlalloc   from the selected vendored allocator instead
#   make GENERIC_ALLOC=tcmalloc   of shm_open()/mmap(); see WIP.md and the
#   make GENERIC_ALLOC=ralloc     "Generic allocator backend" README section.
#   make                (default) no generic backend; POSIX/DAX only.
#
# jemalloc and cxlalloc are built from the vendored submodules by the rules
# below (their own build systems, unmodified) and fully work end to end.
# tcmalloc (needs Bazel, not installed here) and ralloc (needs a Rust
# nightly toolchain old enough to predate features removed years ago) are
# wired with the correct symbol names but are not buildable in this
# environment -- selecting them fails the build with an explanatory message
# rather than a mysterious linker error. See WIP.md for details.
GENERIC_ALLOC ?=

JEMALLOC_DIR   = jemalloc
CXLALLOC_DIR   = cxlalloc
JEMALLOC_LIB   = $(JEMALLOC_DIR)/lib/libjemalloc.a
CXLALLOC_LIB   = $(CXLALLOC_DIR)/target/release/libcxlalloc_static.a

# cxlalloc pins its own Rust toolchain via rust-toolchain.toml; the system
# `cargo`/`rustc` in PATH may be a different (older) version that cannot
# build it, so resolve the pinned toolchain's binaries through rustup
# explicitly rather than relying on PATH.
CXLALLOC_TOOLCHAIN := $(shell awk -F'"' '/channel/{print $$2; exit}' $(CXLALLOC_DIR)/rust-toolchain.toml 2>/dev/null)
CXLALLOC_CARGO     := $(shell rustup which cargo --toolchain $(CXLALLOC_TOOLCHAIN) 2>/dev/null)
CXLALLOC_RUSTC     := $(shell rustup which rustc --toolchain $(CXLALLOC_TOOLCHAIN) 2>/dev/null)

ifeq ($(GENERIC_ALLOC),jemalloc)
  GENERIC_FLAG = -DSHM_GENERIC_JEMALLOC
  GENERIC_LIBS = $(JEMALLOC_LIB) -lpthread -ldl -lm
  GENERIC_DEPS = $(JEMALLOC_LIB)
else ifeq ($(GENERIC_ALLOC),cxlalloc)
  GENERIC_FLAG = -DSHM_GENERIC_CXLALLOC -I$(CXLALLOC_DIR)/cxlalloc-static/include
  GENERIC_LIBS = $(CXLALLOC_LIB) -lpthread -ldl -lm
  GENERIC_DEPS = $(CXLALLOC_LIB)
else ifeq ($(GENERIC_ALLOC),tcmalloc)
  GENERIC_FLAG = -DSHM_GENERIC_TCMALLOC
  GENERIC_LIBS = -lstdc++
  GENERIC_DEPS = tcmalloc-not-buildable-here
else ifeq ($(GENERIC_ALLOC),ralloc)
  GENERIC_FLAG = -DSHM_GENERIC_RALLOC
  GENERIC_LIBS =
  GENERIC_DEPS = ralloc-not-buildable-here
else
  GENERIC_FLAG =
  GENERIC_LIBS =
  GENERIC_DEPS =
endif

# -D_POSIX_C_SOURCE=200809L   unlocks shm_open, strdup, robust mutexes
# -Wall -Wextra               enable common warnings
# -g                          include debug symbols
CFLAGS   = -std=c11   -Wall -Wextra -g -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc $(CACHE_FLAG) $(GENERIC_FLAG) $(BI_FLAG)
CXXFLAGS = -std=c++17 -Wall -Wextra -g -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc $(CACHE_FLAG) $(GENERIC_FLAG) $(BI_FLAG)

# Link with POSIX threads (-pthread), the real-time library (-lrt, for
# shm_open), and whichever generic-allocator library was selected above.
LDFLAGS  = -pthread -lrt $(GENERIC_LIBS)

BUILD = build

# ── Library object files ─────────────────────────────────────────────────────
# shm_cap.o is compiled separately and added to the full library set so that
# any test (or user program) that includes shm_cap.h can link against it.
LIB_OBJS      = $(BUILD)/shm_alloc.o $(BUILD)/shm_ns.o
LIB_OBJS_CAP  = $(LIB_OBJS) $(BUILD)/shm_cap.o

# v0.2 "slim" format: a separate, incompatible module (see WIP.md). It only
# shares the generic shm_ns.o helper with v2, not shm_alloc.o.
LIB_OBJS_SLIM = $(BUILD)/shm_slim.o $(BUILD)/shm_ns.o

# Combined v2 + slim objects, for the test that exercises the generic
# backend against both formats in one binary.
LIB_OBJS_ALL  = $(BUILD)/shm_alloc.o $(BUILD)/shm_slim.o $(BUILD)/shm_ns.o

# v2-compatible API running on the slim engine (src/shm_slim_compat.c).
# NEVER link this alongside shm_alloc.o -- both define shm_alloc()/shm_ptr()/
# etc. and the link will fail with duplicate symbols. See shm_slim_compat.c.
LIB_OBJS_SLIM_V2COMPAT = $(BUILD)/shm_slim.o $(BUILD)/shm_slim_compat.o $(BUILD)/shm_ns.o

# v0.3 formal lifecycle API (src/dmalloc.c), built on the slim engine.
LIB_OBJS_DMALLOC = $(BUILD)/shm_slim.o $(BUILD)/dmalloc.o $(BUILD)/shm_ns.o

# ── Test binaries ────────────────────────────────────────────────────────────
TESTS = $(BUILD)/test_basic $(BUILD)/test_resize $(BUILD)/test_multihost \
        $(BUILD)/test_cap $(BUILD)/test_bounds \
        $(BUILD)/test_slim_basic $(BUILD)/test_slim_bounds $(BUILD)/test_slim_scale \
        $(BUILD)/test_generic_backend $(BUILD)/test_slim_v2compat \
        $(BUILD)/test_slim_namespace $(BUILD)/test_dmalloc_basic

.PHONY: all test clean tcmalloc-not-buildable-here ralloc-not-buildable-here

# Default target: build everything
all: $(TESTS)

# ── Create build directory if it does not exist ──────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

# ── Generic allocator backend: build the vendored libraries on demand ─────────
# Neither rule modifies the vendored source; both just run each project's own,
# unmodified build system. Only invoked when actually needed (GENERIC_DEPS is
# a prerequisite of shm_alloc.o/shm_slim.o below).

$(JEMALLOC_LIB):
	cd $(JEMALLOC_DIR) && ./autogen.sh && $(MAKE) -j$$(nproc)

$(CXLALLOC_LIB):
	@if [ -z "$(CXLALLOC_CARGO)" ]; then \
	  echo "error: rustup toolchain '$(CXLALLOC_TOOLCHAIN)' (pinned by $(CXLALLOC_DIR)/rust-toolchain.toml) was not found."; \
	  echo "       install it with: rustup toolchain install $(CXLALLOC_TOOLCHAIN)"; \
	  exit 1; \
	fi
	@if [ ! -e $(CXLALLOC_DIR)/extern/memento/Cargo.toml ]; then \
	  git -C $(CXLALLOC_DIR) submodule update --init extern/memento; \
	fi
	cd $(CXLALLOC_DIR) && RUSTC="$(CXLALLOC_RUSTC)" PATH="$(dir $(CXLALLOC_CARGO)):$$PATH" \
	  "$(CXLALLOC_CARGO)" build --release -p cxlalloc-static

# tcmalloc's vendored copy only ships a Bazel build (plus a CMake path that
# fetches abseil/protobuf/googletest/RE2 from GitHub at configure time), and
# Bazel is not installed here. ralloc is #![no_std] and depends on
# nightly-only Rust features removed years ago; no available toolchain (or
# any reasonably recent one) can build it. Both are real, external
# prerequisites -- not something to fake or silently skip -- see WIP.md.
tcmalloc-not-buildable-here:
	@echo "error: GENERIC_ALLOC=tcmalloc requires a self-built libtcmalloc (Bazel; not installed here)."
	@echo "       Build tcmalloc/ yourself (see tcmalloc/README.md), then point"
	@echo "       GENERIC_LIBS/-L at it manually, or see WIP.md for details."
	@exit 1

ralloc-not-buildable-here:
	@echo "error: GENERIC_ALLOC=ralloc requires a Rust nightly toolchain old enough"
	@echo "       to support features (optin_builtin_traits, feature-gated const_fn)"
	@echo "       removed from Rust years ago. See WIP.md for details."
	@exit 1

# ── Compile library sources ───────────────────────────────────────────────────
$(BUILD)/shm_alloc.o: src/shm_alloc.c include/shm_alloc.h src/shm_internal.h \
                      src/shm_persist.h src/shm_ns.h src/shm_generic_backend.h \
                      $(GENERIC_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_alloc.c -o $@

$(BUILD)/shm_ns.o: src/shm_ns.c src/shm_ns.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_ns.c -o $@

# shm_cap.c uses shm_internal.h (and thus shm_persist.h) to access block headers.
$(BUILD)/shm_cap.o: src/shm_cap.c include/shm_cap.h src/shm_internal.h \
                    src/shm_persist.h include/shm_alloc.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_cap.c -o $@

# v0.2 "slim" format -- self-contained, does not include shm_internal.h/shm_alloc.h.
$(BUILD)/shm_slim.o: src/shm_slim.c include/shm_slim.h src/shm_slim_internal.h \
                     src/shm_persist.h src/shm_ns.h src/shm_generic_backend.h \
                     $(GENERIC_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_slim.c -o $@

# v2-compatible API bodies, running on the slim engine. See the file header
# comment in src/shm_slim_compat.c for the linking rule this must follow.
$(BUILD)/shm_slim_compat.o: src/shm_slim_compat.c include/shm_slim.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_slim_compat.c -o $@

# v0.3 formal lifecycle API bodies.
$(BUILD)/dmalloc.o: src/dmalloc.c include/dmalloc.h include/shm_slim.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/dmalloc.c -o $@

# ── Compile and link tests ────────────────────────────────────────────────────
$(BUILD)/test_basic: tests/test_basic.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_basic.c $(LIB_OBJS) $(LDFLAGS)

$(BUILD)/test_resize: tests/test_resize.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_resize.c $(LIB_OBJS) $(LDFLAGS)

$(BUILD)/test_multihost: tests/test_multihost.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_multihost.c $(LIB_OBJS) $(LDFLAGS)

# test_cap exercises the capability layer; links against LIB_OBJS_CAP.
$(BUILD)/test_cap: tests/test_cap.c $(LIB_OBJS_CAP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_cap.c $(LIB_OBJS_CAP) $(LDFLAGS)

# test_bounds exercises the bounds-checked offset->block-header conversion
# and reaches into src/shm_internal.h to corrupt offsets/headers directly.
$(BUILD)/test_bounds: tests/test_bounds.c src/shm_internal.h $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_bounds.c $(LIB_OBJS) $(LDFLAGS)

# ── v0.2 "slim" test binaries ─────────────────────────────────────────────────
$(BUILD)/test_slim_basic: tests/test_slim_basic.c $(LIB_OBJS_SLIM) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_slim_basic.c $(LIB_OBJS_SLIM) $(LDFLAGS)

$(BUILD)/test_slim_bounds: tests/test_slim_bounds.c src/shm_slim_internal.h $(LIB_OBJS_SLIM) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_slim_bounds.c $(LIB_OBJS_SLIM) $(LDFLAGS)

$(BUILD)/test_slim_scale: tests/test_slim_scale.c src/shm_slim_internal.h $(LIB_OBJS_SLIM) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_slim_scale.c $(LIB_OBJS_SLIM) $(LDFLAGS)

# Exercises SHM_BACKEND_GENERIC / SHM_SLIM_BACKEND_GENERIC for both formats;
# always built and run, with or without GENERIC_ALLOC selected (see the file
# header comment in tests/test_generic_backend.c).
$(BUILD)/test_generic_backend: tests/test_generic_backend.c $(LIB_OBJS_ALL) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_generic_backend.c $(LIB_OBJS_ALL) $(LDFLAGS)

# Proves the v2-compatible API (bottom of shm_slim.h) works end to end.
$(BUILD)/test_slim_v2compat: tests/test_slim_v2compat.c $(LIB_OBJS_SLIM_V2COMPAT) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_slim_v2compat.c $(LIB_OBJS_SLIM_V2COMPAT) $(LDFLAGS)

# Optional key->namespace binding + shm_slim_set_key(); reaches into
# src/shm_slim_internal.h to fabricate a namespace mismatch directly.
$(BUILD)/test_slim_namespace: tests/test_slim_namespace.c src/shm_slim_internal.h $(LIB_OBJS_SLIM) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_slim_namespace.c $(LIB_OBJS_SLIM) $(LDFLAGS)

# v0.3 formal API sanity test.
$(BUILD)/test_dmalloc_basic: tests/test_dmalloc_basic.c $(LIB_OBJS_DMALLOC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_dmalloc_basic.c $(LIB_OBJS_DMALLOC) $(LDFLAGS)

# ── Run all tests ─────────────────────────────────────────────────────────────
test: all
	@echo "─── test_basic ──────────────────"
	./$(BUILD)/test_basic
	@echo "─── test_resize ─────────────────"
	./$(BUILD)/test_resize
	@echo "─── test_multihost ──────────────"
	./$(BUILD)/test_multihost
	@echo "─── test_cap ────────────────────"
	./$(BUILD)/test_cap
	@echo "─── test_bounds ─────────────────"
	./$(BUILD)/test_bounds
	@echo "─── test_slim_basic ─────────────"
	./$(BUILD)/test_slim_basic
	@echo "─── test_slim_bounds ────────────"
	./$(BUILD)/test_slim_bounds
	@echo "─── test_slim_scale ─────────────"
	./$(BUILD)/test_slim_scale
	@echo "─── test_generic_backend ────────"
	./$(BUILD)/test_generic_backend
	@echo "─── test_slim_v2compat ──────────"
	./$(BUILD)/test_slim_v2compat
	@echo "─── test_slim_namespace ─────────"
	./$(BUILD)/test_slim_namespace
	@echo "─── test_dmalloc_basic ──────────"
	./$(BUILD)/test_dmalloc_basic

# ── Remove build artifacts ────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
	# Remove stale shared memory objects left by failed or interrupted test runs
	-rm -f /dev/shm/shm_test_basic
	-rm -f /dev/shm/shm_test_lookup
	-rm -f /dev/shm/shm_test_perms
	-rm -f /dev/shm/shm_test_multi
	-rm -f /dev/shm/shm_test_resize_shrink
	-rm -f /dev/shm/shm_test_resize_inplace
	-rm -f /dev/shm/shm_test_resize_reloc
	-rm -f /dev/shm/shm_test_resize_perms
	-rm -f /dev/shm/shm_test_multihost
	-rm -f /dev/shm/shm_test_ns_enforce
	-rm -f /dev/shm/test_cap_region
	-rm -f /dev/shm/shm_test_parallel_dir
	-rm -f /dev/shm/shm_test_bounds_read
	-rm -f /dev/shm/shm_test_bounds_corrupt
	-rm -f /dev/shm/shm_test_bounds_dir
	-rm -f /dev/shm/shm_slim_test_basic
	-rm -f /dev/shm/shm_slim_test_anon
	-rm -f /dev/shm/shm_slim_test_openkey
	-rm -f /dev/shm/shm_slim_test_free
	-rm -f /dev/shm/shm_slim_test_names
	-rm -f /dev/shm/shm_slim_test_resize
	-rm -f /dev/shm/shm_slim_test_resize_name
	-rm -f /dev/shm/shm_slim_test_walk
	-rm -f /dev/shm/shm_slim_test_stats
	-rm -f /dev/shm/shm_slim_test_bounds_read
	-rm -f /dev/shm/shm_slim_test_bounds_corrupt
	-rm -f /dev/shm/shm_slim_test_bounds_walk
	-rm -f /dev/shm/shm_slim_test_bounds_key
	-rm -f /dev/shm/shm_slim_test_scale
	# test_generic_backend's regions never touch /dev/shm (SHM_BACKEND_GENERIC
	# sources memory from a library call, not shm_open), so nothing to clean.
	-rm -f /dev/shm/shm_slim_v2compat_test
	-rm -f /dev/shm/shm_slim_v2compat_resize
	-rm -f /dev/shm/shm_slim_test_ns_default
	-rm -f /dev/shm/shm_slim_test_ns_bind
	-rm -f /dev/shm/shm_slim_test_ns_capacity
	-rm -f /dev/shm/shm_slim_test_ns_mismatch
	-rm -f /dev/shm/shm_slim_test_set_key
	-rm -f /dev/shm/shm_slim_test_ns_rebind
	-rm -f /dev/shm/dmalloc_test_basic
