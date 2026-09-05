# WIP: v0.2 "slim" metadata redesign

Status: **done**. `make clean && make test` is green (8/8 binaries, v2 and v0.2
together, `-Wall -Wextra`, no warnings). This file is a record of what was
built and why, not just a plan.

## Why

The existing allocator (call it **v2**, the format already in `src/shm_alloc.c` /
`include/shm_alloc.h`) spends **152 bytes of metadata per object**:

- 56-byte heap block header (`shm_block_hdr_t`)
- 96-byte object directory entry (`shm_obj_entry_t`)

At the scale this allocator is meant for — disaggregated memory holding huge
numbers of small objects — this does not scale. The user's motivating
example: **300M objects representing 2.1 GiB of actual data** needing on the
order of **tens of GiB of metadata** under v2 (quoted as ~70 GiB). Re-deriving
this from the actual struct sizes compiled into v2 (`56 + 96 = 152` bytes of
fixed header+directory per object, before any alignment rounding) gives
`300,000,000 × 152 B ≈ 42.5 GiB` — the same order of magnitude and the same
conclusion (metadata over an order of magnitude larger than the data), even
though it doesn't hit exactly 70 GiB; the gap is likely additional alignment/
fragmentation overhead not modeled here. `tests/test_slim_scale.c` computes
this projection from the real `sizeof()` values at build time rather than
hard-coding either number, so the test tracks whichever numbers are actually
true of the code, and asserts the two claims that matter regardless of the
exact figure: v2's fixed cost is tens of GiB at this scale, and it is more
than an order of magnitude larger than the data.

A typical `malloc` implementation spends **8–16 bytes** of header per chunk
(a size field, sometimes packed with flag bits, with free-list pointers
overlaid on the payload of free chunks rather than kept in the header). v0.2
targets that ballpark for the common case.

## Requirements (from the user)

1. New version, named **v0.2**. The on-disk version field must literally be
   the 4-byte ASCII word `"slim"`, not an incrementing integer. This must be
   mutually incompatible with v2 — opening a v2 region with the slim code
   (or vice versa) must fail cleanly, never silently misinterpret bytes.
2. Metadata must get slim. Drop most existing features. Keep only:
   - name strings (for discovery / bootstrapping)
   - start + size (the offset/size pair describing an object)
   - a fixed 4-or-8-byte access-control **key** slot, replacing the
     owner-id + permission-bitmask ("sharer info") model
   - namespace / cgroup enforcement (already O(1) per region, not per
     object — keep as-is)
   - anything else that's absolutely essential (corruption detection,
     concurrency correctness, bounds-checked dereference safety)
3. Build and test everything.

## Scope decision: new module, not an in-place rewrite

Implemented as a **new, self-contained module** — `include/shm_slim.h`,
`src/shm_slim_internal.h`, `src/shm_slim.c` — rather than rewriting
`shm_alloc.c` in place. Reasons:

- The existing v2 allocator, its CHERI-like capability layer (`shm_cap.*`),
  and its C++ wrappers are working, tested code built around the very
  owner+permission+directory model this task removes. Gutting them in place
  would be a destructive, hard-to-reverse action with no explicit
  instruction to do so.
- "New version" + "must not be compatible with the old version" reads
  naturally as: a new, distinct format existing alongside the old one, not a
  replacement. Nothing in the request said to delete v2.
- Keeping both makes the metadata-size claim independently checkable: the
  same allocator concepts, in the same repo, at two different overhead
  points.

If the intent was actually to retire v2 entirely, that's a follow-up
decision for the user to make once v0.2 exists and can be compared against
it — not something to guess into an irreversible deletion.

## On-disk format design

### Per-object heap block header — 16 bytes, always

```c
typedef struct shm_slim_hdr {
    uint32_t magic;   /* SHM_SLIM_MAGIC_LIVE / _LIVE_NAMED / _FREE */
    uint32_t key;     /* access-control key; 0 (SHM_SLIM_KEY_OPEN) = unprotected */
    uint64_t size;    /* total block size (header + payload), 8-byte aligned */
} shm_slim_hdr_t;   /* sizeof == 16, enforced by _Static_assert */
```

This directly replaces v2's 56-byte `shm_block_hdr_t` (magic, type_tag,
owner, perms, allocated, obj_id, payload_cap, total_size, next_free) with 3
fields:

