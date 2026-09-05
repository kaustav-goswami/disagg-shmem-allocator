# shm_alloc — Shared / Disaggregated Memory Allocator

A C11 / C++17 allocator for **POSIX shared memory** and **DAX / CXL disaggregated memory**, with:

- Self-describing region header with an **object directory** for multi-host discovery
- Monotonic **object IDs** — no hardcoded offsets
- **Dynamic resize** (grow in-place or with relocation; shrink)
- **User-id ownership** and per-object **permission bits**
- **Namespace / cgroup protection** against cross-boundary access
- **Bounds-checked pointer resolution** — a stale, corrupted, or out-of-range
  offset fails cleanly (`NULL` / `EINVAL` / `EFAULT`) instead of dereferencing
  memory outside the mapped region
- **Parallel directory lookups** — `shm_dir_read_slot()` lets worker threads
  partition the object directory and load metadata concurrently
- **Pluggable memory backend** — source a region from POSIX shm, a DAX
  device, or a vendored allocator (jemalloc / tcmalloc / cxlalloc / ralloc),
  selected at compile time
- C++ typed wrappers with placement new, `static_cast`, and `dynamic_cast`
- A **v0.2 "slim" format** with 16 bytes of metadata per object instead of
  152 — API-compatible with the above by swapping one `#include`

Parts of this program is vibe coded.

---

## Two formats: v2 and v0.2 "slim"

This repository now ships **two independent, mutually incompatible formats**:

