// ============================================================================
// mosaic_vec_desc -- the frozen RVV descriptor and legality matrix (I-051).
//
// ------------------------------------------------------------------- why
//
// A vector instruction is one architectural instruction that touches a *group*
// of registers and a *run* of elements. Two mistakes follow from modelling it
// as anything else, and both are named fail modes of this package:
//
//   * expanding every element into its own ROB entry. One vector instruction is
//     one macro identity, one trap origin and one retirement (docs/
//     architecture-review.md line 73). A design that spends ROB capacity per
//     element runs out of window on the first real loop and can no longer name
//     the instruction a fault belongs to.
//   * keeping only a `done` bit. The architectural state of an interrupted
//     vector instruction is not all-or-nothing: the V spec restarts at
//     `vstart`, so the *contiguous committed prefix* and the *earliest fault*
//     are separate facts (architecture-review.md line 249: "descriptor 保存配置
//     快照、mask 版本、source group lifetime、old destination、element/segment
//     progress、最早故障和 partial flags ... 元素完成集合与 restart prefix 分开").
//     This module therefore keeps the element bitmap (which elements completed)
//     and the fault progress (which element faulted first) as separate outputs.
//
// So this file is the frozen *profile* -- VLEN, ELEN, the declared operation
// classes and, for every one of them, the legal EEW/EMUL/LMUL/SEW/overlap/vtype
// combinations -- plus the descriptor storage that records macro identity,
// element progress and fault progress for one in-flight vector macro.
//
// It is deliberately a *module-level* package like the caches and the fabric:
// it does not wire itself into `mosaic_lsu_endpoint` or `mosaic_core`. The
// integration is separate work; what is frozen here is the contract the later
// vector units (I-052..I-059) consume.
//
// --------------------------------------------------------------- the profile
//
// VLEN = 128, ELEN = 64. VLEN is the width of one vector register in bits and
// is *fixed for the life of a hart* (config/profiles/p2.json derived_notes:
// "VLEN is fixed for the whole life of a hart; runtime lane quota changes
// (I-059) must never change architectural VLEN"). The `lane_count_i` input
// exists precisely so the case can prove that: it is echoed on
// `o_lane_count_o` and is used for nothing else. `o_vlen_o`/`o_vlenb_o` are
// constants and a lane-count sweep leaves them unchanged.
//
// ------------------------------------------------------- the legality rules
//
// The V spec (v1.0, tag 3570f998, src/v-spec.adoc) fixes the pieces:
//
//   * vsew[2:0] in vtype selects SEW = 2^vsew. Implementations support a
//     contiguous range; this profile supports SEW 8/16/32/64, i.e. vsew 3..6.
//     vsew 0/1/2/7 are not supported (L309-L314, L326-L327).
//   * vlmul[2:0] selects LMUL: 000=1, 001=2, 010=4, 011=8, 101=1/8, 110=1/4,
//     111=1/2; encoding 100 is reserved (L348-L361). All seven non-reserved
//     settings are supported, which satisfies the mandatory fractional-LMUL
//     floor for ELEN=64 ("fractional LMULs of 1/2, 1/4, and 1/8 must be
//     supported", L305-L314).
//   * a supported SEW/LMUL pair must satisfy SEW <= LMUL*ELEN, i.e. the
//     effective EMUL is at least 1. "For a given supported fractional LMUL
//     setting, implementations must support SEW settings between SEW_MIN and
//     LMUL * ELEN, inclusive" (L323-L324); "The use of vtype encodings with
//     LMUL < SEW_MIN/ELEN is reserved, but implementations can set vill if they
//     do not support these configurations" (L329-L331). This profile sets vill
//     for them.
//     At ELEN=64 the legal vtype count is 7+6+5+4 = 22 of the 64 vsew/vlmul
//     combinations: SEW=8 admits all seven LMULs, SEW=16 excludes 1/8, SEW=32
//     excludes 1/8 and 1/4, SEW=64 excludes 1/8, 1/4 and 1/2.
//
// On top of the *config* rule sit the *operation* rules. Every vector operation
// declares a source effective element width (EEW) and an effective LMUL (EMUL);
// the destination, source and mask register groups must not overlap in a way
// the operation forbids (L~"Vector Register Grouping": "The destination vector
// register group ... cannot overlap a source vector register group of a
// different size" for widening/narrowing; reductions write vd[0] and may not
// overlap their source; "The mask register v0 cannot be the destination of a
// masked instruction"). The class table below states each rule once, and
// `o_reason_o` names the first rule a combination breaks, so every combination
// has a definite outcome rather than a don't-care.
//
// ------------------------------------------------------- descriptor contract
//
// ONE descriptor exists (the baseline is one in-flight vector macro per hart;
// multi-descriptor chaining is I-058). It holds:
//   * macro identity: rob_index + rob_gen + uop_index, the same identity
//     (`mosaic_id_pkg::macro_id_t`) every other unit carries, so a late element
//     completion can be matched to the macro that issued it;
//   * the configuration snapshot (vtype, vl, vstart, vd, mask source version);
//   * the element bitmap: bit i set means element i produced its architectural
//     update;
//   * the contiguous committed prefix (`o_prefix_o`): the count of consecutive
//     set bits from element 0, which is the restart point after a fault;
//   * fault progress: the earliest faulting element and its fault class, frozen
//     once seen (`o_accepting_elems_o` drops, so younger elements cannot commit
//     past a fault).
//
// Reset has a defined effect: it clears the descriptor and its counters, and an
// allocate or element-done pulse presented *while* reset is asserted is
// ignored. That is the "transaction in flight at reset" rule for this package:
// an outstanding vector macro is squashed, publishes no architectural effect,
// and leaves no partial bitmap behind.
//
// ------------------------------------------------------------------- deferred
//
// The matrix above freezes the *shape* of the declared V operation list: one
// class per (source EEW, destination EEW, EMUL multiplier, overlap) shape. Four
// groups of the RVV 1.0 listing are deliberately NOT rows of it, because their
// legality needs an operand the descriptor does not take yet, and a row without
// that operand would be a guess:
//
//   * the 4:1 and 8:1 extension ratios (`vzext.vf4/vf8`, `vsext.vf4/vf8`);
//   * segment accesses (`vlseg..`, `vsseg..`), whose legal EMUL is LMUL*nf and
//     therefore needs the `nf` field (I-056's concern);
//   * indexed accesses (`vluxei..`, `vsuxei..`), whose destination/index group
//     overlap rule needs the index group named (I-056);
//   * fractional-LMUL element/segment field progress (I-057).
//
// Until those rows exist they are deferred and cannot be advertised: the
// capability gate must not claim them. That is the "unimplemented row is
// explicitly deferred" rule of the work package, and it is recorded in
// results/reports/I-051-rvv-descriptor.md under "not covered".
//
// ---------------------------------------------------------- negative control
//
// Three `ifdef` mutants live here, one per fail mode the case's controls
// inject: MOSAIC_VEC_MUTANT_ILLEGAL_ACCEPTED (a combination the matrix calls
// illegal is accepted), MOSAIC_VEC_MUTANT_LOSE_PROGRESS (an element completion
// is dropped from the bitmap) and MOSAIC_VEC_MUTANT_LANE_VLEN (the lane quota
// leaks into the architectural VLEN). Each is off unless its -D is passed and
// is proven to fail CASE=rvv.descriptor_legality; see
// results/reports/I-051-rvv-descriptor.md.
// ============================================================================