- **No `obj_id`** — monotonic per-object IDs are dropped. The `off` handle
  returned by `shm_slim_alloc()` already uniquely and directly identifies
  the object (as it always did in v2, where the "id" was a redundant,
  purely-for-directory-lookup concept). Multi-host sharing works by passing
  the `off` out of band, or via the name table below.
- **No `owner` / `perms`** — replaced by the single `key` field. Access is
  granted iff `key == SHM_SLIM_KEY_OPEN` (0, meaning "unprotected") or the
  caller presents a `key` matching what was stored at `shm_slim_alloc()`
  time. This is a bearer-token model, not RBAC: no owner identity, no
  separate read/write/free/resize bits, no admin bypass. One check replaces
  five.
- **No `type_tag`** — runtime type checking (used by v2's `dynamic_cast`-like
  capability layer) is dropped along with that layer.
- **No `payload_cap` and `total_size` as two fields** — collapsed into one
  `size` field (total block size incl. header); payload capacity is always
  `size - sizeof(shm_slim_hdr_t)`. There is no separate "logical used size"
  distinct from "physical capacity" (same simplification real mallocs make:
  `malloc_usable_size` only ever reports capacity).
- **No `next_free` field** — free-list linkage for a free block is stored
  **in-band inside the free block's own payload area** (the first 8 bytes
  right after the header), exactly like classic `dlmalloc`-style allocators
  overlay `fd`/`bk` pointers on free chunks. A live block pays nothing for
  this; a free block's own otherwise-unused bytes pay for it. This is why
  the minimum block size is `sizeof(hdr) + 8 = 24 bytes`, not 16 — every
  block must have room for the link once freed.
- **Two magic values distinguish anonymous vs. named live blocks**
  (`SHM_SLIM_MAGIC_LIVE` / `SHM_SLIM_MAGIC_LIVE_NAMED`) so `shm_slim_free()`
  and `shm_slim_resize()` can skip the (tiny, but non-zero) name-table
  lookup entirely for the overwhelmingly common anonymous case, at zero
  extra bytes of storage.

Forward traversal of the heap (for enumeration / multi-host discovery) walks
block-to-block via `next_block = this_block + this_block->size` — an
**implicit list**, exactly like boundary-tag mallocs, needing no explicit
next-pointer for live blocks.

