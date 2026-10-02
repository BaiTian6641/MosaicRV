// ============================================================================
// mosaic_vec_fp -- vector floating-point packet execution with per-element
// exception flags (work package I-055, CASE=rvv.fp_flags_reduction).
//
// ------------------------------------------------------------------ what it is
//
// The unit executes one vector *packet*: it walks the destination elements of
// one floating-point operation, reads the source elements it needs from the
// banked VRF (`mosaic_vrf`, I-053) through a single read slot, and writes the
// destination through a single write slot. Its configuration is the snapshot
// `mosaic_vec_cfg` (I-052) hands out, exactly as `mosaic_vec_alu` (I-054) and
// `mosaic_vec_lsu` (I-056) take theirs; nothing here re-derives SEW/LMUL from
// anywhere else.
//
// ------------------------------------------------------------- the FPU reuse
//
// *One* `mosaic_fpu` instance -- the datapath I-049 built and
// CASE=fp.operation_matrix proves -- is instantiated here and driven element by
// element behind its own tagged request/response handshake. No arithmetic is
// written a second time: this module contains no adder, no rounder and no NaN
// classifier. The element engine issues one FPU request, waits for `res_valid`
// with `res_ready` held high, and consumes the value and the five flag bits
// beside it. A second FPU instance was considered and rejected: one packet is
// in flight at a time in this unit, the operations are serialised by the VRF's
// single read slot anyway, and a second datapath would double the area of a
// unit whose throughput is bounded elsewhere (I-027/I-059). That decision is
// recorded in results/reports/I-055-vector-fp.md.
//
// The FPU's own declared latency (`o_latency_o`) is captured with every
// response and exposed as `o_last_latency_o`, so the case can check the
// declaration (1 cycle for everything but `fdiv`, 66 for `fdiv`) rather than
// hard-code it.
//
// ------------------------------------------------------------ the flag rule
//
// The rule, stated once and made checkable in both directions:
//
//   * an element contributes its `{NV,DZ,OF,UF,NX}` to this macro's pending
//     flags if and only if it is **active** (vstart <= i < vl, mask bit set)
//     and the operation is one the spec says raises flags;
//   * an inactive element -- masked off or tail -- performs **no FPU operation**
//     and therefore contributes nothing; it only receives the vma/vta policy
//     write;
//   * elements are folded in ascending element order, and the per-element flag
//     contribution is published on the trace (`o_flag_trace_valid_o` /
//     `o_flag_trace_elem_o` / `o_flag_trace_flags_o`) in that order;
//   * the pending flags become **architectural** only when the macro retires:
//     `commit_valid_i` while a completed macro is pending ORs `pending` into
//     `o_fflags_arch_o`. A `flush_i` before that discards the pending flags and
//     changes nothing architectural. A squashed macro contributes nothing.
//
// The two "inactive" directions the case proves are that a masked-off element
// carrying a signalling NaN does not set NV, and that a masked-off element does
// not perform the FPU operation at all (`o_fpu_issues_o` equals the active
// element count).
//
// ------------------------------------------------------------ the families
//
// NFAM = 10 capability bits, one per family:
//
//   0 ELEM      vfadd, vfsub, vfrsub, vfmul, vfdiv, vfrdiv
//   1 MINMAX    vfmin, vfmax
//   2 SGNJ      vfsgnj, vfsgnjn, vfsgnjx
//   3 CMP       vmfeq, vmfne, vmflt, vmfle, vmfgt, vmfge   (destination = mask)
//   4 CVT       vfcvt.xu.f.v, vfcvt.x.f.v, vfcvt.f.xu.v, vfcvt.f.x.v
//   5 WIDE      vfwcvt.xu.f.v, vfwcvt.x.f.v, vfwcvt.f.xu.v, vfwcvt.f.x.v,
//               vfwcvt.f.f.v                                 (destination 2*SEW)
//   6 NARROW    vfncvt.xu.f.w, vfncvt.x.f.w, vfncvt.f.xu.w, vfncvt.f.x.w,
//               vfncvt.f.f.w, vfncvt.rtz.xu.f.w, vfncvt.rtz.x.f.w  (source 2*SEW)
//   7 REDSUM    vfredosum (ordered), vfredusum (unordered)
//   8 REDMINMAX vfredmin, vfredmax
//   9 REDWIDE   vfwredosum (ordered), vfwredusum (unordered)  (accumulator 2*SEW)
//
// Deliberately NOT implemented, and therefore not claimable -- there is no bit
// for them, so they cannot be advertised: `vfsqrt` (the scalar `fsqrt` is
// declared absent by I-049 and this family would need a datapath that does not
// exist); the fused multiply-accumulate family (`vfmacc`, `vfnmacc`, `vfmsac`,
// `vfnmsac`, `vfmadd`, `vfnmadd`, `vfmsub`, `vfnmsub`, `vfwmacc*`, `vfwmsac`,
// `vfw*` with the same shapes -- the scalar fmadd family is declared absent by
// I-049); `vfncvt.rod.f.f.w` (round-to-odd is not one of the five rounding modes
// the shared FPU implements); `vfmerge`/`vfmv.*`; and every half-precision
// operation -- SEW=16 is refused because the F/D datapath this unit reuses has
// no half format, so this machine supports vector FP at SEW=32 and SEW=64 only.
// The widening/narrowing forms therefore exist for SEW=32 (2*SEW = 64 = ELEN).
//
// `o_illegal_o` is set, and the packet issues no VRF transaction and no FPU
// request, for an unknown family, a family whose capability bit is clear, an
// operation id outside the family's table, or an element width the unit does
// not support.
//
// ---------------------------------------------------------------- policy
//
// Same active/tail rule as I-054, stated once:
//
//   element i is active  iff  vstart <= i < vl AND (v0.mask[i] or unmasked);
//   active                            -> written with the result;
//   inactive with vma/vta == 0        -> unchanged;
//   inactive with vma/vta == 1        -> written with all-ones (for a mask
//                                        destination, the bit is written 1).
//
// A mask destination is written with a read-modify-write of its SEW=8 byte,
// because the other bits of that byte belong to other elements.
//
// ------------------------------------------------------ the determinism rule
//
// Element-wise operations, conversions, min/max, sign-injection, comparisons
// and the **ordered** reductions (`vfredosum`, `vfwredosum`) are *deterministic*:
// their result is bit-exact and the case compares with `==`, never with a
// tolerance. `vfredusum` and `vfwredusum` are *permitted to differ* between
// implementations (the spec permits any reduction tree with exact internal
// sums); this unit implements them as the ordered serial fold, which the spec
// names as a valid implementation, and the case checks the result against an
// explicitly enumerated permitted set rather than a tolerance. See the report's
// determinism table and the comparator in sim/unit/tb_vec.cpp.
//
// Mutation hooks
// --------------
// The shipping build defines none of the `MOSAIC_VEC_FP_MUTANT_*` macros. Each
// injects exactly one broken behaviour so the case can be shown to detect it;
// the table is in results/reports/I-055-vector-fp.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
// Both packages carry their own include guards; see mosaic_muldiv.sv for why
// they are included rather than assumed present on the command line.
`include "mosaic_pkg.sv"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned VFP_ROB_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned VFP_GEN_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned VFP_UOP_W = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;

module mosaic_vec_fp #(
    parameter int unsigned VLEN = 128,
    parameter int unsigned ELEN = 64,
    parameter int unsigned NFAM = 10
) (
    input  logic                    clk_i,
    input  logic                    rst_i,

    // -------------------------------------------------------- capability gate
    input  logic [NFAM-1:0]         caps_i,

    // ----------------------------------------- configuration snapshot (I-052)
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [63:0]             cfg_vtype_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [7:0]              cfg_vl_i,
    input  logic [6:0]              cfg_vstart_i,
    input  logic [2:0]              cfg_frm_i,

    // ------------------------------------------------------------ execution
    input  logic                    exec_valid_i,
    input  logic [4:0]              exec_family_i,
    input  logic [3:0]              exec_op_i,
    input  logic [1:0]              exec_form_i,
    input  logic [4:0]              exec_vd_i,
    input  logic [4:0]              exec_vs1_i,
    input  logic [4:0]              exec_vs2_i,
    input  logic [63:0]             exec_scalar_i,
    input  logic                    exec_mask_en_i,
    // The instruction's own rm field; 111 means "take frm".
    input  logic [2:0]              exec_rm_i,

    // ---------------------------------------------------- the commit boundary
    // "The macro this unit last executed retires." Flags become architectural
    // only here; a flush before this discards them.
    input  logic                    commit_valid_i,
    input  logic                    flush_i,

    // -------------------------------------------------------------- status
    output logic                    exec_busy_o,
    output logic                    exec_done_o,
    output logic                    exec_illegal_o,
    output logic                    exec_trap_o,
    output logic [6:0]              exec_trap_elem_o,
    output logic [7:0]              exec_elems_o,
    output logic [7:0]              exec_cur_o,
    output logic [63:0]             exec_acc_o,
    output logic [7:0]              exec_writes_o,

    // ---------------------------------------------------------------- flags
    output logic [4:0]              o_fflags_pending_o,   // this macro, pre-retire
    output logic [4:0]              o_fflags_arch_o,      // {NV,DZ,OF,UF,NX}
    output logic                    o_commit_o,           // a commit took effect
    output logic [4:0]              o_commit_fflags_o,    // what that commit added
    output logic [15:0]             o_commit_ctr_o,
    output logic [15:0]             o_flush_ctr_o,
    output logic [15:0]             o_spurious_ctr_o,
    output logic [15:0]             o_inactive_flag_ctr_o,
    output logic                    o_macro_pending_o,
    output logic                    o_flag_trace_valid_o,
    output logic [7:0]              o_flag_trace_elem_o,
    output logic [4:0]              o_flag_trace_flags_o,
    output logic [31:0]             o_fpu_issues_o,
    output logic [7:0]              o_fpu_latency_o,
    output logic [7:0]              o_last_latency_o,

    // -------------------------------------------------------- VRF read slot
    output logic                    vrf_rd_valid_o,
    output logic [4:0]              vrf_rd_base_o,
    output logic [6:0]              vrf_rd_elem_o,
    output logic [2:0]              vrf_rd_sew_o,
    output logic [3:0]              vrf_rd_lmul_o,
    output logic [15:0]             vrf_rd_tag_o,
    input  logic                    vrf_rd_gnt_i,
    input  logic                    vrf_rd_rsp_valid_i,
    input  logic [15:0]             vrf_rd_rsp_tag_i,
    input  logic [63:0]             vrf_rd_rsp_data_i,

    // ------------------------------------------------------- VRF write slot
    output logic                    vrf_wr_valid_o,
    output logic [4:0]              vrf_wr_base_o,
    output logic [6:0]              vrf_wr_elem_o,
    output logic [2:0]              vrf_wr_sew_o,
    output logic [3:0]              vrf_wr_lmul_o,
    output logic [63:0]             vrf_wr_data_o,
    input  logic                    vrf_wr_gnt_i
);

  // ---------------------------------------------------------------- mutants
`ifdef MOSAIC_VEC_FP_MUTANT_INACTIVE_FLAG
  // NEGATIVE CONTROL (control 1): a masked-off element is still computed and
  // its flags are merged, so an inactive element pollutes the macro's flags.
  localparam bit MutInactiveFlag = 1'b1;