`default_nettype none
`resetall

module mosaic_vec_desc #(
    parameter int unsigned VLEN = 128,
    parameter int unsigned ELEN = 64,
    parameter int unsigned ROB_INDEX_W = 6,
    parameter int unsigned ROB_GEN_W   = 7,
    parameter int unsigned UOP_INDEX_W = 3
) (
    input  logic                       clk_i,
    input  logic                       rst_i,

    // ---- legality query (combinational) ------------------------------------
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [63:0]                vtype_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [4:0]                 op_class_i,
    input  logic [4:0]                 vd_i,
    input  logic [4:0]                 vs1_i,
    input  logic [4:0]                 vs2_i,
    input  logic [4:0]                 vs3_i,
    input  logic                       mask_en_i,
    input  logic [3:0]                 lane_count_i,

    output logic                       o_vtype_legal_o,
    output logic                       o_cfg_legal_o,
    output logic                       o_illegal_o,
    output logic [3:0]                 o_reason_o,
    output logic [2:0]                 o_sew_log2_o,
    output logic signed [3:0]          o_lmul_exp_o,
    output logic signed [3:0]          o_emul_src_exp_o,
    output logic signed [3:0]          o_emul_dst_exp_o,
    output logic [7:0]                 o_elem_count_o,
    output logic [7:0]                 o_vlen_o,
    output logic [7:0]                 o_vlenb_o,
    output logic [3:0]                 o_lane_count_o,
    output logic [7:0]                 o_class_count_o,

    // ---- descriptor allocate ----------------------------------------------
    input  logic                       alloc_valid_i,
    output logic                       alloc_ready_o,
    input  logic [63:0]                alloc_vtype_i,
    input  logic [7:0]                 alloc_vl_i,
    input  logic [6:0]                 alloc_vstart_i,
    input  logic [4:0]                 alloc_vd_i,
    input  logic [3:0]                 alloc_mask_ver_i,
    input  logic [ROB_INDEX_W-1:0]     alloc_rob_index_i,
    input  logic [ROB_GEN_W-1:0]       alloc_rob_gen_i,
    input  logic [UOP_INDEX_W-1:0]     alloc_uop_index_i,

    // ---- element progress --------------------------------------------------
    input  logic                       elem_done_valid_i,
    input  logic [6:0]                 elem_done_index_i,

    // ---- fault progress ----------------------------------------------------
    input  logic                       fault_valid_i,
    input  logic [6:0]                 fault_elem_i,
    input  logic [3:0]                 fault_code_i,

    // ---- release / retire --------------------------------------------------
    input  logic                       release_i,

    // ---- descriptor state --------------------------------------------------
    output logic                       o_valid_o,
    output logic [ROB_INDEX_W-1:0]     o_macro_rob_index_o,
    output logic [ROB_GEN_W-1:0]       o_macro_rob_gen_o,
    output logic [UOP_INDEX_W-1:0]     o_macro_uop_index_o,
    output logic [63:0]                o_vtype_o,
    output logic [7:0]                 o_vl_o,
    output logic [6:0]                 o_vstart_o,
    output logic [4:0]                 o_vd_o,
    output logic [3:0]                 o_mask_ver_o,
    output logic [VLEN-1:0]            o_elem_bitmap_o,
    output logic [7:0]                 o_prefix_o,
    output logic [7:0]                 o_elems_done_ctr_o,
    output logic                       o_fault_valid_o,
    output logic [6:0]                 o_fault_elem_o,
    output logic [3:0]                 o_fault_code_o,
    output logic                       o_accepting_elems_o,
    output logic [7:0]                 o_rob_entries_used_o,
    output logic [15:0]                o_alloc_ctr_o,
    output logic [15:0]                o_release_ctr_o
);

  // -------------------------------------------------------- operation classes
  // The declared vector operation list, as families. The numbering is the
  // contract between this module and sim/unit/tb_vec.cpp, which mirrors it; the
  // case drives every family, so a renumbering here fails there rather than
  // silently moving a rule to a different family.
  localparam logic [4:0] VOP_IVV      = 5'd0;   // opivv/opivx/opivi integer arithmetic
  localparam logic [4:0] VOP_VMUL     = 5'd1;   // integer multiply / multiply-accumulate
  localparam logic [4:0] VOP_VDIV     = 5'd2;   // integer divide / remainder
  localparam logic [4:0] VOP_VWIDE    = 5'd3;   // widening integer (vwadd, vwmul, vwmacc)
  localparam logic [4:0] VOP_VNARROW  = 5'd4;   // narrowing integer (vnclip, vnsrl, vnsra)
  localparam logic [4:0] VOP_VEXT     = 5'd5;   // integer extension (vzext, vsext)
  localparam logic [4:0] VOP_VRED     = 5'd6;   // integer reduction (vredsum, ...)
  localparam logic [4:0] VOP_VMASK    = 5'd7;   // mask-producing compares (vmseq, vmslt, ...)
  localparam logic [4:0] VOP_VMASKMV  = 5'd8;   // mask-to-mask (vmsbf, vmsif, vmsof)
  localparam logic [4:0] VOP_VSLIDE   = 5'd9;   // slide / permute / gather
  localparam logic [4:0] VOP_VFP      = 5'd10;  // vector FP arithmetic (SEW >= 16)
  localparam logic [4:0] VOP_VFPWIDE  = 5'd11;  // widening vector FP (vfwadd, ...)
  localparam logic [4:0] VOP_VFPRED   = 5'd12;  // vector FP reduction
  localparam logic [4:0] VOP_VLOAD    = 5'd13;  // vector load (unit/strided/indexed)
  localparam logic [4:0] VOP_VSTORE   = 5'd14;  // vector store (no destination group)
  localparam logic [4:0] VOP_VWHOLE   = 5'd15;  // whole-register load/store/move (vtype-free)
  localparam logic [4:0] VOP_VSET     = 5'd16;  // vset{i}vl{i} (vtype-free, defines vtype)
  localparam logic [4:0] VOP_VSPECIAL = 5'd17;  // vmv.x.s / vmv.s.x / scalar moves
  localparam logic [4:0] VOP_COUNT    = 5'd18;

  // log2 of the widest element width. ELEN=64 gives 6, the exponent at which
  // 'effective EMUL >= 1' and 'effective EEW <= ELEN' are expressed.
  localparam int unsigned ELEN_LOG2 = $clog2(ELEN);

  // Legality reason codes. 0 is OK; the rest name the *first* rule a
  // combination breaks, so an illegal combination has a definite cause.
  localparam logic [3:0] RSN_OK              = 4'd0;
  localparam logic [3:0] RSN_VTYPE_UNSUPP    = 4'd1;  // vill set: the vtype argument is unsupported
  localparam logic [3:0] RSN_EMUL_RANGE      = 4'd2;  // effective LMUL outside [1/8, 8]
  localparam logic [3:0] RSN_EEW_RANGE       = 4'd3;  // effective element width outside [8, ELEN]
  localparam logic [3:0] RSN_OVERLAP_SRC     = 4'd4;  // destination/source group overlap forbidden
  localparam logic [3:0] RSN_MASK_DST_OVER   = 4'd5;  // masked destination group includes v0
  localparam logic [3:0] RSN_RESERVED_VLMUL  = 4'd6;  // vlmul == 100
  localparam logic [3:0] RSN_RESERVED_VSEW   = 4'd7;  // vsew outside 3..6
  localparam logic [3:0] RSN_CLASS_INVALID   = 4'd8;  // op_class names no family

  // Class descriptor fields, decoded from op_class_i.
  //   dst_kind   0 = vector element group, 1 = mask group, 2 = no destination
  //   dst_sew_off / src_sew_off    log2 of the element-width multiplier
  //   dst_lmul_off / src_lmul_off  log2 of the EMUL multiplier
  //   overlap    0 = any overlap allowed, 1 = partial overlap forbidden,
  //              2 = any overlap forbidden
  //   min_sew    the smallest SEW raw encoding the family supports
  //   vtype_free  the family does not depend on vtype (L509-L510)
  logic [1:0]        cls_dst_kind;
  logic [1:0]        cls_dst_sew_off;
  logic [1:0]        cls_src_sew_off;
  logic signed [4:0] cls_dst_lmul_off;
  logic signed [4:0] cls_src_lmul_off;
  logic [1:0]        cls_overlap;
  logic [2:0]        cls_min_sew;
  logic              cls_vtype_free;

  // ------------------------------------------------------------ vtype decode
  logic [2:0]        sew_raw;
  logic [2:0]        lmul_raw;
  logic              vill;
  logic              vsew_valid;
  logic              vlmul_valid;
  logic signed [4:0] sew_l;      // 3..6 when valid
  logic signed [4:0] lmul_e;     // -3..3
  logic signed [4:0] lmul_e5;
  logic              vtype_legal_c;
  logic [3:0]        vtype_reason_c;

  // ---------------------------------------------------- effective widths/EMUL
  logic signed [4:0] dst_eew_l;
  logic signed [4:0] src_eew_l;
  logic signed [4:0] dst_emul_e;
  logic signed [4:0] src_emul_e;
  logic              dst_eew_ok;
  logic              src_eew_ok;
  logic              dst_emul_ok;
  logic              src_emul_ok;

  // ------------------------------------------------------- group overlap
  logic              mask_dst_overlap_c;
  logic              overlap_ok_c;
  logic              overlap_partial_c;
  logic              overlap_any_c;

  // ------------------------------------------------------------- results
  logic              cfg_legal_c;
  logic [3:0]        reason_c;
  logic [7:0]        elem_count_c;

  // --------------------------------------------------- descriptor state
  logic              valid_q;
  logic [ROB_INDEX_W-1:0] macro_rob_index_q;
  logic [ROB_GEN_W-1:0]   macro_rob_gen_q;
  logic [UOP_INDEX_W-1:0] macro_uop_index_q;
  logic [63:0]       vtype_q;
  logic [7:0]        vl_q;
  logic [6:0]        vstart_q;
  logic [4:0]        vd_q;
  logic [3:0]        mask_ver_q;
  logic [VLEN-1:0]   bitmap_q;
  logic [7:0]        prefix_c;
  logic [7:0]        done_ctr_q;
  logic              fault_valid_q;
  logic [6:0]        fault_elem_q;
  logic [3:0]        fault_code_q;
  logic [15:0]       alloc_ctr_q;
  logic [15:0]       release_ctr_q;

  // ------------------------------------------------------------------ helpers
  // A vector register group with LMUL exponent `exp` occupies one register when
  // exp < 0 (fractional LMUL) and 2^exp registers aligned to that size when
  // exp >= 0 ("Vector register groups must be aligned to a multiple of the
  // group size" for LMUL > 1).
  function automatic logic [4:0] grp_base (input logic [4:0] reg_num,
                                           input logic signed [3:0] exp);
    logic [4:0] size;
    begin
      if (exp < 0) begin
        grp_base = reg_num;
      end else begin
        size = 5'd1 << exp;
        grp_base = reg_num & ~(size - 5'd1);
      end
    end
  endfunction

  function automatic logic [4:0] grp_size (input logic signed [3:0] exp);
    begin
      if (exp < 0) begin
        grp_size = 5'd1;
      end else begin
        grp_size = 5'd1 << exp;
      end
    end
  endfunction

  function automatic logic grp_overlap (input logic [4:0] a_reg,
                                        input logic signed [3:0] a_exp,
                                        input logic [4:0] b_reg,
                                        input logic signed [3:0] b_exp);
    logic [5:0] a_base, a_size, b_base, b_size;
    begin
      a_base = {1'b0, grp_base(a_reg, a_exp)};
      a_size = {1'b0, grp_size(a_exp)};
      b_base = {1'b0, grp_base(b_reg, b_exp)};
      b_size = {1'b0, grp_size(b_exp)};
      grp_overlap = (a_base < (b_base + b_size)) && (b_base < (a_base + a_size));
    end
  endfunction

  function automatic logic grp_equal (input logic [4:0] a_reg,
                                      input logic signed [3:0] a_exp,
                                      input logic [4:0] b_reg,
                                      input logic signed [3:0] b_exp);
    begin
      grp_equal = (grp_size(a_exp) == grp_size(b_exp)) &&
                  (grp_base(a_reg, a_exp) == grp_base(b_reg, b_exp));
    end
  endfunction

  // ------------------------------------------------------- class decode table
  always_comb begin
    cls_dst_kind     = 2'd0;
    cls_dst_sew_off  = 2'd0;
    cls_src_sew_off  = 2'd0;
    cls_dst_lmul_off = 5'sd0;
    cls_src_lmul_off = 5'sd0;
    cls_overlap      = 2'd0;
    cls_min_sew      = 3'd3;
    cls_vtype_free   = 1'b0;
    case (op_class_i)
      VOP_IVV: begin
        // Plain integer arithmetic: EEW = SEW, EMUL = LMUL. vd may alias vs1/vs2.
      end
      VOP_VMUL: begin
        // Integer multiply: EEW = SEW, EMUL = LMUL.
      end
      VOP_VDIV: begin
        // Integer divide/remainder: EEW = SEW, EMUL = LMUL.
      end
      VOP_VWIDE: begin
        // Widening: destination EEW = 2*SEW, destination EMUL = 2*LMUL,
        // sources at EEW = SEW / EMUL = LMUL. The destination group cannot
        // overlap a source group of a different size (V spec, vector register
        // grouping: a widening destination cannot overlap its narrow sources).
        cls_dst_sew_off  = 2'd1;
        cls_dst_lmul_off = 5'sd1;
        cls_overlap      = 2'd1;
      end
      VOP_VNARROW: begin
        // Narrowing: destination EEW = SEW / EMUL = LMUL, source EEW = 2*SEW /
        // EMUL = 2*LMUL. Same non-overlap rule in the other direction.
        cls_src_sew_off  = 2'd1;
        cls_src_lmul_off = 5'sd1;
        cls_overlap      = 2'd1;
      end
      VOP_VEXT: begin
        // Extension (vzext/vsext): destination EEW = 2*SEW / EMUL = LMUL,
        // sources at EEW = SEW / EMUL = LMUL. Destination cannot overlap a
        // source of a different width.
        cls_dst_sew_off = 2'd1;
        cls_overlap     = 2'd1;
      end
      VOP_VRED: begin
        // Reductions write the result into vd[0] and may not overlap any
        // source group.
        cls_overlap = 2'd2;
      end
      VOP_VMASK: begin
        // Mask-producing compare: destination is a mask group of LMUL bits, so
        // there is no element-width rule beyond a legal LMUL.
        cls_dst_kind = 2'd1;
      end
      VOP_VMASKMV: begin
        // Mask-to-mask ops may not overlap their source mask group
        // ("vmsof.m vd, vs2" requires vd not to overlap vs2).
        cls_dst_kind = 2'd1;
        cls_overlap  = 2'd2;
      end
      VOP_VSLIDE: begin
        // Slide/permute/gather: EEW = SEW, EMUL = LMUL; overlap allowed.
      end
      VOP_VFP: begin
        // Vector FP arithmetic exists for SEW 16/32/64 only.
        cls_min_sew = 3'd4;
      end
      VOP_VFPWIDE: begin
        // Widening FP: destination EEW = 2*SEW / EMUL = 2*LMUL, SEW >= 16.
        cls_dst_sew_off  = 2'd1;
        cls_dst_lmul_off = 5'sd1;
        cls_overlap      = 2'd1;
        cls_min_sew      = 3'd4;
      end
      VOP_VFPRED: begin
        // FP reduction: result in vd[0], must not overlap a source, SEW >= 16.
        cls_overlap = 2'd2;
        cls_min_sew = 3'd4;
      end
      VOP_VLOAD: begin
        // Vector load: EEW = SEW, EMUL = LMUL.
      end
      VOP_VSTORE: begin
        // Vector store: no destination group; the source group is at
        // EEW = SEW / EMUL = LMUL. A masked store is legal, so the
        // mask-destination rule cannot apply.
        cls_dst_kind = 2'd2;
      end
      VOP_VWHOLE: begin
        // Whole-register load/store/move do not depend on vtype (L509-L510).
        cls_vtype_free = 1'b1;
      end
      VOP_VSET: begin
        // vset{i}vl{i} defines vtype and cannot depend on it.
        cls_vtype_free = 1'b1;
      end
      VOP_VSPECIAL: begin
        // Scalar/vector moves: EEW = SEW, EMUL = LMUL.
      end
      default: begin
        // No family. A defined answer rather than a don't-care: the class is
        // invalid, which the reason chain reports.
        cls_vtype_free = 1'b0;
      end
    endcase
  end

  // ------------------------------------------------------------ vtype decode
  always_comb begin
    sew_raw     = vtype_i[7:5];
    lmul_raw    = vtype_i[2:0];
    vill        = vtype_i[63];
    vsew_valid  = (sew_raw >= 3'd3) && (sew_raw <= 3'd6);
    vlmul_valid = (lmul_raw != 3'b100);
    case (lmul_raw)
      3'b000:  lmul_e = 5'sd0;
      3'b001:  lmul_e = 5'sd1;
      3'b010:  lmul_e = 5'sd2;
      3'b011:  lmul_e = 5'sd3;
      3'b101:  lmul_e = -5'sd3;
      3'b110:  lmul_e = -5'sd2;
      3'b111:  lmul_e = -5'sd1;
      default: lmul_e = 5'sd0;   // reserved encoding; caught by vlmul_valid
    endcase
    sew_l    = $signed({2'b0, sew_raw});
    lmul_e5  = lmul_e;

    vtype_legal_c = (!vill) && vsew_valid && vlmul_valid &&
                    ((lmul_e5 + $signed({1'b0, 4'(ELEN_LOG2)})) >= sew_l);
    if (vill)                  vtype_reason_c = RSN_VTYPE_UNSUPP;
    else if (!vsew_valid)      vtype_reason_c = RSN_RESERVED_VSEW;
    else if (!vlmul_valid)     vtype_reason_c = RSN_RESERVED_VLMUL;
    else if ((lmul_e5 + $signed({1'b0, 4'(ELEN_LOG2)})) < sew_l) vtype_reason_c = RSN_EMUL_RANGE;
    else                       vtype_reason_c = RSN_OK;
  end

  // ------------------------------------------------- effective widths and EMUL
  always_comb begin
    dst_eew_l  = sew_l + $signed({3'b0, cls_dst_sew_off});
    src_eew_l  = sew_l + $signed({3'b0, cls_src_sew_off});
    dst_emul_e = lmul_e5 + cls_dst_lmul_off;
    src_emul_e = lmul_e5 + cls_src_lmul_off;

    dst_eew_ok = (dst_eew_l >= 5'sd3) && (dst_eew_l <= $signed({1'b0, 4'(ELEN_LOG2)}));
    src_eew_ok = (src_eew_l >= 5'sd3) && (src_eew_l <= $signed({1'b0, 4'(ELEN_LOG2)}));
    dst_emul_ok = (dst_emul_e >= -5'sd3) && (dst_emul_e <= 5'sd3);
    src_emul_ok = (src_emul_e >= -5'sd3) && (src_emul_e <= 5'sd3);

    // Elements in the group: VLEN * 2^LMUL / 2^SEW = 2^(log2(VLEN)+lmul_e-sew_l).
    // VLEN is a power of two by construction (128), so this is exact. The
    // clamp keeps the value non-negative for an unsupported vtype; the driver
    // only reads it on a legal configuration.
    if ((5'sd7 + lmul_e5 - sew_l) >= 5'sd0) begin
      elem_count_c = 8'd1 << (7 + lmul_e5 - sew_l);
    end else begin
      elem_count_c = 8'd0;
    end
  end

  // ------------------------------------------------------------- mask / overlap
  always_comb begin
    // A destination group includes v0 exactly when its aligned base is 0.
    mask_dst_overlap_c = (cls_dst_kind != 2'd2) &&
                         (grp_base(vd_i, dst_emul_e[3:0]) == 5'd0);

    overlap_partial_c = grp_overlap(vd_i, dst_emul_e[3:0], vs1_i, src_emul_e[3:0]) &&
                        !grp_equal(vd_i, dst_emul_e[3:0], vs1_i, src_emul_e[3:0]);
    overlap_any_c     = grp_overlap(vd_i, dst_emul_e[3:0], vs1_i, src_emul_e[3:0]);

    overlap_partial_c = overlap_partial_c ||
                        (grp_overlap(vd_i, dst_emul_e[3:0], vs2_i, src_emul_e[3:0]) &&
                         !grp_equal(vd_i, dst_emul_e[3:0], vs2_i, src_emul_e[3:0]));
    overlap_any_c     = overlap_any_c ||
                        grp_overlap(vd_i, dst_emul_e[3:0], vs2_i, src_emul_e[3:0]);

    overlap_partial_c = overlap_partial_c ||
                        (grp_overlap(vd_i, dst_emul_e[3:0], vs3_i, src_emul_e[3:0]) &&
                         !grp_equal(vd_i, dst_emul_e[3:0], vs3_i, src_emul_e[3:0]));
    overlap_any_c     = overlap_any_c ||
                        grp_overlap(vd_i, dst_emul_e[3:0], vs3_i, src_emul_e[3:0]);

    if (cls_dst_kind == 2'd2) begin
      // No destination: nothing to overlap.
      overlap_ok_c = 1'b1;
    end else if (cls_overlap == 2'd2) begin
      overlap_ok_c = !overlap_any_c;
    end else if (cls_overlap == 2'd1) begin
      overlap_ok_c = !overlap_partial_c;
    end else begin
      overlap_ok_c = 1'b1;
    end
  end

  // ---------------------------------------------------- reason and verdict
  always_comb begin
    reason_c = RSN_OK;
    cfg_legal_c = 1'b1;
    if (op_class_i >= VOP_COUNT) begin
      reason_c = RSN_CLASS_INVALID;
      cfg_legal_c = 1'b0;
    end else if (cls_vtype_free) begin
      // Whole-register moves and vset do not depend on vtype.
      reason_c = RSN_OK;
      cfg_legal_c = 1'b1;
    end else if (!vtype_legal_c) begin
      reason_c = vtype_reason_c;
      cfg_legal_c = 1'b0;
    end else if (sew_l < $signed({2'b0, cls_min_sew})) begin
      // The family does not exist at this SEW (e.g. no 8-bit vector FP).
      reason_c = RSN_EEW_RANGE;
      cfg_legal_c = 1'b0;
    end else if ((cls_dst_kind == 2'd0) && !dst_eew_ok) begin
      reason_c = RSN_EEW_RANGE;
      cfg_legal_c = 1'b0;
    end else if (!src_eew_ok || !dst_emul_ok || !src_emul_ok) begin
      reason_c = RSN_EMUL_RANGE;
      cfg_legal_c = 1'b0;
    end else if (mask_en_i && mask_dst_overlap_c) begin
      reason_c = RSN_MASK_DST_OVER;
      cfg_legal_c = 1'b0;
    end else if (!overlap_ok_c) begin
      reason_c = RSN_OVERLAP_SRC;
      cfg_legal_c = 1'b0;
    end else begin
      reason_c = RSN_OK;
      cfg_legal_c = 1'b1;
    end

`ifdef MOSAIC_VEC_MUTANT_ILLEGAL_ACCEPTED
    // NEGATIVE CONTROL: a widening configuration whose destination EMUL
    // exceeds 8 is accepted even though the matrix forbids it.
    if ((op_class_i == VOP_VWIDE) && (reason_c == RSN_EMUL_RANGE)) begin
      reason_c = RSN_OK;
      cfg_legal_c = 1'b1;
    end
`endif

    o_vtype_legal_o = vtype_legal_c;
    o_cfg_legal_o   = cfg_legal_c;
    o_illegal_o     = !cfg_legal_c;
    o_reason_o      = reason_c;
    o_sew_log2_o    = sew_raw;
    o_lmul_exp_o    = lmul_e[3:0];
    o_emul_src_exp_o = src_emul_e[3:0];
    o_emul_dst_exp_o = dst_emul_e[3:0];
    o_elem_count_o  = elem_count_c;
    o_vlen_o        = 8'(VLEN);

`ifdef MOSAIC_VEC_MUTANT_LANE_VLEN
    // NEGATIVE CONTROL: the runtime lane quota leaks into the architectural
    // VLEN. The lane count must only change throughput, never VLEN.
    o_vlen_o = 8'(VLEN) + {4'b0, lane_count_i};
`endif

    o_vlenb_o       = 8'(VLEN / 8);
    o_lane_count_o  = lane_count_i;
    o_class_count_o = 8'(VOP_COUNT);
  end

  // ------------------------------------------------------ contiguous prefix
  // The count of consecutive set bits from element 0 -- the committed prefix,
  // and the restart point after a fault. Kept separate from the bitmap's
  // population count: an element may complete out of order and still not be
  // restart-safe.
  always_comb begin
    prefix_c = 8'd0;
    for (int unsigned i = 0; i < VLEN; i++) begin
      if (bitmap_q[i] && (prefix_c == 8'(i))) begin
        prefix_c = prefix_c + 8'd1;
      end
    end
  end

  // ----------------------------------------------------------- descriptor FSM
  assign alloc_ready_o = (!valid_q) || release_i;

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      // Reset has a defined effect: an in-flight macro is squashed and leaves
      // no partial bitmap, no identity and no counters behind.
      valid_q          <= 1'b0;
      macro_rob_index_q <= '0;
      macro_rob_gen_q   <= '0;
      macro_uop_index_q <= '0;
      vtype_q          <= 64'd0;
      vl_q             <= 8'd0;
      vstart_q         <= 7'd0;
      vd_q             <= 5'd0;
      mask_ver_q       <= 4'd0;
      bitmap_q         <= '0;
      done_ctr_q       <= 8'd0;
      fault_valid_q    <= 1'b0;
      fault_elem_q     <= 7'd0;
      fault_code_q     <= 4'd0;
      alloc_ctr_q      <= 16'd0;
      release_ctr_q    <= 16'd0;
    end else begin
      if (release_i) begin
        valid_q       <= 1'b0;
        bitmap_q      <= '0;
        done_ctr_q    <= 8'd0;
        fault_valid_q <= 1'b0;
        release_ctr_q <= release_ctr_q + 16'd1;
      end
      if (elem_done_valid_i && valid_q && !fault_valid_q &&
          !bitmap_q[elem_done_index_i]) begin
        bitmap_q[elem_done_index_i] <= 1'b1;
        done_ctr_q <= done_ctr_q + 8'd1;
      end
`ifdef MOSAIC_VEC_MUTANT_LOSE_PROGRESS
      // NEGATIVE CONTROL: every element completion is dropped from the bitmap,
      // so the descriptor loses progress and a restart would redo committed
      // elements -- or, worse, never see the prefix advance.
      begin
        bitmap_q   <= bitmap_q;
        done_ctr_q <= done_ctr_q;
      end
`endif
      if (fault_valid_i && valid_q && !fault_valid_q) begin
        fault_valid_q <= 1'b1;
        fault_elem_q  <= fault_elem_i;
        fault_code_q  <= fault_code_i;
      end
      if (alloc_valid_i && alloc_ready_o) begin
        // The allocate is last so that a same-cycle release-and-allocate
        // installs the new macro; the ordering is stated, not incidental.
        valid_q           <= 1'b1;
        macro_rob_index_q <= alloc_rob_index_i;
        macro_rob_gen_q   <= alloc_rob_gen_i;
        macro_uop_index_q <= alloc_uop_index_i;
        vtype_q           <= alloc_vtype_i;
        vl_q              <= alloc_vl_i;
        vstart_q          <= alloc_vstart_i;
        vd_q              <= alloc_vd_i;
        mask_ver_q        <= alloc_mask_ver_i;
        bitmap_q          <= '0;
        done_ctr_q        <= 8'd0;
        fault_valid_q     <= 1'b0;
        alloc_ctr_q       <= alloc_ctr_q + 16'd1;
      end
    end
  end

  assign o_valid_o            = valid_q;
  assign o_macro_rob_index_o  = macro_rob_index_q;
  assign o_macro_rob_gen_o    = macro_rob_gen_q;
  assign o_macro_uop_index_o  = macro_uop_index_q;
  assign o_vtype_o            = vtype_q;
  assign o_vl_o               = vl_q;
  assign o_vstart_o           = vstart_q;
  assign o_vd_o               = vd_q;
  assign o_mask_ver_o         = mask_ver_q;
  assign o_elem_bitmap_o      = bitmap_q;
  assign o_prefix_o           = prefix_c;
  assign o_elems_done_ctr_o   = done_ctr_q;
  assign o_fault_valid_o      = fault_valid_q;
  assign o_fault_elem_o       = fault_elem_q;
  assign o_fault_code_o       = fault_code_q;
  assign o_accepting_elems_o  = valid_q && !fault_valid_q;
  // One macro identity, one descriptor entry: this must never scale with the
  // element count.
  assign o_rob_entries_used_o = valid_q ? 8'd1 : 8'd0;
  assign o_alloc_ctr_o        = alloc_ctr_q;
  assign o_release_ctr_o      = release_ctr_q;

endmodule : mosaic_vec_desc

`default_nettype wire
`resetall
