// ============================================================================
// mosaic_vec_alu -- the vector integer / mask / permute / reduction lane
// (work package I-054, CASE=rvv.integer_mask_permute).
//
// The unit executes one vector *packet*: it walks the destination elements of
// one operation, reads the source elements it needs from the banked VRF
// (`mosaic_vrf`, I-053) through a single read slot, and writes the destination
// elements back through a single write slot. Its configuration is not its own:
// `cfg_vtype_i` / `cfg_vl_i` / `cfg_vstart_i` / `cfg_vxrm_i` are the snapshot
// `mosaic_vec_cfg` (I-052) hands out, and `caps_i` is the profile's declared
// family set. Nothing here re-derives SEW/LMUL from anywhere else.
//
// ---------------------------------------------------------------- the rules
//
// Element indexing. Element `i` of a destination register group is what the
// operation names as `vd[i]`. The VRF owns the (register, element, SEW, LMUL)
// -> bank/row mapping and group alignment, so this unit never computes a
// physical address: it names a base register and an element index and the VRF
// resolves them. That is the I-053 contract, reused rather than duplicated.
//
// Mask and tail policy (`vma` = vtype[7], `vta` = vtype[6]), stated once:
//
//     element i is *active*  iff  vstart <= i < vl  AND  (v0.mask[i] or the
//                                instruction is unmasked)
//     an active destination element              is written with the result;
//     an inactive element with vma/vta == 0      is unchanged;
//     an inactive element with vma/vta == 1      is written with all-ones
//                                (masked-off elements use vma, tail elements
//                                i >= vl use vta);
//     an element below vstart is unchanged regardless of vma/vta.
//
// An inactive element performs **no source access**: the engine never issues a
// source read for it, so neither a trap nor a register-file access can follow
// from a masked-off element. `o_src_rd_ctr`, the progress trace and the VRF's
// own read/bad counters are what a case observes.
//
// Reduction ordering, stated once and checked: a reduction folds the source
// elements in **strictly ascending element index order**, starting at vstart,
// over active elements only, with `vs1[0]` as the initial accumulator. There is
// no tree, no reassociation and no parked partial accumulator; the running
// accumulator is exposed on `o_acc` and each folded element index on the
// progress trace (`o_trace_valid` / `o_trace_elem`), so a case can require the
// sequence and not merely the sum -- integer addition is associative, so the
// order is observable only through the sequence, and stating the ordering is
// what the plan asks for. Widening reductions collect into a 2*SEW
// accumulator, and masked-off elements are not read at all.
//
// Capability gating. `caps_i` has one bit per operation family. A family whose
// bit is clear is **refused**: `o_illegal` asserts and the packet issues no VRF
// transaction, so a machine that has not implemented (and so may not advertise)
// a family cannot execute it. `o_illegal` is also set for an unknown family, or
// for a widening/narrowing form whose effective element width would leave the
// supported [8, ELEN] range.
//
// ------------------------------------------------------------- the families
//
// Implemented here (NFAM = 17 bits, the capability word):
//
//   0  ADDSUB     vadd, vsub, vrsub
//   1  WIDE       vwaddu, vwadd, vwsubu, vwsub            (dst 2*SEW)
//   2  MUL        vmul, vmulh, vmulhu, vmulhsu
//   3  MULW       vwmulu, vwmulsu, vwmul                 (dst 2*SEW)
//   4  SHIFT      vsll, vsrl, vsra
//   5  NARROW     vnsrl, vnsra, vnclipu, vnclip          (src 2*SEW)
//   6  LOGIC      vand, vor, vxor, vnot
//   7  MINMAX     vminu, vmin, vmaxu, vmax
//   8  CMP        vmseq, vmsne, vmsltu, vmslt, vmsleu, vmsle, vmsgtu, vmsgt
//   9  SAT        vsaddu, vsadd, vssubu, vssub, vaaddu, vaadd, vasubu, vasub
//  10  MASKLOG    vmand, vmnand, vmor, vmnor, vmxor, vmxnor, vmandn, vmorn
//  11  MASKPFX    vmsbf, vmsif, vmsof
//  12  SLIDE      vslideup, vslidedown, vslide1up, vslide1down
//  13  GATHER     vrgather (vv and vx/vi)
//  14  COMPRESS   vcompress
//  15  REDUCE     vredsum, vredmaxu, vredmax, vredminu, vredmin, vredand,
//                 vredor, vredxor
//  16  REDWIDE    vwredsumu, vwredsum                     (acc 2*SEW)
//
// Deliberately NOT implemented, and therefore not claimable: integer divide
// (`vdiv[u]`, `vrem[u]`), the multiply-accumulate forms (`vmacc`, `vnmsac`,
// `vwmacc*`), scaling shifts (`vssrl`/`vssra`), `vsmul`, integer extension
// (`vzext`/`vsext`), `viota`, `vid`, `vrgatherei16`, the `vmerge`/`vmv*` moves,
// and every vector memory, FP and AMO class. The capability word has no bit for
// them, so they cannot be advertised: a family that is not finished does not
// enter the gate.
//
// Rounding (`vxrm`) comes from the configuration snapshot and follows the
// spec's table: rnu adds v[d-1]; rne adds v[d-1] & (v[d-2:0] != 0 | v[d]); rdn
// adds 0; rod adds !v[d] & (v[d-1:0] != 0). Saturation clamps at the signed or
// unsigned boundary and reports through `o_sat`; the value is never wrapped.
//
// ------------------------------------------------------------------ mutants
//
// Five `-DMOSAIC_VEC_ALU_MUTANT_*` defines inject one defect each; the shipping
// build defines none. Each is proven to fail CASE=rvv.integer_mask_permute in
// results/reports/I-054-vector-integer.md:
//
//   SAT_WRAP      a saturating add/sub/clip wraps instead of clamping
//   WIDE_WIDTH    a widening op drops the signedness of its source operands
//   PERMUTE_ORDER a permute orders elements by LMUL instead of by index
//   MASKED_ACCESS a masked-off element still issues its source access
//   REDUCE_ORDER  a reduction folds the source elements in descending order
//
// Three more inject a defect in the mask-prefix `vstart` rule (I-057); each is
// proven to fail CASE=rvv.mask_prefix_vstart in
// results/reports/rvv-mask-prefix-vstart.md:
//
//   MASKPFX_NO_TRAP     vmsbf/vmsif/vmsof never raise on a non-zero vstart
//   MASKPFX_TRAP_VSTART0 the rule fires even at vstart == 0
//   MASKPFX_WRONG_OP    vmsof is exempted, so only two of the three raise
//
// Three more inject a defect in the mask-prefix *semantics* proper
// (CASE=rvv.mask_prefix_semantics); each is proven to fail it in
// results/reports/rvv-mask-prefix-semantics.md:
//
//   MASKPFX_SAME_EXPR    vmsbf and vmsif share set-including-first again
//   MASKPFX_ALLZERO      an all-zero active source is cleared, not all-ones
//   MASKPFX_POS0         vmsbf sets a bit before a first set bit at position 0
// ============================================================================

`default_nettype none
`resetall