`else
  localparam bit MutInactiveFlag = 1'b0;
`endif
`ifdef MOSAIC_VEC_FP_MUTANT_EARLY_COMMIT
  // NEGATIVE CONTROL (control 2): the macro's flags become architectural when
  // the packet completes, not when it retires.
  localparam bit MutEarlyCommit = 1'b1;
`else
  localparam bit MutEarlyCommit = 1'b0;
`endif
`ifdef MOSAIC_VEC_FP_MUTANT_REDUCE_REASSOC
  // NEGATIVE CONTROL (control 3): an ordered reduction folds its elements in
  // descending order -- a reassociation the specification does not allow.
  localparam bit MutReduceReassoc = 1'b1;
`else
  localparam bit MutReduceReassoc = 1'b0;
`endif
`ifdef MOSAIC_VEC_FP_MUTANT_RM_IGNORE
  // NEGATIVE CONTROL (control 4): the rounding mode is ignored and every
  // operation rounds to nearest-even, so a tie is rounded the wrong way.
  localparam bit MutRmIgnore = 1'b1;
`else
  localparam bit MutRmIgnore = 1'b0;
`endif

  // ------------------------------------------------------------- families
  localparam logic [4:0] F_ELEM      = 5'd0;
  localparam logic [4:0] F_MINMAX    = 5'd1;
  localparam logic [4:0] F_SGNJ      = 5'd2;
  localparam logic [4:0] F_CMP       = 5'd3;
  localparam logic [4:0] F_CVT       = 5'd4;
  localparam logic [4:0] F_WIDE      = 5'd5;
  localparam logic [4:0] F_NARROW    = 5'd6;
  localparam logic [4:0] F_REDSUM    = 5'd7;
  localparam logic [4:0] F_REDMINMAX = 5'd8;
  localparam logic [4:0] F_REDWIDE   = 5'd9;

  localparam logic [1:0] FORM_VV = 2'd0;

  // ------------------------------------------------------------- VRF tags
  localparam logic [15:0] TAG_ACC  = 16'h1000;
  localparam logic [15:0] TAG_VS2  = 16'h2000;
  localparam logic [15:0] TAG_VS1  = 16'h3000;
  localparam logic [15:0] TAG_MASK = 16'h4000;
  localparam logic [15:0] TAG_DST  = 16'h5000;

  // --------------------------------------------------------------- states
  localparam logic [4:0] S_IDLE  = 5'd0;
  localparam logic [4:0] S_SETUP = 5'd1;
  localparam logic [4:0] S_ACCRD = 5'd2;
  localparam logic [4:0] S_ACCW  = 5'd3;
  localparam logic [4:0] S_ELEM  = 5'd4;
  localparam logic [4:0] S_MASKR = 5'd5;
  localparam logic [4:0] S_MASKW = 5'd6;
  localparam logic [4:0] S_PLAN  = 5'd7;
  localparam logic [4:0] S_VS2R  = 5'd8;
  localparam logic [4:0] S_VS2W  = 5'd9;
  localparam logic [4:0] S_VS1R  = 5'd10;
  localparam logic [4:0] S_VS1W  = 5'd11;
  localparam logic [4:0] S_FPU   = 5'd12;
  localparam logic [4:0] S_FPUW  = 5'd13;
  localparam logic [4:0] S_DSTR  = 5'd14;
  localparam logic [4:0] S_DSTW  = 5'd15;
  localparam logic [4:0] S_DSTX  = 5'd16;
  localparam logic [4:0] S_WR    = 5'd17;
  localparam logic [4:0] S_MWR   = 5'd18;
  localparam logic [4:0] S_RWR   = 5'd19;
  localparam logic [4:0] S_NEXT  = 5'd20;
  localparam logic [4:0] S_FINAL = 5'd21;
  localparam logic [4:0] S_DONE  = 5'd22;

  // ---------------------------------------------------------- configuration
  // Decoded from the I-052 snapshot at the ratified positions, and declared
  // here because the operation plan below reads them.
  localparam int unsigned VFP_VLEN_LOG2 = $clog2(VLEN);

  logic [2:0]        sew_l;
  logic signed [3:0] lmul;
  logic              vta;
  logic              vma;
  logic              sew64;
  logic              sew32;

  always_comb begin
    sew_l = cfg_vtype_i[5:3];
    case (cfg_vtype_i[2:0])
      3'd0:    lmul = 4'sd0;
      3'd1:    lmul = 4'sd1;
      3'd2:    lmul = 4'sd2;
      3'd3:    lmul = 4'sd3;
      3'd5:    lmul = -4'sd3;
      3'd6:    lmul = -4'sd2;
      3'd7:    lmul = -4'sd1;
      default: lmul = 4'sd0;
    endcase
    vta   = cfg_vtype_i[6];
    vma   = cfg_vtype_i[7];
    sew64 = (sew_l == 3'd6);
    sew32 = (sew_l == 3'd5);
  end

  // ------------------------------------------------------------- the plan
  typedef struct packed {
    logic       swap;      // FPU operand order (a = the vs2 element)
    logic       invert;    // vmfne: invert the comparison result
    logic [4:0] fop;       // mosaic_pkg::fp_op_e
    logic       fmt;       // fp format operand for the FPU
    logic       iw;        // conversion: 1 = 64-bit integer side
    logic       is;        // conversion: 1 = signed integer side
    logic [2:0] rm;        // rounding mode (instruction's, or fixed for rtz)
    logic       two;       // a second FPU sub-operation is required
  } fp_plan_t;

  function automatic logic [4:0] fp_op_count(input logic [4:0] f);
    begin
      case (f)
        F_ELEM:      fp_op_count = 5'd6;
        F_MINMAX:    fp_op_count = 5'd2;
        F_SGNJ:      fp_op_count = 5'd3;
        F_CMP:       fp_op_count = 5'd6;
        F_CVT:       fp_op_count = 5'd4;
        F_WIDE:      fp_op_count = 5'd5;
        F_NARROW:    fp_op_count = 5'd7;
        F_REDSUM:    fp_op_count = 5'd2;
        F_REDMINMAX: fp_op_count = 5'd2;
        F_REDWIDE:   fp_op_count = 5'd2;
        default:     fp_op_count = 5'd0;
      endcase
    end
  endfunction

  // The operation -> FPU mapping, written from the V specification's operation
  // table. `swap` is set where the RISC-V operand order is the reverse of the
  // FPU's (a, b): vfsub is vs2 - vs1 and vfdiv is vs2 / vs1, while vfrsub and
  // vfrdiv put the scalar first; vfsgnj* take the magnitude from vs2.
  function automatic fp_plan_t fp_plan(input logic [4:0] fam, input logic [3:0] op,
                                       input logic is64, input logic [2:0] rm_instr);
    fp_plan_t p;
    begin
      p.swap     = 1'b0;
      p.invert   = 1'b0;
      p.fop      = mosaic_pkg::FP_ADD;
      // The FPU's `req_fmt_i` is 1 for single and 0 for double, so the format
      // follows the element width: SEW=32 is single, SEW=64 is double.
      p.fmt      = sew32;
      p.iw       = is64;
      p.is       = 1'b0;
      p.rm       = rm_instr;
      p.two      = 1'b0;
      case (fam)
        F_ELEM: begin
          case (op)
            4'd0: p.fop = mosaic_pkg::FP_ADD;
            4'd1: begin p.fop = mosaic_pkg::FP_SUB; p.swap = 1'b1; end
            4'd2: p.fop = mosaic_pkg::FP_SUB;                       // vfrsub
            4'd3: p.fop = mosaic_pkg::FP_MUL;
            4'd4: begin p.fop = mosaic_pkg::FP_DIV; p.swap = 1'b1; end
            default: p.fop = mosaic_pkg::FP_DIV;                    // vfrdiv
          endcase
        end
        F_MINMAX: begin
          p.fop = (op == 4'd0) ? mosaic_pkg::FP_MIN : mosaic_pkg::FP_MAX;
        end
        F_SGNJ: begin
          case (op)
            4'd0: p.fop = mosaic_pkg::FP_SGNJ;
            4'd1: p.fop = mosaic_pkg::FP_SGNJN;
            default: p.fop = mosaic_pkg::FP_SGNJX;
          endcase
          p.swap = 1'b1;
        end
        F_CMP: begin
          case (op)
            4'd0: p.fop = mosaic_pkg::FP_CMP_EQ;
            4'd1: begin p.fop = mosaic_pkg::FP_CMP_EQ; p.invert = 1'b1; end
            4'd2: begin p.fop = mosaic_pkg::FP_CMP_LT; p.swap = 1'b1; end
            4'd3: begin p.fop = mosaic_pkg::FP_CMP_LE; p.swap = 1'b1; end
            4'd4: p.fop = mosaic_pkg::FP_CMP_LT;                    // vmfgt
            default: p.fop = mosaic_pkg::FP_CMP_LE;                 // vmfge
          endcase
        end
        F_CVT: begin
          case (op)
            4'd0: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = is64; p.is = 1'b0; end
            4'd1: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = is64; p.is = 1'b1; end
            4'd2: begin p.fop = mosaic_pkg::FP_CVT_IF; p.iw = is64; p.is = 1'b0; end
            default: begin p.fop = mosaic_pkg::FP_CVT_IF; p.iw = is64; p.is = 1'b1; end
          endcase
        end
        F_WIDE: begin
          p.fmt = 1'b1;              // the source element is single precision
          case (op)
            4'd0: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = 1'b1; p.is = 1'b0; end
            4'd1: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = 1'b1; p.is = 1'b1; end
            4'd2: begin p.fop = mosaic_pkg::FP_CVT_IF; p.fmt = 1'b0;
                        p.iw = 1'b0; p.is = 1'b0; end
            4'd3: begin p.fop = mosaic_pkg::FP_CVT_IF; p.fmt = 1'b0;
                        p.iw = 1'b0; p.is = 1'b1; end
            default: begin p.fop = mosaic_pkg::FP_CVT_SF; p.fmt = 1'b0; end
          endcase
        end
        F_NARROW: begin
          p.fmt = 1'b0;              // the source element is double precision
          case (op)
            4'd0: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = 1'b0; p.is = 1'b0; end
            4'd1: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = 1'b0; p.is = 1'b1; end
            4'd2: begin p.fop = mosaic_pkg::FP_CVT_IF; p.fmt = 1'b1;
                        p.iw = 1'b1; p.is = 1'b0; end
            4'd3: begin p.fop = mosaic_pkg::FP_CVT_IF; p.fmt = 1'b1;
                        p.iw = 1'b1; p.is = 1'b1; end
            4'd4: begin p.fop = mosaic_pkg::FP_CVT_FS; p.fmt = 1'b1; end
            4'd5: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = 1'b0; p.is = 1'b0;
                        p.rm = 3'b001; end   // vfncvt.rtz.xu
            default: begin p.fop = mosaic_pkg::FP_CVT_FI; p.iw = 1'b0; p.is = 1'b1;
                        p.rm = 3'b001; end   // vfncvt.rtz.x
          endcase
        end
        F_REDSUM: begin
          p.fop = mosaic_pkg::FP_ADD;    // ordered and unordered, both serial
        end
        F_REDMINMAX: begin
          p.fop = (op == 4'd0) ? mosaic_pkg::FP_MIN : mosaic_pkg::FP_MAX;
        end
        default: begin                   // F_REDWIDE: promote, then add
          p.fop = mosaic_pkg::FP_CVT_SF;
          p.fmt = 1'b0;
          p.two = 1'b1;
        end
      endcase
      fp_plan = p;
    end
  endfunction

  function automatic logic [63:0] wmask(input int w);
    begin
      if (w >= 64) begin
        wmask = 64'hFFFF_FFFF_FFFF_FFFF;
      end else begin
        wmask = (64'd1 << w) - 64'd1;
      end
    end
  endfunction

  // ------------------------------------------------------------- engine state
  logic [4:0]        state_q;
  logic [4:0]        op_f_q;
  logic [3:0]        op_op_q;
  logic [1:0]        op_form_q;
  logic [4:0]        op_vd_q;
  logic [4:0]        op_vs1_q;
  logic [4:0]        op_vs2_q;
  logic [63:0]       op_scalar_q;
  logic              masken_q;
  logic [2:0]        sew_l_q;
  logic signed [3:0] lmul_q;
  logic              vta_q;
  logic              vma_q;
  logic [7:0]        vl_q;
  logic [6:0]        vstart_q;
  logic [2:0]        rm_eff_q;

  logic [7:0]        dst_n_q;
  logic [7:0]        cur_q;
  logic [7:0]        rcount_q;
  logic              mbit_q;
  logic              active_q;
  logic [1:0]        sub_q;

  logic [63:0]       a_q;
  logic [63:0]       b_q;
  logic [63:0]       acc_q;
  logic [63:0]       result_q;
  logic [7:0]        byte_new_q;
  logic [63:0]       fpu_data_q;
  logic [7:0]        dbyte_q;

  logic [7:0]        elems_r;
  logic [7:0]        writes_r;
  logic              done_r;
  logic              illegal_r;

  logic [4:0]        pending_q;
  logic [4:0]        arch_q;
  logic              macro_pending_q;
  logic              ftrace_v_r;
  logic [7:0]        ftrace_elem_r;
  logic [4:0]        ftrace_flags_r;
  logic              commit_pulse_r;
  logic [4:0]        commit_flags_r;
  logic [15:0]       commit_ctr_r;
  logic [15:0]       flush_ctr_r;
  logic [15:0]       spurious_ctr_r;
  logic [15:0]       inact_flag_ctr_r;
  logic [31:0]       fpu_issues_r;
  logic [7:0]        last_lat_r;

  // -------------------------------------------------------- derived classes
  logic [2:0]        plan_rm;
  fp_plan_t          plan_c;
  logic is_red_c;
  logic is_redwide_c;
  logic dst_mask_c;
  logic unary_c;
  logic binary_c;
  logic two_c;
  logic need_vs1_c;
  logic [3:0] op_count_c;
  logic legal_c;

  always_comb begin
    plan_rm = rm_eff_q;
    if (MutRmIgnore) begin
      plan_rm = 3'b000;                     // MUTANT: always round to nearest-even
    end
    plan_c = fp_plan(op_f_q, op_op_q, (sew_l_q == 3'd6), plan_rm);
  end

  always_comb begin
    is_red_c     = (op_f_q == F_REDSUM) || (op_f_q == F_REDMINMAX) ||
                   (op_f_q == F_REDWIDE);
    is_redwide_c = (op_f_q == F_REDWIDE);
    dst_mask_c   = (op_f_q == F_CMP);
    unary_c      = (op_f_q == F_CVT) || (op_f_q == F_WIDE) || (op_f_q == F_NARROW);
    binary_c     = (op_f_q == F_ELEM) || (op_f_q == F_MINMAX) ||
                   (op_f_q == F_SGNJ) || (op_f_q == F_CMP);
    two_c        = plan_c.two;
    need_vs1_c   = binary_c && (op_form_q == FORM_VV);
    op_count_c   = fp_op_count(op_f_q)[3:0];
  end

  always_comb begin
    legal_c = 1'b1;
    if (op_f_q >= 5'(NFAM)) begin
      legal_c = 1'b0;
    end else if (!caps_i[op_f_q[3:0]]) begin
      legal_c = 1'b0;
    end else if (op_op_q >= 4'(op_count_c)) begin
      legal_c = 1'b0;
    end else if (!(sew32 || sew64)) begin
      legal_c = 1'b0;                       // half precision is not implemented
    end else if ((1 << sew_l) > ELEN) begin
      legal_c = 1'b0;
    end else if ((op_f_q == F_WIDE) || (op_f_q == F_NARROW) ||
                 (op_f_q == F_REDWIDE)) begin
      // The effective element width is 2*SEW and the effective LMUL is 2*LMUL.
      if (!sew32) begin
        legal_c = 1'b0;
      end else if (lmul > 4'sd2) begin
        legal_c = 1'b0;
      end
    end
  end

  logic [2:0]        src_sew_l;
  logic signed [3:0] src_lmul;
  logic [2:0]        dst_sew_l;
  logic signed [3:0] dst_lmul;
  logic [2:0]        acc_sew_l;
  logic signed [3:0] acc_lmul;

  always_comb begin
    src_sew_l = sew_l_q;
    src_lmul  = lmul_q;
    dst_sew_l = sew_l_q;
    dst_lmul  = lmul_q;
    if (op_f_q == F_NARROW) begin
      src_sew_l = sew_l_q + 3'd1;
      src_lmul  = lmul_q + 4'sd1;
    end
    if ((op_f_q == F_WIDE) || (op_f_q == F_REDWIDE)) begin
      dst_sew_l = sew_l_q + 3'd1;
      dst_lmul  = lmul_q + 4'sd1;
    end
    if (op_f_q == F_REDWIDE) begin
      acc_sew_l = sew_l_q + 3'd1;
      acc_lmul  = lmul_q + 4'sd1;
    end else begin
      acc_sew_l = sew_l_q;
      acc_lmul  = lmul_q;
    end
  end

  logic [63:0] ones_c;
  always_comb begin
    if (dst_mask_c) begin
      ones_c = 64'd1;
    end else begin
      ones_c = wmask(1 << dst_sew_l);
    end
  end

  // --------------------------------------------------------- FPU operands
  logic [63:0] op_a_c, op_b_c, fpu_a_c, fpu_b_c;
  always_comb begin
    logic [63:0] vec_a;
    vec_a = (op_form_q == FORM_VV) ? a_q : op_scalar_q;
    if (unary_c) begin
      op_a_c = b_q;
      op_b_c = 64'd0;
    end else if (is_redwide_c) begin
      if (sub_q == 2'd0) begin
        op_a_c = b_q;                       // promote the SEW-wide element
        op_b_c = 64'd0;
      end else begin
        op_a_c = acc_q;                     // add it to the wide accumulator
        op_b_c = fpu_data_q;
      end
    end else if (is_red_c) begin
      op_a_c = acc_q;
      op_b_c = b_q;
    end else begin
      op_a_c = vec_a;
      op_b_c = b_q;
    end
    if (binary_c && plan_c.swap) begin
      fpu_a_c = op_b_c;
      fpu_b_c = op_a_c;
    end else begin
      fpu_a_c = op_a_c;
      fpu_b_c = op_b_c;
    end
  end

  logic [4:0]  fpu_op_c;
  logic        fpu_fmt_c;
  always_comb begin
    fpu_op_c  = plan_c.fop;
    fpu_fmt_c = plan_c.fmt;
    if (two_c && (sub_q == 2'd1)) begin
      fpu_op_c  = mosaic_pkg::FP_ADD;
      fpu_fmt_c = 1'b0;
    end
  end

  logic        fpu_req_c;
  logic        fpu_rsp_v;
  logic        fpu_rsp_ready;
  logic [63:0] fpu_rsp_data;
  logic [4:0]  fpu_rsp_flags;
  logic [7:0]  fpu_latency;
  logic        fpu_req_ready;

  assign fpu_req_c      = (state_q == S_FPU);
  assign fpu_rsp_ready  = 1'b1;

/* verilator lint_off PINCONNECTEMPTY */
  mosaic_fpu u_fpu (
      .clk_i           (clk_i),
      .rst_i           (rst_i),
      .req_valid_i     (fpu_req_c),
      .req_ready_o     (fpu_req_ready),
      .req_op_i        (mosaic_pkg::fp_op_e'(fpu_op_c)),
      .req_fmt_i       (fpu_fmt_c),
      .req_rm_i        (plan_c.rm),
      .req_iw_i        (plan_c.iw),
      .req_is_i        (plan_c.is),
      .req_a_i         (fpu_a_c),
      .req_b_i         (fpu_b_c),
      .req_rob_index_i ({VFP_ROB_W{1'b0}}),
      .req_rob_gen_i   ({VFP_GEN_W{1'b0}}),
      .req_uop_index_i (cur_q[VFP_UOP_W-1:0]),
      .flush_i         (flush_i),
      .res_valid_o     (fpu_rsp_v),
      .res_ready_i     (fpu_rsp_ready),
      .res_data_o      (fpu_rsp_data),
      .res_fflags_o    (fpu_rsp_flags),
      .res_rob_index_o (),
      .res_rob_gen_o   (),
      .res_uop_index_o (),
      .o_busy          (),
      .o_latency_o     (fpu_latency),
      .o_iter          (),
      .o_accepted_ctr  (),
      .o_completed_ctr (),
      .o_cancelled_ctr (),
      .o_killed_res_ctr()
  );
/* verilator lint_on PINCONNECTEMPTY */

  // The final result of the element: the FPU value, or the comparison's single
  // bit (inverted for vmfne), truncated to the destination element width.
  logic [63:0] res_value_c;
  always_comb begin
    if (dst_mask_c) begin
      res_value_c = {63'd0, fpu_rsp_data[0] ^ plan_c.invert};
    end else begin
      res_value_c = fpu_rsp_data;
    end
    res_value_c = res_value_c & wmask(1 << dst_sew_l);
  end

  // ------------------------------------------------------------ VRF demand
  always_comb begin
    vrf_rd_valid_o = 1'b0;
    vrf_rd_base_o  = 5'd0;
    vrf_rd_elem_o  = 7'd0;
    vrf_rd_sew_o   = 3'd0;
    vrf_rd_lmul_o  = 4'd0;
    vrf_rd_tag_o   = 16'd0;
    case (state_q)
      S_ACCRD: begin
        vrf_rd_valid_o = 1'b1;
        vrf_rd_base_o  = op_vs1_q;
        vrf_rd_elem_o  = 7'd0;
        vrf_rd_sew_o   = acc_sew_l;
        vrf_rd_lmul_o  = acc_lmul;
        vrf_rd_tag_o   = TAG_ACC;
      end
      S_MASKR: begin
        vrf_rd_valid_o = 1'b1;
        vrf_rd_base_o  = 5'd0;
        vrf_rd_elem_o  = {3'd0, cur_q[6:3]};
        vrf_rd_sew_o   = 3'd3;
        vrf_rd_lmul_o  = 4'sd0;
        vrf_rd_tag_o   = TAG_MASK;
      end
      S_VS2R: begin
        vrf_rd_valid_o = 1'b1;
        vrf_rd_base_o  = op_vs2_q;
        vrf_rd_elem_o  = cur_q[6:0];
        vrf_rd_sew_o   = src_sew_l;
        vrf_rd_lmul_o  = src_lmul;
        vrf_rd_tag_o   = TAG_VS2;
      end
      S_VS1R: begin
        vrf_rd_valid_o = 1'b1;
        vrf_rd_base_o  = op_vs1_q;
        vrf_rd_elem_o  = cur_q[6:0];
        vrf_rd_sew_o   = src_sew_l;
        vrf_rd_lmul_o  = src_lmul;
        vrf_rd_tag_o   = TAG_VS1;
      end
      S_DSTR: begin
        vrf_rd_valid_o = 1'b1;
        vrf_rd_base_o  = op_vd_q;
        vrf_rd_elem_o  = {3'd0, cur_q[6:3]};
        vrf_rd_sew_o   = 3'd3;
        vrf_rd_lmul_o  = 4'sd0;
        vrf_rd_tag_o   = TAG_DST;
      end
      default: begin
        vrf_rd_valid_o = 1'b0;
      end
    endcase
  end

  // ------------------------------------------------------------ VRF write
  always_comb begin
    vrf_wr_valid_o = 1'b0;
    vrf_wr_base_o  = 5'd0;
    vrf_wr_elem_o  = 7'd0;
    vrf_wr_sew_o   = 3'd0;
    vrf_wr_lmul_o  = 4'd0;
    vrf_wr_data_o  = 64'd0;
    case (state_q)
      S_WR: begin
        vrf_wr_valid_o = 1'b1;
        vrf_wr_base_o  = op_vd_q;
        vrf_wr_elem_o  = cur_q[6:0];
        vrf_wr_sew_o   = dst_sew_l;
        vrf_wr_lmul_o  = dst_lmul;
        vrf_wr_data_o  = result_q & wmask(1 << dst_sew_l);
      end
      S_MWR: begin
        vrf_wr_valid_o = 1'b1;
        vrf_wr_base_o  = op_vd_q;
        vrf_wr_elem_o  = {3'd0, cur_q[6:3]};
        vrf_wr_sew_o   = 3'd3;
        vrf_wr_lmul_o  = 4'sd0;
        vrf_wr_data_o  = {56'd0, byte_new_q};
      end
      S_RWR: begin
        vrf_wr_valid_o = 1'b1;
        vrf_wr_base_o  = op_vd_q;
        vrf_wr_elem_o  = 7'd0;
        vrf_wr_sew_o   = dst_sew_l;
        vrf_wr_lmul_o  = dst_lmul;
        vrf_wr_data_o  = acc_q & wmask(1 << dst_sew_l);
      end
      default: begin
        vrf_wr_valid_o = 1'b0;
      end
    endcase
  end

  // ------------------------------------------------------------ the engine
  logic        enter_done;
  logic        fpu_final;      // this response is the element's last sub-op
  logic        merge_ok;
  logic        vlmax_ok;
  logic [7:0]  dst_n_w;
  logic [31:0] vlmax_shift;

  assign enter_done = (state_q == S_DONE);
  assign fpu_final  = fpu_rsp_v && !(two_c && (sub_q == 2'd0));
  assign merge_ok   = active_q || MutInactiveFlag;
  assign vlmax_ok   = (int'(VFP_VLEN_LOG2) + int'(lmul_q) - int'(sew_l_q)) >= 0;
  assign vlmax_shift = 32'(int'(VFP_VLEN_LOG2) + int'(lmul_q) - int'(sew_l_q));
  assign dst_n_w    = vlmax_ok ? 8'(32'd1 << vlmax_shift) : 8'd0;

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      state_q        <= S_IDLE;
      op_f_q         <= 5'd0;
      op_op_q        <= 4'd0;
      op_form_q      <= 2'd0;
      op_vd_q        <= 5'd0;
      op_vs1_q       <= 5'd0;
      op_vs2_q       <= 5'd0;
      op_scalar_q    <= 64'd0;
      masken_q       <= 1'b0;
      sew_l_q        <= 3'd0;
      lmul_q         <= 4'sd0;
      vta_q          <= 1'b0;
      vma_q          <= 1'b0;
      vl_q           <= 8'd0;
      vstart_q       <= 7'd0;
      rm_eff_q       <= 3'd0;
      dst_n_q        <= 8'd0;
      cur_q          <= 8'd0;
      rcount_q       <= 8'd0;
      mbit_q         <= 1'b0;
      active_q       <= 1'b0;
      sub_q          <= 2'd0;
      a_q            <= 64'd0;
      b_q            <= 64'd0;
      acc_q          <= 64'd0;
      result_q       <= 64'd0;
      byte_new_q     <= 8'd0;
      fpu_data_q     <= 64'd0;
      dbyte_q        <= 8'd0;
      elems_r        <= 8'd0;
      writes_r       <= 8'd0;
      done_r         <= 1'b0;
      illegal_r      <= 1'b0;
      pending_q      <= 5'd0;
      arch_q         <= 5'd0;
      macro_pending_q <= 1'b0;
      ftrace_v_r     <= 1'b0;
      ftrace_elem_r  <= 8'd0;
      ftrace_flags_r <= 5'd0;
      commit_pulse_r <= 1'b0;
      commit_flags_r <= 5'd0;
      commit_ctr_r   <= 16'd0;
      flush_ctr_r    <= 16'd0;
      spurious_ctr_r <= 16'd0;
      inact_flag_ctr_r <= 16'd0;
      fpu_issues_r   <= 32'd0;
      last_lat_r     <= 8'd0;
    end else begin
      // single-cycle pulses
      done_r        <= 1'b0;
      commit_pulse_r <= 1'b0;
      ftrace_v_r    <= 1'b0;

      // ---------------------------------------------- response capture
      if (vrf_rd_rsp_valid_i) begin
        case (vrf_rd_rsp_tag_i)
          TAG_ACC: begin acc_q  <= vrf_rd_rsp_data_i; end
          TAG_MASK: begin mbit_q <= vrf_rd_rsp_data_i[{3'b000, cur_q[2:0]}]; end
          TAG_VS2: begin b_q    <= vrf_rd_rsp_data_i; end
          TAG_VS1: begin a_q    <= vrf_rd_rsp_data_i; end
          TAG_DST: begin dbyte_q <= vrf_rd_rsp_data_i[7:0]; end
          default: begin b_q    <= vrf_rd_rsp_data_i; end
        endcase
      end
      if (fpu_rsp_v) begin
        fpu_data_q  <= fpu_rsp_data;
        // `o_latency_o` is the in-flight operation's declared latency; it is
        // valid while the operation is in flight (the FPU zeroes it in IDLE),
        // so it is captured here rather than at accept.
        last_lat_r  <= fpu_latency;
      end
      if (fpu_req_c && fpu_req_ready) begin
        fpu_issues_r <= fpu_issues_r + 32'd1;
      end

      // ------------------------------------------------------- the FSM
      case (state_q)
        S_IDLE: begin
          if (exec_valid_i) begin
            op_f_q      <= exec_family_i;
            op_op_q     <= exec_op_i;
            op_form_q   <= exec_form_i;
            op_vd_q     <= exec_vd_i;
            op_vs1_q    <= exec_vs1_i;
            op_vs2_q    <= exec_vs2_i;
            op_scalar_q <= exec_scalar_i;
            masken_q    <= exec_mask_en_i;
            sew_l_q     <= sew_l;
            lmul_q      <= lmul;
            vta_q       <= vta;
            vma_q       <= vma;
            vl_q        <= cfg_vl_i;
            vstart_q    <= cfg_vstart_i;
            rm_eff_q    <= (exec_rm_i == 3'b111) ? cfg_frm_i : exec_rm_i;
            illegal_r   <= 1'b0;
            elems_r     <= 8'd0;
            writes_r    <= 8'd0;
            sub_q       <= 2'd0;
            pending_q   <= 5'd0;
            macro_pending_q <= 1'b0;
            state_q     <= S_SETUP;
          end
        end

        S_SETUP: begin
          if (!legal_c) begin
            illegal_r <= 1'b1;
            state_q   <= S_DONE;
          end else begin
            cur_q    <= (MutReduceReassoc && is_red_c) ? (vl_q - 8'd1)
                                                       : {1'b0, vstart_q};
            sub_q    <= 2'd0;
            acc_q    <= 64'd0;
            if (is_red_c) begin
              rcount_q <= (vl_q > {1'b0, vstart_q}) ? (vl_q - {1'b0, vstart_q}) : 8'd0;
              state_q  <= S_ACCRD;
            end else begin
              dst_n_q <= dst_n_w;
              state_q <= S_ELEM;
            end
          end
        end

        S_ACCRD: begin
          if (vrf_rd_gnt_i) begin
            state_q <= S_ACCW;
          end
        end

        S_ACCW: begin
          if (vrf_rd_rsp_valid_i) begin
            state_q <= S_ELEM;
          end
        end

        S_ELEM: begin
          if (is_red_c) begin
            if (rcount_q == 8'd0) begin
              state_q <= S_FINAL;
            end else if (masken_q) begin
              state_q <= S_MASKR;
            end else begin
              active_q <= 1'b1;
              sub_q    <= 2'd0;
              state_q  <= S_VS2R;
            end
          end else if (cur_q >= dst_n_q) begin
            state_q <= S_FINAL;
          end else if (cur_q >= vl_q) begin
            // tail element
            if (vta_q) begin
              active_q <= 1'b0;
              result_q <= ones_c;
              state_q  <= dst_mask_c ? S_DSTR : S_WR;
            end else begin
              state_q <= S_NEXT;
            end
          end else if (masken_q) begin
            state_q <= S_MASKR;
          end else begin
            active_q <= 1'b1;
            sub_q    <= 2'd0;
            state_q  <= S_VS2R;
          end
        end

        S_MASKR: begin
          if (vrf_rd_gnt_i) begin
            state_q <= S_MASKW;
          end
        end

        S_MASKW: begin
          if (vrf_rd_rsp_valid_i) begin
            state_q <= S_PLAN;
          end
        end

        S_PLAN: begin
          if (mbit_q) begin
            active_q <= 1'b1;
            sub_q    <= 2'd0;
            state_q  <= S_VS2R;
          end else begin
            active_q <= 1'b0;
`ifdef MOSAIC_VEC_FP_MUTANT_INACTIVE_FLAG
            // MUTANT: the masked-off element takes the compute path, so its
            // flags are merged even though it is not active.
            sub_q   <= 2'd0;
            state_q <= S_VS2R;
