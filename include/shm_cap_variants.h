/**
 * @file  shm_cap_variants.h
 * @brief Overview of the three capability-pointer physical encodings
 *        explored for the slimmed CapCXL capability design, and the
 *        unimplemented sketch of variant (b).
 *
 * Exactly one of these is "the" representation for a given build or
 * deployment; they are not meant to interoperate, and nothing in this
 * repository converts between them.
 *
 *   (a) Side metadata — shm_cap_side.h / shm_cap_side.c.
 *       256 bits (32 B), stored next to the pointer, not merged into it.
 *       Software-only: works on any host/compiler, no hardware dependency.
 *       Bearer-key model (possession of the key is both necessary and
 *       sufficient; there is no owner/identity check), matching the
 *       CapCXL CEU's rotating-key design.
 *
 *   (b) Merged with the address — shm_cap_fat_t, below. NOT IMPLEMENTED.
 *       See the type's own documentation for why.
 *
 *   (c) LAM-embedded key — include/shm_hwcap.h + include/shm_hwcap_mock.h.
 *       The existing hardware-capability bridge, carrying a bearer key in
 *       the pointer's LAM bits, content-addressed (CAM-matched) against a
 *       table on the remote/device side — the software mock in
 *       shm_hwcap_mock.c implements the same matching algorithm the
 *       CapCXL CEU RTL does, so it stands in for real capability hardware.
 */

#ifndef SHM_CAP_VARIANTS_H
#define SHM_CAP_VARIANTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Variant (b): a single 192-bit value merging the real 64-bit address with
 * 128 bits of capability metadata, so the capability IS the pointer rather
 * than something stored beside it (shm_cap_side_t) or indexed via spare
 * address bits (the LAM path).
 *
 * Deliberately left unimplemented, and not just unfinished: on stock
 * x86-64/ARM64 hardware, no compiler or ABI represents a pointer wider than
 * one general-purpose register. sizeof(T*) is fixed at 64 bits by every
 * mainstream ABI (SysV x86-64, AAPCS64, Win64) — a 192-bit value cannot be
 * passed in a single register, cannot be the address operand of a native
 * load/store instruction, and cannot be returned through the ABI's pointer
 * convention. It cannot participate in ordinary `ptr->field` dereference
 * syntax; every access would need an explicit software call to extract the
 * real address first, which defeats the point of a capability *pointer*.
 *
 * Real CHERI's 128-bit capabilities work around this with a dedicated,
 * widened capability register file (separate from the ordinary 64-bit GPRs)
 * and new ISA instructions (CLC/CSC for capability load/store, CSetBounds /
 * CIncOffset for manipulation) — new silicon and a new ISA, not a bigger
 * struct on an unmodified processor.
 *
 * Field layout (illustrative — see field widths below; no code anywhere in
 * this repository packs, unpacks, or otherwise operates on this type):
 *
 *   addr        : 64 bits — the real address; usable as-is by hardware that
 *                 can actually consume a capability register's address
 *                 field directly (i.e. real capability-aware hardware).
 *   base        : 30 bits — CXL-pool-relative (CXL memory is assumed to
 *                 start at a 1 GiB-aligned offset, so the low 30 bits of an
 *                 absolute base are sufficient within one pool).
 *   bound       : 56 bits — byte-granular extent; 56 bits covers up to a
 *                 2^56-byte region at 1-byte precision (the smallest
 *                 object is 1 byte, so no coarser granularity is assumed).
 *   perms       :  2 bits — R/W, matching the narrowed permission model
 *                 used throughout the rotating-key design.
 *   capability  : 16 bits — allocation identifier.
 *   key         : 24 bits — the rotatable bearer secret.
 *
 *   30 + 56 + 2 + 16 + 24 = 128 bits of metadata, plus the 64-bit address,
 *   for 192 bits total.
 *
 * Theoretically this works with the suggested fields and if we have
 * capability hardware, we can show this implemented correctly.
 */
typedef struct shm_cap_fat {
    uint64_t addr;    /* 64  - real address */
    uint64_t words[2]; /* 128 - packed base/bound/perms/capability/key;
                         * exact bit positions intentionally left unresolved
                         * -- see the field-layout note above. */
} shm_cap_fat_t;

#ifdef __cplusplus
}
#endif

#endif /* SHM_CAP_VARIANTS_H */
