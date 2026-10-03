// ============================================================================
// mask_prefix_ref.h -- the mask-register reference model, stated once and used
// by both the unit-level cases and the core-level case.
//
// The unit-level evidence for the mask families lives in sim/unit/tb_vec.cpp:
// `rvv.integer_mask_permute` drives the element lane over every MASKLOG op and
// every MASKPFX op, and `rvv.mask_prefix_semantics` / `rvv.mask_prefix_masked`
// / `rvv.mask_prefix_vstart` cover the prefix family's boundaries and masked
// forms.  CASE=vec.mask_prefix_at_core advertises those families in the core
// and must check the architectural result against *the same* model -- a second
// host model would be a second oracle and the two could disagree.  So the
// element rules are stated here and both testbenches call them:
//
//   * `Elem`             -- one destination element of vmsbf/vmsif/vmsof, from
//                           the source bit and the running prefix flag.
//   * `ExpectedBits`     -- the whole unmasked destination mask over [0, vl).
//   * `MaskedExpectedBits` -- the masked-form oracle, whose search is over the
//                           ACTIVE elements only and which applies vma/vta.
//   * `MaskLogElem`      -- one destination element of the eight mask-register
//                           logical operations.
//
// The rules are the pinned V spec's (v-spec.adoc, "Vector Mask Instructions"):
//
//   vmsbf.m  vd[i] = 1 iff no active source bit at or before i is set
//   vmsif.m  vd[i] = 1 iff no active source bit strictly before i is set
//   vmsof.m  vd[i] = 1 iff source bit i is the first active set bit
//
// An all-zero active source is therefore all-ones for vmsbf/vmsif and
// all-zeros for vmsof -- the asymmetry the spec states and a naive reading
// misses.
// ============================================================================

#ifndef MOSAIC_MASK_PREFIX_REF_H_
#define MOSAIC_MASK_PREFIX_REF_H_

#include <cstdint>

namespace mosaic_maskpfx {

// One destination element of a mask-prefix instruction. `op` is 0 = vmsbf,
// 1 = vmsif, 2 = vmsof. `src_bit` is the source mask bit at this element;
// `pfx_in` is the OR of the source bits strictly before it. Returns the
// destination bit and, through `pfx_out`, the updated running prefix.
inline bool Elem(int op, bool src_bit, bool pfx_in, bool* pfx_out) {
  bool mres;
  if (op == 0) {
    mres = !(pfx_in || src_bit);   // vmsbf: before the first
  } else if (op == 1) {
    mres = !pfx_in;                // vmsif: through the first
  } else {
    mres = src_bit && !pfx_in;     // vmsof: only the first
  }
  *pfx_out = pfx_in || src_bit;
  return mres;
}

// The unmasked destination mask over the active elements [0, vl), as a bitmask
// with bit i the destination element i. (A mask register at e8/m1 holds VLEN
// bits; the caller passes the vl it configured.)
inline uint64_t ExpectedBits(int op, uint64_t src, int vl) {
  bool pfx = false;
  uint64_t out = 0;
  for (int i = 0; i < vl; ++i) {
    const bool bit = Elem(op, ((src >> i) & 1u) != 0, pfx, &pfx);
    if (bit) out |= (1ull << i);
  }
  return out;
}

// The masked-form oracle (unit CASE=rvv.mask_prefix_masked). `src` and `mask`
// are the source mask and the v0 operand; `old` is the destination mask before
// the instruction. The search is over the ACTIVE elements only -- a masked-off
// source element contributes nothing, it is not read as a zero. `vma`/`vta`
// select the masked-off and tail policies for the mask destination.
inline uint64_t MaskedExpectedBits(int op, uint64_t src, uint64_t mask,
                                   uint64_t old, int vl, int vma, int vta) {
  int k = -1;
  for (int i = 0; i < vl; ++i) {
    if (((mask >> i) & 1u) != 0 && ((src >> i) & 1u) != 0) { k = i; break; }
  }
  uint64_t out = old;
  for (int i = 0; i < 16; ++i) {
    bool bit;
    if (i >= vl) {
      if (!vta) continue;                       // tail, undisturbed
      bit = true;                               // tail, mask-agnostic all-ones
    } else if (((mask >> i) & 1u) == 0) {
      if (!vma) continue;                       // masked off, undisturbed
      bit = true;                               // masked off, all-ones
    } else if (op == 0) {
      bit = (k < 0) || (i < k);                 // vmsbf: before the first
    } else if (op == 1) {
      bit = (k < 0) || (i <= k);                // vmsif: through the first
    } else {
      bit = (k >= 0) && (i == k);               // vmsof: only the first
    }
    out = (out & ~(1ull << i)) | (bit ? (1ull << i) : 0ull);
  }
  return out;
}

// One destination element of the mask-register logical operations. `a` is the
// vs2 mask bit, `b` the vs1 mask bit. Op numbering is the unit lane's:
// 0 vmand, 1 vmnand, 2 vmor, 3 vmnor, 4 vmxor, 5 vmxnor, 6 vmandn, 7 vmorn.
inline bool MaskLogElem(int op, bool a, bool b) {
  switch (op) {
    case 0: return a && b;
    case 1: return !(a && b);
    case 2: return a || b;
    case 3: return !(a || b);
    case 4: return a != b;
    case 5: return !(a != b);
    case 6: return a && !b;
    default: return a || !b;
  }
}

// The unmasked destination mask of a mask-register logical operation over
// [0, vl), with bit i of `a`/`b` the two source mask bits.
inline uint64_t MaskLogExpectedBits(int op, uint64_t a, uint64_t b, int vl) {
  uint64_t out = 0;
  for (int i = 0; i < vl; ++i) {
    if (MaskLogElem(op, ((a >> i) & 1u) != 0, ((b >> i) & 1u) != 0)) {
      out |= (1ull << i);
    }
  }
  return out;
}

}  // namespace mosaic_maskpfx

#endif  // MOSAIC_MASK_PREFIX_REF_H_