Reverse lookup (payload offset → header) is always `header = off -
sizeof(shm_slim_hdr_t)`, unconditionally, whether the block is named or not
— this only works because naming information lives *outside* the block (see
below), not as a variable-length prefix inside it. An earlier design that
tried to put an optional inline name section between the header and the
payload was rejected specifically because it breaks this O(1) invariant
(the reverse offset would depend on information — "is this named?" — that
lives inside the very header you're trying to locate).

### Name table — small, fixed-capacity, separate from the heap

```c
typedef struct shm_slim_name_entry {
    char      name[SHM_SLIM_NAME_MAX];  /* 24 B; name[0] == 0 → empty slot */
    shm_off_t off;                      /* payload offset of the named object */
} shm_slim_name_entry_t;   /* 32 bytes/slot */
```

Sits in its own small region right after the region header, sized by
`name_capacity` at creation time (default 64 slots = 2 KiB total). This is
the *only* place a "directory" still exists, and it is **not sized to the
total object count** — it is sized to how many objects the application
actually wants to name for bootstrapping/discovery (e.g. a handful of root
objects), which is the realistic usage pattern already visible in the v2
README examples (one named `"graph_csr"` root pointing at a large anonymous
blob, not 300M individually-named leaves). Anonymous objects — expected to
be the vast majority at scale — cost zero bytes here.

`shm_slim_free()` / `shm_slim_resize()` on a named object additionally
touch this table (remove-on-free, update-offset-on-relocation) — an O(name
table capacity) scan, not O(total object count), and skipped entirely for
anonymous objects via the magic-value check above.

### Region header

Drops `dir_capacity` and `next_id` (no directory, no monotonic IDs). Keeps
everything that is genuinely O(1) per region rather than O(n) per object:
the process-shared robust mutex, `region_size`/`data_offset`/`name_offset`,
free-list head, `used_bytes`/`block_count` bookkeeping, `host_count`, and —
per the explicit requirement — `ns_inode` / `cgroup_hash` for namespace/
cgroup enforcement, unchanged from v2.

`version` is `SHM_SLIM_VERSION = 0x736C696Du` — the literal ASCII bytes
`"slim"` (same big-endian-reading convention the codebase already uses for
`"SHMA"`/`"BLK!"`), not the integer `2` that v2 uses. `magic` is left as the
same `"SHMA"` family identifier v2 uses (it means "this is an shm_alloc
region," a true statement for both formats) — the **version field alone**
is what makes the two mutually unreadable: v2's open path already rejects
any region whose stored version isn't its own integer constant, and the
slim open path rejects any region whose stored version isn't `"slim"`. Two
different encodings (`2` vs. `0x736c696d`) can never collide by accident.

### What got dropped, explicitly

| v2 feature | v0.2 status | Why |
|---|---|---|
| Object directory (fixed array, 96 B/slot) | **Dropped** — replaced by the tiny optional name table + heap walk | The whole point: this was the O(n) cost that didn't scale |
| Monotonic object IDs | **Dropped** | Redundant with the offset handle, which was always the real handle |
| Owner user-id + 5-bit permission mask + ADMIN bypass | **Dropped** — replaced by one `key` field | Not on the keep-list; a single equality check is the "slim" analog |
| `type_tag` / runtime type checking | **Dropped** | Not on the keep-list; typed access becomes a compile-time-only convenience macro |
| Logical `used_size` distinct from physical capacity | **Dropped** — only capacity is tracked | Matches how real mallocs behave; app-level bookkeeping if needed |
| CHERI-like capability layer (`shm_cap.*`) | **Not ported** | Built entirely on the owner/perms/obj_id/payload_cap fields being removed; would need its own redesign around the key field |
| C++ typed wrappers (`shm::ptr<T>`, `dynamic_cast`) | **Not ported (yet)** | Depended on `type_tag`; a C++ header could be added later as thin sizeof-only macros |
| Name strings | **Kept**, but moved out of the per-object heap block into a small side table |
| Start/size (offset + size) | **Kept** — this is exactly what `shm_slim_hdr_t.size` + the returned offset represent |
| Access control | **Kept**, redesigned as a single key instead of owner+perms |
| Namespace / cgroup enforcement | **Kept unchanged** — O(1) per region |
| Bounds-checked offset resolution (the fix from the previous session) | **Kept / re-implemented** — dropping it would reintroduce the exact out-of-bounds-dereference risk that was just fixed in v2 |

## Measured results (`build/test_slim_scale`)

```
200000 objects, 8-byte payload each: 24.0 bytes/object total (16 of that header)
used_bytes == N * (header + payload) exactly -- no hidden per-object cost

At 300,000,000 objects (~2.10 GiB data):
  v2   metadata (56+96 B/obj): ~42.47 GiB (20.2x the data)
  slim metadata (16 B/obj):    ~4.47 GiB  (2.1x the data)
  reduction: 9.5x
```

Two honest caveats worth stating plainly:

1. **16 bytes is per-object fixed overhead, not a guarantee that metadata
   shrinks below the data.** At the user's own example ratio (~7 bytes of
   payload per object on average), a 16-byte header is still larger than
   the payload it describes. The claim is a ~9.5x reduction versus v2, and
   landing within ~2x of the data instead of ~20x over it -- not that
   metadata becomes negligible. For larger average object sizes the header
   becomes proportionally smaller, same as with any malloc.
2. Named objects still cost a 32-byte name-table slot each, on top of the
   16-byte block header. The whole design assumes naming is used sparingly
   (bootstrapping/discovery), not for every object; nothing enforces that
   assumption at the API level.

## Status log

- [x] Design finalized
- [x] `include/shm_slim.h`, `src/shm_slim_internal.h`, `src/shm_slim.c`
- [x] Test suite: `test_slim_basic.c` (36 checks), `test_slim_bounds.c` (14
      checks, mirrors `test_bounds.c`'s v2 coverage), `test_slim_scale.c`
      (empirical + projected metadata-overhead proof)
- [x] Makefile wiring (`shm_slim.o`, 3 new test binaries, `clean` targets)
- [x] `make clean && make test` green: 8/8 binaries, `-Wall -Wextra`, zero
      warnings, zero stale `/dev/shm` segments after a run
- [x] README.md v0.2 section

## Bug caught during testing

The first draft of `shm_slim_walk()` read a candidate block's 16-byte header
(`magic`, `size`) before checking that all 16 bytes were actually inside the
mapped heap -- if the implicit block chain were corrupted such that the
cursor landed within a few bytes of the heap's end, that read could reach
past the end of the `mmap` region. Fixed by checking
`*cursor + sizeof(shm_slim_hdr_t) > heap_sz` before dereferencing anything,
mirroring the same bounds-first discipline as `shm_slim_blk_of_poff_checked`.
Covered by `test_walk_rejects_corrupted_chain` in `test_slim_bounds.c`.

## Files added

```
include/shm_slim.h            public API (region lifecycle, alloc/free/resize,
                               ptr, lookup_by_name, walk, pool_stats)
src/shm_slim_internal.h       16-byte block header, region header, name-table
                               entry, bounds-checked offset resolution
src/shm_slim.c                implementation
tests/test_slim_basic.c       alloc/free/resize/naming/keys/walk/stats
tests/test_slim_bounds.c      bad-offset, corrupted-header, corrupted-chain,
                               key-mismatch safety (mirrors test_bounds.c)
tests/test_slim_scale.c       proves the metadata-overhead claim, empirically
                               and via a projection at the motivating scale
```

Nothing under `src/shm_alloc.c`, `include/shm_alloc.h`, `src/shm_cap.*`, or
the existing tests was touched -- v2 is unchanged and still fully tested.

---

# Addendum: generic allocator backend

Status: **done**. `jemalloc` and `cxlalloc` are fully wired, built, and
tested end to end (real alloc/write/read/free through each). `tcmalloc` and
`ralloc` are wired with the correct real symbol names but are not buildable
in this environment; selecting them fails the build with an explanatory
message rather than a cryptic linker error.

## The ask

By default the region's backing memory comes from `shm_open()+mmap()`
(POSIX) or `open()+mmap()` on a DAX device. Add a compile-time option to
source it from a vendored general-purpose or CXL-aware allocator instead
(jemalloc, tcmalloc, cxlalloc, ralloc -- all already present as submodules in
this repo), without modifying any of those repos.

## Design

A third backend value, `SHM_BACKEND_GENERIC` (v2) / `SHM_SLIM_BACKEND_GENERIC`
(slim), plus one new shared header, `src/shm_generic_backend.h`, providing
`shm_generic_backend_alloc(size)` / `shm_generic_backend_free(ptr, size)`.
Which library it calls is chosen at compile time by defining exactly one of
`SHM_GENERIC_JEMALLOC` / `_TCMALLOC` / `_CXLALLOC` / `_RALLOC` (enforced with
a `#if ... > 1 #error`, the same pattern `shm_persist.h` already uses for
`CACHE=`). `make GENERIC_ALLOC=<name>` sets the macro and the right
include/link flags. Both `shm_alloc.c` and `shm_slim.c` got the identical
three-line change: in `region_open`, branch to
`shm_generic_backend_alloc(size)` instead of the fd+mmap dance when this
backend is selected; in `region_close`, call `shm_generic_backend_free`
instead of `munmap`. `region_init` and everything above it (region header,
heap, mutex, name table / directory) is completely backend-agnostic already
and needed zero changes.

**Honest limitation, not an oversight**: a generic-backend region is memory
obtained from a library call in one process's address space, not a named OS
object a second process can independently attach to. `shm_region_open()` /
`shm_slim_region_open()` with the create flag unset on a generic backend
returns `ENOTSUP`. Lifting this would mean using cxlalloc's own
`heap_backend="shm"`/`"ivshmem"` modes (it explicitly supports correlating
heaps across processes via a shared `heap_id`) instead of the `"mmap"` mode
used here -- a real follow-up, not attempted in this pass.

## What it took to actually build the vendored libraries (without changing them)

- **jemalloc**: `./autogen.sh && make -j$(nproc)` -- worked immediately, no
  network, no missing pieces. The default (unprefixed) build makes it a
  drop-in `malloc`/`free` replacement rather than exposing `je_malloc`/
  `je_free` (confirmed with `nm libjemalloc.a`); the shim calls plain
  `malloc`/`free` and relies on `libjemalloc.a` being linked ahead of libc.
- **cxlalloc**: `cargo build --release -p cxlalloc-static`. Two real
  obstacles, neither a source change:
  1. The system `cargo`/`rustc` in this environment is 1.75.0; one cxlalloc
     workspace member needs `edition2024`, stabilized in Cargo 1.85+.
     `cxlalloc/rust-toolchain.toml` already pins 1.88.0, which rustup had
     installed -- but `/usr/bin/cargo` is a real standalone binary that
     wins on PATH ahead of rustup's shims, so `rust-toolchain.toml`
     auto-detection never kicked in via a plain `cargo build`, and even
     `rustup run 1.88.0 cargo build` didn't fully work (it still resolved
     `rustc` via PATH inside cargo's own subprocess spawns). What worked:
     invoking the 1.88.0 toolchain's `cargo` **and** `rustc` by their exact
     `rustup which ... --toolchain 1.88.0` paths, with that `rustc` path
     also exported as `RUSTC`. The Makefile rule does this automatically.
  2. `cxlalloc-recover` (an unrelated workspace member, not needed for
     `cxlalloc-static`) path-depends on `cxlalloc/extern/memento`, a git
     submodule that was never initialized in this checkout. Cargo has to
     load every workspace member's manifest to resolve any one of them, so
     the whole build failed until that one submodule was populated with
     `git submodule update --init extern/memento` -- fetching what the
     repo's own submodule pointer already specifies, not a source change.
     The Makefile rule does this automatically too, once, if needed.
  cxlalloc's own C API (`cxlalloc/cxlalloc-static/include/cxlalloc.h`,
  generated via `cbindgen`) was already exactly what was needed:
  `cxlalloc_init_process`, `cxlalloc_malloc`, `cxlalloc_free`.
- **tcmalloc**: this vendored copy ships `MODULE.bazel` as the real build
  (Bazel is not installed here) and a `CMakeLists.txt` that `FetchContent`s
  abseil/protobuf/googletest/RE2 from GitHub at configure time -- a large,
  slow build regardless. The real exported symbol names
  (`TCMallocInternalMalloc`/`TCMallocInternalFree`, from
  `tcmalloc/tcmalloc/tcmalloc.h` -- this build overrides the global
  allocator via a linker section trick rather than exposing a `tc_malloc`-
  style name) are wired into the shim; building the library itself is left
  to the user.
- **ralloc**: `#![no_std]`, and depends on nightly-only Rust features
  (`optin_builtin_traits`, feature-gated `const_fn`, `type_ascription`, ...)
  that were renamed or removed from Rust years ago. Neither the 1.75.0 nor
  1.88.0 toolchain available here -- nor any reasonably current one -- can
  compile it. It also exposes no `extern "C"` API of its own (only internal
  Rust functions), so using it would additionally require writing a small
  wrapper crate; not attempted, since the underlying crate can't build
  regardless.

## Known risk not fully resolved: cxlalloc double-init

`shm_generic_backend.h`'s cxlalloc branch keeps its `pthread_once_t` guard
as a `static` file-scope variable, so `shm_alloc.c` and `shm_slim.c` each
get their **own independent copy** when both are compiled with
`SHM_GENERIC_CXLALLOC` into the same program (as `tests/test_generic_backend.c`
does). In principle this means `cxlalloc_init_process()` gets called twice
in one process. Empirically, `test_generic_backend` passes end to end doing
exactly that (v2 then slim, sequentially, in the same run) -- but this was
only exercised sequentially, single-threaded, in one test. It has not been
stress-tested concurrently, and the fix (moving the guard into a real
translation unit shared by both, or scoping cxlalloc's heap_id/thread_id
per-module) was not done. If you use the generic backend for both v2 and
slim in the same real process, watch for this.

## Files added / changed

```
src/shm_generic_backend.h     new -- the pluggable backend shim
include/shm_alloc.h           + SHM_BACKEND_GENERIC enum value
src/shm_alloc.c               + generic-backend branch in region_open/close
include/shm_slim.h            + SHM_SLIM_BACKEND_GENERIC enum value
src/shm_slim.c                + generic-backend branch in region_open/close
tests/test_generic_backend.c  new -- proves ENOMEM-clean-failure with no
                               backend selected, and real alloc/write/read/
                               free + ENOTSUP-on-attach with one selected
Makefile                      + GENERIC_ALLOC= selector, build rules for
                               jemalloc/cxlalloc, clear-failure targets for
                               tcmalloc/ralloc
```

Vendored submodules (`jemalloc/`, `tcmalloc/`, `cxlalloc/`, `ralloc/`)
were not modified. `cxlalloc/extern/memento` was populated via
`git submodule update --init` (fetching vendored content the repo's own
`.gitmodules` already specifies) -- this and the jemalloc/cxlalloc build
outputs (`jemalloc/lib/`, `cxlalloc/target/`) are build artifacts, not
tracked source changes.

---

# Addendum: v2-compatible API on the slim engine

Status: **done**. `include/shm_slim.h` now additionally declares the exact
names/signatures from `include/shm_alloc.h`; `src/shm_slim_compat.c`
implements them on top of the slim engine. Verified against a real,
pre-existing third-party-style program (`test-prog/objstream_alloc.cpp`),
not just a purpose-built unit test.

## The ask

"Make sure the APIs are the same in the slim and the original version. The
user will simply load a different header file." Verify with
`test-prog/`'s `objstream_alloc` (which already `#include`s `shm_slim.h`
while calling `shm_region_t`/`shm_alloc()`/`shm_ptr()`/etc. with v2's exact
signatures) -- it must build and run successfully.

