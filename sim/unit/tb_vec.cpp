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
      bool bb = ((vs2 >> (index & 7)) & 1u) != 0;
      bool aa = ((vs1 >> (index & 7)) & 1u) != 0;
      switch (op) {
        case 0: o.mres = bb && aa; break;
        case 1: o.mres = !(bb && aa); break;
        case 2: o.mres = bb || aa; break;
        case 3: o.mres = !(bb || aa); break;
        case 4: o.mres = bb != aa; break;
        case 5: o.mres = !(bb != aa); break;
        case 6: o.mres = bb && !aa; break;
        default: o.mres = bb || !aa; break;
      }
      break;
    }
    case VF_MASKPFX: {
      bool bb = ((vs2 >> (index & 7)) & 1u) != 0;
      if (op == 0) o.mres = !pfx_in;                 // vmsbf
      else if (op == 1) o.mres = !pfx_in;            // vmsif
      else o.mres = bb && !pfx_in;                   // vmsof
      o.pfx = pfx_in || bb;
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
      ConfigureVec(cfg, sew_l, 0, 0, 0, 64);
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
    ConfigureVec(cfg, sew_l, 0, 0, 0, 64);
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
      ConfigureVec(cfg, sew_l, VlmulOfExp(lmul_e), 0, 0, vlmax);
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
    ConfigureVec(cfg, sew_l, 0, 0, 0, 64);
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
  ConfigureVec(cfg, 3, 0, 0, 0, 4);
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
        ConfigureVec(cfg, sew_l, VlmulOfExp(lmul_e), 0, 0, static_cast<uint64_t>(vl));
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
    ConfigureVec(cfg, 3, 0, 0, vma, 8);
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
  ConfigureVec(cfg, 3, 0, 0, 0, 4);
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
  ConfigureVec(cfg, 3, 0, 0, 0, 8);
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
  int req_count = 0;
  int device_reqs = 0;
  int ram_reqs = 0;

  LsuMem() : mem(static_cast<size_t>(kSize)) {
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

  void ResetCounters() { req_count = 0; device_reqs = 0; ram_reqs = 0; }

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
};

struct LsuObs {
  bool busy = false, done = false, illegal = false, trap = false;
  int trap_elem = 0, elems = 0;
  uint32_t req_ctr = 0;
  bool req_valid = false;
  int req_elem = 0, req_field = 0;
  uint64_t req_addr = 0;
  uint8_t req_mask = 0;
  uint64_t req_wdata = 0;
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
    if (!pending_.empty() && pending_.front().age >= mem_->latency) {
      const Pending& p = pending_.front();
      rsp = true;
      re = p.elem;
      rf = p.field;
      rdata = mem_->Beat(p.addr);
      rfault = mem_->fault_enable &&
               (mem_->fault_all || ((re == mem_->fault_elem) && (rf == mem_->fault_field)));
      if (!rfault && p.we) mem_->Apply(p.addr, p.mask, p.wdata);
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
    d_->lsu_mem_req_ready_i = s.mem_ready ? 1 : 0;
    d_->lsu_mem_rsp_valid_i = rsp ? 1 : 0;
    d_->lsu_mem_rsp_elem_i = static_cast<uint8_t>(re & 0x7F);
    d_->lsu_mem_rsp_field_i = static_cast<uint8_t>(rf & 0xF);
    d_->lsu_mem_rsp_fault_i = rfault ? 1 : 0;
    d_->lsu_mem_rsp_rdata_i = rdata;

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
    o.req_valid = pre_req;
    o.req_elem = pre_elem;
    o.req_field = pre_field;
    o.req_addr = pre_addr;
    o.req_mask = pre_mask;
    o.req_wdata = pre_wdata;

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

    clk_->Tick();
    return o;
  }

  // launch one macro and run it to completion; returns the final observation
  LsuObs Run(int mode, bool we, bool ordered, int nf, int vd, int data, int index,
             int idx_sew, uint64_t base, uint64_t stride, bool mask_en,
             int guard_cycles = 40000, uint8_t caps = 0xFF) {
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
    LsuObs o = Step(s);

    LsuStim idle;
    idle.caps = caps;
    idle.mem_ready = true;
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
  (void)RunVset(cfg, VSETVLI, 5, 6, avl, Vtypei(sew_l, vlmul, vta, vma));
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
  if (options.case_id == "rvv.integer_mask_permute") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " cells=" +
                                       Dec(vec_cov.cells) + " families=" + Dec(VF_COUNT) +
                                       " cycles=" + Dec(cycles));
  }
  if (options.case_id == "rvv.memory_modes") {
    return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " modes=" +
                                       Dec(lsu_cov.modes) + " cells=" + Dec(lsu_cov.cells) +
                                       " cycles=" + Dec(cycles));
  }
  return reporter.Finish("PASS", "checks=" + Dec(reporter.checks()) + " combos=" +
                                     Dec(VOP_COUNT * 64 * 4 * 2 + 64 + 4) +
                                     " cycles=" + Dec(cycles));
}
