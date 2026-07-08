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

# -D_POSIX_C_SOURCE=200809L   unlocks shm_open, strdup, robust mutexes
# -Wall -Wextra               enable common warnings
# -g                          include debug symbols
CFLAGS   = -std=c11   -Wall -Wextra -g -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc $(CACHE_FLAG)
CXXFLAGS = -std=c++17 -Wall -Wextra -g -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc $(CACHE_FLAG)

# Link with POSIX threads (-pthread) and the real-time library (-lrt, for shm_open)
LDFLAGS  = -pthread -lrt

BUILD = build

# ── Library object files ─────────────────────────────────────────────────────
# shm_cap.o is compiled separately and added to the full library set so that
# any test (or user program) that includes shm_cap.h can link against it.
LIB_OBJS     = $(BUILD)/shm_alloc.o $(BUILD)/shm_ns.o
LIB_OBJS_CAP = $(LIB_OBJS) $(BUILD)/shm_cap.o

# ── Test binaries ────────────────────────────────────────────────────────────
TESTS = $(BUILD)/test_basic $(BUILD)/test_resize $(BUILD)/test_multihost \
        $(BUILD)/test_cap

.PHONY: all test clean

# Default target: build everything
all: $(TESTS)

# ── Create build directory if it does not exist ──────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

# ── Compile library sources ───────────────────────────────────────────────────
$(BUILD)/shm_alloc.o: src/shm_alloc.c include/shm_alloc.h src/shm_internal.h \
                      src/shm_persist.h src/shm_ns.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_alloc.c -o $@

$(BUILD)/shm_ns.o: src/shm_ns.c src/shm_ns.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_ns.c -o $@

# shm_cap.c uses shm_internal.h (and thus shm_persist.h) to access block headers.
$(BUILD)/shm_cap.o: src/shm_cap.c include/shm_cap.h src/shm_internal.h \
                    src/shm_persist.h include/shm_alloc.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_cap.c -o $@

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
