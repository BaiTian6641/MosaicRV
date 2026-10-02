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
  // Ratified v1.0 positions: vlmul[2:0], vsew[5:3], vill[63].
  uint64_t v = (static_cast<uint64_t>(vsew & 7) << 3) | static_cast<uint64_t>(vlmul & 7);
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
  const int e = 7 + LmulExp(vlmul) - vsew;
  if (e < 0 || e > 7) return 0;
  return 1 << e;
}

bool WordSupported(uint64_t v) {
  if (((v >> 63) & 1ull) != 0) return false;
  if (((v >> 8) & ((1ull << 55) - 1ull)) != 0ull) return false;
  const int vsew = static_cast<int>((v >> 3) & 7ull);
  const int vlmul = static_cast<int>(v & 7ull);
  return VsewValid(vsew) && VlmulValid(vlmul) && (LmulExp(vlmul) + 6 >= vsew);
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
                             Vtypei(3, 0) | (1ull << bit));
    rep->Check(r.vill && r.vtype == kVill,
               "vtype-support: reserved vtypei bit " + Dec(bit) + " did not set vill");
  }

  // A full vtype word from vsetvl with a reserved bit set is unsupported too.
  const CfgObs r = RunVset(cfg, VSETVL, 5, 6, kAvlMax, Vtypei(3, 0) | (1ull << 40));
  rep->Check(r.vill && r.vtype == kVill, "vtype-support: reserved vtype bit 40 did not set vill");
}

// The AVL bands, on several distinct configurations so the check cannot pass by
// hard-coding one VLMAX. The shipped policy is vl = min(AVL, VLMAX); the driver
// asserts the spec bounds and determinism, not that exact value.
void PhaseAvlBands(Cfg* cfg, Reporter* rep, VsetCounts* counts) {
  const int configs[4][2] = {{3, 0}, {3, 3}, {6, 3}, {5, 1}};
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
  const uint64_t word = Vtypei(3, 0);  // e8, m1 -> VLMAX 16
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
  o = RunVset(cfg, VSETVLI, 0, 0, 0, Vtypei(5, 0));  // e32,m1 -> VLMAX 4
  rep->Check(o.vill && o.vl == 0, "rd-rs1: the reserved x0/x0 form sets vill");

  // vsetivli: the AVL is the zero-extended 5-bit immediate.
  o = RunVset(cfg, VSETIVLI, 3, 0, 0, word, /*uimm*/ 5);
  rep->Check(o.vl == 5 && o.vset_rd_we && o.vset_rd_val == 5, "rd-rs1: vsetivli uimm=5");
  o = RunVset(cfg, VSETIVLI, 3, 0, 0, word, /*uimm*/ 31);
  rep->Check(o.vl == vlmax, "rd-rs1: vsetivli uimm=31 clamps to VLMAX");

  // vsetvl takes the vtype from rs2.
  o = RunVset(cfg, VSETVL, 4, 7, 9, Vtypei(4, 1));
  rep->Check(!o.vill && o.vtype == Vtypei(4, 1) && o.vl == 9 && o.vset_rd_val == 9,
             "rd-rs1: vsetvl uses rs2 as vtype and x[rs1] as AVL");
}

// vstart is reset to zero by every committed vector instruction, is writable
// through its CSR, and is *not* modified by the illegal-instruction path.
void PhaseVstart(Cfg* cfg, Reporter* rep) {
  CfgObs o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(3, 0));
  rep->Check(o.vstart == 0, "vstart: a committed vset resets vstart");

  o = CsrWrite(cfg, kCsrVstart, 5);
  rep->Check(!o.csr_illegal && o.csr_commit, "vstart: a software write commits");
  o = CsrRead(cfg, kCsrVstart);
  rep->Check(o.csr_rdata == 5, "vstart: value read back as " + Dec(o.csr_rdata));

  o = CsrWrite(cfg, kCsrVstart, kAvlMax);
  o = CsrRead(cfg, kCsrVstart);
  rep->Check(o.csr_rdata == 0x7F, "vstart: upper bits are not writable (" + mosaic::Hex(o.csr_rdata) + ")");

  o = CsrWrite(cfg, kCsrVstart, 5);
  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(3, 0), 0, /*vs_off*/ true);
  rep->Check(o.vset_illegal && !o.vset_commit, "vstart: VS=Off raises illegal instruction");
  o = CsrRead(cfg, kCsrVstart);
  rep->Check(o.csr_rdata == 5, "vstart: an illegal instruction does not modify vstart");

  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(3, 0));
  rep->Check(o.vstart == 0, "vstart: a following committed vset resets vstart");

  o = CsrWrite(cfg, kCsrVstart, 9);
  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(0, 0));  // unsupported -> vill
  rep->Check(o.vill && o.vstart == 0, "vstart: an unsupported vtype still resets vstart");
}

