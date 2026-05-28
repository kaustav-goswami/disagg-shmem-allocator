# ─────────────────────────────────────────────────────────────────────────────
# Makefile for shm_alloc — shared / disaggregated memory allocator
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CXX     = g++

# -D_POSIX_C_SOURCE=200809L   unlocks shm_open, strdup, robust mutexes
# -Wall -Wextra               enable common warnings
# -g                          include debug symbols
CFLAGS   = -std=c11   -Wall -Wextra -g -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc
CXXFLAGS = -std=c++17 -Wall -Wextra -g -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc

# Link with POSIX threads (-pthread) and the real-time library (-lrt, for shm_open)
LDFLAGS  = -pthread -lrt

BUILD = build

# ── Library object files ─────────────────────────────────────────────────────
LIB_OBJS = $(BUILD)/shm_alloc.o $(BUILD)/shm_ns.o

# ── Test binaries ────────────────────────────────────────────────────────────
TESTS = $(BUILD)/test_basic $(BUILD)/test_resize $(BUILD)/test_multihost

.PHONY: all test clean

# Default target: build everything
all: $(TESTS)

# ── Create build directory if it does not exist ──────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

# ── Compile library sources ───────────────────────────────────────────────────
$(BUILD)/shm_alloc.o: src/shm_alloc.c include/shm_alloc.h src/shm_ns.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_alloc.c -o $@

$(BUILD)/shm_ns.o: src/shm_ns.c src/shm_ns.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/shm_ns.c -o $@

# ── Compile and link tests ────────────────────────────────────────────────────
$(BUILD)/test_basic: tests/test_basic.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_basic.c $(LIB_OBJS) $(LDFLAGS)

$(BUILD)/test_resize: tests/test_resize.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_resize.c $(LIB_OBJS) $(LDFLAGS)

$(BUILD)/test_multihost: tests/test_multihost.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_multihost.c $(LIB_OBJS) $(LDFLAGS)

# ── Run all tests ─────────────────────────────────────────────────────────────
test: all
	@echo "─── test_basic ──────────────────"
	./$(BUILD)/test_basic
	@echo "─── test_resize ─────────────────"
	./$(BUILD)/test_resize
	@echo "─── test_multihost ──────────────"
	./$(BUILD)/test_multihost

# ── Remove build artifacts ────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
	# Also remove any stale shared memory objects left by failed test runs
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
