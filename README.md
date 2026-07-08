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

## File structure

```
include/
  shm_alloc.h      C public API (types, constants, all function declarations)
  shm_alloc.hpp    C++ typed wrappers (shm::ptr<T>, make, cast, find, free)
  shm_cap.h        C capability API (shm_cap_t, derive, deref, narrow, …)
  shm_cap.hpp      C++ typed capability wrapper (shm::cap<T>)
src/
  shm_internal.h   Internal layout types + inline helpers (shm_block_hdr_t, …)
  shm_persist.h    Cache-line flush / write-back helpers (CLFLUSH/CLWB/CBO/none)
  shm_alloc.c      Core allocator (region, heap, directory, resize)
  shm_cap.c        CHERI-like capability layer (derive, deref, validate)
  shm_ns.h         Internal: IPC namespace + cgroup fingerprint declarations
  shm_ns.c         Internal: /proc/self/ns/ipc inode + FNV-1a cgroup hash
tests/
  test_basic.c     open/close, alloc, lookup, permissions, multi-object
  test_resize.c    shrink, in-place grow, relocation grow, permission check
  test_multihost.c fork-based multi-process discovery, NS enforcement
  test_cap.c       capability derive, deref, tag bit, ABA, narrowing, forgery
Makefile
```