- **v2** (everything below, through [File structure](#file-structure)) — the
  original, full-featured format: object directory, monotonic IDs, owner +
  permission bitmask, type tags, the CHERI-like capability layer, C++ typed
  wrappers. 152 bytes of metadata per object (56-byte block header + 96-byte
  directory entry).
- **[v0.2 "slim"](#v02--the-slim-format)** — a from-scratch, much smaller
  format built for the same disaggregated-memory use case at much larger
  object counts. 16 bytes of metadata per anonymous object. See that section
  for the full design and [WIP.md](WIP.md) for the rationale and every
  decision made along the way.

They are separate modules (`shm_alloc.*` / `shm_cap.*` vs. `shm_slim.*`) that
can be built and used independently, or together in the same program (no
symbol or type collisions). Neither can open a region created by the other —
see [Incompatibility with v2](#incompatibility-with-v2) for how that's
enforced.

---

## Memory layout

*(v2 format. For the slim format, jump to [v0.2 "slim"](#v02--the-slim-format).)*

The mapping is split into three contiguous zones.  `dir_offset` and
`data_offset` are written into the region header at creation time, so a new
host process can find everything by reading just those two fields.

```
byte 0                 +0x0000c8              +0x0060c8
  │                        │                      │
  ▼                        ▼                      ▼
┌────────────────────┬──────────────────────┬───────────────────────────┐
│   region header    │   object directory   │   heap (blocks…)          │
│      200 B         │  N × 96 B each       │  region_size − data_offset│
└────────────────────┴──────────────────────┴───────────────────────────┘
```

**Default offsets for a 1 MiB region with 256 directory slots:**

```
  [0x000000 – 0x0000c8)  region header         200 B
  [0x0000c8 – 0x0060c8)  object directory    24 576 B   (256 × 96 B)
  [0x0060c8 – 0x100000)  heap             1 023 800 B
```

---

## Permission and ownership layout

Security fields are marked `◀ SECURITY` in each diagram.
All sizes are for x86-64 Linux (LP64 ABI).

### Region header — `shm_region_hdr_t`  (200 bytes total)

Holds identity, locking, and **namespace / cgroup fingerprints** used for
cross-host access control.

```
 Offset  Size  Field
──────────────────────────────────────────────────────────────────────────
   +0      4   magic          0x53484D41 ("SHMA") — corruption guard
   +4      4   version        format version; reject mismatch on open
   +8      4   backend        0 = POSIX shm_open  /  1 = DAX /dev/daxX.Y
  +12      4   dir_capacity   max slots in the object directory
  +16     40   mutex          process-shared robust pthread_mutex_t
                              (locked for every alloc / free / resize)
  +56      4   host_count     number of processes currently attached
  +60      4   (pad)
  +64      8   region_size    total mapping size in bytes
  +72      8   dir_offset     byte offset of object directory from base
  +80      8   data_offset    byte offset of heap start from base
  +88      8   free_list      1-based hoff link to first free heap block
  +96      8   used_bytes     bytes committed to live blocks
 +104      8   block_count    number of live heap blocks
 +112      8   next_id        next monotonic object ID (starts at 1)
 +120      8   ns_inode     ◀ SECURITY — IPC namespace inode of creator
                              checked when SHM_OPEN_ENFORCE_NS is set;
                              EPERM if caller's /proc/self/ns/ipc differs
 +128      8   cgroup_hash  ◀ SECURITY — FNV-1a hash of creator's
                              /proc/self/cgroup; checked when
                              SHM_OPEN_ENFORCE_CGROUP is set
 +136     64   shm_name       the name / path used at creation (debug)
──────────────────────────────────────────────────────────────────────────
          200  total
```

### Directory entry — `shm_obj_entry_t`  (96 bytes per slot)

One slot per live object.  Permission checks for `shm_lookup()`,
`shm_free()`, and `shm_resize()` all start here before the heap block
is touched.

```
 Offset  Size  Field
──────────────────────────────────────────────────────────────────────────
   +0      8   id           monotonic uint64_t; 0 = slot is free
   +8     48   name[48]     optional human-readable name (NUL-padded)
  +56      8   off          heap-relative payload offset (shm_off_t)
  +64      8   capacity     physical payload bytes allocated
  +72      8   used_size    logical bytes in use (updated by shm_resize)
  +80      4   type_tag     caller-defined type identifier
  +84      4   owner      ◀ SECURITY — shm_user_id_t (uint32_t) of the
                            process that called shm_alloc(); must match
                            the caller's user_id on every mutation unless
                            the caller supplies SHM_PERM_ADMIN
  +88      4   perms      ◀ SECURITY — shm_perm_t bitmask stored on
                            the object; each operation checks that the
                            required bit is set here AND in the caller's
                            own perm argument before proceeding
  +92      1   alive        1 = live object; 0 = freed / reusable slot
  +93      3   (pad)
──────────────────────────────────────────────────────────────────────────
          96   total
```

### Heap block header — `shm_block_hdr_t`  (56 bytes)

Lives immediately before every payload in the heap.  Fields are a
**redundant copy** of the directory entry's security fields so that
`shm_ptr()` can validate a raw offset without a directory scan.

```
 Offset  Size  Field
──────────────────────────────────────────────────────────────────────────
   +0      4   magic        0x424C4B21 ("BLK!") — detects corrupt/wrong offset
   +4      4   type_tag     copy of directory entry's type_tag
   +8      4   owner      ◀ SECURITY — copy of directory entry's owner;
                            shm_ptr() compares this against caller's user_id
  +12      4   perms      ◀ SECURITY — copy of directory entry's perms;
                            shm_ptr() ANDs required_perms against this mask
  +16      1   allocated  ◀ SECURITY — 1 = live, 0 = on free list;
                            shm_ptr() returns NULL if this is 0 (use-after-free guard)
  +17      7   (pad)        keeps obj_id on an 8-byte boundary
  +24      8   obj_id       directory entry ID; 0 for free blocks
  +32      8   payload_cap  physical payload bytes (excl. this header)
  +40      8   total_size   sizeof(header) + payload_cap; used by the heap
  +48      8   next_free    1-based hoff of next free block; 0 = end of list
──────────────────────────────────────────────────────────────────────────
          56   total
```

### Permission bitmask — `shm_perm_t`  (uint32_t, 4 bytes)

```
 Bit   Hex    Constant          Required by
──────────────────────────────────────────────────────────────────────────
   0  0x01   SHM_PERM_READ     shm_ptr()          (read access)
   1  0x02   SHM_PERM_WRITE    shm_ptr()          (write access)
   2  0x04   SHM_PERM_FREE     shm_free()
   3  0x08   SHM_PERM_RESIZE   shm_resize()
 4–6   —     (reserved)
   7  0x80   SHM_PERM_ADMIN    bypass owner-id check on all operations
 8–31  —     (reserved)
──────────────────────────────────────────────────────────────────────────
  0x0F  SHM_PERM_DEFAULT  = READ | WRITE | FREE | RESIZE
```

### Check flow for every data-plane operation

```
caller supplies:  user_id  +  caller_perms  +  required_perms
                       │            │                  │
                       ▼            ▼                  ▼
              ┌────────────────────────────────────────────────┐
              │  1. block->magic == SHM_ALLOC_MAGIC_BLOCK?     │ → EINVAL
              │  2. block->allocated == 1?                     │ → EINVAL
              │  3. caller_perms & SHM_PERM_ADMIN?             │ → PASS (skip 4–6)
              │  4. user_id == block->owner?                   │ → EACCES
              │  5. (block->perms & required) == required?     │ → EACCES
              │  6. (caller_perms & required) == required?     │ → EACCES
              └────────────────────────────────────────────────┘
                                    │ all pass
                                    ▼
                          return payload pointer
```

`shm_free()` and `shm_resize()` run the same check via the directory entry
**before** touching any heap block, giving a second layer of validation.

---

## Backends

| Backend | How to select | Notes |
|---------|--------------|-------|
| `SHM_BACKEND_POSIX` | default | `shm_open("/name")` + `mmap(MAP_SHARED)` |
| `SHM_BACKEND_DAX`   | `opts.backend = SHM_BACKEND_DAX` | `open("/dev/dax0.0")` + `mmap(MAP_SHARED)`, no `ftruncate` |
| `SHM_BACKEND_GENERIC` | `opts.backend = SHM_BACKEND_GENERIC` + a compile-time flag | sources the region from jemalloc / tcmalloc / cxlalloc / ralloc instead -- see below |

---

## Generic allocator backend

A third backend lets you source a region's backing memory from a vendored
allocator library instead of `shm_open`/DAX -- available identically on
v2 (`SHM_BACKEND_GENERIC`) and [v0.2 slim](#v02--the-slim-format)
(`SHM_SLIM_BACKEND_GENERIC`). Select exactly one library at compile time:

```bash
make GENERIC_ALLOC=jemalloc   # builds jemalloc/ (autoconf) and links it
make GENERIC_ALLOC=cxlalloc   # builds cxlalloc/cxlalloc-static (cargo) and links it
make GENERIC_ALLOC=tcmalloc   # wired, but you must build libtcmalloc yourself (needs Bazel)
make GENERIC_ALLOC=ralloc     # wired, but not buildable with any available Rust toolchain
```

None of the vendored submodules (`jemalloc/`, `tcmalloc/`, `cxlalloc/`,
`ralloc/`) are modified — this only calls each library's existing public
API. `jemalloc` and `cxlalloc` are fully working (built and tested end to
end as part of this change); `tcmalloc` and `ralloc` are wired with the
correct real symbol names but fail the build with an explanatory message
instead of silently doing nothing — see [WIP.md](WIP.md) for exactly what
each one required (or didn't) to build, and a known caveat around using the
cxlalloc backend for both v2 and slim in the same process.

```c
shm_region_open_opts_t opts = {
    .backend = SHM_BACKEND_GENERIC,
    .flags   = SHM_OPEN_CREATE,
};
shm_region_t *region;
shm_region_open("my-pool", 1 << 20, &opts, &region);   // name is just a debug label here
```

**This is single-process only.** A generic-backend region is memory
obtained from a library call (`malloc`-style) in the calling process's own
address space, not a named OS object like a POSIX shm segment or DAX device
that a second process can independently attach to. Opening a generic-backend
region with `SHM_OPEN_CREATE` unset returns `ENOTSUP` — this is a real
capability gap versus POSIX/DAX, not an unimplemented detail. (cxlalloc
itself does support real cross-process sharing via its `heap_id` and
`"shm"`/`"ivshmem"` heap-backend modes; this integration uses its `"mmap"`
mode and doesn't yet take advantage of that — a natural follow-up.)

---

## Multi-host discovery

Every `shm_alloc()` assigns a **monotonically increasing uint64_t ID** and registers the object in the directory with an optional name.

A new host opens the region without `SHM_OPEN_CREATE`, then:

```c
// Option A: look up by known ID
shm_off_t off;
shm_lookup(region, known_id, uid, SHM_PERM_READ, &off, NULL);

// Option B: look up by name
shm_lookup_by_name(region, "graph_csr", uid, SHM_PERM_READ, &id, &off);

// Option C: iterate everything in the directory
uint32_t cursor = 0;
shm_dir_entry_t entry;
while (shm_dir_next(region, &cursor, &entry) == 0) {
    printf("id=%llu name=%s size=%zu\n", entry.id, entry.name, entry.used_size);
}
```

---

## Parallel directory lookups

`shm_dir_next()` iterates the directory through a single shared cursor, so
concurrent readers either serialize on it or race each other. `shm_dir_read_slot()`
gives worker threads direct access to a physical slot index, so a fleet of
readers can partition `[0, dir_capacity)` and load object metadata in
parallel with no shared state:

```c
shm_pool_stats_t st;
shm_pool_stats(region, &st);

// Worker `w` of `n_workers` scans every n_workers-th slot.
for (uint32_t i = worker_id; i < st.dir_capacity; i += n_workers) {
    shm_dir_entry_t e;
    int rc = shm_dir_read_slot(region, i, SHM_DIR_READ_LOCK_FREE, &e);
    if (rc == 0) {
        // e.id, e.off, e.name, ... — same fields as shm_dir_next()
    } else if (rc == ENOENT) {
        continue;   // slot is unused or was freed
    }
}
```

`shm_dir_read_mode_t` selects how the read is synchronized:

| Mode                     | Behavior |
|--------------------------|----------|
| `SHM_DIR_READ_LOCKED`    | Takes the region mutex; serializes with `shm_alloc`/`shm_free`/`shm_resize`/`shm_block_set_perms`. Safe at any time. |
| `SHM_DIR_READ_LOCK_FREE` | No locking. Only safe under a **quiescent-directory contract**: no process may allocate, free, resize, or change permissions while any lock-free reader is running. Violating this is undefined behavior — use it for read-mostly / snapshot-style workloads (e.g. a scan phase between mutation phases), not concurrently with live writers. |

`shm_dir_read_slot()` returns `0` with `*out` filled, `ENOENT` for an unused
or freed slot, `ERANGE` if `slot_index >= dir_capacity`, or `EINVAL` for a
NULL region/out pointer or an unrecognized mode.

As a companion optimization, `shm_alloc()`'s free-slot scan now starts at the
slot corresponding to `next_id` instead of always scanning from slot 0, making
append-only allocation patterns (the common case) O(1) instead of O(directory
size); it still scans cyclically so freed slots are still reused.

---

## Build

```bash
make          # build all test binaries
make test     # build + run all tests
make clean    # remove build artefacts and stale /dev/shm objects
```

Requires GCC / G++ with C11 and C++17 support, `libpthread`, `librt`.

---

## C API quick reference

### Region lifecycle

```c
// Create a fresh 1 MB region with 256 directory slots
shm_region_open_opts_t opts = {
    .backend      = SHM_BACKEND_POSIX,
    .flags        = SHM_OPEN_CREATE,
    .dir_capacity = 256,
};
shm_region_t *region;
shm_region_open("/my_pool", 1 << 20, &opts, &region);

// Open existing region on a second host (no create)
shm_region_open_opts_t ro = { .backend = SHM_BACKEND_POSIX, .flags = 0 };
shm_region_open("/my_pool", 0, &ro, &region);

shm_region_close(region, /*unlink=*/true);
```

### Allocation (registers in directory)

```c
uint64_t id;
shm_off_t off;

// Typed alloc (sizeof is used for capacity and type_tag)
shm_alloc_typed(region, MyStruct, "my_obj", user_id, SHM_PERM_DEFAULT, &id, &off);

// Raw alloc with explicit size and tag
shm_alloc(region, 4096, "my_buf", user_id, SHM_PERM_DEFAULT, MY_TYPE_TAG, &id, &off);
```

### Typed pointer access

```c
// Mutable access (checks READ|WRITE)
MyStruct *p = shm_cast_mut(region, off, MyStruct, user_id, SHM_PERM_DEFAULT);
p->field = 42;

// Read-only access
const MyStruct *q = shm_cast(region, off, MyStruct, user_id, SHM_PERM_READ);
```

### Lookup

```c
shm_off_t off; size_t used;
shm_lookup(region, id, user_id, SHM_PERM_READ, &off, &used);
shm_lookup_by_name(region, "my_obj", user_id, SHM_PERM_READ, &id, &off);
```

### Resize (grow / shrink)

```c
// Grow to 8192 bytes (may relocate; re-lookup offset afterwards)
shm_resize(region, id, 8192, user_id, SHM_PERM_DEFAULT);
shm_lookup(region, id, user_id, SHM_PERM_READ, &off, NULL);  // refresh offset

// Shrink to 512 bytes (always in-place; offset unchanged)
shm_resize(region, id, 512, user_id, SHM_PERM_DEFAULT);
```

### Free

```c
shm_free_id(region, id, user_id, SHM_PERM_DEFAULT);
```

---

## C++ API

```cpp
#include "shm_alloc.hpp"

// Allocate + construct in one step
shm::ptr<Derived> h;
shm::make<Derived>(region, user_id, SHM_PERM_DEFAULT, &h, /*name=*/nullptr,
                   shm::type_tag_of<Derived>(), /* ctor args: */ 99);

// Mutable access
Derived *d = shm::get_mut(region, h, user_id, SHM_PERM_DEFAULT);

// static_cast to Base
Base *b = shm::cast<Base>(region, h, user_id, SHM_PERM_DEFAULT);

// dynamic_cast (checks type_tag; uses vtable for polymorphic types)
Derived *d2 = shm::cast_dyn<Derived>(region, shm::ptr<Base>{h.raw()}, uid, perms);

// Lookup by ID → typed handle
shm::ptr<Derived> found;
shm::find<Derived>(region, known_id, user_id, SHM_PERM_READ, &found);

// Destroy + free
shm::free(region, h, user_id, SHM_PERM_DEFAULT);
```

---

## Namespace / cgroup protection

The creator's IPC namespace inode and cgroup hash are stored in the region header.
Enable enforcement when opening:

```c
shm_region_open_opts_t opts = {
    .backend = SHM_BACKEND_POSIX,
    .flags   = SHM_OPEN_ENFORCE_NS | SHM_OPEN_ENFORCE_CGROUP,
};
shm_region_open("/my_pool", 0, &opts, &region);
// → returns EPERM if caller is in a different IPC namespace or cgroup
```

---

## Permission model

| Bit | Constant | Required for |
|-----|----------|-------------|
| 0x01 | `SHM_PERM_READ`   | `shm_ptr()` read, `shm_lookup()` |
| 0x02 | `SHM_PERM_WRITE`  | `shm_ptr()` write |
| 0x04 | `SHM_PERM_FREE`   | `shm_free()` |
| 0x08 | `SHM_PERM_RESIZE` | `shm_resize()` |
| 0x80 | `SHM_PERM_ADMIN`  | bypass owner-id check |

Both the **block's stored perms** and the **caller's perms** must contain the required bits.

---

## Bounds-checked offset resolution

Every `shm_off_t` that reaches a data-plane function is either supplied
directly by a caller (`shm_ptr`, `shm_block_info`, `shm_block_set_perms`) or
read back out of a directory entry (`shm_free`, `shm_resize`). In either case
the value can be stale, corrupted (e.g. a torn write from another host), or
simply wrong. Converting such an offset into a block-header pointer with raw
arithmetic — `heap_base + off - sizeof(header)` — can land outside
`[heap_base, heap_base + heap_size)` and get dereferenced immediately,
turning a bad offset into a crash or an out-of-bounds read/write instead of a
clean error.

All five of these functions now resolve `off` through a single bounds-checked
choke point before touching the block header. It rejects the offset if:

1. it is the `SHM_OFF_NULL` sentinel, or too small to have a header before it,
2. the header itself would not fit entirely inside the mapped heap, or
3. the header's own `total_size` claims to extend past the end of the heap
   (a corrupted-but-in-range header).

| Function                  | Behavior on an invalid offset (unchanged call signature) |
|----------------------------|-----------------------------------------------------------|
| `shm_ptr()`                | Returns `NULL` (same as any other access-check failure) |
| `shm_block_info()`         | Returns `EINVAL` |
| `shm_block_set_perms()`    | Returns `EINVAL` |
| `shm_free()`               | Returns `EFAULT` (directory entry points outside the heap) |
| `shm_resize()`             | Returns `EFAULT` (directory entry points outside the heap) |

No public API signatures changed — existing callers that already check the
return value / NULL correctly require no changes. `tests/test_bounds.c`
covers the null sentinel, zero, just-past-heap-end, wildly out-of-range
offsets, a header whose `total_size` overruns the heap, and directory entries
corrupted to point outside the heap for both `shm_free()` and `shm_resize()`.

---

## Comparison with CHERI capability pointers

[CHERI](https://www.cl.cam.ac.uk/research/security/ctsrd/cheri/) (Capability
Hardware Enhanced RISC Instructions) is a hardware ISA extension that replaces
raw pointers with *capabilities* — hardware-tagged, bounds-checked, permission-
bearing tokens that cannot be forged or widened by software.

`shm_alloc` reaches for many of the same safety properties in *software*, applied
to shared / disaggregated memory rather than a single address space.  The
comparison is instructive for understanding what the allocator does, what it
intentionally omits, and where the two models diverge.

---

### Side-by-side field mapping

A 128-bit CHERI capability (morello / RISC-V Cheriot layout, compressed) next to
the fields that form our "software capability" — the `shm_off_t` handle backed
by a `shm_block_hdr_t` and a `shm_obj_entry_t`.

```
┌─────────────────────────────────────────┬──────────────────────────────────────────────┐
│       CHERI capability  (128-bit)       │  shm_alloc software capability               │
│                                         │  handle (shm_off_t, 8 B)                     │
│                                         │  + block header (56 B)                       │
│                                         │  + directory entry (96 B)                    │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ Tag bit (1, hardware-maintained)        │ block->allocated (uint8_t, +16 in hdr)       │
│   0 = invalid / revoked capability      │   0 = block is on free list (revoked)        │
│   1 = valid, may be used as a pointer   │   1 = block is live, shm_ptr() will proceed  │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ Address / cursor (64-bit)               │ shm_off_t  (heap-relative payload offset)    │
│   current pointer value within bounds  │   process-neutral; re-based per mmap         │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ Base  (compressed in capability)        │ heap_base(region) + off − sizeof(hdr)        │
│   lowest valid address                  │   block header address (start of allocation) │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ Top / Length  (compressed in cap.)      │ block->payload_cap  (+32 in hdr, 8 B)        │
│   upper bound = base + length           │   payload bytes; shm_ptr never returns a ptr │
│                                         │   past base + payload_cap                    │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ Permissions  (hardware-defined bits)    │ block->perms  shm_perm_t  (+12 in hdr, 4 B) │
│   LOAD / STORE / EXECUTE /              │   READ / WRITE / FREE / RESIZE / ADMIN       │
│   LOAD_CAP / STORE_CAP / SEAL /…        │   (two-party check: block AND caller)        │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ Object type  (otype, sealed caps only)  │ block->type_tag  (+4 in hdr, 4 B)           │
│   software-defined sealed type token    │   shm::cast_dyn<To>() checks this before     │
│   prevents direct dereference; must     │   dynamic_cast; sizeof(T) default or custom  │
│   be unsealed via matching otype cap    │   protocol enum                              │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ (no concept of owner identity)          │ block->owner  shm_user_id_t  (+8, 4 B)      │
│                                         │   uint32_t set at alloc time; caller must    │
│                                         │   match or hold SHM_PERM_ADMIN               │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ (no OS-level containment)               │ region_hdr->ns_inode  (+120, 8 B)           │
│                                         │ region_hdr->cgroup_hash  (+128, 8 B)        │
│                                         │   Linux namespace / cgroup fingerprints;     │
│                                         │   checked on shm_region_open() when          │
│                                         │   enforcement flags are set                  │
├─────────────────────────────────────────┼──────────────────────────────────────────────┤
│ Integrity: hardware tag (unforgeable)   │ block->magic  0x424C4B21  (+0, 4 B)         │
│   CPU refuses to use a capability whose │   shm_ptr() returns NULL on mismatch;        │
│   tag was not set by a store-cap instr  │   software-only; a malicious writer can      │
│                                         │   still fake the magic (see Differences)     │
└─────────────────────────────────────────┴──────────────────────────────────────────────┘
```

---

### Where `shm_alloc` mirrors CHERI

**Opaque, non-address handle.**
In CHERI, application code never manipulates raw addresses; it derives new
capabilities from existing ones.  In `shm_alloc`, application code never forms
a raw pointer — it calls `shm_ptr()` (or a typed wrapper) which re-validates on
every dereference.  The `shm_off_t` is an integer, not an address; process A and
process B get different virtual addresses for the same object, just as CHERI
addresses are relative to base.

**Bounds enforcement.**
CHERI hardware traps any load/store outside `[base, base+length)`.
`shm_alloc` encodes bounds in `block->payload_cap` and will only return a
pointer to the payload region; out-of-bounds offsets produce a NULL.

**Permission narrowing.**
Both systems store permission bits on the capability itself, not only on the
page table.  CHERI disallows widening permissions (you can only clear bits from
a derived capability).  `shm_alloc` enforces this at the API level: `shm_ptr()`
requires the caller to declare what permissions they need, and checks those
against both the block's stored mask and the caller's supplied mask.

**Liveness / revocation.**
CHERI's tag bit becomes 0 when a capability is invalidated.  The `allocated`
byte in the block header plays the same role: when a block is freed it is set to
0, and any subsequent call to `shm_ptr()` with the same offset returns NULL,
preventing use-after-free through stale `shm_off_t` values.

**Type sealing.**
CHERI sealed capabilities cannot be directly dereferenced; they carry an object
type token and must be unsealed by code that holds a matching sealing capability.
`type_tag` plus `shm::cast_dyn<T>()` approximate this: a mismatch between the
stored tag and `type_tag_of<T>()` aborts the cast, guarding against aliasing
between objects that happen to share the same heap offset over time.

**Two-party permission check.**
CHERI checks capability permissions at hardware level *and* honours the CPU
privilege ring.  `shm_alloc` checks two independent permission words on every
operation: `block->perms` (set by the allocator at creation time) and
`caller_perms` (provided by the calling code at dereference time).  Both must
contain the required bits — analogous to needing both a valid capability and
sufficient privilege ring.

---

### Where `shm_alloc` diverges from CHERI

| Dimension | CHERI | `shm_alloc` |
|-----------|-------|-------------|
| **Enforcement** | Hardware; cannot be bypassed by any software | Software; a process with write access to the mapping can corrupt headers |
| **Granularity** | Every load/store instruction is checked | Checked at allocator API boundaries only |
| **Unforgeability** | Tag bit is writable only by capability-store instructions; integer stores always clear it | `magic` field is an integrity hint, not a cryptographic guarantee |
| **Sub-object bounds** | Can restrict a capability to a field within a struct | No sub-object bounds; the unit is always the whole heap block |
| **Monotonic IDs** | No native concept; capabilities can be reused across objects | `next_id` is never decremented; a stale ID after free returns `ENOENT`, not a dangling reference |
| **Multi-host identity** | No concept of "which machine" holds a capability | `ns_inode` + `cgroup_hash` add Linux-level containment across hosts sharing a DAX device |
| **Owner identity** | No per-capability owner; access governed by sealing and permissions alone | `shm_user_id_t owner` adds a discretionary access layer on top of permissions |
| **Sharing model** | Capability can be passed between compartments as a value | `shm_off_t` is a plain integer; any process with the fd can open the same region, access control then applies |

---

### The gap that matters most

The fundamental difference is **trust model**.  A CHERI CPU refuses to execute
any memory operation that does not originate from a valid tagged capability —
there is no way for software running at user privilege to forge one.

In `shm_alloc`, the region is a `mmap`-backed file.  Any process holding the
file descriptor and sufficient OS permissions can write arbitrary bytes into the
mapping, overwriting magic values, forging permission masks, or clearing the
`allocated` flag.  The allocator's checks protect against *accidental* misuse
(wrong type, stale offset, access by the wrong user) but not against a
*malicious* co-tenant.

Combining `shm_alloc` with OS-level isolation — separate cgroups, separate IPC
namespaces (`SHM_OPEN_ENFORCE_NS`), and filesystem permissions on `/dev/shm` or
the DAX device — closes most of this gap for the disaggregated-memory use case,
where the threat model is node failure and buggy code rather than active
adversaries sharing the same physical hardware.

---

## CHERI-like capability pointer mode

`shm_alloc` ships an optional capability layer that lets you hold a reference
to a shared-memory object as a **128-bit software capability** instead of a
plain `shm_off_t` offset.  Include `shm_cap.h` (C) or `shm_cap.hpp` (C++) and
link against `shm_cap.o`; existing code that uses `shm_off_t` / `shm_ptr()`
continues to work unchanged on the same region — the two representations
coexist at runtime.

### 128-bit capability layout

```
 127                  64 63                   0
┌──────────────────────┬──────────────────────┐
│       meta           │        addr          │
│   (high 64 bits)     │   (low  64 bits)     │
└──────────────────────┴──────────────────────┘

addr  — heap-relative payload offset  (identical to shm_off_t)

meta bit layout:

 63      56 55      48 47             32 31              0
┌──────────┬──────────┬────────────────┬──────────────────┐
│  perms   │ owner_lo │    epoch       │     bounds       │
│  (8 bit) │  (8 bit) │   (16 bit)     │    (32 bit)      │
└──────────┴──────────┴────────────────┴──────────────────┘
```

| Field      | Bits    | Width | Contents                                          |
|------------|---------|-------|---------------------------------------------------|
| `perms`    | [63:56] | 8 b   | `shm_perm_t` bits sealed at derive time           |
| `owner_lo` | [55:48] | 8 b   | Low byte of `shm_user_id_t` (identity hint)       |
| `epoch`    | [47:32] | 16 b  | Low 16 bits of `obj_id` — ABA protection token   |
| `bounds`   | [31: 0] | 32 b  | `payload_cap` at derive time — upper access bound |

### The tag bit

The **tag bit** is *not* stored in the capability value itself — it lives in
the heap block header as `shm_block_hdr_t::allocated`.  Every
`shm_cap_deref()` call re-reads the live header.  This means that freeing a
block **immediately** invalidates every outstanding capability that points to
it — no revocation scan required.

### Validation steps on every dereference

`shm_cap_deref(region, cap, required_perms)` performs seven ordered checks:

1. **Non-null** — `addr != 0 || meta != 0`
2. **Magic** — `block->magic == SHM_ALLOC_MAGIC_BLOCK` (corruption guard)
3. **Tag bit** — `block->allocated == 1` (use-after-free detection)
4. **Epoch / ABA** — `(block->obj_id & 0xFFFF) == meta.epoch`
5. **Bounds** — `block->payload_cap == meta.bounds` (resize invalidates old caps)
6. **Capability perms** — `(meta.perms & required) == required`
7. **Block perms** — `(block->perms & required) == required` (live narrowing)

If any check fails the function returns `NULL`; no exception is thrown.

### Capability monotonicity (narrowing)

```c
shm_cap_t ro = shm_cap_narrow(rw_cap, SHM_PERM_READ);
```

`shm_cap_narrow()` removes permission bits — it can never add them.  Passing
a `new_perms` value with a bit absent from the current capability returns
`SHM_CAP_NULL`.  This mirrors CHERI's capability monotonicity invariant in
software.

### C API quick-reference

```c
#include "shm_cap.h"

/* Derive a capability from an existing allocation. */
shm_cap_t cap = shm_cap_derive(region, off, uid, caller_perms, grant_perms);

/* Validate + return payload pointer (NULL on any failure). */
MyObj *p = shm_cap_deref_as(region, cap, MyObj, SHM_PERM_READ | SHM_PERM_WRITE);

/* Narrow to read-only (removes WRITE bit). */
shm_cap_t ro = shm_cap_narrow(cap, SHM_PERM_READ);

/* Structural validity check (no permission argument). */
bool live = shm_cap_is_valid(region, cap);

/* Field accessors. */
shm_perm_t p  = shm_cap_perms(cap);   /* sealed perm mask   */
size_t     b  = shm_cap_bounds(cap);  /* sealed payload cap */
shm_off_t  o  = shm_cap_offset(cap);  /* heap offset        */
uint16_t   e  = shm_cap_epoch(cap);   /* ABA epoch          */
```

### C++ API quick-reference

```cpp
#include "shm_cap.hpp"

// Derive from a typed handle
shm::cap<MyObj> c = shm::cap<MyObj>::from_ptr(
    region, handle, uid, caller_perms, SHM_PERM_READ | SHM_PERM_WRITE);

// Read-write deref
MyObj *p = c.rw(region);           // requires READ | WRITE
const MyObj *r = c.get(region);    // requires READ (default)

// Narrow and pass to untrusted code
shm::cap<MyObj> ro = c.narrow(SHM_PERM_READ);

// Boolean validity check
if (!ro.valid(region)) { /* handle stale/freed */ }

// Unsafe reinterpret (type annotation only, no security impact)
shm::cap<Base> base = c.reinterpret_as<Base>();
```

### Comparison to hardware CHERI

| Property              | Hardware CHERI                     | shm_alloc capability            |
|-----------------------|------------------------------------|---------------------------------|
| Enforcement           | CPU microcode; unforgeable in HW   | Software checks on every call   |
| Tag bit storage       | 1 extra bit per memory word (HW)   | `block->allocated` in heap hdr  |
| Forgeability          | Impossible below the kernel        | Defeated by raw `mmap` write    |
| Sub-object bounds     | Full 64-bit base + length          | 32-bit `bounds` (≤ 4 GB obj)    |
| Revocation            | Load-side tag check, zero-cost     | Free clears tag; deref re-reads |
| ABA protection        | Not needed (HW enforced)           | 16-bit epoch in meta word       |
| Cross-host identity   | Single-system                      | `ns_inode` + `cgroup_hash`      |
| Permission narrowing  | Hardware monotonicity              | Software `shm_cap_narrow()`     |

The capability layer provides CHERI-*like* semantics — opaque handles, bounds
enforcement, monotonic narrowing, and automatic liveness checking — at the
cost of being bypassable by a process that holds the underlying `mmap` file
descriptor.  Pairing it with OS-level isolation (IPC namespaces,
`SHM_OPEN_ENFORCE_NS`, cgroup enforcement) achieves meaningful protection for
the disaggregated-memory threat model.

---

## Cache persistence (clflush / clwb / cbo)

By default `shm_alloc` does not issue any cache-flush or write-back
instructions.  On conventional DRAM-backed shared memory, hardware cache
coherency makes explicit flushes unnecessary.  On **persistent / disaggregated
memory** (Intel Optane, CXL PMEM, `/dev/dax`), writes that stay in CPU caches
will not survive a host crash.

`src/shm_persist.h` provides `shm_persist(addr, len)` and `shm_drain()`.
The allocator calls these automatically on every internal write — block headers,
directory entries, and the region header — so metadata is durable without any
change to user code.  **Users remain responsible for flushing their own payload
data** after writing it.

### Selecting a mode at compile time

Pass exactly one `CACHE=` value to `make` (or add the corresponding `-D` flag
directly to your compiler invocation):

```
make CACHE=CLFLUSH       # x86 / x86-64 — CLFLUSH (flush + invalidate)
make CACHE=CLFLUSHOPT    # x86 / x86-64 — CLFLUSHOPT (Broadwell+, non-serialising)
make CACHE=CLWB          # x86 / x86-64 — CLWB write-back (Cannon Lake+)
make CACHE=CBO_FLUSH     # RISC-V Zicbom — cbo.flush (flush + invalidate)
make CACHE=CBO_CLEAN     # RISC-V Zicbom — cbo.clean (write-back ≈ CLWB)
make                     # (default)     — compiler barrier only; no HW ops
```

Selecting more than one mode, or selecting an x86 mode on a RISC-V target (or
vice versa), is a **compile-time error** caught by `shm_persist.h`'s
architecture guards.

### Instruction summary

| Mode | x86 mnemonic | Semantics | Requires SFENCE after? |
|------|-------------|-----------|------------------------|
| `CLFLUSH` | `clflush [mem]` | flush + invalidate cache line | no (serialising) |
| `CLFLUSHOPT` | `clflushopt [mem]` | flush + invalidate (weakly ordered) | **yes** |
| `CLWB` | `clwb [mem]` | write-back, line stays valid (Modified→Shared) | **yes** |

| Mode | RISC-V mnemonic | Semantics | Requires FENCE after? |
|------|----------------|-----------|----------------------|
| `CBO_FLUSH` | `cbo.flush (rs1)` | flush + invalidate cache block | **yes** |
| `CBO_CLEAN` | `cbo.clean (rs1)` | write-back, block stays valid | **yes** |

`shm_drain()` issues the appropriate fence (`SFENCE` on x86,
`fence rw, rw` on RISC-V) after a group of persist calls to enforce ordering.
It is called once at the end of each public API mutation (`shm_alloc`,
`shm_free`, `shm_resize`, `shm_block_set_perms`).

### Cache line size

The default cache line size is 64 bytes.  Override with:

```
make CACHE=CLFLUSH EXTRA_CFLAGS="-DSHM_CACHE_LINE_BYTES=128"
```

### Persist call sites

Every allocator write that must be visible to other hosts is covered:

| Operation | Structs persisted |
|-----------|------------------|
| `shm_region_open` (create) | entire region header + directory + initial block |
| `shm_region_open` (attach) | `host_count` field |
| `shm_region_close` | `host_count` field |
| `shm_alloc` | zero-filled payload, block header, directory entry, `next_id` |
| `shm_free` | block header (via `freelist_insert`), directory entry |
| `shm_resize` | modified block headers, directory entry, `used_bytes` |
| `shm_block_set_perms` | block header, directory entry |

### CPU feature requirements

`CLWB` requires CPUID.07H.EBX[bit 24] = 1 (Cannon Lake / Ice Lake and later).  
`CLFLUSHOPT` requires CPUID.07H.EBX[bit 23] = 1 (Broadwell and later).  
`cbo.flush` and `cbo.clean` require the RISC-V Zicbom extension.

The allocator **does not check CPU features at runtime** when compiled with a
specific cache mode — the assumption is that the user has verified the target
hardware.  An unsupported instruction will raise `SIGILL`.

---

## v0.2 — the "slim" format

### Why

v2 spends 152 bytes of metadata per object (56-byte block header + 96-byte
directory entry) whether that object is 8 bytes or 8 megabytes. At the scale
this allocator targets — disaggregated memory holding huge numbers of small
objects — that stops working: **300M objects representing 2.1 GiB of actual
data need on the order of 40+ GiB of metadata under v2** (`300,000,000 × 152
bytes ≈ 42.5 GiB`, using the real struct sizes above — over 20x the data
itself). A typical `malloc` spends 8–16 bytes per chunk. v0.2 targets that
ballpark for the common case: **16 bytes for an anonymous object, always.**

Full design rationale, every dropped/kept feature and why, and the bug that
testing caught along the way are in [WIP.md](WIP.md).

### What's different, in one table

| | v2 | v0.2 "slim" |
|---|---|---|
| Per-object fixed overhead | 152 B (56 B block header + 96 B directory entry) | 16 B (block header only) |
| Object directory | Fixed array, one 96 B slot per object, sized to the max object count | Gone. Optional name table sized to how many objects you *name*, not how many you allocate (default 64 slots × 32 B = 2 KiB total, independent of object count) |
| Object identity | Monotonic `uint64_t` ID *and* an offset handle | Offset handle only (the ID was always redundant with it) |
| Access control | Owner `uint32_t` + 5-bit permission mask + admin bypass (2-party check, 5 steps) | One `uint32_t` key; match or don't (1 step) |
| Type checking | `type_tag` + `dynamic_cast`-style capability layer | Not ported — compile-time-only `sizeof` macros |
| Physical vs. logical size | Both tracked separately per object | Capacity only, like `malloc_usable_size` |
| Free-list linkage | Explicit `next_free` field in every block header | Overlaid on a free block's own (otherwise unused) payload bytes — costs nothing on a live block |
| Namespace / cgroup enforcement | Yes | Yes, unchanged |
| Bounds-checked offset resolution | Yes | Yes, unchanged in spirit |
| CHERI-like capability layer | Yes (`shm_cap.*`) | Not ported (built entirely on the fields removed above) |
| C++ typed wrappers | Yes | Not ported yet |

### Memory layout

```
base[0]              base + name_offset       base + data_offset
  │                        │                         │
  ▼                        ▼                         ▼
┌──────────────────┬───────────────────────────┬────────────────────────┐
│  region header    │  name table (optional,    │  heap (16-byte-header  │
│                   │  fixed small capacity)    │  blocks, implicit list)│
└──────────────────┴───────────────────────────┴────────────────────────┘
```

There is no separate directory zone. The heap itself is the only place
objects live; enumeration walks it directly (see `shm_slim_walk()` below).

### The 16-byte block header

```c
typedef struct shm_slim_hdr {
    uint32_t magic;   /* SHM_SLIM_MAGIC_LIVE / _LIVE_NAMED / _FREE */
    uint32_t key;     /* access-control key; 0 = SHM_SLIM_KEY_OPEN (no check) */
    uint64_t size;    /* total block size (header + payload), 8-byte aligned */
} shm_slim_hdr_t;   /* sizeof == 16, enforced by _Static_assert */
```

- **`magic`** distinguishes live-anonymous, live-named, and free — the same
  corruption guard v2 has, plus it's how `shm_slim_free()`/`shm_slim_resize()`
  know whether to bother touching the (tiny) name table at all: anonymous
  objects, expected to be the overwhelming majority, skip that check entirely.
- **`key`** is the entire access-control model: `SHM_SLIM_KEY_OPEN` (0) means
  unprotected (anyone may access it); any other value must be matched exactly
  by every subsequent `shm_slim_ptr` / `shm_slim_free` / `shm_slim_resize` /
  `shm_slim_lookup_by_name` call. No owner identity, no per-operation
  permission bits, no admin bypass — a bearer token, not RBAC.
- **`size`** is the block's total size including this header; payload
  capacity is always `size - 16`. Forward heap traversal is
  `next_block = this_block + this_block->size` — an implicit list, like a
  boundary-tag malloc, needing no explicit next-pointer for live blocks.
- Reverse lookup (payload offset → header) is always `header = off - 16`,
  unconditionally, whether the object is named or not — naming information
  lives entirely outside the block (in the name table), never as a
  variable-length prefix inside it, specifically so this stays O(1) and
  unconditional.
- A **free** block's free-list link is stored *in its own payload bytes*
  (the first 8 bytes after the header) rather than as a header field — the
  same technique classic `dlmalloc`-style allocators use to overlay `fd`/`bk`
  on free chunks. This is why the minimum block size is 24 bytes (16-byte
  header + 8 bytes of reserved link space), not 16: every block needs room
  for that link once freed, even if its live payload was smaller.

### The name table

```c
typedef struct shm_slim_name_entry {
    char      name[24];  /* name[0] == 0 -> empty slot */
    shm_off_t off;        /* payload offset of the named object */
} shm_slim_name_entry_t;   /* 32 bytes/slot */
```

Sized by `name_capacity` at creation (default 64 slots = 2 KiB). This is for
naming a handful of well-known root/entry-point objects for multi-host
discovery — e.g. the same `"graph_csr"`-style example used elsewhere in this
README — not for naming every object. Anonymous objects cost zero bytes
here. Duplicate names are rejected (`EEXIST`); an object's name-table entry
is removed on free and its offset is updated automatically if a resize
relocates it.

### Incompatibility with v2

The version field is the literal 4-byte ASCII word `"slim"`
(`SHM_SLIM_VERSION = 0x736C696D`), not an incrementing integer. v2 stores an
integer (`2`) and rejects any region whose version isn't that integer; this
format stores `"slim"` and rejects any region whose version isn't that word.
The two encodings can never collide, so a v2 region opened with the slim
code (or vice versa) fails cleanly with `ENOENT` instead of misinterpreting
the layout. The region magic (`"SHMA"`) is intentionally left shared between
the two — it only asserts "this is an shm_alloc-family region," which is
true of both; the version field is what actually gates compatibility.

### C API quick reference

```c
#include "shm_slim.h"

// Create a region (name table defaults to 64 slots if unset).
shm_slim_region_open_opts_t opts = {
    .backend = SHM_SLIM_BACKEND_POSIX,
    .flags   = SHM_SLIM_OPEN_CREATE,
};
shm_slim_region_t *region;
shm_slim_region_open("/my_pool", 1 << 20, &opts, &region);

// Anonymous allocation -- 16 bytes of metadata, no name-table cost.
shm_off_t off;
shm_slim_alloc(region, 64, /*name=*/NULL, /*key=*/0xCAFEu, &off);

// Access requires the same key.
void *p = shm_slim_ptr(region, off, 0xCAFEu);          // succeeds
void *q = shm_slim_ptr(region, off, 0xDEADu);          // NULL: wrong key
// SHM_SLIM_KEY_OPEN (0) means "no protection": any key is accepted.

// Named allocation for discovery/bootstrapping.
shm_off_t root;
shm_slim_alloc(region, sizeof(MyRoot), "graph_root", 0xCAFEu, &root);
shm_slim_lookup_by_name(region, "graph_root", 0xCAFEu, &root);

// Resize may relocate; the (possibly new) offset is always returned
// directly -- there is no per-object directory entry to re-query.
shm_off_t new_off;
shm_slim_resize(region, off, 4096, 0xCAFEu, &new_off);

// Enumerate every live object by walking the heap (no directory to scan).
uint64_t cursor = 0;
shm_slim_entry_t e;
while (shm_slim_walk(region, &cursor, &e) == 0) {
    printf("off=%llu size=%zu name=%s\n", e.off, e.size, e.name);
}

shm_slim_free(region, off, 0xCAFEu);
shm_slim_region_close(region, /*unlink=*/true);
```

### What was deliberately not ported, and why

| Dropped | Why |
|---|---|
| Object directory | The entire point — this was the O(n)-per-object storage cost that didn't scale |
| Monotonic object IDs | Redundant with the offset handle, which was always the real handle passed around |
| Owner ID + permission bitmask + admin bypass | Not on the keep list; replaced by a single access-control key |
| `type_tag` / runtime type checks | Not on the keep list |
| CHERI-like capability layer (`shm_cap.*`) | Built entirely on the owner/perms/obj_id/payload_cap fields being removed; would need its own redesign around the key field |
| C++ typed wrappers | Depended on `type_tag`; could be re-added later as sizeof-only macros |

Namespace/cgroup enforcement and bounds-checked offset resolution were
**kept unchanged in spirit** — both are essential to the project's safety
story and neither one scales per object (namespace/cgroup fingerprints are
O(1) per region; bounds checking is O(1) per call), so neither was part of
the metadata-size problem in the first place.

### Honest limits

- 16 bytes is fixed overhead, not a promise that metadata shrinks below the
  data: at the motivating example's own ratio (~7 bytes of payload per
  object on average), a 16-byte header is still larger than the payload.
  The real claim is a ~9.5x reduction versus v2 and landing within ~2-3x of
  the data instead of ~20x over it — see `tests/test_slim_scale.c` for the
  exact, reproducible numbers (computed from the real `sizeof()` values at
  build time, not hard-coded).
- Named objects still cost a 32-byte name-table slot each. Nothing enforces
  that naming stays rare — that's a usage convention (name a few roots, not
  every object), not an API-level guarantee.
- There is no read/write distinction and no per-operation permission bits —
  a matching key grants full access, same as a bearer token. If you need
  finer-grained access control than "has the key or doesn't," that's a
  deliberate simplification you'd need to build on top (e.g. different keys
  for different sub-objects).

---

## Running v2 code on the slim engine

`shm_slim.h` additionally declares v2's exact public names and signatures —
`shm_region_t`, `shm_region_open_opts_t`, `shm_pool_stats_t`,
`SHM_PERM_*`/`SHM_OPEN_*`, `shm_region_open`/`close`/`base`/`size`,
`shm_alloc`/`shm_free`/`shm_resize`/`shm_ptr`/`shm_pool_stats`, and the
`shm_alloc_typed`/`shm_cast`/`shm_cast_mut`/`shm_free_id` macros — so code
written against v2 can run on the slim engine by **only changing which
header it includes**:

```diff
- #include "shm_alloc.h"
+ #include "shm_slim.h"
```

...and linking `shm_slim.o` + `shm_slim_compat.o` + `shm_ns.o` instead of
`shm_alloc.o` + `shm_ns.o`. No other source change. `test-prog/objstream_alloc.cpp`
is a real, unmodified example of exactly this — see
[`test-prog/`](test-prog/) and [WIP.md](WIP.md) for the full verification,
including that its allocator metadata stays at a constant ~2.2 KB across a
600,000-object run where v2 would need ~91 MB.

This is a **compatibility layer**, not a second full implementation of v2's
feature set — `user_id`+`perms` collapse into the slim engine's one access
key, `type_tag` is accepted but not persisted, `obj_id` is just the offset
(`id == off`), and there is deliberately no compatibility shim for
`shm_lookup(id)` / `shm_dir_next` / `shm_dir_read_slot` / `shm_block_info` /
`shm_block_set_perms` — faithfully replicating those needs the per-object
directory this format exists to eliminate. See the "What was deliberately
not ported" table above and WIP.md for the complete mapping and rationale.

**Never `#include` both `shm_alloc.h` and `shm_slim.h` in the same file**
without first `#define SHM_SLIM_NO_V2_COMPAT` — both declare `shm_alloc()`,
`shm_ptr()`, and friends with the same names, and the compiler will
correctly refuse the conflicting declarations. Defining that macro before
including `shm_slim.h` skips its v2-compatible section entirely, leaving
only the native `shm_slim_*` API (see `tests/test_generic_backend.c` for a
real example that needs exactly this).

---

## File structure

```
include/
  shm_alloc.h        v2 C public API (types, constants, all function declarations)
  shm_alloc.hpp      v2 C++ typed wrappers (shm::ptr<T>, make, cast, find, free)
  shm_cap.h          v2 C capability API (shm_cap_t, derive, deref, narrow, …)
  shm_cap.hpp        v2 C++ typed capability wrapper (shm::cap<T>)
  shm_slim.h         v0.2 "slim" public API, PLUS a v2-compatible API section
                     (see "Running v2 code on the slim engine" above)
src/
  shm_internal.h     v2 internal layout types + inline helpers (shm_block_hdr_t, …)
  shm_persist.h      Cache-line flush / write-back helpers (shared by v2 and slim)
  shm_alloc.c        v2 core allocator (region, heap, directory, resize)
  shm_cap.c          v2 CHERI-like capability layer (derive, deref, validate)
  shm_ns.h           Internal: IPC namespace + cgroup fingerprint decls (shared)
  shm_ns.c           Internal: /proc/self/ns/ipc inode + FNV-1a cgroup hash (shared)
  shm_slim_internal.h v0.2 internal layout (16-byte shm_slim_hdr_t, name table, …)
  shm_slim.c         v0.2 implementation -- self-contained, does not include
                     shm_internal.h or shm_alloc.h
  shm_generic_backend.h  optional jemalloc/tcmalloc/cxlalloc/ralloc backend,
                     shared by shm_alloc.c and shm_slim.c
  shm_slim_compat.c  v2-compatible API bodies on the slim engine -- a
                     SEPARATE translation unit from shm_slim.c; never link
                     this alongside shm_alloc.o (duplicate shm_alloc()/etc.)
tests/
  test_basic.c       v2: open/close, alloc, lookup, permissions, multi-object,
                     parallel slot-partitioned directory reads
  test_resize.c      v2: shrink, in-place grow, relocation grow, permission check
  test_multihost.c   v2: fork-based multi-process discovery, NS enforcement
  test_cap.c         v2: capability derive, deref, tag bit, ABA, narrowing, forgery
  test_bounds.c      v2: bounds-checked offset resolution (EFAULT/EINVAL/NULL)
  test_slim_basic.c  v0.2: alloc/free/resize, naming, key access control, walk, stats
  test_slim_bounds.c v0.2: bad-offset/corrupted-header/corrupted-chain safety
  test_slim_scale.c  v0.2: proves the metadata-overhead claim, empirically and
                     via a projection at the motivating 300M-object scale
  test_generic_backend.c  both: SHM_BACKEND_GENERIC / SHM_SLIM_BACKEND_GENERIC,
                     with and without a backend selected
  test_slim_v2compat.c  v2-compatible API section, running on the slim engine
Makefile
WIP.md               v0.2 design rationale, decisions, and status log
test-prog/           STREAM-style benchmarks; objstream_alloc.cpp is a real
                     example of v2-style code running on the slim engine
                     via the v2-compatible API (see WIP.md)
```