// The permissions and the field layout of the seven unprivileged vector CSRs.
void PhaseCsrPermissions(Cfg* cfg, Reporter* rep) {
  // Configure a known state: e32,m1 with vl = 3.
  RunVset(cfg, VSETVLI, 5, 6, 3, Vtypei(5, 0));

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
  rep->Check(o.csr_rdata == Vtypei(5, 0), "csr: vtype reads the configured type");

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

  CfgObs o = RunVset(cfg, VSETVLI, 5, 6, 7, Vtypei(3, 0));
  const uint64_t v1 = o.vtype;
  const uint64_t vl1 = o.vl;
  const int g1 = o.gen;
  rep->Check(!o.vill && v1 == Vtypei(3, 0) && vl1 == 7, "snapshot: V1 configured");

  CfgStim cap;
  cap.snap_capture = true;
  const CfgObs c1 = cfg->Cycle(cap);
  rep->Check(c1.snap_valid && c1.snap_vtype == v1 && c1.snap_vl == vl1 &&
                 c1.snap_vstart == 0 && c1.snap_gen == g1,
             "snapshot: the snapshot is not the configuration in force [v=" +
                 mosaic::Hex(c1.snap_vtype) + " vl=" + Dec(c1.snap_vl) + " vs=" +
                 Dec(c1.snap_vstart) + " gen=" + Dec(c1.snap_gen) + "]");

  o = RunVset(cfg, VSETVLI, 5, 6, 3, Vtypei(5, 0));
  rep->Check(!o.vill && o.vtype == Vtypei(5, 0) && o.vl == 3, "snapshot: V2 configured");
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
  CfgObs o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(3, 0));
  rep->Check(!o.vill, "vill-blocks: a supported vtype clears vill");
  CfgStim e;
  e.exec_valid = true;
  e.exec_vtype_dep = true;
  CfgObs x = cfg->Cycle(e);
  rep->Check(!x.exec_illegal, "vill-blocks: a vtype-dependent instruction blocked while vill is clear");

  o = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(0, 0));  // SEW=1, unsupported
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
  for (int vsew = 3; vsew <= 6; ++vsew) {
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
  for (int vsew = 3; vsew <= 6; ++vsew) {
    for (int vlmul = 0; vlmul < 8; ++vlmul) {
      if (!VlmulValid(vlmul) || (LmulExp(vlmul) + 6 < vsew)) continue;
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
  for (int vsew = 3; vsew <= 6; ++vsew) {
    for (int vlmul = 0; vlmul < 8; ++vlmul) {
      if (!VlmulValid(vlmul) || (LmulExp(vlmul) + 6 < vsew)) continue;
      for (int ta = 0; ta < 2; ++ta) {
        for (int ma = 0; ma < 2; ++ma) {
          const uint64_t word = Vtypei(vsew, vlmul, ta, ma);
          // opivv with disjoint register groups: legality depends only on vtype.
          const QueryObs q = desc->Query(word, VOP_IVV, 5, 6, 7, 8, false, 4);
          const std::string name = "vtype-layout descriptor vsew=" + Dec(vsew) +
                                   " vlmul=" + Dec(vlmul) + " ta=" + Dec(ta) +
                                   " ma=" + Dec(ma);
          rep->Check(q.vtype_legal, name + ": a legal configuration was rejected");
          rep->Check(q.sew_log2 == vsew, name + ": sew_log2 decoded from the wrong bits");
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
  // SEW = 1 (vsew = 0) is not supported: vill set, vtype[62:0] zero, vl = 0.
  const CfgObs bad = RunVset(cfg, VSETVLI, 5, 6, kAvlMax, Vtypei(0, 0, 1, 1));
  rep->Check(bad.vill, "vtype-layout vill: an unsupported vtype did not set vill");
  rep->Check(bad.vtype == kVill,
             "vtype-layout vill: vtype[62:0] not zeroed (" + mosaic::Hex(bad.vtype) + ")");
  rep->Check(bad.vl == 0, "vtype-layout vill: an unsupported vtype left vl != 0");

  // A reserved bit of the vtype argument makes the value unsupported: "all bits
  // of the vtype argument must be considered".
  for (int bit : {8, 30, 62}) {
    const CfgObs r = RunVset(cfg, VSETVL, 5, 6, kAvlMax, Vtypei(3, 0) | (1ull << bit));
    rep->Check(r.vill && r.vtype == kVill,
               "vtype-layout reserved: vsetvl vtype bit " + Dec(bit) + " did not set vill");
  }

  // URO: a software CSR write to vtype is illegal and the register is unchanged.
  const CfgObs good = RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(5, 1, 1, 0));
  const uint64_t held = good.vtype;
  rep->Check(!good.vill && held == Vtypei(5, 1, 1, 0), "vtype-layout uro: V1 not configured");
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
  RunVset(cfg, VSETVLI, 5, 6, 4, Vtypei(0, 0));
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
    } else if (options.case_id == "rvv.vtype_layout") {
      RunVtypeLayoutCase(&dut, &unit, &clk, &reporter, &layout_counts);
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
  return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " combos=" +
                                     Dec(VOP_COUNT * 64 * 4 * 2 + 64 + 4) +
                                     " cycles=" + Dec(cycles));
}
