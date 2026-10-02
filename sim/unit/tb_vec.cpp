// ============================================================================
// tb_vec.cpp -- CASE=rvv.descriptor_legality, work package I-051.
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

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
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

uint64_t Vtype(int vsew, int vlmul, bool vill) {
  uint64_t v = (static_cast<uint64_t>(vsew & 7) << 5) | static_cast<uint64_t>(vlmul & 7);
  if (vill) v |= (1ull << 63);
  return v;
}

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

bool VsewValid(int vsew) { return vsew >= 3 && vsew <= 6; }
bool VlmulValid(int vlmul) { return vlmul != 4; }

int VtypeReason(int vsew, int vlmul, bool vill) {
  if (vill) return RSN_VTYPE_UNSUPP;
  if (!VsewValid(vsew)) return RSN_RESERVED_VSEW;
  if (!VlmulValid(vlmul)) return RSN_RESERVED_VLMUL;
  if (LmulExp(vlmul) + 6 < vsew) return RSN_EMUL_RANGE;
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
  int sew_l = vsew;
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
  int e = 7 + LmulExp(vlmul) - vsew;  // VLEN=128 -> log2 = 7
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
      : d_(d), clk_(clk), rep_(rep) {}

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
  const QueryObs q = d->Query(Vtype(5, 0, false), VOP_IVV, 1, 2, 3, 4, false, 2);
  rep->Check(q.vlenb == 16, "geometry: vlenb read back as " + Dec(q.vlenb) + ", expected 16");
  rep->Check(q.class_count == VOP_COUNT,
             "geometry: class count " + Dec(q.class_count) + ", expected " + Dec(VOP_COUNT));
  rep->Check(q.class_count == 18, "geometry: the declared operation list is 18 families");

  // The effective-width arithmetic that the matrix is built on, stated as
  // direct examples: a widening operation doubles both EEW and EMUL, a
  // narrowing source doubles its EMUL.
  const QueryObs wide = d->Query(Vtype(3, 0, false), VOP_VWIDE, 1, 2, 3, 4, false, 2);
  rep->Check(wide.cfg_legal && wide.emul_dst_exp == 1 && wide.emul_src_exp == 0,
             "geometry: vwadd at LMUL=1 has destination EMUL 2 and source EMUL 1");
  const QueryObs narrow = d->Query(Vtype(3, 0, false), VOP_VNARROW, 1, 2, 3, 4, false, 2);
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
        rep->Check(q.sew_log2 == vsew, name + ": sew_log2");
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
  const uint64_t vt = Vtype(3, 0, false);  // SEW=8, LMUL=1
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
  s.alloc_vtype = Vtype(5, 0, false);  // SEW=32, LMUL=1 -> 4 elements
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
  busy.alloc_vtype = Vtype(6, 3, false);
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
  reuse.alloc_vtype = Vtype(4, 1, false);
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
  s.alloc_vtype = Vtype(3, 3, false);  // SEW=8, LMUL=8 -> 128 elements
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
  s.alloc_vtype = Vtype(5, 0, false);
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
  for (int i = 0; i < 2; ++i) {
    Stim r;
    r.rst = true;
    r.alloc_valid = true;
    r.alloc_vtype = Vtype(6, 3, false);
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
  a.alloc_vtype = Vtype(5, 0, false);
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
  const QueryObs vill = d->Query(Vtype(3, 0, true), VOP_IVV, 1, 2, 3, 4, false, 2);
  ++reason_hist[vill.reason];
  rep->Check(vill.reason == RSN_VTYPE_UNSUPP,
             "reason-coverage: vill reports VTYPE_UNSUPPORTED");
  const QueryObs unknown = d->Query(Vtype(3, 0, false), VOP_COUNT, 1, 2, 3, 4, false, 2);
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

}  // namespace

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
  return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " combos=" +
                                     Dec(VOP_COUNT * 64 * 4 * 2 + 64 + 4) +
                                     " cycles=" + Dec(cycles));
}