module mosaic_vec_alu #(
    parameter int unsigned VLEN = 128,
    parameter int unsigned ELEN = 64,
    parameter int unsigned NFAM = 17
) (
    input  logic                    clk_i,
    input  logic                    rst_i,

    // -------------------------------------------------- capability gate
    input  logic [NFAM-1:0]         caps_i,

    // --------------------------------- configuration snapshot (from I-052)
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [63:0]             cfg_vtype_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [7:0]              cfg_vl_i,
    input  logic [6:0]              cfg_vstart_i,
    input  logic [1:0]              cfg_vxrm_i,

    // ------------------------------------------------ direct element lane
    // One destination element, computed combinationally. The packet engine
    // below is built from exactly this lane; the ports exist so a case can
    // drive boundary operands without a register file in the way.
    input  logic                    e_valid_i,
    input  logic [4:0]              e_family_i,
    input  logic [3:0]              e_op_i,
    input  logic [1:0]              e_form_i,
    input  logic [63:0]             e_vs2_i,
    input  logic [63:0]             e_vs1_i,
    input  logic [63:0]             e_acc_i,
    input  logic                    e_mask_i,
    input  logic [63:0]             e_scalar_i,
    input  logic [7:0]              e_index_i,
    input  logic                    e_pfx_i,
    output logic [63:0]             e_result_o,
    output logic                    e_mres_o,
    output logic                    e_sat_o,
    output logic                    e_illegal_o,
    output logic                    e_trap_o,
    output logic                    e_access_o,
    output logic                    e_write_o,
    output logic [7:0]              e_rd2_o,
    output logic                    e_pfx_o,

    // --------------------------------------------------------- packet exec
    input  logic                    exec_valid_i,
    input  logic [4:0]              exec_family_i,
    input  logic [3:0]              exec_op_i,
    input  logic [1:0]              exec_form_i,
    input  logic [4:0]              exec_vd_i,
    input  logic [4:0]              exec_vs1_i,
    input  logic [4:0]              exec_vs2_i,
    input  logic [63:0]             exec_scalar_i,
    input  logic                    exec_mask_en_i,
    output logic                    exec_busy_o,
    output logic                    exec_done_o,
    output logic                    exec_illegal_o,
    output logic                    exec_trap_o,
    output logic [6:0]              exec_trap_elem_o,
    output logic                    exec_sat_o,
    output logic [7:0]              exec_elems_o,
    output logic [7:0]              exec_cur_o,
    output logic [63:0]             exec_acc_o,
    output logic                    exec_trace_valid_o,
    output logic [7:0]              exec_trace_elem_o,
    output logic [31:0]             exec_src_rd_ctr_o,

    // -------------------------------------------------- VRF read / write
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

    output logic                    vrf_wr_valid_o,
    output logic [4:0]              vrf_wr_base_o,
    output logic [6:0]              vrf_wr_elem_o,
    output logic [2:0]              vrf_wr_sew_o,
    output logic [3:0]              vrf_wr_lmul_o,
    output logic [63:0]             vrf_wr_data_o,
    input  logic                    vrf_wr_gnt_i
);

  // ------------------------------------------------------------------ family
  localparam logic [4:0] F_ADDSUB   = 5'd0;
  localparam logic [4:0] F_WIDE     = 5'd1;
  localparam logic [4:0] F_MUL      = 5'd2;
  localparam logic [4:0] F_MULW     = 5'd3;
  localparam logic [4:0] F_SHIFT    = 5'd4;
  localparam logic [4:0] F_NARROW   = 5'd5;
  localparam logic [4:0] F_LOGIC    = 5'd6;
  localparam logic [4:0] F_MINMAX   = 5'd7;
  localparam logic [4:0] F_CMP      = 5'd8;
  localparam logic [4:0] F_SAT      = 5'd9;
  localparam logic [4:0] F_MASKLOG  = 5'd10;
  localparam logic [4:0] F_MASKPFX  = 5'd11;
  localparam logic [4:0] F_SLIDE    = 5'd12;
  localparam logic [4:0] F_GATHER   = 5'd13;
  localparam logic [4:0] F_COMPRESS = 5'd14;
  localparam logic [4:0] F_REDUCE   = 5'd15;
  localparam logic [4:0] F_REDWIDE  = 5'd16;

  localparam logic [1:0] FORM_VV = 2'd0;
  localparam logic [1:0] FORM_VX = 2'd1;
  localparam logic [1:0] FORM_VI = 2'd2;

  localparam logic [1:0] DST_ELEM   = 2'd0;
  localparam logic [1:0] DST_MASK   = 2'd1;
  localparam logic [1:0] DST_SCALAR = 2'd2;

  localparam logic [2:0] S_IDLE  = 3'd0;
  localparam logic [2:0] S_SETUP = 3'd1;
  localparam logic [2:0] S_ELEM  = 3'd2;
  localparam logic [2:0] S_FINAL = 3'd3;
  localparam logic [2:0] S_DONE  = 3'd4;

  localparam logic [3:0] E_MASK    = 4'd0;
  localparam logic [3:0] E_MASKW   = 4'd1;
  localparam logic [3:0] E_PLAN    = 4'd2;
  localparam logic [3:0] E_VS1     = 4'd3;
  localparam logic [3:0] E_VS1W    = 4'd4;
  localparam logic [3:0] E_VS2OUT  = 4'd5;
  localparam logic [3:0] E_VS2W    = 4'd6;
  localparam logic [3:0] E_EXEC    = 4'd7;
  localparam logic [3:0] E_DSTRD   = 4'd8;
  localparam logic [3:0] E_DSTRDW  = 4'd9;
  localparam logic [3:0] E_DSTEXEC = 4'd10;
  localparam logic [3:0] E_WR      = 4'd11;
  localparam logic [3:0] E_NEXT    = 4'd12;

  localparam logic [15:0] TAG_MASK = 16'h3000;
  localparam logic [15:0] TAG_SRC1 = 16'h1000;
  localparam logic [15:0] TAG_SRC2 = 16'h2000;
  localparam logic [15:0] TAG_DST  = 16'h4000;

  // ------------------------------------------------------------------ helpers
  function automatic logic [127:0] width_mask(input int w);
    begin
      if (w >= 128) begin
        width_mask = {128{1'b1}};
      end else begin
        width_mask = (128'd1 << w) - 128'd1;
      end
    end
  endfunction

  function automatic logic [127:0] uext(input logic [63:0] v, input int w);
    begin
      uext = ({64'd0, v}) & width_mask(w);
    end
  endfunction

  function automatic logic signed [127:0] sext(input logic [63:0] v, input int w);
    logic [127:0] t;
    begin
      t = {64'd0, v};
      if (w < 128) begin
        t = t << (128 - w);
      end
      sext = $signed(t) >>> (128 - w);
    end
  endfunction

  // The spec's `(v >> d) + r`, with `r` from the vxrm table.
  function automatic logic [127:0] rnd(input logic [127:0] v, input int d,
                                       input logic sgn, input logic [1:0] rm);
    logic [127:0] base;
    logic [127:0] below;
    logic [127:0] below2;
    logic [127:0] r;
    logic         vd;
    logic         vdm1;
    begin
      if (d <= 0) begin
        rnd = v;
      end else begin
        base   = sgn ? $unsigned($signed(v) >>> d) : (v >> d);
        below  = v & width_mask(d);
        below2 = (d >= 2) ? (v & width_mask(d - 1)) : 128'd0;
        vd     = (d <= 127) ? v[d] : 1'b0;
        vdm1   = (d >= 1) ? v[d-1] : 1'b0;
        case (rm)
          2'd0:    r = {127'd0, vdm1};
          2'd1:    r = {127'd0, vdm1} &
                         ((below2 != 128'd0) ? 128'd1 : {127'd0, vd});
          2'd2:    r = 128'd0;
          default: r = {127'd0, ~vd} & ((below != 128'd0) ? 128'd1 : 128'd0);
        endcase
        rnd = base + r;
      end
    end
  endfunction

  // Sign-extend the low `w` bits of `v` to 64 bits.
  function automatic logic [63:0] sext64(input logic [63:0] v, input int w);
    logic [63:0] msk;
    begin
      if (w <= 0) begin
        sext64 = 64'd0;
      end else if (w >= 64) begin
        sext64 = v;
      end else begin
        msk    = (64'd1 << w) - 64'd1;
        sext64 = (v & msk) | (((v >> (w - 1)) & 64'd1) != 64'd0 ? ~msk : 64'd0);
      end
    end
  endfunction

  function automatic logic is_mask_family(input logic [4:0] f);
    begin
      is_mask_family = (f == F_CMP) || (f == F_MASKLOG) || (f == F_MASKPFX);
    end
  endfunction

  // ------------------------------------------------------- configuration
  logic [2:0]        sew_l;
  logic signed [3:0] lmul;
  logic              vta;
  logic              vma;
  int                sew;
  int                vlmax;
  int                vlmax_log;
  localparam int unsigned VLEN_LOG2 = $clog2(VLEN);
  localparam int unsigned ELEN_HALF = ELEN / 2;

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
    vta = cfg_vtype_i[6];
    vma = cfg_vtype_i[7];
    sew = 1 << sew_l;
    vlmax_log = int'(VLEN_LOG2) + int'(lmul) - int'(sew_l);
    if (vlmax_log >= 0) begin
      vlmax = 1 << vlmax_log;
    end else begin
      vlmax = 0;
    end
  end

  // ------------------------------------------------------------- engine state
  logic [2:0]  state_q;
  logic [3:0]  step_q;
  logic [4:0]  op_f_q;
  logic [3:0]  op_op_q;
  logic [1:0]  op_form_q;
  logic [4:0]  op_vd_q;
  logic [4:0]  op_vs1_q;
  logic [4:0]  op_vs2_q;
  logic [63:0] op_scalar_q;
  logic [1:0]  op_vxrm_q;
  logic        masken_q;

  logic [63:0] a_q;
  logic [63:0] b_q;
  logic [7:0]  dbyte_q;
  logic [63:0] dwdata_q;
  logic        mbit_q;
  logic        mres_q;    // the destination mask bit, latched at E_EXEC
  logic [63:0] acc_q;
  logic        pfx_q;
  logic [7:0]  cur_q;
  logic [7:0]  count_q;
  logic [7:0]  dst_n_q;
  logic [7:0]  rcount_q;
  logic        redup_q;
  logic        redinit_q;
  logic        redfin_q;
  logic        fill_q;

  logic        busy_r;
  logic        done_r;
  logic        illegal_r;
  logic        trap_r;
  logic [6:0]  trap_elem_r;
  logic        sat_r;
  logic [7:0]  elems_r;
  logic [31:0] srccnt_r;

  logic        eng_active;
  assign eng_active = (state_q != S_IDLE);

  logic        is_red;
  logic        is_redwide;
  logic        is_compress;
  assign is_red      = (op_f_q == F_REDUCE) || (op_f_q == F_REDWIDE);
  assign is_redwide  = (op_f_q == F_REDWIDE);
  assign is_compress = (op_f_q == F_COMPRESS);

  // --------------------------------------------------- operand source select
  logic [4:0]  ef_family;
  logic [3:0]  ef_op;
  logic [1:0]  ef_form;
  logic [63:0] ef_vs2;
  logic [63:0] ef_vs1;
  logic [63:0] ef_acc;
  logic [63:0] ef_scalar;
  logic        ef_mask;
  logic [7:0]  ef_index;
  logic        ef_pfx;
  logic [1:0]  ef_vxrm;

  always_comb begin
    if (eng_active) begin
      ef_family = op_f_q;
      ef_op     = op_op_q;
      ef_form   = op_form_q;
      ef_vs2    = b_q;
      ef_vs1    = a_q;
      ef_acc    = acc_q;
      ef_scalar = op_scalar_q;
      ef_mask   = mbit_q;
      ef_index  = cur_q;
      ef_pfx    = pfx_q;
      ef_vxrm   = op_vxrm_q;
    end else begin
      ef_family = e_family_i;
      ef_op     = e_op_i;
      ef_form   = e_form_i;
      ef_vs2    = e_vs2_i;
      ef_vs1    = e_vs1_i;
      ef_acc    = e_acc_i;
      ef_scalar = e_scalar_i;
      ef_mask   = e_mask_i;
      ef_index  = e_index_i;
      ef_pfx    = e_pfx_i;
      ef_vxrm   = cfg_vxrm_i;
    end
  end

  // ------------------------------------------------- mask-prefix vstart rule
  // `vmsbf`/`vmsif`/`vmsof` cannot be restarted from an element boundary: they
  // are the three instructions the pinned specification makes an
  // illegal-instruction exception when `vstart` is non-zero (v-spec.adoc,
  // "Traps on `vmsbf.m`/`vmsif.m`/`vmsof.m` are always reported with a
  // `vstart` of 0 ... will raise an illegal-instruction exception if `vstart`
  // is non-zero"). The rule is a property of the whole instruction, so it is
  // decided before any element is touched: the destination is left unchanged
  // and no source element is read. `vstart == 0` executes normally.
  logic maskpfx_vstart_illegal;
  always_comb begin
    maskpfx_vstart_illegal = (ef_family == F_MASKPFX) && (cfg_vstart_i != 7'd0);
`ifdef MOSAIC_VEC_ALU_MUTANT_MASKPFX_NO_TRAP
    // NEGATIVE CONTROL: the rule is not implemented at all.
    maskpfx_vstart_illegal = 1'b0;
`endif
`ifdef MOSAIC_VEC_ALU_MUTANT_MASKPFX_TRAP_VSTART0
    // NEGATIVE CONTROL: the rule fires even at vstart == 0.
    maskpfx_vstart_illegal = (ef_family == F_MASKPFX);
`endif
`ifdef MOSAIC_VEC_ALU_MUTANT_MASKPFX_WRONG_OP
    // NEGATIVE CONTROL: vmsof (op 2) is exempted, so only vmsbf/vmsif raise.
    maskpfx_vstart_illegal =
        (ef_family == F_MASKPFX) && (ef_op != 4'd2) && (cfg_vstart_i != 7'd0);
`endif
  end

  // ------------------------------------------------------------ element lane
  logic [63:0] sel_result;
  logic        sel_mres;
  logic        sel_sat;
  logic        sel_illegal;
  logic        sel_trap;
  logic        sel_access;
  logic        sel_write;
  logic [7:0]  sel_rd2;
  logic        sel_pfx;
  logic        sel_active;

  always_comb begin
    logic [127:0] m;
    logic [127:0] m2;
    logic [127:0] v128;
    logic [127:0] hi128;
    logic [127:0] lo128;
    logic signed [127:0] s128;
    logic [63:0]  aa;
    logic [63:0]  bb;
    logic [63:0]  zz;
    logic [5:0]   bidx;
    logic         mm;
    logic         bbit;
    logic         abit;
    logic         wr;
    logic         acc;
    int           idx;
    int           off;
    int           sh;
    logic [63:0]  rd2;      // unsigned: a SEW=64 gather index need not fit an int

    m     = width_mask(sew);
    m2    = width_mask(2 * sew);
    aa    = ef_vs1 & m[63:0];
    bb    = ef_vs2 & m[63:0];
    zz    = m[63:0];
    mm    = ef_mask;
    bidx  = {3'b000, ef_index[2:0]};
    bbit  = bb[bidx];
    abit  = aa[bidx];
    wr    = 1'b0;
    acc   = 1'b0;
    v128  = 128'd0;
    s128  = 128'sd0;
    sh    = 0;
    idx   = int'(ef_index);
    off   = 0;
    rd2   = 64'(idx);
    sel_sat     = 1'b0;
    sel_illegal = 1'b0;
    sel_trap    = 1'b0;
    sel_mres    = 1'b0;
    sel_pfx     = ef_pfx;
    sel_access  = 1'b0;
    sel_active  = 1'b0;

    if (int'(ef_family) >= NFAM) begin
      sel_illegal = 1'b1;
    end else if (!caps_i[ef_family]) begin
      sel_illegal = 1'b1;
    end else if (maskpfx_vstart_illegal) begin
      // vmsbf/vmsif/vmsof with a non-zero vstart: an illegal instruction, not
      // an element fault and not a partial execution.
      sel_illegal = 1'b1;
    end

    // `aa` is the second operand: a vector element for .vv, the scalar for
    // .vx/.vi.
    if (ef_form != FORM_VV) begin
      aa = ef_scalar & m[63:0];
    end

    // Active/write policy (see the header).
    if (idx < int'(cfg_vstart_i)) begin
      wr  = 1'b0;
      acc = 1'b0;
    end else if (idx >= int'(cfg_vl_i)) begin
      wr  = vta;
      acc = 1'b0;
    end else begin
      wr  = mm ? 1'b1 : vma;
      acc = mm;
    end

    case (ef_family)
      F_ADDSUB: begin
        case (ef_op)
          4'd0:    v128 = (uext(bb, sew) + uext(aa, sew)) & m;   // vadd
          4'd1:    v128 = (uext(bb, sew) - uext(aa, sew)) & m;   // vsub
          default: v128 = (uext(aa, sew) - uext(bb, sew)) & m;   // vrsub
        endcase
      end
      F_WIDE: begin
        if (sew > int'(ELEN_HALF)) begin
          sel_illegal = 1'b1;
        end else begin
          if (ef_op == 4'd0) begin          // vwaddu
`ifdef MOSAIC_VEC_ALU_MUTANT_WIDE_WIDTH
            v128 = (uext(bb, sew) + uext(aa, sew)) & m2;
`else
            v128 = (uext(bb, sew) + uext(aa, sew)) & m2;
`endif
          end else if (ef_op == 4'd1) begin // vwadd
`ifdef MOSAIC_VEC_ALU_MUTANT_WIDE_WIDTH
            // NEGATIVE CONTROL: the signed form is zero-extended, so
            // vwadd/vwsub lose the sign the specification requires.
            v128 = (uext(bb, sew) + uext(aa, sew)) & m2;
`else
            v128 = $unsigned(sext(bb, sew) + sext(aa, sew)) & m2;
`endif
          end else if (ef_op == 4'd2) begin // vwsubu
            v128 = (uext(bb, sew) - uext(aa, sew)) & m2;
          end else begin                   // vwsub
`ifdef MOSAIC_VEC_ALU_MUTANT_WIDE_WIDTH
            v128 = (uext(bb, sew) - uext(aa, sew)) & m2;
`else
            v128 = $unsigned(sext(bb, sew) - sext(aa, sew)) & m2;
`endif
          end
        end
      end
      F_MUL: begin
        case (ef_op)
          4'd0:    v128 = (uext(bb, sew) * uext(aa, sew)) & m;   // vmul
          4'd1:    v128 = $unsigned((sext(bb, sew) * sext(aa, sew)) >>> sew) & m;
          4'd2:    v128 = (uext(bb, sew) * uext(aa, sew)) >> sew;
          default: v128 = $unsigned(($signed(sext(bb, sew)) *
                                     $signed(uext(aa, sew))) >>> sew) & m;
        endcase
      end
      F_MULW: begin
        if (sew > int'(ELEN_HALF)) begin
          sel_illegal = 1'b1;
        end else begin
          case (ef_op)
            4'd0:    v128 = (uext(bb, sew) * uext(aa, sew)) & m2;   // vwmulu
            4'd1:    v128 = $unsigned(sext(bb, sew) * $signed(uext(aa, sew))) & m2;
            default: v128 = $unsigned(sext(bb, sew) * sext(aa, sew)) & m2;
          endcase
        end
      end
      F_SHIFT: begin
        sh = int'(aa[5:0]) & (sew - 1);
        case (ef_op)
          4'd0:    v128 = (uext(bb, sew) << sh) & m;                     // vsll
          4'd1:    v128 = uext(bb, sew) >> sh;                           // vsrl
          default: v128 = $unsigned(sext(bb, sew) >>> sh) & m;           // vsra
        endcase
      end
      F_NARROW: begin
        if (sew > int'(ELEN_HALF)) begin
          sel_illegal = 1'b1;
        end else begin
          sh = int'(aa[5:0]) & (2 * sew - 1);
          case (ef_op)
            4'd0: v128 = uext(bb, 2 * sew) >> sh;                        // vnsrl
            4'd1: v128 = $unsigned(sext(bb, 2 * sew) >>> sh) & m;        // vnsra
            4'd2: begin                                                  // vnclipu
              v128 = rnd(uext(bb, 2 * sew), sh, 1'b0, ef_vxrm);
              if (v128 > m) begin
                sel_sat = 1'b1;
                v128    = m;
              end
            end
            default: begin                                               // vnclip
              v128  = rnd(uext(bb, 2 * sew), sh, 1'b1, ef_vxrm);
              hi128 = (128'd1 << (sew - 1)) - 128'd1;
              lo128 = ~hi128;
              if ($signed(v128) > $signed(hi128)) begin
                sel_sat = 1'b1;
                v128    = hi128;
              end else if ($signed(v128) < $signed(lo128)) begin
                sel_sat = 1'b1;
                v128    = lo128;
              end else begin
                v128 = v128 & m;
              end
            end
          endcase
        end
      end
      F_LOGIC: begin
        case (ef_op)
          4'd0:    v128 = uext(bb, sew) & uext(aa, sew);   // vand
          4'd1:    v128 = uext(bb, sew) | uext(aa, sew);   // vor
          4'd2:    v128 = uext(bb, sew) ^ uext(aa, sew);   // vxor
          default: v128 = (~uext(bb, sew)) & m;            // vnot
        endcase
      end
      F_MINMAX: begin
        case (ef_op)
          4'd0:    v128 = (uext(bb, sew) <= uext(aa, sew)) ? uext(bb, sew) : uext(aa, sew);
          4'd1:    v128 = ($signed(sext(bb, sew)) <= $signed(sext(aa, sew))) ?
                          uext(bb, sew) : uext(aa, sew);
          4'd2:    v128 = (uext(bb, sew) >= uext(aa, sew)) ? uext(bb, sew) : uext(aa, sew);
          default: v128 = ($signed(sext(bb, sew)) >= $signed(sext(aa, sew))) ?
                          uext(bb, sew) : uext(aa, sew);
        endcase
      end
      F_CMP: begin
        case (ef_op)
          4'd0:    sel_mres = (bb == aa);                                     // vmseq
          4'd1:    sel_mres = (bb != aa);                                     // vmsne
          4'd2:    sel_mres = (uext(bb, sew) <  uext(aa, sew));               // vmsltu
          4'd3:    sel_mres = ($signed(sext(bb, sew)) <  $signed(sext(aa, sew)));
          4'd4:    sel_mres = (uext(bb, sew) <= uext(aa, sew));               // vmsleu
          4'd5:    sel_mres = ($signed(sext(bb, sew)) <= $signed(sext(aa, sew)));
          4'd6:    sel_mres = (uext(bb, sew) >  uext(aa, sew));               // vmsgtu
          default: sel_mres = ($signed(sext(bb, sew)) >  $signed(sext(aa, sew)));
        endcase
      end
      F_SAT: begin
        if (ef_op == 4'd0) begin   // vsaddu
`ifdef MOSAIC_VEC_ALU_MUTANT_SAT_WRAP
          v128 = (uext(bb, sew) + uext(aa, sew)) & m;
`else
          v128 = uext(bb, sew) + uext(aa, sew);
          if (v128 > m) begin
            sel_sat = 1'b1;
            v128    = m;
          end
`endif
        end else if (ef_op == 4'd1) begin   // vsadd
`ifdef MOSAIC_VEC_ALU_MUTANT_SAT_WRAP
          v128 = (uext(bb, sew) + uext(aa, sew)) & m;
`else
          s128  = sext(bb, sew) + sext(aa, sew);
          hi128 = (128'd1 << (sew - 1)) - 128'd1;
          lo128 = ~hi128;
          if ($signed(s128) > $signed(hi128)) begin
            sel_sat = 1'b1;
            v128    = hi128;
          end else if ($signed(s128) < $signed(lo128)) begin
            sel_sat = 1'b1;
            v128    = lo128;
          end else begin
            v128 = $unsigned(s128) & m;
          end
`endif
        end else if (ef_op == 4'd2) begin   // vssubu
`ifdef MOSAIC_VEC_ALU_MUTANT_SAT_WRAP
          v128 = (uext(bb, sew) - uext(aa, sew)) & m;
`else
          if (uext(bb, sew) < uext(aa, sew)) begin
            sel_sat = 1'b1;
            v128    = 128'd0;
          end else begin
            v128 = (uext(bb, sew) - uext(aa, sew)) & m;
          end
`endif
        end else if (ef_op == 4'd3) begin   // vssub
`ifdef MOSAIC_VEC_ALU_MUTANT_SAT_WRAP
          v128 = (uext(bb, sew) - uext(aa, sew)) & m;
`else
          s128  = sext(bb, sew) - sext(aa, sew);
          hi128 = (128'd1 << (sew - 1)) - 128'd1;
          lo128 = ~hi128;
          if ($signed(s128) > $signed(hi128)) begin
            sel_sat = 1'b1;
            v128    = hi128;
          end else if ($signed(s128) < $signed(lo128)) begin
            sel_sat = 1'b1;
            v128    = lo128;
          end else begin
            v128 = $unsigned(s128) & m;
          end
`endif
        end else if (ef_op == 4'd4) begin   // vaaddu
          v128 = rnd(uext(bb, sew) + uext(aa, sew), 1, 1'b0, ef_vxrm) & m;
        end else if (ef_op == 4'd5) begin   // vaadd
          v128 = rnd($unsigned(sext(bb, sew) + sext(aa, sew)), 1, 1'b1, ef_vxrm) & m;
        end else if (ef_op == 4'd6) begin   // vasubu
          v128 = rnd(uext(bb, sew) - uext(aa, sew), 1, 1'b0, ef_vxrm) & m;
        end else begin                      // vasub
          v128 = rnd($unsigned(sext(bb, sew) - sext(aa, sew)), 1, 1'b1, ef_vxrm) & m;
        end
      end
      F_MASKLOG: begin
        case (ef_op)
          4'd0:    sel_mres = bbit & abit;          // vmand
          4'd1:    sel_mres = ~(bbit & abit);       // vmnand
          4'd2:    sel_mres = bbit | abit;          // vmor
          4'd3:    sel_mres = ~(bbit | abit);       // vmnor
          4'd4:    sel_mres = bbit ^ abit;          // vmxor
          4'd5:    sel_mres = ~(bbit ^ abit);       // vmxnor
          4'd6:    sel_mres = bbit & (~abit);       // vmandn
          default: sel_mres = bbit | (~abit);       // vmorn
        endcase
        rd2 = 64'(idx / 8);
      end
      F_MASKPFX: begin
        // ---------------------------------------------------- the three rules
        // For a source mask whose first set bit over the *active* elements is
        // at position k (the running prefix `ef_pfx` is the OR of the source
        // bits strictly before the current element):
        //
        //   vmsbf: bits 0..k-1 set.  With no set bit, every active bit set.
        //   vmsif: bits 0..k   set.  With no set bit, every active bit set.
        //   vmsof: only bit k  set.  With no set bit, no active bit set.
        //
        // The all-zero row is where the three are *not* symmetric: vmsbf and
        // vmsif are all-ones, vmsof is all-zeros (v-spec.adoc: vmsbf "if there
        // is no set bit in the active elements of the source vector, then all
        // active elements in the destination are written with a 1"; vmsif is
        // "similar ... except it also includes the element with a set bit", so
        // with no such element it is the same all-ones; vmsof "only sets the
        // first element with a bit set, if any").
        case (ef_op)
          4'd0:    sel_mres = ~(ef_pfx | bbit);     // vmsbf: before the first
          4'd1:    sel_mres = ~ef_pfx;              // vmsif: through the first
          default: sel_mres = bbit & (~ef_pfx);     // vmsof: only the first
        endcase
`ifdef MOSAIC_VEC_ALU_MUTANT_MASKPFX_SAME_EXPR
        // NEGATIVE CONTROL: vmsbf and vmsif share the set-including-first
        // expression again -- exactly the defect this case exists to catch.
        if (ef_op == 4'd0) sel_mres = ~ef_pfx;
`endif
`ifdef MOSAIC_VEC_ALU_MUTANT_MASKPFX_ALLZERO
        // NEGATIVE CONTROL: an all-zero active source is treated as "no first
        // set bit, so nothing is set" instead of the specification's all-ones
        // for vmsbf/vmsif. The whole active-slice OR is known only at the final
        // active element, where the running prefix and this element's source
        // bit together cover every active source bit.
        if ((ef_op != 4'd2) && (cfg_vl_i != 8'd0) &&
            (int'(ef_index) == (int'(cfg_vl_i) - 1)) && !(ef_pfx | bbit))
          sel_mres = 1'b0;
`endif
`ifdef MOSAIC_VEC_ALU_MUTANT_MASKPFX_POS0
        // NEGATIVE CONTROL: vmsbf reports a set bit before the first element
        // even when element 0 is itself the first set bit (k = 0).
        if ((ef_op == 4'd0) && (ef_index == 8'd0)) sel_mres = 1'b1;
`endif
        sel_pfx = ef_pfx | bbit;
        rd2     = 64'(idx / 8);
      end
      F_SLIDE: begin
        off = (ef_op == 4'd0 || ef_op == 4'd1) ? int'(ef_scalar[7:0]) : 1;
        v128 = uext(bb, sew);
        if (ef_op == 4'd0) begin          // vslideup
          if (idx < off) begin
            // destination elements below OFFSET are unchanged, not merely
            // "not written by the mask": no source is read and no update is
            // produced for them.
            wr  = 1'b0;
            acc = 1'b0;
            rd2 = 64'd0;
          end else begin
            rd2 = 64'(idx - off);
          end
        end else if (ef_op == 4'd1) begin // vslidedown
          rd2 = 64'(idx + off);
          if (rd2 >= 64'(vlmax)) begin
            v128 = 128'd0;
          end
        end else if (ef_op == 4'd2) begin // vslide1up
          rd2 = (idx >= 1) ? 64'(idx - 1) : 64'd0;
          if (idx == int'(cfg_vstart_i)) begin
            v128 = uext(ef_scalar, sew);
          end
        end else begin                    // vslide1down
          rd2 = 64'(idx + 1);
          if (idx == (int'(cfg_vl_i) - 1)) begin
            v128 = uext(ef_scalar, sew);
          end
        end
      end
      F_GATHER: begin
        if (ef_form == FORM_VV) begin
          rd2 = ef_vs1 & m[63:0];
        end else if ((ef_form == FORM_VX) || (ef_form == FORM_VI)) begin
          rd2 = ef_scalar & m[63:0];
        end
`ifdef MOSAIC_VEC_ALU_MUTANT_PERMUTE_ORDER
        // NEGATIVE CONTROL: the permute orders by LMUL instead of by index.
        if (int'(lmul) > 0) begin
          rd2 = rd2 << int'(lmul);
        end
`endif
        if (rd2 >= 64'(vlmax)) begin
          v128 = 128'd0;
        end else begin
          v128 = uext(bb, sew);
        end
      end
      F_COMPRESS: begin
        rd2  = 64'(idx);
        v128 = uext(bb, sew);
      end
      F_REDUCE: begin
        case (ef_op)
          4'd0:    v128 = (uext(ef_acc, sew) + uext(bb, sew)) & m;   // vredsum
          4'd1:    v128 = (uext(ef_acc, sew) >= uext(bb, sew)) ? uext(ef_acc, sew) : uext(bb, sew);
          4'd2:    v128 = ($signed(sext(ef_acc, sew)) >= $signed(sext(bb, sew))) ?
                          uext(ef_acc, sew) : uext(bb, sew);
          4'd3:    v128 = (uext(ef_acc, sew) <= uext(bb, sew)) ? uext(ef_acc, sew) : uext(bb, sew);
          4'd4:    v128 = ($signed(sext(ef_acc, sew)) <= $signed(sext(bb, sew))) ?
                          uext(ef_acc, sew) : uext(bb, sew);
          4'd5:    v128 = uext(ef_acc, sew) & uext(bb, sew);         // vredand
          4'd6:    v128 = uext(ef_acc, sew) | uext(bb, sew);         // vredor
          default: v128 = uext(ef_acc, sew) ^ uext(bb, sew);         // vredxor
        endcase
      end
      default: begin
        // F_REDWIDE: fold SEW-wide elements into a 2*SEW accumulator
        if (ef_op == 4'd0) begin
          v128 = (uext(ef_acc, 2 * sew) + uext(bb, sew)) & m2;
        end else begin
          v128 = $unsigned(sext(ef_acc, 2 * sew) + sext(bb, sew)) & m2;
        end
      end
    endcase

    // The computed element value; a policy write substitutes all-ones.
    sel_result = v128[63:0];
    if (!acc && wr) begin
      sel_result = zz;
      sel_mres   = 1'b1;
    end

    // Which source element (vs2) this element needs.
    sel_active = acc;
    sel_access = acc;
    sel_rd2    = 8'(rd2[7:0]);

    // An inactive element must not access. A permute whose index leaves the
    // group reads nothing (the specification makes it a zero, not an access).
    if (ef_family == F_SLIDE) begin
      if (ef_op == 4'd0 && idx < off) begin
        sel_access = 1'b0;
      end else if (ef_op == 4'd0 && (idx - off) >= vlmax) begin
        sel_access = 1'b0;
      end else if (ef_op == 4'd1 && (idx + off) >= vlmax) begin
        sel_access = 1'b0;
      end else if (ef_op == 4'd2 && idx == int'(cfg_vstart_i)) begin
        sel_access = 1'b0;
      end else if (ef_op == 4'd3 && idx == (int'(cfg_vl_i) - 1)) begin
        sel_access = 1'b0;
      end
    end else if (ef_family == F_GATHER) begin
      if (rd2 >= 64'(vlmax)) begin
        sel_access = 1'b0;
      end
    end

`ifdef MOSAIC_VEC_ALU_MUTANT_MASKED_ACCESS
    // NEGATIVE CONTROL: an element the mask disables still issues its source
    // access, so an address it should never have formed can be presented.
    if (!ef_mask) begin
      sel_access = 1'b1;
    end
`endif

    // An access whose element index leaves the group is a fault the case can
    // observe; the shipping build never produces one.
    sel_trap = sel_access && (rd2 >= 64'(vlmax));

    if (sel_illegal) begin
      sel_access = 1'b0;
      sel_write  = 1'b0;
      sel_trap   = 1'b0;
    end else if (is_red) begin
      sel_write = 1'b0;     // the engine writes the scalar result itself
    end else begin
      sel_write = wr | acc;
    end
  end
  // --------------------------------------------------------- lane read-outs
  assign e_result_o  = sel_result;
  assign e_rd2_o     = sel_rd2;
  assign e_pfx_o     = sel_pfx & e_valid_i;
  assign e_mres_o    = sel_mres & e_valid_i;
  assign e_sat_o     = sel_sat & e_valid_i;
  assign e_illegal_o = sel_illegal & e_valid_i;
  assign e_trap_o    = sel_trap & e_valid_i;
  assign e_access_o  = sel_access & e_valid_i;
  assign e_write_o   = sel_write & e_valid_i;

  // ---------------------------------------------------- widths and kind
  logic [2:0] src1_sew;
  logic [3:0] src1_lmul;
  logic [2:0] src2_sew;
  logic [3:0] src2_lmul;
  logic [2:0] dst_sew;
  logic [3:0] dst_lmul;
  logic [1:0] dst_kind;

  always_comb begin
    src1_sew  = sew_l;
    src1_lmul = 4'(lmul);
    src2_sew  = sew_l;
    src2_lmul = 4'(lmul);
    dst_sew   = sew_l;
    dst_lmul  = 4'(lmul);
    dst_kind  = DST_ELEM;

    if (is_mask_family(op_f_q)) begin
      dst_kind = DST_MASK;
      dst_sew  = 3'd3;     // a mask register is SEW=8, LMUL=1
      dst_lmul = 4'd0;
    end
    if ((op_f_q == F_MASKLOG) || (op_f_q == F_MASKPFX)) begin
      src2_sew  = 3'd3;
      src2_lmul = 4'd0;
    end
    if (op_f_q == F_MASKLOG) begin
      src1_sew  = 3'd3;
      src1_lmul = 4'd0;
    end
    if (op_f_q == F_COMPRESS) begin
      src1_sew  = 3'd3;
      src1_lmul = 4'd0;
    end
    if (op_f_q == F_NARROW) begin
      src2_sew  = 3'(sew_l + 3'd1);
      src2_lmul = 4'(lmul + 4'sd1);
    end
    if ((op_f_q == F_WIDE) || (op_f_q == F_MULW) || (op_f_q == F_REDWIDE)) begin
      dst_sew  = 3'(sew_l + 3'd1);
      dst_lmul = 4'(lmul + 4'sd1);
    end
    if (op_f_q == F_REDWIDE) begin
      dst_kind = DST_SCALAR;
    end
    if (op_f_q == F_REDUCE) begin
      dst_kind = DST_SCALAR;
    end
  end

  // --------------------------------------------------- per-element decisions
  logic        mask_read_needed;
  logic        need_vs1_read;
  logic [4:0]  mask_base;
  logic [6:0]  src1_elem;
  logic [63:0] ones_val;

  always_comb begin
    mask_read_needed = 1'b0;
    mask_base        = 5'd0;
    need_vs1_read    = 1'b0;
    ones_val         = width_mask(sew)[63:0];

    if (is_compress) begin
      // the vcompress mask is the vs1 vector register
      mask_read_needed = (cur_q < 8'(cfg_vl_i));
      mask_base        = op_vs1_q;
    end else if (masken_q && (cur_q < 8'(cfg_vl_i)) &&
                 (cur_q >= 8'(cfg_vstart_i))) begin
      mask_read_needed = 1'b1;
      mask_base        = 5'd0;
    end

    if ((op_form_q == FORM_VV) &&
        ((op_f_q == F_ADDSUB) || (op_f_q == F_WIDE) || (op_f_q == F_MUL) ||
         (op_f_q == F_MULW) || (op_f_q == F_SHIFT) || (op_f_q == F_NARROW) ||
         (op_f_q == F_LOGIC) || (op_f_q == F_MINMAX) || (op_f_q == F_CMP) ||
         (op_f_q == F_SAT) || (op_f_q == F_GATHER))) begin
      need_vs1_read = 1'b1;
    end
    if (op_f_q == F_MASKLOG) begin
      need_vs1_read = 1'b1;
    end

    // a mask-register source is read at the SEW=8 element holding the bit
    src1_elem = (op_f_q == F_MASKLOG) ? 7'(cur_q >> 3) : cur_q[6:0];
  end

  // -------------------------------------------------- request / data outputs
  logic        rd_valid_c;
  logic [4:0]  rd_base_c;
  logic [6:0]  rd_elem_c;
  logic [2:0]  rd_sew_c;
  logic [3:0]  rd_lmul_c;
  logic [15:0] rd_tag_c;
  logic        wr_valid_c;
  logic [4:0]  wr_base_c;
  logic [6:0]  wr_elem_c;
  logic [2:0]  wr_sew_c;
  logic [3:0]  wr_lmul_c;
  logic [63:0] wr_data_c;
  logic        trace_c;
  logic [7:0]  trace_elem_c;
  logic [7:0]  byte_new;

  always_comb begin
    logic in_flight;

    rd_valid_c = 1'b0;
    rd_base_c  = 5'd0;
    rd_elem_c  = 7'd0;
    rd_sew_c   = src2_sew;
    rd_lmul_c  = src2_lmul;
    rd_tag_c   = TAG_SRC2;
    wr_valid_c = 1'b0;
    wr_base_c  = op_vd_q;
    wr_elem_c  = 7'd0;
    wr_sew_c   = dst_sew;
    wr_lmul_c  = dst_lmul;
    wr_data_c  = dwdata_q;
    trace_c    = 1'b0;
    trace_elem_c = 8'd0;

    in_flight = vrf_rd_rsp_valid_i;   // re-issue only while a response is absent

    if (state_q == S_ELEM) begin
      if ((step_q == E_MASK) || (step_q == E_MASKW)) begin
        if (mask_read_needed && ((step_q == E_MASK) || !in_flight)) begin
          rd_valid_c = 1'b1;
          rd_base_c  = mask_base;
          rd_elem_c  = 7'(cur_q >> 3);
          rd_sew_c   = 3'd3;
          rd_lmul_c  = 4'd0;
          rd_tag_c   = TAG_MASK;
        end
      end else if ((step_q == E_VS1) || (step_q == E_VS1W)) begin
        if ((redinit_q || need_vs1_read) && ((step_q == E_VS1) || !in_flight)) begin
          rd_valid_c = 1'b1;
          rd_base_c  = op_vs1_q;
          rd_elem_c  = redinit_q ? 7'd0 : src1_elem[6:0];
          rd_sew_c   = src1_sew;
          rd_lmul_c  = src1_lmul;
          rd_tag_c   = TAG_SRC1;
        end
      end else if ((step_q == E_VS2OUT) || (step_q == E_VS2W)) begin
        if (sel_access && ((step_q == E_VS2OUT) || !in_flight)) begin
          rd_valid_c = 1'b1;
          rd_base_c  = op_vs2_q;
          rd_elem_c  = sel_rd2[6:0];
          rd_sew_c   = src2_sew;
          rd_lmul_c  = src2_lmul;
          rd_tag_c   = TAG_SRC2;
        end
        if ((step_q == E_VS2OUT) && sel_access) begin
          trace_c      = 1'b1;
          trace_elem_c = sel_rd2;
        end
      end else if ((step_q == E_DSTRD) || (step_q == E_DSTRDW)) begin
        if ((step_q == E_DSTRD) || !in_flight) begin
          rd_valid_c = 1'b1;
          rd_base_c  = op_vd_q;
          rd_elem_c  = 7'(cur_q >> 3);
          rd_sew_c   = 3'd3;
          rd_lmul_c  = 4'd0;
          rd_tag_c   = TAG_DST;
        end
      end

      if (step_q == E_WR) begin
        wr_valid_c = 1'b1;
        if (fill_q) begin
          wr_elem_c = cur_q[6:0];
          wr_data_c = ones_val;
        end else if (redfin_q) begin
          wr_elem_c = 7'd0;
          wr_data_c = acc_q;
        end else if (is_compress) begin
          wr_elem_c = count_q[6:0];
          wr_data_c = dwdata_q;
        end else begin
          // a mask destination is addressed by its SEW=8 byte, so the write
          // element is i/8, not i.  The read path (E_DSTRD) already uses
          // `cur_q >> 3`; this expression previously multiplied that quotient
          // back by 8, so every element from 8 up wrote byte 8 (or a multiple)
          // and the bytes in between kept the stale destination byte.  Found by
          // CASE=rvv.mask_prefix_semantics, which is the first case to drive a
          // mask destination past element 8.
          wr_elem_c = (dst_kind == DST_MASK) ? 7'(cur_q >> 3) : cur_q[6:0];
          wr_data_c = dwdata_q;
        end
      end
    end

    // byte_new is only meaningful for a mask destination
    byte_new = dbyte_q;
    byte_new[cur_q[2:0]] = mres_q;
  end

  // --------------------------------------------------------------- outputs
  assign vrf_rd_valid_o = rd_valid_c;
  assign vrf_rd_base_o  = rd_base_c;
  assign vrf_rd_elem_o  = rd_elem_c;
  assign vrf_rd_sew_o   = rd_sew_c;
  assign vrf_rd_lmul_o  = rd_lmul_c;
  assign vrf_rd_tag_o   = rd_tag_c;
  assign vrf_wr_valid_o = wr_valid_c;
  assign vrf_wr_base_o  = wr_base_c;
  assign vrf_wr_elem_o  = wr_elem_c;
  assign vrf_wr_sew_o   = wr_sew_c;
  assign vrf_wr_lmul_o  = wr_lmul_c;
  assign vrf_wr_data_o  = wr_data_c;

  assign exec_busy_o        = busy_r;
  assign exec_done_o        = done_r;
  assign exec_illegal_o     = illegal_r;
  assign exec_trap_o        = trap_r;
  assign exec_trap_elem_o   = trap_elem_r;
  assign exec_sat_o         = sat_r;
  assign exec_elems_o       = elems_r;
  assign exec_cur_o         = cur_q;
  assign exec_acc_o         = acc_q;
  assign exec_trace_valid_o = trace_c;
  assign exec_trace_elem_o  = trace_elem_c;
  assign exec_src_rd_ctr_o  = srccnt_r;

  // ---------------------------------------------------------------- FSM
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      state_q     <= S_IDLE;
      step_q      <= E_MASK;
      op_f_q      <= 5'd0;
      op_op_q     <= 4'd0;
      op_form_q   <= 2'd0;
      op_vd_q     <= 5'd0;
      op_vs1_q    <= 5'd0;
      op_vs2_q    <= 5'd0;
      op_scalar_q <= 64'd0;
      op_vxrm_q   <= 2'd0;
      masken_q    <= 1'b0;
      a_q         <= 64'd0;
      b_q         <= 64'd0;
      dbyte_q     <= 8'd0;
      dwdata_q    <= 64'd0;
      mbit_q      <= 1'b0;
      mres_q      <= 1'b0;
      acc_q       <= 64'd0;
      pfx_q       <= 1'b0;
      cur_q       <= 8'd0;
      count_q     <= 8'd0;
      dst_n_q     <= 8'd0;
      rcount_q    <= 8'd0;
      redup_q     <= 1'b1;
      redinit_q   <= 1'b0;
      redfin_q    <= 1'b0;
      fill_q      <= 1'b0;
      busy_r      <= 1'b0;
      done_r      <= 1'b0;
      illegal_r   <= 1'b0;
      trap_r      <= 1'b0;
      trap_elem_r <= 7'd0;
      sat_r       <= 1'b0;
      elems_r     <= 8'd0;
      srccnt_r    <= 32'd0;
    end else begin
      done_r <= 1'b0;

      // ---- response capture -------------------------------------------
      if (vrf_rd_rsp_valid_i) begin
        case (vrf_rd_rsp_tag_i)
          TAG_MASK: mbit_q  <= vrf_rd_rsp_data_i[{3'b000, cur_q[2:0]}];
          TAG_SRC1: a_q     <= vrf_rd_rsp_data_i;
          TAG_SRC2: b_q     <= vrf_rd_rsp_data_i;
          TAG_DST:  dbyte_q <= vrf_rd_rsp_data_i[7:0];
          default:  b_q     <= vrf_rd_rsp_data_i;
        endcase
      end

      // ---- source-read accounting --------------------------------------
      if ((state_q == S_ELEM) && (step_q == E_VS2OUT) && sel_access && rd_valid_c) begin
        srccnt_r <= srccnt_r + 32'd1;
      end

      case (state_q)
        S_IDLE: begin
          busy_r <= 1'b0;
          if (exec_valid_i) begin
            busy_r      <= 1'b1;
            state_q     <= S_SETUP;
            step_q      <= E_MASK;
            op_f_q      <= exec_family_i;
            op_op_q     <= exec_op_i;
            op_form_q   <= exec_form_i;
            op_vd_q     <= exec_vd_i;
            op_vs1_q    <= exec_vs1_i;
            op_vs2_q    <= exec_vs2_i;
            op_scalar_q <= exec_scalar_i;
            op_vxrm_q   <= cfg_vxrm_i;
            masken_q    <= exec_mask_en_i;
            illegal_r   <= 1'b0;
            trap_r      <= 1'b0;
            trap_elem_r <= 7'd0;
            sat_r       <= 1'b0;
            elems_r     <= 8'd0;
            srccnt_r    <= 32'd0;
            acc_q       <= 64'd0;
            pfx_q       <= 1'b0;
            count_q     <= 8'd0;
            redinit_q   <= 1'b0;
            redfin_q    <= 1'b0;
            fill_q      <= 1'b0;
            mbit_q      <= 1'b1;
          end
        end

        S_SETUP: begin
          if ((int'(op_f_q) >= NFAM) || (!caps_i[op_f_q]) ||
              (((op_f_q == F_NARROW) || (op_f_q == F_WIDE) || (op_f_q == F_MULW) ||
                (op_f_q == F_REDWIDE)) &&
               ((sew > int'(ELEN_HALF)) || ((int'(lmul) + 1) > 3)))) begin
            illegal_r <= 1'b1;
            state_q   <= S_DONE;
          end else if (maskpfx_vstart_illegal) begin
            // vmsbf/vmsif/vmsof with a non-zero vstart: refuse before any
            // element is touched (no VRF transaction, no write).
            illegal_r <= 1'b1;
            state_q   <= S_DONE;
          end else if (is_red) begin
            redup_q <= 1'b1;
`ifdef MOSAIC_VEC_ALU_MUTANT_REDUCE_ORDER
            // NEGATIVE CONTROL: fold the source elements in descending order.
            redup_q <= 1'b0;
`endif
            redinit_q <= 1'b1;
            redfin_q  <= 1'b0;
            fill_q    <= 1'b0;
            count_q   <= 8'd0;
            cur_q     <= 8'd0;
            state_q   <= S_ELEM;
            step_q    <= E_VS1;
          end else begin
            redinit_q <= 1'b0;
            redfin_q  <= 1'b0;
            fill_q    <= 1'b0;
            cur_q     <= 8'd0;
            count_q   <= 8'd0;
            if (vlmax_log >= 0) begin
              dst_n_q <= vlmax[7:0];
            end else begin
              dst_n_q <= 8'd0;
            end
            state_q <= S_ELEM;
            step_q  <= E_MASK;
          end
        end

        S_ELEM: begin
          case (step_q)
            E_MASK: begin
              if (is_red) begin
                if (rcount_q == 8'd0) begin
                  state_q <= S_FINAL;
                end else if (mask_read_needed) begin
                  if (vrf_rd_gnt_i) begin
                    step_q <= E_MASKW;
                  end
                end else begin
                  mbit_q <= 1'b1;
                  step_q <= E_PLAN;
                end
              end else if (is_compress) begin
                if (cur_q >= 8'(cfg_vl_i)) begin
                  state_q <= S_FINAL;
                end else if (vrf_rd_gnt_i) begin
                  step_q <= E_MASKW;
                end
              end else begin
                if (cur_q >= dst_n_q) begin
                  state_q <= S_FINAL;
                end else if (cur_q >= 8'(cfg_vl_i)) begin
                  if (vta) begin
                    step_q <= E_EXEC;      // tail-agnostic: write ones
                  end else begin
                    state_q <= S_DONE;     // tail undisturbed: nothing left
                  end
                end else if (cur_q < 8'(cfg_vstart_i)) begin
                  step_q <= E_NEXT;        // below vstart: unchanged
                end else if (masken_q) begin
                  if (vrf_rd_gnt_i) begin
                    step_q <= E_MASKW;
                  end
                end else begin
                  mbit_q <= 1'b1;
                  step_q <= E_PLAN;
                end
              end
            end

            E_MASKW: begin
              if (vrf_rd_rsp_valid_i) begin
                step_q <= E_PLAN;
              end
            end

            E_PLAN: begin
              if (is_compress) begin
                // an element the vcompress mask clears is skipped entirely
                if (mbit_q) begin
                  step_q <= E_VS1;
                end else begin
                  step_q <= E_NEXT;
                end
              end else if (is_red) begin
                if (mbit_q) begin
                  step_q <= E_VS1;
                end else begin
                  step_q <= E_NEXT;        // masked-off: skipped, not folded
                end
`ifdef MOSAIC_VEC_ALU_MUTANT_MASKED_ACCESS
              end else begin
                // NEGATIVE CONTROL: masked-off elements take the access path.
                step_q <= E_VS1;
`else
              end else if (mbit_q) begin
                step_q <= E_VS1;
              end else begin
                // masked off: policy write only, no source access
                step_q <= E_EXEC;
`endif
              end
            end

            E_VS1: begin
              if (redinit_q || need_vs1_read) begin
                if (vrf_rd_gnt_i) begin
                  step_q <= E_VS1W;
                end
              end else begin
                step_q <= E_VS2OUT;
              end
            end

            E_VS1W: begin
              if (vrf_rd_rsp_valid_i) begin
                if (redinit_q) begin
                  // the reduction's initial accumulator is vs1[0]; it comes
                  // from the response directly, because a_q is captured on this
                  // same edge and would still hold the previous value.
                  if (is_redwide) begin
                    if (op_op_q == 4'd0) begin
                      acc_q <= vrf_rd_rsp_data_i & width_mask(sew)[63:0];
                    end else begin
                      acc_q <= sext64(vrf_rd_rsp_data_i, sew);
                    end
                  end else begin
                    acc_q <= vrf_rd_rsp_data_i & width_mask(sew)[63:0];
                  end
                  redinit_q <= 1'b0;
                  rcount_q  <= 8'(cfg_vl_i) - 8'(cfg_vstart_i);
                  if (redup_q) begin
                    cur_q <= 8'(cfg_vstart_i);
                  end else begin
                    cur_q <= 8'(cfg_vl_i) - 8'd1;
                  end
                  step_q <= E_MASK;
                end else begin
                  step_q <= E_VS2OUT;
                end
              end
            end

            E_VS2OUT: begin
              if (sel_access) begin
                if (vrf_rd_gnt_i) begin
                  step_q <= E_VS2W;
                end
              end else begin
                step_q <= E_EXEC;
              end
            end

            E_VS2W: begin
              if (vrf_rd_rsp_valid_i) begin
                step_q <= E_EXEC;
              end
            end

            E_EXEC: begin
              if (sel_active) begin
                pfx_q <= sel_pfx;
              end
              mres_q <= sel_mres;
              sat_r <= sat_r | (sel_active ? sel_sat : 1'b0);
              if (is_red) begin
                acc_q    <= sel_result;
                step_q   <= E_NEXT;
              end else if (is_compress) begin
                dwdata_q <= sel_result;
                step_q   <= E_WR;
              end else if (!sel_write) begin
                step_q <= E_NEXT;
              end else if (dst_kind == DST_MASK) begin
                step_q <= E_DSTRD;
              end else begin
                dwdata_q <= sel_result;
                step_q   <= E_WR;
              end
            end

            E_DSTRD: begin
              if (vrf_rd_gnt_i) begin
                step_q <= E_DSTRDW;
              end
            end

            E_DSTRDW: begin
              if (vrf_rd_rsp_valid_i) begin
                step_q <= E_DSTEXEC;
              end
            end

            E_DSTEXEC: begin
              dwdata_q <= {56'd0, byte_new};
              step_q   <= E_WR;
            end

            E_WR: begin
              if (vrf_wr_gnt_i) begin
                elems_r <= elems_r + 8'd1;
                if (redfin_q) begin
                  state_q <= S_DONE;
                end else if (fill_q) begin
                  if ((cur_q + 8'd1) >= dst_n_q) begin
                    state_q <= S_DONE;
                  end else begin
                    cur_q  <= cur_q + 8'd1;
                    step_q <= E_WR;
                  end
                end else begin
                  step_q <= E_NEXT;
                end
              end
            end

            E_NEXT: begin
              if (is_compress) begin
                if (mbit_q) begin
                  count_q <= count_q + 8'd1;
                end
                if ((cur_q + 8'd1) >= 8'(cfg_vl_i)) begin
                  state_q <= S_FINAL;
                end else begin
                  cur_q  <= cur_q + 8'd1;
                  step_q <= E_MASK;
                end
              end else if (is_red) begin
                if (rcount_q <= 8'd1) begin
                  rcount_q <= 8'd0;
                  state_q  <= S_FINAL;
                end else begin
                  rcount_q <= rcount_q - 8'd1;
                  if (redup_q) begin
                    cur_q <= cur_q + 8'd1;
                  end else begin
                    cur_q <= cur_q - 8'd1;
                  end
                  step_q <= E_MASK;
                end
              end else begin
                if ((cur_q + 8'd1) >= dst_n_q) begin
                  state_q <= S_FINAL;
                end else begin
                  cur_q  <= cur_q + 8'd1;
                  step_q <= E_MASK;
                end
              end
            end

            default: state_q <= S_FINAL;
          endcase
        end

        S_FINAL: begin
          // The scalar write of a reduction, and the tail fill of a compress,
          // are issued by the element engine, so the state returns to S_ELEM
          // with the write step selected.
          if (is_red) begin
            redfin_q <= 1'b1;
            step_q   <= E_WR;
            state_q  <= S_ELEM;
          end else if (is_compress && vta && (count_q < dst_n_q)) begin
            fill_q  <= 1'b1;
            cur_q   <= count_q;
            step_q  <= E_WR;
            state_q <= S_ELEM;
          end else begin
            state_q <= S_DONE;
          end
        end

        S_DONE: begin
          done_r  <= 1'b1;
          busy_r  <= 1'b0;
          state_q <= S_IDLE;
        end

        default: state_q <= S_IDLE;
      endcase
    end
  end

endmodule : mosaic_vec_alu

`resetall
`default_nettype wire
