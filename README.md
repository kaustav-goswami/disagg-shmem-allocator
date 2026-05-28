# shm_alloc — Shared / Disaggregated Memory Allocator

A C11 / C++17 allocator for **POSIX shared memory** and **DAX / CXL disaggregated memory**, with:

- Self-describing region header with an **object directory** for multi-host discovery
- Monotonic **object IDs** — no hardcoded offsets
- **Dynamic resize** (grow in-place or with relocation; shrink)
- **User-id ownership** and per-object **permission bits**
- **Namespace / cgroup protection** against cross-boundary access
- C++ typed wrappers with placement new, `static_cast`, and `dynamic_cast`

Parts of this program is vibe coded.

---

## Memory layout

```
base[0]              dir_offset               data_offset
  │                      │                         │
  ▼                      ▼                         ▼
┌──────────────────┬──────────────────────────┬────────────────────────┐
│  region header   │   object directory       │   heap (blocks…)       │
└──────────────────┴──────────────────────────┴────────────────────────┘
```

`dir_offset` and `data_offset` are stored in the region header, so the layout
is **self-describing**. A new host only needs to open the same name / device
and read those two fields.

---

## Backends

| Backend | How to select | Notes |
|---------|--------------|-------|
| `SHM_BACKEND_POSIX` | default | `shm_open("/name")` + `mmap(MAP_SHARED)` |
| `SHM_BACKEND_DAX`   | `opts.backend = SHM_BACKEND_DAX` | `open("/dev/dax0.0")` + `mmap(MAP_SHARED)`, no `ftruncate` |

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

## File structure

```
include/
  shm_alloc.h      C public API (types, constants, all function declarations)
  shm_alloc.hpp    C++ typed wrappers (shm::ptr<T>, make, cast, find, free)
src/
  shm_alloc.c      Core allocator (region, heap, directory, resize)
  shm_ns.h         Internal: IPC namespace + cgroup fingerprint declarations
  shm_ns.c         Internal: /proc/self/ns/ipc inode + FNV-1a cgroup hash
tests/
  test_basic.c     open/close, alloc, lookup, permissions, multi-object
  test_resize.c    shrink, in-place grow, relocation grow, permission check
  test_multihost.c fork-based multi-process discovery, NS enforcement
Makefile
```