`else
            if (is_red_c) begin
              state_q <= S_NEXT;
            end else if (vma_q) begin
              result_q <= ones_c;
              state_q  <= dst_mask_c ? S_DSTR : S_WR;
            end else begin
              state_q <= S_NEXT;
            end
`endif
          end
        end

        S_VS2R: begin
          if (vrf_rd_gnt_i) begin
            state_q <= S_VS2W;
          end
        end

        S_VS2W: begin
          if (vrf_rd_rsp_valid_i) begin
            state_q <= need_vs1_c ? S_VS1R : S_FPU;
          end
        end

        S_VS1R: begin
          if (vrf_rd_gnt_i) begin
            state_q <= S_VS1W;
          end
        end

        S_VS1W: begin
          if (vrf_rd_rsp_valid_i) begin
            state_q <= S_FPU;
          end
        end

        S_FPU: begin
          if (fpu_req_ready) begin
            state_q <= S_FPUW;
          end
        end

        S_FPUW: begin
          if (fpu_rsp_v) begin
            if (!fpu_final) begin
              sub_q   <= 2'd1;
              state_q <= S_FPU;
            end else begin
              if (merge_ok) begin
                pending_q      <= pending_q | fpu_rsp_flags;
                ftrace_v_r     <= 1'b1;
                ftrace_elem_r  <= cur_q;
                ftrace_flags_r <= fpu_rsp_flags;
                if (!active_q && (fpu_rsp_flags != 5'd0)) begin
                  inact_flag_ctr_r <= inact_flag_ctr_r + 16'd1;
                end
              end
              if (is_red_c) begin
                acc_q <= res_value_c;
                if (active_q) begin
                  elems_r <= elems_r + 8'd1;
                end
                state_q <= S_NEXT;
              end else begin
                result_q <= res_value_c;
                if (active_q) begin
                  elems_r <= elems_r + 8'd1;
                end
                state_q <= dst_mask_c ? S_DSTR : S_WR;
              end
            end
          end
        end

        S_DSTR: begin
          if (vrf_rd_gnt_i) begin
            state_q <= S_DSTW;
          end
        end

        S_DSTW: begin
          if (vrf_rd_rsp_valid_i) begin
            state_q <= S_DSTX;
          end
        end

        S_DSTX: begin
          byte_new_q <= (dbyte_q & ~(8'd1 << cur_q[2:0])) |
                        (result_q[0] ? (8'd1 << cur_q[2:0]) : 8'd0);
          state_q    <= S_MWR;
        end

        S_WR: begin
          if (vrf_wr_gnt_i) begin
            writes_r <= writes_r + 8'd1;
            state_q  <= S_NEXT;
          end
        end

        S_MWR: begin
          if (vrf_wr_gnt_i) begin
            writes_r <= writes_r + 8'd1;
            state_q  <= S_NEXT;
          end
        end

        S_RWR: begin
          if (vrf_wr_gnt_i) begin
            writes_r <= writes_r + 8'd1;
            state_q  <= S_DONE;
          end
        end

        S_NEXT: begin
          if (is_red_c) begin
            if (rcount_q <= 8'd1) begin
              rcount_q <= 8'd0;
              state_q  <= S_FINAL;
            end else begin
              rcount_q <= rcount_q - 8'd1;
`ifdef MOSAIC_VEC_FP_MUTANT_REDUCE_REASSOC
              // MUTANT: fold in descending element order.
              cur_q   <= cur_q - 8'd1;
`else
              cur_q   <= cur_q + 8'd1;
`endif
              state_q <= S_ELEM;
            end
          end else begin
            if ((cur_q + 8'd1) >= dst_n_q) begin
              state_q <= S_FINAL;
            end else begin
              cur_q   <= cur_q + 8'd1;
              state_q <= S_ELEM;
            end
          end
        end

        S_FINAL: begin
          if (is_red_c) begin
            state_q <= S_RWR;
          end else begin
            state_q <= S_DONE;
          end
        end

        S_DONE: begin
          done_r  <= 1'b1;
          state_q <= S_IDLE;
        end

        default: begin
          state_q <= S_IDLE;
        end
      endcase

      // ------------------------------------------- the flag commit boundary
      if (flush_i) begin
        pending_q       <= 5'd0;
        macro_pending_q <= 1'b0;
        flush_ctr_r     <= flush_ctr_r + 16'd1;
      end else begin
        if (enter_done) begin
          macro_pending_q <= 1'b1;
          if (MutEarlyCommit) begin
            // MUTANT: the macro's flags become architectural at completion.
            arch_q <= arch_q | pending_q;
          end
        end
        if (commit_valid_i) begin
          if (macro_pending_q) begin
            arch_q         <= arch_q | pending_q;
            commit_pulse_r <= 1'b1;
            commit_flags_r <= pending_q;
            commit_ctr_r   <= commit_ctr_r + 16'd1;
            macro_pending_q <= 1'b0;
            pending_q      <= 5'd0;
          end else begin
            spurious_ctr_r <= spurious_ctr_r + 16'd1;
          end
        end
      end
    end
  end

  // ---------------------------------------------------------------- outputs
  assign exec_busy_o            = (state_q != S_IDLE);
  assign exec_done_o            = done_r;
  assign exec_illegal_o         = illegal_r;
  assign exec_trap_o            = 1'b0;
  assign exec_trap_elem_o       = 7'd0;
  assign exec_elems_o           = elems_r;
  assign exec_cur_o             = cur_q;
  assign exec_acc_o             = acc_q;
  assign exec_writes_o          = writes_r;
  assign o_fflags_pending_o     = pending_q;
  assign o_fflags_arch_o        = arch_q;
  assign o_commit_o             = commit_pulse_r;
  assign o_commit_fflags_o      = commit_flags_r;
  assign o_commit_ctr_o         = commit_ctr_r;
  assign o_flush_ctr_o          = flush_ctr_r;
  assign o_spurious_ctr_o       = spurious_ctr_r;
  assign o_inactive_flag_ctr_o  = inact_flag_ctr_r;
  assign o_macro_pending_o      = macro_pending_q;
  assign o_flag_trace_valid_o   = ftrace_v_r;
  assign o_flag_trace_elem_o    = ftrace_elem_r;
  assign o_flag_trace_flags_o   = ftrace_flags_r;
  assign o_fpu_issues_o         = fpu_issues_r;
  assign o_fpu_latency_o        = fpu_latency;
  assign o_last_latency_o       = last_lat_r;

endmodule : mosaic_vec_fp

`resetall
`default_nettype wire
