// ============================================================================
// tb_vec.cpp -- CASE=rvv.descriptor_legality (work package I-051),
//               CASE=rvv.vset_boundaries (work package I-052) and
//               CASE=rvv.vtype_layout (work package I-051).
//
// The single binary serves all three cases; `--case` selects which phase set
// runs and the RESULT line names it (`mosaic::Reporter::Finish` prints
// `options.case_id`).
//
// This case freezes two things and proves both.
//
// 1. The legality matrix. `mosaic_vec_desc` answers, for one (vtype, operation
//    class, register assignment, mask) tuple, whether the configuration is legal
//    and -- if not -- the first rule it breaks. The oracle for that answer is
//    *written here from the V spec* (the rules quoted in the RTL header), not
//    read from the DUT: a rule set that agreed with the DUT by construction
//    would agree with a wrong DUT. Every one of the 64 x 8 x 4 x 2 combinations
//    is visited and asserted, so the matrix is enumerated rather than sampled,
//    and coverage is itself a check: the legal count, the illegal count, the set
//    of classes visited and the set of distinct reasons observed must all match
//    what the sweep was designed to cover. A rule that is never reached fails
//    the reason-coverage check even if every visited combination passed.
//
// 2. The descriptor. One vector macro -- one ROB entry -- carries a macro
//    identity, an element *bitmap* and a separate fault record, rather than one
//    ROB entry per element. The phases drive completions out of order, a fault
//    after a partial prefix, a second allocate while busy, and a reset while a
//    macro and its completions are in flight. The standing invariant checked on
//    every cycle is that one valid descriptor never consumes more than one ROB
//    entry and that the done counter equals the bitmap population count.
//
// Mutation hooks: the shipping build passes; each `-DMOSAIC_VEC_MUTANT_*` build
// must fail on the named check. See results/reports/I-051-rvv-descriptor.md.
// ============================================================================

#include <verilated.h>

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sim_common.h"
#include "mask_prefix_ref.h"
#include "Vmosaic_vec_tb.h"

namespace {

using mosaic::ClockDriver;
using mosaic::Reporter;

// ------------------------------------------------------------------- encodings
// Mirrors of mosaic_vec_desc's localparams. The numbering is the contract
// between the RTL and this driver; a renumbering in the module fails here.
enum : int {
  VOP_IVV = 0, VOP_VMUL = 1, VOP_VDIV = 2, VOP_VWIDE = 3, VOP_VNARROW = 4,
  VOP_VEXT = 5, VOP_VRED = 6, VOP_VMASK = 7, VOP_VMASKMV = 8, VOP_VSLIDE = 9,
  VOP_VFP = 10, VOP_VFPWIDE = 11, VOP_VFPRED = 12, VOP_VLOAD = 13,
  VOP_VSTORE = 14, VOP_VWHOLE = 15, VOP_VSET = 16, VOP_VSPECIAL = 17,
  VOP_COUNT = 18
};

enum : int {
  RSN_OK = 0, RSN_VTYPE_UNSUPP = 1, RSN_EMUL_RANGE = 2, RSN_EEW_RANGE = 3,
  RSN_OVERLAP_SRC = 4, RSN_MASK_DST_OVER = 5, RSN_RESERVED_VLMUL = 6,
  RSN_RESERVED_VSEW = 7, RSN_CLASS_INVALID = 8, RSN_COUNT = 9
};

const char* ClassName(int op) {
  switch (op) {
    case VOP_IVV: return "IVV";
    case VOP_VMUL: return "VMUL";
    case VOP_VDIV: return "VDIV";
    case VOP_VWIDE: return "VWIDE";
    case VOP_VNARROW: return "VNARROW";
    case VOP_VEXT: return "VEXT";
    case VOP_VRED: return "VRED";
    case VOP_VMASK: return "VMASK";
    case VOP_VMASKMV: return "VMASKMV";
    case VOP_VSLIDE: return "VSLIDE";
    case VOP_VFP: return "VFP";
    case VOP_VFPWIDE: return "VFPWIDE";
    case VOP_VFPRED: return "VFPRED";
    case VOP_VLOAD: return "VLOAD";
    case VOP_VSTORE: return "VSTORE";
    case VOP_VWHOLE: return "VWHOLE";
    case VOP_VSET: return "VSET";
    case VOP_VSPECIAL: return "VSPECIAL";
    default: return "?";
  }
}

const char* ReasonName(int r) {
  switch (r) {
    case RSN_OK: return "OK";
    case RSN_VTYPE_UNSUPP: return "VTYPE_UNSUPPORTED";
    case RSN_EMUL_RANGE: return "EMUL_RANGE";
    case RSN_EEW_RANGE: return "EEW_RANGE";
    case RSN_OVERLAP_SRC: return "OVERLAP_SRC";
    case RSN_MASK_DST_OVER: return "MASK_DST_OVERLAP";
    case RSN_RESERVED_VLMUL: return "RESERVED_VLMUL";
    case RSN_RESERVED_VSEW: return "RESERVED_VSEW";
    case RSN_CLASS_INVALID: return "CLASS_INVALID";
    default: return "?";
  }
}

std::string Dec(uint64_t v) { return std::to_string(v); }

// `vsew` in this file is the RVV 1.0 *field* value: log2(SEW) - 3, i.e. 0=e8,
// 1=e16, 2=e32, 3=e64. Widths are derived with `+ kSewLog2Base`. This is the
// specification's encoding, so the oracle can disagree with a DUT that stored
// the old log2(SEW) encoding.
constexpr int kSewLog2Base = 3;   // log2(SEW) = vsew_field + kSewLog2Base

// `vsew` is the RVV 1.0 vtype.vsew field value: log2(SEW) - 3 (0=e8 .. 3=e64).
uint64_t Vtype(int vsew, int vlmul, bool vill) {
  // Ratified v1.0 positions: vlmul[2:0], vsew[5:3], vill[63].
  uint64_t v = (static_cast<uint64_t>(vsew & 7) << 3) | static_cast<uint64_t>(vlmul & 7);
  if (vill) v |= (1ull << 63);
  return v;
}

// The field value for a width exponent: vsew = log2(SEW) - 3. Used at the few
// call sites that carry a width (sew_l) rather than an already-encoded field.
int SewField(int sew_l) { return sew_l - kSewLog2Base; }

// ---------------------------------------------------------------- spec oracle
// The class table, transcribed from the V-spec rules quoted in the RTL header.
// dst_kind: 0 element, 1 mask, 2 none.
// overlap:  0 any allowed, 1 partial forbidden, 2 any forbidden.
struct Cls {
  int dst_kind;
  int dst_sew_off;
  int src_sew_off;
  int dst_lmul_off;
  int src_lmul_off;
  int overlap;
  int min_sew;
  bool vtype_free;
};

const Cls kClasses[VOP_COUNT] = {
    /* IVV      */ {0, 0, 0, 0, 0, 0, 3, false},
    /* VMUL     */ {0, 0, 0, 0, 0, 0, 3, false},
    /* VDIV     */ {0, 0, 0, 0, 0, 0, 3, false},
    /* VWIDE    */ {0, 1, 0, 1, 0, 1, 3, false},
    /* VNARROW  */ {0, 0, 1, 0, 1, 1, 3, false},
    /* VEXT     */ {0, 1, 0, 0, 0, 1, 3, false},
    /* VRED     */ {0, 0, 0, 0, 0, 2, 3, false},
    /* VMASK    */ {1, 0, 0, 0, 0, 0, 3, false},
    /* VMASKMV  */ {1, 0, 0, 0, 0, 2, 3, false},
    /* VSLIDE   */ {0, 0, 0, 0, 0, 0, 3, false},
    /* VFP      */ {0, 0, 0, 0, 0, 0, 4, false},
    /* VFPWIDE  */ {0, 1, 0, 1, 0, 1, 4, false},
    /* VFPRED   */ {0, 0, 0, 0, 0, 2, 4, false},
    /* VLOAD    */ {0, 0, 0, 0, 0, 0, 3, false},
    /* VSTORE   */ {2, 0, 0, 0, 0, 0, 3, false},
    /* VWHOLE   */ {0, 0, 0, 0, 0, 0, 3, true},
    /* VSET     */ {0, 0, 0, 0, 0, 0, 3, true},
    /* VSPECIAL */ {0, 0, 0, 0, 0, 0, 3, false},
};

int LmulExp(int vlmul) {
  switch (vlmul) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 5: return -3;
    case 6: return -2;
    case 7: return -1;
    default: return 0;  // reserved encoding; rejected separately
  }
}

// `vsew` here and everywhere below is the RVV 1.0 *field* value: log2(SEW) - 3,
// i.e. 0=e8, 1=e16, 2=e32, 3=e64. Widths are derived with `+ 3`.

bool VsewValid(int vsew) { return vsew >= 0 && vsew <= 3; }
bool VlmulValid(int vlmul) { return vlmul != 4; }

int VtypeReason(int vsew, int vlmul, bool vill) {
  if (vill) return RSN_VTYPE_UNSUPP;
  if (!VsewValid(vsew)) return RSN_RESERVED_VSEW;
  if (!VlmulValid(vlmul)) return RSN_RESERVED_VLMUL;
  if (LmulExp(vlmul) + 6 < vsew + kSewLog2Base) return RSN_EMUL_RANGE;
  return RSN_OK;
}

bool VtypeLegal(int vsew, int vlmul, bool vill) {
  return VtypeReason(vsew, vlmul, vill) == RSN_OK;
}

int GrpSize(int e) { return e < 0 ? 1 : (1 << e); }
int GrpBase(int reg, int e) { return e < 0 ? reg : (reg & ~(GrpSize(e) - 1)); }

bool GrpOverlap(int a, int ae, int b, int be) {
  int ab = GrpBase(a, ae), as = GrpSize(ae);
  int bb = GrpBase(b, be), bs = GrpSize(be);
  return (ab < bb + bs) && (bb < ab + as);
}
bool GrpEqual(int a, int ae, int b, int be) {
  return GrpSize(ae) == GrpSize(be) && GrpBase(a, ae) == GrpBase(b, be);
}

int OracleReason(int op, int vsew, int vlmul, bool vill, int vd, int vs1, int vs2,
                 int vs3, bool mask_en) {
  if (op < 0 || op >= VOP_COUNT) return RSN_CLASS_INVALID;
  const Cls& c = kClasses[op];
  if (c.vtype_free) return RSN_OK;
  int vr = VtypeReason(vsew, vlmul, vill);
  if (vr != RSN_OK) return vr;
  int sew_l = vsew + kSewLog2Base;   // field -> width exponent
  if (sew_l < c.min_sew) return RSN_EEW_RANGE;
  int lmul_e = LmulExp(vlmul);
  int dst_eew = sew_l + c.dst_sew_off;
  int src_eew = sew_l + c.src_sew_off;
  int dst_emul = lmul_e + c.dst_lmul_off;
  int src_emul = lmul_e + c.src_lmul_off;
  bool dst_eew_ok = dst_eew >= 3 && dst_eew <= 6;
  bool src_eew_ok = src_eew >= 3 && src_eew <= 6;
  bool dst_emul_ok = dst_emul >= -3 && dst_emul <= 3;
  bool src_emul_ok = src_emul >= -3 && src_emul <= 3;
  if (c.dst_kind == 0 && !dst_eew_ok) return RSN_EEW_RANGE;
  if (!src_eew_ok || !dst_emul_ok || !src_emul_ok) return RSN_EMUL_RANGE;
  bool mask_dst_overlap = (c.dst_kind != 2) && (GrpBase(vd, dst_emul) == 0);
  if (mask_en && mask_dst_overlap) return RSN_MASK_DST_OVER;
  if (c.dst_kind != 2) {
    bool partial = GrpOverlap(vd, dst_emul, vs1, src_emul) && !GrpEqual(vd, dst_emul, vs1, src_emul);
    bool any = GrpOverlap(vd, dst_emul, vs1, src_emul);
    partial = partial || (GrpOverlap(vd, dst_emul, vs2, src_emul) && !GrpEqual(vd, dst_emul, vs2, src_emul));
    any = any || GrpOverlap(vd, dst_emul, vs2, src_emul);
    partial = partial || (GrpOverlap(vd, dst_emul, vs3, src_emul) && !GrpEqual(vd, dst_emul, vs3, src_emul));
    any = any || GrpOverlap(vd, dst_emul, vs3, src_emul);
    if (c.overlap == 2 && any) return RSN_OVERLAP_SRC;
    if (c.overlap == 1 && partial) return RSN_OVERLAP_SRC;
  }
  return RSN_OK;
}

int OracleElemCount(int vsew, int vlmul) {
  int e = 7 + LmulExp(vlmul) - (vsew + kSewLog2Base);  // VLEN=128 -> log2 = 7
  if (e < 0) return 0;
  return 1 << e;
}

// ------------------------------------------------------------------- stimulus
struct RegCase {
  int vd, vs1, vs2, vs3;
  const char* name;
};
const RegCase kRegs[4] = {
    {5, 6, 7, 8, "disjoint"},
    {5, 5, 5, 5, "exact"},
    {5, 6, 5, 8, "partial"},
    {0, 1, 2, 3, "vd0"},
};

struct QueryObs {
  bool vtype_legal = false;
  bool cfg_legal = false;
  bool illegal = false;
  int reason = 0;
  int sew_log2 = 0;
  int lmul_exp = 0;
  int emul_src_exp = 0;
  int emul_dst_exp = 0;
  int elem_count = 0;
  int vlen = 0;
  int vlenb = 0;
  int lane_count = 0;
  int class_count = 0;
};

struct Stim {
  bool rst = false;
  bool alloc_valid = false;
  uint64_t alloc_vtype = 0;
  int alloc_vl = 0;
  int alloc_vstart = 0;
  int alloc_vd = 0;
  int alloc_mask_ver = 0;
  uint64_t rob_index = 0;
  uint64_t rob_gen = 0;
  uint64_t uop_index = 0;
  bool elem_done_valid = false;
  int elem_done_index = 0;
  bool fault_valid = false;
  int fault_elem = 0;
  int fault_code = 0;
  bool release = false;
};

struct DescObs {
  bool valid = false;
  bool alloc_ready = false;
  uint64_t rob_index = 0;
  uint64_t rob_gen = 0;
  uint64_t uop_index = 0;
  uint64_t vtype = 0;
  int vl = 0;
  int vstart = 0;
  int vd = 0;
  int mask_ver = 0;
  uint64_t bm_lo = 0;
  uint64_t bm_hi = 0;
  int prefix = 0;
  int done_ctr = 0;
  bool fault_valid = false;
  int fault_elem = 0;
  int fault_code = 0;
  bool accepting = false;
  int rob_used = 0;
  int alloc_ctr = 0;
  int release_ctr = 0;
};

// Verilator reads a `logic signed [3:0]` port as an unsigned nibble; the
// driver owns the sign interpretation of these two's-complement values.
int Sgn4(int v) { return (v & 8) ? v - 16 : v; }

int Popcount128(uint64_t lo, uint64_t hi) {
  return __builtin_popcountll(lo) + __builtin_popcountll(hi);
}
bool BitmapBit(uint64_t lo, uint64_t hi, int i) {
  return i < 64 ? ((lo >> i) & 1ull) != 0 : ((hi >> (i - 64)) & 1ull) != 0;
}

// -------------------------------------------------------------------- harness
class Dut {
 public:
  Dut(Vmosaic_vec_tb* d, ClockDriver* clk, Reporter* rep)
      : d_(d), clk_(clk), rep_(rep) {
    // The I-054 ports are quiescent for every other case: the vector ALU must
    // not start a packet because an undriven input happened to be set.
    d_->mem_owner_i = 0;
    d_->mem_rd_valid_i = 0;
    d_->mem_wr_valid_i = 0;
    d_->alu_exec_valid_i = 0;
    d_->alu_caps_i = 0;
    d_->el_valid_i = 0;
  }

  void Masks(uint64_t rob, uint64_t gen, uint64_t uop) {
    rob_mask_ = rob;
    gen_mask_ = gen;
    uop_mask_ = uop;
  }

  QueryObs Query(uint64_t vtype, int op, int vd, int vs1, int vs2, int vs3,
                 bool mask_en, int lane_count) {
    d_->clk = 0;
    d_->rst = 0;
    d_->q_vtype = vtype;
    d_->q_op_class = static_cast<uint8_t>(op & 0x1F);
    d_->q_vd = static_cast<uint8_t>(vd & 0x1F);
    d_->q_vs1 = static_cast<uint8_t>(vs1 & 0x1F);
    d_->q_vs2 = static_cast<uint8_t>(vs2 & 0x1F);
    d_->q_vs3 = static_cast<uint8_t>(vs3 & 0x1F);
    d_->q_mask_en = mask_en ? 1 : 0;
    d_->q_lane_count = static_cast<uint8_t>(lane_count & 0xF);
    d_->eval();
    QueryObs q;
    q.vtype_legal = d_->o_vtype_legal != 0;
    q.cfg_legal = d_->o_cfg_legal != 0;
    q.illegal = d_->o_illegal != 0;
    q.reason = static_cast<int>(d_->o_reason);
    q.sew_log2 = static_cast<int>(d_->o_sew_log2);
    q.lmul_exp = Sgn4(static_cast<int>(d_->o_lmul_exp));
    q.emul_src_exp = Sgn4(static_cast<int>(d_->o_emul_src_exp));
    q.emul_dst_exp = Sgn4(static_cast<int>(d_->o_emul_dst_exp));
    q.elem_count = static_cast<int>(d_->o_elem_count);
    q.vlen = static_cast<int>(d_->o_vlen);
    q.vlenb = static_cast<int>(d_->o_vlenb);
    q.lane_count = static_cast<int>(d_->o_lane_count);
    q.class_count = static_cast<int>(d_->o_class_count);
    return q;
  }

  DescObs Cycle(const Stim& s) {
    d_->clk = 0;
    d_->rst = s.rst ? 1 : 0;
    d_->alloc_valid = s.alloc_valid ? 1 : 0;
    d_->alloc_vtype = s.alloc_vtype;
    d_->alloc_vl = static_cast<uint8_t>(s.alloc_vl & 0xFF);
    d_->alloc_vstart = static_cast<uint8_t>(s.alloc_vstart & 0x7F);
    d_->alloc_vd = static_cast<uint8_t>(s.alloc_vd & 0x1F);
    d_->alloc_mask_ver = static_cast<uint8_t>(s.alloc_mask_ver & 0xF);
    d_->alloc_rob_index = s.rob_index & rob_mask_;
    d_->alloc_rob_gen = s.rob_gen & gen_mask_;
    d_->alloc_uop_index = s.uop_index & uop_mask_;
    d_->elem_done_valid = s.elem_done_valid ? 1 : 0;
    d_->elem_done_index = static_cast<uint8_t>(s.elem_done_index & 0x7F);
    d_->fault_valid = s.fault_valid ? 1 : 0;
    d_->fault_elem = static_cast<uint8_t>(s.fault_elem & 0x7F);
    d_->fault_code = static_cast<uint8_t>(s.fault_code & 0xF);
    d_->desc_release = s.release ? 1 : 0;
    d_->eval();
    d_->clk = 1;
    d_->eval();
    d_->clk = 0;
    d_->eval();

    DescObs o;
    o.valid = d_->o_valid != 0;
    o.alloc_ready = d_->alloc_ready != 0;
    o.rob_index = d_->o_macro_rob_index;
    o.rob_gen = d_->o_macro_rob_gen;
    o.uop_index = d_->o_macro_uop_index;
    o.vtype = d_->o_desc_vtype;
    o.vl = static_cast<int>(d_->o_desc_vl);
    o.vstart = static_cast<int>(d_->o_desc_vstart);
    o.vd = static_cast<int>(d_->o_desc_vd);
    o.mask_ver = static_cast<int>(d_->o_desc_mask_ver);
    o.bm_lo = d_->o_elem_bitmap_lo;
    o.bm_hi = d_->o_elem_bitmap_hi;
    o.prefix = static_cast<int>(d_->o_prefix);
    o.done_ctr = static_cast<int>(d_->o_elems_done_ctr);
    o.fault_valid = d_->o_fault_valid != 0;
    o.fault_elem = static_cast<int>(d_->o_fault_elem);
    o.fault_code = static_cast<int>(d_->o_fault_code);
    o.accepting = d_->o_accepting_elems != 0;
    o.rob_used = static_cast<int>(d_->o_rob_entries_used);
    o.alloc_ctr = static_cast<int>(d_->o_alloc_ctr);
    o.release_ctr = static_cast<int>(d_->o_release_ctr);
    clk_->Tick();
    CheckInvariant(o, "cycle");
    return o;
  }

  // The standing descriptor invariant: one macro identity is at most one ROB
  // entry, and the done counter is the bitmap population count. A descriptor
  // that leaks an entry per element, or loses a completion, breaks one of these.
  void CheckInvariant(const DescObs& o, const std::string& where) {
    rep_->Check(o.rob_used <= 1,
                where + ": rob-entries-used " + Dec(o.rob_used) +
                    " exceeds one for a single vector macro");
    rep_->Check(o.done_ctr == Popcount128(o.bm_lo, o.bm_hi),
                where + ": done-ctr " + Dec(o.done_ctr) + " != bitmap popcount " +
                    Dec(Popcount128(o.bm_lo, o.bm_hi)));
    if (o.valid && !o.fault_valid) {
      rep_->Check(o.accepting, where + ": a fault-free valid descriptor refuses elements");
    }
  }

 private:
  Vmosaic_vec_tb* d_;
  ClockDriver* clk_;
  Reporter* rep_;
  uint64_t rob_mask_ = 0xFFFFFFFFull;
  uint64_t gen_mask_ = 0xFFFFFFFFull;
  uint64_t uop_mask_ = 0xFFFFFFFFull;
};

// --------------------------------------------------------------------- phases
void PhaseGeometry(Dut* d, Reporter* rep) {
  // VLEN itself is checked in PhaseLaneInvariance, where it is checked against
  // the lane quota that must not affect it; here the profile constants that are
  // not a function of lanes are established.
  const QueryObs q = d->Query(Vtype(2, 0, false), VOP_IVV, 1, 2, 3, 4, false, 2);
  rep->Check(q.vlenb == 16, "geometry: vlenb read back as " + Dec(q.vlenb) + ", expected 16");
  rep->Check(q.class_count == VOP_COUNT,
             "geometry: class count " + Dec(q.class_count) + ", expected " + Dec(VOP_COUNT));
  rep->Check(q.class_count == 18, "geometry: the declared operation list is 18 families");

  // The effective-width arithmetic that the matrix is built on, stated as
  // direct examples: a widening operation doubles both EEW and EMUL, a
  // narrowing source doubles its EMUL.
  const QueryObs wide = d->Query(Vtype(0, 0, false), VOP_VWIDE, 1, 2, 3, 4, false, 2);
  rep->Check(wide.cfg_legal && wide.emul_dst_exp == 1 && wide.emul_src_exp == 0,
             "geometry: vwadd at LMUL=1 has destination EMUL 2 and source EMUL 1");
  const QueryObs narrow = d->Query(Vtype(0, 0, false), VOP_VNARROW, 1, 2, 3, 4, false, 2);
  rep->Check(narrow.cfg_legal && narrow.emul_src_exp == 1 && narrow.emul_dst_exp == 0,
             "geometry: vnclip at LMUL=1 has source EMUL 2 and destination EMUL 1");
}

void PhaseVtypeMatrix(Dut* d, Reporter* rep, int* reason_hist) {
  int legal = 0, illegal = 0, visited = 0;
  for (int vsew = 0; vsew < 8; ++vsew) {
    for (int vlmul = 0; vlmul < 8; ++vlmul) {
      const uint64_t vt = Vtype(vsew, vlmul, false);
      const QueryObs q = d->Query(vt, VOP_IVV, 1, 2, 3, 4, false, 2);
      const bool expect = VtypeLegal(vsew, vlmul, false);
      const int expect_reason = VtypeReason(vsew, vlmul, false);
      const std::string name = "vtype-matrix vsew=" + Dec(vsew) + " vlmul=" + Dec(vlmul);
      rep->Check(q.vtype_legal == expect,
                 name + ": vtype legality expected " + (expect ? "legal" : "illegal"));
      if (q.vtype_legal != expect) {
        continue;
      }
      ++visited;
      if (!expect) {
        rep->Check(q.reason == expect_reason,
                   name + ": reason expected " + ReasonName(expect_reason) + " got " +
                       ReasonName(q.reason));
        ++reason_hist[q.reason];
      } else {
        rep->Check(q.cfg_legal && q.reason == RSN_OK,
                   name + ": a legal vtype with VOP_IVV is legal");
        rep->Check(q.sew_log2 == vsew + kSewLog2Base, name + ": sew_log2");
        rep->Check(q.lmul_exp == LmulExp(vlmul), name + ": lmul_exp");
        rep->Check(q.elem_count == OracleElemCount(vsew, vlmul), name + ": elem_count");
        ++reason_hist[RSN_OK];
      }
      legal += expect ? 1 : 0;
      illegal += expect ? 0 : 1;
    }
  }
  rep->Check(visited == 64, "vtype-matrix: visited " + Dec(visited) + " of 64 vsew/vlmul pairs");
  rep->Check(legal == 22, "vtype-matrix: " + Dec(legal) + " legal vtypes, expected 22");
  rep->Check(illegal == 42, "vtype-matrix: " + Dec(illegal) + " illegal vtypes, expected 42");
}

void PhaseOpMatrix(Dut* d, Reporter* rep, int* reason_hist, bool* class_seen) {
  int combos = 0, dut_legal = 0, oracle_legal = 0, mismatches = 0;
  for (int op = 0; op < VOP_COUNT; ++op) {
    for (int vsew = 0; vsew < 8; ++vsew) {
      for (int vlmul = 0; vlmul < 8; ++vlmul) {
        for (int r = 0; r < 4; ++r) {
          for (int m = 0; m < 2; ++m) {
            const uint64_t vt = Vtype(vsew, vlmul, false);
            const QueryObs q = d->Query(vt, op, kRegs[r].vd, kRegs[r].vs1,
                                        kRegs[r].vs2, kRegs[r].vs3, m != 0, 2);
            const int expect_reason = OracleReason(op, vsew, vlmul, false, kRegs[r].vd,
                                                   kRegs[r].vs1, kRegs[r].vs2,
                                                   kRegs[r].vs3, m != 0);
            const bool expect_legal = expect_reason == RSN_OK;
            const bool got_legal = q.cfg_legal;
            ++combos;
            dut_legal += got_legal ? 1 : 0;
            oracle_legal += expect_legal ? 1 : 0;
            ++reason_hist[q.reason];
            class_seen[op] = true;
            if (got_legal != expect_legal || q.reason != expect_reason ||
                q.illegal == got_legal) {
              ++mismatches;
              if (mismatches <= 8) {
                rep->Check(false, "op-matrix " + std::string(ClassName(op)) + " vsew=" +
                                      Dec(vsew) + " vlmul=" + Dec(vlmul) + " reg=" +
                                      kRegs[r].name + " mask=" + Dec(m) +
                                      ": expected " + ReasonName(expect_reason) +
                                      " got " + ReasonName(q.reason));
              }
            }
          }
        }
      }
    }
  }
  rep->Check(combos == VOP_COUNT * 64 * 4 * 2,
             "op-matrix: enumerated " + Dec(combos) + " combinations");
  rep->Check(dut_legal == oracle_legal,
             "op-matrix: DUT legal count " + Dec(dut_legal) + " != oracle " +
                 Dec(oracle_legal));
  rep->Check(mismatches == 0, "op-matrix: " + Dec(mismatches) + " combination(s) disagree");
  int seen = 0;
  for (int op = 0; op < VOP_COUNT; ++op) seen += class_seen[op] ? 1 : 0;
  rep->Check(seen == VOP_COUNT,
             "op-matrix: visited " + Dec(seen) + " of " + Dec(VOP_COUNT) + " families");
}

// A configuration that is legal in every dimension, used to watch VLEN.
void PhaseLaneInvariance(Dut* d, Reporter* rep) {
  const uint64_t vt = Vtype(0, 0, false);  // SEW=8, LMUL=1
  int vlen_seen[3] = {0, 0, 0};
  const int lanes[3] = {2, 4, 8};
  for (int i = 0; i < 3; ++i) {
    const QueryObs q = d->Query(vt, VOP_IVV, 1, 2, 3, 4, false, lanes[i]);
    rep->Check(q.vlen == 128,
               "lane-invariance: lane count " + Dec(lanes[i]) + " changed VLEN to " +
                   Dec(q.vlen) + ", expected 128");
    rep->Check(q.vlenb == 16,
               "lane-invariance: lane count " + Dec(lanes[i]) + " changed vlenb to " +
                   Dec(q.vlenb) + ", expected 16");
    rep->Check(q.lane_count == lanes[i], "lane-invariance: lane count echo");
    // The number of elements is an architectural function of vtype, not of the
    // runtime lane quota.
    rep->Check(q.elem_count == 16,
               "lane-invariance: lane count " + Dec(lanes[i]) + " changed elem_count to " +
                   Dec(q.elem_count) + ", expected 16");
    vlen_seen[i] = q.vlen;
  }
  rep->Check(vlen_seen[0] == vlen_seen[1] && vlen_seen[1] == vlen_seen[2],
             "lane-invariance: VLEN differed across lane counts");
}

void PhaseDescriptorProgress(Dut* d, Reporter* rep) {
  Stim s;
  DescObs o = d->Cycle(s);
  rep->Check(!o.valid, "descriptor-progress: valid after reset");
  rep->Check(o.alloc_ready, "descriptor-progress: not ready after reset");
  rep->Check(o.rob_used == 0, "descriptor-progress: ROB entries used while empty");

  // Allocate identity A.
  s = Stim{};
  s.alloc_valid = true;
  s.alloc_vtype = Vtype(2, 0, false);  // SEW=32, LMUL=1 -> 4 elements
  s.alloc_vl = 5;
  s.alloc_vd = 4;
  s.alloc_mask_ver = 2;
  s.rob_index = 7;
  s.rob_gen = 3;
  s.uop_index = 1;
  o = d->Cycle(s);
  rep->Check(o.valid, "descriptor-progress: allocate was refused");
  rep->Check(o.rob_index == 7 && o.rob_gen == 3 && o.uop_index == 1,
             "descriptor-progress: macro identity not preserved");
  rep->Check(o.vtype == s.alloc_vtype && o.vl == 5 && o.vd == 4 && o.mask_ver == 2,
             "descriptor-progress: configuration snapshot not preserved");
  rep->Check(o.rob_used == 1, "descriptor-progress: one macro must use one ROB entry");
  rep->Check(!o.fault_valid, "descriptor-progress: fault set at allocate");

  // A second allocate while busy is refused and must not disturb identity A.
  Stim busy;
  busy.alloc_valid = true;
  busy.alloc_vtype = Vtype(3, 3, false);
  busy.rob_index = 9;
  busy.rob_gen = 9;
  busy.uop_index = 9;
  o = d->Cycle(busy);
  rep->Check(!o.alloc_ready, "descriptor-progress: a second allocate was accepted");
  rep->Check(o.rob_index == 7 && o.rob_gen == 3 && o.uop_index == 1,
             "descriptor-progress: a refused allocate overwrote the identity");

  // Out-of-order completions: 0, 1, 3, then 2 closes the prefix.
  for (int i : {0, 1}) {
    Stim e;
    e.elem_done_valid = true;
    e.elem_done_index = i;
    o = d->Cycle(e);
  }
  rep->Check(BitmapBit(o.bm_lo, o.bm_hi, 0) && BitmapBit(o.bm_lo, o.bm_hi, 1),
             "descriptor-progress: elements 0 and 1 not recorded");
  rep->Check(o.prefix == 2, "descriptor-progress: prefix " + Dec(o.prefix) + ", expected 2");
  Stim e3;
  e3.elem_done_valid = true;
  e3.elem_done_index = 3;
  o = d->Cycle(e3);
  rep->Check(BitmapBit(o.bm_lo, o.bm_hi, 3), "descriptor-progress: element 3 not recorded");
  rep->Check(o.prefix == 2,
             "descriptor-progress: prefix advanced past a hole: " + Dec(o.prefix));
  rep->Check(o.done_ctr == 3, "descriptor-progress: done counter " + Dec(o.done_ctr));
  Stim e2;
  e2.elem_done_valid = true;
  e2.elem_done_index = 2;
  o = d->Cycle(e2);
  rep->Check(o.prefix == 4,
             "descriptor-progress: prefix " + Dec(o.prefix) + " after the hole closed");
  rep->Check(o.done_ctr == 4, "descriptor-progress: done counter " + Dec(o.done_ctr));
  rep->Check(Popcount128(o.bm_lo, o.bm_hi) == 4, "descriptor-progress: bitmap population");
  rep->Check(o.rob_used == 1, "descriptor-progress: four elements must still be one entry");

  // Release, then a fresh macro with a different identity.
  Stim rel;
  rel.release = true;
  o = d->Cycle(rel);
  rep->Check(!o.valid, "descriptor-progress: release left the descriptor valid");
  rep->Check(o.release_ctr == 1, "descriptor-progress: release counter");
  rep->Check(o.rob_used == 0 && o.done_ctr == 0 && o.prefix == 0,
             "descriptor-progress: release left progress behind");
  Stim reuse;
  reuse.alloc_valid = true;
  reuse.alloc_vtype = Vtype(1, 1, false);
  reuse.rob_index = 11;
  reuse.rob_gen = 2;
  reuse.uop_index = 0;
  o = d->Cycle(reuse);
  rep->Check(o.valid && o.rob_index == 11 && o.rob_gen == 2 && o.uop_index == 0,
             "descriptor-progress: the reused descriptor has the wrong identity");
  Stim rel2;
  rel2.release = true;
  d->Cycle(rel2);
}

void PhaseFaultProgress(Dut* d, Reporter* rep) {
  Stim s;
  s.alloc_valid = true;
  s.alloc_vtype = Vtype(0, 3, false);  // SEW=8, LMUL=8 -> 128 elements
  s.alloc_vl = 8;
  s.rob_index = 5;
  s.rob_gen = 5;
  s.uop_index = 5;
  DescObs o = d->Cycle(s);
  for (int i : {0, 1}) {
    Stim e;
    e.elem_done_valid = true;
    e.elem_done_index = i;
    o = d->Cycle(e);
  }
  Stim f;
  f.fault_valid = true;
  f.fault_elem = 5;
  f.fault_code = 2;
  o = d->Cycle(f);
  rep->Check(o.fault_valid, "fault-progress: fault not recorded");
  rep->Check(o.fault_elem == 5, "fault-progress: fault element " + Dec(o.fault_elem));
  rep->Check(o.fault_code == 2, "fault-progress: fault code " + Dec(o.fault_code));
  rep->Check(!o.accepting, "fault-progress: the descriptor still accepts elements");
  rep->Check(o.prefix == 2, "fault-progress: prefix " + Dec(o.prefix) + " after fault");
  rep->Check(o.rob_used == 1, "fault-progress: fault must not add ROB entries");

  // A younger element completion after the fault must not commit past it.
  Stim e6;
  e6.elem_done_valid = true;
  e6.elem_done_index = 6;
  o = d->Cycle(e6);
  rep->Check(!BitmapBit(o.bm_lo, o.bm_hi, 6),
             "fault-progress: element 6 committed after the fault");
  rep->Check(o.done_ctr == 2, "fault-progress: done counter moved after the fault");

  // A second fault must not overwrite the earliest fault.
  Stim f2;
  f2.fault_valid = true;
  f2.fault_elem = 3;
  f2.fault_code = 1;
  o = d->Cycle(f2);
  rep->Check(o.fault_elem == 5 && o.fault_code == 2,
             "fault-progress: a later fault overwrote the earliest");
  Stim rel;
  rel.release = true;
  d->Cycle(rel);
}

void PhaseResetInFlight(Dut* d, Reporter* rep) {
  Stim s;
  s.alloc_valid = true;
  s.alloc_vtype = Vtype(2, 0, false);
  s.rob_index = 3;
  s.rob_gen = 3;
  s.uop_index = 3;
  DescObs o = d->Cycle(s);
  rep->Check(o.valid, "reset-in-flight: allocate before reset was refused");
  Stim e;
  e.elem_done_valid = true;
  e.elem_done_index = 0;
  o = d->Cycle(e);
  rep->Check(BitmapBit(o.bm_lo, o.bm_hi, 0), "reset-in-flight: element 0 not recorded");

  // Reset with the macro in flight, and pulses presented while reset is high.
  //
  // The contract this asserts is *reset dominance on the DUT*: with `rst` high
  // the descriptor unit must leave no state behind, and the allocate / element
  // done / fault pulses offered on these cycles must not create or preserve any.
  // That is the opposite direction from the harness-side reset-traffic rule
  // (V-010), which says a *bus model* ignores a request the DUT presents while
  // reset is asserted. There is no bus model here -- the DUT's ports are driven
  // directly -- so V-010 does not apply to this case, and the two contracts are
  // not in conflict: one constrains the environment, this one the DUT.
  for (int i = 0; i < 2; ++i) {
    Stim r;
    r.rst = true;
    r.alloc_valid = true;
    r.alloc_vtype = Vtype(3, 3, false);
    r.rob_index = 15;
    r.rob_gen = 15;
    r.uop_index = 7;
    r.elem_done_valid = true;
    r.elem_done_index = 4;
    r.fault_valid = true;
    r.fault_elem = 4;
    o = d->Cycle(r);
  }
  rep->Check(!o.valid, "reset-in-flight: reset left a valid descriptor");
  rep->Check(o.bm_lo == 0 && o.bm_hi == 0, "reset-in-flight: reset left bitmap bits set");
  rep->Check(!o.fault_valid && o.fault_elem == 0,
             "reset-in-flight: reset left fault progress");
  rep->Check(o.prefix == 0 && o.done_ctr == 0, "reset-in-flight: reset left counters");
  rep->Check(o.alloc_ctr == 0 && o.release_ctr == 0,
             "reset-in-flight: reset left the allocate/release counters");
  rep->Check(o.rob_used == 0, "reset-in-flight: reset left a ROB entry charged");

  // Reset deasserted: the descriptor is usable again.
  Stim d0;
  DescObs after = d->Cycle(d0);
  rep->Check(!after.valid && after.alloc_ready, "reset-in-flight: unusable after reset");
  Stim a;
  a.alloc_valid = true;
  a.alloc_vtype = Vtype(2, 0, false);
  a.rob_index = 1;
  o = d->Cycle(a);
  rep->Check(o.valid && o.rob_index == 1, "reset-in-flight: unusable after reset");
  rep->Check(o.alloc_ctr == 1, "reset-in-flight: allocate counter after reset");
  Stim rel;
  rel.release = true;
  d->Cycle(rel);
}

void PhaseReasonCoverage(Dut* d, Reporter* rep, int* reason_hist, bool* class_seen) {
  // Two reason classes need inputs the sweep does not generate: vill (an
  // unsupported vtype argument) and an unknown operation family.
  const QueryObs vill = d->Query(Vtype(0, 0, true), VOP_IVV, 1, 2, 3, 4, false, 2);
  ++reason_hist[vill.reason];
  rep->Check(vill.reason == RSN_VTYPE_UNSUPP,
             "reason-coverage: vill reports VTYPE_UNSUPPORTED");
  const QueryObs unknown = d->Query(Vtype(0, 0, false), VOP_COUNT, 1, 2, 3, 4, false, 2);
  ++reason_hist[unknown.reason];
  rep->Check(unknown.reason == RSN_CLASS_INVALID,
             "reason-coverage: an unknown operation family reports CLASS_INVALID");

  int seen = 0;
  for (int r = 0; r < RSN_COUNT; ++r) seen += reason_hist[r] > 0 ? 1 : 0;
  rep->Check(seen == RSN_COUNT,
             "reason-coverage: " + Dec(seen) + " of " + Dec(RSN_COUNT) +
                 " reasons observed");
  int classes = 0;
  for (int op = 0; op < VOP_COUNT; ++op) classes += class_seen[op] ? 1 : 0;
  rep->Check(classes == VOP_COUNT, "reason-coverage: operation families visited");
  rep->Check(class_seen[VOP_VWHOLE] && class_seen[VOP_VSET],
             "reason-coverage: the vtype-free families were visited");
}

// ============================================================================
// I-052: vset{i}vl{i} and the vector CSRs (CASE=rvv.vset_boundaries).
//
// The oracle for the AVL rules is written from the V spec, not from the DUT:
// the constraints are the three bands ("Constraints on Setting vl"), the
// mandatory properties (vl = 0 iff AVL = 0, vl <= AVL, vl <= VLMAX) and
// determinism. The driver deliberately does *not* require one particular legal
// vl for AVL > VLMAX -- the spec permits several, and the package's declared
// fail mode is enforcing the reference's choice on every implementation.
// ============================================================================

enum : int { VSETVLI = 0, VSETIVLI = 1, VSETVL = 2 };

constexpr uint64_t kCsrVstart = 0x008;
constexpr uint64_t kCsrVxsat  = 0x009;
constexpr uint64_t kCsrVxrm   = 0x00A;
constexpr uint64_t kCsrVcsr   = 0x00F;
constexpr uint64_t kCsrVl     = 0xC20;
constexpr uint64_t kCsrVtype  = 0xC21;
constexpr uint64_t kCsrVlenb  = 0xC22;
constexpr uint64_t kAvlMax    = ~0ull;
constexpr uint64_t kVill      = 0x8000000000000000ull;

// The vtype/vtypei word in the ratified v1.0 layout (vtype-format.adoc of the
// pinned tag; the same fields in the `vset{i}vl{i}` 11-bit immediate):
// vlmul[2:0] = bits 2:0, vsew[2:0] = bits 5:3, vta = bit 6, vma = bit 7.
// This is the specification's encoding, not a mirror of the RTL's constants:
// the case encodes here and requires the unit to decode what was encoded.
uint64_t Vtypei(int vsew, int vlmul, int ta = 0, int ma = 0) {
  return (static_cast<uint64_t>(ma & 1) << 7) |
         (static_cast<uint64_t>(ta & 1) << 6) |
         (static_cast<uint64_t>(vsew & 7) << 3) |
         static_cast<uint64_t>(vlmul & 7);
}

int VlmaxOf(int vsew, int vlmul) {
  if (!VsewValid(vsew) || !VlmulValid(vlmul)) return 0;
  const int e = 7 + LmulExp(vlmul) - (vsew + kSewLog2Base);
  if (e < 0 || e > 7) return 0;
  return 1 << e;
}

bool WordSupported(uint64_t v) {
  if (((v >> 63) & 1ull) != 0) return false;
  if (((v >> 8) & ((1ull << 55) - 1ull)) != 0ull) return false;
  const int vsew = static_cast<int>((v >> 3) & 7ull);
  const int vlmul = static_cast<int>(v & 7ull);
  return VsewValid(vsew) && VlmulValid(vlmul) && (LmulExp(vlmul) + 6 >= vsew + kSewLog2Base);
}

// The spec's AVL bands as bounds, not one blessed value.
bool VlLegal(uint64_t avl, uint64_t vlmax, uint64_t vl) {
  if (vlmax == 0) return false;
  if (avl == 0) return vl == 0;
  if (vl == 0) return false;                    // AVL > 0 requires vl > 0
  if (vl > vlmax) return false;                 // vl <= VLMAX
  if (vl > avl) return false;                   // vl <= AVL
  if (avl <= vlmax) return vl == avl;           // band 1
  if (avl < 2ull * vlmax) return vl >= (avl + 1ull) / 2ull;  // band 2
  return vl == vlmax;                           // band 3
}

struct CfgStim {
  bool rst = false;
  bool vset_valid = false;
  int kind = VSETVLI;
  int rd = 0, rs1 = 0;
  uint64_t rs1_val = 0;
  uint64_t rs2_val = 0;
  int uimm = 0;
  uint64_t vtypei = 0;
  bool vs_off = false;
  bool snap_capture = false;
  bool replay_valid = false;
  int replay_gen = 0;
  bool exec_valid = false;
  bool exec_vtype_dep = false;
  bool csr_valid = false;
  uint64_t csr_addr = 0;
  bool csr_write = false;
  uint64_t csr_wdata = 0;
  int csr_priv = 3;
  bool csr_vs_off = false;
};

struct CfgObs {
  bool vset_illegal = false;
  bool vset_commit = false;
  bool vset_rd_we = false;
  uint64_t vset_rd_val = 0;
  uint64_t vtype = 0;
  uint64_t vl = 0;
  uint64_t vstart = 0;
  uint64_t vxrm = 0;
  uint64_t vxsat = 0;
  uint64_t vcsr = 0;
  uint64_t vlenb = 0;
  uint64_t vlmax = 0;
  bool vill = false;
  int gen = 0;
  bool snap_valid = false;
  uint64_t snap_vtype = 0;
  uint64_t snap_vl = 0;
  uint64_t snap_vstart = 0;
  int snap_gen = 0;
  bool replay_ok = false;
  uint64_t replay_vtype = 0;
  uint64_t replay_vl = 0;
  uint64_t replay_vstart = 0;
  bool exec_illegal = false;
  bool csr_ready = false;
  bool csr_illegal = false;
  bool csr_commit = false;
  uint64_t csr_rdata = 0;
};

class Cfg {
 public:
  Cfg(Vmosaic_vec_tb* d, ClockDriver* clk) : d_(d), clk_(clk) {}

  CfgObs Cycle(const CfgStim& s) {
    d_->clk = 0;
    d_->rst = s.rst ? 1 : 0;
    d_->cfg_vset_valid = s.vset_valid ? 1 : 0;
    d_->cfg_vset_kind = static_cast<uint8_t>(s.kind & 3);
    d_->cfg_vset_rd = static_cast<uint8_t>(s.rd & 0x1F);
    d_->cfg_vset_rs1 = static_cast<uint8_t>(s.rs1 & 0x1F);
    d_->cfg_vset_rs1_val = s.rs1_val;
    d_->cfg_vset_rs2_val = s.rs2_val;
    d_->cfg_vset_uimm = static_cast<uint8_t>(s.uimm & 0x1F);
    d_->cfg_vset_vtypei = static_cast<uint16_t>(s.vtypei & 0x7FF);
    d_->cfg_vset_vs_off = s.vs_off ? 1 : 0;
    d_->cfg_snap_capture = s.snap_capture ? 1 : 0;
    d_->cfg_replay_valid = s.replay_valid ? 1 : 0;
    d_->cfg_replay_gen = static_cast<uint16_t>(s.replay_gen & 0xFFFF);
    d_->cfg_exec_valid = s.exec_valid ? 1 : 0;
    d_->cfg_exec_vtype_dep = s.exec_vtype_dep ? 1 : 0;
    d_->cfg_csr_valid = s.csr_valid ? 1 : 0;
    d_->cfg_csr_addr = static_cast<uint16_t>(s.csr_addr & 0xFFF);
    d_->cfg_csr_write = s.csr_write ? 1 : 0;
    d_->cfg_csr_wdata = s.csr_wdata;
    d_->cfg_csr_priv = static_cast<uint8_t>(s.csr_priv & 3);
    d_->cfg_csr_vs_off = s.csr_vs_off ? 1 : 0;
    d_->eval();
    d_->clk = 1;
    d_->eval();
    d_->clk = 0;
    d_->eval();

    CfgObs o;
    o.vset_illegal = d_->cfg_vset_illegal != 0;
    o.vset_commit = d_->cfg_vset_commit != 0;
    o.vset_rd_we = d_->cfg_vset_rd_we != 0;
    o.vset_rd_val = d_->cfg_vset_rd_val;
    o.vtype = d_->cfg_vtype;
    o.vl = d_->cfg_vl;
    o.vstart = d_->cfg_vstart;
    o.vxrm = d_->cfg_vxrm;
    o.vxsat = d_->cfg_vxsat;
    o.vcsr = d_->cfg_vcsr;
    o.vlenb = d_->cfg_vlenb;
    o.vlmax = d_->cfg_vlmax;
    o.vill = d_->cfg_vill != 0;
    o.gen = static_cast<int>(d_->cfg_gen);
    o.snap_valid = d_->cfg_snap_valid != 0;
    o.snap_vtype = d_->cfg_snap_vtype;
    o.snap_vl = d_->cfg_snap_vl;
    o.snap_vstart = d_->cfg_snap_vstart;
    o.snap_gen = static_cast<int>(d_->cfg_snap_gen);
    o.replay_ok = d_->cfg_replay_ok != 0;
    o.replay_vtype = d_->cfg_replay_vtype;
    o.replay_vl = d_->cfg_replay_vl;
    o.replay_vstart = d_->cfg_replay_vstart;
    o.exec_illegal = d_->cfg_exec_illegal != 0;
    o.csr_ready = d_->cfg_csr_ready != 0;
    o.csr_illegal = d_->cfg_csr_illegal != 0;
    o.csr_commit = d_->cfg_csr_commit != 0;
    o.csr_rdata = d_->cfg_csr_rdata;
    clk_->Tick();
    return o;
  }

 private:
  Vmosaic_vec_tb* d_;
  ClockDriver* clk_;
};

CfgObs RunVset(Cfg* cfg, int kind, int rd, int rs1, uint64_t avl,
               uint64_t vtype_word, int uimm = 0, bool vs_off = false) {
  CfgStim s;
  s.vset_valid = true;
  s.kind = kind;
  s.rd = rd;
  s.rs1 = rs1;
  s.rs1_val = avl;
  s.rs2_val = vtype_word;
  s.vtypei = vtype_word & 0x7FFull;
  s.uimm = uimm;
  s.vs_off = vs_off;
  return cfg->Cycle(s);
}

CfgObs CsrRead(Cfg* cfg, uint64_t addr, bool vs_off = false) {
  CfgStim s;
  s.csr_valid = true;
  s.csr_addr = addr;
  s.csr_write = false;
  s.csr_vs_off = vs_off;
  return cfg->Cycle(s);
}

CfgObs CsrWrite(Cfg* cfg, uint64_t addr, uint64_t data, bool vs_off = false) {
  CfgStim s;
  s.csr_valid = true;
  s.csr_addr = addr;
  s.csr_write = true;
  s.csr_wdata = data;
  s.csr_vs_off = vs_off;
  return cfg->Cycle(s);
}

struct VsetCounts {
  int avl_bands = 0;
  int vtypes = 0;
};

// Every SEW/LMUL encoding is visited: a supported pair must take the argument
// (rest of vtype zero), an unsupported one must set vill, zero the rest of
// vtype and set vl = 0. Also covers vtypei reserved bits and the read-back of
// vta/vma.
void PhaseVtypeSupport(Cfg* cfg, Reporter* rep, VsetCounts* counts) {
  int legal = 0, illegal = 0;
  for (int vsew = 0; vsew < 8; ++vsew) {
    for (int vlmul = 0; vlmul < 8; ++vlmul) {
      const uint64_t word = Vtypei(vsew, vlmul, 0, 0);
      const CfgObs o = RunVset(cfg, VSETVLI, 5, 6, kAvlMax, word);
      const bool expect = WordSupported(word);
      const std::string name = "vtype-support vsew=" + Dec(vsew) + " vlmul=" + Dec(vlmul);
      rep->Check(o.vset_commit && !o.vset_illegal, name + ": vset did not commit");
      if (expect) {
        rep->Check(!o.vill, name + ": a supported vtype set vill");
        rep->Check(o.vtype == word, name + ": vtype read back as " + mosaic::Hex(o.vtype) +
                                        ", expected " + mosaic::Hex(word));
        rep->Check(o.vl == static_cast<uint64_t>(VlmaxOf(vsew, vlmul)),
                   name + ": vl " + Dec(o.vl) + ", expected VLMAX " +
                       Dec(VlmaxOf(vsew, vlmul)));
        rep->Check(o.vlmax == static_cast<uint64_t>(VlmaxOf(vsew, vlmul)),
                   name + ": VLMAX read back wrong");
        rep->Check(o.vlenb == 16, name + ": vlenb changed");
      } else {
        rep->Check(o.vill, name + ": an unsupported vtype did not set vill");
        rep->Check(o.vtype == kVill, name + ": vill did not zero vtype[62:0]");
        rep->Check(o.vl == 0, name + ": an unsupported vtype left vl != 0");
      }
      expect ? ++legal : ++illegal;
      ++counts->vtypes;
    }
  }
  rep->Check(legal == 22, "vtype-support: " + Dec(legal) + " legal vtypes, expected 22");
  rep->Check(illegal == 42, "vtype-support: " + Dec(illegal) + " illegal vtypes, expected 42");

  // vta/vma are preserved on a legal write and never affect legality.
  const uint64_t ta_word = Vtypei(3, 0, 1, 1);
  const CfgObs ta = RunVset(cfg, VSETVLI, 5, 6, kAvlMax, ta_word);
  rep->Check(!ta.vill && ta.vtype == ta_word, "vtype-support: vta/vma bits preserved");

  // Reserved immediate bits set vill.
  for (int bit : {8, 9, 10}) {
    const CfgObs r = RunVset(cfg, VSETVLI, 5, 6, kAvlMax,
                             Vtypei(0, 0) | (1ull << bit));
    rep->Check(r.vill && r.vtype == kVill,
               "vtype-support: reserved vtypei bit " + Dec(bit) + " did not set vill");
  }

  // A full vtype word from vsetvl with a reserved bit set is unsupported too.
  const CfgObs r = RunVset(cfg, VSETVL, 5, 6, kAvlMax, Vtypei(0, 0) | (1ull << 40));
  rep->Check(r.vill && r.vtype == kVill, "vtype-support: reserved vtype bit 40 did not set vill");
}

// The AVL bands, on several distinct configurations so the check cannot pass by
// hard-coding one VLMAX. The shipped policy is vl = min(AVL, VLMAX); the driver
// asserts the spec bounds and determinism, not that exact value.
void PhaseAvlBands(Cfg* cfg, Reporter* rep, VsetCounts* counts) {
  const int configs[4][2] = {{0, 0}, {0, 3}, {3, 3}, {2, 1}};
  for (const auto& c : configs) {
    const int vsew = c[0], vlmul = c[1];
    const uint64_t vlmax = static_cast<uint64_t>(VlmaxOf(vsew, vlmul));
    const uint64_t word = Vtypei(vsew, vlmul);
    const uint64_t avls[] = {0, 1, vlmax - 1, vlmax, vlmax + 1, 2 * vlmax - 1,
                             2 * vlmax, 2 * vlmax + 1, kAvlMax};
    for (uint64_t avl : avls) {
      const CfgObs o = RunVset(cfg, VSETVLI, 5, 6, avl, word);
      const CfgObs o2 = RunVset(cfg, VSETVLI, 5, 6, avl, word);
      const std::string name = "avl-band vsew=" + Dec(vsew) + " vlmul=" + Dec(vlmul) +
                               " avl=" + mosaic::Hex(avl, 4) + " vlmax=" + Dec(vlmax);
      rep->Check(!o.vill && o.vlmax == vlmax, name + ": configuration not established");
      rep->Check(VlLegal(avl, vlmax, o.vl),
                 name + ": vl " + Dec(o.vl) + " is not a legal choice");
      rep->Check(o2.vl == o.vl, name + ": vl is not deterministic (" + Dec(o.vl) +
                                    " then " + Dec(o2.vl) + ")");
      // A vl read back and used as the AVL gives the same vl.
      const CfgObs o3 = RunVset(cfg, VSETVLI, 5, 6, o.vl, word);
      rep->Check(o3.vl == o.vl, name + ": vl is not idempotent under re-entry");
      ++counts->avl_bands;
    }
  }
}

// The rd/rs1 table: rs1 != x0 uses x[rs1]; rs1 = x0 with rd != x0 uses ~0
// (VLMAX); rs1 = x0 with rd = x0 keeps the current vl (and is reserved -- our
// deterministic answer is vill -- when the new ratio changes VLMAX).
void PhaseRdRs1(Cfg* cfg, Reporter* rep) {
  const uint64_t word = Vtypei(0, 0);  // e8, m1 -> VLMAX 16
  const uint64_t vlmax = 16;

  CfgObs o = RunVset(cfg, VSETVLI, 5, 6, 7, word);
  rep->Check(o.vl == 7 && o.vset_rd_we && o.vset_rd_val == 7,
             "rd-rs1: AVL from x[rs1]");

  o = RunVset(cfg, VSETVLI, 0, 6, 7, word);
  rep->Check(o.vl == 7 && !o.vset_rd_we, "rd-rs1: rd=x0 writes nothing");

  o = RunVset(cfg, VSETVLI, 5, 0, 0, word);
  rep->Check(o.vl == vlmax && o.vset_rd_we && o.vset_rd_val == vlmax,
             "rd-rs1: rs1=x0, rd!=x0 selects VLMAX");

  // Keep the current vl: configure vl = 7, then rs1 = x0 / rd = x0.
  o = RunVset(cfg, VSETVLI, 5, 6, 7, word);
  o = RunVset(cfg, VSETVLI, 0, 0, 0, word);
  rep->Check(!o.vill && o.vl == 7 && !o.vset_rd_we,
             "rd-rs1: rs1=x0, rd=x0 keeps the current vl");

  // Same form with a VLMAX-changing ratio is reserved: the unit sets vill.
  o = RunVset(cfg, VSETVLI, 0, 0, 0, Vtypei(2, 0));  // e32,m1 -> VLMAX 4
  rep->Check(o.vill && o.vl == 0, "rd-rs1: the reserved x0/x0 form sets vill");

  // vsetivli: the AVL is the zero-extended 5-bit immediate.
  o = RunVset(cfg, VSETIVLI, 3, 0, 0, word, /*uimm*/ 5);
  rep->Check(o.vl == 5 && o.vset_rd_we && o.vset_rd_val == 5, "rd-rs1: vsetivli uimm=5");
  o = RunVset(cfg, VSETIVLI, 3, 0, 0, word, /*uimm*/ 31);
  rep->Check(o.vl == vlmax, "rd-rs1: vsetivli uimm=31 clamps to VLMAX");

  // vsetvl takes the vtype from rs2.
  o = RunVset(cfg, VSETVL, 4, 7, 9, Vtypei(1, 1));
  rep->Check(!o.vill && o.vtype == Vtypei(1, 1) && o.vl == 9 && o.vset_rd_val == 9,
             "rd-rs1: vsetvl uses rs2 as vtype and x[rs1] as AVL");
}

// vstart is reset to zero by every committed vector instruction, is writable
// through its CSR, and is *not* modified by the illegal-instruction path.
void PhaseVstart(Cfg* cfg, Reporter* rep) {
  CfgObs o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(0, 0));
  rep->Check(o.vstart == 0, "vstart: a committed vset resets vstart");

  o = CsrWrite(cfg, kCsrVstart, 5);
  rep->Check(!o.csr_illegal && o.csr_commit, "vstart: a software write commits");
  o = CsrRead(cfg, kCsrVstart);
  rep->Check(o.csr_rdata == 5, "vstart: value read back as " + Dec(o.csr_rdata));

  o = CsrWrite(cfg, kCsrVstart, kAvlMax);
  o = CsrRead(cfg, kCsrVstart);
  rep->Check(o.csr_rdata == 0x7F, "vstart: upper bits are not writable (" + mosaic::Hex(o.csr_rdata) + ")");

  o = CsrWrite(cfg, kCsrVstart, 5);
  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(0, 0), 0, /*vs_off*/ true);
  rep->Check(o.vset_illegal && !o.vset_commit, "vstart: VS=Off raises illegal instruction");
  o = CsrRead(cfg, kCsrVstart);
  rep->Check(o.csr_rdata == 5, "vstart: an illegal instruction does not modify vstart");

  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(0, 0));
  rep->Check(o.vstart == 0, "vstart: a following committed vset resets vstart");

  o = CsrWrite(cfg, kCsrVstart, 9);
  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(4, 0));  // reserved vsew -> vill
  rep->Check(o.vill && o.vstart == 0, "vstart: an unsupported vtype still resets vstart");
}

// The permissions and the field layout of the seven unprivileged vector CSRs.
void PhaseCsrPermissions(Cfg* cfg, Reporter* rep) {
  // Configure a known state: e32,m1 with vl = 3.
  RunVset(cfg, VSETVLI, 5, 6, 3, Vtypei(2, 0));

  struct Entry {
    uint64_t addr;
    const char* name;
    bool read_only;
  };
  const Entry entries[] = {
      {kCsrVstart, "vstart", false}, {kCsrVxsat, "vxsat", false},
      {kCsrVxrm, "vxrm", false},     {kCsrVcsr, "vcsr", false},
      {kCsrVl, "vl", true},          {kCsrVtype, "vtype", true},
      {kCsrVlenb, "vlenb", true}};
  for (const Entry& e : entries) {
    const CfgObs r = CsrRead(cfg, e.addr);
    rep->Check(r.csr_ready && !r.csr_illegal, std::string("csr: read ") + e.name + " is legal");
    const CfgObs w = CsrWrite(cfg, e.addr, ~0ull);
    if (e.read_only) {
      rep->Check(w.csr_illegal && !w.csr_commit,
                 std::string("csr: a write to read-only ") + e.name + " is illegal");
    } else {
      rep->Check(!w.csr_illegal && w.csr_commit,
                 std::string("csr: a write to ") + e.name + " is legal");
    }
  }

  // WARL fields.
  CfgObs o = CsrRead(cfg, kCsrVlenb);
  rep->Check(o.csr_rdata == 16, "csr: vlenb reads 16");
  o = CsrRead(cfg, kCsrVl);
  rep->Check(o.csr_rdata == 3, "csr: vl reads the configured length");
  o = CsrRead(cfg, kCsrVtype);
  rep->Check(o.csr_rdata == Vtypei(2, 0), "csr: vtype reads the configured type");

  CsrWrite(cfg, kCsrVxsat, 0);
  CsrWrite(cfg, kCsrVxrm, 0xFF);
  o = CsrRead(cfg, kCsrVxrm);
  rep->Check(o.csr_rdata == 3, "csr: vxrm holds two WARL bits (" + mosaic::Hex(o.csr_rdata) + ")");
  o = CsrRead(cfg, kCsrVcsr);
  rep->Check(o.csr_rdata == 6, "csr: vcsr mirrors vxrm in bits 2:1 (" + mosaic::Hex(o.csr_rdata) + ")");

  CsrWrite(cfg, kCsrVxsat, 1);
  o = CsrRead(cfg, kCsrVxsat);
  rep->Check(o.csr_rdata == 1, "csr: vxsat holds one bit");
  o = CsrRead(cfg, kCsrVcsr);
  rep->Check(o.csr_rdata == 7, "csr: vcsr mirrors vxsat in bit 0 (" + mosaic::Hex(o.csr_rdata) + ")");

  CsrWrite(cfg, kCsrVcsr, 4);
  o = CsrRead(cfg, kCsrVxrm);
  rep->Check(o.csr_rdata == 2, "csr: a vcsr write updates vxrm");
  o = CsrRead(cfg, kCsrVxsat);
  rep->Check(o.csr_rdata == 0, "csr: a vcsr write updates vxsat");

  // VS = Off makes every vector CSR access illegal.
  for (const Entry& e : entries) {
    const CfgObs r = CsrRead(cfg, e.addr, /*vs_off*/ true);
    rep->Check(r.csr_illegal, std::string("csr: ") + e.name + " access with VS=Off is illegal");
  }
  // An address that is none of the seven is illegal.
  const CfgObs unknown = CsrRead(cfg, 0xC23);
  rep->Check(unknown.csr_illegal, "csr: an unknown vector-CSR address is illegal");
}

// The descriptor must be fed the configuration the unit holds, and a replay must
// use the captured snapshot even after the live configuration has moved on.
void PhaseSnapshotReplay(Cfg* cfg, Dut* desc, Reporter* rep) {
  CfgStim idle;
  cfg->Cycle(idle);

  CfgObs o = RunVset(cfg, VSETVLI, 5, 6, 7, Vtypei(0, 0));
  const uint64_t v1 = o.vtype;
  const uint64_t vl1 = o.vl;
  const int g1 = o.gen;
  rep->Check(!o.vill && v1 == Vtypei(0, 0) && vl1 == 7, "snapshot: V1 configured");

  CfgStim cap;
  cap.snap_capture = true;
  const CfgObs c1 = cfg->Cycle(cap);
  rep->Check(c1.snap_valid && c1.snap_vtype == v1 && c1.snap_vl == vl1 &&
                 c1.snap_vstart == 0 && c1.snap_gen == g1,
             "snapshot: the snapshot is not the configuration in force [v=" +
                 mosaic::Hex(c1.snap_vtype) + " vl=" + Dec(c1.snap_vl) + " vs=" +
                 Dec(c1.snap_vstart) + " gen=" + Dec(c1.snap_gen) + "]");

  o = RunVset(cfg, VSETVLI, 5, 6, 3, Vtypei(2, 0));
  rep->Check(!o.vill && o.vtype == Vtypei(2, 0) && o.vl == 3, "snapshot: V2 configured");
  rep->Check(o.gen == g1 + 1, "snapshot: the generation did not advance");

  CfgStim r;
  r.replay_valid = true;
  r.replay_gen = g1;
  const CfgObs r1 = cfg->Cycle(r);
  rep->Check(r1.replay_ok, "replay: the captured generation was refused");
  rep->Check(r1.replay_vtype == v1,
             "replay: a replay used the newer vtype " + mosaic::Hex(r1.replay_vtype) +
                 ", expected the captured " + mosaic::Hex(v1));
  rep->Check(r1.replay_vl == vl1 && r1.replay_vstart == 0,
             "replay: the replay did not serve the captured vl/vstart");

  CfgStim r2;
  r2.replay_valid = true;
  r2.replay_gen = g1 + 50;
  const CfgObs r3 = cfg->Cycle(r2);
  rep->Check(!r3.replay_ok, "replay: an unknown generation was accepted");

  // Feed the descriptor the configuration snapshot the unit hands out.
  Stim a;
  a.alloc_valid = true;
  a.alloc_vtype = o.vtype;
  a.alloc_vl = static_cast<int>(o.vl & 0xFF);
  a.alloc_vstart = 0;
  a.alloc_vd = 4;
  a.alloc_mask_ver = 1;
  a.rob_index = 3;
  a.rob_gen = 2;
  a.uop_index = 1;
  const DescObs dsc = desc->Cycle(a);
  rep->Check(dsc.valid && dsc.vtype == o.vtype && dsc.vl == static_cast<int>(o.vl) &&
                 dsc.vstart == 0,
             "snapshot: the descriptor did not capture the configuration unit's snapshot");
  Stim rel;
  rel.release = true;
  desc->Cycle(rel);
}

// While vill is set a vtype-dependent instruction is illegal; a vtype-free one
// is not. Reset leaves vill set.
void PhaseVillBlocks(Cfg* cfg, Reporter* rep) {
  CfgObs o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(0, 0));
  rep->Check(!o.vill, "vill-blocks: a supported vtype clears vill");
  CfgStim e;
  e.exec_valid = true;
  e.exec_vtype_dep = true;
  CfgObs x = cfg->Cycle(e);
  rep->Check(!x.exec_illegal, "vill-blocks: a vtype-dependent instruction blocked while vill is clear");

  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(4, 0));  // reserved vsew, unsupported
  rep->Check(o.vill && o.vl == 0, "vill-blocks: an unsupported vtype sets vill and vl=0");
  x = cfg->Cycle(e);
  rep->Check(x.exec_illegal, "vill-blocks: a vtype-dependent instruction is not blocked by vill");
  CfgStim e2;
  e2.exec_valid = true;
  e2.exec_vtype_dep = false;
  x = cfg->Cycle(e2);
  rep->Check(!x.exec_illegal, "vill-blocks: a vtype-free instruction was blocked by vill");

  CfgStim r;
  r.rst = true;
  cfg->Cycle(r);
  cfg->Cycle(r);
  o = cfg->Cycle(CfgStim{});
  rep->Check(o.vill, "vill-blocks: reset did not leave vill set");
  x = cfg->Cycle(e);
  rep->Check(x.exec_illegal, "vill-blocks: the reset vill state does not block execution");
}

// No configuration action may change VLEN / vlenb.
void PhaseVlenInvariance(Cfg* cfg, Reporter* rep) {
  for (int vsew = 0; vsew <= 3; ++vsew) {
    for (int vlmul = 0; vlmul < 8; ++vlmul) {
      const uint64_t word = Vtypei(vsew, vlmul);
      if (!WordSupported(word)) continue;
      const CfgObs o = RunVset(cfg, VSETVLI, 5, 6, kAvlMax, word);
      rep->Check(o.vlenb == 16, "vlen-invariance: vlenb changed to " + Dec(o.vlenb));
      rep->Check(o.vlmax <= 128, "vlen-invariance: VLMAX " + Dec(o.vlmax) + " exceeds VLEN");
    }
  }
}

void RunVsetCase(Vmosaic_vec_tb* dut, Dut* desc, ClockDriver* clk, Reporter* rep,
                 VsetCounts* counts) {
  Cfg cfg(dut, clk);
  PhaseAvlBands(&cfg, rep, counts);
  PhaseVtypeSupport(&cfg, rep, counts);
  PhaseRdRs1(&cfg, rep);
  PhaseVstart(&cfg, rep);
  PhaseCsrPermissions(&cfg, rep);
  PhaseSnapshotReplay(&cfg, desc, rep);
  PhaseVillBlocks(&cfg, rep);
  PhaseVlenInvariance(&cfg, rep);
}

// ============================================================================
// CASE=rvv.vtype_layout -- the ratified v1.0 `vtype` field positions.
//
// The specification's layout (vtype-format.adoc of the pinned tag, included at
// L190 of src/v-spec.adoc; the prose at L186-L188 names vill/vma/vta/vsew/
// vlmul) is, for XLEN=64:
//
//   bit 63 vill | bits 62:8 reserved | bit 7 vma | bit 6 vta |
//   bits 5:3 vsew[2:0] | bits 2:0 vlmul[2:0]
//
// Reading `vtype` back and decoding `vsew` at 5:3 is how a program discovers the
// SEW the machine is configured for, and the `vset{i}vl{i}` immediate uses the
// same positions. The case therefore encodes SEW/LMUL/vta/vma at *these*
// positions (never at a position read out of the RTL), drives the word through
// the configuration unit and the descriptor, and requires each unit to decode
// what was encoded and to read the same positions back. An implementation that
// placed `vsew` at 7:5 would answer a different SEW/LMUL -- the defect this
// registered case exists to catch.
// ============================================================================

struct LayoutCounts {
  int words = 0;        // spec-encoded words round-tripped through the register
  int descriptors = 0;  // spec-encoded words decoded by the descriptor
  int writes = 0;       // illegal software writes the URO rule must refuse
};

// The specification's field positions and extractors, named once here so the
// assertions below are written against the manual, not against the RTL.
constexpr int kVsewLo = 3;
constexpr int kVtaPos = 6;
constexpr int kVmaPos = 7;
constexpr uint64_t kLowByte = 0xFFull;

int SpecVsew(uint64_t v) { return static_cast<int>((v >> kVsewLo) & 7ull); }
int SpecVlmul(uint64_t v) { return static_cast<int>(v & 7ull); }
bool SpecVta(uint64_t v) { return ((v >> kVtaPos) & 1ull) != 0; }
bool SpecVma(uint64_t v) { return ((v >> kVmaPos) & 1ull) != 0; }

// Every legal SEW (8/16/32/64) and every legal LMUL, crossed with every vta/vma
// combination, is encoded at the specification's positions, driven through
// `vsetvli`, and read back. The SEW/LMUL the unit decodes is checked through
// VLMAX = VLEN*LMUL/SEW and through the CSR read path, so the check fails if the
// field is read from anywhere but 5:3 and 2:0.
void PhaseLayoutRoundTrip(Cfg* cfg, Reporter* rep, LayoutCounts* counts) {
  for (int vsew = 0; vsew <= 3; ++vsew) {
    for (int vlmul = 0; vlmul < 8; ++vlmul) {
      if (!VlmulValid(vlmul) || (LmulExp(vlmul) + 6 < vsew + kSewLog2Base)) continue;
      for (int ta = 0; ta < 2; ++ta) {
        for (int ma = 0; ma < 2; ++ma) {
          const uint64_t word = Vtypei(vsew, vlmul, ta, ma);
          const uint64_t vlmax = static_cast<uint64_t>(VlmaxOf(vsew, vlmul));
          const std::string name = "vtype-roundtrip vsew=" + Dec(vsew) +
                                   " vlmul=" + Dec(vlmul) + " ta=" + Dec(ta) +
                                   " ma=" + Dec(ma);
          const CfgObs o = RunVset(cfg, VSETVLI, 5, 6, kAvlMax, word);
          rep->Check(o.vset_commit && !o.vset_illegal && !o.vill,
                     name + ": a legal configuration was not accepted");
          // The unit must decode the encoded SEW and LMUL: VLMAX only matches
          // when both fields were read at 5:3 and 2:0.
          rep->Check(o.vlmax == vlmax,
                     name + ": VLMAX " + Dec(o.vlmax) +
                         " does not match the encoded SEW/LMUL (expected " + Dec(vlmax) + ")");
          rep->Check(o.vl == vlmax,
                     name + ": vl " + Dec(o.vl) + " does not match VLMAX " + Dec(vlmax));
          // Read `vtype` back and decode it at the specification's positions.
          rep->Check(o.vtype == word,
                     name + ": vtype read back as " + mosaic::Hex(o.vtype) + ", expected " +
                         mosaic::Hex(word));
          rep->Check(SpecVsew(o.vtype) == vsew, name + ": vsew is not at bits 5:3");
          rep->Check(SpecVlmul(o.vtype) == vlmul, name + ": vlmul is not at bits 2:0");
          rep->Check(SpecVta(o.vtype) == (ta != 0), name + ": vta is not at bit 6");
          rep->Check(SpecVma(o.vtype) == (ma != 0), name + ": vma is not at bit 7");
          rep->Check((o.vtype & ~kLowByte) == 0ull, name + ": bits 62:8 do not read zero");
          // The CSR read path returns the same word.
          const CfgObs r = CsrRead(cfg, kCsrVtype);
          rep->Check(r.csr_rdata == word, name + ": the CSR read-back differs");
          ++counts->words;
        }
      }
    }
  }
}

// The descriptor decodes the same fields for its legality query: a spec-encoded
// word must be decoded as the SEW/LMUL it encodes, and a legal one accepted.
void PhaseLayoutDescriptor(Dut* desc, Reporter* rep, LayoutCounts* counts) {
  for (int vsew = 0; vsew <= 3; ++vsew) {
    for (int vlmul = 0; vlmul < 8; ++vlmul) {
      if (!VlmulValid(vlmul) || (LmulExp(vlmul) + 6 < vsew + kSewLog2Base)) continue;
      for (int ta = 0; ta < 2; ++ta) {
        for (int ma = 0; ma < 2; ++ma) {
          const uint64_t word = Vtypei(vsew, vlmul, ta, ma);
          // opivv with disjoint register groups: legality depends only on vtype.
          const QueryObs q = desc->Query(word, VOP_IVV, 5, 6, 7, 8, false, 4);
          const std::string name = "vtype-layout descriptor vsew=" + Dec(vsew) +
                                   " vlmul=" + Dec(vlmul) + " ta=" + Dec(ta) +
                                   " ma=" + Dec(ma);
          rep->Check(q.vtype_legal, name + ": a legal configuration was rejected");
          rep->Check(q.sew_log2 == vsew + kSewLog2Base, name + ": sew_log2 decoded from the wrong bits");
          rep->Check(q.lmul_exp == LmulExp(vlmul),
                     name + ": lmul_exp decoded from the wrong bits");
          rep->Check(q.elem_count == VlmaxOf(vsew, vlmul),
                     name + ": elem_count does not match the encoded SEW/LMUL");
          ++counts->descriptors;
        }
      }
    }
  }
}

// `vill` for an unsupported configuration; reserved bits honoured; and the URO
// rule -- a software CSR write to vtype is illegal, so software cannot write
// `vill` (or anything else) into the register.
void PhaseLayoutVill(Cfg* cfg, Reporter* rep, LayoutCounts* counts) {
  // vsew = 4 is a reserved encoding (the implemented field values are 0..3):
  // vill set, vtype[62:0] zero, vl = 0.
  const CfgObs bad = RunVset(cfg, VSETVLI, 5, 6, kAvlMax, Vtypei(4, 0, 1, 1));
  rep->Check(bad.vill, "vtype-layout vill: an unsupported vtype did not set vill");
  rep->Check(bad.vtype == kVill,
             "vtype-layout vill: vtype[62:0] not zeroed (" + mosaic::Hex(bad.vtype) + ")");
  rep->Check(bad.vl == 0, "vtype-layout vill: an unsupported vtype left vl != 0");

  // A reserved bit of the vtype argument makes the value unsupported: "all bits
  // of the vtype argument must be considered".
  for (int bit : {8, 30, 62}) {
    const CfgObs r = RunVset(cfg, VSETVL, 5, 6, kAvlMax, Vtypei(0, 0) | (1ull << bit));
    rep->Check(r.vill && r.vtype == kVill,
               "vtype-layout reserved: vsetvl vtype bit " + Dec(bit) + " did not set vill");
  }

  // URO: a software CSR write to vtype is illegal and the register is unchanged.
  const CfgObs good = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(2, 1, 1, 0));
  const uint64_t held = good.vtype;
  rep->Check(!good.vill && held == Vtypei(2, 1, 1, 0), "vtype-layout uro: V1 not configured");
  const CfgObs w = CsrWrite(cfg, kCsrVtype, kVill);
  rep->Check(w.csr_illegal && !w.csr_commit,
             "vtype-layout uro: software set vill through a vtype CSR write");
  const CfgObs r1 = CsrRead(cfg, kCsrVtype);
  rep->Check(r1.csr_rdata == held,
             "vtype-layout uro: a software write changed vtype (" + mosaic::Hex(r1.csr_rdata) +
                 ")");
  const CfgObs w2 = CsrWrite(cfg, kCsrVtype, 0);
  rep->Check(w2.csr_illegal, "vtype-layout uro: a software write of 0 to vtype is not illegal");
  // A software write aimed at a reserved bit is refused, and the reserved field
  // reads zero afterwards, as the specification requires (bits 62:8 read zero).
  const CfgObs wres = CsrWrite(cfg, kCsrVtype, 1ull << 40);
  const CfgObs rres = CsrRead(cfg, kCsrVtype);
  rep->Check(wres.csr_illegal && rres.csr_rdata == held,
             "vtype-layout reserved: a software write of reserved bit 40 was not refused");
  rep->Check((rres.csr_rdata >> 8) == 0ull,
             "vtype-layout reserved: bits 62:8 do not read zero (" +
                 mosaic::Hex(rres.csr_rdata) + ")");
  // While vill is set, software still cannot clear it.
  RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(4, 0));
  const CfgObs w3 = CsrWrite(cfg, kCsrVtype, 0);
  const CfgObs r2 = CsrRead(cfg, kCsrVtype);
  rep->Check(w3.csr_illegal && r2.csr_rdata == kVill,
             "vtype-layout uro: software cleared vill (" + mosaic::Hex(r2.csr_rdata) + ")");
  counts->writes += 3;
}

void RunVtypeLayoutCase(Vmosaic_vec_tb* dut, Dut* desc, ClockDriver* clk, Reporter* rep,
                        LayoutCounts* counts) {
  Cfg cfg(dut, clk);
  PhaseLayoutRoundTrip(&cfg, rep, counts);
  PhaseLayoutDescriptor(desc, rep, counts);
  PhaseLayoutVill(&cfg, rep, counts);
}

}  // namespace


// ============================================================================
// I-054 -- the phase set for CASE=rvv.integer_mask_permute
// ============================================================================
// ============================================================================
// I-054 -- vector integer, mask, permute and reduction
//           (CASE=rvv.integer_mask_permute).
//
// The expectation is computed here, element by element, from the V
// specification's rules: `OracleElem` is written from the operation table in
// src/v-spec.adoc (fixpoint rounding from the vxrm table, saturation at the
// signed/unsigned boundary, widening/narrowing at the source element's own
// width), not from a second copy of the RTL's expressions. The register file
// is modelled as 32 x 128-bit values and addressed with the same
// (base, element, SEW, LMUL) rule the VRF implements, so an operation's
// *element order* can be checked and not merely its arithmetic.
//
// Coverage is itself a check: every family the case claims to exercise must be
// shown to have run at every element width and every LMUL in scope, and the
// matrix is asserted to be complete at the end.
// ============================================================================

enum : int {
  VF_ADDSUB = 0, VF_WIDE = 1, VF_MUL = 2, VF_MULW = 3, VF_SHIFT = 4,
  VF_NARROW = 5, VF_LOGIC = 6, VF_MINMAX = 7, VF_CMP = 8, VF_SAT = 9,
  VF_MASKLOG = 10, VF_MASKPFX = 11, VF_SLIDE = 12, VF_GATHER = 13,
  VF_COMPRESS = 14, VF_REDUCE = 15, VF_REDWIDE = 16, VF_COUNT = 17
};
const uint64_t kAllCaps = (1ull << VF_COUNT) - 1ull;

constexpr int kFormVv = 0, kFormVx = 1;

const char* VecFamilyName(int f) {
  switch (f) {
    case VF_ADDSUB: return "addsub";
    case VF_WIDE: return "wide";
    case VF_MUL: return "mul";
    case VF_MULW: return "mulw";
    case VF_SHIFT: return "shift";
    case VF_NARROW: return "narrow";
    case VF_LOGIC: return "logic";
    case VF_MINMAX: return "minmax";
    case VF_CMP: return "cmp";
    case VF_SAT: return "sat";
    case VF_MASKLOG: return "masklog";
    case VF_MASKPFX: return "maskpfx";
    case VF_SLIDE: return "slide";
    case VF_GATHER: return "gather";
    case VF_COMPRESS: return "compress";
    case VF_REDUCE: return "reduce";
    case VF_REDWIDE: return "redwide";
    default: return "?";
  }
}

int VecFamilyOps(int f) {
  switch (f) {
    case VF_ADDSUB: return 3;
    case VF_WIDE: return 4;
    case VF_MUL: return 4;
    case VF_MULW: return 3;
    case VF_SHIFT: return 3;
    case VF_NARROW: return 4;
    case VF_LOGIC: return 4;
    case VF_MINMAX: return 4;
    case VF_CMP: return 8;
    case VF_SAT: return 8;
    case VF_MASKLOG: return 8;
    case VF_MASKPFX: return 3;
    case VF_SLIDE: return 4;
    case VF_GATHER: return 2;
    case VF_COMPRESS: return 1;
    case VF_REDUCE: return 8;
    case VF_REDWIDE: return 2;
    default: return 0;
  }
}

// ------------------------------------------------------------------ width utils
uint64_t MaskW(int w) { return w >= 64 ? ~0ull : ((1ull << w) - 1ull); }

using u128 = unsigned __int128;
using i128 = __int128;

i128 SxW(uint64_t v, int w) {
  uint64_t m = MaskW(w);
  u128 t = static_cast<u128>(v & m);
  if (w > 0 && w < 128 && ((t >> (w - 1)) & 1u) != 0) t |= (~static_cast<u128>(0)) << w;
  return static_cast<i128>(t);
}

// The vxrm table: r depends on the rounded-off bits of the pre-rounding value.
u128 RndUn(u128 v, int d, int rm) {
  if (d <= 0) return v;
  u128 base = v >> d;
  u128 below = v & ((static_cast<u128>(1) << d) - 1);
  bool vd = ((v >> d) & 1u) != 0;
  bool vdm1 = ((v >> (d - 1)) & 1u) != 0;
  u128 below2 = d >= 2 ? (v & ((static_cast<u128>(1) << (d - 1)) - 1)) : 0;
  u128 r = 0;
  switch (rm) {
    case 0: r = vdm1 ? 1 : 0; break;
    case 1: r = (vdm1 && (below2 != 0 || vd)) ? 1 : 0; break;
    case 2: r = 0; break;
    default: r = (!vd && below != 0) ? 1 : 0; break;
  }
  return base + r;
}

i128 RndSg(i128 v, int d, int rm) {
  if (d <= 0) return v;
  i128 base = v >> d;
  u128 uv = static_cast<u128>(v);
  u128 below = uv & ((static_cast<u128>(1) << d) - 1);
  bool vd = ((uv >> d) & 1u) != 0;
  bool vdm1 = ((uv >> (d - 1)) & 1u) != 0;
  u128 below2 = d >= 2 ? (uv & ((static_cast<u128>(1) << (d - 1)) - 1)) : 0;
  i128 r = 0;
  switch (rm) {
    case 0: r = vdm1 ? 1 : 0; break;
    case 1: r = (vdm1 && (below2 != 0 || vd)) ? 1 : 0; break;
    case 2: r = 0; break;
    default: r = (!vd && below != 0) ? 1 : 0; break;
  }
  return base + r;
}

// ---------------------------------------------------------------- the oracle
struct ElemVal {
  uint64_t result = 0;
  bool mres = false;
  bool sat = false;
  bool pfx = false;
};

// One destination element, computed from the operation's rule. `vs1` is the
// vector second operand; `scalar` is used for the .vx/.vi forms.
ElemVal OracleElem(int fam, int op, int sew, int form, uint64_t vs2, uint64_t vs1,
                   uint64_t scalar, uint64_t acc, int vxrm, int index, bool pfx_in) {
  ElemVal o;
  uint64_t m = MaskW(sew);
  uint64_t a = vs2 & m;
  uint64_t b = (form == kFormVv) ? (vs1 & m) : (scalar & m);
  int sh = 0;
  switch (fam) {
    case VF_ADDSUB:
      if (op == 0) o.result = (a + b) & m;
      else if (op == 1) o.result = (a - b) & m;
      else o.result = (b - a) & m;                      // vrsub: scalar - vs2
      break;
    case VF_WIDE: {
      bool sgn = (op == 1 || op == 3);
      u128 av = sgn ? static_cast<u128>(SxW(a, sew)) : static_cast<u128>(a);
      u128 bv = sgn ? static_cast<u128>(SxW(b, sew)) : static_cast<u128>(b);
      u128 r = (op == 0 || op == 1) ? (av + bv) : (av - bv);
      o.result = static_cast<uint64_t>(r) & MaskW(2 * sew);
      break;
    }
    case VF_MUL:
      if (op == 0) o.result = (a * b) & m;
      else if (op == 1) o.result = static_cast<uint64_t>((SxW(a, sew) * SxW(b, sew)) >> sew) & m;
      else if (op == 2) o.result = static_cast<uint64_t>((static_cast<u128>(a) * b) >> sew) & m;
      else o.result = static_cast<uint64_t>((SxW(a, sew) * static_cast<i128>(b)) >> sew) & m;
      break;
    case VF_MULW: {
      u128 r;
      if (op == 0) r = static_cast<u128>(a) * b;
      else if (op == 1) r = static_cast<u128>(SxW(a, sew) * static_cast<i128>(b));
      else r = static_cast<u128>(SxW(a, sew) * SxW(b, sew));
      o.result = static_cast<uint64_t>(r) & MaskW(2 * sew);
      break;
    }
    case VF_SHIFT:
      sh = static_cast<int>(b) & (sew - 1);
      if (op == 0) o.result = (a << sh) & m;
      else if (op == 1) o.result = (a >> sh) & m;
      else o.result = static_cast<uint64_t>(SxW(a, sew) >> sh) & m;
      break;
    case VF_NARROW: {
      sh = static_cast<int>(b) & (2 * sew - 1);
      if (op == 0) o.result = static_cast<uint64_t>(static_cast<u128>(a) >> sh) & m;
      else if (op == 1) o.result = static_cast<uint64_t>(SxW(a, 2 * sew) >> sh) & m;
      else if (op == 2) {
        u128 r = RndUn(a, sh, vxrm);
        if (r > m) { o.sat = true; o.result = m; }
        else o.result = static_cast<uint64_t>(r) & m;
      } else {
        i128 r = RndSg(SxW(a, 2 * sew), sh, vxrm);
        i128 hi = (static_cast<i128>(1) << (sew - 1)) - 1;
        i128 lo = -(static_cast<i128>(1) << (sew - 1));
        if (r > hi) { o.sat = true; o.result = static_cast<uint64_t>(hi); }
        else if (r < lo) { o.sat = true; o.result = static_cast<uint64_t>(lo) & m; }
        else o.result = static_cast<uint64_t>(r) & m;
      }
      break;
    }
    case VF_LOGIC:
      if (op == 0) o.result = a & b;
      else if (op == 1) o.result = a | b;
      else if (op == 2) o.result = a ^ b;
      else o.result = (~a) & m;
      break;
    case VF_MINMAX: {
      bool sgn = (op == 1 || op == 3);
      bool gt = (op == 2 || op == 3);
      bool take_a;
      if (sgn) take_a = gt ? (SxW(a, sew) >= SxW(b, sew)) : (SxW(a, sew) <= SxW(b, sew));
      else take_a = gt ? (a >= b) : (a <= b);
      o.result = take_a ? a : b;
      break;
    }
    case VF_CMP:
      switch (op) {
        case 0: o.mres = (a == b); break;
        case 1: o.mres = (a != b); break;
        case 2: o.mres = (a < b); break;
        case 3: o.mres = (SxW(a, sew) < SxW(b, sew)); break;
        case 4: o.mres = (a <= b); break;
        case 5: o.mres = (SxW(a, sew) <= SxW(b, sew)); break;
        case 6: o.mres = (a > b); break;
        default: o.mres = (SxW(a, sew) > SxW(b, sew)); break;
      }
      break;
    case VF_SAT: {
      i128 hi = (static_cast<i128>(1) << (sew - 1)) - 1;
      i128 lo = -(static_cast<i128>(1) << (sew - 1));
      if (op == 0) {                       // vsaddu
        u128 r = static_cast<u128>(a) + b;
        if (r > m) { o.sat = true; o.result = m; } else o.result = static_cast<uint64_t>(r);
      } else if (op == 1) {                // vsadd
        i128 r = SxW(a, sew) + SxW(b, sew);
        if (r > hi) { o.sat = true; o.result = static_cast<uint64_t>(hi); }
        else if (r < lo) { o.sat = true; o.result = static_cast<uint64_t>(lo) & m; }
        else o.result = static_cast<uint64_t>(r) & m;
      } else if (op == 2) {                // vssubu
        if (a < b) { o.sat = true; o.result = 0; } else o.result = (a - b) & m;
      } else if (op == 3) {                // vssub
        i128 r = SxW(a, sew) - SxW(b, sew);
        if (r > hi) { o.sat = true; o.result = static_cast<uint64_t>(hi); }
        else if (r < lo) { o.sat = true; o.result = static_cast<uint64_t>(lo) & m; }
        else o.result = static_cast<uint64_t>(r) & m;
      } else if (op == 4) {                // vaaddu
        o.result = static_cast<uint64_t>(RndUn(static_cast<u128>(a) + b, 1, vxrm)) & m;
      } else if (op == 5) {                // vaadd
        o.result = static_cast<uint64_t>(
            RndSg(SxW(a, sew) + SxW(b, sew), 1, vxrm)) & m;
      } else if (op == 6) {                // vasubu
        o.result = static_cast<uint64_t>(RndUn(static_cast<u128>(a) - b, 1, vxrm)) & m;
      } else {                             // vasub
        o.result = static_cast<uint64_t>(
            RndSg(SxW(a, sew) - SxW(b, sew), 1, vxrm)) & m;
      }
      break;
    }
    case VF_MASKLOG: {
      // The shared model (sim/unit/mask_prefix_ref.h): one statement of the
      // eight mask-register logical rules, used by the unit lane and by the
      // core-level case alike.
      const bool bb = ((vs2 >> (index & 7)) & 1u) != 0;
      const bool aa = ((vs1 >> (index & 7)) & 1u) != 0;
      o.mres = mosaic_maskpfx::MaskLogElem(op, bb, aa);
      break;
    }
    case VF_MASKPFX: {
      // The specification's three rules, stated once in the shared model
      // (sim/unit/mask_prefix_ref.h).  With `pfx_in` the OR of the source bits
      // strictly before element `index`:
      //   vmsbf[i] = 1 iff no set bit at or before i  (all-ones if none)
      //   vmsif[i] = 1 iff no set bit strictly before i (all-ones if none)
      //   vmsof[i] = 1 iff source bit i is the first set bit
      // An all-zero active source is therefore all-ones for vmsbf and vmsif,
      // and all-zeros for vmsof -- the asymmetry this case encodes.
      const bool bb = ((vs2 >> (index & 7)) & 1u) != 0;
      o.mres = mosaic_maskpfx::Elem(op, bb, pfx_in, &o.pfx);
      break;
    }
    case VF_REDUCE: {
      // one fold of the ascending chain: acc op vs2[i]
      switch (op) {
        case 0: o.result = (acc + a) & m; break;                        // vredsum
        case 1: o.result = (acc >= a) ? acc : a; break;                 // vredmaxu
        case 2: o.result = (SxW(acc, sew) >= SxW(a, sew)) ? acc : a; break;
        case 3: o.result = (acc <= a) ? acc : a; break;                 // vredminu
        case 4: o.result = (SxW(acc, sew) <= SxW(a, sew)) ? acc : a; break;
        case 5: o.result = (acc & a) & m; break;                        // vredand
        case 6: o.result = (acc | a) & m; break;                        // vredor
        default: o.result = (acc ^ a) & m; break;                       // vredxor
      }
      break;
    }
    case VF_REDWIDE: {
      // 2*SEW accumulator, SEW-wide source
      if (op == 0) {
        o.result = static_cast<uint64_t>(static_cast<u128>(acc & MaskW(2 * sew)) +
                                         static_cast<u128>(a)) & MaskW(2 * sew);
      } else {
        i128 sum = static_cast<i128>(SxW(acc, 2 * sew)) + SxW(a, sew);
        o.result = static_cast<uint64_t>(static_cast<u128>(sum)) & MaskW(2 * sew);
      }
      break;
    }
    default:
      break;
  }
  return o;
}

// --------------------------------------------------------------- host model
struct HostVrf {
  u128 r[32];
  HostVrf() { for (int i = 0; i < 32; ++i) r[i] = 0; }
};

int HostGrpBase(int base, int lmul) {
  int grp = lmul >= 0 ? (1 << lmul) : 1;
  return base & ~(grp - 1);
}

uint64_t HostGet(const HostVrf& vf, int base, int elem, int sew_l, int lmul) {
  int sew = 1 << sew_l;
  int bit_off = elem * sew;
  int reg = HostGrpBase(base, lmul) + bit_off / 128;

  int bit = bit_off % 128;
  u128 m = (static_cast<u128>(1) << sew) - 1;
  return static_cast<uint64_t>((vf.r[reg] >> bit) & m);
}

void HostSet(HostVrf& vf, int base, int elem, int sew_l, int lmul, uint64_t val) {
  int sew = 1 << sew_l;
  int bit_off = elem * sew;
  int reg = HostGrpBase(base, lmul) + bit_off / 128;

  int bit = bit_off % 128;
  u128 m = (static_cast<u128>(1) << sew) - 1;
  vf.r[reg] = (vf.r[reg] & ~(m << bit)) | ((static_cast<u128>(val) & m) << bit);
}

// ------------------------------------------------------------- the harness

struct VecStim {
  bool mem_owner = true;
  bool mem_rd_valid = false;
  int mem_rd_base = 0, mem_rd_elem = 0, mem_rd_sew = 0, mem_rd_lmul = 0;
  uint64_t mem_rd_tag = 0x5151;
  bool mem_wr_valid = false;
  int mem_wr_base = 0, mem_wr_elem = 0, mem_wr_sew = 0, mem_wr_lmul = 0;
  uint64_t mem_wr_data = 0;

  bool alu_exec_valid = false;
  int alu_family = 0;
  int alu_op = 0;
  int alu_form = 0;
  int alu_vd = 0, alu_vs1 = 0, alu_vs2 = 0;
  uint64_t alu_scalar = 0;
  bool alu_mask_en = false;
  uint64_t alu_caps = kAllCaps;

  bool el_valid = false;
  int el_family = 0, el_op = 0, el_form = 0;
  uint64_t el_vs2 = 0, el_vs1 = 0, el_acc = 0, el_scalar = 0;
  bool el_mask = false;
  int el_index = 0;
  bool el_pfx = false;
};

struct VecObs {
  bool mem_rd_gnt = false;
  bool mem_rd_rsp_valid = false;
  uint64_t mem_rd_rsp_data = 0;
  uint64_t mem_rd_rsp_tag = 0;
  bool mem_wr_gnt = false;

  bool alu_busy = false, alu_done = false, alu_illegal = false, alu_trap = false;
  bool alu_sat = false;
  int alu_trap_elem = 0, alu_elems = 0, alu_cur = 0;
  uint64_t alu_acc = 0;
  bool trace_valid = false;
  int trace_elem = 0;
  int src_rd_ctr = 0;

  uint64_t el_result = 0;
  bool el_mres = false, el_sat = false, el_illegal = false, el_trap = false;
  bool el_access = false, el_write = false, el_pfx = false;
  int el_rd2 = 0;

  int rd_gnt_ctr = 0, rd_bad_ctr = 0, wr_gnt_ctr = 0, rd_latency = 0;
  int rows = 0, banks = 0;
};

class Vec {
 public:
  Vec(Vmosaic_vec_tb* d, ClockDriver* clk) : d_(d), clk_(clk) {}

  VecObs Cycle(const VecStim& s) {
    d_->rst = 0;
    // the configuration unit is quiescent while the ALU runs
    d_->cfg_vset_valid = 0;
    d_->cfg_snap_capture = 0;
    d_->cfg_replay_valid = 0;
    d_->cfg_exec_valid = 0;
    d_->cfg_csr_valid = 0;

    d_->mem_owner_i = s.mem_owner ? 1 : 0;
    d_->mem_rd_valid_i = s.mem_rd_valid ? 1 : 0;
    d_->mem_rd_base_i = static_cast<uint8_t>(s.mem_rd_base & 0x1F);
    d_->mem_rd_elem_i = static_cast<uint8_t>(s.mem_rd_elem & 0x7F);
    d_->mem_rd_sew_i = static_cast<uint8_t>(s.mem_rd_sew & 0x7);
    d_->mem_rd_lmul_i = static_cast<uint8_t>(s.mem_rd_lmul & 0xF);
    d_->mem_rd_tag_i = static_cast<uint16_t>(s.mem_rd_tag);
    d_->mem_wr_valid_i = s.mem_wr_valid ? 1 : 0;
    d_->mem_wr_base_i = static_cast<uint8_t>(s.mem_wr_base & 0x1F);
    d_->mem_wr_elem_i = static_cast<uint8_t>(s.mem_wr_elem & 0x7F);
    d_->mem_wr_sew_i = static_cast<uint8_t>(s.mem_wr_sew & 0x7);
    d_->mem_wr_lmul_i = static_cast<uint8_t>(s.mem_wr_lmul & 0xF);
    d_->mem_wr_data_i = s.mem_wr_data;

    d_->alu_caps_i = static_cast<uint32_t>(s.alu_caps & 0x1FFFFull);
    d_->alu_exec_valid_i = s.alu_exec_valid ? 1 : 0;
    d_->alu_family_i = static_cast<uint8_t>(s.alu_family & 0x1F);
    d_->alu_op_i = static_cast<uint8_t>(s.alu_op & 0xF);
    d_->alu_form_i = static_cast<uint8_t>(s.alu_form & 0x3);
    d_->alu_vd_i = static_cast<uint8_t>(s.alu_vd & 0x1F);
    d_->alu_vs1_i = static_cast<uint8_t>(s.alu_vs1 & 0x1F);
    d_->alu_vs2_i = static_cast<uint8_t>(s.alu_vs2 & 0x1F);
    d_->alu_scalar_i = s.alu_scalar;
    d_->alu_mask_en_i = s.alu_mask_en ? 1 : 0;

    d_->el_valid_i = s.el_valid ? 1 : 0;
    d_->el_family_i = static_cast<uint8_t>(s.el_family & 0x1F);
    d_->el_op_i = static_cast<uint8_t>(s.el_op & 0xF);
    d_->el_form_i = static_cast<uint8_t>(s.el_form & 0x3);
    d_->el_vs2_i = s.el_vs2;
    d_->el_vs1_i = s.el_vs1;
    d_->el_acc_i = s.el_acc;
    d_->el_mask_i = s.el_mask ? 1 : 0;
    d_->el_scalar_i = s.el_scalar;
    d_->el_index_i = static_cast<uint8_t>(s.el_index & 0xFF);
    d_->el_pfx_i = s.el_pfx ? 1 : 0;

    d_->eval();
    d_->clk = 1;
    d_->eval();
    d_->clk = 0;
    d_->eval();

    VecObs o;
    o.mem_rd_gnt = d_->mem_rd_gnt_o != 0;
    o.mem_rd_rsp_valid = d_->mem_rd_rsp_valid_o != 0;
    o.mem_rd_rsp_data = d_->mem_rd_rsp_data_o;
    o.mem_rd_rsp_tag = d_->mem_rd_rsp_tag_o;
    o.mem_wr_gnt = d_->mem_wr_gnt_o != 0;

    o.alu_busy = d_->alu_busy_o != 0;
    o.alu_done = d_->alu_done_o != 0;
    o.alu_illegal = d_->alu_illegal_o != 0;
    o.alu_trap = d_->alu_trap_o != 0;
    o.alu_trap_elem = static_cast<int>(d_->alu_trap_elem_o);
    o.alu_sat = d_->alu_sat_o != 0;
    o.alu_elems = static_cast<int>(d_->alu_elems_o);
    o.alu_cur = static_cast<int>(d_->alu_cur_o);
    o.alu_acc = d_->alu_acc_o;
    o.trace_valid = d_->alu_trace_valid_o != 0;
    o.trace_elem = static_cast<int>(d_->alu_trace_elem_o);
    o.src_rd_ctr = static_cast<int>(d_->alu_src_rd_ctr_o);

    o.el_result = d_->el_result_o;
    o.el_mres = d_->el_mres_o != 0;
    o.el_sat = d_->el_sat_o != 0;
    o.el_illegal = d_->el_illegal_o != 0;
    o.el_trap = d_->el_trap_o != 0;
    o.el_access = d_->el_access_o != 0;
    o.el_write = d_->el_write_o != 0;
    o.el_pfx = d_->el_pfx_o != 0;
    o.el_rd2 = static_cast<int>(d_->el_rd2_o);

    o.rd_gnt_ctr = static_cast<int>(d_->vrf_rd_gnt_ctr_o);
    o.rd_bad_ctr = static_cast<int>(d_->vrf_rd_bad_ctr_o);
    o.wr_gnt_ctr = static_cast<int>(d_->vrf_wr_gnt_ctr_o);
    o.rd_latency = static_cast<int>(d_->vrf_rd_latency_o);
    o.rows = static_cast<int>(d_->vrf_rows_o);
    o.banks = static_cast<int>(d_->vrf_banks_o);

    clk_->Tick();
    return o;
  }

  // one memory read; the response is one cycle after the grant
  uint64_t MemRead(int base, int elem, int sew_l, int lmul, bool* ok = nullptr) {
    VecStim s;
    s.mem_owner = true;
    s.mem_rd_valid = true;
    s.mem_rd_base = base; s.mem_rd_elem = elem;
    s.mem_rd_sew = sew_l; s.mem_rd_lmul = lmul;
    VecObs o = Cycle(s);
    int guard = 0;
    while (!o.mem_rd_gnt && ++guard < 16) o = Cycle(s);
    VecStim w;
    w.mem_owner = true;
    guard = 0;
    while (!o.mem_rd_rsp_valid && ++guard < 16) o = Cycle(w);
    if (ok != nullptr) *ok = (guard < 16);
    return o.mem_rd_rsp_data;
  }

  void MemWrite(int base, int elem, int sew_l, int lmul, uint64_t data) {
    VecStim s;
    s.mem_owner = true;
    s.mem_wr_valid = true;
    s.mem_wr_base = base; s.mem_wr_elem = elem;
    s.mem_wr_sew = sew_l; s.mem_wr_lmul = lmul;
    s.mem_wr_data = data;
    VecObs o = Cycle(s);
    int guard = 0;
    while (!o.mem_wr_gnt && ++guard < 16) o = Cycle(s);
  }

  // Prime one source-element through the VRF and the host model together.
  void Prime(HostVrf& vf, int base, int elem, int sew_l, int lmul, uint64_t val) {
    MemWrite(base, elem, sew_l, lmul, val);
    HostSet(vf, base, elem, sew_l, lmul, val);
  }

  uint64_t Peek(int base, int elem, int sew_l, int lmul) {
    return MemRead(base, elem, sew_l, lmul);
  }

  VecObs RunPacket(int family, int op, int form, int vd, int vs1, int vs2,
                   uint64_t scalar, bool mask_en, uint64_t caps,
                   std::vector<int>* trace = nullptr) {
    VecStim s;
    s.mem_owner = false;
    s.alu_exec_valid = true;
    s.alu_family = family;
    s.alu_op = op;
    s.alu_form = form;
    s.alu_vd = vd;
    s.alu_vs1 = vs1;
    s.alu_vs2 = vs2;
    s.alu_scalar = scalar;
    s.alu_mask_en = mask_en;
    s.alu_caps = caps;
    VecObs o = Cycle(s);

    VecStim idle;
    idle.mem_owner = false;
    // The capability mask is read while the packet is in flight, so the idle
    // stimulus must carry the same one as the launch.
    idle.alu_caps = caps;
    int guard = 0;
    while (!o.alu_done && ++guard < 40000) {
      if (trace != nullptr && o.trace_valid) trace->push_back(o.trace_elem);
      o = Cycle(idle);
    }
    if (trace != nullptr && o.trace_valid) trace->push_back(o.trace_elem);
    return o;
  }

  int RdGnt() { return static_cast<int>(d_->vrf_rd_gnt_ctr_o); }
  int WrGnt() { return static_cast<int>(d_->vrf_wr_gnt_ctr_o); }
  int RdBad() { return static_cast<int>(d_->vrf_rd_bad_ctr_o); }

 private:
  Vmosaic_vec_tb* d_;
  ClockDriver* clk_;
};

int LmulExpOf(int vlmul) {
  switch (vlmul) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 5: return -3;
    case 6: return -2;
    case 7: return -1;
    default: return 0;
  }
}
int VlmulOfExp(int e) {
  switch (e) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case -3: return 5;
    case -2: return 6;
    case -1: return 7;
    default: return 0;
  }
}

const int kLmulExps[7] = {-3, -2, -1, 0, 1, 2, 3};

int VlmaxOf2(int sew_l, int lmul_e) {
  int e = 7 + lmul_e - sew_l;
  return e < 0 ? 0 : (1 << e);
}

// element width and LMUL the *source(s)* and *destination* of a family use
void FamilyWidths(int fam, int sew_l, int lmul_e, int* s1w, int* s1l, int* s2w,
                  int* s2l, int* dw, int* dl) {
  *s1w = sew_l; *s1l = lmul_e; *s2w = sew_l; *s2l = lmul_e;
  *dw = sew_l; *dl = lmul_e;
  if (fam == VF_CMP || fam == VF_MASKLOG || fam == VF_MASKPFX) {
    *dw = 3; *dl = 0;
  }
  if (fam == VF_MASKLOG || fam == VF_MASKPFX) { *s2w = 3; *s2l = 0; }
  if (fam == VF_MASKLOG) { *s1w = 3; *s1l = 0; }
  if (fam == VF_COMPRESS) { *s1w = 3; *s1l = 0; }
  if (fam == VF_NARROW) { *s2w = sew_l + 1; *s2l = lmul_e + 1; }
  if (fam == VF_WIDE || fam == VF_MULW || fam == VF_REDWIDE) { *dw = sew_l + 1; *dl = lmul_e + 1; }
}

// A widening or narrowing form doubles the effective LMUL, so LMUL=8 would
// leave the [1/8, 8] range the descriptor admits. Those cells are not in
// scope, exactly as the legality matrix has them.
bool FamilySupportsLmul(int fam, int lmul_e) {
  if (fam == VF_WIDE || fam == VF_MULW || fam == VF_NARROW || fam == VF_REDWIDE) {
    return lmul_e <= 2;
  }
  return true;
}

bool FamilySupportsSew(int fam, int sew) {
  if (fam == VF_WIDE || fam == VF_MULW || fam == VF_NARROW || fam == VF_REDWIDE) {
    return sew <= 32;
  }
  return true;
}

// ------------------------------------------------------------------- coverage
struct Coverage {
  bool cell[VF_COUNT][4][7] = {};
  int  cells = 0;
};

// Configure the I-052 unit and capture the snapshot the ALU executes from.
void ConfigureVec(Cfg* cfg, int vsew, int vlmul, int vta, int vma, uint64_t avl) {
  (void)RunVset(cfg, VSETVLI, 5, 6, avl, Vtypei(vsew, vlmul, vta, vma));
  CfgStim cap;
  cap.snap_capture = true;
  cfg->Cycle(cap);
}

// The same, but with a non-zero `vstart` in the captured snapshot. `vstart` is
// written through its CSR *after* the vset (which would have reset it) and
// before the capture, so the ALU sees an instruction that names element
// `vstart` as its first. This is exactly the state the mask-prefix rule is
// defined against.
void ConfigureVecVstart(Cfg* cfg, int vsew, int vlmul, int vta, int vma, uint64_t avl,
                        uint64_t vstart) {
  (void)RunVset(cfg, VSETVLI, 5, 6, avl, Vtypei(vsew, vlmul, vta, vma));
  if (vstart != 0) (void)CsrWrite(cfg, kCsrVstart, vstart);
  CfgStim cap;
  cap.snap_capture = true;
  cfg->Cycle(cap);
}

uint64_t Pat(int seed) {
  uint64_t x = static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 29;
  return x;
}

bool MaskBit(const HostVrf& vf, int i) {
  return ((HostGet(vf, 0, i / 8, 3, 0) >> (i % 8)) & 1u) != 0;
}

void BoundaryValues(int sew, std::vector<uint64_t>* out) {
  uint64_t m = MaskW(sew);
  uint64_t sign = 1ull << (sew - 1);
  out->clear();
  out->push_back(0);
  out->push_back(1);
  out->push_back(m);
  out->push_back(m - 1);
  out->push_back(sign);
  out->push_back(sign - 1);
  out->push_back(sign + 1);
  out->push_back(0xAAAA5555AAAA5555ull & m);
}

// ------------------------------------------------------- the element lane
void PhaseElementLane(Cfg* cfg, Vec* vec, Reporter* rep) {
  const int fams[] = {VF_ADDSUB, VF_WIDE, VF_MUL, VF_MULW, VF_SHIFT,
                      VF_NARROW, VF_LOGIC, VF_MINMAX, VF_CMP, VF_SAT};
  for (int fam : fams) {
    for (int sew_l = 3; sew_l <= 6; ++sew_l) {
      int sew = 1 << sew_l;
      if (!FamilySupportsSew(fam, sew)) continue;
      ConfigureVec(cfg, SewField(sew_l), 0, 0, 0, 64);
      (void)CsrWrite(cfg, kCsrVxrm, 2);
      std::vector<uint64_t> vals;
      BoundaryValues(sew, &vals);
      const int rms[2] = {2, 0};
      int nrms = (fam == VF_SAT) ? 2 : 1;
      for (int op = 0; op < VecFamilyOps(fam); ++op) {
        for (int form = kFormVv; form <= kFormVx; ++form) {
          for (int ri = 0; ri < nrms; ++ri) {
            (void)CsrWrite(cfg, kCsrVxrm, rms[ri]);
            for (size_t xi = 0; xi < vals.size(); ++xi) {
              for (size_t yi = 0; yi < vals.size(); ++yi) {
                uint64_t vs2 = vals[xi];
                uint64_t vs1 = vals[yi];
                VecStim s;
                s.el_valid = true;
                s.el_family = fam;
                s.el_op = op;
                s.el_form = form;
                s.el_vs2 = vs2;
                s.el_vs1 = vs1;
                s.el_scalar = vs1;
                s.el_index = 0;
                s.el_mask = true;
                VecObs o = vec->Cycle(s);
                ElemVal e = OracleElem(fam, op, sew, form, vs2, vs1, vs1, 0, rms[ri], 0, false);
                const char* tag = (fam == VF_SAT) ? "sat-boundary" :
                                  ((fam == VF_WIDE || fam == VF_MULW || fam == VF_NARROW) ?
                                       "wide-width" : "element-lane");
                std::string name = std::string(tag) + " " + VecFamilyName(fam) + " op" +
                                   Dec(op) + " sew" + Dec(sew) + " form" + Dec(form) +
                                   " vxrm" + Dec(rms[ri]);
                if (fam == VF_CMP) {
                  rep->Check(o.el_mres == e.mres,
                             name + ": mask " + Dec(o.el_mres) + " expected " + Dec(e.mres));
                } else {
                  uint64_t cm = (fam == VF_WIDE || fam == VF_MULW || fam == VF_REDWIDE)
                                    ? MaskW(2 * sew) : MaskW(sew);
                  rep->Check((o.el_result & cm) == (e.result & cm),
                             name + ": got " + mosaic::Hex(o.el_result & cm, 16) +
                                 " expected " + mosaic::Hex(e.result & cm, 16) +
                                 " [a=" + mosaic::Hex(vs2, 16) + " b=" + mosaic::Hex(vs1, 16) + "]");
                }
                rep->Check((o.el_sat != 0) == e.sat,
                           name + ": vxsat " + Dec(o.el_sat) + " expected " + Dec(e.sat));
                rep->Check(o.el_illegal == 0, name + ": an undeclared family was refused");
              }
            }
          }
        }
      }
    }
  }
}

// mask logical and mask prefix, which take their operands from mask registers
void PhaseMaskLane(Cfg* cfg, Vec* vec, Reporter* rep) {
  for (int sew_l = 3; sew_l <= 6; ++sew_l) {
    ConfigureVec(cfg, SewField(sew_l), 0, 0, 0, 64);
    for (int idx = 0; idx < 8; ++idx) {
      for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
          uint64_t va = (static_cast<uint64_t>(a) * 0x11u) & 0xFFull;
          uint64_t vb = (static_cast<uint64_t>(b) * 0x37u) & 0xFFull;
          for (int op = 0; op < 8; ++op) {
            VecStim s;
            s.el_valid = true;
            s.el_family = VF_MASKLOG;
            s.el_op = op;
            s.el_form = kFormVx;
            s.el_vs2 = vb;
            s.el_vs1 = va;
            s.el_index = static_cast<uint8_t>(idx);
            s.el_mask = true;
            VecObs o = vec->Cycle(s);
            ElemVal e = OracleElem(VF_MASKLOG, op, 8, kFormVv, vb, va, 0, 0, 0, idx, false);
            rep->Check(o.el_mres == e.mres,
                       std::string("mask-logic op") + Dec(op) + " a=" + Dec(a) + " b=" +
                           Dec(b) + " bit" + Dec(idx) + ": mask " + Dec(o.el_mres) +
                           " expected " + Dec(e.mres));
          }
        }
      }
    }
    // the prefix family carries state from element to element
    for (int op = 0; op < 3; ++op) {
      for (int pattern = 0; pattern < 16; ++pattern) {
        bool pfx = false;
        for (int idx = 0; idx < 8; ++idx) {
          uint64_t src = static_cast<uint64_t>((pattern >> (idx / 2)) & 3u) ? 0x01u : 0x00u;
          VecStim s;
          s.el_valid = true;
          s.el_family = VF_MASKPFX;
          s.el_op = op;
          s.el_form = kFormVv;
          s.el_vs2 = src;
          s.el_index = static_cast<uint8_t>(idx);
          s.el_pfx = pfx;
          s.el_mask = true;
          VecObs o = vec->Cycle(s);
          ElemVal e = OracleElem(VF_MASKPFX, op, 8, kFormVv, src, 0, 0, 0, 0, idx, pfx);
          rep->Check(o.el_mres == e.mres,
                     std::string("mask-prefix op") + Dec(op) + " pattern" + Dec(pattern) +
                         " bit" + Dec(idx) + ": mask " + Dec(o.el_mres) + " expected " +
                         Dec(e.mres));
          rep->Check(o.el_pfx == e.pfx,
                     std::string("mask-prefix op") + Dec(op) + " pattern" + Dec(pattern) +
                         " bit" + Dec(idx) + ": state " + Dec(o.el_pfx) + " expected " +
                         Dec(e.pfx));
          pfx = e.pfx;
        }
      }
    }
  }
}

// --------------------------------------------- permute: the element ordering
// The permute's contract is *which* source element each destination element
// names. `e_rd2_o` is that index, so the ordering is checked directly rather
// than inferred from a value that a wrong order could still produce.
void PhasePermuteLane(Cfg* cfg, Vec* vec, Reporter* rep) {
  for (int lmul_e = 0; lmul_e <= 2; ++lmul_e) {
    for (int sew_l = 3; sew_l <= 6; ++sew_l) {
      int vlmax = VlmaxOf2(sew_l, lmul_e);
      if (vlmax == 0) continue;
      ConfigureVec(cfg, SewField(sew_l), VlmulOfExp(lmul_e), 0, 0, vlmax);
      int n = vlmax < 12 ? vlmax : 12;
      // vslideup / vslidedown with an offset
      for (int off = 0; off <= 3; ++off) {
        for (int i = 0; i < n; ++i) {
          VecStim s;
          s.el_valid = true;
          s.el_family = VF_SLIDE;
          s.el_op = 0;
          s.el_form = kFormVx;
          s.el_scalar = static_cast<uint64_t>(off);
          s.el_index = static_cast<uint8_t>(i);
          s.el_mask = true;
          s.el_vs2 = 0x1234;
          VecObs o = vec->Cycle(s);
          int exp_rd2 = (i - off) & 0xFF;
          bool exp_access = (i >= off) && (i - off) < vlmax;
          bool rd_ok = (!exp_access) || (o.el_rd2 == exp_rd2);
          rep->Check(rd_ok && ((o.el_access != 0) == exp_access),
                     "permute-order slideup off" + Dec(off) + " lmul" + Dec(lmul_e) +
                         " i" + Dec(i) + ": rd2 " + Dec(o.el_rd2) + " access " +
                         Dec(o.el_access) + ", expected source " + Dec(exp_rd2) + " access " +
                         Dec(exp_access));

          VecStim d;
          d.el_valid = true;
          d.el_family = VF_SLIDE;
          d.el_op = 1;
          d.el_form = kFormVx;
          d.el_scalar = static_cast<uint64_t>(off);
          d.el_index = static_cast<uint8_t>(i);
          d.el_mask = true;
          d.el_vs2 = 0x1234;
          VecObs od = vec->Cycle(d);
          int exp_d = i + off;
          bool exp_da = exp_d < vlmax;
          rep->Check(((!exp_da) || (od.el_rd2 == exp_d)) && ((od.el_access != 0) == exp_da),
                     "permute-order slidedown off" + Dec(off) + " lmul" + Dec(lmul_e) +
                         " i" + Dec(i) + ": access " + Dec(od.el_access) + " expected " +
                         Dec(exp_da));
          if (exp_da) {
            rep->Check(od.el_rd2 == exp_d,
                       "permute-order slidedown off" + Dec(off) + " lmul" + Dec(lmul_e) +
                           " i" + Dec(i) + ": rd2 " + Dec(od.el_rd2) + " expected " +
                           Dec(exp_d));
          }
        }
      }
      // vrgather: the index comes from the data, so the ordering is data-driven
      for (int i = 0; i < n; ++i) {
        for (int k = 0; k <= 3; ++k) {
          uint64_t index = (k == 3) ? static_cast<uint64_t>(vlmax + 5) : static_cast<uint64_t>(k);
          VecStim s;
          s.el_valid = true;
          s.el_family = VF_GATHER;
          s.el_op = 0;
          s.el_form = kFormVv;
          s.el_vs1 = index;
          s.el_index = static_cast<uint8_t>(i);
          s.el_mask = true;
          s.el_vs2 = 0x55;
          VecObs o = vec->Cycle(s);
          bool exp_a = index < static_cast<uint64_t>(vlmax);
          rep->Check((o.el_access != 0) == exp_a,
                     "permute-order gather lmul" + Dec(lmul_e) + " i" + Dec(i) + " idx" +
                         Dec(index) + ": access " + Dec(o.el_access) + " expected " +
                         Dec(exp_a));
          if (exp_a) {
            rep->Check(o.el_rd2 == static_cast<int>(index),
                       "permute-order gather lmul" + Dec(lmul_e) + " i" + Dec(i) +
                           ": rd2 " + Dec(o.el_rd2) + " expected " + Dec(index));
          }
          rep->Check(o.el_trap == 0, "permute-order gather: an in-range index trapped");
        }
      }
      // vcompress walks the source in order
      for (int i = 0; i < n; ++i) {
        VecStim s;
        s.el_valid = true;
        s.el_family = VF_COMPRESS;
        s.el_op = 0;
        s.el_form = kFormVv;
        s.el_index = static_cast<uint8_t>(i);
        s.el_mask = true;
        VecObs o = vec->Cycle(s);
        rep->Check(o.el_rd2 == i && o.el_access != 0,
                   "permute-order compress lmul" + Dec(lmul_e) + " i" + Dec(i) +
                       ": rd2 " + Dec(o.el_rd2) + " access " + Dec(o.el_access));
      }
    }
  }
}

// ------------------------------------------------ reduction: the fold order
// A reduction is a strictly left-to-right chain starting from vs1[0]; the
// running accumulator after each element is compared with the specification's
// fold, and the order itself is checked through the engine's trace below.
void PhaseReduceLane(Cfg* cfg, Vec* vec, Reporter* rep) {
  for (int sew_l = 3; sew_l <= 6; ++sew_l) {
    int sew = 1 << sew_l;
    ConfigureVec(cfg, SewField(sew_l), 0, 0, 0, 64);
    std::vector<uint64_t> vals;
    BoundaryValues(sew, &vals);
    for (int op = 0; op < 8; ++op) {
      for (size_t si = 0; si < vals.size(); ++si) {
        for (size_t ai = 0; ai < vals.size(); ++ai) {
          uint64_t acc = vals[ai];
          for (int k = 0; k < 4; ++k) {
            uint64_t src = vals[(k * 3 + 1) % vals.size()];
            VecStim s;
            s.el_valid = true;
            s.el_family = VF_REDUCE;
            s.el_op = op;
            s.el_form = kFormVv;
            s.el_vs2 = src;
            s.el_acc = acc;
            s.el_mask = true;
            s.el_index = static_cast<uint8_t>(k);
            VecObs o = vec->Cycle(s);
            ElemVal e = OracleElem(VF_REDUCE, op, sew, kFormVv, src, 0, 0, acc, 0, 0, false);
            rep->Check((o.el_result & MaskW(sew)) == (e.result & MaskW(sew)),
                       std::string("reduce-order ") + VecFamilyName(VF_REDUCE) + " op" + Dec(op) + " sew" +
                           Dec(sew) + " step" + Dec(k) + ": " +
                           mosaic::Hex(o.el_result & MaskW(sew), 16) + " expected " +
                           mosaic::Hex(e.result & MaskW(sew), 16));
            acc = e.result & MaskW(sew);
          }
        }
      }
    }
    // widening reductions
    if (sew <= 32) {
      for (int op = 0; op < 2; ++op) {
        for (size_t ai = 0; ai < vals.size(); ++ai) {
          uint64_t acc = vals[ai] & MaskW(2 * sew);
          for (int k = 0; k < 4; ++k) {
            uint64_t src = vals[(k * 5 + 2) % vals.size()];
            VecStim s;
            s.el_valid = true;
            s.el_family = VF_REDWIDE;
            s.el_op = op;
            s.el_form = kFormVv;
            s.el_vs2 = src;
            s.el_acc = acc;
            s.el_mask = true;
            s.el_index = static_cast<uint8_t>(k);
            VecObs o = vec->Cycle(s);
            ElemVal e = OracleElem(VF_REDWIDE, op, sew, kFormVv, src, 0, 0, acc, 0, 0, false);
            rep->Check((o.el_result & MaskW(2 * sew)) == (e.result & MaskW(2 * sew)),
                       std::string("reduce-order redwide op") + Dec(op) + " sew" + Dec(sew) +
                           " step" + Dec(k) + ": " +
                           mosaic::Hex(o.el_result & MaskW(2 * sew), 16) + " expected " +
                           mosaic::Hex(e.result & MaskW(2 * sew), 16));
            acc = e.result & MaskW(2 * sew);
          }
        }
      }
    }
  }
}

// ------------------------------------------------------- capability gating
void PhaseCapabilityGate(Cfg* cfg, Vec* vec, Reporter* rep) {
  ConfigureVec(cfg, 0, 0, 0, 0, 4);
  for (int fam = 0; fam < VF_COUNT; ++fam) {
    // with the family declared, it must execute
    int before_rd = vec->RdGnt();
    int before_wr = vec->WrGnt();
    VecObs ok = vec->RunPacket(fam, 0, kFormVv, 4, 5, 6, 3, false, kAllCaps);
    rep->Check(!ok.alu_illegal,
               std::string("capability-gate ") + VecFamilyName(fam) +
                   ": a declared family was refused");
    (void)before_rd;
    (void)before_wr;

    // with the family's bit clear, it must be refused and touch nothing
    int rd0 = vec->RdGnt();
    int wr0 = vec->WrGnt();
    uint64_t caps = kAllCaps & ~(1ull << fam);
    VecObs bad = vec->RunPacket(fam, 0, kFormVv, 4, 5, 6, 3, false, caps);
    rep->Check(bad.alu_illegal,
               std::string("capability-gate ") + VecFamilyName(fam) +
                   ": an undeclared family was executed");
    rep->Check(!bad.alu_trap, std::string("capability-gate ") + VecFamilyName(fam) +
                                  ": a refused family raised a trap");
    rep->Check(vec->RdGnt() == rd0 && vec->WrGnt() == wr0,
               std::string("capability-gate ") + VecFamilyName(fam) +
                   ": a refused family touched the register file");
  }

  // an unknown family id is refused too
  int rd0 = vec->RdGnt();
  VecObs unk = vec->RunPacket(31, 0, kFormVv, 4, 5, 6, 0, false, kAllCaps);
  rep->Check(unk.alu_illegal, "capability-gate: an unknown family id was executed");
  rep->Check(vec->RdGnt() == rd0, "capability-gate: an unknown family id touched the VRF");
}

// A destination group's element value after a packet.
uint64_t DstElem(const HostVrf& vf, int vd, int i, int sew_l, int lmul_e) {
  return HostGet(vf, vd, i, sew_l, lmul_e);
}

// --------------------------------------------------------------- group layout
int GrpSizeE(int lmul_e) { return lmul_e >= 0 ? (1 << lmul_e) : 1; }

struct Layout {
  int vd = 0, vs1 = 0, vs2 = 0;
  bool mask = false;
};

Layout PlanLayout(int fam, int lmul_e, bool want_mask) {
  int s1w, s1l, s2w, s2l, dw, dl;
  FamilyWidths(fam, 3, lmul_e, &s1w, &s1l, &s2w, &s2l, &dw, &dl);
  int n1 = GrpSizeE(s1l), n2 = GrpSizeE(s2l), nd = GrpSizeE(dl);
  int maxn = n1 > n2 ? n1 : n2;
  if (nd > maxn) maxn = nd;
  bool usemask = want_mask && (maxn <= 2);
  int chunk = usemask ? 1 : 0;
  chunk = (chunk + n2 - 1) & ~(n2 - 1);
  int b2 = chunk; chunk += n2;
  chunk = (chunk + n1 - 1) & ~(n1 - 1);
  int b1 = chunk; chunk += n1;
  chunk = (chunk + nd - 1) & ~(nd - 1);
  int bd = chunk;
  Layout L;
  L.vd = bd; L.vs1 = b1; L.vs2 = b2; L.mask = usemask;
  return L;
}

// Prime a whole group through the VRF and the host model together.
void PrimeGroup(Vec* vec, HostVrf* vf, int base, int n, int sew_l, int lmul_e, int seed) {
  for (int i = 0; i < n; ++i) {
    vec->Prime(*vf, base, i, sew_l, lmul_e, Pat(seed * 131 + i));
  }
}

void PrimeMaskReg(Vec* vec, HostVrf* vf, int base, int nbits, int seed) {
  for (int b = 0; b * 8 < nbits; ++b) {
    uint64_t byte = 0;
    for (int k = 0; k < 8; ++k) {
      int bit = b * 8 + k;
      if (bit < 128 && (Pat(seed * 977 + bit) & 3ull) != 0) byte |= (1ull << k);
    }
    vec->Prime(*vf, base, b, 3, 0, byte);
  }
}

// ------------------------------------------------------- expected destination
void ComputeExpected(int fam, int op, int form, int sew_l, int lmul_e, int vstart, int vl,
                     int vta, int vma, bool mask_en, const HostVrf& vf, const Layout& L,
                     uint64_t scalar, int vxrm, int n, std::vector<uint64_t>* ev,
                     std::vector<bool>* mv) {
  int sew = 1 << sew_l;
  int vlmax = VlmaxOf2(sew_l, lmul_e);
  int s1w, s1l, s2w, s2l, dw, dl;
  FamilyWidths(fam, sew_l, lmul_e, &s1w, &s1l, &s2w, &s2l, &dw, &dl);
  int ds = 1 << dw;
  uint64_t ones = ds >= 64 ? ~0ull : ((1ull << ds) - 1ull);
  bool maskdst = (fam == VF_CMP || fam == VF_MASKLOG || fam == VF_MASKPFX);
  ev->assign(static_cast<size_t>(n), 0);
  mv->assign(static_cast<size_t>(n), false);

  if (fam == VF_COMPRESS) {
    for (int i = 0; i < n; ++i) {
      bool indis = ((HostGet(vf, L.vs1, i / 8, 3, 0) >> (i % 8)) & 1u) != 0;
      ev->at(static_cast<size_t>(i)) = HostGet(vf, L.vd, i, dw, dl);
      (void)indis;
    }
    int count = 0;
    for (int i = 0; i < vl; ++i) {
      bool on = ((HostGet(vf, L.vs1, i / 8, 3, 0) >> (i % 8)) & 1u) != 0;
      if (on && count < n) {
        ev->at(static_cast<size_t>(count)) = HostGet(vf, L.vs2, i, s2w, s2l);
        count++;
      }
    }
    for (int i = count; i < n; ++i) {
      if (vta) ev->at(static_cast<size_t>(i)) = ones;
    }
    return;
  }

  if (fam == VF_REDUCE || fam == VF_REDWIDE) {
    // only element 0 is written; the rest of the destination group is
    // undisturbed
    for (int i = 0; i < n; ++i) {
      ev->at(static_cast<size_t>(i)) = HostGet(vf, L.vd, i, dw, dl);
    }
    int acc_w = (fam == VF_REDWIDE) ? 2 * sew : sew;
    uint64_t acc = HostGet(vf, L.vs1, 0, sew_l, lmul_e);
    if (fam == VF_REDWIDE) {
      acc = (op == 0) ? (acc & MaskW(sew)) : static_cast<uint64_t>(SxW(acc, sew)) & MaskW(acc_w);
    }
    for (int i = vstart; i < vl; ++i) {
      if (mask_en && !MaskBit(vf, i)) continue;   // masked-off elements are not read
      uint64_t src = HostGet(vf, L.vs2, i, s2w, s2l);
      ElemVal e = OracleElem(fam, op, sew, kFormVv, src, 0, 0, acc, vxrm, i, false);
      acc = e.result & MaskW(acc_w);
    }
    uint64_t pv = HostGet(vf, L.vd, 0, dw, dl);
    ev->at(0) = (vstart < vl) ? acc : pv;
    return;
  }

  bool pfx = false;
  for (int i = 0; i < n; ++i) {
    bool mbit = mask_en ? MaskBit(vf, i) : true;
    uint64_t undis = maskdst ? (static_cast<uint64_t>(((HostGet(vf, L.vd, i / 8, 3, 0) >>
                                                         (i % 8)) & 1u) != 0))
                             : HostGet(vf, L.vd, i, dw, dl);
    uint64_t dval = undis;
    bool dbit = (undis & 1ull) != 0;
    if (i < vstart) {
      // unchanged
    } else if (i >= vl) {
      if (vta) { dval = ones; dbit = true; }
    } else if (!mbit) {
      if (vma) { dval = ones; dbit = true; }
    } else {
      uint64_t a = HostGet(vf, L.vs2,
                           (fam == VF_MASKLOG || fam == VF_MASKPFX) ? (i / 8) : i,
                           s2w, s2l);
      uint64_t b = (form == kFormVv)
                       ? HostGet(vf, L.vs1, (fam == VF_MASKLOG) ? (i / 8) : i, s1w, s1l)
                       : scalar;
      if (fam == VF_CMP || fam == VF_MASKLOG) {
        ElemVal e = OracleElem(fam, op, sew, form, a, b, scalar, 0, vxrm, i, false);
        dbit = e.mres;
        dval = dbit ? 1 : 0;
      } else if (fam == VF_MASKPFX) {
        ElemVal e = OracleElem(fam, op, sew, kFormVv, a, 0, 0, 0, vxrm, i, pfx);
        dbit = e.mres;
        dval = dbit ? 1 : 0;
        pfx = e.pfx;
      } else if (fam == VF_SLIDE) {
        int off = (op <= 1) ? static_cast<int>(scalar) : 1;
        if (op == 0) {
          dval = (i < off) ? undis : HostGet(vf, L.vs2, i - off, s2w, s2l);
          dbit = true;
          if (i < off) { dval = undis; dbit = (undis & 1ull) != 0; }
        } else if (op == 1) {
          dval = (i + off) < vlmax ? HostGet(vf, L.vs2, i + off, s2w, s2l) : 0;
          dbit = true;
        } else if (op == 2) {
          dval = (i == vstart) ? scalar : HostGet(vf, L.vs2, i - 1, s2w, s2l);
          dbit = true;
        } else {
          dval = (i == (vl - 1)) ? scalar : HostGet(vf, L.vs2, i + 1, s2w, s2l);
          dbit = true;
        }
        dval &= MaskW(ds);
      } else if (fam == VF_GATHER) {
        uint64_t idx = (form == kFormVv) ? HostGet(vf, L.vs1, i, s1w, s1l) : scalar;
        dval = (idx < static_cast<uint64_t>(vlmax))
                   ? HostGet(vf, L.vs2, static_cast<int>(idx), s2w, s2l)
                   : 0;
        dbit = true;
      } else {
        ElemVal e = OracleElem(fam, op, sew, form, a, b, scalar, 0, vxrm, 0, false);
        dval = e.result & MaskW(ds);
        dbit = true;
      }
    }
    ev->at(static_cast<size_t>(i)) = dval & MaskW(ds);
    mv->at(static_cast<size_t>(i)) = dbit;
  }
}

bool MaskDstFamily(int fam) {
  return fam == VF_CMP || fam == VF_MASKLOG || fam == VF_MASKPFX;
}

// --------------------------------------------------------------- the phases
void PhaseCoverage(Cfg* cfg, Vec* vec, Reporter* rep, Coverage* cov) {
  int expected_cells = 0;
  for (int fam = 0; fam < VF_COUNT; ++fam) {
    for (int si = 0; si < 4; ++si) {
      int sew_l = 3 + si;
      int sew = 1 << sew_l;
      if (!FamilySupportsSew(fam, sew)) continue;
      for (int li = 0; li < 7; ++li) {
        int lmul_e = kLmulExps[li];
        if (lmul_e + 6 < sew_l) continue;
        if (!FamilySupportsLmul(fam, lmul_e)) continue;
        int vlmax = VlmaxOf2(sew_l, lmul_e);
        if (vlmax == 0) continue;
        ++expected_cells;
        int vl = vlmax < 8 ? vlmax : 8;
        int form = kFormVv;
        uint64_t scalar = 0;
        if (fam == VF_SLIDE) { form = kFormVx; scalar = 2; }
        if (fam == VF_SHIFT) { form = kFormVx; scalar = 3; }
        if (fam == VF_GATHER) { form = kFormVv; }
        bool want_mask = !(fam == VF_MASKLOG || fam == VF_MASKPFX || fam == VF_COMPRESS);
        Layout L = PlanLayout(fam, lmul_e, want_mask);
        bool mask_en = L.mask;
        ConfigureVec(cfg, SewField(sew_l), VlmulOfExp(lmul_e), 0, 0, static_cast<uint64_t>(vl));
        (void)CsrWrite(cfg, kCsrVxrm, 2);
        HostVrf vf;
        // a permute may read a source element anywhere below VLMAX (and a
        // slide a few beyond it), so the whole source group must be primed
        // rather than only the active prefix.
        int np;
        if (fam == VF_GATHER || fam == VF_SLIDE) {
          np = vlmax + 8;
        } else {
          np = vl + 4;
        }
        if (np > 128) np = 128;
        int nd = vlmax < 16 ? vlmax : 16;
        int f1w, f1l, f2w, f2l, fdw, fdl;
        FamilyWidths(fam, sew_l, lmul_e, &f1w, &f1l, &f2w, &f2l, &fdw, &fdl);
        if (mask_en) PrimeMaskReg(vec, &vf, 0, np, fam * 7 + li + 1);
        PrimeGroup(vec, &vf, L.vs2, np, f2w, f2l, fam * 31 + li + 3);
        PrimeGroup(vec, &vf, L.vs1, np, f1w, f1l, fam * 17 + li + 5);
        if (MaskDstFamily(fam)) {
          PrimeMaskReg(vec, &vf, L.vd, nd, fam * 11 + li + 7);
        } else {
          PrimeGroup(vec, &vf, L.vd, nd, fdw, fdl, fam * 11 + li + 7);
        }

        std::vector<int> trace;
        VecObs o = vec->RunPacket(fam, 0, form, L.vd, L.vs1, L.vs2, scalar, mask_en, kAllCaps,
                                  &trace);
        std::string name = std::string("coverage ") + VecFamilyName(fam) + " sew" + Dec(sew) +
                           " lmul" + std::to_string(lmul_e);
        rep->Check(!o.alu_illegal, name + ": a declared family was refused");
        rep->Check(!o.alu_trap, name + ": the packet trapped");
        rep->Check(!o.alu_busy, name + ": the packet never completed");

        std::vector<uint64_t> ev;
        std::vector<bool> mv;
        ComputeExpected(fam, 0, form, sew_l, lmul_e, 0, vl, 0, 0, mask_en, vf, L, scalar, 2, nd,
                        &ev, &mv);
        for (int i = 0; i < nd; ++i) {
          if (MaskDstFamily(fam)) {
            bool got = ((vec->MemRead(L.vd, i / 8, 3, 0) >> (i % 8)) & 1u) != 0;
            rep->Check(got == mv[static_cast<size_t>(i)],
                       name + " bit" + Dec(i) + ": " + Dec(got) + " expected " +
                           Dec(mv[static_cast<size_t>(i)]));
          } else {
            int dw, dl;
            int t1, t2, t3, t4;
            FamilyWidths(fam, sew_l, lmul_e, &t1, &t2, &t3, &t4, &dw, &dl);
            uint64_t got = vec->MemRead(L.vd, i, dw, dl);
            rep->Check(got == ev[static_cast<size_t>(i)],
                       name + " elem" + Dec(i) + ": " + mosaic::Hex(got, 16) + " expected " +
                           mosaic::Hex(ev[static_cast<size_t>(i)], 16));
          }
        }
        cov->cell[fam][si][li] = true;
        ++cov->cells;
        if (mask_en) {
          rep->Check(o.src_rd_ctr <= np, name + ": the packet read beyond its sources");
        }
      }
    }
  }
  rep->Check(cov->cells == expected_cells,
             "coverage: " + Dec(cov->cells) + " cells ran, expected " + Dec(expected_cells));
}

// A masked-off element must neither trap nor access. The gather index of a
// masked-off element is deliberately out of range: an implementation that
// computed the address before consulting the mask would issue a bad demand.
void PhaseMaskedOff(Cfg* cfg, Vec* vec, Reporter* rep) {
  for (int vma = 0; vma < 2; ++vma) {
    ConfigureVec(cfg, 0, 0, 0, vma, 8);
    HostVrf vf;
    Layout L;
    L.vd = 8; L.vs1 = 24; L.vs2 = 16; L.mask = true;
    const int active[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    uint64_t byte = 0;
    for (int k = 0; k < 8; ++k) {
      if (active[k]) byte |= (1ull << k);
      uint64_t idx = active[k] ? static_cast<uint64_t>(7 - k) : 250ull;
      vec->Prime(vf, L.vs1, k, 3, 0, idx);
      vec->Prime(vf, L.vs2, k, 3, 0, 0x40u + static_cast<uint64_t>(k));
      vec->Prime(vf, L.vd, k, 3, 0, 0xC0u + static_cast<uint64_t>(k));
    }
    vec->Prime(vf, 0, 0, 3, 0, byte);

    int rd0 = vec->RdGnt();
    int bad0 = vec->RdBad();
    int active_count = 0;
    for (int k = 0; k < 8; ++k) active_count += active[k];
    std::vector<int> trace;
    VecObs o = vec->RunPacket(VF_GATHER, 0, kFormVv, L.vd, L.vs1, L.vs2, 0, true, kAllCaps,
                              &trace);
    std::string name = std::string("masked-off vma") + Dec(vma);
    rep->Check(!o.alu_trap, name + ": a masked-off element raised a trap");
    rep->Check(vec->RdBad() == bad0, name + ": a masked-off element made a bad access");
    rep->Check(o.src_rd_ctr == active_count,
               name + ": " + Dec(o.src_rd_ctr) + " source reads for " + Dec(active_count) +
                   " active elements");
    rep->Check(static_cast<int>(trace.size()) == active_count,
               name + ": the access trace lists " + Dec(static_cast<int>(trace.size())) +
                   " elements, expected " + Dec(active_count));
    rep->Check(vec->RdGnt() >= rd0, name + ": the read counter went backwards");
    for (int k = 0; k < 8; ++k) {
      uint64_t got = vec->MemRead(L.vd, k, 3, 0);
      uint64_t exp;
      if (active[k]) {
        exp = 0x40u + static_cast<uint64_t>(7 - k);
      } else if (vma) {
        exp = 0xFFu;
      } else {
        exp = 0xC0u + static_cast<uint64_t>(k);
      }
      rep->Check(got == exp, name + " elem" + Dec(k) + ": " + mosaic::Hex(got, 2) +
                                 " expected " + mosaic::Hex(exp, 2));
    }
    (void)active;
  }

  // An *active* element whose gather index leaves the group reads nothing and
  // its result is zero: the range check, not a fault.
  ConfigureVec(cfg, 0, 0, 0, 0, 4);
  {
    HostVrf vf;
    Layout L;
    L.vd = 8; L.vs1 = 24; L.vs2 = 16; L.mask = false;
    for (int k = 0; k < 4; ++k) {
      vec->Prime(vf, L.vs1, k, 3, 0, static_cast<uint64_t>(16 + k));  // >= VLMAX
      vec->Prime(vf, L.vs2, k, 3, 0, 0x30u + static_cast<uint64_t>(k));
      vec->Prime(vf, L.vd, k, 3, 0, 0xE0u + static_cast<uint64_t>(k));
    }
    int bad0 = vec->RdBad();
    VecObs o = vec->RunPacket(VF_GATHER, 0, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
    rep->Check(!o.alu_trap, "masked-off: an out-of-range gather index trapped");
    rep->Check(vec->RdBad() == bad0, "masked-off: an out-of-range gather index accessed");
    for (int k = 0; k < 4; ++k) {
      uint64_t got = vec->MemRead(L.vd, k, 3, 0);
      rep->Check(got == 0, "masked-off: out-of-range gather elem" + Dec(k) + " gave " +
                               mosaic::Hex(got, 2) + ", expected 0");
    }
  }
}

// The reduction's order, observed through the engine's element trace: the
// declared order is ascending from vstart, with no reassociation.
void PhaseReductionOrder(Cfg* cfg, Vec* vec, Reporter* rep) {
  ConfigureVec(cfg, 0, 0, 0, 0, 8);
  HostVrf vf;
  Layout L;
  L.vd = 8; L.vs1 = 24; L.vs2 = 16; L.mask = false;
  for (int i = 0; i < 8; ++i) {
    vec->Prime(vf, L.vs2, i, 3, 0, Pat(700 + i) & 0xFFull);
  }
  vec->Prime(vf, L.vs1, 0, 3, 0, 0x11u);
  vec->Prime(vf, L.vd, 0, 3, 0, 0x99u);

  for (int op = 0; op < 8; ++op) {
    HostVrf vf2 = vf;
    std::vector<int> trace;
    VecObs o = vec->RunPacket(VF_REDUCE, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps,
                              &trace);
    std::string name = std::string("reduce-order packet op") + Dec(op);
    bool ascending = (trace.size() == 8);
    for (size_t k = 0; k < trace.size(); ++k) {
      if (trace[k] != static_cast<int>(k)) ascending = false;
    }
    rep->Check(ascending, name + ": the fold order was not ascending from vstart");
    uint64_t acc = HostGet(vf2, L.vs1, 0, 3, 0);
    for (int i = 0; i < 8; ++i) {
      uint64_t src = HostGet(vf2, L.vs2, i, 3, 0);
      ElemVal e = OracleElem(VF_REDUCE, op, 8, kFormVv, src, 0, 0, acc, 0, i, false);
      acc = e.result;
    }
    rep->Check(o.alu_acc == acc, name + ": accumulator " + mosaic::Hex(o.alu_acc, 2) +
                                     " expected " + mosaic::Hex(acc, 2));
    uint64_t got = vec->MemRead(L.vd, 0, 3, 0);
    rep->Check(got == acc, name + ": vd[0] " + mosaic::Hex(got, 2) + " expected " +
                               mosaic::Hex(acc, 2));
  }
}

void RunVecIntCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep, Coverage* cov) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);
  PhaseElementLane(&cfg, &vec, rep);
  PhaseMaskLane(&cfg, &vec, rep);
  PhasePermuteLane(&cfg, &vec, rep);
  PhaseReduceLane(&cfg, &vec, rep);
  PhaseCapabilityGate(&cfg, &vec, rep);
  // the targeted policy checks come before the bulk coverage sweep, so the
  // first failure a policy defect produces names the policy
  PhaseMaskedOff(&cfg, &vec, rep);
  PhaseReductionOrder(&cfg, &vec, rep);
  PhaseCoverage(&cfg, &vec, rep, cov);
}

// ============================================================================
// I-056 -- the phase set for CASE=rvv.memory_modes
// ============================================================================
// ============================================================================
// I-056 -- the vector memory packetizer (CASE=rvv.memory_modes).
//
// The expectation is computed here from the V specification's address rules,
// not from the RTL: `PlanLsu` is written from the spec's element-address
// formulas (unit/strided/indexed/segment/whole/mask, offset zero-extension,
// NFIELDS and the EVL of a mask transfer), and it is the same function the
// phase uses to decide which memory a request should have reached. The
// packetizer's request stream is captured on its memory port and compared
// item-by-item and byte-mask-by-byte-mask.
//
// The host memory model carries the *region map*: a request whose enabled bytes
// leave one region -- a merge across a permission or MMIO boundary -- is caught
// however it was formed, so the device-straddling phase does not need to trust
// the packetizer's own notion of a region.
//
// Coverage is itself a check: every memory mode the case claims to exercise
// must be shown to have run, and the cell count must match the scope.
// ============================================================================

enum : int {
  LS_UNIT = 0, LS_STRIDED = 1, LS_INDEXED = 2,
  LS_SEG_UNIT = 3, LS_SEG_STRIDED = 4, LS_SEG_INDEXED = 5,
  LS_WHOLE = 6, LS_MASK = 7, LS_MODE_COUNT = 8
};

const char* LsuModeName(int m) {
  switch (m) {
    case LS_UNIT: return "unit";
    case LS_STRIDED: return "strided";
    case LS_INDEXED: return "indexed";
    case LS_SEG_UNIT: return "seg-unit";
    case LS_SEG_STRIDED: return "seg-strided";
    case LS_SEG_INDEXED: return "seg-indexed";
    case LS_WHOLE: return "whole";
    case LS_MASK: return "mask";
    default: return "?";
  }
}

int LsuVlmaxOf(int sew_l, int lmul_e) {
  int e = 7 + lmul_e - sew_l;
  return e < 0 ? 0 : (1 << e);
}
int LsuEmulRegs(int lmul_e) { return lmul_e >= 0 ? (1 << lmul_e) : 1; }
int LsuGrpAlign(int base, int lmul_e) { int g = LsuEmulRegs(lmul_e); return base & ~(g - 1); }
int LsuIsSeg(int mode) {
  return mode == LS_SEG_UNIT || mode == LS_SEG_STRIDED || mode == LS_SEG_INDEXED;
}
int LsuIsIndexed(int mode) { return mode == LS_INDEXED || mode == LS_SEG_INDEXED; }

// The byte enables of an element at `addr`, relative to its 8-byte beat.
uint8_t LsuByteMaskOf(int be, uint64_t addr) {
  int lane = static_cast<int>(addr & 7ull);
  if (be >= 8) return 0xFF;
  return static_cast<uint8_t>(((1u << be) - 1u) << lane);
}

// ---------------------------------------------------------------- the oracle
// One expected item: the logical index, the field and the byte address. Written
// from the spec's address rules.
struct ExpReq {
  int elem = 0;
  int field = 0;
  uint64_t addr = 0;
  uint8_t mask = 0;
};

void PlanLsu(int mode, int sew_l, int lmul_e, int nf, int vl, int vstart,
             bool mask_en, const std::vector<bool>& mbits, uint64_t base,
             int64_t stride, const std::vector<uint64_t>& idxv,
             std::vector<ExpReq>* out) {
  (void)lmul_e;   // the offsets do not scale with LMUL; the group size does
  out->clear();
  int be = (1 << sew_l) / 8;   // EEW in bytes
  int nfe = LsuIsSeg(mode) ? nf : 1;
  int vlmax_w = 16 / be;   // VLEN=128 bits / EEW
  int elem_end;
  if (mode == LS_WHOLE) elem_end = nf * vlmax_w;
  else if (mode == LS_MASK) elem_end = (vl + 7) / 8;
  else elem_end = vl;
  for (int e = vstart; e < elem_end; ++e) {
    if (mode != LS_WHOLE && mode != LS_MASK && mask_en && !mbits[static_cast<size_t>(e)]) {
      continue;
    }
    for (int f = 0; f < nfe; ++f) {
      uint64_t off = 0;
      switch (mode) {
        case LS_UNIT:        off = static_cast<uint64_t>(e) * static_cast<uint64_t>(be); break;
        case LS_STRIDED:     off = static_cast<uint64_t>(e) * static_cast<uint64_t>(stride); break;
        case LS_INDEXED:     off = idxv[static_cast<size_t>(e)]; break;
        case LS_SEG_UNIT:    off = (static_cast<uint64_t>(e) * nf + f) * static_cast<uint64_t>(be); break;
        case LS_SEG_STRIDED: off = static_cast<uint64_t>(e) * static_cast<uint64_t>(stride) +
                                   static_cast<uint64_t>(f) * static_cast<uint64_t>(be); break;
        case LS_SEG_INDEXED: off = idxv[static_cast<size_t>(e)] +
                                   static_cast<uint64_t>(f) * static_cast<uint64_t>(be); break;
        case LS_WHOLE:       off = static_cast<uint64_t>(e) * static_cast<uint64_t>(be); break;
        default:             off = static_cast<uint64_t>(e); break;   // mask: one byte
      }
      ExpReq r;
      r.elem = e;
      r.field = f;
      r.addr = base + off;
      r.mask = LsuByteMaskOf(mode == LS_MASK ? 1 : be, r.addr);
      out->push_back(r);
    }
  }
}

// ------------------------------------------------------------- the host memory
struct LsuRegion {
  uint64_t lo = 0;
  uint64_t hi = 0;
  bool device = false;
  bool read_ok = true;
  bool write_ok = true;
};

struct LsuRec {
  int elem = 0;
  int field = 0;
  uint64_t addr = 0;
  uint8_t mask = 0;
  uint64_t wdata = 0;
  bool we = false;
  int size = 0;
  bool ordered = false;
};

struct LsuMem {
  static const int kSize = 0x10000;
  std::vector<uint8_t> mem;
  std::vector<LsuRegion> regions;
  int latency = 2;
  bool fault_enable = false;
  bool fault_all = false;
  int fault_elem = -1;
  int fault_field = 0;
  // I-061: a byte-granular fault map. A merge makes a request cover several
  // elements, so the fault decision must be a property of the *bytes* the
  // request enables (an address permission), not of one element index. When
  // this map is non-empty it decides the fault; otherwise the element/field
  // match does, which is what the I-056 and I-057 phases use.
  std::vector<bool> fault_bytes;
  // I-057 stop path: extra cycles a *faulting* response is held before it is
  // returned, so that a later element has been issued (and is genuinely in
  // flight) when the fault is reported. Zero for every other case.
  int fault_hold = 0;
  int req_count = 0;
  int device_reqs = 0;
  int ram_reqs = 0;
  // I-057: how many times each byte has been written, so a duplicated store
  // side effect across a restart is visible rather than inferred.
  std::vector<int> byte_writes;

  LsuMem() : mem(static_cast<size_t>(kSize)), byte_writes(static_cast<size_t>(kSize), 0) {
    for (int i = 0; i < kSize; ++i) {
      mem[static_cast<size_t>(i)] = static_cast<uint8_t>(Pat(4000 + i) & 0xFFull);
    }
    OneRam();
  }

  void OneRam() {
    regions.clear();
    LsuRegion r;
    r.lo = 0; r.hi = static_cast<uint64_t>(kSize); r.device = false;
    regions.push_back(r);
  }

  void DeviceAt(uint64_t boundary) {
    regions.clear();
    LsuRegion a; a.lo = 0; a.hi = boundary; a.device = false; regions.push_back(a);
    LsuRegion b; b.lo = boundary; b.hi = static_cast<uint64_t>(kSize); b.device = true;
    regions.push_back(b);
  }

  void ResetCounters() {
    req_count = 0; device_reqs = 0; ram_reqs = 0;
    for (size_t i = 0; i < byte_writes.size(); ++i) byte_writes[i] = 0;
  }

  int RegionOf(uint64_t byte) const {
    for (size_t i = 0; i < regions.size(); ++i) {
      if (byte >= regions[i].lo && byte < regions[i].hi) return static_cast<int>(i);
    }
    return -1;
  }

  bool IsDevice(uint64_t byte) const {
    int r = RegionOf(byte);
    return r >= 0 && regions[static_cast<size_t>(r)].device;
  }

  // Clear the byte-granular fault map and any element-keyed fault. The map is
  // emptied rather than filled with `false`: `Faults` tests `!fault_bytes.empty()`
  // to choose the byte path, so a non-empty all-false map would silently make an
  // element-keyed fault unreachable.
  void ClearFaults() {
    fault_enable = false;
    fault_all = false;
    fault_elem = -1;
    fault_field = 0;
    fault_bytes.clear();
  }

  // Fault an element's bytes, addressed as `addr`..`addr+be-1`.
  void FaultByteRange(uint64_t addr, int be) {
    fault_bytes.assign(static_cast<size_t>(kSize), false);
    for (int k = 0; k < be; ++k) {
      fault_bytes[static_cast<size_t>((addr + static_cast<uint64_t>(k)) & 0xFFFFull)] = true;
    }
    fault_enable = true;
    fault_all = false;
    fault_elem = -1;
  }

  // Does a request with `addr`/`mask` fault? Byte-granular when the map is set,
  // element-keyed otherwise.
  bool Faults(uint64_t addr, uint8_t mask, int elem, int field) const {
    if (fault_all) return true;
    if (!fault_bytes.empty()) {
      uint64_t beat = addr & ~7ull;
      for (int k = 0; k < 8; ++k) {
        if (((mask >> k) & 1u) == 0) continue;
        if (fault_bytes[static_cast<size_t>((beat + static_cast<uint64_t>(k)) & 0xFFFFull)]) return true;
      }
      return false;
    }
    return (elem == fault_elem) && (field == fault_field);
  }

  uint64_t Beat(uint64_t addr) const {
    uint64_t beat = addr & ~7ull;
    uint64_t v = 0;
    for (int k = 0; k < 8; ++k) {
      v |= static_cast<uint64_t>(mem[static_cast<size_t>((beat + static_cast<uint64_t>(k)) & 0xFFFFull)]) << (8 * k);
    }
    return v;
  }

  void Apply(uint64_t addr, uint8_t mask, uint64_t wdata) {
    uint64_t beat = addr & ~7ull;
    for (int k = 0; k < 8; ++k) {
      if ((mask >> k) & 1u) {
        mem[static_cast<size_t>((beat + static_cast<uint64_t>(k)) & 0xFFFFull)] =
            static_cast<uint8_t>((wdata >> (8 * k)) & 0xFFull);
        byte_writes[static_cast<size_t>((beat + static_cast<uint64_t>(k)) & 0xFFFFull)] += 1;
      }
    }
  }

  // the element's little-endian bytes starting at addr
  uint64_t Elem(uint64_t addr, int be) const {
    uint64_t v = 0;
    for (int k = 0; k < be; ++k) {
      v |= static_cast<uint64_t>(mem[static_cast<size_t>((addr + static_cast<uint64_t>(k)) & 0xFFFFull)]) << (8 * k);
    }
    return v;
  }
};

// ---------------------------------------------------------------- the harness
struct LsuStim {
  bool exec_valid = false;
  int mode = 0;
  bool we = false;
  bool ordered = false;
  int nf = 1;
  int vd = 0, data = 0, index = 0, idx_sew = 3;
  uint64_t base = 0, stride = 0;
  bool mask_en = false;
  uint8_t caps = 0xFF;
  bool mem_ready = true;
  // ---- I-061 coalescing --------------------------------------------------
  bool coalesce = false;        // enable same-hart, same-beat line coalescing
  bool atomic = false;          // the macro is an atomic class access
  // ---- I-057 restart controller -----------------------------------------
  bool fof = false;             // fault-only-first unit-stride load
  bool intr = false;            // precise interrupt request at a boundary
  int fault_code = 0;           // class forwarded with a faulting response
  bool bind = false;            // the controller owns the descriptor progress
  bool desc_alloc = false;
  int desc_alloc_vl = 0;
  int desc_alloc_vstart = 0;
  uint64_t desc_alloc_vtype = 0;
  bool desc_release = false;
  bool desc_fault_clear = false;
};

struct LsuObs {
  bool busy = false, done = false, illegal = false, trap = false;
  int trap_elem = 0, elems = 0;
  uint32_t req_ctr = 0;
  uint32_t merge_ctr = 0;      // I-061: elements a merge removed
  bool req_valid = false;
  int req_elem = 0, req_field = 0;
  uint64_t req_addr = 0;
  uint8_t req_mask = 0;
  uint64_t req_wdata = 0;
  // ---- I-057 -------------------------------------------------------------
  int trap_code = 0;
  bool stopped = false;
  int stop_elem = 0;
  bool rst_busy = false, rst_resolved = false, rst_illegal = false;
  bool rst_trap = false;
  int rst_vstart = 0;
  int rst_trap_code = 0;
  bool rst_vl_write = false;
  int rst_vl_new = 0;
  bool rst_fof_trim = false;
  bool rst_complete = false, rst_retire_ok = false;
  bool rst_restart_ready = false;
  int rst_restart_vstart = 0;
  int rst_elems_committed = 0;
  bool rst_prefix_agree = false;
  // descriptor read-back
  bool desc_valid = false;
  int desc_prefix = 0, desc_done_ctr = 0;
  bool desc_fault_valid = false;
  int desc_fault_elem = 0, desc_fault_code = 0;
  uint64_t desc_bm_lo = 0, desc_bm_hi = 0;
  // ---- I-057 stop path ---------------------------------------------------
  // How many requests the harness had in flight (accepted, no response yet)
  // when this observation was taken, and the element of the oldest one. The
  // stop-path case uses it to prove the scenario is not vacuous: a response was
  // genuinely in flight at the moment the stop was taken.
  int flight = 0;
  int flight_head = -1;
};

class Lsu {
 public:
  Lsu(Vmosaic_vec_tb* d, ClockDriver* clk) : d_(d), clk_(clk) {
    d_->lsu_exec_valid_i = 0;
    d_->lsu_caps_i = 0;
    d_->lsu_mem_req_ready_i = 0;
    d_->lsu_mem_rsp_valid_i = 0;
  }

  void BindMem(LsuMem* m) { mem_ = m; }

  // Clear the recorded request stream and the in-flight pipeline, so two runs
  // of one macro can be observed separately or together.
  void ClearReqs() {
    reqs_.clear();
    pending_.clear();
    overlapped_ = false;
    ordered_overlap_ = false;
    max_outstanding_ = 0;
  }

  struct Pending {
    int elem = 0;
    int field = 0;
    uint64_t addr = 0;
    uint8_t mask = 0;
    uint64_t wdata = 0;
    bool we = false;
    int age = 0;
  };

  LsuObs Step(const LsuStim& s) {
    bool rsp = false;
    int re = 0, rf = 0;
    bool rfault = false;
    uint64_t rdata = 0;
    if (!pending_.empty()) {
      const Pending& p0 = pending_.front();
      const bool front_faults =
          mem_->fault_enable && mem_->Faults(p0.addr, p0.mask, p0.elem, p0.field);
      const int need = mem_->latency + (front_faults ? mem_->fault_hold : 0);
      if (p0.age >= need) {
        const Pending& p = pending_.front();
        rsp = true;
        re = p.elem;
        rf = p.field;
        rdata = mem_->Beat(p.addr);
        rfault = front_faults;
        if (!rfault && p.we) mem_->Apply(p.addr, p.mask, p.wdata);
      }
    }

    d_->clk = 0;
    d_->rst = 0;
    d_->mem_owner_i = 0;
    d_->mem_rd_valid_i = 0;
    d_->mem_wr_valid_i = 0;
    d_->alu_exec_valid_i = 0;
    d_->alu_caps_i = 0;
    d_->el_valid_i = 0;
    d_->cfg_vset_valid = 0;
    d_->cfg_snap_capture = 0;
    d_->cfg_replay_valid = 0;
    d_->cfg_exec_valid = 0;
    d_->cfg_csr_valid = 0;

    d_->lsu_caps_i = s.caps;
    d_->lsu_exec_valid_i = s.exec_valid ? 1 : 0;
    d_->lsu_mode_i = static_cast<uint8_t>(s.mode & 0xF);
    d_->lsu_we_i = s.we ? 1 : 0;
    d_->lsu_ordered_i = s.ordered ? 1 : 0;
    d_->lsu_nf_i = static_cast<uint8_t>(s.nf & 0xF);
    d_->lsu_vd_i = static_cast<uint8_t>(s.vd & 0x1F);
    d_->lsu_data_i = static_cast<uint8_t>(s.data & 0x1F);
    d_->lsu_index_i = static_cast<uint8_t>(s.index & 0x1F);
    d_->lsu_idx_sew_i = static_cast<uint8_t>(s.idx_sew & 0x7);
    d_->lsu_base_i = s.base;
    d_->lsu_stride_i = s.stride;
    d_->lsu_mask_en_i = s.mask_en ? 1 : 0;
    d_->lsu_coalesce_i = s.coalesce ? 1 : 0;
    d_->lsu_atomic_i = s.atomic ? 1 : 0;
    d_->lsu_mem_req_ready_i = s.mem_ready ? 1 : 0;
    d_->lsu_mem_rsp_valid_i = rsp ? 1 : 0;
    d_->lsu_mem_rsp_elem_i = static_cast<uint8_t>(re & 0x7F);
    d_->lsu_mem_rsp_field_i = static_cast<uint8_t>(rf & 0xF);
    d_->lsu_mem_rsp_fault_i = rfault ? 1 : 0;
    d_->lsu_mem_rsp_fault_code_i = rfault ? static_cast<uint8_t>(s.fault_code & 0xF) : 0;
    d_->lsu_mem_rsp_rdata_i = rdata;

    // ---- I-057 restart controller -----------------------------------------
    // `bind` gives the controller the descriptor's progress ports; when it is
    // clear the driver drives them (here: idle), so the descriptor case keeps
    // its own ownership.
    d_->rst_bind_i = s.bind ? 1 : 0;
    d_->rst_exec_valid_i = s.exec_valid ? 1 : 0;
    d_->rst_exec_mode_i = static_cast<uint8_t>(s.mode & 0xF);
    d_->rst_exec_we_i = s.we ? 1 : 0;
    d_->rst_exec_fof_i = s.fof ? 1 : 0;
    d_->rst_exec_nf_i = static_cast<uint8_t>(s.nf & 0xF);
    d_->rst_intr_i = s.intr ? 1 : 0;
    d_->alloc_valid = s.desc_alloc ? 1 : 0;
    d_->alloc_vtype = s.desc_alloc_vtype;
    d_->alloc_vl = static_cast<uint8_t>(s.desc_alloc_vl & 0xFF);
    d_->alloc_vstart = static_cast<uint8_t>(s.desc_alloc_vstart & 0x7F);
    d_->alloc_vd = static_cast<uint8_t>(s.vd & 0x1F);
    d_->alloc_mask_ver = 0;
    d_->alloc_rob_index = 0;
    d_->alloc_rob_gen = 0;
    d_->alloc_uop_index = 0;
    d_->desc_release = s.desc_release ? 1 : 0;
    d_->desc_fault_clear = s.desc_fault_clear ? 1 : 0;
    d_->elem_done_valid = 0;
    d_->elem_done_index = 0;
    d_->fault_valid = 0;
    d_->fault_elem = 0;
    d_->fault_code = 0;

    d_->eval();
    const bool pre_req = d_->lsu_mem_req_valid_o != 0;
    const int pre_elem = static_cast<int>(d_->lsu_mem_req_elem_o);
    const int pre_field = static_cast<int>(d_->lsu_mem_req_field_o);
    const uint64_t pre_addr = d_->lsu_mem_req_addr_o;
    const uint8_t pre_mask = static_cast<uint8_t>(d_->lsu_mem_req_wmask_o);
    const uint64_t pre_wdata = d_->lsu_mem_req_wdata_o;
    const bool pre_we = d_->lsu_mem_req_we_o != 0;
    const int pre_size = static_cast<int>(d_->lsu_mem_req_size_o);
    const bool pre_ordered = d_->lsu_mem_req_ordered_o != 0;

    d_->clk = 1;
    d_->eval();
    d_->clk = 0;
    d_->eval();

    LsuObs o;
    o.busy = d_->lsu_busy_o != 0;
    o.done = d_->lsu_done_o != 0;
    o.illegal = d_->lsu_illegal_o != 0;
    o.trap = d_->lsu_trap_o != 0;
    o.trap_elem = static_cast<int>(d_->lsu_trap_elem_o);
    o.elems = static_cast<int>(d_->lsu_elems_o);
    o.req_ctr = static_cast<uint32_t>(d_->lsu_req_ctr_o);
    o.merge_ctr = static_cast<uint32_t>(d_->lsu_merge_ctr_o);
    o.req_valid = pre_req;
    o.req_elem = pre_elem;
    o.req_field = pre_field;
    o.req_addr = pre_addr;
    o.req_mask = pre_mask;
    o.req_wdata = pre_wdata;

    o.trap_code = static_cast<int>(d_->lsu_trap_code_o);
    o.stopped = d_->lsu_stopped_o != 0;
    o.stop_elem = static_cast<int>(d_->lsu_stop_elem_o);
    o.rst_busy = d_->o_rst_busy_o != 0;
    o.rst_resolved = d_->o_rst_resolved_o != 0;
    o.rst_illegal = d_->o_rst_illegal_o != 0;
    o.rst_trap = d_->o_rst_trap_o != 0;
    o.rst_vstart = static_cast<int>(d_->o_rst_vstart_o);
    o.rst_trap_code = static_cast<int>(d_->o_rst_trap_code_o);
    o.rst_vl_write = d_->o_rst_vl_write_o != 0;
    o.rst_vl_new = static_cast<int>(d_->o_rst_vl_new_o);
    o.rst_fof_trim = d_->o_rst_fof_trim_o != 0;
    o.rst_complete = d_->o_rst_complete_o != 0;
    o.rst_retire_ok = d_->o_rst_retire_ok_o != 0;
    o.rst_restart_ready = d_->o_rst_restart_ready_o != 0;
    o.rst_restart_vstart = static_cast<int>(d_->o_rst_restart_vstart_o);
    o.rst_elems_committed = static_cast<int>(d_->o_rst_elems_committed_o);
    o.rst_prefix_agree = d_->o_rst_prefix_agree_o != 0;
    o.desc_valid = d_->o_valid != 0;
    o.desc_prefix = static_cast<int>(d_->o_prefix);
    o.desc_done_ctr = static_cast<int>(d_->o_elems_done_ctr);
    o.desc_fault_valid = d_->o_fault_valid != 0;
    o.desc_fault_elem = static_cast<int>(d_->o_fault_elem);
    o.desc_fault_code = static_cast<int>(d_->o_fault_code);
    o.desc_bm_lo = d_->o_elem_bitmap_lo;
    o.desc_bm_hi = d_->o_elem_bitmap_hi;

    if (pre_req && s.mem_ready) {
      if (!pending_.empty()) {
        overlapped_ = true;
        if (pre_ordered) ordered_overlap_ = true;
      }
      Pending p;
      p.elem = pre_elem;
      p.field = pre_field;
      p.addr = pre_addr;
      p.mask = pre_mask;
      p.wdata = pre_wdata;
      p.we = pre_we;
      p.age = 0;
      pending_.push_back(p);
      LsuRec r;
      r.elem = pre_elem;
      r.field = pre_field;
      r.addr = pre_addr;
      r.mask = pre_mask;
      r.wdata = pre_wdata;
      r.we = pre_we;
      r.size = pre_size;
      r.ordered = pre_ordered;
      reqs_.push_back(r);
      if (static_cast<int>(pending_.size()) > max_outstanding_) {
        max_outstanding_ = static_cast<int>(pending_.size());
      }
    }
    if (rsp) pending_.erase(pending_.begin());
    for (size_t i = 0; i < pending_.size(); ++i) pending_[i].age++;
    o.flight = static_cast<int>(pending_.size());
    o.flight_head = pending_.empty() ? -1 : pending_.front().elem;

    clk_->Tick();
    return o;
  }

  // launch one macro and run it to completion; returns the final observation
  LsuObs Run(int mode, bool we, bool ordered, int nf, int vd, int data, int index,
             int idx_sew, uint64_t base, uint64_t stride, bool mask_en,
             int guard_cycles = 40000, uint8_t caps = 0xFF,
             bool coalesce = false, bool atomic = false) {
    reqs_.clear();
    pending_.clear();
    overlapped_ = false;
    ordered_overlap_ = false;
    max_outstanding_ = 0;

    LsuStim s;
    s.exec_valid = true;
    s.mode = mode;
    s.we = we;
    s.ordered = ordered;
    s.nf = nf;
    s.vd = vd;
    s.data = data;
    s.index = index;
    s.idx_sew = idx_sew;
    s.base = base;
    s.stride = stride;
    s.mask_en = mask_en;
    s.caps = caps;
    s.mem_ready = true;
    s.coalesce = coalesce;
    s.atomic = atomic;
    LsuObs o = Step(s);

    LsuStim idle;
    idle.caps = caps;
    idle.mem_ready = true;
    idle.coalesce = coalesce;
    idle.atomic = atomic;
    int guard = 0;
    while (!o.done && ++guard < guard_cycles) o = Step(idle);
    if (!o.done) o.busy = false;
    return o;
  }

  const std::vector<LsuRec>& reqs() const { return reqs_; }
  bool overlapped() const { return overlapped_; }
  bool ordered_overlap() const { return ordered_overlap_; }
  int max_outstanding() const { return max_outstanding_; }

 private:
  Vmosaic_vec_tb* d_;
  ClockDriver* clk_;
  LsuMem* mem_ = nullptr;
  std::vector<Pending> pending_;
  std::vector<LsuRec> reqs_;
  bool overlapped_ = false;
  bool ordered_overlap_ = false;
  int max_outstanding_ = 0;
};

// configure I-052 and capture the snapshot the packetizer executes from
void LsuConfig(Cfg* cfg, int sew_l, int vlmul, uint64_t avl, int vstart, int vta, int vma) {
  (void)RunVset(cfg, VSETVLI, 5, 6, avl, Vtypei(SewField(sew_l), vlmul, vta, vma));
  if (vstart != 0) (void)CsrWrite(cfg, kCsrVstart, static_cast<uint64_t>(vstart));
  CfgStim cap;
  cap.snap_capture = true;
  cfg->Cycle(cap);
}

// ------------------------------------------------------------- coverage state
struct LsuCoverage {
  bool mode_seen[LS_MODE_COUNT] = {};
  int cells = 0;
  int modes = 0;
};

// Compare the recorded request stream with the oracle, item by item.
void CheckLsuReqs(Reporter* rep, const std::string& name,
                  const std::vector<LsuRec>& got, const std::vector<ExpReq>& want) {
  rep->Check(got.size() == want.size(),
             name + ": " + Dec(got.size()) + " memory requests, expected " +
                 Dec(want.size()) + " (one per item, no merge)");
  size_t n = got.size() < want.size() ? got.size() : want.size();
  for (size_t i = 0; i < n; ++i) {
    const LsuRec& g = got[i];
    const ExpReq& w = want[i];
    std::string at = name + " item" + Dec(i);
    rep->Check(g.elem == w.elem && g.field == w.field,
               at + ": index (" + Dec(g.elem) + "," + Dec(g.field) + ") expected (" +
                   Dec(w.elem) + "," + Dec(w.field) + ")");
    rep->Check(g.addr == (w.addr & ~7ull),
               at + ": beat " + mosaic::Hex(g.addr, 16) + " expected " +
                   mosaic::Hex(w.addr & ~7ull, 16) + " (element at " +
                   mosaic::Hex(w.addr, 16) + ")");
    rep->Check(g.mask == w.mask,
               at + ": mask " + mosaic::Hex(g.mask, 2) + " expected " +
                   mosaic::Hex(w.mask, 2));
  }
}

// One full cell: configure, prime, run, and compare the request stream.
struct LsuRun {
  int mode = LS_UNIT;
  bool we = false;
  bool ordered = false;
  int nf = 1;
  int sew_l = 3;
  int lmul = 0;
  int idx_l = 3;
  int vd = 8, data = 16, index = 24;
  uint64_t base = 0x1000;
  int64_t stride = 0;
  int vl = 4;
  int vstart = 0;
  bool mask_en = false;
  int vta = 0, vma = 0;
  const char* tag = "";
};

void RunLsuCell(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                const LsuRun& R, bool check_dest, bool check_store, LsuCoverage* cov) {
  const std::string name = std::string("lsu ") + LsuModeName(R.mode) +
                           (R.we ? " store " : " load ") + R.tag;
  const int sew = 1 << R.sew_l;
  const int be = sew / 8;
  const int vlmax = LsuVlmaxOf(R.sew_l, R.lmul);
  const int emul = LsuEmulRegs(R.lmul);
  const int nfe = LsuIsSeg(R.mode) ? R.nf : 1;
  const bool indexed = LsuIsIndexed(R.mode) != 0;

  LsuConfig(cfg, R.sew_l, VlmulOfExp(R.lmul), static_cast<uint64_t>(R.vl), R.vstart, R.vta, R.vma);
  HostVrf vf;

  // ---- prime the mask register and remember the bits
  std::vector<bool> mbits(128, false);
  if (R.mask_en) {
    for (int b = 0; b < 16; ++b) {
      uint64_t byte = 0;
      for (int k = 0; k < 8; ++k) {
        int bit = b * 8 + k;
        bool set = (Pat(31 * R.mode + 7 * bit + 3) & 3ull) != 0;
        if (set) byte |= (1ull << k);
        mbits[static_cast<size_t>(bit)] = set;
      }
      vec->Prime(vf, 0, b, 3, 0, byte);
    }
  }

  // ---- prime the index group for an indexed access
  std::vector<uint64_t> idxv(128, 0);
  if (indexed) {
    const int idx_emul = R.lmul + (R.idx_l - R.sew_l);
    for (int i = 0; i < 128; ++i) {
      // byte offsets, naturally aligned to the element
      uint64_t off = static_cast<uint64_t>(0x100) +
                     static_cast<uint64_t>(i) * 16ull;   // well-separated, aligned
      idxv[static_cast<size_t>(i)] = off;
      if (i < vlmax) vec->Prime(vf, R.index, i, R.idx_l, idx_emul, off);
    }
  }

  // ---- prime the data (store) or destination (load) group
  const int grp = R.we ? R.data : R.vd;
  const int grp_a = LsuGrpAlign(grp, R.lmul);
  auto SetElem = [&](int field, int i, uint64_t val) {
    if (LsuIsSeg(R.mode)) {
      vec->Prime(vf, grp_a + field * emul, i, R.sew_l, R.lmul, val);
    } else {
      vec->Prime(vf, grp, i, R.sew_l, R.lmul, val);
    }
  };
  const int nfle = (R.nf == 1) ? 0 : (R.nf == 2) ? 1 : (R.nf == 4) ? 2 : 3;
  auto GetElem = [&](int field, int i) -> uint64_t {
    if (R.mode == LS_WHOLE) return vec->MemRead(grp, i, R.sew_l, nfle);
    if (LsuIsSeg(R.mode)) return vec->MemRead(grp_a + field * emul, i, R.sew_l, R.lmul);
    return vec->MemRead(grp, i, R.sew_l, R.lmul);
  };

  int nprime;
  if (R.mode == LS_WHOLE) nprime = R.nf * (16 / be);
  else if (R.mode == LS_MASK) nprime = 16;
  else nprime = vlmax;
  std::vector<std::vector<uint64_t> > oldv(static_cast<size_t>(nfe),
                                           std::vector<uint64_t>(static_cast<size_t>(nprime), 0));

  if (R.mode == LS_WHOLE) {
    for (int i = 0; i < nprime; ++i) {
      uint64_t val = Pat(97 * R.mode + 13 * i + 5) & MaskW(sew);
      vec->Prime(vf, grp, i, R.sew_l, nfle, val);
    }
  } else {
    for (int f = 0; f < nfe; ++f) {
      for (int i = 0; i < nprime; ++i) {
        uint64_t val = Pat(97 * R.mode + 131 * f + 13 * i + 5) & MaskW(sew);
        oldv[static_cast<size_t>(f)][static_cast<size_t>(i)] = val;
        SetElem(f, i, val);
      }
    }
  }

  // the configuration unit clamps vl to VLMAX, so the oracle must too
  const int vl_eff = (R.mode == LS_WHOLE || R.mode == LS_MASK)
                         ? R.vl
                         : (R.vl > vlmax ? vlmax : R.vl);

  // ---- the expected request stream
  std::vector<ExpReq> want;
  PlanLsu(R.mode, R.sew_l, R.lmul, R.nf, vl_eff, R.vstart, R.mask_en, mbits,
          R.base, R.stride, idxv, &want);

  // ---- run
  LsuObs o = lsu->Run(R.mode, R.we, R.ordered, R.nf, R.vd, R.data, R.index,
                      R.idx_l, R.base, R.stride, R.mask_en);
  rep->Check(!o.illegal, name + ": a declared mode was refused");
  rep->Check(!o.trap, name + ": the packet trapped");
  rep->Check(o.done, name + ": the packet never completed");
  // ---- the region property: no request's bytes may cross a region boundary
  for (size_t i = 0; i < lsu->reqs().size(); ++i) {
    const LsuRec& g = lsu->reqs()[i];
    uint64_t beat = g.addr & ~7ull;
    int region = -1;
    bool span = false;
    for (int k = 0; k < 8; ++k) {
      if (((g.mask >> k) & 1u) == 0) continue;
      int r = mem->RegionOf(beat + static_cast<uint64_t>(k));
      if (region < 0) region = r;
      else if (r != region) span = true;
    }
    rep->Check(!span, name + " req" + Dec(i) + ": a request spans two regions");
  }

  CheckLsuReqs(rep, name, lsu->reqs(), want);


  // ---- the store payload and the memory side
  if (R.we && check_store) {
    for (size_t i = 0; i < lsu->reqs().size() && i < want.size(); ++i) {
      const LsuRec& g = lsu->reqs()[i];
      const ExpReq& w = want[i];
      uint64_t src;
      if (R.mode == LS_WHOLE) {
        src = HostGet(vf, grp, w.elem, R.sew_l, nfle);
      } else if (LsuIsSeg(R.mode)) {
        src = HostGet(vf, grp_a + w.field * emul, w.elem, R.sew_l, R.lmul);
      } else {
        src = HostGet(vf, grp, w.elem, R.sew_l, R.lmul);
      }
      int lane = static_cast<int>(w.addr & 7ull);
      uint64_t exp_wdata = src << (8 * lane);
      rep->Check(g.wdata == exp_wdata,
                 name + " req" + Dec(i) + ": wdata " + mosaic::Hex(g.wdata, 16) +
                     " expected " + mosaic::Hex(exp_wdata, 16));
      uint64_t ev = mem->Elem(w.addr, be);
      rep->Check(ev == (src & MaskW(sew)),
                 name + " req" + Dec(i) + ": memory has " + mosaic::Hex(ev, 16) +
                     " expected " + mosaic::Hex(src & MaskW(sew), 16));
    }
  }

  // ---- the load destination
  if (!R.we && check_dest) {
    for (int f = 0; f < nfe; ++f) {
      for (int i = 0; i < nprime; ++i) {
        const uint64_t old = (R.mode == LS_WHOLE)
                                 ? 0
                                 : oldv[static_cast<size_t>(f)][static_cast<size_t>(i)];
        uint64_t exp;
        if (R.mode == LS_WHOLE) {
          exp = mem->Elem(R.base + static_cast<uint64_t>(i) * be, be) & MaskW(sew);
        } else if (R.mode == LS_MASK) {
          exp = (i < (vl_eff + 7) / 8) ? (mem->Elem(R.base + i, 1) & 0xFFull) : 0xFFull;
        } else if (i >= vl_eff) {
          exp = R.vta ? MaskW(sew) : old;
        } else if (R.mask_en && !mbits[static_cast<size_t>(i)]) {
          exp = R.vma ? MaskW(sew) : old;
        } else {
          exp = old;
          for (size_t k = 0; k < want.size(); ++k) {
            if (want[k].elem == i && want[k].field == f) {
              exp = mem->Elem(want[k].addr, be) & MaskW(sew);
              break;
            }
          }
        }
        uint64_t got = GetElem(f, i);
        rep->Check(got == exp, name + " dest f" + Dec(f) + " e" + Dec(i) + ": " +
                                  mosaic::Hex(got, 16) + " expected " +
                                  mosaic::Hex(exp, 16));
      }
    }
  }

  if (cov != nullptr) {
    if (!cov->mode_seen[R.mode]) {
      cov->mode_seen[R.mode] = true;
      cov->modes += 1;
    }
    cov->cells += 1;
  }
}

// ------------------------------------------------------------------- phases

// The device-straddling access: a byte-granular unit-stride run crosses a
// permission/MMIO boundary *inside one 8-byte beat*, so a merge could not hide:
// every request must keep its bytes on one side, the RAM elements must reach RAM
// and the device elements the device, and the request count must be the item
// count.
void PhaseLsuDeviceStraddle(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                            LsuCoverage* cov) {
  const uint64_t boundary = 0x105;   // not beat-aligned: beat 0x100 straddles it
  const uint64_t base = 0x100;
  const int vl = 8;

  for (int we = 0; we < 2; ++we) {
    mem->DeviceAt(boundary);
    mem->ResetCounters();
    LsuRun R;
    R.mode = LS_UNIT;
    R.we = (we != 0);
    R.sew_l = 3;
    R.lmul = 0;
    R.base = base;
    R.vl = vl;
    R.vd = 8;
    R.data = 16;
    R.tag = "device-straddle";
    RunLsuCell(cfg, vec, lsu, mem, rep, R, we == 0, we != 0, cov);

    std::string name = std::string("device-straddle ") + (we ? "store" : "load");
    // each element is one byte; the boundary sits between element 4 (0x104)
    // and element 5 (0x105)
    int ram = 0, dev = 0;
    bool addr_ok = true;
    for (size_t i = 0; i < lsu->reqs().size(); ++i) {
      const LsuRec& g = lsu->reqs()[i];
      if (g.mask == 0 || (g.mask & static_cast<uint8_t>(g.mask - 1)) != 0) addr_ok = false;
      int bit = 0;
      for (int k = 0; k < 8; ++k) if ((g.mask >> k) & 1u) bit = k;
      uint64_t byte = (g.addr & ~7ull) + static_cast<uint64_t>(bit);
      if (byte <= 0x104) ram += 1;
      else dev += 1;
    }
    rep->Check(addr_ok, name + ": a byte request did not enable exactly its byte");
    rep->Check(lsu->reqs().size() == static_cast<size_t>(vl),
               name + ": " + Dec(lsu->reqs().size()) + " requests for " + Dec(vl) +
                   " elements (a merge crossed the boundary)");
    rep->Check(ram == 5 && dev == 3,
               name + ": ram=" + Dec(ram) + " device=" + Dec(dev) +
                   " (expected 5 RAM and 3 device elements)");
    bool dev_in_device = true;
    for (size_t i = 0; i < lsu->reqs().size(); ++i) {
      const LsuRec& g = lsu->reqs()[i];
      uint64_t a = g.addr & ~7ull;
      for (int k = 0; k < 8; ++k) {
        if (((g.mask >> k) & 1u) == 0) continue;
        uint64_t byte = a + static_cast<uint64_t>(k);
        bool is_dev = mem->IsDevice(byte);
        if (byte >= 0x105 && !is_dev) dev_in_device = false;
        if (byte < 0x105 && is_dev) dev_in_device = false;
      }
    }
    rep->Check(dev_in_device,
               name + ": a device element was addressed in RAM or vice versa");
  }
  mem->OneRam();
}

// The byte mask must name the element's bytes at its own lane, for every width
// and every lane the width admits.
void PhaseLsuByteMask(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                      LsuCoverage* cov) {
  (void)cov;
  struct Bm { int mode; int sew_l; int lmul; int64_t stride; uint64_t base; };
  const Bm cases[] = {
      {LS_UNIT, 3, 0, 0, 0x2000},
      {LS_UNIT, 4, 0, 0, 0x2002},
      {LS_UNIT, 5, 0, 0, 0x2004},
      {LS_UNIT, 6, 0, 0, 0x2008},
      {LS_STRIDED, 4, 0, 6, 0x2012},
      {LS_STRIDED, 5, 0, 12, 0x2024},
      {LS_INDEXED, 5, 0, 0, 0x2030},
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    const Bm& b = cases[c];
    LsuRun R;
    R.mode = b.mode;
    R.we = false;
    R.sew_l = b.sew_l;
    R.lmul = b.lmul;
    R.idx_l = b.sew_l;
    R.base = b.base;
    R.stride = b.stride;
    R.vl = 4;
    R.vd = 8;
    R.data = 16;
    R.index = 24;
    R.tag = "byte-mask";
    // RunLsuCell compares every request's byte mask byte-for-byte against the
    // oracle, which is the width-and-position mask; the phase is named so a
    // lane-0 or full-width mask fails a check whose text says "byte-mask".
    RunLsuCell(cfg, vec, lsu, mem, rep, R, true, false, cov);
  }
}

// A fault must be attributed to the element that caused it: vstart is that
// element's index, the elements before it have taken effect, the faulting one
// and every later one have not.
void PhaseLsuFaultPosition(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                           LsuCoverage* cov) {
  const int vl = 6;
  const int positions[] = {0, 1, 3, 5};
  for (int pi = 0; pi < 4; ++pi) {
    const int pos = positions[pi];
    for (int we = 0; we < 2; ++we) {
      mem->OneRam();
      LsuRun R;
      R.mode = LS_UNIT;
      R.we = (we != 0);
      R.sew_l = 3;
      R.lmul = 0;
      R.base = 0x3000;
      R.vl = vl;
      R.vd = 8;
      R.data = 16;
      R.tag = "fault-position";
      const int sew = 8;
      LsuConfig(cfg, R.sew_l, VlmulOfExp(R.lmul), static_cast<uint64_t>(R.vl), 0, 0, 0);
      HostVrf vf;
      const int grp = R.we ? R.data : R.vd;
      std::vector<uint64_t> src(vl, 0);
      std::vector<uint64_t> old(vl, 0);
      for (int i = 0; i < vl; ++i) {
        uint64_t v = Pat(211 * i + 17) & MaskW(sew);
        src[static_cast<size_t>(i)] = v;
        old[static_cast<size_t>(i)] = v;
        vec->Prime(vf, grp, i, R.sew_l, R.lmul, v);
      }
      // snapshot the memory the store would have written
      std::vector<uint8_t> before(mem->mem.begin(), mem->mem.begin() + 0x3100);

      mem->fault_enable = true;
      mem->fault_all = false;
      mem->fault_elem = pos;
      mem->fault_field = 0;
      LsuObs o = lsu->Run(R.mode, R.we, R.ordered, R.nf, R.vd, R.data, R.index,
                          R.idx_l, R.base, R.stride, R.mask_en);
      mem->fault_enable = false;

      std::string name = std::string("fault-position ") + (we ? "store" : "load") +
                         " at " + Dec(pos) + " of " + Dec(vl);
      rep->Check(o.done, name + ": the packet never completed");
      rep->Check(o.trap, name + ": the fault was not reported");
      rep->Check(o.trap_elem == pos,
                 name + ": vstart " + Dec(o.trap_elem) + " expected " + Dec(pos) +
                     " (whole-macro trap or wrong element)");
      // the elements strictly before the fault are all present; a store must
      // have issued nothing past the fault (a load may have its next request
      // already on the wire, but that result is discarded)
      int n_before = 0;
      int n_after = 0;
      for (size_t i = 0; i < lsu->reqs().size(); ++i) {
        if (lsu->reqs()[i].elem < pos) n_before += 1;
        if (lsu->reqs()[i].elem > pos) n_after += 1;
      }
      rep->Check(n_before == pos,
                 name + ": " + Dec(n_before) + " requests before the fault, expected " + Dec(pos));
      if (we != 0) {
        rep->Check(n_after == 0,
                   name + ": " + Dec(n_after) + " stores were issued past the fault");
      }

      if (we != 0) {
        // the stores before the fault are in memory; the faulting one and later are not
        bool ok = true;
        for (int e = 0; e < vl; ++e) {
          uint64_t addr = R.base + static_cast<uint64_t>(e);
          uint8_t got = mem->mem[static_cast<size_t>(addr & 0xFFFFull)];
          uint8_t exp;
          if (e < pos) exp = static_cast<uint8_t>(src[static_cast<size_t>(e)] & 0xFFull);
          else exp = before[static_cast<size_t>(addr)];
          if (got != exp) ok = false;
        }
        rep->Check(ok, name + ": a store at or after the fault reached memory, or one "
                          "before it was lost");
      } else {
        bool ok = true;
        for (int e = 0; e < vl; ++e) {
          uint64_t got = vec->MemRead(grp, e, R.sew_l, R.lmul);
          uint64_t exp;
          if (e < pos) exp = mem->Elem(R.base + static_cast<uint64_t>(e), 1);
          else exp = old[static_cast<size_t>(e)];
          if (got != exp) ok = false;
        }
        rep->Check(ok, name + ": a load element at or after the fault was written, or one "
                          "before it was lost");
      }
    }
  }
  (void)cov;
}

// The ordered indexed forms must not be reordered: no request is offered until
// the previous one has completed.
void PhaseLsuOrderedOrder(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                          LsuCoverage* cov) {
  (void)cov;
  const int vl = 6;
  for (int ordered = 0; ordered < 2; ++ordered) {
    mem->OneRam();
    // the response must be slower than the index read a request costs, or the
    // ordered form would have completed before the next item was even ready
    mem->latency = 6;
    LsuRun R;
    R.mode = LS_INDEXED;
    R.we = false;
    R.ordered = (ordered != 0);
    R.sew_l = 5;
    R.lmul = 0;
    R.idx_l = 5;
    R.base = 0x4000;
    R.vl = vl;
    R.vd = 8;
    R.index = 24;
    R.tag = ordered ? "ordered" : "unordered";
    RunLsuCell(cfg, vec, lsu, mem, rep, R, true, false, cov);

    std::string name = std::string("ordered-order ") + (ordered ? "ordered" : "unordered");
    // the sequence on the memory port is ascending for both; the ordered form
    // additionally never overlaps
    bool ascending = true;
    for (size_t i = 0; i < lsu->reqs().size(); ++i) {
      if (lsu->reqs()[i].elem != static_cast<int>(i)) ascending = false;
    }
    rep->Check(ascending, name + ": the request order on the memory port is not ascending");
    if (ordered) {
      rep->Check(!lsu->ordered_overlap(),
                 name + ": a second ordered request was issued before the first completed");
      rep->Check(lsu->max_outstanding() <= 1,
                 name + ": " + Dec(lsu->max_outstanding()) + " ordered requests were outstanding");
    }
  }
  mem->latency = 2;
}

// The capability gate: each mode runs with its bit set and is refused with no
// memory transaction when the bit is clear.
void PhaseLsuCapGate(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                     LsuCoverage* cov) {
  (void)vec;
  (void)cov;
  for (int mode = 0; mode < LS_MODE_COUNT; ++mode) {
    std::string name = std::string("capability-gate ") + LsuModeName(mode);
    mem->OneRam();
    LsuRun R;
    R.mode = mode;
    R.we = false;
    R.sew_l = 3;
    R.lmul = 0;
    R.nf = (mode == LS_WHOLE) ? 2 : 1;
    R.base = 0x5000;
    R.vl = 4;
    R.vd = 8;
    R.tag = "cap";
    LsuConfig(cfg, R.sew_l, VlmulOfExp(R.lmul), static_cast<uint64_t>(R.vl), 0, 0, 0);
    uint8_t caps = static_cast<uint8_t>(0xFFu & ~(1u << mode));
    LsuObs o = lsu->Run(R.mode, R.we, R.ordered, R.nf, R.vd, R.data, R.index,
                        R.idx_l, R.base, R.stride, R.mask_en, 4000, caps);
    rep->Check(o.done, name + ": the refused packet never completed");
    rep->Check(o.illegal, name + ": a mode whose capability bit is clear was executed");
    rep->Check(lsu->reqs().empty(),
               name + ": a refused mode issued " + Dec(lsu->reqs().size()) +
                   " memory requests");
  }
}

// The bulk matrix: every mode, every direction, several widths and group
// geometries, plus accesses that cross a page and a line boundary.
void PhaseLsuCoverage(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                      LsuCoverage* cov) {
  struct Row {
    int mode; int sew_l; int lmul; int nf; int vl; uint64_t base; int64_t stride;
    bool mask_en; int vta; int vma; const char* tag;
  };
  const Row rows[] = {
      {LS_UNIT, 3, 0, 1, 8, 0x6000, 0, false, 0, 0, "sew8"},
      {LS_UNIT, 4, 0, 1, 6, 0x6000, 0, false, 0, 0, "sew16"},
      {LS_UNIT, 5, 0, 1, 4, 0x6000, 0, false, 0, 0, "sew32"},
      {LS_UNIT, 6, 0, 1, 3, 0x6000, 0, false, 0, 0, "sew64"},
      {LS_UNIT, 4, 1, 1, 4, 0x6000, 0, false, 0, 0, "lmul2"},
      {LS_UNIT, 5, -1, 1, 4, 0x6000, 0, false, 0, 0, "lmul1of2"},
      {LS_UNIT, 3, 0, 1, 8, 0x6000, 0, true, 0, 0, "masked-tail-undisturbed"},
      {LS_UNIT, 3, 0, 1, 8, 0x6000, 0, true, 1, 1, "masked-tail-agnostic"},
      {LS_UNIT, 3, 0, 1, 8, 0x6000, 0, true, 0, 1, "masked-vma1"},
      // page and line boundaries: the run crosses the edge, each element does not
      {LS_UNIT, 6, 0, 1, 4, 0x0FF8, 0, false, 0, 0, "page-cross"},
      {LS_UNIT, 3, 0, 1, 12, 0x103C, 0, false, 0, 0, "line-cross"},
      {LS_STRIDED, 4, 0, 1, 5, 0x6010, 6, false, 0, 0, "sew16"},
      {LS_STRIDED, 5, 0, 1, 4, 0x6010, 12, false, 0, 0, "sew32"},
      {LS_INDEXED, 5, 0, 1, 4, 0x6020, 0, false, 0, 0, "ei32"},
      {LS_INDEXED, 4, 0, 1, 4, 0x6020, 0, false, 0, 0, "ei16"},
      {LS_SEG_UNIT, 3, 0, 2, 4, 0x6030, 0, false, 0, 0, "nf2-sew8"},
      {LS_SEG_UNIT, 5, 0, 4, 4, 0x6030, 0, false, 0, 0, "nf4-sew32"},
      {LS_SEG_STRIDED, 4, 0, 2, 4, 0x6040, 8, false, 0, 0, "nf2-sew16"},
      {LS_SEG_INDEXED, 5, 0, 2, 4, 0x6050, 0, false, 0, 0, "nf2-sew32"},
      {LS_WHOLE, 6, 0, 1, 0, 0x6060, 0, false, 0, 0, "nf1-sew64"},
      {LS_WHOLE, 6, 0, 2, 0, 0x6060, 0, false, 0, 0, "nf2-sew64"},
      {LS_WHOLE, 6, 0, 4, 0, 0x6060, 0, false, 0, 0, "nf4-sew64"},
      {LS_WHOLE, 6, 0, 8, 0, 0x6060, 0, false, 0, 0, "nf8-sew64"},
      {LS_MASK, 3, 0, 1, 16, 0x6070, 0, false, 0, 0, "vl16"},
      {LS_MASK, 3, 0, 1, 8, 0x6070, 0, false, 0, 0, "vl8"},
  };
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
    const Row& w = rows[r];
    for (int dir = 0; dir < 2; ++dir) {
      mem->OneRam();
      LsuRun R;
      R.mode = w.mode;
      R.we = (dir != 0);
      R.ordered = false;
      R.nf = w.nf;
      R.sew_l = w.sew_l;
      R.lmul = w.lmul;
      R.idx_l = w.sew_l;
      R.vl = w.vl == 0 ? 4 : w.vl;
      R.base = w.base;
      R.stride = w.stride;
      R.mask_en = w.mask_en;
      R.vta = w.vta;
      R.vma = w.vma;
      R.vd = 8;
      R.data = 16;
      R.index = 24;
      R.tag = w.tag;
      // a masked access must not let vd overlap the mask register
      RunLsuCell(cfg, vec, lsu, mem, rep, R, dir == 0, dir != 0, cov);
    }
  }
  rep->Check(cov->cells >= 50, "coverage: only " + Dec(cov->cells) + " cells ran");
}

void RunLsuCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep, LsuCoverage* cov) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);
  LsuMem mem;
  Lsu lsu(dut, clk);
  lsu.BindMem(&mem);
  // the targeted policy phases run before the bulk sweep, so the first failure a
  // policy defect produces names the policy
  PhaseLsuDeviceStraddle(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseLsuByteMask(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseLsuFaultPosition(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseLsuOrderedOrder(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseLsuCapGate(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseLsuCoverage(&cfg, &vec, &lsu, &mem, rep, cov);

  for (int m = 0; m < LS_MODE_COUNT; ++m) {
    rep->Check(cov->mode_seen[m],
               std::string("coverage: mode ") + LsuModeName(m) + " did not run");
  }
  rep->Check(cov->modes == LS_MODE_COUNT,
             "coverage: " + Dec(cov->modes) + " modes ran, expected " + Dec(LS_MODE_COUNT));
}

// ============================================================================
// I-055 -- vector floating point and per-element flags
//           (CASE=rvv.fp_flags_reduction).
//
// The expectation is computed here from the V specification's operation table,
// with the host's floating-point unit and <fenv.h> supplying the arithmetic and
// the exception flags for the value cases (the style I-049's CASE=fp.
// operation_matrix established), a bit-level model supplying the NaN rules
// (which the host's payload-propagating NaN cannot express), and a bit-level
// model supplying the saturating fp->integer conversions (which C's conversion
// leaves undefined out of range). No comparison uses an epsilon anywhere: every
// deterministic result is compared with `==` on the bit pattern, and the
// unordered reductions are compared against an explicitly enumerated permitted
// set -- the values reachable by any valid reduction tree. That distinction is
// the point of the case: an epsilon would accept an implementation that
// reassociated an *ordered* reduction.
//
// The register file is modelled as 32 x 128-bit values and addressed with the
// same (base, element, SEW, LMUL) rule the VRF implements.
// ============================================================================

enum : int {
  FF_ELEM = 0, FF_MINMAX = 1, FF_SGNJ = 2, FF_CMP = 3, FF_CVT = 4,
  FF_WIDE = 5, FF_NARROW = 6, FF_REDSUM = 7, FF_REDMINMAX = 8, FF_REDWIDE = 9,
  FF_COUNT = 10
};
const uint64_t kAllFpCaps = (1ull << FF_COUNT) - 1ull;

const char* FpFamilyName(int f) {
  switch (f) {
    case FF_ELEM: return "elem";
    case FF_MINMAX: return "minmax";
    case FF_SGNJ: return "sgnj";
    case FF_CMP: return "cmp";
    case FF_CVT: return "cvt";
    case FF_WIDE: return "wide";
    case FF_NARROW: return "narrow";
    case FF_REDSUM: return "redsum";
    case FF_REDMINMAX: return "redminmax";
    case FF_REDWIDE: return "redwide";
    default: return "?";
  }
}

int FpFamilyOps(int f) {
  switch (f) {
    case FF_ELEM: return 6;
    case FF_MINMAX: return 2;
    case FF_SGNJ: return 3;
    case FF_CMP: return 6;
    case FF_CVT: return 4;
    case FF_WIDE: return 5;
    case FF_NARROW: return 7;
    case FF_REDSUM: return 2;
    case FF_REDMINMAX: return 2;
    case FF_REDWIDE: return 2;
    default: return 0;
  }
}

// The five RISC-V flags in their CSR bit order, {NV,DZ,OF,UF,NX}.
enum : uint32_t { FL_NX = 1u, FL_UF = 2u, FL_OF = 4u, FL_DZ = 8u, FL_NV = 16u };

std::string FlagStr(uint32_t f) {
  std::string s;
  s += (f & FL_NV) ? "N" : "-";
  s += (f & FL_DZ) ? "D" : "-";
  s += (f & FL_OF) ? "O" : "-";
  s += (f & FL_UF) ? "U" : "-";
  s += (f & FL_NX) ? "X" : "-";
  return s;
}

// ------------------------------------------------------------ bit patterns
uint32_t F32Bits(float f) { uint32_t u = 0; std::memcpy(&u, &f, 4); return u; }
float F32From(uint32_t u) { float f = 0; std::memcpy(&f, &u, 4); return f; }
uint64_t F64Bits(double d) { uint64_t u = 0; std::memcpy(&u, &d, 8); return u; }
double F64From(uint64_t u) { double d = 0; std::memcpy(&d, &u, 8); return d; }

uint64_t CanonNaN(bool fmt) {
  return fmt ? 0x0000'0000'7FC0'0000ull : 0x7FF8'0000'0000'0000ull;
}
bool IsNaNBits(uint64_t bits, bool fmt) {
  if (fmt) return ((bits >> 23) & 0xFFull) == 0xFFull && (bits & 0x7FFFFFull) != 0;
  return ((bits >> 52) & 0x7FFull) == 0x7FFull && (bits & 0xFFFFFFFFFFFFFull) != 0;
}
bool IsSNaNBits(uint64_t bits, bool fmt) {
  if (!IsNaNBits(bits, fmt)) return false;
  if (fmt) return ((bits >> 22) & 1ull) == 0;
  return ((bits >> 51) & 1ull) == 0;
}
bool SignBitOf(uint64_t bits, bool fmt) {
  return fmt ? ((bits >> 31) & 1ull) != 0 : ((bits >> 63) & 1ull) != 0;
}

int HostModeOf(int rm) {
  switch (rm) {
    case 1: return FE_TOWARDZERO;
    case 2: return FE_DOWNWARD;
    case 3: return FE_UPWARD;
    default: return FE_TONEAREST;
  }
}
uint32_t MapFlags(int f) {
  uint32_t r = 0;
  if (f & FE_INEXACT) r |= FL_NX;
  if (f & FE_UNDERFLOW) r |= FL_UF;
  if (f & FE_OVERFLOW) r |= FL_OF;
  if (f & FE_DIVBYZERO) r |= FL_DZ;
  if (f & FE_INVALID) r |= FL_NV;
  return r;
}

struct FpExp {
  uint64_t bits = 0;
  uint32_t flags = 0;
};

// op: 0 add, 1 sub, 2 mul, 3 div. The host's unit supplies the value and the
// flags; the NaN rules come from the bit model, because the host propagates a
// payload while this design returns the canonical NaN.
FpExp HostArithExp(int hop, bool fmt, uint64_t a, uint64_t b, int rm) {
  FpExp e;
  if (IsNaNBits(a, fmt) || IsNaNBits(b, fmt)) {
    e.bits = CanonNaN(fmt);
    e.flags = (IsSNaNBits(a, fmt) || IsSNaNBits(b, fmt)) ? FL_NV : 0u;
    return e;
  }
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(HostModeOf(rm));
  bool is_nan = false;
  if (fmt) {
    volatile float va = F32From(static_cast<uint32_t>(a));
    volatile float vb = F32From(static_cast<uint32_t>(b));
    float r = va;
    switch (hop) {
      case 0: r = static_cast<float>(va + vb); break;
      case 1: r = static_cast<float>(va - vb); break;
      case 2: r = static_cast<float>(va * vb); break;
      default: r = static_cast<float>(va / vb); break;
    }
    is_nan = std::isnan(r) != 0;
    e.bits = F32Bits(r);
  } else {
    volatile double va = F64From(a);
    volatile double vb = F64From(b);
    double r = va;
    switch (hop) {
      case 0: r = static_cast<double>(va + vb); break;
      case 1: r = static_cast<double>(va - vb); break;
      case 2: r = static_cast<double>(va * vb); break;
      default: r = static_cast<double>(va / vb); break;
    }
    is_nan = std::isnan(r) != 0;
    e.bits = F64Bits(r);
  }
  e.flags = MapFlags(fetestexcept(FE_ALL_EXCEPT));
  fesetround(FE_TONEAREST);
  // 0*inf, inf-inf, 0/0, inf/inf: the design returns the canonical NaN and NV.
  if (is_nan) {
    e.bits = CanonNaN(fmt);
    e.flags = FL_NV;
  }
  return e;
}

bool NumLt(uint64_t a, uint64_t b, bool fmt) {
  return fmt ? (F32From(static_cast<uint32_t>(a)) < F32From(static_cast<uint32_t>(b)))
             : (F64From(a) < F64From(b));
}
bool NumEq(uint64_t a, uint64_t b, bool fmt) {
  return fmt ? (F32From(static_cast<uint32_t>(a)) == F32From(static_cast<uint32_t>(b)))
             : (F64From(a) == F64From(b));
}

// RISC-V fmin/fmax: NaN handling and the sign of a zero result are defined by
// the ISA, not by the host's fmin/fmax, so the model is written out.
FpExp MinMaxExp(bool is_min, bool fmt, uint64_t a, uint64_t b) {
  FpExp e;
  bool na = IsNaNBits(a, fmt), nb = IsNaNBits(b, fmt);
  if (na && nb) {
    e.bits = CanonNaN(fmt);
    e.flags = (IsSNaNBits(a, fmt) || IsSNaNBits(b, fmt)) ? FL_NV : 0u;
    return e;
  }
  if (na) { e.bits = b; e.flags = IsSNaNBits(a, fmt) ? FL_NV : 0u; return e; }
  if (nb) { e.bits = a; e.flags = IsSNaNBits(b, fmt) ? FL_NV : 0u; return e; }
  if (NumLt(a, b, fmt)) { e.bits = is_min ? a : b; return e; }
  if (NumLt(b, a, fmt)) { e.bits = is_min ? b : a; return e; }
  // equal, or +0 vs -0
  bool sa = SignBitOf(a, fmt);
  e.bits = is_min ? (sa ? a : b) : (sa ? b : a);
  return e;
}

FpExp SgnjExp(int op, bool fmt, uint64_t a, uint64_t b) {
  FpExp e;
  uint64_t magmask = fmt ? 0x0000'0000'7FFFFFFFull : 0x7FFF'FFFF'FFFF'FFFFull;
  uint64_t sbit = fmt ? 0x0000'0000'80000000ull : 0x8000'0000'0000'0000ull;
  uint64_t sa = a & sbit, sb = b & sbit;
  uint64_t s = (op == 0) ? sb : (op == 1 ? (sb ^ sbit) : (sa ^ sb));
  e.bits = (a & magmask) | s;
  return e;
}

// op: 0 eq, 1 ne, 2 lt, 3 le, 4 gt, 5 ge. The NV rule is the ISA's: vmfeq and
// vmfne raise it only for a signalling NaN, the ordering compares for any NaN.
FpExp CmpExp(int op, bool fmt, uint64_t a, uint64_t b) {
  FpExp e;
  bool na = IsNaNBits(a, fmt), nb = IsNaNBits(b, fmt);
  bool sn = IsSNaNBits(a, fmt) || IsSNaNBits(b, fmt);
  bool res = false;
  if (op == 0 || op == 1) {
    e.flags = sn ? FL_NV : 0u;
    bool eq = !na && !nb && NumEq(a, b, fmt);
    res = (op == 0) ? eq : !eq;
  } else {
    e.flags = (na || nb) ? FL_NV : 0u;
    if (!na && !nb) {
      bool lt = NumLt(a, b, fmt);
      bool eq = NumEq(a, b, fmt);
      switch (op) {
        case 2: res = lt; break;
        case 3: res = lt || eq; break;
        case 4: res = NumLt(b, a, fmt); break;
        default: res = NumLt(b, a, fmt) || NumEq(b, a, fmt); break;
      }
    }
  }
  e.bits = res ? 1u : 0u;
  return e;
}

// --------------------------------------------------- saturating fp -> int
// Written from the ISA: NaN and infinity raise NV and deliver the saturating
// value with no NX; a value outside the target range raises NV and saturates;
// otherwise the value is rounded by the active mode and NX is raised when the
// rounding was inexact.
void FpToIntExp(uint64_t bits, bool fmt, int rm, bool is_signed, bool iw,
                uint64_t* out, uint32_t* flags) {
  const int fw = fmt ? 23 : 52;
  const int bias = fmt ? 127 : 1023;
  const int ebits = fmt ? 8 : 11;
  const uint64_t frac_mask = fmt ? 0x7FFFFFull : 0xFFFFFFFFFFFFFull;
  const int width = iw ? 64 : 32;
  const uint64_t umax = iw ? 0xFFFF'FFFF'FFFF'FFFFull : 0xFFFF'FFFFull;
  const uint64_t smax = iw ? 0x7FFF'FFFF'FFFF'FFFFull : 0x7FFF'FFFFull;
  const uint64_t smin_mag = iw ? 0x8000'0000'0000'0000ull : 0x8000'0000ull;

  bool sign = ((bits >> (fw + ebits)) & 1ull) != 0;
  uint64_t expf = (bits >> fw) & ((1ull << ebits) - 1ull);
  uint64_t frac = bits & frac_mask;
  *flags = 0;
  *out = 0;

  if (expf == ((1ull << ebits) - 1ull)) {          // NaN or infinity
    *flags = FL_NV;
    *out = is_signed ? (sign ? smin_mag : smax) : (sign ? 0ull : umax);
    return;
  }

  uint64_t m;
  int e2;
  if (expf == 0) { m = frac; e2 = 1 - bias - fw; }
  else { m = frac | (1ull << fw); e2 = static_cast<int>(expf) - bias - fw; }

  u128 mag = 0;
  bool inexact = false;
  if (e2 >= 64) {
    mag = (static_cast<u128>(1) << 100);           // larger than any target
  } else if (e2 >= 0) {
    mag = static_cast<u128>(m) << e2;
  } else {
    int d = -e2;
    u128 qq = 0;
    bool halfbit = false, below = false;
    if (d >= 128) {
      qq = 0;
      inexact = (m != 0);
    } else {
      qq = static_cast<u128>(m) >> d;
      u128 rr = static_cast<u128>(m) & ((static_cast<u128>(1) << d) - 1);
      inexact = (rr != 0);
      halfbit = (d - 1 < 64) && (((m >> (d - 1)) & 1ull) != 0);
      if (d >= 2) {
        int dd = d - 1;
        uint64_t lowmask = (dd >= 64) ? ~0ull : ((1ull << dd) - 1ull);
        below = (m & lowmask) != 0;
      }
    }
    bool up = false;
    switch (rm) {
      case 1: up = false; break;                              // RTZ
      case 2: up = inexact && sign; break;                    // RDN
      case 3: up = inexact && !sign; break;                   // RUP
      case 4: up = halfbit; break;                            // RMM
      default: up = halfbit && (below || ((qq & 1) != 0)); break;  // RNE
    }
    mag = qq + (up ? 1 : 0);
  }

  if (is_signed) {
    if (sign) {
      if (mag > static_cast<u128>(smin_mag)) {
        *flags = FL_NV;
        *out = smin_mag;
      } else {
        *out = static_cast<uint64_t>(-static_cast<int64_t>(mag));
        if (!inexact && false) {}
        if (inexact) *flags = FL_NX;
      }
    } else {
      if (mag > static_cast<u128>(smax)) {
        *flags = FL_NV;
        *out = smax;
      } else {
        *out = static_cast<uint64_t>(mag);
        if (inexact) *flags = FL_NX;
      }
    }
  } else {
    if (sign) {
      if (mag != 0) { *flags = FL_NV; *out = 0; }
      else { *out = 0; if (inexact) *flags = FL_NX; }
    } else {
      if (mag > static_cast<u128>(umax)) {
        *flags = FL_NV;
        *out = umax;
      } else {
        *out = static_cast<uint64_t>(mag);
        if (inexact) *flags = FL_NX;
      }
    }
  }
  *out &= (width == 64) ? ~0ull : 0xFFFF'FFFFull;
}

FpExp IntToFpExp(uint64_t value, bool is_signed, bool src64, bool fmt, int rm) {
  FpExp e;
  int64_t sv = 0;
  uint64_t uv = 0;
  if (src64) {
    sv = static_cast<int64_t>(value);
    uv = value;                              // an unsigned 64-bit source is the
  } else {                                   // whole register, not a 32-bit field
    sv = static_cast<int64_t>(static_cast<int32_t>(value));
    uv = value & 0xFFFF'FFFFull;
  }
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(HostModeOf(rm));
  if (fmt) {
    volatile float r = is_signed ? static_cast<float>(sv) : static_cast<float>(uv);
    e.bits = F32Bits(r);
  } else {
    volatile double r = is_signed ? static_cast<double>(sv) : static_cast<double>(uv);
    e.bits = F64Bits(r);
  }
  e.flags = MapFlags(fetestexcept(FE_ALL_EXCEPT));
  fesetround(FE_TONEAREST);
  return e;
}

// fp -> fp format conversion (widening is exact, narrowing rounds).
FpExp FmtCvtExp(bool src_fmt, bool dst_fmt, uint64_t bits, int rm) {
  FpExp e;
  if (IsNaNBits(bits, src_fmt)) {
    e.bits = CanonNaN(dst_fmt);
    e.flags = IsSNaNBits(bits, src_fmt) ? FL_NV : 0u;
    return e;
  }
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(HostModeOf(rm));
  if (dst_fmt) {
    float r;
    if (src_fmt) {
      r = F32From(static_cast<uint32_t>(bits));
    } else {
      volatile double d = F64From(bits);
      r = static_cast<float>(d);
    }
    e.bits = F32Bits(r);
  } else {
    double r;
    if (src_fmt) {
      r = static_cast<double>(F32From(static_cast<uint32_t>(bits)));
    } else {
      r = F64From(bits);
    }
    e.bits = F64Bits(r);
  }
  e.flags = MapFlags(fetestexcept(FE_ALL_EXCEPT));
  fesetround(FE_TONEAREST);
  return e;
}

// One element's expectation for the element-wise families. `vs2` is the source
// element, `b` is the second operand (vs1's element for .vv, the scalar for
// .vf). Conversions ignore `b`.
FpExp FpElemExp(int fam, int op, int sew_l, uint64_t vs2, uint64_t b, int rm) {
  // The oracle's convention matches the FPU's `req_fmt_i`: true is single
  // precision, which is SEW=32.
  bool fmt = (sew_l == 5);
  switch (fam) {
    case FF_ELEM:
      switch (op) {
        case 0: return HostArithExp(0, fmt, vs2, b, rm);
        case 1: return HostArithExp(1, fmt, vs2, b, rm);
        case 2: return HostArithExp(1, fmt, b, vs2, rm);   // vfrsub
        case 3: return HostArithExp(2, fmt, vs2, b, rm);
        case 4: return HostArithExp(3, fmt, vs2, b, rm);
        default: return HostArithExp(3, fmt, b, vs2, rm);  // vfrdiv
      }
    case FF_MINMAX:
      return MinMaxExp(op == 0, fmt, vs2, b);
    case FF_SGNJ:
      return SgnjExp(op, fmt, vs2, b);
    case FF_CMP:
      return CmpExp(op, fmt, vs2, b);
    case FF_CVT: {
      FpExp e;
      switch (op) {
        case 0: FpToIntExp(vs2, fmt, rm, false, !fmt, &e.bits, &e.flags); return e;
        case 1: FpToIntExp(vs2, fmt, rm, true, !fmt, &e.bits, &e.flags); return e;
        case 2: return IntToFpExp(vs2, false, !fmt, fmt, rm);
        default: return IntToFpExp(vs2, true, !fmt, fmt, rm);
      }
    }
    case FF_WIDE: {
      FpExp e;
      switch (op) {
        case 0: FpToIntExp(vs2, true, rm, false, true, &e.bits, &e.flags); return e;
        case 1: FpToIntExp(vs2, true, rm, true, true, &e.bits, &e.flags); return e;
        case 2: return IntToFpExp(vs2, false, false, false, rm);
        case 3: return IntToFpExp(vs2, true, false, false, rm);
        default: return FmtCvtExp(true, false, vs2, rm);   // vfwcvt.f.f.v
      }
    }
    default: {  // FF_NARROW
      FpExp e;
      switch (op) {
        case 0: FpToIntExp(vs2, false, rm, false, false, &e.bits, &e.flags); return e;
        case 1: FpToIntExp(vs2, false, rm, true, false, &e.bits, &e.flags); return e;
        case 2: return IntToFpExp(vs2, false, true, true, rm);
        case 3: return IntToFpExp(vs2, true, true, true, rm);
        case 4: return FmtCvtExp(false, true, vs2, rm);    // vfncvt.f.f.w
        case 5: FpToIntExp(vs2, false, 1, false, false, &e.bits, &e.flags); return e;
        default: FpToIntExp(vs2, false, 1, true, false, &e.bits, &e.flags); return e;
      }
    }
  }
}

// ------------------------------------------------- the unordered comparator
// The spec permits vfredusum/vfwredusum to produce any result reachable by a
// binary reduction tree whose nodes round an *exact* intermediate sum to a
// format at least as wide as the element format. This enumerates that set:
// every tree shape over the active elements plus the scalar accumulator, at
// both permitted internal precisions (SEW and 2*SEW), with the root rounded to
// the result format. An implementation may also add one additive identity, so a
// zero result may carry either sign.
void CollectTree(std::vector<double> vals, bool internal_wide, bool root_wide,
                 int rm, std::set<uint64_t>* out) {
  if (vals.size() == 1) {
    double v = vals[0];
    if (root_wide) {
      out->insert(F64Bits(v));
    } else {
      volatile float f = static_cast<float>(v);
      out->insert(static_cast<uint64_t>(F32Bits(f)));
    }
    return;
  }
  for (size_t i = 0; i < vals.size(); ++i) {
    for (size_t j = i + 1; j < vals.size(); ++j) {
      feclearexcept(FE_ALL_EXCEPT);
      fesetround(HostModeOf(rm));
      double s;
      if (internal_wide) {
        volatile double d = vals[i] + vals[j];
        s = d;
      } else {
        volatile float f = static_cast<float>(vals[i]) + static_cast<float>(vals[j]);
        s = static_cast<double>(f);
      }
      fesetround(FE_TONEAREST);
      std::vector<double> next;
      for (size_t k = 0; k < vals.size(); ++k) {
        if (k != i && k != j) next.push_back(vals[k]);
      }
      next.push_back(s);
      CollectTree(next, internal_wide, root_wide, rm, out);
    }
  }
}

std::set<uint64_t> UnorderedPermitted(const std::vector<double>& elems, double acc,
                                      bool root_wide, int rm) {
  std::set<uint64_t> out;
  std::vector<double> leaves;
  leaves.push_back(acc);
  for (double e : elems) leaves.push_back(e);
  if (leaves.size() == 1) {
    if (root_wide) out.insert(F64Bits(acc));
    else out.insert(static_cast<uint64_t>(F32Bits(static_cast<float>(acc))));
    return out;
  }
  CollectTree(leaves, true, root_wide, rm, &out);
  if (!root_wide) CollectTree(leaves, false, root_wide, rm, &out);
  // the additive-identity allowance: a zero result may be either signed zero
  uint64_t pz = root_wide ? 0ull : 0ull;
  uint64_t nz = root_wide ? 0x8000'0000'0000'0000ull : 0x0000'0000'8000'0000ull;
  if (out.count(pz) != 0) out.insert(nz);
  if (out.count(nz) != 0) out.insert(pz);
  return out;
}

// ------------------------------------------------------------ the harness
struct FpStim {
  uint64_t caps = kAllFpCaps;
  bool exec_valid = false;
  int family = 0, op = 0, form = 0, vd = 0, vs1 = 0, vs2 = 0;
  uint64_t scalar = 0;
  bool mask_en = false;
  int rm = 7;
  int frm = 0;
  bool commit_valid = false;
  bool flush = false;
};

struct FpObs {
  bool busy = false, done = false, illegal = false, trap = false;
  int elems = 0, cur = 0, writes = 0;
  uint64_t acc = 0;
  uint32_t pending = 0, arch = 0;
  bool commit = false;
  uint32_t commit_flags = 0;
  int commit_ctr = 0, flush_ctr = 0, spurious_ctr = 0, inactive_flag_ctr = 0;
  bool macro_pending = false;
  bool trace_valid = false;
  int trace_elem = 0;
  uint32_t trace_flags = 0;
  int issues = 0, last_lat = 0;
  int rd_gnt = 0, rd_bad = 0, wr_gnt = 0;
};

class VecFp {
 public:
  VecFp(Vmosaic_vec_tb* d, ClockDriver* clk) : d_(d), clk_(clk) {}

  FpObs Cycle(const FpStim& s) {
    d_->rst = 0;
    d_->cfg_vset_valid = 0;
    d_->cfg_snap_capture = 0;
    d_->cfg_replay_valid = 0;
    d_->cfg_exec_valid = 0;
    d_->cfg_csr_valid = 0;
    d_->mem_owner_i = 0;
    d_->mem_rd_valid_i = 0;
    d_->mem_wr_valid_i = 0;
    d_->alu_caps_i = 0x1FFFF;
    d_->alu_exec_valid_i = 0;
    d_->el_valid_i = 0;
    d_->lsu_caps_i = 0xFF;
    d_->lsu_exec_valid_i = 0;
    d_->lsu_mem_req_ready_i = 1;
    d_->lsu_mem_rsp_valid_i = 0;

    d_->fp_caps_i = static_cast<uint16_t>(s.caps & 0x3FFull);
    d_->fp_exec_valid_i = s.exec_valid ? 1 : 0;
    d_->fp_family_i = static_cast<uint8_t>(s.family & 0x1F);
    d_->fp_op_i = static_cast<uint8_t>(s.op & 0xF);
    d_->fp_form_i = static_cast<uint8_t>(s.form & 0x3);
    d_->fp_vd_i = static_cast<uint8_t>(s.vd & 0x1F);
    d_->fp_vs1_i = static_cast<uint8_t>(s.vs1 & 0x1F);
    d_->fp_vs2_i = static_cast<uint8_t>(s.vs2 & 0x1F);
    d_->fp_scalar_i = s.scalar;
    d_->fp_mask_en_i = s.mask_en ? 1 : 0;
    d_->fp_rm_i = static_cast<uint8_t>(s.rm & 0x7);
    d_->fp_frm_i = static_cast<uint8_t>(s.frm & 0x7);
    d_->fp_commit_valid_i = s.commit_valid ? 1 : 0;
    d_->fp_flush_i = s.flush ? 1 : 0;

    d_->eval();
    d_->clk = 1;
    d_->eval();
    d_->clk = 0;
    d_->eval();

    FpObs o;
    o.busy = d_->fp_busy_o != 0;
    o.done = d_->fp_done_o != 0;
    o.illegal = d_->fp_illegal_o != 0;
    o.trap = d_->fp_trap_o != 0;
    o.elems = static_cast<int>(d_->fp_elems_o);
    o.cur = static_cast<int>(d_->fp_cur_o);
    o.writes = static_cast<int>(d_->fp_writes_o);
    o.acc = d_->fp_acc_o;
    o.pending = d_->fp_fflags_pending_o;
    o.arch = d_->fp_fflags_arch_o;
    o.commit = d_->fp_commit_o != 0;
    o.commit_flags = d_->fp_commit_fflags_o;
    o.commit_ctr = static_cast<int>(d_->fp_commit_ctr_o);
    o.flush_ctr = static_cast<int>(d_->fp_flush_ctr_o);
    o.spurious_ctr = static_cast<int>(d_->fp_spurious_ctr_o);
    o.inactive_flag_ctr = static_cast<int>(d_->fp_inactive_flag_ctr_o);
    o.macro_pending = d_->fp_macro_pending_o != 0;
    o.trace_valid = d_->fp_flag_trace_valid_o != 0;
    o.trace_elem = static_cast<int>(d_->fp_flag_trace_elem_o);
    o.trace_flags = d_->fp_flag_trace_flags_o;
    o.issues = static_cast<int>(d_->fp_fpu_issues_o);
    o.last_lat = static_cast<int>(d_->fp_last_latency_o);
    o.rd_gnt = static_cast<int>(d_->vrf_rd_gnt_ctr_o);
    o.rd_bad = static_cast<int>(d_->vrf_rd_bad_ctr_o);
    o.wr_gnt = static_cast<int>(d_->vrf_wr_gnt_ctr_o);

    clk_->Tick();
    return o;
  }

  // Launch one packet and run to completion, collecting the flag trace.
  FpObs Run(const FpStim& s, std::vector<std::pair<int, uint32_t> >* trace = nullptr) {
    FpStim launch = s;
    launch.exec_valid = true;
    FpObs o = Cycle(launch);
    FpStim idle;
    idle.caps = s.caps;
    idle.frm = s.frm;
    int guard = 0;
    while (!o.done && ++guard < 200000) {
      if (trace != nullptr && o.trace_valid) {
        trace->push_back(std::make_pair(o.trace_elem, o.trace_flags));
      }
      o = Cycle(idle);
    }
    if (trace != nullptr && o.trace_valid) {
      trace->push_back(std::make_pair(o.trace_elem, o.trace_flags));
    }
    return o;
  }

  // One cycle with the packet engine idle (used for commit/flush strobes).
  FpObs Idle(const FpStim& s) { return Cycle(s); }

  // Drive every fp input to its idle value for one cycle. The driver's VRF
  // priming uses the `Vec` helper, which does not drive the fp ports, so a
  // strobe asserted for one cycle would otherwise persist through the priming
  // cycles that follow.
  void Quiesce() { FpStim s; (void)Cycle(s); }

  int Issues() { return static_cast<int>(d_->fp_fpu_issues_o); }
  uint64_t Now() { return clk_->cycle(); }

 private:
  Vmosaic_vec_tb* d_;
  ClockDriver* clk_;
};

// ------------------------------------------------------------ coverage
struct FpCov {
  bool cell[FF_COUNT][2][2] = {};   // family x SEW(32,64) x form/element
  int cells = 0;
};

// Configure the I-052 unit and capture the snapshot the FP unit executes from.
void FpConfig(Cfg* cfg, int sew_l, int vlmul, int vta, int vma, uint64_t avl) {
  ConfigureVec(cfg, SewField(sew_l), vlmul, vta, vma, avl);
}

// Interesting operands for a width: numbers, zeros, infinities and the two NaN
// classes, so the NaN rules and the flags are exercised, not merely the values.
std::vector<uint64_t> FpValues(int sew_l) {
  std::vector<uint64_t> v;
  if (sew_l == 5) {
    const uint64_t f[] = {0x3F800000ull, 0x40000000ull, 0x3F000000ull,
                          0xBFC00000ull, 0x40600000ull, 0x7F800000ull,
                          0xFF800000ull, 0x00000000ull, 0x80000000ull,
                          0x7FC00000ull, 0x7FA00000ull, 0x00000001ull,
                          0x7F7FFFFFull, 0x4B000000ull};
    for (uint64_t x : f) v.push_back(x);
  } else {
    const uint64_t f[] = {0x3FF0000000000000ull, 0x4000000000000000ull,
                          0x3FE0000000000000ull, 0xBFF8000000000000ull,
                          0x400C000000000000ull, 0x7FF0000000000000ull,
                          0xFFF0000000000000ull, 0x0000000000000000ull,
                          0x8000000000000000ull, 0x7FF8000000000000ull,
                          0x7FF4000000000000ull, 0x0000000000000001ull,
                          0x7FEFFFFFFFFFFFFFull, 0x4330000000000000ull};
    for (uint64_t x : f) v.push_back(x);
  }
  return v;
}

int FpVlmax(int sew_l, int lmul_e) {
  int e = 7 + lmul_e - sew_l;
  return e < 0 ? 0 : (1 << e);
}

// One element-wise packet: prime the operands, run, and compare every
// destination element and the aggregate flags with the oracle.
void RunFpElemPacket(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep, FpCov* cov,
                     int fam, int op, int form, int sew_l, int lmul_e,
                     const std::vector<uint64_t>& va,
                     const std::vector<uint64_t>& vb, uint64_t scalar, int rm,
                     bool mask_en, uint64_t mbits) {
  const int vlmax = FpVlmax(sew_l, lmul_e);
  const int vd = 8, vs1 = 16, vs2 = 24;
  const std::string name = std::string("fpelem ") + FpFamilyName(fam) + " op" +
                           Dec(op) + " sew" + Dec(1 << sew_l) + " form" + Dec(form);
  FpConfig(cfg, sew_l, VlmulOfExp(lmul_e), 0, 0, 64);

  HostVrf vf;
  // The source element width is SEW for most families and 2*SEW for a narrowing
  // conversion (vfncvt reads a double-width source), so the prime follows the
  // source's own (SEW, LMUL), not the destination's.
  const int sw = (fam == FF_NARROW) ? (sew_l + 1) : sew_l;
  const int sl = (fam == FF_NARROW) ? (lmul_e + 1) : lmul_e;
  for (int i = 0; i < vlmax; ++i) {
    uint64_t a = va[static_cast<size_t>(i % va.size())];
    uint64_t b = (form == 0) ? vb[static_cast<size_t>(i % vb.size())] : scalar;
    vec->Prime(vf, vs2, i, sw, sl, a);
    if (form == 0) vec->Prime(vf, vs1, i, sw, sl, b);
  }
  if (mask_en) {
    for (int byte = 0; byte < 16; ++byte) {
      uint64_t val = (mbits >> (8 * byte)) & 0xFFull;
      vec->Prime(vf, 0, byte, 3, 0, val);
    }
  }
  if (fam == FF_CMP) {
    for (int i = 0; i < 16; ++i) vec->Prime(vf, vd, i, 3, 0, 0x5A5A5A5A5A5A5A5Aull);
  }

  FpStim s;
  s.family = fam; s.op = op; s.form = form; s.vd = vd; s.vs1 = vs1; s.vs2 = vs2;
  s.scalar = scalar; s.mask_en = mask_en; s.rm = rm;
  std::vector<std::pair<int, uint32_t> > trace;
  FpObs o = fp->Run(s, &trace);

  rep->Check(!o.illegal, name + ": a declared family was refused");
  rep->Check(!o.trap, name + ": the packet trapped");

  // The per-element flag contribution, in element order, over active elements
  // only: the trace names the element and its flags, so a wrong flag is
  // attributed to the element that produced it.
  for (size_t t = 0; t < trace.size(); ++t) {
    int idx = trace[t].first;
    bool act = (idx < vlmax) && (!mask_en || (((mbits >> idx) & 1ull) != 0));
    uint32_t ef = 0;
    if (act) {
      uint64_t a = va[static_cast<size_t>(idx % va.size())];
      uint64_t b = (form == 0) ? vb[static_cast<size_t>(idx % vb.size())] : scalar;
      ef = FpElemExp(fam, op, sew_l, a, b, rm).flags;
    }
    rep->Check(act && (trace[t].second == ef),
               name + " e" + Dec(idx) + " flags " + FlagStr(trace[t].second) +
                   " expected " + FlagStr(ef));
  }

  uint32_t exp_flags = 0;
  int active = 0;
  for (int i = 0; i < vlmax; ++i) {
    bool act = true;
    if (mask_en) act = ((mbits >> i) & 1ull) != 0;
    uint64_t a = va[static_cast<size_t>(i % va.size())];
    uint64_t b = (form == 0) ? vb[static_cast<size_t>(i % vb.size())] : scalar;
    FpExp e = FpElemExp(fam, op, sew_l, a, b, rm);
    if (act) {
      active += 1;
      exp_flags |= e.flags;
    }
    if (fam == FF_CMP) {
      // the destination bit is the comparison result for an active element
      bool want = act ? (e.bits != 0) : ((mbits >> i) & 1ull) != 0;
      (void)want;
    }
  }
  if (fam == FF_CMP) {
    // The destination is a mask register. Elements 0..vlmax-1 are written;
    // the rest of the byte was primed and must be undisturbed, which is what
    // proves the read-modify-write. vma/vta are 0 here, so an inactive
    // element's bit is undisturbed too.
    const uint64_t primed = 0x5Aull;
    uint64_t want = 0;
    for (int i = 0; i < 8; ++i) {
      bool bit;
      if (i < vlmax) {
        bool act = !mask_en || (((mbits >> i) & 1ull) != 0);
        if (act) {
          uint64_t a = va[static_cast<size_t>(i % va.size())];
          uint64_t b = (form == 0) ? vb[static_cast<size_t>(i % vb.size())] : scalar;
          bit = FpElemExp(fam, op, sew_l, a, b, rm).bits != 0;
        } else {
          bit = ((primed >> i) & 1ull) != 0;   // undisturbed
        }
      } else {
        bit = ((primed >> i) & 1ull) != 0;     // outside the destination group
      }
      want |= (bit ? 1ull : 0ull) << i;
    }
    uint64_t got = vec->Peek(vd, 0, 3, 0) & 0xFFull;
    rep->Check(got == want, name + ": mask byte " + mosaic::Hex(got, 2) +
                                " expected " + mosaic::Hex(want, 2));
    // the neighbouring bytes were primed and must be untouched
    rep->Check((vec->Peek(vd, 1, 3, 0) & 0xFFull) == primed,
               name + ": a neighbouring mask byte was disturbed");
  } else {
    for (int i = 0; i < vlmax; ++i) {
      bool act = !mask_en || (((mbits >> i) & 1ull) != 0);
      uint64_t a = va[static_cast<size_t>(i % va.size())];
      uint64_t b = (form == 0) ? vb[static_cast<size_t>(i % vb.size())] : scalar;
      FpExp e = FpElemExp(fam, op, sew_l, a, b, rm);
      int dw = (fam == FF_WIDE) ? (sew_l + 1) : sew_l;
      int dl = (fam == FF_WIDE) ? (lmul_e + 1) : lmul_e;
      uint64_t got = vec->Peek(vd, i, dw, dl);
      uint64_t want;
      if (act) want = e.bits & MaskW(1 << dw);
      else want = MaskW(1 << dw);   // vma/vta agnostic: all-ones
      rep->Check(got == want, name + " e" + Dec(i) + ": got " +
                                  mosaic::Hex(got, 16) + " expected " +
                                  mosaic::Hex(want, 16) + " [a=" +
                                  mosaic::Hex(a, 16) + " b=" +
                                  mosaic::Hex(b, 16) + "]");
    }
  }
  rep->Check(o.pending == exp_flags,
             name + ": pending flags " + FlagStr(o.pending) + " expected " +
                 FlagStr(exp_flags));
  rep->Check(o.elems == active,
             name + ": " + Dec(o.elems) + " active elements, expected " + Dec(active));
  rep->Check(o.inactive_flag_ctr == 0,
             name + ": an inactive element contributed a flag");
  if (cov != nullptr) {
    int wi = (sew_l == 6) ? 1 : 0;
    if (!cov->cell[fam][wi][form]) {
      cov->cell[fam][wi][form] = true;
      cov->cells += 1;
    }
  }
}

// -------------------------------------------------------- phase 1: element lane
void PhaseFpElementLane(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep, FpCov* cov) {
  const int fams[] = {FF_ELEM, FF_MINMAX, FF_SGNJ};
  for (int sew_l = 5; sew_l <= 6; ++sew_l) {
    std::vector<uint64_t> vals = FpValues(sew_l);
    for (int fam : fams) {
      for (int op = 0; op < FpFamilyOps(fam); ++op) {
        for (int form = 0; form <= 1; ++form) {
          for (size_t k = 0; k < vals.size(); ++k) {
            std::vector<uint64_t> va, vb;
            va.push_back(vals[k]);
            vb.push_back(vals[(k + 5) % vals.size()]);
            // fill the rest of the group with a second pair
            va.push_back(vals[(k + 3) % vals.size()]);
            vb.push_back(vals[(k + 9) % vals.size()]);
            uint64_t scalar = vals[(k + 7) % vals.size()];
            RunFpElemPacket(cfg, vec, fp, rep, cov, fam, op, form, sew_l, 0,
                            va, vb, scalar, 0, false, 0);
          }
        }
      }
    }
  }
  // one LMUL=2 cell at SEW=32 to exercise grouping
  std::vector<uint64_t> vals = FpValues(5);
  for (int op = 0; op < 6; ++op) {
    RunFpElemPacket(cfg, vec, fp, rep, cov, FF_ELEM, op, 0, 5, 1, vals, vals,
                    0, 0, false, 0);
  }
}

// -------------------------------------------------------- phase 2: masked sNaN
// Both directions: an active sNaN sets NV and yields the canonical NaN; a
// masked-off sNaN sets nothing and performs no operation.
void PhaseFpMaskedSNaN(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep) {
  const int sew_l = 5;
  const int vd = 8, vs1 = 16, vs2 = 24;
  const uint64_t snan = 0x7FA00000ull;      // signalling NaN, single
  const uint64_t one = 0x3F800000ull;
  const uint64_t two = 0x40000000ull;

  // (a) active sNaN: element 0 is sNaN, unmasked
  {
    FpConfig(cfg, sew_l, 0, 0, 0, 64);
    HostVrf vf;
    for (int i = 0; i < 4; ++i) {
      vec->Prime(vf, vs2, i, sew_l, 0, (i == 0) ? snan : one);
      vec->Prime(vf, vs1, i, sew_l, 0, two);
    }
    FpStim s;
    s.family = FF_ELEM; s.op = 0; s.form = 0; s.vd = vd; s.vs1 = vs1; s.vs2 = vs2;
    const int i0 = fp->Issues();
    FpObs o = fp->Run(s);
    const int i1 = fp->Issues();
    rep->Check((o.pending & FL_NV) != 0,
               "masked-snan active: an active sNaN did not set NV");
    uint64_t got = vec->Peek(vd, 0, sew_l, 0);
    rep->Check(got == 0x7FC00000ull,
               "masked-snan active: result " + mosaic::Hex(got, 16) +
                   " expected the canonical quiet NaN");
    rep->Check(o.elems == 4, "masked-snan active: 4 elements expected, got " +
                                 Dec(o.elems));
    rep->Check(i1 - i0 == 4, "masked-snan active: 4 FPU operations expected, got " +
                                 Dec(i1 - i0));
  }

  // (b) the same sNaN masked off: no NV, no operation, destination undisturbed
  {
    FpConfig(cfg, sew_l, 0, 0, 0, 64);
    HostVrf vf;
    for (int i = 0; i < 4; ++i) {
      vec->Prime(vf, vs2, i, sew_l, 0, (i == 1) ? snan : one);
      vec->Prime(vf, vs1, i, sew_l, 0, two);
    }
    // mask register v0: bit 1 clear, bits 0/2/3 set
    uint64_t mbits = 0x0Dull;   // 0b1101
    vec->Prime(vf, 0, 0, 3, 0, mbits & 0xFFull);
    vec->Prime(vf, 0, 1, 3, 0, (mbits >> 8) & 0xFFull);
    // destination primed with a sentinel so "undisturbed" is observable
    for (int i = 0; i < 4; ++i) vec->Prime(vf, vd, i, sew_l, 0, 0xDEADBEEFull);

    FpStim s;
    s.family = FF_ELEM; s.op = 0; s.form = 0; s.vd = vd; s.vs1 = vs1; s.vs2 = vs2;
    s.mask_en = true;
    const int j0 = fp->Issues();
    FpObs o = fp->Run(s);
    const int j1 = fp->Issues();
    rep->Check((o.pending & FL_NV) == 0,
               "masked-snan inactive: a masked-off sNaN polluted NV");
    rep->Check(o.pending == 0,
               "masked-snan inactive: pending flags " + FlagStr(o.pending) +
                   " expected none");
    rep->Check(o.elems == 3, "masked-snan inactive: 3 active elements expected, got " +
                                 Dec(o.elems));
    rep->Check(j1 - j0 == 3,
               "masked-snan inactive: 3 FPU operations expected (the masked-off "
               "element must not compute), got " + Dec(j1 - j0));
    rep->Check(o.inactive_flag_ctr == 0,
               "masked-snan inactive: an inactive element contributed a flag");
    uint64_t got = vec->Peek(vd, 1, sew_l, 0);
    rep->Check(got == 0xDEADBEEFull,
               "masked-snan inactive: the masked-off destination was disturbed: " +
                   mosaic::Hex(got, 16));
  }
}

// -------------------------------------------------------- phase 3: conversions
void PhaseFpConversions(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep, FpCov* cov) {
  // same-width conversions at both SEW widths
  for (int sew_l = 5; sew_l <= 6; ++sew_l) {
    std::vector<uint64_t> vals = FpValues(sew_l);
    for (int op = 0; op < 4; ++op) {
      for (size_t k = 0; k < vals.size(); ++k) {
        std::vector<uint64_t> va, vb;
        va.push_back(vals[k]);
        va.push_back(vals[(k + 6) % vals.size()]);
        vb.push_back(0);
        vb.push_back(0);
        RunFpElemPacket(cfg, vec, fp, rep, cov, FF_CVT, op, 0, sew_l, 0, va, vb,
                        0, 0, false, 0);
      }
    }
  }
  // widening conversions (SEW=32 -> 64)
  {
    std::vector<uint64_t> vals = FpValues(5);
    for (int op = 0; op < FpFamilyOps(FF_WIDE); ++op) {
      for (size_t k = 0; k < vals.size(); ++k) {
        std::vector<uint64_t> va, vb;
        va.push_back(vals[k]);
        va.push_back(vals[(k + 4) % vals.size()]);
        vb.push_back(0);
        vb.push_back(0);
        RunFpElemPacket(cfg, vec, fp, rep, cov, FF_WIDE, op, 0, 5, 0, va, vb, 0,
                        0, false, 0);
      }
    }
  }
  // narrowing conversions (source 64 -> 32)
  {
    std::vector<uint64_t> vals = FpValues(6);
    for (int op = 0; op < FpFamilyOps(FF_NARROW); ++op) {
      for (size_t k = 0; k < vals.size(); ++k) {
        std::vector<uint64_t> va, vb;
        va.push_back(vals[k]);
        va.push_back(vals[(k + 4) % vals.size()]);
        vb.push_back(0);
        vb.push_back(0);
        RunFpElemPacket(cfg, vec, fp, rep, cov, FF_NARROW, op, 0, 5, 0, va, vb,
                        0, 0, false, 0);
      }
    }
  }
}

// -------------------------------------------------------- phase 3b: compares
// The six FP comparisons produce a mask, and their NV rule is the ISA's:
// vmfeq/vmfne raise it only for a signalling NaN, the ordering compares for any
// NaN. The NaN operands in the value set exercise exactly that split.
void PhaseFpCompare(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep, FpCov* cov) {
  for (int sew_l = 5; sew_l <= 6; ++sew_l) {
    std::vector<uint64_t> vals = FpValues(sew_l);
    for (int op = 0; op < FpFamilyOps(FF_CMP); ++op) {
      for (int form = 0; form <= 1; ++form) {
        for (size_t k = 0; k < vals.size(); ++k) {
          std::vector<uint64_t> va, vb;
          va.push_back(vals[k]);
          vb.push_back(vals[(k + 5) % vals.size()]);
          va.push_back(vals[(k + 3) % vals.size()]);
          vb.push_back(vals[(k + 9) % vals.size()]);
          uint64_t scalar = vals[(k + 7) % vals.size()];
          RunFpElemPacket(cfg, vec, fp, rep, cov, FF_CMP, op, form, sew_l, 0,
                          va, vb, scalar, 0, false, 0);
        }
      }
    }
  }
}

// -------------------------------------------------------- phase 4: reductions
// The ordered reductions are checked bit-exactly against the serial fold in
// element order; the unordered ones against the enumerated permitted set. The
// element vectors are chosen so that the left fold and the right fold differ,
// which is what makes the bit-exact check discriminating: an implementation
// that reassociated an ordered reduction would fail it, and a case that compared
// with an epsilon would not.
void RunFpReduction(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep, FpCov* cov,
                    int fam, int op, int sew_l, int lmul_e, bool mask_en,
                    uint64_t mbits, const std::vector<uint64_t>& elems,
                    uint64_t acc0) {
  const bool wide = (fam == FF_REDWIDE);
  // The oracle's convention matches the FPU's: true is single (SEW=32).
  const bool fmt = (sew_l == 5);
  const int vlmax = FpVlmax(sew_l, lmul_e);
  const int vd = 8, vs1 = 16, vs2 = 24;
  const int acc_sew_l = wide ? (sew_l + 1) : sew_l;
  const int acc_lmul = wide ? (lmul_e + 1) : lmul_e;
  const std::string name = std::string("fpred ") + FpFamilyName(fam) + " op" +
                           Dec(op) + " sew" + Dec(1 << sew_l);
  FpConfig(cfg, sew_l, VlmulOfExp(lmul_e), 0, 0, 64);

  HostVrf vf;
  vec->Prime(vf, vs1, 0, acc_sew_l, acc_lmul, acc0);
  for (int i = 0; i < vlmax; ++i) {
    vec->Prime(vf, vs2, i, sew_l, lmul_e, elems[static_cast<size_t>(i % elems.size())]);
  }
  if (mask_en) {
    vec->Prime(vf, 0, 0, 3, 0, mbits & 0xFFull);
    vec->Prime(vf, 0, 1, 3, 0, (mbits >> 8) & 0xFFull);
  }

  FpStim s;
  s.family = fam; s.op = op; s.vd = vd; s.vs1 = vs1; s.vs2 = vs2;
  s.mask_en = mask_en; s.rm = 0;
  std::vector<std::pair<int, uint32_t> > trace;
  FpObs o = fp->Run(s, &trace);

  rep->Check(!o.illegal, name + ": refused");

  // the serial, ascending fold, element by element
  uint64_t acc = acc0;
  uint32_t exp_flags = 0;
  std::vector<double> elems_d;
  int active = 0;
  for (int i = 0; i < vlmax; ++i) {
    bool act = !mask_en || (((mbits >> i) & 1ull) != 0);
    if (!act) continue;
    active += 1;
    uint64_t e = elems[static_cast<size_t>(i % elems.size())];
    if (wide) {
      double ed = static_cast<double>(F32From(static_cast<uint32_t>(e)));
      elems_d.push_back(ed);
      FpExp add = HostArithExp(0, false, acc, F64Bits(ed), 0);
      acc = add.bits;
      exp_flags |= add.flags;
    } else if (fam == FF_REDMINMAX) {
      FpExp mm = MinMaxExp(op == 0, fmt, acc, e);
      acc = mm.bits;
      exp_flags |= mm.flags;
      elems_d.push_back(fmt ? static_cast<double>(F32From(static_cast<uint32_t>(e)))
                            : F64From(e));
    } else {
      FpExp add = HostArithExp(0, fmt, acc, e, 0);
      acc = add.bits;
      exp_flags |= add.flags;
      elems_d.push_back(fmt ? static_cast<double>(F32From(static_cast<uint32_t>(e)))
                            : F64From(e));
    }
  }

  // the ordering is observable through the flag trace: ascending from vstart
  bool ascending = true;
  int prev = -1;
  for (auto& t : trace) {
    if (t.first <= prev) ascending = false;
    prev = t.first;
  }
  rep->Check(ascending, name + ": the element fold order was not ascending");

  // the right fold, for the discriminating-vector property
  uint64_t acc_r = acc0;
  for (int i = vlmax - 1; i >= 0; --i) {
    bool act = !mask_en || (((mbits >> i) & 1ull) != 0);
    if (!act) continue;
    uint64_t e = elems[static_cast<size_t>(i % elems.size())];
    if (wide) {
      double ed = static_cast<double>(F32From(static_cast<uint32_t>(e)));
      acc_r = HostArithExp(0, false, acc_r, F64Bits(ed), 0).bits;
    } else if (fam == FF_REDMINMAX) {
      acc_r = MinMaxExp(op == 0, fmt, acc_r, e).bits;
    } else {
      acc_r = HostArithExp(0, fmt, acc_r, e, 0).bits;
    }
  }

  const bool ordered = (op == 0);
  if (fam == FF_REDMINMAX) {
    rep->Check(o.acc == acc, name + ": acc " + mosaic::Hex(o.acc, 16) +
                                 " expected " + mosaic::Hex(acc, 16));
    rep->Check(o.pending == exp_flags, name + ": flags " + FlagStr(o.pending) +
                                           " expected " + FlagStr(exp_flags));
  } else if (ordered) {
    // deterministic: bit-exact, and the vectors make the association visible
    if (!mask_en) {
      rep->Check(acc != acc_r,
                 name + " ordered: the chosen vectors do not distinguish the two "
                 "associations, so the bit-exact check proves nothing");
    }
    rep->Check(o.acc == acc, name + " ordered: acc " + mosaic::Hex(o.acc, 16) +
                                 " expected " + mosaic::Hex(acc, 16) +
                                 " (bit-exact; the reassociated value is " +
                                 mosaic::Hex(acc_r, 16) + ")");
    if (!mask_en) {
      rep->Check(o.acc != acc_r,
                 name + " ordered: the result equals the reassociated fold");
    }
    rep->Check(o.pending == exp_flags, name + " ordered: flags " +
                                           FlagStr(o.pending) + " expected " +
                                           FlagStr(exp_flags));
  } else {
    // permitted to differ: the result must be in the enumerated tree set
    double accd = wide ? F64From(acc0)
                       : (fmt ? static_cast<double>(F32From(static_cast<uint32_t>(acc0)))
                              : F64From(acc0));
    std::set<uint64_t> permit = UnorderedPermitted(elems_d, accd, wide, 0);
    uint64_t got = wide ? o.acc : (o.acc & MaskW(1 << sew_l));
    bool in_set = permit.count(got) != 0;
    std::string list;
    for (uint64_t x : permit) {
      if (list.size() < 200) list += " " + mosaic::Hex(x, 16);
    }
    rep->Check(in_set, name + " unordered: result " + mosaic::Hex(got, 16) +
                           " is not in the permitted set {" + list + " }");
    // the permitted set is a finite set of bit patterns, not a tolerance: for
    // these vectors it holds more than one value, so the comparator is a real
    // membership test.
    rep->Check(permit.size() >= 2, name + " unordered: the permitted set holds " +
                                       Dec(static_cast<int>(permit.size())) +
                                       " value(s), so the vectors do not exercise "
                                       "a permitted difference");
  }
  rep->Check(o.elems == active, name + ": " + Dec(o.elems) + " folds, expected " +
                                    Dec(active));
  if (cov != nullptr) {
    int wi = (sew_l == 6) ? 1 : 0;
    if (!cov->cell[fam][wi][0]) { cov->cell[fam][wi][0] = true; cov->cells += 1; }
  }
}

void PhaseFpReductions(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep, FpCov* cov) {
  // SEW=32: 1.0, 2^24, 1.0, -2^24 -- the left fold is 0 and the right fold is 1.
  const std::vector<uint64_t> v32 = {0x3F800000ull, 0x4B800000ull,
                                     0x3F800000ull, 0xCB800000ull};
  // SEW=64 (LMUL=2 so the group holds four elements): 1.0, 2^53, 1.0, -2^53.
  const std::vector<uint64_t> v64 = {0x3FF0000000000000ull, 0x4340000000000000ull,
                                     0x3FF0000000000000ull, 0xC340000000000000ull};
  // widening reduction: 1.0, 2^60, 1.0, -2^60 -- the f64 fold loses the 1s.
  const std::vector<uint64_t> vw = {0x3F800000ull, 0x5D800000ull,
                                    0x3F800000ull, 0xDD800000ull};

  for (int op = 0; op < 2; ++op) {
    RunFpReduction(cfg, vec, fp, rep, cov, FF_REDSUM, op, 5, 0, false, 0, v32, 0);
    RunFpReduction(cfg, vec, fp, rep, cov, FF_REDSUM, op, 6, 1, false, 0, v64, 0);
    RunFpReduction(cfg, vec, fp, rep, cov, FF_REDMINMAX, op, 5, 0, false, 0, v32, 0);
    RunFpReduction(cfg, vec, fp, rep, cov, FF_REDMINMAX, op, 6, 1, false, 0, v64, 0);
    RunFpReduction(cfg, vec, fp, rep, cov, FF_REDWIDE, op, 5, 0, false, 0, vw, 0);
  }
  // a masked reduction: element 2 is masked off, so the fold runs over elements
  // 0, 1 and 3 -- still a discriminating set (the left fold is 0, the right 1).
  RunFpReduction(cfg, vec, fp, rep, cov, FF_REDSUM, 0, 5, 0, true, 0x0Bull, v32, 0);
  RunFpReduction(cfg, vec, fp, rep, cov, FF_REDSUM, 1, 5, 0, true, 0x0Bull, v32, 0);
}

// -------------------------------------------------------- phase 5: rounding
// A tie is rounded differently by different modes; vxrm is a fixed-point field
// and must have no effect on the floating-point path.
void PhaseFpRounding(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep) {
  const int sew_l = 5;
  const int vd = 8, vs1 = 16, vs2 = 24;
  const uint64_t one = 0x3F800000ull;
  const uint64_t tie = 0x33000000ull;   // 2^-25: 1.0 + 2^-25 is a tie in single
  const int modes[4] = {0, 3, 2, 1};    // RNE, RUP, RDN, RTZ
  const uint64_t want[4] = {0x3F800000ull, 0x3F800001ull, 0x3F800000ull, 0x3F800000ull};
  for (int mi = 0; mi < 4; ++mi) {
    FpConfig(cfg, sew_l, 0, 0, 0, 64);
    HostVrf vf;
    for (int i = 0; i < 4; ++i) {
      vec->Prime(vf, vs2, i, sew_l, 0, one);
      vec->Prime(vf, vs1, i, sew_l, 0, tie);
    }
    FpStim s;
    s.family = FF_ELEM; s.op = 0; s.form = 0; s.vd = vd; s.vs1 = vs1; s.vs2 = vs2;
    s.rm = 7;             // dynamic: take frm
    s.frm = modes[mi];
    FpObs o = fp->Run(s);
    uint64_t got = vec->Peek(vd, 0, sew_l, 0);
    rep->Check(got == want[mi], std::string("round frm") + Dec(modes[mi]) +
                                   ": got " + mosaic::Hex(got, 16) + " expected " +
                                   mosaic::Hex(want[mi], 16));
    rep->Check((o.pending & FL_NX) != 0, std::string("round frm") + Dec(modes[mi]) +
                                             ": an inexact tie did not set NX");
  }
  // the same tie with vxrm varying: the result must not move
  for (int vxrm = 0; vxrm < 4; ++vxrm) {
    FpConfig(cfg, sew_l, 0, 0, 0, 64);
    (void)CsrWrite(cfg, kCsrVxrm, static_cast<uint64_t>(vxrm));
    CfgStim cap;
    cap.snap_capture = true;
    cfg->Cycle(cap);
    HostVrf vf;
    for (int i = 0; i < 4; ++i) {
      vec->Prime(vf, vs2, i, sew_l, 0, one);
      vec->Prime(vf, vs1, i, sew_l, 0, tie);
    }
    FpStim s;
    s.family = FF_ELEM; s.op = 0; s.form = 0; s.vd = vd; s.vs1 = vs1; s.vs2 = vs2;
    s.rm = 7; s.frm = 3;   // RUP
    (void)fp->Run(s);
    uint64_t got = vec->Peek(vd, 0, sew_l, 0);
    rep->Check(got == 0x3F800001ull,
               std::string("round vxrm") + Dec(vxrm) +
                   ": the fixed-point rounding field changed an FP result: " +
                   mosaic::Hex(got, 16));
  }
}

// -------------------------------------------------- phase 6: flag aggregate
// A whole macro, including masked-off elements, and the commit boundary.
void PhaseFpFlagAggregate(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep) {
  const int sew_l = 5;
  const int vd = 8, vs1 = 16, vs2 = 24;
  const uint64_t snan = 0x7FA00000ull;
  const uint64_t inf = 0x7F800000ull;
  const uint64_t ninf = 0xFF800000ull;
  const uint64_t one = 0x3F800000ull;

  // A macro with a masked-off sNaN and an active inf + (-inf) (which sets NV):
  // the aggregate is the OR over the active elements only, and nothing is
  // architectural until the macro commits.
  FpConfig(cfg, sew_l, 0, 0, 0, 64);
  HostVrf vf;
  for (int i = 0; i < 4; ++i) {
    vec->Prime(vf, vs2, i, sew_l, 0, (i == 0) ? inf : ((i == 1) ? snan : one));
    vec->Prime(vf, vs1, i, sew_l, 0, (i == 0) ? ninf : one);
  }
  vec->Prime(vf, 0, 0, 3, 0, 0x0Dull);   // element 1 masked off
  FpStim s;
  s.family = FF_ELEM; s.op = 0; s.form = 0; s.vd = vd; s.vs1 = vs1; s.vs2 = vs2;
  s.mask_en = true;
  FpObs o = fp->Run(s);
  uint32_t want = FL_NV;   // inf + (-inf) on element 0 only
  rep->Check(o.pending == want, "agg: pending " + FlagStr(o.pending) + " expected " +
                                    FlagStr(want));
  rep->Check(o.elems == 3, "agg: 3 active elements expected, got " + Dec(o.elems));
  rep->Check(o.inactive_flag_ctr == 0, "agg: an inactive element contributed a flag");

  // the premature-flag fail mode: nothing architectural before the commit
  FpStim idle;
  FpObs before = fp->Idle(idle);
  rep->Check(before.arch == 0,
             "agg: flags became architectural before the macro retired: " +
                 FlagStr(before.arch));
  rep->Check(before.macro_pending, "agg: a completed macro is not pending");

  // the commit
  FpStim cm;
  cm.commit_valid = true;
  FpObs after = fp->Idle(cm);
  rep->Check(after.commit, "agg: the commit did not take effect");
  rep->Check(after.arch == want, "agg: architectural flags " + FlagStr(after.arch) +
                                     " expected " + FlagStr(want));
  rep->Check(after.commit_ctr == 1, "agg: commit counter " + Dec(after.commit_ctr) +
                                        " expected 1");
  rep->Check(after.pending == 0, "agg: pending flags were not cleared at commit");
  // clear the strobe: the driver's VRF priming does not drive the fp inputs, so
  // a stale `commit_valid` would be a second (spurious) commit.
  fp->Quiesce();

  // a squashed macro contributes nothing: run a flag-producing macro, flush it,
  // and require the architectural flags to be unchanged.
  FpConfig(cfg, sew_l, 0, 0, 0, 64);
  HostVrf vf2;
  for (int i = 0; i < 4; ++i) {
    vec->Prime(vf2, vs2, i, sew_l, 0, inf);
    vec->Prime(vf2, vs1, i, sew_l, 0, ninf);
  }
  FpStim s2;
  s2.family = FF_ELEM; s2.op = 0; s2.form = 0; s2.vd = vd; s2.vs1 = vs1; s2.vs2 = vs2;
  FpObs o2 = fp->Run(s2);
  rep->Check(o2.pending == FL_NV, "agg squash: pending NV expected");
  FpStim fl;
  fl.flush = true;
  FpObs after_flush = fp->Idle(fl);
  rep->Check(after_flush.pending == 0, "agg squash: the flush did not clear pending");
  rep->Check(after_flush.arch == want, "agg squash: a squashed macro changed the "
                                       "architectural flags: " + FlagStr(after_flush.arch));
  rep->Check(after_flush.flush_ctr == 1, "agg squash: flush counter " +
                                             Dec(after_flush.flush_ctr) + " expected 1");
  fp->Quiesce();

  // a commit with no completed macro is spurious and changes nothing
  FpStim sp;
  sp.commit_valid = true;
  FpObs spurious = fp->Idle(sp);
  rep->Check(spurious.arch == want, "agg: a spurious commit changed the flags");
  rep->Check(spurious.spurious_ctr == 1, "agg: spurious commit counter " +
                                             Dec(spurious.spurious_ctr) + " expected 1");
  fp->Quiesce();
}

// -------------------------------------------------------- phase 7: capability
void PhaseFpCapGate(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep) {
  const int sew_l = 5;
  FpConfig(cfg, sew_l, 0, 0, 0, 64);
  HostVrf vf;
  for (int i = 0; i < 4; ++i) {
    vec->Prime(vf, 16, i, sew_l, 0, 0x3F800000ull);
    vec->Prime(vf, 24, i, sew_l, 0, 0x40000000ull);
  }
  // every declared family runs
  for (int fam = 0; fam < FF_COUNT; ++fam) {
    FpStim s;
    s.family = fam; s.op = 0; s.vd = 8; s.vs1 = 16; s.vs2 = 24;
    FpObs o = fp->Run(s);
    rep->Check(!o.illegal, std::string("capgate: family ") + FpFamilyName(fam) +
                               " was refused although declared");
  }
  // each family is refused with its bit clear and issues no VRF transaction
  for (int fam = 0; fam < FF_COUNT; ++fam) {
    FpStim s;
    s.family = fam; s.op = 0; s.vd = 8; s.vs1 = 16; s.vs2 = 24;
    s.caps = kAllFpCaps & ~(1ull << fam);
    FpObs o = fp->Run(s);
    rep->Check(o.illegal, std::string("capgate: family ") + FpFamilyName(fam) +
                              " ran with its capability bit clear");
  }
  // an unknown family id is refused
  {
    FpStim s;
    s.family = 15; s.op = 0; s.vd = 8; s.vs1 = 16; s.vs2 = 24;
    FpObs o = fp->Run(s);
    rep->Check(o.illegal, "capgate: an unknown family id was not refused");
  }
  // an operation outside the family's table is refused
  {
    FpStim s;
    s.family = FF_MINMAX; s.op = 5; s.vd = 8; s.vs1 = 16; s.vs2 = 24;
    FpObs o = fp->Run(s);
    rep->Check(o.illegal, "capgate: an unknown operation id was not refused");
  }
  // SEW=16 is refused: the F/D datapath has no half format
  {
    FpConfig(cfg, 4, 0, 0, 0, 64);
    FpStim s;
    s.family = FF_ELEM; s.op = 0; s.vd = 8; s.vs1 = 16; s.vs2 = 24;
    FpObs o = fp->Run(s);
    rep->Check(o.illegal, "capgate: SEW=16 was not refused");
  }
  // widening at LMUL=8 is refused: the effective LMUL would be 16
  {
    FpConfig(cfg, 5, 3, 0, 0, 64);
    FpStim s;
    s.family = FF_WIDE; s.op = 4; s.vd = 8; s.vs1 = 16; s.vs2 = 24;
    FpObs o = fp->Run(s);
    rep->Check(o.illegal, "capgate: widening at LMUL=8 was not refused");
  }
}

// -------------------------------------------------------- phase 8: latency
// The FPU's declared latency is captured and checked against the declaration
// (1 cycle for everything but fdiv, 66 for fdiv), and a divide packet is shown
// to take many more cycles than an add packet, which is the evidence that the
// engine actually waits for the datapath rather than assuming an answer.
void PhaseFpLatency(Cfg* cfg, Vec* vec, VecFp* fp, Reporter* rep) {
  const int sew_l = 5;
  FpConfig(cfg, sew_l, 0, 0, 0, 64);
  HostVrf vf;
  for (int i = 0; i < 4; ++i) {
    vec->Prime(vf, 16, i, sew_l, 0, 0x3F800000ull);
    vec->Prime(vf, 24, i, sew_l, 0, 0x40000000ull);
  }
  FpStim add;
  add.family = FF_ELEM; add.op = 0; add.vd = 8; add.vs1 = 16; add.vs2 = 24;
  const int ia0 = fp->Issues();
  const uint64_t ca0 = fp->Now();
  FpObs oa = fp->Run(add);
  const int ia1 = fp->Issues();
  const uint64_t ca1 = fp->Now();
  rep->Check(oa.last_lat == 1, "latency: an add declared latency " +
                                   Dec(oa.last_lat) + " expected 1");
  rep->Check(ia1 - ia0 == 4, "latency: " + Dec(ia1 - ia0) +
                                 " FPU operations for a 4-element add, expected 4");

  FpStim div;
  div.family = FF_ELEM; div.op = 4; div.vd = 8; div.vs1 = 16; div.vs2 = 24;
  const int id0 = fp->Issues();
  const uint64_t cd0 = fp->Now();
  FpObs od = fp->Run(div);
  const int id1 = fp->Issues();
  const uint64_t cd1 = fp->Now();
  rep->Check(od.last_lat == 66, "latency: a divide declared latency " +
                                    Dec(od.last_lat) + " expected 66");
  rep->Check(id1 - id0 == 4, "latency: " + Dec(id1 - id0) +
                                 " FPU operations for a 4-element divide, expected 4");
  rep->Check((cd1 - cd0) > (ca1 - ca0),
             "latency: the divide packet did not take longer than the add packet (" +
                 Dec(static_cast<int>(cd1 - cd0)) + " vs " +
                 Dec(static_cast<int>(ca1 - ca0)) + " cycles)");
}

// ------------------------------------------------------------------- runner
void RunVecFpCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep, FpCov* cov) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);
  VecFp fp(dut, clk);
  // settle the FP inputs before anything else runs
  {
    FpStim idle;
    (void)fp.Cycle(idle);
  }
  PhaseFpElementLane(&cfg, &vec, &fp, rep, cov);
  PhaseFpMaskedSNaN(&cfg, &vec, &fp, rep);
  PhaseFpConversions(&cfg, &vec, &fp, rep, cov);
  PhaseFpCompare(&cfg, &vec, &fp, rep, cov);
  PhaseFpRounding(&cfg, &vec, &fp, rep);
  PhaseFpFlagAggregate(&cfg, &vec, &fp, rep);
  PhaseFpReductions(&cfg, &vec, &fp, rep, cov);
  PhaseFpCapGate(&cfg, &vec, &fp, rep);
  PhaseFpLatency(&cfg, &vec, &fp, rep);

  for (int f = 0; f < FF_COUNT; ++f) {
    bool any = cov->cell[f][0][0] || cov->cell[f][0][1] || cov->cell[f][1][0] ||
               cov->cell[f][1][1];
    rep->Check(any, std::string("coverage: family ") + FpFamilyName(f) +
                        " was never exercised");
  }
}

// ============================================================================
// I-057 -- the partial-trap / `vstart` / fault-only-first controller.
//
// The oracle below is written from the pinned V spec's rules (quoted in
// rtl/core/mosaic_vec_restart.sv), not read from the DUT: a rule set that agreed
// with the DUT by construction would agree with a wrong DUT.  The expected
// `vstart`, `vl` trim, committed element set, store side effects and destination
// state are computed here, and the DUT's descriptor bitmap is only ever
// *compared* against them.
// ============================================================================

// the controller's fault classes (rtl/core/mosaic_vec_restart.sv)
enum : int { RC_NONE = 0, RC_PAGE = 1, RC_ACCESS = 2, RC_INTR = 3, RC_OTHER = 4 };

const char* RstCodeName(int c) {
  switch (c) {
    case RC_NONE: return "none";
    case RC_PAGE: return "page";
    case RC_ACCESS: return "access";
    case RC_INTR: return "interrupt";
    default: return "other";
  }
}

// the four forms this package claims for the boundary sweep
enum : int {
  RF_UNIT_LOAD = 0, RF_UNIT_STORE = 1, RF_STRIDED_LOAD = 2, RF_STRIDED_STORE = 3,
  RF_FORM_COUNT = 4
};

struct RstForm {
  int mode;
  bool we;
  int64_t stride;
  const char* name;
};

const RstForm kRstForms[RF_FORM_COUNT] = {
    {LS_UNIT,    false, 0, "unit-load"},
    {LS_UNIT,    true,  0, "unit-store"},
    {LS_STRIDED, false, 8, "strided-load"},
    {LS_STRIDED, true,  8, "strided-store"},
};

// ------------------------------------------------------------- the host oracle
struct RstExpect {
  bool trap = false;
  int vstart = 0;
  int trap_code = RC_NONE;
  bool vl_write = false;
  int vl_new = 0;
  bool complete = true;
  int committed = 0;
};

// The specification's rule, applied on the host:
//   * a synchronous fault at element k: `vstart` = k, the elements strictly
//     before k took effect, no trim (R1);
//   * a fault-only-first unit-stride load: element 0 raises the trap with `vl`
//     unmodified; element k > 0 is absorbed with `vl` = k and no trap (R3);
//   * anything else -- a store, a strided form, an interrupt -- always traps.
RstExpect HostRst(int mode, bool we, bool fof, int vl, int vstart, int fault_elem,
                  int fault_code) {
  RstExpect e;
  if (fault_elem < 0) {
    e.complete = true;
    e.committed = vl - vstart;
    return e;
  }
  e.committed = fault_elem - vstart;
  const bool absorb = fof && !we && (mode == LS_UNIT) && (fault_elem > 0) &&
                      (fault_code == RC_PAGE || fault_code == RC_ACCESS);
  if (absorb) {
    e.trap = false;
    e.vl_write = true;
    e.vl_new = fault_elem;
    e.complete = true;
  } else {
    e.trap = true;
    e.vstart = fault_elem;
    e.trap_code = fault_code;
    e.complete = false;
  }
  return e;
}

// -------------------------------------------------------------- the runner
struct RstCfg {
  int mode = LS_UNIT;
  bool we = false;
  bool fof = false;
  int sew_l = 3, lmul = 0, idx_l = 3;
  int vd = 8, data = 16, index = 24;
  uint64_t base = 0x8000;
  int64_t stride = 0;
  int vl = 6;
  bool mask_en = false;
  int vta = 0, vma = 0;
};

void RstFillStim(LsuStim* s, const RstCfg& R, bool exec) {
  s->exec_valid = exec;
  s->mode = R.mode;
  s->we = R.we;
  s->fof = R.fof;
  s->nf = 1;
  s->vd = R.vd;
  s->data = R.data;
  s->index = R.index;
  s->idx_sew = R.idx_l;
  s->base = R.base;
  s->stride = static_cast<uint64_t>(R.stride);
  s->mask_en = R.mask_en;
  s->bind = true;
  s->mem_ready = true;
}

void RstAlloc(Lsu* lsu, int vl, int vstart, uint64_t vtype) {
  LsuStim s;
  s.bind = true;
  s.desc_alloc = true;
  s.desc_alloc_vl = vl;
  s.desc_alloc_vstart = vstart;
  s.desc_alloc_vtype = vtype;
  lsu->Step(s);
}

void RstRelease(Lsu* lsu) {
  LsuStim s;
  s.bind = true;
  s.desc_release = true;
  lsu->Step(s);
}

void RstFaultClear(Lsu* lsu) {
  LsuStim s;
  s.bind = true;
  s.desc_fault_clear = true;
  lsu->Step(s);
}

struct RstResult {
  LsuObs o;
  bool early_complete = false;
};

// Run one macro to completion (the drain included) and then let the controller
// finish its walk over the committed elements.  `fault_elem` < 0 injects no
// fault; `intr_at` >= 0 paces the memory so exactly that many elements are
// performed and then requests a boundary stop.
RstResult RstRunMacro(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, const RstCfg& R,
                      int vstart, int fault_elem, int fault_code, int intr_at) {
  (void)vec;
  LsuConfig(cfg, R.sew_l, VlmulOfExp(R.lmul), static_cast<uint64_t>(R.vl), vstart,
            R.vta, R.vma);
  mem->fault_enable = (fault_elem >= 0);
  mem->fault_all = false;
  mem->fault_elem = fault_elem;
  mem->fault_field = 0;

  LsuStim s;
  RstFillStim(&s, R, true);
  s.fault_code = fault_code;
  LsuObs o = lsu->Step(s);

  RstResult res;
  bool stopping = false;
  int guard = 0;
  while (!o.done && ++guard < 40000) {
    if (o.rst_complete && o.busy) res.early_complete = true;
    LsuStim t;
    RstFillStim(&t, R, false);
    t.fault_code = fault_code;
    if (intr_at >= 0) {
      if (o.elems >= intr_at) stopping = true;
      if (stopping) {
        t.intr = true;
        t.mem_ready = false;
      }
    }
    o = lsu->Step(t);
  }
  // let the controller walk the committed elements into the descriptor
  while (o.rst_busy && ++guard < 40000) {
    LsuStim t;
    RstFillStim(&t, R, false);
    o = lsu->Step(t);
  }
  mem->fault_enable = false;
  res.o = o;
  return res;
}

struct RstCoverage {
  int cells = 0;
  bool form_pos[RF_FORM_COUNT][16] = {};
  int intr_cells = 0;
  int fof_cells = 0;
  int restart_cells = 0;
};

// --------------------------------------------------------------- phases

// R4: a macro is complete only after the packetizer drained every response, not
// when the last request was offered.  This phase runs a clean macro and requires
// that `o_complete_o` never stands while the packetizer is busy.
void PhaseRstRetireGate(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                        RstCoverage* cov) {
  (void)cov;
  const int vl = 6;
  mem->OneRam();
  mem->ResetCounters();
  RstCfg R;
  R.mode = LS_UNIT; R.we = false; R.vl = vl; R.base = 0x8000;
  lsu->ClearReqs();
  RstAlloc(lsu, vl, 0, Vtypei(SewField(R.sew_l), R.lmul));
  HostVrf vf;
  for (int i = 0; i < vl; ++i) vec->Prime(vf, R.vd, i, R.sew_l, R.lmul, Pat(31 * i + 11) & 0xFFull);
  RstResult res = RstRunMacro(cfg, vec, lsu, mem, R, 0, -1, RC_NONE, -1);
  rep->Check(!res.early_complete,
             "retire-gate: o_complete_o stood while the packetizer was busy -- the "
             "last packet had arrived, the responses had not drained");
  rep->Check(res.o.rst_complete && res.o.rst_retire_ok,
             "retire-gate: a clean macro did not complete");
  rep->Check(!res.o.rst_trap, "retire-gate: a clean macro raised a trap");
  rep->Check(res.o.rst_prefix_agree, "retire-gate: bitmap and prefix disagree");
  rep->Check(res.o.desc_prefix == vl,
             "retire-gate: prefix " + Dec(res.o.desc_prefix) + " expected " + Dec(vl));
  RstRelease(lsu);
}

// A fault at *every* legal element boundary, for each claimed form and both
// fault classes: `vstart` is the faulting index, the elements before it took
// effect, the faulting one and later did not, and the descriptor's bitmap and
// fault record say so.
void PhaseRstFaultBoundaries(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                             RstCoverage* cov) {
  const int vl = 6;
  const int codes[2] = {RC_PAGE, RC_ACCESS};
  for (int f = 0; f < RF_FORM_COUNT; ++f) {
    const RstForm& F = kRstForms[f];
    for (int pos = 0; pos < vl; ++pos) {
      for (int ci = 0; ci < 2; ++ci) {
        const int code = codes[ci];
        mem->OneRam();
        mem->ResetCounters();
        RstCfg R;
        R.mode = F.mode; R.we = F.we; R.stride = F.stride; R.vl = vl;
        R.base = 0x8000 + 0x100 * f + 0x10 * pos + ci;
        lsu->ClearReqs();
        RstAlloc(lsu, vl, 0, Vtypei(SewField(R.sew_l), R.lmul));
        HostVrf vf;
        const int grp = R.we ? R.data : R.vd;
        std::vector<uint64_t> val(static_cast<size_t>(vl), 0);
        for (int i = 0; i < vl; ++i) {
          val[static_cast<size_t>(i)] = Pat(97 * f + 13 * i + 5) & 0xFFull;
          vec->Prime(vf, grp, i, R.sew_l, R.lmul, val[static_cast<size_t>(i)]);
        }
        std::vector<uint8_t> before(mem->mem.begin(), mem->mem.begin() + 0x9000);

        RstResult res = RstRunMacro(cfg, vec, lsu, mem, R, 0, pos, code, -1);
        const RstExpect ex = HostRst(R.mode, R.we, false, vl, 0, pos, code);
        const std::string name = std::string("fault-boundary ") + F.name + " at " +
                                 Dec(pos) + " code " + RstCodeName(code);

        rep->Check(res.o.trap && res.o.trap_elem == pos,
                   name + ": packetizer vstart " + Dec(res.o.trap_elem) + " expected " + Dec(pos));
        rep->Check(res.o.trap_code == code,
                   name + ": packetizer class " + RstCodeName(res.o.trap_code) + " expected " +
                       RstCodeName(code));
        rep->Check(res.o.rst_trap && res.o.rst_vstart == ex.vstart,
                   name + ": controller trap=" + Dec(res.o.rst_trap) + " vstart=" +
                       Dec(res.o.rst_vstart) + " expected 1/" + Dec(ex.vstart));
        rep->Check(res.o.rst_trap_code == ex.trap_code,
                   name + ": controller class " + RstCodeName(res.o.rst_trap_code) + " expected " +
                       RstCodeName(ex.trap_code));
        rep->Check(!res.o.rst_vl_write && !res.o.rst_fof_trim,
                   name + ": a macro that must trap trimmed vl");
        rep->Check(!res.o.rst_complete && !res.o.rst_retire_ok,
                   name + ": a trapped macro was called complete");
        rep->Check(res.o.rst_elems_committed == ex.committed,
                   name + ": committed " + Dec(res.o.rst_elems_committed) + " expected " +
                       Dec(ex.committed));
        rep->Check(res.o.desc_prefix == pos && res.o.desc_fault_valid &&
                       res.o.desc_fault_elem == pos && res.o.desc_fault_code == code,
                   name + ": descriptor prefix=" + Dec(res.o.desc_prefix) + " fault=" +
                       Dec(res.o.desc_fault_valid) + "/" + Dec(res.o.desc_fault_elem) + "/" +
                       RstCodeName(res.o.desc_fault_code));
        rep->Check(res.o.rst_prefix_agree, name + ": bitmap and prefix disagree");

        bool ok = true;
        std::string detail;
        for (int e = 0; e < vl; ++e) {
          const uint64_t addr = R.base + static_cast<uint64_t>(e) *
                                (F.mode == LS_UNIT ? 1ull : static_cast<uint64_t>(R.stride));
          if (R.we) {
            const uint8_t got = mem->mem[static_cast<size_t>(addr & 0xFFFFull)];
            const uint8_t want = (e < pos)
                                     ? static_cast<uint8_t>(val[static_cast<size_t>(e)])
                                     : before[static_cast<size_t>(addr)];
            if (got != want) { ok = false; detail += " mem" + Dec(e); }
          } else {
            const uint64_t got = vec->MemRead(grp, e, R.sew_l, R.lmul);
            const uint64_t want = (e < pos) ? mem->Elem(addr, 1) : val[static_cast<size_t>(e)];
            if (got != want) { ok = false; detail += " dest" + Dec(e); }
          }
        }
        rep->Check(ok, name + ": the partial state is wrong:" + detail);

        cov->form_pos[f][pos] = true;
        cov->cells += 1;
        RstRelease(lsu);
      }
    }
  }
}

// A precise interrupt at an element boundary: the packetizer stops at the
// boundary, the controller traps with `vstart` = the first unperformed element
// and never trims `vl`, and the elements before the boundary took effect.
void PhaseRstInterrupt(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                       RstCoverage* cov) {
  const int vl = 6;
  for (int b = 0; b < vl; ++b) {
    mem->OneRam();
    mem->ResetCounters();
    RstCfg R;
    R.mode = LS_UNIT; R.we = false; R.vl = vl; R.base = 0x9000;
    lsu->ClearReqs();
    RstAlloc(lsu, vl, 0, Vtypei(SewField(R.sew_l), R.lmul));
    HostVrf vf;
    std::vector<uint64_t> val(static_cast<size_t>(vl), 0);
    for (int i = 0; i < vl; ++i) {
      val[static_cast<size_t>(i)] = Pat(41 * i + 3) & 0xFFull;
      vec->Prime(vf, R.vd, i, R.sew_l, R.lmul, val[static_cast<size_t>(i)]);
    }
    RstResult res = RstRunMacro(cfg, vec, lsu, mem, R, 0, -1, RC_NONE, b);
    const std::string name = "interrupt at boundary " + Dec(b) + " of " + Dec(vl);

    rep->Check(res.o.stopped && res.o.stop_elem == b,
               name + ": packetizer stopped=" + Dec(res.o.stopped) + " at " +
                   Dec(res.o.stop_elem) + " expected " + Dec(b));
    rep->Check(res.o.rst_trap && res.o.rst_trap_code == RC_INTR && res.o.rst_vstart == b,
               name + ": controller trap=" + Dec(res.o.rst_trap) + " class=" +
                   RstCodeName(res.o.rst_trap_code) + " vstart=" + Dec(res.o.rst_vstart) +
                   " expected trap=1 class=interrupt vstart=" + Dec(b));
    rep->Check(!res.o.rst_vl_write, name + ": an interrupt trimmed vl");
    rep->Check(res.o.desc_prefix == b && res.o.desc_fault_valid &&
                   res.o.desc_fault_code == RC_INTR,
               name + ": descriptor prefix=" + Dec(res.o.desc_prefix) + " class=" +
                   RstCodeName(res.o.desc_fault_code));
    bool ok = true;
    for (int e = 0; e < vl; ++e) {
      const uint64_t got = vec->MemRead(R.vd, e, R.sew_l, R.lmul);
      const uint64_t want = (e < b) ? mem->Elem(R.base + static_cast<uint64_t>(e), 1)
                                    : val[static_cast<size_t>(e)];
      if (got != want) ok = false;
    }
    rep->Check(ok, name + ": the partial destination state is wrong");
    cov->intr_cells += 1;
    RstRelease(lsu);
  }
}

// Fault-only-first, the sharp edge: element 0 raises the trap with `vl`
// unmodified; a later element is absorbed with `vl` = the faulting index and no
// trap; a strided form or a store is never absorbed.
void PhaseRstFof(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                 RstCoverage* cov) {
  const int vl = 6;
  // (a) element 0 faults: the trap is taken and `vl` is not modified
  for (int ci = 0; ci < 2; ++ci) {
    const int code = (ci == 0) ? RC_PAGE : RC_ACCESS;
    mem->OneRam();
    mem->ResetCounters();
    RstCfg R;
    R.mode = LS_UNIT; R.we = false; R.fof = true; R.vl = vl; R.base = 0xA000 + 0x20 * ci;
    lsu->ClearReqs();
    RstAlloc(lsu, vl, 0, Vtypei(SewField(R.sew_l), R.lmul));
    HostVrf vf;
    std::vector<uint64_t> val(static_cast<size_t>(vl), 0);
    for (int i = 0; i < vl; ++i) {
      val[static_cast<size_t>(i)] = Pat(29 * i + 9) & 0xFFull;
      vec->Prime(vf, R.vd, i, R.sew_l, R.lmul, val[static_cast<size_t>(i)]);
    }
    RstResult res = RstRunMacro(cfg, vec, lsu, mem, R, 0, 0, code, -1);
    const std::string name = std::string("fof element-0 fault code ") + RstCodeName(code);
    rep->Check(res.o.rst_trap && res.o.rst_vstart == 0,
               name + ": trap=" + Dec(res.o.rst_trap) + " vstart=" + Dec(res.o.rst_vstart) +
                   " (the specification takes the trap on element 0)");
    rep->Check(!res.o.rst_vl_write && !res.o.rst_fof_trim,
               name + ": element 0 was absorbed instead of trapping");
    rep->Check(res.o.desc_fault_valid && res.o.desc_fault_elem == 0,
               name + ": no fault was recorded");
    cov->fof_cells += 1;
    RstRelease(lsu);
  }

  // (b) a later element faults: absorbed, `vl` shortened, no trap
  for (int k = 1; k < vl; ++k) {
    for (int ci = 0; ci < 2; ++ci) {
      const int code = (ci == 0) ? RC_PAGE : RC_ACCESS;
      mem->OneRam();
      mem->ResetCounters();
      RstCfg R;
      R.mode = LS_UNIT; R.we = false; R.fof = true; R.vl = vl;
      R.base = 0xB000 + 0x40 * k + 0x10 * ci;
      lsu->ClearReqs();
      RstAlloc(lsu, vl, 0, Vtypei(SewField(R.sew_l), R.lmul));
      HostVrf vf;
      std::vector<uint64_t> val(static_cast<size_t>(vl), 0);
      for (int i = 0; i < vl; ++i) {
        val[static_cast<size_t>(i)] = Pat(37 * i + 13) & 0xFFull;
        vec->Prime(vf, R.vd, i, R.sew_l, R.lmul, val[static_cast<size_t>(i)]);
      }
      RstResult res = RstRunMacro(cfg, vec, lsu, mem, R, 0, k, code, -1);
      const RstExpect ex = HostRst(R.mode, R.we, true, vl, 0, k, code);
      const std::string name = "fof later fault at " + Dec(k) + " code " + RstCodeName(code);

      rep->Check(!res.o.rst_trap && res.o.rst_vl_write && res.o.rst_fof_trim &&
                     res.o.rst_vl_new == ex.vl_new,
                 name + ": trap=" + Dec(res.o.rst_trap) + " vl_write=" +
                     Dec(res.o.rst_vl_write) + " vl_new=" + Dec(res.o.rst_vl_new) +
                     " expected no trap and vl=" + Dec(ex.vl_new));
      rep->Check(res.o.rst_complete && res.o.rst_retire_ok && !res.o.rst_restart_ready,
                 name + ": a trimmed macro did not complete normally");
      rep->Check(res.o.rst_elems_committed == k,
                 name + ": committed " + Dec(res.o.rst_elems_committed) + " expected " + Dec(k));
      rep->Check(res.o.desc_prefix == k && !res.o.desc_fault_valid,
                 name + ": prefix=" + Dec(res.o.desc_prefix) + " fault=" +
                     Dec(res.o.desc_fault_valid));
      bool ok = true;
      for (int e = 0; e < vl; ++e) {
        const uint64_t got = vec->MemRead(R.vd, e, R.sew_l, R.lmul);
        const uint64_t want = (e < k) ? mem->Elem(R.base + static_cast<uint64_t>(e), 1)
                                      : val[static_cast<size_t>(e)];
        if (got != want) ok = false;
      }
      rep->Check(ok, name + ": the faulting element or a later one was written, or an "
                        "earlier one was lost");
      cov->fof_cells += 1;
      RstRelease(lsu);
    }
  }

  // (c) only the unit-stride load form may absorb: a strided form and a store
  //     raise the fault
  struct Neg { int mode; bool we; const char* name; };
  const Neg negs[2] = {{LS_STRIDED, false, "fof strided load"},
                       {LS_UNIT, true, "fof store"}};
  for (int n = 0; n < 2; ++n) {
    mem->OneRam();
    mem->ResetCounters();
    RstCfg R;
    R.mode = negs[n].mode; R.we = negs[n].we; R.fof = true; R.vl = vl;
    R.base = 0xC000 + 0x100 * n; R.stride = 8;
    lsu->ClearReqs();
    RstAlloc(lsu, vl, 0, Vtypei(SewField(R.sew_l), R.lmul));
    HostVrf vf;
    const int grp = R.we ? R.data : R.vd;
    for (int i = 0; i < vl; ++i) vec->Prime(vf, grp, i, R.sew_l, R.lmul, Pat(17 * i + 1) & 0xFFull);
    RstResult res = RstRunMacro(cfg, vec, lsu, mem, R, 0, 3, RC_PAGE, -1);
    rep->Check(res.o.rst_trap && !res.o.rst_vl_write,
               std::string(negs[n].name) + ": absorbed a fault it may not (trap=" +
                   Dec(res.o.rst_trap) + " vl_write=" + Dec(res.o.rst_vl_write) + ")");
    RstRelease(lsu);
  }
}

// R2: a restart must not duplicate an irreversible side effect.  A store that
// already wrote its early elements is restarted from the controller's restart
// point; every byte is written exactly once across the fault and the restart.
void PhaseRstRestart(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                     RstCoverage* cov) {
  const int vl = 6;
  const int ks[3] = {0, 2, 5};
  for (int ki = 0; ki < 3; ++ki) {
    const int k = ks[ki];
    mem->OneRam();
    mem->ResetCounters();
    RstCfg R;
    R.mode = LS_UNIT; R.we = true; R.vl = vl; R.base = 0xD000;
    lsu->ClearReqs();
    RstAlloc(lsu, vl, 0, Vtypei(SewField(R.sew_l), R.lmul));
    HostVrf vf;
    std::vector<uint64_t> src(static_cast<size_t>(vl), 0);
    for (int i = 0; i < vl; ++i) {
      src[static_cast<size_t>(i)] = Pat(53 * i + 7) & 0xFFull;
      vec->Prime(vf, R.data, i, R.sew_l, R.lmul, src[static_cast<size_t>(i)]);
    }
    std::vector<uint8_t> before(mem->mem.begin(), mem->mem.begin() + 0xE000);
    const std::string name = "restart store fault at " + Dec(k);

    RstResult r1 = RstRunMacro(cfg, vec, lsu, mem, R, 0, k, RC_PAGE, -1);
    rep->Check(r1.o.rst_trap && r1.o.rst_vstart == k,
               name + ": run1 vstart=" + Dec(r1.o.rst_vstart) + " expected " + Dec(k));
    rep->Check(r1.o.rst_restart_ready && r1.o.rst_restart_vstart == k,
               name + ": restart point " + Dec(r1.o.rst_restart_vstart) + " expected " + Dec(k));
    bool ok1 = true;
    for (int e = 0; e < vl; ++e) {
      const uint64_t addr = R.base + static_cast<uint64_t>(e);
      const int c = mem->byte_writes[static_cast<size_t>(addr & 0xFFFFull)];
      const uint8_t got = mem->mem[static_cast<size_t>(addr & 0xFFFFull)];
      if (e < k) {
        if (c != 1 || got != static_cast<uint8_t>(src[static_cast<size_t>(e)])) ok1 = false;
      } else if (c != 0 || got != before[static_cast<size_t>(addr)]) {
        ok1 = false;
      }
    }
    rep->Check(ok1, name + ": the partial store state after the fault is wrong");

    // restart: consume the fault record, keep the bitmap, re-execute from the
    // controller's restart point (the first element not performed)
    RstFaultClear(lsu);
    const int reqs_before = static_cast<int>(lsu->reqs().size());
    RstResult r2 = RstRunMacro(cfg, vec, lsu, mem, R, k, -1, RC_NONE, -1);
    rep->Check(!r2.o.rst_trap && r2.o.rst_complete && r2.o.rst_retire_ok,
               name + ": the restart did not complete cleanly");
    rep->Check(r2.o.desc_prefix == vl && r2.o.rst_prefix_agree,
               name + ": after the restart prefix=" + Dec(r2.o.desc_prefix) + " expected " + Dec(vl));
    bool ok2 = true;
    for (int e = 0; e < vl; ++e) {
      const uint64_t addr = R.base + static_cast<uint64_t>(e);
      if (mem->byte_writes[static_cast<size_t>(addr & 0xFFFFull)] != 1) ok2 = false;
      if (mem->mem[static_cast<size_t>(addr & 0xFFFFull)] !=
          static_cast<uint8_t>(src[static_cast<size_t>(e)])) {
        ok2 = false;
      }
    }
    rep->Check(ok2, name + ": a store side effect was duplicated or lost across the restart");

    // the restart issued exactly the remaining elements: the faulting element is
    // re-issued (its store was withheld), the committed ones are not
    const std::vector<LsuRec>& reqs = lsu->reqs();
    bool seq = (static_cast<int>(reqs.size()) == vl + 1);
    for (int i = 0; i <= k && seq; ++i) {
      if (reqs[static_cast<size_t>(i)].elem != i) seq = false;
    }
    for (int j = 0; j < vl - k && seq; ++j) {
      if (reqs[static_cast<size_t>(k + 1 + j)].elem != k + j) seq = false;
    }
    rep->Check(seq, name + ": the two runs issued " + Dec(reqs.size()) + " requests; expected " +
                     Dec(vl + 1) + " (elements 0.." + Dec(k) + " then " + Dec(k) + ".." +
                     Dec(vl - 1) + ")");
    rep->Check(static_cast<int>(lsu->reqs().size()) - reqs_before == vl - k,
               name + ": the restart issued " +
                   Dec(static_cast<int>(lsu->reqs().size()) - reqs_before) + " requests, expected " +
                   Dec(vl - k));
    cov->restart_cells += 1;
    RstRelease(lsu);
  }
}

void RunVecRestartCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep,
                       RstCoverage* cov) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);
  LsuMem mem;
  Lsu lsu(dut, clk);
  lsu.BindMem(&mem);

  // the retire gate runs first, so the "last packet arrived" defect is named
  PhaseRstRetireGate(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseRstFaultBoundaries(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseRstInterrupt(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseRstFof(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseRstRestart(&cfg, &vec, &lsu, &mem, rep, cov);

  // coverage is asserted, not implied: every claimed form saw a fault at every
  // element boundary
  for (int f = 0; f < RF_FORM_COUNT; ++f) {
    int seen = 0;
    for (int p = 0; p < 6; ++p) if (cov->form_pos[f][p]) seen += 1;
    rep->Check(seen == 6, std::string("coverage: form ") + kRstForms[f].name + " saw " +
                              Dec(seen) + " element boundaries, expected 6");
  }
  rep->Check(cov->cells >= 48, "coverage: only " + Dec(cov->cells) + " boundary cells ran");
  rep->Check(cov->intr_cells == 6, "coverage: " + Dec(cov->intr_cells) +
                                       " interrupt boundaries ran, expected 6");
  rep->Check(cov->fof_cells >= 12, "coverage: only " + Dec(cov->fof_cells) + " FOF cells ran");
  rep->Check(cov->restart_cells == 3, "coverage: " + Dec(cov->restart_cells) +
                                          " restart cells ran, expected 3");
}

// ============================================================================
// I-057 -- the stop path: the disposition of an in-flight load
// (CASE=rvv.stop_path_inflight).
//
// A vector memory macro can stop in more than one way, and the architecture
// decides what happens to a response that is still in flight by one sentence:
// an element that has already **committed** must not be discarded, and an
// element that will be **re-executed** must not be written back. This case
// drives each stop kind with a load in flight at the moment the stop is taken
// and checks the architectural outcome -- the destination element, the memory
// side effect, and the re-execution -- against an expectation computed here
// from that sentence, never read from the DUT.
//
// The kinds, and what each one means for an in-flight response:
//
//   element fault at k    partial: [0,k) committed, [k,vl) re-executed.
//                         An in-flight response for an element >= k is stale
//                         and must be discarded; the prefix is written back.
//   whole-macro trap      nothing committed (here: a fault at element 0, and
//                         the coalesced-group fallback), so every element is
//                         re-executed and every in-flight response is discarded.
//   redirect / cancel     the boundary stop a precise interrupt takes at
//                         element b: [0,b) committed, nothing at or after b
//                         issued, the resume re-executes [b,vl). Every response
//                         in flight is for an element < b and must be WRITTEN
//                         BACK. This is where the coalescing lane's probe found
//                         the defect (`wf_push_c` gated by `abort_q`); the case
//                         asserts the prefix and is the check that replaced the
//                         probe.
//   lane broker drain     not a stop: I-059's broker waits for the macro to
//                         drain and never asserts `stop_i`. The macro completes
//                         and every response is written back. The case paces
//                         the memory to keep the pipeline full and requires the
//                         complete, undiscarded result.
//
// A store is issued strictly (limit 1), so no store response can be in flight
// at a boundary stop; the store rows carry the resume/no-duplicate-side-effect
// half of the rule (a resume that re-issued an already-completed element would
// write a byte twice).
// ============================================================================

enum : int { SK_FAULT = 0, SK_WHOLE = 1, SK_REDIRECT = 2, SK_DRAIN = 3, SK_KIND_COUNT = 4 };

const char* StopKindName(int k) {
  switch (k) {
    case SK_FAULT: return "element-fault";
    case SK_WHOLE: return "whole-macro-trap";
    case SK_REDIRECT: return "redirect-cancel";
    case SK_DRAIN: return "lane-broker-drain";
    default: return "?";
  }
}

struct StopCoverage {
  int cells[SK_KIND_COUNT] = {};
  int resume_cells = 0;
  int inflight_cells = 0;
};

struct StopRun {
  LsuObs fin;                 // the cycle `done` pulsed
  LsuObs post;                // after the controller walked the committed elements
  bool captured = false;      // a stop or a fault was observed
  int inflight_at_stop = 0;   // responses in flight when it was taken
  int head_at_stop = -1;      // element of the oldest one
  int stop_elem = -1;
};

// Run one macro, requesting a boundary stop once `stop_after_elems` elements
// have been offered (or injecting a fault at `fault_elem`), and stop the
// observation at the cycle the macro finishes. `mem_ready_after_stop` keeps the
// memory ready so the request pipeline stays full and a response is genuinely
// in flight when the stop is taken.
StopRun StopRunMacro(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, const RstCfg& R,
                     int vstart, int fault_elem, int fault_code, int stop_after_elems,
                     bool mem_ready_after_stop) {
  (void)vec;
  LsuConfig(cfg, R.sew_l, VlmulOfExp(R.lmul), static_cast<uint64_t>(R.vl), vstart,
            R.vta, R.vma);
  mem->ClearFaults();
  if (fault_elem >= 0) {
    mem->fault_enable = true;
    mem->fault_all = false;
    mem->fault_elem = fault_elem;
    mem->fault_field = 0;
  }
  RstAlloc(lsu, R.vl, vstart, Vtypei(SewField(R.sew_l), R.lmul));

  LsuStim s;
  RstFillStim(&s, R, true);
  s.fault_code = fault_code;
  LsuObs o = lsu->Step(s);

  StopRun r;
  bool stopping = false;
  int guard = 0;
  while (!o.done && ++guard < 40000) {
    if (!r.captured && (o.stopped || o.trap)) {
      r.captured = true;
      r.inflight_at_stop = o.flight;
      r.head_at_stop = o.flight_head;
    }
    if (stop_after_elems >= 0 && o.elems >= stop_after_elems) stopping = true;
    LsuStim t;
    RstFillStim(&t, R, false);
    t.fault_code = fault_code;
    if (stopping) {
      t.intr = true;
      t.mem_ready = mem_ready_after_stop;
    }
    o = lsu->Step(t);
  }
  if (!r.captured && (o.stopped || o.trap)) {
    r.captured = true;
    r.inflight_at_stop = o.flight;
    r.head_at_stop = o.flight_head;
  }
  r.fin = o;
  r.stop_elem = static_cast<int>(o.stop_elem);
  // let the controller finish its walk over the committed elements
  while (o.rst_busy && ++guard < 40000) {
    LsuStim t;
    RstFillStim(&t, R, false);
    o = lsu->Step(t);
  }
  r.post = o;
  mem->ClearFaults();
  return r;
}

// The prefix of the request stream a resume from `start` must issue: exactly the
// elements [start, vl), in order, and nothing else.
bool StopResumeIssued(const std::vector<LsuRec>& reqs, size_t from, int start, int vl) {
  if (static_cast<int>(reqs.size() - from) != vl - start) return false;
  for (int j = 0; j < vl - start; ++j) {
    if (reqs[from + static_cast<size_t>(j)].elem != start + j) return false;
  }
  return true;
}

// --- kind 1: an element fault at k, with a later element in flight ----------
void PhaseStopFault(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                    StopCoverage* cov) {
  const int vl = 6, sew_l = 3, lmul = 0, vd = 8;
  const int ks[2] = {2, 3};
  // Hold the faulting response so the next element has been issued by the time
  // the fault is reported: the later element is then genuinely in flight, and
  // the discard is a real decision rather than a race the case would win by
  // accident.
  mem->fault_hold = 4;
  for (int ki = 0; ki < 2; ++ki) {
    const int k = ks[ki];
    mem->OneRam();
    mem->ResetCounters();
    RstCfg R;
    R.mode = LS_UNIT; R.we = false; R.vl = vl; R.sew_l = sew_l; R.lmul = lmul;
    R.vd = vd; R.base = 0x8000 + 0x40 * ki;
    lsu->ClearReqs();
    HostVrf vf;
    std::vector<uint64_t> old(static_cast<size_t>(vl), 0);
    for (int e = 0; e < vl; ++e) {
      old[static_cast<size_t>(e)] = Pat(211 * e + 7 * ki + 3) & 0xFFull;
      vec->Prime(vf, vd, e, sew_l, lmul, old[static_cast<size_t>(e)]);
    }
    StopRun r = StopRunMacro(cfg, vec, lsu, mem, R, 0, k, RC_PAGE, -1, true);
    const std::string name = "element fault at " + Dec(k);

    rep->Check(r.fin.trap && r.fin.trap_elem == k,
               name + ": packetizer trap=" + Dec(r.fin.trap) + " at " + Dec(r.fin.trap_elem) +
                   " expected 1/" + Dec(k));
    rep->Check(r.post.rst_trap && r.post.rst_vstart == k,
               name + ": controller trap=" + Dec(r.post.rst_trap) + " vstart=" +
                   Dec(r.post.rst_vstart) + " expected 1/" + Dec(k));
    // an element strictly after the fault was in flight when it was taken, so
    // the discard is a real decision and not vacuous
    rep->Check(r.inflight_at_stop >= 1 && r.head_at_stop > k,
               name + ": no element after the fault was in flight (flight=" +
                   Dec(r.inflight_at_stop) + " head=" + Dec(r.head_at_stop) +
                   "), the discard check would be vacuous");
    if (r.inflight_at_stop >= 1 && r.head_at_stop > k) cov->inflight_cells += 1;

    bool ok = true;
    std::string detail;
    for (int e = 0; e < vl; ++e) {
      const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
      const uint64_t want = (e < k) ? mem->Elem(R.base + static_cast<uint64_t>(e), 1)
                                    : old[static_cast<size_t>(e)];
      if (got != want) { ok = false; detail += " dest" + Dec(e); }
    }
    rep->Check(ok, name + ": the partial destination is wrong (the committed prefix must be "
                      "loaded, the faulting and later elements untouched):" + detail);

    // the restart re-executes [k,vl) and nothing else
    RstFaultClear(lsu);
    const size_t before = lsu->reqs().size();
    StopRun r2 = StopRunMacro(cfg, vec, lsu, mem, R, k, -1, RC_NONE, -1, true);
    rep->Check(!r2.fin.trap && r2.fin.done, name + ": the restart did not complete cleanly");
    bool ok2 = true;
    for (int e = 0; e < vl; ++e) {
      const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
      const uint64_t want = mem->Elem(R.base + static_cast<uint64_t>(e), 1);
      if (got != want) ok2 = false;
    }
    rep->Check(ok2, name + ": after the restart an element is not the value the re-execution "
                        "must produce");
    rep->Check(r2.post.desc_prefix == vl, name + ": after the restart prefix=" +
                                               Dec(r2.post.desc_prefix) + " expected " + Dec(vl));
    rep->Check(StopResumeIssued(lsu->reqs(), before, k, vl),
               name + ": the restart did not issue exactly elements " + Dec(k) + ".." +
                   Dec(vl - 1) + " (no committed element may be re-issued)");
    cov->cells[SK_FAULT] += 1;
    cov->resume_cells += 1;
    RstRelease(lsu);
  }
  mem->fault_hold = 0;
}

// --- kind 2: a whole-macro trap (nothing committed) -------------------------
void PhaseStopWhole(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                    StopCoverage* cov) {
  const int vl = 6, sew_l = 3, lmul = 0, vd = 8;
  mem->OneRam();
  mem->ResetCounters();
  RstCfg R;
  R.mode = LS_UNIT; R.we = false; R.vl = vl; R.sew_l = sew_l; R.lmul = lmul;
  R.vd = vd; R.base = 0x8800;
  lsu->ClearReqs();
  mem->fault_hold = 4;
  HostVrf vf;
  std::vector<uint64_t> old(static_cast<size_t>(vl), 0);
  for (int e = 0; e < vl; ++e) {
    old[static_cast<size_t>(e)] = Pat(191 * e + 17) & 0xFFull;
    vec->Prime(vf, vd, e, sew_l, lmul, old[static_cast<size_t>(e)]);
  }
  StopRun r = StopRunMacro(cfg, vec, lsu, mem, R, 0, 0, RC_ACCESS, -1, true);
  const std::string name = "whole-macro trap (fault at 0)";

  rep->Check(r.fin.trap && r.fin.trap_elem == 0 && r.post.rst_vstart == 0,
             name + ": trap=" + Dec(r.fin.trap) + " at " + Dec(r.fin.trap_elem) +
                 " vstart=" + Dec(r.post.rst_vstart) + " expected 1/0/0");
  // nothing committed: every element is re-executed, so an in-flight element
  // after 0 must be discarded, not written back
  rep->Check(r.inflight_at_stop >= 1 && r.head_at_stop > 0,
             name + ": no element after 0 was in flight (flight=" +
                 Dec(r.inflight_at_stop) + " head=" + Dec(r.head_at_stop) + ")");
  bool ok = true;
  std::string detail;
  for (int e = 0; e < vl; ++e) {
    const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
    if (got != old[static_cast<size_t>(e)]) { ok = false; detail += " dest" + Dec(e); }
  }
  rep->Check(ok, name + ": an element took effect even though the whole macro is "
                    "re-executed:" + detail);

  RstFaultClear(lsu);
  const size_t before = lsu->reqs().size();
  StopRun r2 = StopRunMacro(cfg, vec, lsu, mem, R, 0, -1, RC_NONE, -1, true);
  rep->Check(!r2.fin.trap && r2.fin.done, name + ": the re-execution did not complete cleanly");
  bool ok2 = true;
  for (int e = 0; e < vl; ++e) {
    const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
    const uint64_t want = mem->Elem(R.base + static_cast<uint64_t>(e), 1);
    if (got != want) ok2 = false;
  }
  rep->Check(ok2, name + ": the re-execution from 0 did not load every element");
  rep->Check(StopResumeIssued(lsu->reqs(), before, 0, vl),
             name + ": the re-execution did not issue exactly elements 0.." + Dec(vl - 1));
  cov->cells[SK_WHOLE] += 1;
  cov->resume_cells += 1;
  RstRelease(lsu);
  mem->fault_hold = 0;
}

// --- kind 3: the boundary stop / redirect (the reported defect) -------------
void PhaseStopRedirect(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                       StopCoverage* cov) {
  const int vl = 8, sew_l = 3, lmul = 0, vd = 8, data = 16;
  for (int we = 0; we < 2; ++we) {
    mem->OneRam();
    mem->ResetCounters();
    RstCfg R;
    R.mode = LS_UNIT; R.we = (we != 0); R.vl = vl; R.sew_l = sew_l; R.lmul = lmul;
    R.vd = vd; R.data = data; R.base = 0x9000 + 0x40 * we;
    lsu->ClearReqs();
    HostVrf vf;
    const int grp = we ? data : vd;
    std::vector<uint64_t> val(static_cast<size_t>(vl), 0);
    std::vector<uint8_t> pristine;
    for (int e = 0; e < vl; ++e) {
      val[static_cast<size_t>(e)] = Pat(97 * e + 31 * we + 9) & 0xFFull;
      vec->Prime(vf, grp, e, sew_l, lmul, val[static_cast<size_t>(e)]);
    }
    pristine = mem->mem;

    // the memory stays ready: the pipeline is full and a response is in flight
    // when the stop is taken
    StopRun r = StopRunMacro(cfg, vec, lsu, mem, R, 0, -1, RC_NONE, 4, true);
    const std::string name = std::string("redirect/cancel boundary stop ") +
                             (we ? "store" : "load");
    const int b = r.stop_elem;

    rep->Check(r.fin.done, name + ": the macro never completed");
    rep->Check(!r.fin.trap, name + ": a boundary stop reported a fault");
    rep->Check(r.fin.stopped && b >= 1 && b <= vl,
               name + ": stopped=" + Dec(r.fin.stopped) + " stop_elem=" + Dec(b) +
                   " is out of range");
    if (b < 1 || b > vl) { RstRelease(lsu); continue; }

    // the reported boundary is the first element not performed: exactly the
    // elements [0,b) were offered in this run, so a resume from b cannot
    // re-issue an element that already completed
    const int offered = static_cast<int>(lsu->reqs().size());
    rep->Check(offered == b, name + ": the stop reported boundary " + Dec(b) + " but " +
                   Dec(offered) + " elements had been offered -- the resume would re-issue "
                   "an element that already completed and duplicate its effect");

    // the prefix took effect, the boundary and later did not
    bool ok = true;
    std::string detail;
    for (int e = 0; e < vl; ++e) {
      if (we) {
        const uint64_t addr = R.base + static_cast<uint64_t>(e);
        const int writes = mem->byte_writes[static_cast<size_t>(addr & 0xFFFFull)];
        const uint8_t got = mem->mem[static_cast<size_t>(addr & 0xFFFFull)];
        if (e < b) {
          if (writes != 1 || got != static_cast<uint8_t>(val[static_cast<size_t>(e)])) {
            ok = false; detail += " mem" + Dec(e);
          }
        } else if (writes != 0 ||
                   got != pristine[static_cast<size_t>(addr & 0xFFFFull)]) {
          ok = false; detail += " mem" + Dec(e);
        }
      } else {
        const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
        const uint64_t want = (e < b) ? mem->Elem(R.base + static_cast<uint64_t>(e), 1)
                                      : val[static_cast<size_t>(e)];
        if (got != want) { ok = false; detail += " dest" + Dec(e); }
      }
    }
    rep->Check(ok, name + ": the partial state after the stop is wrong (an in-flight load "
                      "whose element committed must be written back, an element at or after "
                      "the boundary must be untouched):" + detail);

    // the load row carries the in-flight evidence: with the memory ready the
    // last pre-boundary element is still in flight when the stop is taken
    if (!we) {
      rep->Check(r.inflight_at_stop >= 1 && r.head_at_stop >= 0 && r.head_at_stop < b,
                 name + ": no element below the boundary was in flight (flight=" +
                     Dec(r.inflight_at_stop) + " head=" + Dec(r.head_at_stop) + " b=" +
                     Dec(b) + "), the write-back check would be vacuous");
      if (r.inflight_at_stop >= 1 && r.head_at_stop >= 0 && r.head_at_stop < b) {
        cov->inflight_cells += 1;
      }
    }

    // the resume from b re-executes [b,vl) and nothing before it
    RstFaultClear(lsu);
    const size_t before = lsu->reqs().size();
    StopRun r2 = StopRunMacro(cfg, vec, lsu, mem, R, b, -1, RC_NONE, -1, true);
    rep->Check(!r2.fin.trap && r2.fin.done, name + ": the resume did not complete cleanly");
    rep->Check(StopResumeIssued(lsu->reqs(), before, b, vl),
               name + ": the resume did not issue exactly elements " + Dec(b) + ".." +
                   Dec(vl - 1) + " (a re-issued completed element duplicates an effect)");
    bool ok2 = true;
    for (int e = 0; e < vl; ++e) {
      if (we) {
        const uint64_t addr = R.base + static_cast<uint64_t>(e);
        if (mem->byte_writes[static_cast<size_t>(addr & 0xFFFFull)] != 1) ok2 = false;
        if (mem->mem[static_cast<size_t>(addr & 0xFFFFull)] !=
            static_cast<uint8_t>(val[static_cast<size_t>(e)])) {
          ok2 = false;
        }
      } else {
        const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
        if (got != mem->Elem(R.base + static_cast<uint64_t>(e), 1)) ok2 = false;
      }
    }
    rep->Check(ok2, name + ": after the resume an element is wrong or a side effect was "
                        "duplicated or lost");
    rep->Check(r2.post.desc_prefix == vl, name + ": after the resume prefix=" +
                                               Dec(r2.post.desc_prefix) + " expected " + Dec(vl));
    cov->cells[SK_REDIRECT] += 1;
    cov->resume_cells += 1;
    RstRelease(lsu);
  }
}

// --- kind 4: the lane broker's drain (no stop, nothing discarded) -----------
void PhaseStopDrain(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                    StopCoverage* cov) {
  const int vl = 8, sew_l = 3, lmul = 0, vd = 8, data = 16;
  for (int we = 0; we < 2; ++we) {
    mem->OneRam();
    mem->ResetCounters();
    RstCfg R;
    R.mode = LS_UNIT; R.we = (we != 0); R.vl = vl; R.sew_l = sew_l; R.lmul = lmul;
    R.vd = vd; R.data = data; R.base = 0xA000 + 0x40 * we;
    lsu->ClearReqs();
    HostVrf vf;
    const int grp = we ? data : vd;
    std::vector<uint64_t> val(static_cast<size_t>(vl), 0);
    for (int e = 0; e < vl; ++e) {
      val[static_cast<size_t>(e)] = Pat(113 * e + 41 * we + 7) & 0xFFull;
      vec->Prime(vf, grp, e, sew_l, lmul, val[static_cast<size_t>(e)]);
    }
    LsuConfig(cfg, R.sew_l, VlmulOfExp(R.lmul), static_cast<uint64_t>(R.vl), 0, R.vta, R.vma);
    mem->ClearFaults();
    // a longer latency plus a paced memory keeps two requests in flight, so the
    // drain really is waiting on a busy macro rather than an empty pipeline
    mem->latency = 4;
    RstAlloc(lsu, R.vl, 0, Vtypei(SewField(R.sew_l), R.lmul));

    // pace the memory so the pipeline is genuinely occupied; the broker's drain
    // never asserts `stop_i`, it waits for exactly this macro to finish
    LsuStim s;
    RstFillStim(&s, R, true);
    LsuObs o = lsu->Step(s);
    int maxf = o.flight;
    bool ready = true;
    int guard = 0;
    while (!o.done && ++guard < 40000) {
      LsuStim t;
      RstFillStim(&t, R, false);
      t.mem_ready = ready;
      ready = !ready;
      o = lsu->Step(t);
      if (o.flight > maxf) maxf = o.flight;
    }
    const std::string name = std::string("lane-broker drain ") + (we ? "store" : "load");
    rep->Check(o.done && !o.stopped && !o.trap,
               name + ": done=" + Dec(o.done) + " stopped=" + Dec(o.stopped) + " trap=" +
                   Dec(o.trap) + " (the drain is not a stop)");
    // a load's unordered pipeline is two deep; a store is issued strictly (one
    // in flight) by design, so its occupancy is one
    const int minf = we ? 1 : 2;
    rep->Check(maxf >= minf, name + ": the pipeline never held " + Dec(minf) +
                                 " request(s) (max flight " + Dec(maxf) +
                                 "), the drain would not be waiting on a busy macro");
    bool ok = true;
    for (int e = 0; e < vl; ++e) {
      if (we) {
        const uint64_t addr = R.base + static_cast<uint64_t>(e);
        if (mem->byte_writes[static_cast<size_t>(addr & 0xFFFFull)] != 1) ok = false;
        if (mem->mem[static_cast<size_t>(addr & 0xFFFFull)] !=
            static_cast<uint8_t>(val[static_cast<size_t>(e)])) {
          ok = false;
        }
      } else {
        const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
        if (got != mem->Elem(R.base + static_cast<uint64_t>(e), 1)) ok = false;
      }
    }
    rep->Check(ok, name + ": an element was discarded or a side effect lost while the macro "
                        "drained");
    while (o.rst_busy && ++guard < 40000) {
      LsuStim t;
      RstFillStim(&t, R, false);
      o = lsu->Step(t);
    }
    cov->cells[SK_DRAIN] += 1;
    RstRelease(lsu);
  }
  mem->latency = 2;
}

void RunStopPathCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep,
                     StopCoverage* cov) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);
  LsuMem mem;
  Lsu lsu(dut, clk);
  lsu.BindMem(&mem);

  PhaseStopFault(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseStopWhole(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseStopRedirect(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseStopDrain(&cfg, &vec, &lsu, &mem, rep, cov);

  // coverage is asserted, not implied: every stop kind ran, and the in-flight
  // write-back and the in-flight discard were each observed on a real flight
  for (int k = 0; k < SK_KIND_COUNT; ++k) {
    rep->Check(cov->cells[k] >= 1, std::string("coverage: stop kind ") + StopKindName(k) +
                                       " never ran");
  }
  rep->Check(cov->cells[SK_FAULT] == 2, "coverage: " + Dec(cov->cells[SK_FAULT]) +
                                            " element-fault cells ran, expected 2");
  rep->Check(cov->cells[SK_REDIRECT] == 2, "coverage: " + Dec(cov->cells[SK_REDIRECT]) +
                                               " boundary-stop cells ran, expected 2");
  rep->Check(cov->inflight_cells >= 2, "coverage: only " + Dec(cov->inflight_cells) +
                                           " cells observed a genuine in-flight response");
  rep->Check(cov->resume_cells >= 4, "coverage: only " + Dec(cov->resume_cells) +
                                         " cells exercised a resume");
}

// ============================================================================
// I-058 -- vector chaining, element readiness and the two element-granular
// hazards.  CASE=rvv.chaining_hazards.
//
// The oracle here is the chaining rule, not the DUT.  Element i's payload is an
// element-wise 64-bit integer add of two host arrays (the I-054 datapath's
// rule, computed here from the host sources); the case checks that the network
// delivers *that element's* value for *that element*, in order, and only after
// its producer wrote it.  The network's readiness bitmap, its counters and the
// descriptor's bitmap are only ever *compared* against the host's numbers.
//
// The four `-DMOSAIC_VEC_CHAIN_MUTANT_*` builds each break exactly one of the
// checks below; the shipping build defines none of them.
// ============================================================================

// ---- the producer's operation, computed on the host ------------------------
uint64_t ChainSrcA(int gen, int i) {
  return 0x2000000000000000ull +
         0x0101010101010101ull * static_cast<uint64_t>(gen * 16 + i);
}
uint64_t ChainSrcB(int i) {
  return 0x0000FFFF0000FFFFull ^
         (0x1111111111111111ull * static_cast<uint64_t>(i + 1));
}
// element i of the producing macro, by the operation's rule: an element-wise
// add of the two host sources (wraparound is the same rule either way)
uint64_t ChainElemVal(int gen, int i) { return ChainSrcA(gen, i) + ChainSrcB(i); }

enum : int { CH_ORDER_ASC = 0, CH_ORDER_DESC = 1, CH_ORDER_INTER = 2, CH_ORDER_COUNT = 3 };

const char* ChainOrderName(int kind) {
  switch (kind) {
    case CH_ORDER_ASC: return "ascending";
    case CH_ORDER_DESC: return "descending";
    default: return "interleaved";
  }
}

// The order the producer's elements complete in.  Ascending writes element 0
// first; descending writes it last; interleaved writes the even elements and
// then the odd ones, so element 1 completes after element 6.
std::vector<int> ChainOrder(int kind, int n) {
  std::vector<int> o;
  if (kind == CH_ORDER_DESC) {
    for (int i = n - 1; i >= 0; --i) o.push_back(i);
  } else if (kind == CH_ORDER_INTER) {
    for (int i = 0; i < n; i += 2) o.push_back(i);
    for (int i = 1; i < n; i += 2) o.push_back(i);
  } else {
    for (int i = 0; i < n; ++i) o.push_back(i);
  }
  return o;
}

// --------------------------------------------------------------- the harness
struct ChainStim {
  bool rst = false;
  bool en = true;
  bool bind = true;
  // the architectural macro the descriptor tracks
  bool desc_alloc = false;
  uint32_t desc_gen = 0;
  int desc_vl = 0;
  int desc_vd = 0;
  bool desc_release = false;
  // producer
  bool p_alloc = false;
  uint32_t p_gen = 0;
  int p_vd = 0;
  int p_vl = 0;
  bool p_wr = false;
  int p_wr_index = 0;
  uint64_t p_wr_data = 0;
  uint32_t p_wr_gen = 0;
  bool p_fault = false;
  int p_fault_elem = 0;
  bool p_cancel = false;
  bool p_done = false;
  // consumers
  bool c0_alloc = false; uint32_t c0_gen = 0; int c0_vs = 0; int c0_vl = 0;
  bool c0_req = false; int c0_index = 0; bool c0_finish = false;
  bool c1_alloc = false; uint32_t c1_gen = 0; int c1_vs = 0; int c1_vl = 0;
  bool c1_req = false; int c1_index = 0; bool c1_finish = false;
  // WAR overwrite arbiter
  bool war_valid = false; int war_vd = 0; int war_elem = 0; uint64_t war_data = 0;
};

struct ChainObs {
  // combinational handshakes, sampled before the clock edge
  bool p_alloc_ready = false;
  bool p_wr_accept = false;
  bool accept_valid = false;
  int accept_index = 0;
  bool c0_alloc_ready = false, c1_alloc_ready = false;
  bool c0_req_accept = false, c1_req_accept = false;
  bool c0_rdy = false, c1_rdy = false;
  uint64_t c0_data = 0, c1_data = 0;
  bool war_grant = false, src_release_ok = false;
  // registered state, sampled after the clock edge
  bool valid = false;
  uint32_t gen = 0;
  int vd = 0;
  bool done = false;
  bool fault = false;
  int fault_elem = 0;
  uint64_t ready_lo = 0, ready_hi = 0;
  uint32_t pkt_accept = 0, pkt_refuse = 0, fwd = 0, stall = 0;
  bool desc_valid = false;
  uint64_t desc_bm_lo = 0, desc_bm_hi = 0;
  uint32_t desc_gen = 0;
};

class Chain {
 public:
  Chain(Vmosaic_vec_tb* d, ClockDriver* clk) : d_(d), clk_(clk) {}

  void Reset() {
    ChainStim s;
    s.rst = true;
    for (int i = 0; i < 3; ++i) Step(s);
  }

  ChainObs Step(const ChainStim& s) {
    d_->clk = 0;
    d_->rst = s.rst ? 1 : 0;

    // everything else in the wrapper is quiescent: no other vector unit runs,
    // and the driver owns the descriptor unless the chain is bound.
    d_->alloc_valid = s.desc_alloc ? 1 : 0;
    d_->alloc_vtype = Vtype(3, 0, false);
    d_->alloc_vl = static_cast<uint8_t>(s.desc_vl & 0xFF);
    d_->alloc_vstart = 0;
    d_->alloc_vd = static_cast<uint8_t>(s.desc_vd & 0x1F);
    d_->alloc_mask_ver = 0;
    d_->alloc_rob_index = 0;
    d_->alloc_rob_gen = s.desc_gen;
    d_->alloc_uop_index = 0;
    d_->elem_done_valid = 0;
    d_->elem_done_index = 0;
    d_->fault_valid = 0;
    d_->fault_elem = 0;
    d_->fault_code = 0;
    d_->desc_release = s.desc_release ? 1 : 0;
    d_->desc_fault_clear = 0;
    d_->rst_bind_i = 0;
    d_->rst_exec_valid_i = 0;
    d_->rst_intr_i = 0;
    d_->lsu_mem_rsp_fault_code_i = 0;
    d_->mem_owner_i = 0;
    d_->mem_rd_valid_i = 0;
    d_->mem_wr_valid_i = 0;
    d_->alu_exec_valid_i = 0;
    d_->alu_caps_i = 0;
    d_->el_valid_i = 0;
    d_->lsu_exec_valid_i = 0;
    d_->lsu_caps_i = 0;
    d_->lsu_mem_req_ready_i = 0;
    d_->lsu_mem_rsp_valid_i = 0;
    d_->fp_exec_valid_i = 0;
    d_->fp_caps_i = 0;
    d_->fp_commit_valid_i = 0;
    d_->fp_flush_i = 0;
    d_->cfg_vset_valid = 0;
    d_->cfg_snap_capture = 0;
    d_->cfg_replay_valid = 0;
    d_->cfg_exec_valid = 0;
    d_->cfg_csr_valid = 0;

    d_->chain_en_i = s.en ? 1 : 0;
    d_->chain_bind_i = s.bind ? 1 : 0;
    d_->chain_p_alloc_valid_i = s.p_alloc ? 1 : 0;
    d_->chain_p_gen_i = s.p_gen;
    d_->chain_p_vd_i = static_cast<uint8_t>(s.p_vd & 0x1F);
    d_->chain_p_vl_i = static_cast<uint8_t>(s.p_vl & 0xFF);
    d_->chain_p_wr_valid_i = s.p_wr ? 1 : 0;
    d_->chain_p_wr_index_i = static_cast<uint8_t>(s.p_wr_index & 0x7F);
    d_->chain_p_wr_data_i = s.p_wr_data;
    d_->chain_p_wr_gen_i = s.p_wr_gen;
    d_->chain_p_fault_valid_i = s.p_fault ? 1 : 0;
    d_->chain_p_fault_elem_i = static_cast<uint8_t>(s.p_fault_elem & 0x7F);
    d_->chain_p_cancel_i = s.p_cancel ? 1 : 0;
    d_->chain_p_done_i = s.p_done ? 1 : 0;
    d_->chain_c0_alloc_valid_i = s.c0_alloc ? 1 : 0;
    d_->chain_c0_gen_i = s.c0_gen;
    d_->chain_c0_vs_i = static_cast<uint8_t>(s.c0_vs & 0x1F);
    d_->chain_c0_vl_i = static_cast<uint8_t>(s.c0_vl & 0xFF);
    d_->chain_c0_req_valid_i = s.c0_req ? 1 : 0;
    d_->chain_c0_req_index_i = static_cast<uint8_t>(s.c0_index & 0x7F);
    d_->chain_c0_finish_i = s.c0_finish ? 1 : 0;
    d_->chain_c1_alloc_valid_i = s.c1_alloc ? 1 : 0;
    d_->chain_c1_gen_i = s.c1_gen;
    d_->chain_c1_vs_i = static_cast<uint8_t>(s.c1_vs & 0x1F);
    d_->chain_c1_vl_i = static_cast<uint8_t>(s.c1_vl & 0xFF);
    d_->chain_c1_req_valid_i = s.c1_req ? 1 : 0;
    d_->chain_c1_req_index_i = static_cast<uint8_t>(s.c1_index & 0x7F);
    d_->chain_c1_finish_i = s.c1_finish ? 1 : 0;
    d_->chain_war_valid_i = s.war_valid ? 1 : 0;
    d_->chain_war_vd_i = static_cast<uint8_t>(s.war_vd & 0x1F);
    d_->chain_war_elem_i = static_cast<uint8_t>(s.war_elem & 0x7F);
    d_->chain_war_data_i = s.war_data;

    d_->eval();

    ChainObs o;
    o.p_alloc_ready = d_->chain_p_alloc_ready_o != 0;
    o.p_wr_accept = d_->chain_p_wr_accept_o != 0;
    o.accept_valid = d_->chain_accept_valid_o != 0;
    o.accept_index = static_cast<int>(d_->chain_accept_index_o);
    o.c0_alloc_ready = d_->chain_c0_alloc_ready_o != 0;
    o.c1_alloc_ready = d_->chain_c1_alloc_ready_o != 0;
    o.c0_req_accept = d_->chain_c0_req_accept_o != 0;
    o.c1_req_accept = d_->chain_c1_req_accept_o != 0;
    o.c0_rdy = d_->chain_c0_rdy_o != 0;
    o.c1_rdy = d_->chain_c1_rdy_o != 0;
    o.c0_data = d_->chain_c0_data_o;
    o.c1_data = d_->chain_c1_data_o;
    o.war_grant = d_->chain_war_grant_o != 0;
    o.src_release_ok = d_->chain_src_release_ok_o != 0;

    d_->clk = 1;
    d_->eval();
    d_->clk = 0;
    d_->eval();

    o.valid = d_->chain_valid_o != 0;
    o.gen = static_cast<uint32_t>(d_->chain_gen_o);
    o.vd = static_cast<int>(d_->chain_vd_o);
    o.done = d_->chain_done_o != 0;
    o.fault = d_->chain_fault_o != 0;
    o.fault_elem = static_cast<int>(d_->chain_fault_elem_o);
    o.ready_lo = d_->chain_ready_lo_o;
    o.ready_hi = d_->chain_ready_hi_o;
    o.pkt_accept = static_cast<uint32_t>(d_->chain_pkt_accept_ctr_o);
    o.pkt_refuse = static_cast<uint32_t>(d_->chain_pkt_refuse_ctr_o);
    o.fwd = static_cast<uint32_t>(d_->chain_fwd_ctr_o);
    o.stall = static_cast<uint32_t>(d_->chain_stall_ctr_o);
    o.desc_valid = d_->o_valid != 0;
    o.desc_bm_lo = d_->o_elem_bitmap_lo;
    o.desc_bm_hi = d_->o_elem_bitmap_hi;
    o.desc_gen = static_cast<uint32_t>(d_->o_macro_rob_gen);

    clk_->Tick();
    return o;
  }

 private:
  Vmosaic_vec_tb* d_;
  ClockDriver* clk_;
};

// ------------------------------------------------------------------ coverage
struct ChainCov {
  bool order_seen[CH_ORDER_COUNT] = {};
  int order_cells = 0;
  int onoff_runs = 0;
  int war_cells = 0;
  int cancel_cells = 0;
  int fault_cells = 0;
  int cycles_on = 0, cycles_off = 0;
  uint32_t fwd_on = 0, fwd_off = 0;
  uint32_t stall_on = 0, stall_off = 0;
  uint32_t pkt_on = 0, pkt_off = 0;
  int pre_on = 0, pre_off = 0;
};

// --------------------------------------------------------- the chained run
struct ChainProgram {
  bool en = true;
  int order_kind = CH_ORDER_ASC;
  int vl = 8;
  int vd = 4;
  int gen = 1;
};

struct ChainResult {
  std::vector<uint64_t> read_val;
  std::vector<char> read_ok;
  bool early_read = false;
  bool write_refused = false;
  int cycles = 0;
  int reads_before_done = 0;
  int reads_after_done = 0;
  uint32_t pkt_accept = 0, pkt_refuse = 0, fwd = 0, stall = 0;
  uint64_t desc_bm_lo = 0, desc_bm_hi = 0;
  uint32_t desc_gen = 0;
};

// One full chained program: allocate the architectural macro and its producer
// under the same generation, allocate the consumer, let the producer write its
// elements in the program's order while the consumer reads them in element
// order, then finish.  The loop is the "memory" of the case; everything it
// observes is recorded for the phases to check against the host's numbers.
ChainResult RunChained(Chain* ch, const ChainProgram& P) {
  ChainResult R;
  R.read_val.assign(P.vl, 0);
  R.read_ok.assign(P.vl, 0);
  ch->Reset();

  {
    ChainStim s;
    s.en = P.en;
    s.desc_alloc = true; s.desc_gen = static_cast<uint32_t>(P.gen);
    s.desc_vl = P.vl; s.desc_vd = P.vd;
    s.p_alloc = true; s.p_gen = static_cast<uint32_t>(P.gen);
    s.p_vd = P.vd; s.p_vl = P.vl;
    (void)ch->Step(s);
  }
  {
    ChainStim s;
    s.en = P.en;
    s.c0_alloc = true; s.c0_gen = static_cast<uint32_t>(P.gen);
    s.c0_vs = P.vd; s.c0_vl = P.vl;
    (void)ch->Step(s);
  }

  const std::vector<int> order = ChainOrder(P.order_kind, P.vl);
  std::vector<char> written(P.vl, 0);
  int next_wr = 0, next_rd = 0;
  int guard = 4000;
  ChainObs o;
  while (next_rd < P.vl && guard-- > 0) {
    ChainStim s;
    s.en = P.en;
    if (next_wr < P.vl) {
      s.p_wr = true;
      s.p_wr_index = order[next_wr];
      s.p_wr_data = ChainElemVal(P.gen, order[next_wr]);
      s.p_wr_gen = static_cast<uint32_t>(P.gen);
    } else {
      s.p_done = true;
    }
    s.c0_req = true;
    s.c0_index = next_rd;
    o = ch->Step(s);
    R.cycles += 1;
    if (o.p_wr_accept && next_wr < P.vl) {
      written[order[next_wr]] = 1;
      next_wr += 1;
    } else if (next_wr < P.vl && s.p_wr) {
      R.write_refused = true;
    }
    if (o.c0_req_accept) {
      if (!written[next_rd]) R.early_read = true;
      R.read_val[next_rd] = o.c0_data;
      R.read_ok[next_rd] = 1;
      if (o.done) R.reads_after_done += 1; else R.reads_before_done += 1;
      next_rd += 1;
    }
  }

  {
    ChainStim s;
    s.en = P.en;
    s.p_done = true;
    s.c0_finish = true;
    o = ch->Step(s);
    R.cycles += 1;
  }
  R.pkt_accept = o.pkt_accept;
  R.pkt_refuse = o.pkt_refuse;
  R.fwd = o.fwd;
  R.stall = o.stall;
  R.desc_bm_lo = o.desc_bm_lo;
  R.desc_bm_hi = o.desc_bm_hi;
  R.desc_gen = o.desc_gen;
  return R;
}

std::string ChainBitmap(uint64_t lo, uint64_t hi) {
  return mosaic::Hex(hi, 16) + mosaic::Hex(lo, 16);
}

// ---------------------------------------------------------------- the phases
// The producer's elements complete in ascending, descending and interleaved
// order; the consumer must read every element's *own* value, and only after the
// producer wrote it.  The descending order finishes the last element first, so
// a macro-level ready bit is caught here.
void PhaseChainOrder(Chain* ch, Reporter* rep, ChainCov* cov) {
  const int kVl = 8;
  for (int kind = 0; kind < CH_ORDER_COUNT; ++kind) {
    ChainProgram P;
    P.en = true;
    P.order_kind = kind;
    P.vl = kVl;
    P.vd = 4;
    P.gen = 1 + kind;
    const std::string name = std::string("order ") + ChainOrderName(kind);

    ChainResult R = RunChained(ch, P);
    cov->order_seen[kind] = true;
    cov->order_cells += 1;

    rep->Check(!R.early_read,
               name + ": an element was read before its producer wrote it");
    rep->Check(!R.write_refused, name + ": a producer element packet was refused");
    for (int i = 0; i < kVl; ++i) {
      rep->Check(R.read_ok[i] != 0, name + ": element " + Dec(i) + " was never read");
      const uint64_t want = ChainElemVal(P.gen, i);
      rep->Check(R.read_val[i] == want,
                 name + ": element " + Dec(i) + " read " + mosaic::Hex(R.read_val[i]) +
                     " expected " + mosaic::Hex(want));
    }
    rep->Check(R.desc_bm_lo == ((1ull << kVl) - 1ull) && R.desc_bm_hi == 0ull,
               name + ": descriptor bitmap " + ChainBitmap(R.desc_bm_lo, R.desc_bm_hi) +
                   " expected the " + Dec(kVl) + " written elements");
    rep->Check(R.desc_gen == static_cast<uint32_t>(P.gen),
               name + ": the descriptor generation moved to " + Dec(R.desc_gen) +
                   ", expected " + Dec(P.gen));
  }
}

// The same program with chaining on and off.  The architectural results must be
// identical field for field; the counters and the cycle counts are the
// performance difference, reported rather than assumed.
void PhaseChainOnOff(Chain* ch, Reporter* rep, ChainCov* cov) {
  ChainProgram P;
  P.vl = 8;
  P.vd = 4;
  P.gen = 9;
  P.order_kind = CH_ORDER_ASC;

  P.en = true;
  ChainResult on = RunChained(ch, P);
  P.en = false;
  ChainResult off = RunChained(ch, P);
  cov->onoff_runs += 2;
  cov->cycles_on = on.cycles;
  cov->cycles_off = off.cycles;
  cov->fwd_on = on.fwd;
  cov->fwd_off = off.fwd;
  cov->stall_on = on.stall;
  cov->stall_off = off.stall;
  cov->pkt_on = on.pkt_accept;
  cov->pkt_off = off.pkt_accept;
  cov->pre_on = on.reads_before_done;
  cov->pre_off = off.reads_before_done;

  for (int i = 0; i < P.vl; ++i) {
    rep->Check(on.read_ok[i] != 0 && off.read_ok[i] != 0,
               "on/off: element " + Dec(i) + " unread in a configuration");
    rep->Check(on.read_val[i] == off.read_val[i],
               "on/off: element " + Dec(i) + " differs: chaining " +
                   mosaic::Hex(on.read_val[i]) + " no-chaining " +
                   mosaic::Hex(off.read_val[i]));
    rep->Check(on.read_val[i] == ChainElemVal(P.gen, i),
               "on/off: element " + Dec(i) + " read " +
                   mosaic::Hex(on.read_val[i]) + " expected " +
                   mosaic::Hex(ChainElemVal(P.gen, i)));
  }
  rep->Check(on.desc_bm_lo == off.desc_bm_lo && on.desc_bm_hi == off.desc_bm_hi,
             "on/off: the descriptor bitmap differs between chaining " +
                 ChainBitmap(on.desc_bm_lo, on.desc_bm_hi) + " and no-chaining " +
                 ChainBitmap(off.desc_bm_lo, off.desc_bm_hi));
  rep->Check(on.pkt_accept == static_cast<uint32_t>(P.vl) &&
                 off.pkt_accept == static_cast<uint32_t>(P.vl),
             "on/off: accepted packet count " + Dec(on.pkt_accept) + "/" +
                 Dec(off.pkt_accept) + " expected " + Dec(P.vl) + " each");

  // The configurations differ in the intended way: with chaining the reads are
  // element-granular and most of them land before the producer finished; the
  // control can read nothing until the whole macro is done.
  rep->Check(on.fwd > 0 && on.reads_before_done > 0,
             "on/off: chaining produced no element-granular forward");
  rep->Check(off.fwd == 0, "on/off: the no-chaining control produced " +
                              Dec(off.fwd) + " element-granular forwards");
  rep->Check(off.reads_before_done == 0,
             "on/off: the no-chaining control read " + Dec(off.reads_before_done) +
                 " elements before the producer finished");
  rep->Check(off.stall > on.stall,
             "on/off: the no-chaining control stalled " + Dec(off.stall) +
                 " cycles and chaining " + Dec(on.stall) +
                 "; the control was expected to stall longer");
  rep->Check(on.cycles < off.cycles,
             "on/off: chaining took " + Dec(on.cycles) + " cycles, no-chaining " +
                 Dec(off.cycles) + "; chaining was expected to be faster");
}

// The WAR hazard.  A source register must not be released -- or overwritten --
// while a younger macro still needs to read it, and another reader finishing
// must not release it.
void PhaseChainWar(Chain* ch, Reporter* rep, ChainCov* cov) {
  // ---- A: one consumer has read 0..k; element k is releasable, k+1 is not
  {
    const int kVl = 6, kGen = 20, kGrp = 6;
    ch->Reset();
    { ChainStim s; s.desc_alloc = true; s.desc_gen = kGen; s.desc_vl = kVl; s.desc_vd = kGrp;
      s.p_alloc = true; s.p_gen = kGen; s.p_vd = kGrp; s.p_vl = kVl; (void)ch->Step(s); }
    { ChainStim s; s.c0_alloc = true; s.c0_gen = kGen; s.c0_vs = kGrp; s.c0_vl = kVl; (void)ch->Step(s); }
    for (int i = 0; i < kVl; ++i) {
      ChainStim s;
      s.p_wr = true; s.p_wr_index = i; s.p_wr_data = ChainElemVal(kGen, i); s.p_wr_gen = kGen;
      ChainObs o = ch->Step(s);
      rep->Check(o.p_wr_accept, "war A: element " + Dec(i) + " packet was refused");
    }
    { ChainStim s; s.p_done = true; (void)ch->Step(s); }

    for (int i = 0; i < 3; ++i) {
      ChainStim s; s.c0_req = true; s.c0_index = i;
      ChainObs o = ch->Step(s);
      rep->Check(o.c0_req_accept, "war A: element " + Dec(i) + " was not ready to read");
      rep->Check(o.c0_data == ChainElemVal(kGen, i),
                 "war A: element " + Dec(i) + " read " + mosaic::Hex(o.c0_data) +
                     " expected " + mosaic::Hex(ChainElemVal(kGen, i)));
    }
    {   // element 4 has not been read: the overwrite must be held
      ChainStim s; s.war_valid = true; s.war_vd = kGrp; s.war_elem = 4; s.war_data = 0xDEADBEEFull;
      ChainObs o = ch->Step(s);
      rep->Check(!o.war_grant,
                 "war A: element 4 was released while the consumer had read only elements 0..2");
      rep->Check(!o.src_release_ok,
                 "war A: the source group was released with elements 3..5 still unread");
    }
    {   // element 1 has been read: it is releasable
      ChainStim s; s.war_valid = true; s.war_vd = kGrp; s.war_elem = 1;
      s.war_data = 0xA5A5A5A5A5A5A5A5ull;
      ChainObs o = ch->Step(s);
      rep->Check(o.war_grant,
                 "war A: element 1 was not released after the consumer had read it");
    }
    for (int i = 3; i < kVl; ++i) {
      ChainStim s; s.c0_req = true; s.c0_index = i;
      ChainObs o = ch->Step(s);
      rep->Check(o.c0_req_accept, "war A: element " + Dec(i) + " was not ready to read");
      rep->Check(o.c0_data == ChainElemVal(kGen, i),
                 "war A: element " + Dec(i) + " read " + mosaic::Hex(o.c0_data) +
                     " expected " + mosaic::Hex(ChainElemVal(kGen, i)));
    }
    {   // every element read: the group is releasable
      ChainStim s; s.war_valid = true; s.war_vd = kGrp; s.war_elem = 5; s.war_data = 1;
      ChainObs o = ch->Step(s);
      rep->Check(o.src_release_ok,
                 "war A: the source group was not released after every element had been read");
      rep->Check(o.war_grant,
                 "war A: element 5 was not releasable after it had been read");
    }
    {   // the granted overwrite landed in the element slot
      ChainStim a; a.c1_alloc = true; a.c1_gen = kGen; a.c1_vs = kGrp; a.c1_vl = kVl;
      (void)ch->Step(a);
      ChainStim s; s.c1_req = true; s.c1_index = 1;
      ChainObs o = ch->Step(s);
      rep->Check(o.c1_req_accept && o.c1_data == 0xA5A5A5A5A5A5A5A5ull,
                 "war A: the granted overwrite did not land in element 1: read " +
                     mosaic::Hex(o.c1_data));
    }
    {   // the second consumer read element 1 only, so it still holds element 4 of
        // the same group -- but a writer to another group is not blocked by it
      ChainStim s; s.war_valid = true; s.war_vd = kGrp; s.war_elem = 4; s.war_data = 0x22ull;
      ChainObs o = ch->Step(s);
      rep->Check(!o.war_grant,
                 "war A: element 4 was released while a later consumer still needed it");
      ChainStim t; t.war_valid = true; t.war_vd = kGrp + 1; t.war_elem = 4; t.war_data = 0x11ull;
      ChainObs p = ch->Step(t);
      rep->Check(p.war_grant,
                 "war A: a writer to another group was blocked by a hold on group " +
                     Dec(kGrp));
    }
    cov->war_cells += 1;
  }

  // ---- B: the other reader finishing must not release the group
  {
    const int kVl = 6, kGen = 21, kGrp = 7;
    ch->Reset();
    { ChainStim s; s.desc_alloc = true; s.desc_gen = kGen; s.desc_vl = kVl; s.desc_vd = kGrp;
      s.p_alloc = true; s.p_gen = kGen; s.p_vd = kGrp; s.p_vl = kVl; (void)ch->Step(s); }
    { ChainStim s; s.c0_alloc = true; s.c0_gen = kGen; s.c0_vs = kGrp; s.c0_vl = kVl;
      s.c1_alloc = true; s.c1_gen = kGen; s.c1_vs = kGrp; s.c1_vl = kVl;
      ChainObs o = ch->Step(s);
      rep->Check(o.c0_alloc_ready && o.c1_alloc_ready,
                 "war B: two consumers could not register");
    }
    for (int i = 0; i < kVl; ++i) {
      ChainStim s;
      s.p_wr = true; s.p_wr_index = i; s.p_wr_data = ChainElemVal(kGen, i); s.p_wr_gen = kGen;
      ChainObs o = ch->Step(s);
      rep->Check(o.p_wr_accept, "war B: element " + Dec(i) + " packet was refused");
    }
    { ChainStim s; s.p_done = true; (void)ch->Step(s); }

    // consumer 1 reads every element and finishes; consumer 0 reads only 0..2
    for (int i = 0; i < kVl; ++i) {
      ChainStim s; s.c1_req = true; s.c1_index = i;
      ChainObs o = ch->Step(s);
      rep->Check(o.c1_req_accept, "war B: consumer 1 could not read element " + Dec(i));
    }
    { ChainStim s; s.c1_finish = true; (void)ch->Step(s); }
    for (int i = 0; i < 3; ++i) {
      ChainStim s; s.c0_req = true; s.c0_index = i;
      ChainObs o = ch->Step(s);
      rep->Check(o.c0_req_accept, "war B: consumer 0 could not read element " + Dec(i));
    }
    {   // the other reader has finished; consumer 0 still needs element 4
      ChainStim s; s.war_valid = true; s.war_vd = kGrp; s.war_elem = 4; s.war_data = 0xFEEDFACEull;
      ChainObs o = ch->Step(s);
      rep->Check(!o.war_grant,
                 "war B: element 4 was released because the other reader finished, "
                 "while consumer 0 had not read it");
      rep->Check(!o.src_release_ok,
                 "war B: the source group was released because the other reader "
                 "finished, while consumer 0 still needed elements 3..5");
    }
    for (int i = 3; i < kVl; ++i) {
      ChainStim s; s.c0_req = true; s.c0_index = i;
      ChainObs o = ch->Step(s);
      rep->Check(o.c0_req_accept, "war B: consumer 0 could not read element " + Dec(i));
      rep->Check(o.c0_data == ChainElemVal(kGen, i),
                 "war B: element " + Dec(i) + " read " + mosaic::Hex(o.c0_data) +
                     " expected " + mosaic::Hex(ChainElemVal(kGen, i)));
    }
    { ChainStim s; s.war_valid = true; s.war_vd = kGrp; s.war_elem = 4; s.war_data = 0xFEEDFACEull;
      ChainObs o = ch->Step(s);
      rep->Check(o.src_release_ok && o.war_grant,
                 "war B: the group stayed held after consumer 0 finished its reads");
    }
    cov->war_cells += 1;
  }
}

// Instruction cancellation.  A packet produced by a macro that was subsequently
// cancelled must not be accepted by the descriptor that takes over the slot:
// each packet carries the generation that produced it, and a mismatch is
// refused.  A producer fault has the same shape -- the elements after it must
// not be accepted.
void PhaseChainCancel(Chain* ch, Reporter* rep, ChainCov* cov) {
  // ---- the cancelled generation's packet must not reach the new descriptor
  {
    const int kVl = 6, kGrp = 5;
    ch->Reset();
    { ChainStim s; s.desc_alloc = true; s.desc_gen = 30; s.desc_vl = kVl; s.desc_vd = kGrp;
      s.p_alloc = true; s.p_gen = 30; s.p_vd = kGrp; s.p_vl = kVl; (void)ch->Step(s); }
    for (int i = 0; i < 3; ++i) {
      ChainStim s;
      s.p_wr = true; s.p_wr_index = i; s.p_wr_data = ChainElemVal(30, i); s.p_wr_gen = 30;
      ChainObs o = ch->Step(s);
      rep->Check(o.p_wr_accept, "cancel: the live producer's element " + Dec(i) +
                                    " packet was refused");
    }
    {   // cancel the macro and release its descriptor
      ChainStim s; s.p_cancel = true; s.desc_release = true; (void)ch->Step(s);
    }
    {   // a new descriptor takes the slot, under a new generation
      ChainStim s; s.desc_alloc = true; s.desc_gen = 31; s.desc_vl = kVl; s.desc_vd = kGrp;
      s.p_alloc = true; s.p_gen = 31; s.p_vd = kGrp; s.p_vl = kVl;
      ChainObs o = ch->Step(s);
      rep->Check(Popcount128(o.desc_bm_lo, o.desc_bm_hi) == 0,
                 "cancel: the new descriptor did not start clean: bitmap " +
                     ChainBitmap(o.desc_bm_lo, o.desc_bm_hi));
      rep->Check(o.desc_gen == 31u,
                 "cancel: the new descriptor generation is " + Dec(o.desc_gen));
    }
    {   // the wrong-path packet: produced by generation 30, offered to generation 31
      ChainStim s;
      s.p_wr = true; s.p_wr_index = 3; s.p_wr_data = ChainElemVal(30, 3); s.p_wr_gen = 30;
      ChainObs o = ch->Step(s);
      rep->Check(!o.p_wr_accept,
                 "cancel: a packet from the cancelled generation 30 was accepted by "
                 "the new descriptor");
      rep->Check(Popcount128(o.desc_bm_lo, o.desc_bm_hi) == 0,
                 "cancel: the new descriptor progressed on a stale packet: bitmap " +
                     ChainBitmap(o.desc_bm_lo, o.desc_bm_hi));
      rep->Check((o.ready_lo & (1ull << 3)) == 0ull,
                 "cancel: the stale packet marked element 3 ready");
    }
    { ChainStim s; s.c0_alloc = true; s.c0_gen = 31; s.c0_vs = kGrp; s.c0_vl = kVl; (void)ch->Step(s); }

    const int order[6] = {0, 1, 2, 4, 5, 3};
    std::vector<char> written(kVl, 0);
    std::vector<uint64_t> got(kVl, 0);
    int next_wr = 0, next_rd = 0, guard = 200;
    bool early = false;
    ChainObs o;
    while (next_rd < kVl && guard-- > 0) {
      ChainStim s;
      if (next_wr < kVl) {
        s.p_wr = true; s.p_wr_index = order[next_wr];
        s.p_wr_data = ChainElemVal(31, order[next_wr]); s.p_wr_gen = 31;
      } else {
        s.p_done = true;
      }
      s.c0_req = true; s.c0_index = next_rd;
      o = ch->Step(s);
      if (o.p_wr_accept && next_wr < kVl) { written[order[next_wr]] = 1; next_wr += 1; }
      if (o.c0_req_accept) {
        if (!written[next_rd]) early = true;
        got[next_rd] = o.c0_data;
        next_rd += 1;
      }
    }
    rep->Check(!early,
               "cancel: the consumer read an element before the new producer wrote "
               "it -- a stale packet was forwarded");
    for (int i = 0; i < kVl; ++i) {
      rep->Check(got[i] == ChainElemVal(31, i),
                 "cancel: element " + Dec(i) + " read " + mosaic::Hex(got[i]) +
                     " expected generation 31's " + mosaic::Hex(ChainElemVal(31, i)));
    }
    rep->Check(o.desc_bm_lo == ((1ull << kVl) - 1ull) && o.desc_bm_hi == 0ull,
               "cancel: the new descriptor bitmap " + ChainBitmap(o.desc_bm_lo, o.desc_bm_hi) +
                   " expected the new generation's " + Dec(kVl) + " elements");
    cov->cancel_cells += 1;
  }

  // ---- a producer fault stops the elements after it
  {
    const int kVl = 6, kGen = 32, kGrp = 8;
    ch->Reset();
    { ChainStim s; s.desc_alloc = true; s.desc_gen = kGen; s.desc_vl = kVl; s.desc_vd = kGrp;
      s.p_alloc = true; s.p_gen = kGen; s.p_vd = kGrp; s.p_vl = kVl; (void)ch->Step(s); }
    for (int i = 0; i < 3; ++i) {
      ChainStim s;
      s.p_wr = true; s.p_wr_index = i; s.p_wr_data = ChainElemVal(kGen, i); s.p_wr_gen = kGen;
      ChainObs o = ch->Step(s);
      rep->Check(o.p_wr_accept, "fault: element " + Dec(i) + " packet was refused");
    }
    { ChainStim s; s.p_fault = true; s.p_fault_elem = 3;
      ChainObs o = ch->Step(s);
      rep->Check(o.fault && o.fault_elem == 3,
                 "fault: the producer's fault at element 3 was not recorded"); }
    for (int i = 3; i < kVl; ++i) {
      ChainStim s;
      s.p_wr = true; s.p_wr_index = i; s.p_wr_data = ChainElemVal(kGen, i); s.p_wr_gen = kGen;
      ChainObs o = ch->Step(s);
      rep->Check(!o.p_wr_accept,
                 "fault: element " + Dec(i) + " was accepted after a producer fault at 3");
    }
    { ChainStim s; s.c0_alloc = true; s.c0_gen = kGen; s.c0_vs = kGrp; s.c0_vl = kVl; (void)ch->Step(s); }
    for (int i = 0; i < 3; ++i) {
      ChainStim s; s.c0_req = true; s.c0_index = i;
      ChainObs o = ch->Step(s);
      rep->Check(o.c0_req_accept && o.c0_data == ChainElemVal(kGen, i),
                 "fault: committed element " + Dec(i) + " was not readable");
    }
    { ChainStim s; s.c0_req = true; s.c0_index = 3;
      ChainObs o = ch->Step(s);
      rep->Check(!o.c0_req_accept,
                 "fault: element 3 was readable past a producer fault at 3"); }
    cov->fault_cells += 1;
  }
}

void RunVecChainCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep,
                     ChainCov* cov) {
  Chain ch(dut, clk);
  PhaseChainOrder(&ch, rep, cov);
  PhaseChainOnOff(&ch, rep, cov);
  PhaseChainWar(&ch, rep, cov);
  PhaseChainCancel(&ch, rep, cov);

  for (int k = 0; k < CH_ORDER_COUNT; ++k) {
    rep->Check(cov->order_seen[k],
               std::string("coverage: producer order ") + ChainOrderName(k) + " never ran");
  }
  rep->Check(cov->onoff_runs == 2,
             "coverage: " + Dec(cov->onoff_runs) + " on/off runs, expected 2");
  rep->Check(cov->war_cells >= 2,
             "coverage: " + Dec(cov->war_cells) + " WAR cells ran, expected 2");
  rep->Check(cov->cancel_cells >= 1,
             "coverage: " + Dec(cov->cancel_cells) + " cancellation cells ran, expected 1");
  rep->Check(cov->fault_cells >= 1,
             "coverage: " + Dec(cov->fault_cells) + " fault cells ran, expected 1");
}

// ---------------------------------------------------------------------------
// CASE=rvv.mask_prefix_vstart (work package I-057).
//
// `vmsbf`/`vmsif`/`vmsof` are the three instructions the pinned V spec makes an
// illegal-instruction exception when `vstart` is non-zero: they cannot be
// restarted part-way. The rule is a property of the whole instruction, so it is
// decided before any element is touched, and a non-zero `vstart` still executes
// normally. This case observes, for each of the three operations, that
//   * `vstart == 0` executes and writes the prefix mask the model expects;
//   * a non-zero `vstart` is refused as an illegal instruction, with no VRF
//     access and no destination update, and *not* reported as an element fault;
//   * the rule also guards the boundary element lane, so a single operand
//     cannot slip past it without a packet.
void RunMaskPrefixVstartCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);

  const char* kName[3] = {"vmsbf", "vmsif", "vmsof"};
  const int kSewL = 3;       // e8: a mask register is SEW=8, LMUL=1
  const int kVl = 8;
  Layout L;
  L.vd = 8; L.vs1 = 24; L.vs2 = 16; L.mask = false;

  int normal_cells = 0;
  int illegal_cells = 0;

  for (int op = 0; op < 3; ++op) {
    const std::string tag = kName[op];

    // ------------------------------------------------ vstart == 0: executes
    ConfigureVecVstart(&cfg, SewField(kSewL), 0, 0, 0, kVl, 0);
    HostVrf vf;
    PrimeMaskReg(&vec, &vf, L.vs2, kVl, 40 + op);
    PrimeMaskReg(&vec, &vf, L.vs1, kVl, 70 + op);
    PrimeMaskReg(&vec, &vf, L.vd, kVl, 100 + op);
    VecObs ok = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
    rep->Check(!ok.alu_illegal, tag + " vstart=0: the instruction was refused as illegal");
    rep->Check(!ok.alu_trap, tag + " vstart=0: the instruction raised an element fault");
    // `alu_elems` is the count of destination writes the engine performed; the
    // readback below is what proves they landed.
    rep->Check(ok.alu_elems == kVl,
               tag + " vstart=0: " + Dec(ok.alu_elems) + " elements executed, expected " +
                   Dec(kVl));
    std::vector<uint64_t> ev;
    std::vector<bool> mv;
    ComputeExpected(VF_MASKPFX, op, kFormVv, kSewL, 0, 0, kVl, 0, 0, false, vf, L, 0, 0, kVl,
                    &ev, &mv);
    for (int i = 0; i < kVl; ++i) {
      bool got = ((vec.MemRead(L.vd, i / 8, 3, 0) >> (i % 8)) & 1u) != 0;
      rep->Check(got == mv[static_cast<size_t>(i)],
                 tag + " vstart=0 bit" + Dec(i) + ": " + Dec(got) + " expected " +
                     Dec(mv[static_cast<size_t>(i)]));
    }
    ++normal_cells;

    // ------------------------------------------- vstart != 0: illegal, and
    // nothing is read, written or discarded.
    const int vstarts[2] = {1, 3};
    for (int vi = 0; vi < 2; ++vi) {
      const int vs = vstarts[vi];
      ConfigureVecVstart(&cfg, SewField(kSewL), 0, 0, 0, kVl, static_cast<uint64_t>(vs));
      HostVrf vf2;
      PrimeMaskReg(&vec, &vf2, L.vs2, kVl, 40 + op);
      PrimeMaskReg(&vec, &vf2, L.vd, kVl, 100 + op);
      const uint64_t dst_before = HostGet(vf2, L.vd, 0, 3, 0);
      const int rd0 = vec.RdGnt();
      VecObs bad = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
      const std::string t = tag + " vstart=" + Dec(vs);
      rep->Check(bad.alu_illegal, t + ": a non-zero vstart did not raise an illegal instruction");
      rep->Check(!bad.alu_trap, t + ": the exception was reported as an element fault");
      rep->Check(bad.src_rd_ctr == 0 && vec.RdGnt() == rd0,
                 t + ": the refused instruction read the register file");
      rep->Check(bad.alu_elems == 0,
                 t + ": " + Dec(bad.alu_elems) + " elements executed, expected 0");
      rep->Check(vec.MemRead(L.vd, 0, 3, 0) == dst_before,
                 t + ": the refused instruction changed the destination");
      ++illegal_cells;
    }
  }

  // The boundary element lane carries the same rule: a single operand cannot
  // be executed at a non-zero vstart without a packet.
  for (int op = 0; op < 3; ++op) {
    const std::string tag = kName[op];
    VecStim s;
    s.el_valid = true;
    s.el_family = VF_MASKPFX;
    s.el_op = op;
    s.el_form = kFormVv;
    s.el_vs2 = 0x01;
    s.el_pfx = false;
    s.el_index = 0;
    s.el_mask = true;

    ConfigureVecVstart(&cfg, SewField(kSewL), 0, 0, 0, kVl, 0);
    VecObs a = vec.Cycle(s);
    rep->Check(!a.el_illegal, tag + " lane vstart=0: the element was refused as illegal");

    ConfigureVecVstart(&cfg, SewField(kSewL), 0, 0, 0, kVl, 2);
    VecObs b = vec.Cycle(s);
    rep->Check(b.el_illegal, tag + " lane vstart=2: the element was not refused as illegal");
  }

  rep->Check(normal_cells == 3,
             "coverage: " + Dec(normal_cells) + " normal cells ran, expected 3");
  rep->Check(illegal_cells == 6,
             "coverage: " + Dec(illegal_cells) + " illegal-vstart cells ran, expected 6");
}

// ---------------------------------------------------------------------------
// CASE=rvv.mask_prefix_semantics.
//
// The three mask-prefix instructions differ at the first set bit of the source
// mask, and at the all-zero boundary they are not symmetric. Written as rules
// over the active elements [0, vl) of the source, with k the position of the
// first source bit set ("no k" meaning the active slice is all-zero):
//
//   vmsbf.m  vd[i] = 1  iff i <  k;   with no k, vd[i] = 1 for every active i
//   vmsif.m  vd[i] = 1  iff i <= k;   with no k, vd[i] = 1 for every active i
//   vmsof.m  vd[i] = 1  iff i == k;   with no k, vd[i] = 0 for every active i
//
// This is the specification's own statement (v-spec.adoc, "vmsbf.m
// set-before-first mask bit" and its two neighbours), and it is the *source* of
// every expectation here; the RTL is what is under test. The case drives all
// three operations over a source whose first set bit is at every position the
// packet engine can address, plus the three boundaries -- all-zero, first set
// bit at position 0, and a set bit above vl -- and compares the destination
// mask read back from the VRF.
//
// The all-zero row is the surprise the specification states and a naive reading
// misses: vmsbf and vmsif both produce all-ones while vmsof produces all-zeros,
// so an all-zero source mask is *not* symmetric among the three instructions.
void RunMaskPrefixSemanticsCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);

  const char* kName[3] = {"vmsbf", "vmsif", "vmsof"};
  const int kSewL = 3;      // a mask register is SEW=8, LMUL=1
  const int kVlmax = 16;    // VLEN=128 / SEW=8, LMUL=1 -> 16 addressable bits
  Layout L;
  L.vd = 8; L.vs1 = 24; L.vs2 = 16; L.mask = false;

  // Prime a mask register from a 16-bit pattern, through both the VRF and the
  // host model so the oracle sees the same source.
  auto prime = [&](HostVrf* vf, int base, uint64_t bits) {
    for (int b = 0; b < kVlmax / 8; ++b) {
      vec.Prime(*vf, base, b, 3, 0, (bits >> (8 * b)) & 0xFFull);
    }
  };
  auto dst_bit = [&](int i) {
    return ((vec.MemRead(L.vd, i / 8, 3, 0) >> (i % 8)) & 1u) != 0;
  };

  int position_cells = 0;   // (op, first-set-bit position 0..15)
  int allzero_cells = 0;
  int above_vl_cells = 0;
  int tail_cells = 0;
  int pos0_cells = 0;
  int anchor_cells = 0;

  // ------------------------------------------------------------------------
  // Anchor checks against the specification's own worked examples. These do
  // not go through `ComputeExpected`: they are the spec's printed input/output
  // pairs, so the oracle itself is under test and a shared misreading of the
  // rules cannot make both sides agree.
  {
    struct Anchor { uint64_t src; uint64_t want[3]; };
    const Anchor kAnchors[3] = {
        // v3 = 1 0 0 1 0 1 0 0 (first set bit at 2): vmsbf 0000011,
        // vmsif 0000111, vmsof 0000100.
        {0x94ull, {0x03ull, 0x07ull, 0x04ull}},
        // v3 = 1 0 0 1 0 1 0 1 (first set bit at 0): vmsbf 0000000,
        // vmsif 0000001, vmsof 0000001.
        {0x95ull, {0x00ull, 0x01ull, 0x01ull}},
        // first set bit at the top of the byte: 0111111 / 1111111 / 1000000.
        {0x80ull, {0x7Full, 0xFFull, 0x80ull}},
    };
    for (int ai = 0; ai < 3; ++ai) {
      for (int op = 0; op < 3; ++op) {
        ConfigureVec(&cfg, SewField(kSewL), 0, 0, 0, 8);
        HostVrf vf;
        prime(&vf, L.vs2, kAnchors[ai].src);
        prime(&vf, L.vd, 0);
        VecObs o = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false,
                                 kAllCaps);
        rep->Check(!o.alu_illegal && !o.alu_trap,
                   std::string("spec-anchor ") + kName[op] + ": refused (illegal=" +
                       Dec(o.alu_illegal) + " trap=" + Dec(o.alu_trap) + ")");
        for (int i = 0; i < 8; ++i) {
          bool got = dst_bit(i);
          bool exp = ((kAnchors[ai].want[op] >> i) & 1u) != 0;
          rep->Check(got == exp,
                     std::string("spec-anchor ") + kName[op] + " src=" +
                         Dec(static_cast<int>(kAnchors[ai].src)) + " k-bit" + Dec(i) +
                         ": " + Dec(got) + " expected " + Dec(exp));
        }
        ++anchor_cells;
      }
    }
  }

  // ------------------------------------------------------------------------
  // First set bit at every addressable position, vl = kVlmax, against the host
  // model (which is the rule set stated at the top of this function).
  for (int op = 0; op < 3; ++op) {
    for (int k = 0; k < kVlmax; ++k) {
      ConfigureVec(&cfg, SewField(kSewL), 0, 0, 0, kVlmax);
      HostVrf vf;
      prime(&vf, L.vs2, 1ull << k);
      prime(&vf, L.vd, 0xFFFFull);
      VecObs o = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
      std::vector<uint64_t> ev;
      std::vector<bool> mv;
      ComputeExpected(VF_MASKPFX, op, kFormVv, kSewL, 0, 0, kVlmax, 0, 0, false, vf, L, 0, 0,
                      kVlmax, &ev, &mv);
      rep->Check(!o.alu_illegal && !o.alu_trap,
                 std::string("first-bit ") + kName[op] + " k=" + Dec(k) +
                     ": refused (illegal=" + Dec(o.alu_illegal) + " trap=" + Dec(o.alu_trap) +
                     ")");
      for (int i = 0; i < kVlmax; ++i) {
        bool got = dst_bit(i);
        rep->Check(got == mv[static_cast<size_t>(i)],
                   std::string("first-bit ") + kName[op] + " k=" + Dec(k) + " bit" + Dec(i) +
                       ": " + Dec(got) + " expected " + Dec(mv[static_cast<size_t>(i)]));
      }
      ++position_cells;
      if (k == 0) ++pos0_cells;
    }
  }

  // ------------------------------------------------------------------------
  // The all-zero boundary, where the three are not symmetric. The rule is
  // stated directly (vmsbf/vmsif all-ones, vmsof all-zeros) rather than through
  // the shared oracle, so this row cannot be satisfied by a shared mistake.
  for (int op = 0; op < 3; ++op) {
    ConfigureVec(&cfg, SewField(kSewL), 0, 0, 0, kVlmax);
    HostVrf vf;
    prime(&vf, L.vs2, 0);
    prime(&vf, L.vd, 0xFFFFull);
    VecObs o = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
    rep->Check(!o.alu_illegal && !o.alu_trap,
               std::string("all-zero ") + kName[op] + ": refused");
    const bool want = (op < 2);   // vmsbf/vmsif all-ones, vmsof all-zeros
    for (int i = 0; i < kVlmax; ++i) {
      bool got = dst_bit(i);
      rep->Check(got == want,
                 std::string("all-zero ") + kName[op] + " bit" + Dec(i) + ": " + Dec(got) +
                     " expected " + Dec(want) + " (all-zero is not symmetric among the three)");
    }
    ++allzero_cells;
  }

  // ------------------------------------------------------------------------
  // A source bit above vl is not an active element: with vl = 8 and a source
  // whose only set bit is at position 12, the active slice is all-zero, so the
  // rule applies over [0, 8) and the tail [8, 16) stays undisturbed (vta = 0).
  for (int op = 0; op < 3; ++op) {
    ConfigureVec(&cfg, SewField(kSewL), 0, 0, 0, 8);
    HostVrf vf;
    prime(&vf, L.vs2, 1ull << 12);
    prime(&vf, L.vd, 0x55AAull);
    VecObs o = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
    rep->Check(!o.alu_illegal && !o.alu_trap,
               std::string("above-vl ") + kName[op] + ": refused");
    for (int i = 0; i < 8; ++i) {
      bool got = dst_bit(i);
      bool exp = (op < 2);      // active slice is all-zero
      rep->Check(got == exp,
                 std::string("above-vl ") + kName[op] + " active bit" + Dec(i) + ": " + Dec(got) +
                     " expected " + Dec(exp) + " (a bit above vl is not a first set bit)");
    }
    for (int i = 8; i < kVlmax; ++i) {
      bool got = dst_bit(i);
      bool exp = ((0x55AAull >> i) & 1u) != 0;
      rep->Check(got == exp,
                 std::string("above-vl ") + kName[op] + " tail bit" + Dec(i) + ": " + Dec(got) +
                     " expected " + Dec(exp) + " (vta=0 leaves the tail undisturbed)");
    }
    ++above_vl_cells;
  }
  // ... and a set bit below vl wins over one above it: the search is bounded by
  // the active region, so k = 5, not 12.
  for (int op = 0; op < 3; ++op) {
    ConfigureVec(&cfg, SewField(kSewL), 0, 0, 0, 8);
    HostVrf vf;
    prime(&vf, L.vs2, (1ull << 5) | (1ull << 12));
    prime(&vf, L.vd, 0x55AAull);
    VecObs o = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
    rep->Check(!o.alu_illegal && !o.alu_trap,
               std::string("above-vl2 ") + kName[op] + ": refused");
    const uint64_t want_active[3] = {0x1Full, 0x3Full, 0x20ull};
    for (int i = 0; i < 8; ++i) {
      bool got = dst_bit(i);
      bool exp = ((want_active[op] >> i) & 1u) != 0;
      rep->Check(got == exp,
                 std::string("above-vl2 ") + kName[op] + " active bit" + Dec(i) + ": " + Dec(got) +
                     " expected " + Dec(exp) + " (k = 5, the bit above vl is ignored)");
    }
    for (int i = 8; i < kVlmax; ++i) {
      bool got = dst_bit(i);
      bool exp = ((0x55AAull >> i) & 1u) != 0;
      rep->Check(got == exp,
                 std::string("above-vl2 ") + kName[op] + " tail bit" + Dec(i) + ": " + Dec(got) +
                     " expected " + Dec(exp) + " (vta=0 leaves the tail undisturbed)");
    }
    ++above_vl_cells;
  }

  // ------------------------------------------------------------------------
  // The tail policy the mask family implies: mask destinations are tail-
  // agnostic, so with vta = 1 the elements at and above vl are written with
  // all-ones (the agnostic value this unit chooses) while the active elements
  // still follow the rule. With vta = 0 they are left undisturbed, which the
  // above-vl cells just checked.
  for (int op = 0; op < 3; ++op) {
    ConfigureVec(&cfg, SewField(kSewL), 0, /*vta=*/1, 0, 8);
    HostVrf vf;
    prime(&vf, L.vs2, 0);          // active slice all-zero
    prime(&vf, L.vd, 0);           // destination clear, so a write of 1 shows
    VecObs o = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, false, kAllCaps);
    rep->Check(!o.alu_illegal && !o.alu_trap,
               std::string("tail-agnostic ") + kName[op] + ": refused");
    for (int i = 0; i < 8; ++i) {
      bool got = dst_bit(i);
      bool exp = (op < 2);     // active all-zero rule
      rep->Check(got == exp,
                 std::string("tail-agnostic ") + kName[op] + " active bit" + Dec(i) + ": " +
                     Dec(got) + " expected " + Dec(exp));
    }
    for (int i = 8; i < kVlmax; ++i) {
      bool got = dst_bit(i);
      rep->Check(got,
                 std::string("tail-agnostic ") + kName[op] + " tail bit" + Dec(i) +
                     ": " + Dec(got) + " expected 1 (mask tails are agnostic)");
    }
    ++tail_cells;
  }

  // ------------------------------------------------------------------------ coverage
  rep->Check(position_cells == 3 * kVlmax,
             "coverage: " + Dec(position_cells) + " first-set-bit cells ran, expected " +
                 Dec(3 * kVlmax));
  rep->Check(pos0_cells == 3,
             "coverage: " + Dec(pos0_cells) + " first-set-bit-at-0 cells ran, expected 3");
  rep->Check(allzero_cells == 3,
             "coverage: " + Dec(allzero_cells) + " all-zero cells ran, expected 3");
  rep->Check(above_vl_cells == 6,
             "coverage: " + Dec(above_vl_cells) + " above-vl cells ran, expected 6");
  rep->Check(tail_cells == 3,
             "coverage: " + Dec(tail_cells) + " tail-agnostic cells ran, expected 3");
  rep->Check(anchor_cells == 9,
             "coverage: " + Dec(anchor_cells) + " spec-anchor cells ran, expected 9");
}

// ---------------------------------------------------------------------------
// CASE=rvv.mask_prefix_masked.
//
// The masked forms of the three mask-prefix instructions, `vmsbf.m`/`vmsif.m`/
// `vmsof.m vd, vs2, v0.t`. The specification's own worked example for the
// masked form is the rule's source (v-spec.adoc, "vmsbf.m set-before-first mask
// bit", the fourth example):
//
//     1 1 0 0 0 0 1 1   v0 (mask)
//     1 0 0 1 0 1 0 0   v3 (source)
//                       vmsbf.m v2, v3, v0.t
//     0 1 x x x x 1 1   v2
//
// The instruction "writes a 1 to all active mask elements before the first
// active source element that is a 1" (vmsbf), "also includes the element with a
// set bit" (vmsif) and "only sets the first element with a bit set, if any"
// (vmsof). The search is therefore over the ACTIVE elements only: element 4 of
// the example has a set source bit but is masked off, and it does not make
// element 4 -- or anything before it -- "the first". A masked-off source
// element contributes *nothing* to the search; it is ignored, not read as a
// zero (the two agree on k, and the rule that matters is that the element is
// not in the search at all).
//
// The destination of a masked-off element is a mask destination, so the
// masked-off (vma) and tail (vta) policies apply: vma/vta = 0 leaves the
// element undisturbed, vma/vta = 1 writes the mask-agnostic all-ones. The
// specification's printed examples show `x` for the masked-off elements, i.e.
// vma = 0.
//
// The host oracle is the shared `mosaic_maskpfx::MaskedExpectedBits`
// (sim/unit/mask_prefix_ref.h), which states the rules directly and is computed
// on the host from the source and mask patterns; it is never read back from the
// RTL. The specification's own printed masked examples are checked separately,
// as spec-anchor cells, so the oracle itself is under test. CASE=
// vec.mask_prefix_at_core calls the same function, so the core-level masked
// cells cannot drift from this case's.
uint64_t MaskedPrefixExpectedBits(int op, uint64_t src, uint64_t mask, uint64_t old,
                                  int vl, int vma, int vta) {
  return mosaic_maskpfx::MaskedExpectedBits(op, src, mask, old, vl, vma, vta);
}

void RunMaskPrefixMaskedCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);

  const char* kName[3] = {"vmsbf", "vmsif", "vmsof"};
  const int kSewL = 3;      // a mask register is SEW=8, LMUL=1
  const int kVlmax = 16;    // VLEN=128 / SEW=8, LMUL=1 -> 16 addressable bits
  Layout L;
  L.vd = 8; L.vs1 = 24; L.vs2 = 16; L.mask = false;

  auto prime = [&](HostVrf* vf, int base, uint64_t bits) {
    for (int b = 0; b < kVlmax / 8; ++b) {
      vec.Prime(*vf, base, b, 3, 0, (bits >> (8 * b)) & 0xFFull);
    }
  };
  auto dst_bit = [&](int i) {
    return ((vec.MemRead(L.vd, i / 8, 3, 0) >> (i % 8)) & 1u) != 0;
  };

  int anchor_cells = 0, position_cells = 0, masked_before_cells = 0;
  int masked_only_cells = 0, vma_cells = 0, vta_cells = 0;

  // Run one masked cell and compare every destination bit against the host
  // oracle. `mask_bits` is the v0 operand. When `want_override` is non-null the
  // expected destination is that value (the specification's own printed result)
  // rather than the oracle's.
  auto run_cell = [&](const std::string& what, int op, uint64_t src, uint64_t mask_bits,
                      uint64_t dst_seed, int vl, int vma, int vta,
                      const uint64_t* want_override) {
    ConfigureVec(&cfg, SewField(kSewL), 0, vta, vma, static_cast<uint64_t>(vl));
    HostVrf vf;
    prime(&vf, L.vs2, src);
    prime(&vf, 0, mask_bits);
    prime(&vf, L.vd, dst_seed);
    const uint64_t want =
        (want_override != nullptr)
            ? *want_override
            : MaskedPrefixExpectedBits(op, src, mask_bits, dst_seed, vl, vma, vta);
    VecObs o = vec.RunPacket(VF_MASKPFX, op, kFormVv, L.vd, L.vs1, L.vs2, 0, true, kAllCaps);
    const std::string tag = what + " " + kName[op];
    rep->Check(!o.alu_illegal && !o.alu_trap,
               tag + ": refused (illegal=" + Dec(o.alu_illegal) + " trap=" +
                   Dec(o.alu_trap) + ")");
    for (int i = 0; i < kVlmax; ++i) {
      const bool got = dst_bit(i);
      const bool exp = ((want >> i) & 1u) != 0;
      rep->Check(got == exp,
                 tag + " bit" + Dec(i) + ": " + Dec(got) + " expected " + Dec(exp));
    }
  };

  // ------------------------------------------------------------------------
  // The first active set bit at every addressable position. The mask turns off
  // exactly the elements below k, so the first ACTIVE set bit is k for every k.
  // No masked-off element here carries a set bit, which is the cell the
  // masked-off-as-a-set-bit defect must fail and the search-over-all defect
  // must pass.
  for (int op = 0; op < 3; ++op) {
    for (int k = 0; k < kVlmax; ++k) {
      const uint64_t m = (k == 0) ? 0xFFFFull : ((0xFFFFull << k) & 0xFFFFull);
      run_cell("masked-position k=" + Dec(k), op, m, m, 0x5555ull, kVlmax, 0, 0, nullptr);
      ++position_cells;
    }
  }

  // ------------------------------------------------------------------------
  // A set bit masked off *before* a set bit that is active: the two searches
  // disagree about k. A masked-off element here does carry a set bit, which is
  // the cell the search-over-all defect must fail.
  for (int op = 0; op < 3; ++op) {
    for (int k = 1; k < kVlmax; ++k) {
      const uint64_t m = 0xFFFEull;             // element 0 is masked off
      const uint64_t s = 1ull | (1ull << k);    // set at 0 (masked off) and at k
      run_cell("masked-off-before-active k=" + Dec(k), op, s, m, 0x0000ull, kVlmax, 0, 0,
               nullptr);
      ++masked_before_cells;
    }
  }

  // ------------------------------------------------------------------------
  // The only set bit is masked off: the active search finds none. The active
  // slice is all-zero, where the three are not symmetric -- vmsbf/vmsif write
  // all-ones, vmsof all-zeros -- and an implementation that searched over all
  // elements would find k instead.
  for (int op = 0; op < 3; ++op) {
    for (int k = 0; k < kVlmax; ++k) {
      const uint64_t s = 1ull << k;
      const uint64_t m = (~(1ull << k)) & 0xFFFFull;
      run_cell("masked-off-only k=" + Dec(k), op, s, m, 0x5555ull, kVlmax, 0, 0, nullptr);
      ++masked_only_cells;
    }
  }

  // ------------------------------------------------------------------------
  // vma = 0 and vma = 1 on a masked destination. Masked-off elements sit both
  // before and after the first active set bit; under vma = 0 they are
  // undisturbed (the seed survives), under vma = 1 they are written with the
  // mask-agnostic all-ones.
  {
    struct VmaCase { uint64_t mask, src; };
    const VmaCase kCases[2] = {
        {0x0F0Full, 0x0022ull},   // k = 1; a set bit at 5 is masked off
        {0xFFF0ull, 0x0011ull},   // k = 4; masked-off elements precede k
    };
    for (int ci = 0; ci < 2; ++ci) {
      for (int vma = 0; vma < 2; ++vma) {
        for (int op = 0; op < 3; ++op) {
          run_cell("vma=" + Dec(vma) + " pattern=" + Dec(ci), op, kCases[ci].src,
                   kCases[ci].mask, 0x0000ull, kVlmax, vma, 0, nullptr);
          ++vma_cells;
        }
      }
    }
  }

  // ------------------------------------------------------------------------
  // vta past vl: with vl = 8 the tail [8, 16) is undisturbed under vta = 0 and
  // all-ones under vta = 1, and a source bit above vl is not a first set bit.
  {
    struct VtaCase { uint64_t mask, src; const char* name; };
    const VtaCase kCases[2] = {
        {0x00FFull, 0x0010ull, "tail-below-set"},   // k = 4 within [0, 8)
        {0x00FFull, 0x1000ull, "tail-above-set"},   // the only set bit is above vl
    };
    for (int ci = 0; ci < 2; ++ci) {
      for (int vta = 0; vta < 2; ++vta) {
        for (int op = 0; op < 3; ++op) {
          run_cell(std::string("vta=") + Dec(vta) + " " + kCases[ci].name, op,
                   kCases[ci].src, kCases[ci].mask, 0x0000ull, 8, 0, vta, nullptr);
          ++vta_cells;
        }
      }
    }
  }

  // ------------------------------------------------------------------------
  // The specification's own printed masked examples, checked directly rather
  // than through the oracle, so a shared misreading cannot make both sides
  // agree. The `x` elements of the printed result are the masked-off elements
  // under vma = 0: they keep the destination seed.
  {
    struct MaskAnchor { uint64_t v0; uint64_t vs2[3]; uint64_t want[3]; };
    const MaskAnchor kAnchors[1] = {
        // v0 = 11000011. v3 = 10010100 for vmsbf/vmsif (first active set bit at
        // element 7; elements 4 and 2 are set but masked off). The printed
        // results are 00000011 / 11000011, i.e. the active bits are 0x43 /
        // 0xC3. For vmsof v3 = 11010100, whose first active set bit is element
        // 6, and the printed result 01000000 is 0x40.
        {0xC3ull, {0x94ull, 0x94ull, 0xD4ull}, {0x43ull, 0xC3ull, 0x40ull}},
    };
    const uint64_t kSeed = 0x5Aull;
    for (int ai = 0; ai < 1; ++ai) {
      for (int op = 0; op < 3; ++op) {
        const uint64_t active = kAnchors[ai].v0;
        const uint64_t want = (kSeed & ~active) | (kAnchors[ai].want[op] & active);
        run_cell("spec-anchor", op, kAnchors[ai].vs2[op], kAnchors[ai].v0, kSeed,
                 kVlmax, 0, 0, &want);
        ++anchor_cells;
      }
    }
  }

  // ------------------------------------------------------------------ coverage
  rep->Check(position_cells == 3 * kVlmax,
             "coverage: " + Dec(position_cells) + " first-active-set-bit cells ran, expected " +
                 Dec(3 * kVlmax));
  rep->Check(masked_before_cells == 3 * (kVlmax - 1),
             "coverage: " + Dec(masked_before_cells) +
                 " masked-off-before-active cells ran, expected " + Dec(3 * (kVlmax - 1)));
  rep->Check(masked_only_cells == 3 * kVlmax,
             "coverage: " + Dec(masked_only_cells) + " masked-off-only cells ran, expected " +
                 Dec(3 * kVlmax));
  rep->Check(vma_cells == 12,
             "coverage: " + Dec(vma_cells) + " vma cells ran, expected 12");
  rep->Check(vta_cells == 12,
             "coverage: " + Dec(vta_cells) + " vta cells ran, expected 12");
  rep->Check(anchor_cells == 3,
             "coverage: " + Dec(anchor_cells) + " spec-anchor cells ran, expected 3");
}

// ============================================================================
// I-061 -- the same-hart line coalescer (CASE=coalesce.element_faults).
//
// The coalescer merges the items of one vector memory macro that share one
// 8-byte memory beat whenever the merge cannot change the defined result. The
// case runs the *same* macros with coalescing on and off and requires every
// defined value and fault to be identical; on top of that identity it asserts
// the merge actually happened (the request count fell and the reduction counter
// moved -- a coalescer that never merges is the silent failure), that a fault
// inside a merged group still names the element that caused it and leaves the
// elements before it in effect, that a device or atomic access is never merged
// even when its elements share a beat, and that two same-address stores apply
// in element order.
//
// The two addresses the case uses are read off the profiles' memory maps
// (config/memory/*.json): RAM is normal, idempotent memory in every profile and
// the UART is a device in every profile.
// ============================================================================

const uint64_t kCoalRamBase = 0x80000000ull;   // normal memory, every profile
const uint64_t kCoalDevBase = 0x100000ull;     // uart, a device, every profile

int CoalBe(int sew_l) { return (1 << sew_l) / 8; }

uint64_t CoalAddr(int mode, uint64_t base, int64_t stride, int e, int be) {
  if (mode == LS_UNIT) {
    return base + static_cast<uint64_t>(e) * static_cast<uint64_t>(be);
  }
  return base + static_cast<uint64_t>(e) * static_cast<uint64_t>(stride);
}

uint64_t CoalVal(bool we, int i, int sew_l) {
  return Pat(313 * (we ? 1 : 0) + 17 * i + 5) & MaskW(1 << sew_l);
}

struct CoalCoverage {
  bool mode_seen[LS_MODE_COUNT] = {};
  bool dir_seen[2] = {};
  int cells = 0;
  int merged_cells = 0;
  int req_coal = 0;
  int req_unco = 0;
};

struct CoalRes {
  std::vector<uint64_t> dest;
  std::vector<uint8_t> mem;
  int requests = 0;
  uint32_t merge_ctr = 0;
  bool done = false;
  bool trap = false;
  int trap_elem = 0;
  std::vector<LsuRec> reqrec;
};

bool CoalSameBytes(const std::vector<uint8_t>& got, const std::vector<uint8_t>& exp,
                   std::string* why) {
  if (got.size() != exp.size()) {
    *why = "size";
    return false;
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != exp[i]) {
      *why = "byte " + mosaic::Hex(static_cast<uint64_t>(i), 4) + " is " +
             mosaic::Hex(got[i], 2) + " expected " + mosaic::Hex(exp[i], 2);
      return false;
    }
  }
  return true;
}

// Configure the macro and prime both the source (store data) and destination
// (load destination) groups, so an undisturbed destination element is known.
void CoalSetup(Cfg* cfg, Vec* vec, int sew_l, int lmul, int vl, HostVrf* vf, int vd,
               int data, std::vector<uint64_t>* old) {
  LsuConfig(cfg, sew_l, VlmulOfExp(lmul), static_cast<uint64_t>(vl), 0, 0, 0);
  const int vlmax = LsuVlmaxOf(sew_l, lmul);
  old->assign(static_cast<size_t>(vlmax), 0);
  for (int i = 0; i < vlmax; ++i) {
    vec->Prime(*vf, data, i, sew_l, lmul, CoalVal(true, i, sew_l));
    const uint64_t o = CoalVal(false, i, sew_l);
    vec->Prime(*vf, vd, i, sew_l, lmul, o);
    (*old)[static_cast<size_t>(i)] = o;
  }
}

CoalRes CoalExec(Vec* vec, Lsu* lsu, LsuMem* mem, int mode, int sew_l, int lmul, int vl,
                 uint64_t base, int64_t stride, bool we, int vd, int data, bool coalesce,
                 bool atomic) {
  CoalRes r;
  LsuObs o = lsu->Run(mode, we, false, 1, vd, data, 0, sew_l, base, stride, false,
                      40000, 0xFF, coalesce, atomic);
  r.done = o.done;
  r.trap = o.trap;
  r.trap_elem = o.trap_elem;
  r.requests = static_cast<int>(lsu->reqs().size());
  r.merge_ctr = o.merge_ctr;
  r.reqrec = lsu->reqs();
  const int vlmax = LsuVlmaxOf(sew_l, lmul);
  r.dest.assign(static_cast<size_t>(vlmax), 0);
  for (int i = 0; i < vlmax; ++i) {
    r.dest[static_cast<size_t>(i)] = vec->MemRead(vd, i, sew_l, lmul);
  }
  r.mem = mem->mem;
  return r;
}

// The memory a store macro defines: every element applied in ascending element
// order. That order is the rule the coalescer declares for repeated-address
// stores, so this function is that rule's oracle.
std::vector<uint8_t> CoalExpStoreMem(const std::vector<uint8_t>& pristine, int mode,
                                     uint64_t base, int64_t stride, int vl_eff, int be,
                                     int sew_l) {
  std::vector<uint8_t> m = pristine;
  for (int e = 0; e < vl_eff; ++e) {
    const uint64_t addr = CoalAddr(mode, base, stride, e, be);
    const uint64_t val = CoalVal(true, e, sew_l);
    for (int k = 0; k < be; ++k) {
      m[static_cast<size_t>((addr + static_cast<uint64_t>(k)) & 0xFFFFull)] =
          static_cast<uint8_t>((val >> (8 * k)) & 0xFFull);
    }
  }
  return m;
}

// The byte-enabled beats of a request stream: one entry per beat, the union of
// the byte masks of the requests that named it.
std::map<uint64_t, uint8_t> CoalBeats(const std::vector<LsuRec>& reqs) {
  std::map<uint64_t, uint8_t> m;
  for (size_t i = 0; i < reqs.size(); ++i) {
    m[reqs[i].addr & ~7ull] |= reqs[i].mask;
  }
  return m;
}

// The coalesced and uncoalesced runs must define the same destination values.
void CoalCheckDest(Reporter* rep, const std::string& name, const CoalRes& off,
                   const CoalRes& on, const std::vector<uint64_t>& old, int vl_eff) {
  for (size_t i = 0; i < off.dest.size() && i < on.dest.size(); ++i) {
    const uint64_t want = (static_cast<int>(i) < vl_eff) ? off.dest[i] : old[i];
    rep->Check(on.dest[i] == want,
               name + " dest e" + Dec(static_cast<int>(i)) + ": coalesced " +
                   mosaic::Hex(on.dest[i], 16) + " uncoalesced " + mosaic::Hex(off.dest[i], 16));
  }
}

// ------------------------------------------------------------------ phases

// The coalesced run must define the same values as the uncoalesced one, merge
// where a merge is legal, and not merge where it is not.
void PhaseCoalEquivalence(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                          CoalCoverage* cov) {
  struct Row {
    int mode; int sew_l; int lmul; int vl; uint64_t off; int64_t stride; bool we;
    bool merge; const char* tag;
  };
  const Row rows[] = {
      {LS_UNIT, 3, 0, 8, 0, 0, false, true, "e8"},
      {LS_UNIT, 3, 0, 8, 0, 0, true, true, "e8"},
      {LS_UNIT, 3, 0, 16, 3, 0, false, true, "e8-midbeat"},
      {LS_UNIT, 3, 0, 16, 3, 0, true, true, "e8-midbeat"},
      {LS_UNIT, 4, 0, 8, 0, 0, false, true, "e16"},
      {LS_UNIT, 4, 0, 8, 0, 0, true, true, "e16"},
      {LS_UNIT, 5, 0, 4, 0, 0, false, true, "e32"},
      {LS_UNIT, 5, 0, 4, 0, 0, true, true, "e32"},
      {LS_UNIT, 6, 0, 2, 0, 0, false, false, "e64"},
      {LS_STRIDED, 4, 0, 4, 0, 4, false, true, "stride4"},
      {LS_STRIDED, 3, 0, 4, 0, 2, false, true, "stride2"},
      {LS_STRIDED, 3, 0, 4, 0, 8, false, false, "stride8"},
      {LS_STRIDED, 5, 0, 4, 0, 4, true, true, "stride4"},
      {LS_STRIDED, 5, 0, 4, 0, 0, false, true, "same-address"},
  };
  const int vd = 8, data = 16;
  for (size_t ri = 0; ri < sizeof(rows) / sizeof(rows[0]); ++ri) {
    const Row& R = rows[ri];
    const std::string name = std::string("coalesce ") + LsuModeName(R.mode) + " " + R.tag +
                             (R.we ? " store" : " load");
    const int be = CoalBe(R.sew_l);
    const int vlmax = LsuVlmaxOf(R.sew_l, R.lmul);
    const int vl_eff = R.vl > vlmax ? vlmax : R.vl;
    const uint64_t base = kCoalRamBase + R.off;

    HostVrf vf;
    std::vector<uint64_t> old;
    mem->OneRam();
    mem->ClearFaults();
    CoalSetup(cfg, vec, R.sew_l, R.lmul, R.vl, &vf, vd, data, &old);
    const std::vector<uint8_t> pristine = mem->mem;

    mem->mem = pristine;
    CoalRes off = CoalExec(vec, lsu, mem, R.mode, R.sew_l, R.lmul, R.vl, base, R.stride,
                           R.we, vd, data, false, false);
    mem->mem = pristine;
    CoalRes on = CoalExec(vec, lsu, mem, R.mode, R.sew_l, R.lmul, R.vl, base, R.stride,
                          R.we, vd, data, true, false);

    rep->Check(off.done && on.done, name + ": a macro never completed");
    rep->Check(!off.trap && !on.trap, name + ": a macro trapped");
    rep->Check(off.requests == vl_eff,
               name + " (uncoalesced): " + Dec(off.requests) + " requests for " + Dec(vl_eff) +
                   " elements (one per item)");

    if (R.we) {
      const std::vector<uint8_t> exp =
          CoalExpStoreMem(pristine, R.mode, base, R.stride, vl_eff, be, R.sew_l);
      std::string why;
      rep->Check(CoalSameBytes(off.mem, exp, &why), name + " (uncoalesced): " + why);
      rep->Check(CoalSameBytes(on.mem, exp, &why), name + " (coalesced): " + why);
      rep->Check(CoalSameBytes(on.mem, off.mem, &why),
                 name + ": coalesced memory differs from uncoalesced: " + why);
    } else {
      for (int i = 0; i < vlmax; ++i) {
        const uint64_t exp = (i < vl_eff)
            ? (mem->Elem(CoalAddr(R.mode, base, R.stride, i, be), be) & MaskW(1 << R.sew_l))
            : old[static_cast<size_t>(i)];
        rep->Check(off.dest[static_cast<size_t>(i)] == exp,
                   name + " (uncoalesced) dest e" + Dec(i) + ": " +
                       mosaic::Hex(off.dest[static_cast<size_t>(i)], 16) + " expected " +
                       mosaic::Hex(exp, 16));
        rep->Check(on.dest[static_cast<size_t>(i)] == exp,
                   name + " (coalesced) dest e" + Dec(i) + ": " +
                       mosaic::Hex(on.dest[static_cast<size_t>(i)], 16) + " expected " +
                       mosaic::Hex(exp, 16));
      }
    }

    // the merged byte mask must be the union of the elements' masks
    rep->Check(CoalBeats(on.reqrec) == CoalBeats(off.reqrec),
               name + ": the coalesced byte mask is not the union of the elements' masks");

    if (R.merge) {
      rep->Check(on.requests < off.requests,
                 name + ": no merge happened (" + Dec(on.requests) + " of " +
                     Dec(off.requests) + " requests)");
      rep->Check(static_cast<int>(on.merge_ctr) == off.requests - on.requests,
                 name + ": the reduction counter says " + Dec(on.merge_ctr) +
                     ", requests fell by " + Dec(off.requests - on.requests));
      cov->merged_cells += 1;
    } else {
      rep->Check(on.requests == off.requests,
                 name + ": a macro with no mergeable items was merged (" + Dec(on.requests) +
                     " of " + Dec(off.requests) + ")");
      rep->Check(on.merge_ctr == 0, name + ": the reduction counter moved with no merge");
    }

    cov->mode_seen[R.mode] = true;
    cov->dir_seen[R.we ? 1 : 0] = true;
    cov->cells += 1;
    cov->req_coal += on.requests;
    cov->req_unco += off.requests;
  }
}

// A fault inside a merged group must still name the element that caused it; the
// elements before it must take effect and the ones at or after it must not.
void PhaseCoalFault(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                    CoalCoverage* cov) {
  struct Row { int mode; int sew_l; int lmul; int vl; int64_t stride; const char* tag; };
  const Row rows[] = {
      {LS_UNIT, 3, 0, 8, 0, "e8"},
      {LS_UNIT, 4, 0, 4, 0, "e16"},
      {LS_STRIDED, 5, 0, 4, 4, "stride4"},
  };
  const int positions[] = {0, 1, 3, 7};
  const int vd = 8, data = 16;
  for (size_t ri = 0; ri < sizeof(rows) / sizeof(rows[0]); ++ri) {
    const Row& R = rows[ri];
    for (int we = 0; we < 2; ++we) {
      for (int pi = 0; pi < 4; ++pi) {
        const int pos = positions[pi];
        if (pos >= R.vl) continue;
        const int be = CoalBe(R.sew_l);
        const int vl_eff = LsuVlmaxOf(R.sew_l, R.lmul) < R.vl ? LsuVlmaxOf(R.sew_l, R.lmul) : R.vl;
        const uint64_t base = kCoalRamBase;
        const std::string name = std::string("coalesce-fault ") + LsuModeName(R.mode) + " " +
                                 R.tag + (we ? " store " : " load ") + "at " + Dec(pos);

        HostVrf vf;
        std::vector<uint64_t> old;
        mem->OneRam();
        mem->ClearFaults();
        CoalSetup(cfg, vec, R.sew_l, R.lmul, R.vl, &vf, vd, data, &old);
        const std::vector<uint8_t> pristine = mem->mem;
        const uint64_t faddr = CoalAddr(R.mode, base, R.stride, pos, be);
        mem->FaultByteRange(faddr, be);

        mem->mem = pristine;
        CoalRes off = CoalExec(vec, lsu, mem, R.mode, R.sew_l, R.lmul, R.vl, base, R.stride,
                               we != 0, vd, data, false, false);
        mem->mem = pristine;
        CoalRes on = CoalExec(vec, lsu, mem, R.mode, R.sew_l, R.lmul, R.vl, base, R.stride,
                              we != 0, vd, data, true, false);

        rep->Check(off.done && on.done, name + ": a macro never completed");
        rep->Check(off.trap && on.trap, name + ": the fault was not reported");
        rep->Check(on.trap_elem == pos,
                   name + ": vstart " + Dec(on.trap_elem) + " expected " + Dec(pos) +
                       " (a whole-group fault destroys element granularity)");
        rep->Check(on.trap_elem == off.trap_elem,
                   name + ": the coalesced fault names element " + Dec(on.trap_elem) +
                       ", the uncoalesced run names " + Dec(off.trap_elem));

        // the elements before the fault took effect; at or after did not
        if (we != 0) {
          std::string why;
          rep->Check(CoalSameBytes(on.mem, off.mem, &why),
                     name + ": coalesced memory differs from uncoalesced: " + why);
          for (int e = 0; e < vl_eff; ++e) {
            const uint64_t addr = CoalAddr(R.mode, base, R.stride, e, be);
            const uint8_t got = mem->mem[static_cast<size_t>(addr & 0xFFFFull)];
            const uint8_t exp = (e < pos)
                ? static_cast<uint8_t>(CoalVal(true, e, R.sew_l) & 0xFFull)
                : pristine[static_cast<size_t>(addr & 0xFFFFull)];
            rep->Check(got == exp, name + ": store element " + Dec(e) + " is " +
                                       mosaic::Hex(got, 2) + " expected " + mosaic::Hex(exp, 2));
          }
        } else {
          CoalCheckDest(rep, name, off, on, old, vl_eff);
          for (int e = 0; e < vl_eff; ++e) {
            const uint64_t want = (e < pos)
                ? (mem->Elem(CoalAddr(R.mode, base, R.stride, e, be), be) & MaskW(1 << R.sew_l))
                : old[static_cast<size_t>(e)];
            rep->Check(on.dest[static_cast<size_t>(e)] == want,
                       name + ": load element " + Dec(e) + " is " +
                           mosaic::Hex(on.dest[static_cast<size_t>(e)], 16) + " expected " +
                           mosaic::Hex(want, 16));
          }
        }
        cov->cells += 1;
      }
    }
  }
}

// An MMIO/device or atomic access must never be merged, even when its elements
// share one beat; the same shape in RAM does merge, so the predicate is what
// makes the difference.
void PhaseCoalDevice(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                     CoalCoverage* cov) {
  const int vd = 8, data = 16;
  const int sew_l = 3, lmul = 0, vl = 8;

  // the whole macro sits in the device and shares one beat
  HostVrf vf;
  std::vector<uint64_t> old;
  mem->ClearFaults();
  CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
  CoalRes dev = CoalExec(vec, lsu, mem, LS_UNIT, sew_l, lmul, vl, kCoalDevBase, 0, false,
                         vd, data, true, false);
  rep->Check(dev.done && !dev.trap, "device: the macro did not complete cleanly");
  rep->Check(dev.requests == vl,
             "device: " + Dec(dev.requests) + " requests for " + Dec(vl) +
                 " device elements sharing one beat (a device access was coalesced)");
  rep->Check(dev.merge_ctr == 0, "device: the reduction counter moved for a device access");
  bool at_dev = true;
  for (size_t i = 0; i < dev.reqrec.size(); ++i) {
    if ((dev.reqrec[i].addr & ~7ull) != (kCoalDevBase & ~7ull)) at_dev = false;
  }
  rep->Check(at_dev, "device: a request left the device region");

  // the same shape in RAM does merge, so the check above is not vacuous
  mem->OneRam();
  CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
  CoalRes ram = CoalExec(vec, lsu, mem, LS_UNIT, sew_l, lmul, vl, kCoalRamBase, 0, false,
                         vd, data, true, false);
  rep->Check(ram.requests == 1 && ram.merge_ctr == vl - 1,
             "device: the RAM contrast did not merge (" + Dec(ram.requests) + " requests)");

  // an atomic class access is never merged, and the same macro without the
  // atomic class does merge
  mem->OneRam();
  CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
  CoalRes at = CoalExec(vec, lsu, mem, LS_UNIT, sew_l, lmul, vl, kCoalRamBase, 0, false,
                        vd, data, true, true);
  rep->Check(at.requests == vl && at.merge_ctr == 0,
             "atomic: an atomic class access was coalesced (" + Dec(at.requests) +
                 " requests, merge_ctr " + Dec(at.merge_ctr) + ")");
  mem->OneRam();
  CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
  CoalRes nat = CoalExec(vec, lsu, mem, LS_UNIT, sew_l, lmul, vl, kCoalRamBase, 0, false,
                         vd, data, true, false);
  rep->Check(nat.requests == 1, "atomic: the non-atomic contrast did not merge");

  // a device element among normal elements is not merged with them
  mem->OneRam();
  CoalSetup(cfg, vec, sew_l, lmul, 2, &vf, vd, data, &old);
  CoalRes mixed = CoalExec(vec, lsu, mem, LS_STRIDED, 5, lmul, 2, kCoalDevBase, 0x7FF00000LL,
                           false, vd, data, true, false);
  rep->Check(mixed.done && !mixed.trap, "device: the mixed macro did not complete cleanly");
  rep->Check(mixed.requests == 2,
             "device: " + Dec(mixed.requests) + " requests for a device element among "
             "normal ones (an MMIO access was merged)");
  rep->Check(mixed.merge_ctr == 0, "device: the reduction counter moved for a mixed macro");

  cov->cells += 4;
}

// Two stores to the same address inside one macro apply in the order the
// instruction type defines: element order. They are never merged, because their
// bytes overlap.
void PhaseCoalStoreOrder(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                         CoalCoverage* cov) {
  const int vd = 8, data = 16;
  const int sew_l = 5, lmul = 0, vl = 4;
  const int be = CoalBe(sew_l);
  const uint64_t base = kCoalRamBase;

  HostVrf vf;
  std::vector<uint64_t> old;
  mem->OneRam();
  mem->ClearFaults();
  CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
  const std::vector<uint8_t> pristine = mem->mem;

  mem->mem = pristine;
  CoalRes off = CoalExec(vec, lsu, mem, LS_STRIDED, sew_l, lmul, vl, base, 0, true, vd, data,
                         false, false);
  mem->mem = pristine;
  CoalRes on = CoalExec(vec, lsu, mem, LS_STRIDED, sew_l, lmul, vl, base, 0, true, vd, data,
                        true, false);

  const std::string name = "store-order same-address";
  rep->Check(off.done && on.done && !off.trap && !on.trap, name + ": a macro did not complete");
  rep->Check(off.requests == vl,
             name + " (uncoalesced): " + Dec(off.requests) + " requests expected " + Dec(vl));
  rep->Check(on.requests == vl,
             name + ": " + Dec(on.requests) + " requests for " + Dec(vl) +
                 " overlapping stores (an overlapping pair was merged)");
  rep->Check(on.merge_ctr == 0, name + ": the reduction counter moved for overlapping stores");

  const uint64_t want = CoalVal(true, vl - 1, sew_l) & MaskW(1 << sew_l);
  const uint64_t got = mem->Elem(base, be);
  rep->Check(got == want,
             name + ": memory holds " + mosaic::Hex(got, 16) + " expected the last element's " +
                 mosaic::Hex(want, 16) + " (the declared order is element order)");

  std::string why;
  rep->Check(CoalSameBytes(on.mem, off.mem, &why),
             name + ": coalesced memory differs from uncoalesced: " + why);

  bool ascending = on.reqrec.size() == static_cast<size_t>(vl);
  for (size_t i = 0; ascending && i + 1 < on.reqrec.size(); ++i) {
    if (on.reqrec[i].elem > on.reqrec[i + 1].elem) ascending = false;
  }
  rep->Check(ascending, name + ": the coalesced request order is not ascending element order");

  // a disjoint store pair in the same beat *does* merge, so the rule is about
  // the bytes overlapping, not about the beat
  mem->OneRam();
  CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
  CoalRes dis_off = CoalExec(vec, lsu, mem, LS_STRIDED, sew_l, lmul, vl, base, 4, true, vd,
                             data, false, false);
  mem->OneRam();
  CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
  CoalRes dis = CoalExec(vec, lsu, mem, LS_STRIDED, sew_l, lmul, vl, base, 4, true, vd, data,
                         true, false);
  rep->Check(dis.requests == 2 &&
                 dis.merge_ctr == static_cast<uint32_t>(dis_off.requests - dis.requests),
             name + ": a disjoint pair in one beat did not merge (" + Dec(dis.requests) +
                 " of " + Dec(dis_off.requests) + " requests, merge_ctr " + Dec(dis.merge_ctr) +
                 ")");

  cov->cells += 2;
}

// A precise-interrupt boundary stop taken while a group is pending must finish
// that group -- its items lie before the boundary -- then stop, leaving `vstart`
// at the first element not performed. The stop is driven through the I-057
// controller (with an allocated descriptor), so this exercises the LSU's stop
// path under coalescing; the uncoalesced pacing is the same shape.
void PhaseCoalStop(Cfg* cfg, Vec* vec, Lsu* lsu, LsuMem* mem, Reporter* rep,
                   CoalCoverage* cov) {
  const int vl = 16, sew_l = 3, lmul = 0, vd = 8, data = 16;
  const int intr_at = 8;   // request the stop once the first beat is issued
  for (int we = 0; we < 2; ++we) {
    for (int coalesce = 0; coalesce < 2; ++coalesce) {
      const std::string name = std::string("coalesce-stop unit e8 ") + (we ? "store " : "load ") +
                               (coalesce ? "coalescing" : "uncoalesced");
      HostVrf vf;
      std::vector<uint64_t> old;
      mem->OneRam();
      mem->ClearFaults();
      CoalSetup(cfg, vec, sew_l, lmul, vl, &vf, vd, data, &old);
      const std::vector<uint8_t> pristine = mem->mem;

      RstCfg R;
      R.mode = LS_UNIT;
      R.we = (we != 0);
      R.vl = vl;
      R.sew_l = sew_l;
      R.lmul = lmul;
      R.vd = vd;
      R.data = data;
      R.base = kCoalRamBase;

      lsu->ClearReqs();
      RstAlloc(lsu, vl, 0, Vtypei(SewField(sew_l), lmul));

      LsuStim s;
      RstFillStim(&s, R, true);
      s.coalesce = (coalesce != 0);
      LsuObs o = lsu->Step(s);
      bool stopping = false;
      int guard = 0;
      while (!o.done && ++guard < 40000) {
        if (o.elems >= intr_at) stopping = true;
        LsuStim t;
        RstFillStim(&t, R, false);
        t.coalesce = (coalesce != 0);
        if (stopping) t.intr = true;
        o = lsu->Step(t);
      }
      // `done` is a pulse, so remember it before the controller drain, which
      // would otherwise overwrite the observation
      const bool completed = o.done;
      while (o.rst_busy && ++guard < 40000) {
        LsuStim t;
        RstFillStim(&t, R, false);
        t.coalesce = (coalesce != 0);
        o = lsu->Step(t);
      }
      RstRelease(lsu);

      rep->Check(completed, name + ": the macro never completed");
      rep->Check(!o.trap, name + ": a boundary stop reported a fault");
      rep->Check(o.stopped, name + ": the boundary stop was not reported");
      const int b = o.stop_elem;
      rep->Check(b > 0 && b < vl, name + ": stop_elem " + Dec(b) + " is out of range");

      // The boundary is inviolable: nothing at or above stop_elem took effect,
      // in either build. The elements strictly below it took effect -- the
      // in-flight response a boundary stop leaves outstanding is for one of
      // them and is written back (I-057's stop rule) -- so the prefix is
      // asserted for both the coalesced and the uncoalesced run. This was the
      // I-061 lane's probe while the uncoalesced path discarded that response;
      // it is a check now.
      for (int e = 0; e < vl; ++e) {
        if (e >= b) {
          if (we != 0) {
            const uint64_t addr = CoalAddr(LS_UNIT, kCoalRamBase, 0, e, 1);
            const uint8_t got = mem->mem[static_cast<size_t>(addr & 0xFFFFull)];
            const uint8_t exp = pristine[static_cast<size_t>(addr & 0xFFFFull)];
            rep->Check(got == exp, name + ": store element " + Dec(e) + " (stop_elem " +
                                       Dec(b) + ") is " + mosaic::Hex(got, 2) +
                                       " expected the untouched " + mosaic::Hex(exp, 2));
          } else {
            const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
            rep->Check(got == old[static_cast<size_t>(e)],
                       name + ": load element " + Dec(e) + " (stop_elem " + Dec(b) + ") is " +
                           mosaic::Hex(got, 16) + " expected the untouched " +
                           mosaic::Hex(old[static_cast<size_t>(e)], 16));
          }
        } else {
          if (we != 0) {
            const uint64_t addr = CoalAddr(LS_UNIT, kCoalRamBase, 0, e, 1);
            const uint8_t got = mem->mem[static_cast<size_t>(addr & 0xFFFFull)];
            const uint8_t exp = static_cast<uint8_t>(CoalVal(true, e, sew_l) & 0xFFull);
            rep->Check(got == exp, name + ": store element " + Dec(e) + " (stop_elem " +
                                       Dec(b) + ") is " + mosaic::Hex(got, 2) +
                                       " expected " + mosaic::Hex(exp, 2));
          } else {
            const uint64_t got = vec->MemRead(vd, e, sew_l, lmul);
            const uint64_t exp = mem->Elem(CoalAddr(LS_UNIT, kCoalRamBase, 0, e, 1), 1) & 0xFFull;
            rep->Check(got == exp, name + ": load element " + Dec(e) + " (stop_elem " +
                                       Dec(b) + ") is " + mosaic::Hex(got, 16) +
                                       " expected " + mosaic::Hex(exp, 16));
          }
        }
      }
      cov->cells += 1;
    }
  }
}

void RunCoalCase(Vmosaic_vec_tb* dut, ClockDriver* clk, Reporter* rep, CoalCoverage* cov) {
  Cfg cfg(dut, clk);
  Vec vec(dut, clk);
  LsuMem mem;
  Lsu lsu(dut, clk);
  lsu.BindMem(&mem);
  PhaseCoalEquivalence(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseCoalFault(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseCoalDevice(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseCoalStoreOrder(&cfg, &vec, &lsu, &mem, rep, cov);
  PhaseCoalStop(&cfg, &vec, &lsu, &mem, rep, cov);

  for (int m = 0; m < LS_MODE_COUNT; ++m) {
    if (m == LS_UNIT || m == LS_STRIDED) {
      rep->Check(cov->mode_seen[m], std::string("coverage: mode ") + LsuModeName(m) +
                                        " did not run under coalescing");
    }
  }
  rep->Check(cov->dir_seen[0] && cov->dir_seen[1],
             "coverage: both load and store directions did not run under coalescing");
  rep->Check(cov->merged_cells >= 5,
             "coverage: only " + Dec(cov->merged_cells) + " cells actually merged");
  rep->Check(cov->cells >= 30, "coverage: only " + Dec(cov->cells) + " cells ran");
}

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr,
                 "usage: %s --case <id> [--out <dir>] [--seed <n>] "
                 "[--max-cycles <n>] [--verbose]\n%s\n",
                 argv[0], error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  ClockDriver clk;
  Vmosaic_vec_tb dut;
  Dut unit(&dut, &clk, &reporter);
  VsetCounts vset_counts;
  LayoutCounts layout_counts;
  Coverage vec_cov;
  LsuCoverage lsu_cov;
  FpCov fp_cov;
  RstCoverage rst_cov;
  ChainCov chain_cov;
  CoalCoverage coal_cov;
  StopCoverage stop_cov;

  std::string detail;
  bool aborted = false;
  try {
    // Reset, then read the elaborated identity widths back from the wrapper.
    for (int i = 0; i < 4; ++i) {
      Stim r;
      r.rst = true;
      unit.Cycle(r);
    }
    const uint32_t rob_w = dut.o_rob_index_w;
    const uint32_t gen_w = dut.o_rob_gen_w;
    const uint32_t uop_w = dut.o_uop_index_w;
    if (rob_w == 0 || rob_w > 32 || gen_w == 0 || gen_w > 32 || uop_w == 0 || uop_w > 32) {
      reporter.Check(false, "geometry: the DUT reported an unusable identity width");
      return reporter.Finish("FAIL", "geometry");
    }
    unit.Masks((1ull << rob_w) - 1ull, (1ull << gen_w) - 1ull, (1ull << uop_w) - 1ull);

    if (options.case_id == "rvv.vset_boundaries") {
      RunVsetCase(&dut, &unit, &clk, &reporter, &vset_counts);
    } else if (options.case_id == "rvv.integer_mask_permute") {
      RunVecIntCase(&dut, &clk, &reporter, &vec_cov);
    } else if (options.case_id == "rvv.memory_modes") {
      RunLsuCase(&dut, &clk, &reporter, &lsu_cov);
    } else if (options.case_id == "rvv.fp_flags_reduction") {
      RunVecFpCase(&dut, &clk, &reporter, &fp_cov);
    } else if (options.case_id == "rvv.partial_fault_restart") {
      RunVecRestartCase(&dut, &clk, &reporter, &rst_cov);
    } else if (options.case_id == "rvv.stop_path_inflight") {
      RunStopPathCase(&dut, &clk, &reporter, &stop_cov);
    } else if (options.case_id == "rvv.chaining_hazards") {
      RunVecChainCase(&dut, &clk, &reporter, &chain_cov);
    } else if (options.case_id == "rvv.vtype_layout") {
      RunVtypeLayoutCase(&dut, &unit, &clk, &reporter, &layout_counts);
    } else if (options.case_id == "rvv.mask_prefix_vstart") {
      RunMaskPrefixVstartCase(&dut, &clk, &reporter);
    } else if (options.case_id == "rvv.mask_prefix_semantics") {
      RunMaskPrefixSemanticsCase(&dut, &clk, &reporter);
    } else if (options.case_id == "rvv.mask_prefix_masked") {
      RunMaskPrefixMaskedCase(&dut, &clk, &reporter);
    } else if (options.case_id == "coalesce.element_faults") {
      RunCoalCase(&dut, &clk, &reporter, &coal_cov);
    } else {
      int reason_hist[RSN_COUNT] = {0};
      bool class_seen[VOP_COUNT] = {false};

      PhaseGeometry(&unit, &reporter);
      PhaseVtypeMatrix(&unit, &reporter, reason_hist);
      PhaseOpMatrix(&unit, &reporter, reason_hist, class_seen);
      PhaseLaneInvariance(&unit, &reporter);
      PhaseReasonCoverage(&unit, &reporter, reason_hist, class_seen);
      PhaseDescriptorProgress(&unit, &reporter);
      PhaseFaultProgress(&unit, &reporter);
      PhaseResetInFlight(&unit, &reporter);
    }
  } catch (const std::exception& e) {
    aborted = true;
    detail = e.what();
  }

  const int cycles = static_cast<int>(clk.cycle());
  if (aborted) {
    return reporter.Finish("FAIL", "aborted: " + detail);
  }
  if (reporter.failures() != 0) {
    return reporter.Finish("FAIL", "checks=" + Dec(reporter.checks()) + " cycles=" +
                                         Dec(cycles));
  }
  if (options.case_id == "rvv.vset_boundaries") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " vtypes=" +
                                       Dec(vset_counts.vtypes) + " avl_bands=" +
                                       Dec(vset_counts.avl_bands) + " cycles=" +
                                       Dec(cycles));
  }
  if (options.case_id == "rvv.vtype_layout") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " words=" +
                                       Dec(layout_counts.words) + " descriptors=" +
                                       Dec(layout_counts.descriptors) + " cycles=" +
                                       Dec(cycles));
  }
  if (options.case_id == "rvv.integer_mask_permute") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " cells=" +
                                       Dec(vec_cov.cells) + " families=" + Dec(VF_COUNT) +
                                       " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.fp_flags_reduction") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " families=" +
                                       Dec(FF_COUNT) + " cells=" + Dec(fp_cov.cells) +
                                       " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.memory_modes") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " modes=" +
                                       Dec(lsu_cov.modes) + " cells=" + Dec(lsu_cov.cells) +
                                       " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.partial_fault_restart") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " cells=" +
                                       Dec(rst_cov.cells) + " interrupts=" +
                                       Dec(rst_cov.intr_cells) + " fof=" + Dec(rst_cov.fof_cells) +
                                       " restarts=" + Dec(rst_cov.restart_cells) +
                                       " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.stop_path_inflight") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " kinds=" +
                                       Dec(SK_KIND_COUNT) + " cells=" +
                                       Dec(stop_cov.cells[SK_FAULT] + stop_cov.cells[SK_WHOLE] +
                                           stop_cov.cells[SK_REDIRECT] + stop_cov.cells[SK_DRAIN]) +
                                       " resumes=" + Dec(stop_cov.resume_cells) + " inflight=" +
                                       Dec(stop_cov.inflight_cells) + " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.chaining_hazards") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " orders=" +
                                       Dec(chain_cov.order_cells) + " cycles_on=" +
                                       Dec(chain_cov.cycles_on) + " cycles_off=" +
                                       Dec(chain_cov.cycles_off) + " fwd_on=" +
                                       Dec(chain_cov.fwd_on) + " fwd_off=" +
                                       Dec(chain_cov.fwd_off) + " stall_on=" +
                                       Dec(chain_cov.stall_on) + " stall_off=" +
                                       Dec(chain_cov.stall_off) + " pkt_on=" +
                                       Dec(chain_cov.pkt_on) + " pkt_off=" +
                                       Dec(chain_cov.pkt_off) + " pre_on=" +
                                       Dec(chain_cov.pre_on) + " pre_off=" +
                                       Dec(chain_cov.pre_off) + " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.mask_prefix_vstart") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " ops=3 vstart_cells=6"
                                       " cycles=" + Dec(cycles));
  }
  if (options.case_id == "coalesce.element_faults") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " cells=" +
                                       Dec(coal_cov.cells) + " merged=" +
                                       Dec(coal_cov.merged_cells) + " req_on=" +
                                       Dec(coal_cov.req_coal) + " req_off=" +
                                       Dec(coal_cov.req_unco) + " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.mask_prefix_semantics") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " ops=3"
                                       " positions=16 allzero=3 anchors=9 cycles=" +
                                       Dec(cycles));
  }
  if (options.case_id == "rvv.mask_prefix_masked") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " ops=3"
                                       " positions=16 masked_before=45 masked_only=48"
                                       " vma=12 vta=12 anchors=3 cycles=" +
                                       Dec(cycles));
  }
  return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " combos=" +
                                     Dec(VOP_COUNT * 64 * 4 * 2 + 64 + 4) +
                                     " cycles=" + Dec(cycles));
}