## What was actually broken before this

Two independent things needed fixing, not one:

1. `include/shm_slim.h` had no v2-named declarations at all -- only its own
   native `shm_slim_*` API -- so `objstream_alloc.cpp`'s calls to bare
   `shm_alloc()`, `shm_ptr()`, `shm_region_t`, `SHM_PERM_DEFAULT`, etc.
   would not even compile.
2. `test-prog/Makefile` linked `objstream-alloc` against **`shm_alloc.o`**
   (v2's real engine) regardless -- so even if the header had compiled by
   accident, the program would have been running on v2 while claiming
   (via its `#include`) to run on slim. Both needed to agree.

## Design: compatibility layer, not a second full implementation

Added a clearly-delineated block at the bottom of `shm_slim.h`
(`shm_region_t`, `shm_region_open_opts_t`, `shm_pool_stats_t`,
`shm_backend_t`, `SHM_PERM_*`/`SHM_OPEN_*`, and
`shm_region_open`/`close`/`base`/`size`/`shm_alloc`/`shm_free`/
`shm_resize`/`shm_ptr`/`shm_pool_stats` plus the `shm_alloc_typed`/
`shm_cast`/`shm_cast_mut`/`shm_free_id` macros) with bodies in a **new,
separate** `src/shm_slim_compat.c` that calls into the native `shm_slim_*`
functions. Kept deliberately out of `src/shm_slim.c` and out of
`LIB_OBJS_SLIM`/`LIB_OBJS_ALL` in the main Makefile, for one hard reason:

**`shm_alloc.o` and `shm_slim_compat.o` must never be linked into the same
binary.** Both define a function literally named `shm_alloc` (and `shm_ptr`,
`shm_free`, `shm_resize`, `shm_pool_stats`, `shm_region_open/close/base/
size`) -- linking both is a duplicate-symbol error, not a subtle bug. This
was caught immediately by the build: `tests/test_generic_backend.c` (from
the previous change) already includes both `shm_alloc.h` and `shm_slim.h`
in one file to exercise both engines side by side, and adding the v2-compat
declarations to `shm_slim.h` broke it instantly with "conflicting types for
`shm_alloc`" et al. Fixed by adding an opt-out guard,
`SHM_SLIM_NO_V2_COMPAT`, defaulting to *compat included* (so the common
case -- one header, no changes needed -- stays true) with
`test_generic_backend.c` as the one place in this repo that defines the
macro to opt back out.

## The honest mapping (and what does NOT carry over)

The slim engine has no owner/permission bitmask, no monotonic IDs, no
type tags, and no per-object directory -- v2's richest features are exactly
what it exists to not pay for. The compat layer maps what it reasonably
can and is explicit about what it can't:

| v2 concept | Compat-layer behavior |
|---|---|
| `user_id` + `perms` | Collapse into the slim engine's one `key` (`key := user_id`); `perms` is accepted for signature compatibility but not enforced bit-by-bit |
| `type_tag` | Accepted, not persisted |
| `obj_id` | Synthesized as `(uint64_t)off` -- i.e. `shm_free`/`shm_resize`'s `obj_id` parameter is expected to be the offset `shm_alloc` returned, not a true independent identity |
| `dir_capacity` | Accepted, ignored -- the slim name table is sized to how many objects are actually *named*, never to the total object count |
| `shm_resize()` growth requiring relocation | Returns `ENOTSUP` rather than silently returning a stale id, since there is no id-to-offset directory here to keep it transparent the way v2's does |
| `shm_lookup(id)`, `shm_dir_next`, `shm_dir_read_slot`, `shm_block_info`, `shm_block_set_perms` | **Not provided.** Faithfully replicating these needs a per-object id/type_tag/capacity/used_size directory -- reintroducing it would undo the entire redesign. Use the native `shm_slim_walk()` / `shm_slim_lookup_by_name()` / `shm_slim_block_size()`, or stay on v2, for discovery/introspection needs. |

This is why the ask was scoped to `objstream_alloc` (which only uses
`shm_region_open`, `shm_alloc`, `shm_ptr`, `shm_pool_stats`,
`shm_region_size`/`base`/`close` -- all covered) and not `objstream_run.cpp`
(which needs `shm_lookup_by_name` + `shm_dir_read_slot` returning
`type_tag`/`id` to dispatch A/B/C scalar groups -- not reasonably
achievable without the directory this redesign removed).
`objstream_run.cpp` correctly still includes `shm_alloc.h` and is
unaffected by any of this.

## Verification: test-prog/objstream_alloc, not just a unit test

Fixed `test-prog/Makefile` to compile+link `objstream-alloc` against
`shm_slim.o` + `shm_slim_compat.o` + `shm_ns.o` instead of `shm_alloc.o` +
`shm_ns.o` (matching what `objstream_alloc.cpp`'s `#include "shm_slim.h"`
actually claims), with zero changes to `objstream_alloc.cpp` itself.
`stream_alloc.cpp`/`stream_run.cpp`/`objstream_run.cpp` (all three already
correctly `#include "shm_alloc.h"`) were untouched.

```
$ ./objstream-alloc.hw 1000
Shared-memory region: /objstream_benchmark
...
Total scalar payload size: 24000 bytes (0.000022 GiB)
Allocator metadata section size: 2240 bytes (0.002136 MiB)
Directory capacity: 3001
Objects initialized; run objstream-run.hw to attach.
$ echo $?
0
```

Also re-ran at 200,000 elements (600,001 objects total): the allocator
metadata section size stayed exactly **2240 bytes** regardless (only the
one named `"objstream_metadata"` object touches the name table; the other
600,000 are anonymous) -- versus the ~91.2 MB v2's 152-bytes/object would
have needed for that many objects. This is a sharper, real-program
demonstration of the same claim `tests/test_slim_scale.c` makes
analytically.

Also confirmed `make all` in `test-prog/` still builds every other binary
(`stream`, `stream-alloc`, `stream-run`, `objstream-run`) unaffected, and
the full main-repo `make clean && make test` (13 binaries now) stays green
with zero warnings after adding the compat layer.

## Files added / changed

```
include/shm_slim.h        + v2-compatible API section (guarded by the new
                           opt-out macro SHM_SLIM_NO_V2_COMPAT)
src/shm_slim_compat.c     new -- v2-compatible API bodies on the slim engine
tests/test_slim_v2compat.c new -- regression test for the compat layer,
                           links shm_slim.o+shm_slim_compat.o+shm_ns.o
tests/test_generic_backend.c + #define SHM_SLIM_NO_V2_COMPAT (needed once
                           shm_slim.h could otherwise collide with shm_alloc.h
                           in this one file, which includes both)
Makefile                  + shm_slim_compat.o rule, LIB_OBJS_SLIM_V2COMPAT,
                           test_slim_v2compat wiring
test-prog/Makefile        objstream-alloc now links shm_slim.o +
                           shm_slim_compat.o + shm_ns.o instead of
                           shm_alloc.o + shm_ns.o
```

`src/shm_slim.c` and `include/shm_alloc.h`/`src/shm_alloc.c` were not
touched.

## Follow-up bug: objstream_alloc's own region-size math was still v2-era

Running `objstream-alloc.hw 10000000` produced a `/dev/shm/objstream_benchmark`
of **7.4 GiB** despite the allocator reporting only 2,240 bytes of metadata.
Root cause: `checked_layout()` in `objstream_alloc.cpp` sized the requested
region using budget constants written for the old, directory-based v2
engine --  `kDirectoryEntryBudget = 128` applied to *every* object (a
"directory slot" that no longer exists at all under slim) plus
`kHeapBlockHeaderBudget = 128` per object (8x the slim engine's real 16-byte
header). For 30,000,001 objects that alone accounts for `30,000,001 × 128 ≈
3.84 GB` plus `30,000,000 × 136 ≈ 4.08 GB` -- summing to exactly the
7,920,008,512 bytes printed. Nobody updated this sizing formula when
`objstream_alloc.cpp` was switched from the v2 engine onto the slim engine.

This isn't just a cosmetic `ls -la` number, either: `shm_slim_region_init()`
does a single `memset(base, 0, total_size)` at creation time
(`src/shm_slim.c`), so the full requested size is genuinely touched and
resident in RAM (tmpfs), not sparse. An oversized request has a real memory
cost.

**Fix** (`test-prog/objstream_alloc.cpp` only -- no changes to `src/` or
`include/`): replaced the per-object directory-entry budget with a
one-time, fixed name-table-entry budget (`kNameTableEntryBudget = 64`,
applied once, for the single named `"objstream_metadata"` object -- the
3N scalar objects are all anonymous and cost the name table nothing), and
shrank `kHeapBlockHeaderBudget` from 128 to 32 (2x the real 16-byte header,
still comfortable slack). Verified at `element_count = 10,000,000`:

| | Before | After |
|---|---|---|
| Requested region size | 7,920,008,512 B (7.92 GB) | 1,200,008,352 B (1.2 GB) |
| Allocation time | 4.63 s | 1.61 s |
| Allocator metadata section | 2,240 B (unchanged) | 2,240 B (unchanged) |
| Exit code | 0 | 0 |

The allocation-time improvement (not just the file-size improvement) is a
direct consequence of the same `memset` touching ~6.6x fewer pages.
`test-prog`'s full `make all` and the main repo's `make clean && make test`
both stay green after this change.

## Follow-up bug: the metadata-size *stat itself* didn't scale with object count

After the region-sizing fix above, `objstream-alloc.hw 100000000` still
printed a constant `Allocator metadata section size: 2240 bytes` regardless
of object count -- flagged by the user as obviously wrong, correctly.

Root cause: the stat was computed as
`shm_region_size(region) - pool_stats.total_bytes`. That difference is only
the **fixed prefix** before the heap starts (region header + name table
under slim). It happened to be the right formula for v2, where the entire
per-object directory sits in that same pre-heap zone and scales with object
count -- but under slim, each object's 16-byte header lives *inside* the
heap, interleaved with its payload, so `pool_stats.total_bytes` (and
`used_bytes`) already include it, and the subtraction against
`region_size` throws it away. The stat was silently measuring "fixed
per-region overhead" and mislabeling it "allocator metadata," which is why
it never moved.

**Fix** (`test-prog/objstream_alloc.cpp` only): recovered the per-object
heap overhead as `pool_stats.used_bytes` (bytes committed to live blocks,
header + payload) minus the payload bytes the application actually
requested (`total_scalar_bytes + sizeof(ObjStreamMetadata)`), and added
that back to the fixed prefix:

```cpp
total_payload_bytes = total_scalar_bytes + sizeof(ObjStreamMetadata);
fixed_prefix_bytes  = shm_region_size(region) - pool_stats.total_bytes;
heap_overhead_bytes = pool_stats.used_bytes - total_payload_bytes;
allocator_metadata_bytes = fixed_prefix_bytes + heap_overhead_bytes;
```

Verified the result is now exact, at three scales (`object_count × 16 +
2240` -- the `2240` is the fixed region-header + name-table prefix, unchanged
from before):

| element_count | objects | reported metadata | `objects × 16 + 2240` |
|---|---|---|---|
| 1,000,000 | 3,000,001 | 48,002,256 B | 48,002,256 B |
| 2,000,000 | 6,000,001 | 96,002,256 B | 96,002,256 B |
| 100,000,000 (the user's repro) | 300,000,001 | 4,800,002,256 B (4.48 GiB) | 4,800,002,256 B |

This calculation is backend-agnostic and strictly more correct for v2 too
(v2's 56-byte block headers inside the heap were equally being missed by
the old formula) -- not something this fix changed behavior for, since
`objstream_alloc.cpp` only ever runs on the slim engine, but worth noting
for anyone porting the same stat-computation pattern elsewhere.
