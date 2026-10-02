// ============================================================================
// mosaic_vec_cfg -- vector configuration unit and vector CSR state (I-052).
//
// ------------------------------------------------------------------- why
//
// The RVV descriptor (I-051) freezes *what* a vector instruction is allowed to
// look like once a configuration exists. Something has to decide what that
// configuration *is*: the `vset{i}vl{i}` instructions set `vtype` and `vl`, and
// the seven unprivileged vector CSRs hold the other architectural state. This
// module is that decision point. It is deliberately a module-level package,
// like the descriptor and the caches: it does not wire itself into
// `mosaic_core`, so it lints and elaborates under every profile, and the
// integration is separate work.
//
// Three fail modes are named by the work package and each has a definite
// answer here:
//
//   * an **unsupported `vtype` argument must not execute**. It sets `vill`,
//     zeroes the rest of `vtype`, and sets `vl = 0` (V spec v1.0, tag
//     3570f998, "Unsupported vtype Values"); while `vill` is set a
//     `vtype`-dependent instruction is an illegal instruction ("Vector Type
//     Illegal"). `vset{i}vl{i}` and whole-register move/load/store do *not*
//     depend on `vtype` and are not blocked.
//   * a **replay must use the configuration the macro was issued under**, not
//     whatever `vtype` happens to hold later. The unit captures a snapshot on
//     the descriptor-allocation handshake and serves replays from it.
//   * an **AVL band must not choose a `vl` the spec forbids**. The chosen
//     deterministic policy is `vl = min(AVL, VLMAX)`; the case asserts the
//     spec *band* constraints and determinism, not one blessed value, because
//     the spec explicitly permits several (docs/stage-4-vector-locality.md
//     I-052: "合法vl选择固定可重放，但不强迫所有实现同一合法值").
//
// ---------------------------------------------------- vtype field encoding
//
// The `vtype` fields are decoded at bits 2:0 (`vlmul`), 3 (`vma`), 4 (`vta`),
// 7:5 (`vsew`), 62:8 (reserved, must read zero), 63 (`vill`). This is the
// encoding the delivered `mosaic_vec_desc` (I-051) decodes and the one
// `config/csr/vector.json` records for `vtype`, so the configuration snapshot
// this unit hands the descriptor is understood by it. NOTE: the ratified
// `vtype-format.adoc` of the pinned tag places `vsew` at bits 5:3 and
// `vma`/`vta` at 7/6; the whole delivered vector family uses 7:5 instead.
// `vset`/CSR semantics (AVL rules, `vill`, permissions) are independent of that
// choice, and this module follows the family's recorded layout so the
// descriptor and this unit cannot disagree. The divergence is recorded in
// results/reports/I-052-vset.md ("not covered"/finding).
//
// ------------------------------------------------------- configuration state
//
//   vtype : the configuration word. Reset value is `vill = 1`, everything else
//           zero -- the spec's recommendation and the same reset
//           config/csr/vector.json declares for the CSR.
//   vl    : read-only to software; only `vset{i}vl{i}` (and fault-only-first
//           loads, not modelled here) write it.
//   vstart: read-write, only as many bits as the largest element index needs
//           (VLEN-1 = 127 -> 7 bits). Reset to zero by *every* committed
//           vector instruction, including `vset{i}vl{i}`, and explicitly *not*
//           modified by the illegal-instruction path.
//   vxrm / vxsat / vcsr : the fixed-point rounding mode (2 bits), the saturate
//           flag (1 bit) and their mirror. `vcsr[2:1] == vxrm`, `vcsr[0] ==
//           vxsat`.
//   vlenb : VLEN/8 = 16. A design-time constant; no configuration action may
//           change it (the fail mode "configuration action changes VLEN").
//
// ----------------------------------------------------------- CSR permissions
//
// `vl`, `vtype` and `vlenb` are unprivileged **read-only** CSRs: the only
// writer of `vl`/`vtype` is `vset{i}vl{i}`, so a software CSR write to any of
// the three raises an illegal-instruction exception (V spec: "The read-only
// XLEN-wide vector type CSR ... can only be updated by vset{i}vl{i}"). NOTE:
// config/csr/vector.json records `vtype` as `access: rwr` with writable fields
// 7:0 and 63; that describes the *storage* `vset` writes, not software write
// permission, and the spec rule (software writes illegal) is implemented here.
// `vstart`, `vxsat`, `vxrm` and `vcsr` are read-write with WARL fields.
// Accessing any of the seven while `mstatus.VS == Off` raises
// illegal-instruction, and an address that is none of the seven raises
// illegal-instruction.
//
// ---------------------------------------------------------- negative control
//
// Four `ifdef` mutants live here, one per fail mode the case's controls inject:
//   MOSAIC_VEC_MUTANT_AVL_UNCLAMPED   (an AVL band returns AVL > VLMAX)
//   MOSAIC_VEC_MUTANT_VILL_NO_BLOCK   (vill does not block execution)
//   MOSAIC_VEC_MUTANT_REPLAY_NEW_VTYPE (a replay returns the current vtype)
//   MOSAIC_VEC_MUTANT_SILENT_M1       (an unsupported vtype is accepted as m1)
// Each is off unless its `-D` is passed and is proven to fail
// CASE=rvv.vset_boundaries; see results/reports/I-052-vset.md.
// ============================================================================

`default_nettype none
`resetall

module mosaic_vec_cfg #(
    parameter int unsigned VLEN = 128,
    parameter int unsigned ELEN = 64
) (
    input  logic        clk_i,
    input  logic        rst_i,

    // ---- vset{i}vl{i} execute ---------------------------------------------
    input  logic        vset_valid_i,
    input  logic [1:0]  vset_kind_i,      // 0=vsetvli, 1=vsetivli, 2=vsetvl
    input  logic [4:0]  vset_rd_i,
    input  logic [4:0]  vset_rs1_i,
    input  logic [63:0] vset_rs1_val_i,   // x[rs1], the AVL source
    input  logic [63:0] vset_rs2_val_i,   // x[rs2], the vtype source (vsetvl)
    input  logic [4:0]  vset_uimm_i,      // uimm[4:0] (vsetivli)
    input  logic [10:0] vset_vtypei_i,    // vtypei[10:0] (vsetvli/vsetivli)
    input  logic        vset_vs_off_i,    // mstatus.VS == Off

    output logic        vset_illegal_o,   // illegal instruction (VS == Off)
    output logic        vset_commit_o,    // vtype/vl/state take effect
    output logic        vset_rd_we_o,     // write rd
    output logic [63:0] vset_rd_val_o,    // value written to rd (the new vl)

    // ---- architectural vector CSR state -----------------------------------
    output logic [63:0] o_vtype_o,
    output logic [63:0] o_vl_o,
    output logic [63:0] o_vstart_o,
    output logic [63:0] o_vxrm_o,
    output logic [63:0] o_vxsat_o,
    output logic [63:0] o_vcsr_o,
    output logic [63:0] o_vlenb_o,
    output logic [63:0] o_vlmax_o,        // VLMAX of the current vtype (0 if unsupported)
    output logic        o_vill_o,
    output logic [15:0] o_cfg_gen_o,      // committed configuration generation

    // ---- configuration snapshot (descriptor allocate handshake) -----------
    input  logic        snap_capture_i,
    output logic        snap_valid_o,
    output logic [63:0] snap_vtype_o,
    output logic [63:0] snap_vl_o,
    output logic [63:0] snap_vstart_o,
    output logic [15:0] snap_gen_o,

    // ---- replay of a captured macro ---------------------------------------
    input  logic        replay_valid_i,
    input  logic [15:0] replay_gen_i,
    output logic        replay_ok_o,
    output logic [63:0] replay_vtype_o,
    output logic [63:0] replay_vl_o,
    output logic [63:0] replay_vstart_o,

    // ---- vtype-dependent execution legality -------------------------------
    input  logic        exec_valid_i,
    input  logic        exec_vtype_dep_i,
    output logic        exec_illegal_o,

    // ---- software CSR access ----------------------------------------------
    input  logic        csr_valid_i,
    input  logic [11:0] csr_addr_i,
    input  logic        csr_write_i,
    // Only the low field bits of wdata are used (vstart[6:0], vxrm[1:0],
    // vxsat[0]); csr_priv_i is a CSR requirement but every vector CSR is
    // unprivileged, so it never gates an access.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [63:0] csr_wdata_i,
    input  logic [1:0]  csr_priv_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic        csr_vs_off_i,     // mstatus.VS == Off
    output logic        csr_ready_o,
    output logic        csr_illegal_o,
    output logic [63:0] csr_rdata_o,
    output logic        csr_commit_o
);

  // The vset kinds, mirrored by the driver.
  localparam logic [1:0] VSET_VLI  = 2'd0;   // vsetvli
  localparam logic [1:0] VSET_IVLI = 2'd1;   // vsetivli
  localparam logic [1:0] VSET_VL   = 2'd2;   // vsetvl

  // The vector CSR numbers (config/csr/vector.json).
  localparam logic [11:0] CSR_VSTART = 12'h008;
  localparam logic [11:0] CSR_VXSAT  = 12'h009;
  localparam logic [11:0] CSR_VXRM   = 12'h00A;
  localparam logic [11:0] CSR_VCSR   = 12'h00F;
  localparam logic [11:0] CSR_VL     = 12'hC20;
  localparam logic [11:0] CSR_VTYPE  = 12'hC21;
  localparam logic [11:0] CSR_VLENB  = 12'hC22;

  // vtype bit 63 is vill; the whole of [62:0] reads zero while vill is set.
  localparam logic [63:0] VTYPE_VILL = 64'h8000_0000_0000_0000;

  // vstart holds indices 0..VLEN-1, so it needs $clog2(VLEN) bits.
  localparam int unsigned VLEN_LOG2 = $clog2(VLEN);
  localparam int unsigned ELEN_LOG2 = $clog2(ELEN);

  // ------------------------------------------------------------- decode helpers
  function automatic int lmul_exp_of (input logic [2:0] vlmul);
    begin
      case (vlmul)
        3'b000:  lmul_exp_of = 0;
        3'b001:  lmul_exp_of = 1;
        3'b010:  lmul_exp_of = 2;
        3'b011:  lmul_exp_of = 3;
        3'b101:  lmul_exp_of = -3;
        3'b110:  lmul_exp_of = -2;
        3'b111:  lmul_exp_of = -1;
        default: lmul_exp_of = 0;   // reserved encoding; rejected separately
      endcase
    end
  endfunction

  function automatic logic vsew_ok (input logic [2:0] vsew);
    begin
      vsew_ok = (vsew >= 3'd3) && (vsew <= 3'd6);
    end
  endfunction

  function automatic logic vlmul_ok (input logic [2:0] vlmul);
    begin
      vlmul_ok = (vlmul != 3'b100);
    end
  endfunction

  // VLMAX = VLEN * LMUL / SEW = 2^(log2(VLEN) + lmul_exp - vsew). Returns 0 for
  // a vtype the profile does not support, so callers get a defined value.
  function automatic logic [7:0] vlmax_of (input logic [2:0] vsew,
                                           input logic [2:0] vlmul);
    int exp;
    begin
      exp = int'(VLEN_LOG2) + lmul_exp_of(vlmul) - int'(vsew);
      if (!vsew_ok(vsew) || !vlmul_ok(vlmul) || (exp < 0) || (exp > 7)) begin
        vlmax_of = 8'd0;
      end else begin
        vlmax_of = 8'(1 << exp);
      end
    end
  endfunction

  // The EMUL floor: SEW <= LMUL*ELEN, i.e. lmul_exp + log2(ELEN) >= vsew.
  function automatic logic emul_ok (input logic [2:0] vsew, input logic [2:0] vlmul);
    begin
      emul_ok = (lmul_exp_of(vlmul) + int'(ELEN_LOG2)) >= int'(vsew);
    end
  endfunction

  // A full vtype word is supported only if vill is clear, every reserved bit is
  // zero, and SEW/LMUL are both encodings the profile implements. vta/vma
  // (bits 4:3) do not affect support.
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic vtype_supported (input logic [63:0] v);
    begin
      vtype_supported = (v[63] == 1'b0) && (v[62:8] == 55'd0) &&
                        vsew_ok(v[7:5]) && vlmul_ok(v[2:0]) &&
                        emul_ok(v[7:5], v[2:0]);
    end
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  // ------------------------------------------------------- registered state
  logic [63:0] vtype_q;
  logic [63:0] vl_q;
  logic [63:0] vstart_q;
  logic [63:0] vxrm_q;
  logic [63:0] vxsat_q;
  logic [15:0] gen_q;

  logic        snap_valid_q;
  logic [63:0] snap_vtype_q;
  logic [63:0] snap_vl_q;
  logic [63:0] snap_vstart_q;
  logic [15:0] snap_gen_q;

  // --------------------------------------------------------- next-state
  logic [63:0] vtype_d;
  logic [63:0] vl_d;
  logic [63:0] vstart_d;
  logic [63:0] vxrm_d;
  logic [63:0] vxsat_d;
  logic [15:0] gen_d;

  // --------------------------------------------------------- combinational
  logic [63:0] arg;             // the vtype argument (immediate or rs2)
  logic [2:0]  arg_vsew;
  logic [2:0]  arg_vlmul;
  logic [7:0]  vtype_field;
  logic        supported;
  logic [7:0]  new_vlmax;
  logic [7:0]  cur_vlmax;
  logic        old_valid;
  logic [7:0]  old_vlmax;
  logic        special_x0x0;
  logic [63:0] avl;
  logic [63:0] vlmax64;
  logic        commit_c;
  logic        csr_known_c;
  logic        csr_ro_c;
  logic        csr_write_ok_c;

  always_comb begin
    // ---- vtype argument -------------------------------------------------
    if (vset_kind_i == VSET_VL) begin
      arg = vset_rs2_val_i;
    end else begin
      arg = {53'b0, vset_vtypei_i};   // 11-bit immediate, zero-extended
    end
    arg_vsew = arg[7:5];
    arg_vlmul = arg[2:0];
    vtype_field = arg[7:0];
    supported = vtype_supported(arg);

`ifdef MOSAIC_VEC_MUTANT_SILENT_M1
    // NEGATIVE CONTROL: an unsupported SEW/LMUL encoding is silently accepted as
    // SEW=8, LMUL=1 instead of setting vill -- the spec's "unsupported config
    // silently becomes m1" fail mode.
    if (!arg[63] && !supported) begin
      arg_vsew  = 3'd3;
      arg_vlmul = 3'd0;
      vtype_field = {arg_vsew, arg[4:3], arg_vlmul};
      supported = 1'b1;
    end
`endif

    new_vlmax = vlmax_of(arg_vsew, arg_vlmul);

    // ---- rd/rs1 special cases (V spec "AVL encoding") -------------------
    //   rs1 != x0                  : AVL = x[rs1]
    //   rs1 == x0, rd != x0        : AVL = ~0     -> vl = VLMAX
    //   rs1 == x0, rd == x0        : AVL = vl     -> keep the current vl
    //                                (reserved if the new VLMAX differs)
    special_x0x0 = ((vset_kind_i == VSET_VLI) || (vset_kind_i == VSET_VL)) &&
                   (vset_rs1_i == 5'd0) && (vset_rd_i == 5'd0);
    if (vset_kind_i == VSET_IVLI) begin
      avl = {59'b0, vset_uimm_i};
    end else if (special_x0x0) begin
      avl = vl_q;
    end else if (vset_rs1_i == 5'd0) begin
      avl = 64'hFFFF_FFFF_FFFF_FFFF;
    end else begin
      avl = vset_rs1_val_i;
    end
    vlmax64 = {56'b0, new_vlmax};

    // ---- the previous configuration, for the x0/x0 reserved check -------
    old_valid = !vtype_q[63] && (vtype_q[62:8] == 55'd0) &&
                vsew_ok(vtype_q[7:5]) && vlmul_ok(vtype_q[2:0]) &&
                emul_ok(vtype_q[7:5], vtype_q[2:0]);
    old_vlmax = vlmax_of(vtype_q[7:5], vtype_q[2:0]);

    // ---- defaults -------------------------------------------------------
    vtype_d  = vtype_q;
    vl_d     = vl_q;
    vstart_d = vstart_q;
    vxrm_d   = vxrm_q;
    vxsat_d  = vxsat_q;
    gen_d    = gen_q;

    commit_c = vset_valid_i && !vset_vs_off_i;

    if (commit_c) begin
      // Every vector instruction resets vstart to zero, and this is a normal
      // instruction even when the argument is unsupported (that path sets vill,
      // it does not raise an illegal instruction).
      vstart_d = 64'd0;
      gen_d    = gen_q + 16'd1;
      if (!supported) begin
        // Unsupported: vill set, the other XLEN-1 bits zero, vl = 0.
        vtype_d = VTYPE_VILL;
        vl_d    = 64'd0;
      end else if (special_x0x0) begin
        // Keep the current vl; reserved (our choice: set vill) when there is no
        // valid previous VLMAX or the new ratio would change VLMAX.
        if (!old_valid || (old_vlmax != new_vlmax)) begin
          vtype_d = VTYPE_VILL;
          vl_d    = 64'd0;
        end else begin
          vtype_d = {56'b0, vtype_field};
          vl_d    = vl_q;
        end
      end else begin
        vtype_d = {56'b0, vtype_field};
        // The deterministic AVL policy: vl = AVL if AVL <= VLMAX, else VLMAX.
        // This satisfies all three spec bands (for VLMAX < AVL < 2*VLMAX the
        // spec permits ceil(AVL/2) <= vl <= VLMAX, and VLMAX is in range).
`ifdef MOSAIC_VEC_MUTANT_AVL_UNCLAMPED
        // NEGATIVE CONTROL: the AVL is returned unclamped, so an AVL band above
        // VLMAX selects an illegal vl.
        vl_d = avl;
`else
        vl_d = (avl < vlmax64) ? avl : vlmax64;
`endif
      end
    end

    // ---- software CSR access -------------------------------------------
    csr_known_c = (csr_addr_i == CSR_VSTART) || (csr_addr_i == CSR_VXSAT) ||
                  (csr_addr_i == CSR_VXRM)   || (csr_addr_i == CSR_VCSR)  ||
                  (csr_addr_i == CSR_VL)     || (csr_addr_i == CSR_VTYPE) ||
                  (csr_addr_i == CSR_VLENB);
    csr_ro_c = (csr_addr_i == CSR_VL) || (csr_addr_i == CSR_VTYPE) ||
               (csr_addr_i == CSR_VLENB);
    csr_illegal_o = csr_valid_i &&
                    (csr_vs_off_i || !csr_known_c || (csr_write_i && csr_ro_c));
    csr_ready_o   = csr_valid_i;
    csr_write_ok_c = csr_valid_i && csr_write_i && !csr_illegal_o;

    case (csr_addr_i)
      CSR_VSTART: csr_rdata_o = vstart_q;
      CSR_VXSAT:  csr_rdata_o = {63'b0, vxsat_q[0]};
      CSR_VXRM:   csr_rdata_o = {62'b0, vxrm_q[1:0]};
      CSR_VCSR:   csr_rdata_o = {61'b0, vxrm_q[1:0], vxsat_q[0]};
      CSR_VL:     csr_rdata_o = vl_q;
      CSR_VTYPE:  csr_rdata_o = vtype_q;
      CSR_VLENB:  csr_rdata_o = 64'(VLEN / 8);
      default:    csr_rdata_o = 64'd0;
    endcase

    if (csr_write_ok_c) begin
      case (csr_addr_i)
        CSR_VSTART: vstart_d = {57'b0, csr_wdata_i[6:0]};
        CSR_VXSAT:  vxsat_d  = {63'b0, csr_wdata_i[0]};
        CSR_VXRM:   vxrm_d   = {62'b0, csr_wdata_i[1:0]};
        CSR_VCSR:   begin
          vxrm_d  = {62'b0, csr_wdata_i[2:1]};
          vxsat_d = {63'b0, csr_wdata_i[0]};
        end
        default: ;   // read-only addresses are rejected above
      endcase
    end
    csr_commit_o = csr_write_ok_c;

    // ---- vtype-dependent execution legality -----------------------------
    // While vill is set, a vtype-dependent vector instruction is illegal;
    // vtype-free instructions (vset, whole-register moves) are not affected.
`ifdef MOSAIC_VEC_MUTANT_VILL_NO_BLOCK
    // NEGATIVE CONTROL: vill never blocks execution.
    exec_illegal_o = 1'b0;
`else
    exec_illegal_o = exec_valid_i && exec_vtype_dep_i && vtype_q[63];
`endif

    // ---- replay: serve the captured snapshot, never the current state ---
    replay_ok_o = replay_valid_i && snap_valid_q && (replay_gen_i == snap_gen_q);
`ifdef MOSAIC_VEC_MUTANT_REPLAY_NEW_VTYPE
    // NEGATIVE CONTROL: a replay is served the current (possibly newer) vtype
    // instead of the snapshot the macro was issued under.
    replay_vtype_o = vtype_q;
`else
    replay_vtype_o = snap_vtype_q;
`endif
    replay_vl_o     = snap_vl_q;
    replay_vstart_o = snap_vstart_q;

    // ---- combinational vset results -------------------------------------
    vset_illegal_o = vset_valid_i && vset_vs_off_i;
    vset_commit_o  = commit_c;
    vset_rd_we_o   = commit_c && (vset_rd_i != 5'd0);
    vset_rd_val_o  = vl_d;

    // ---- architectural read-backs ---------------------------------------
    o_vtype_o  = vtype_q;
    o_vl_o     = vl_q;
    o_vstart_o = vstart_q;
    o_vxrm_o   = vxrm_q;
    o_vxsat_o  = vxsat_q;
    o_vcsr_o   = {61'b0, vxrm_q[1:0], vxsat_q[0]};
    o_vlenb_o  = 64'(VLEN / 8);
    o_vill_o   = vtype_q[63];
    cur_vlmax  = old_valid ? old_vlmax : 8'd0;
    o_vlmax_o  = {56'b0, cur_vlmax};
    o_cfg_gen_o = gen_q;
  end

  // --------------------------------------------------------- registers
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      vtype_q      <= VTYPE_VILL;
      vl_q         <= 64'd0;
      vstart_q     <= 64'd0;
      vxrm_q       <= 64'd0;
      vxsat_q      <= 64'd0;
      gen_q        <= 16'd0;
      snap_valid_q <= 1'b0;
      snap_vtype_q <= 64'd0;
      snap_vl_q    <= 64'd0;
      snap_vstart_q <= 64'd0;
      snap_gen_q   <= 16'd0;
    end else begin
      vtype_q  <= vtype_d;
      vl_q     <= vl_d;
      vstart_q <= vstart_d;
      vxrm_q   <= vxrm_d;
      vxsat_q  <= vxsat_d;
      gen_q    <= gen_d;
      if (snap_capture_i) begin
        // Capture the configuration in effect at the end of this cycle, so a
        // descriptor allocated alongside a retiring vset captures the new one.
        snap_valid_q  <= 1'b1;
        snap_vtype_q  <= vtype_d;
        snap_vl_q     <= vl_d;
        snap_vstart_q <= vstart_d;
        snap_gen_q    <= gen_d;
      end
    end
  end

  // The captured configuration the descriptor reads on allocate.
  assign snap_valid_o  = snap_valid_q;
  assign snap_vtype_o  = snap_vtype_q;
  assign snap_vl_o     = snap_vl_q;
  assign snap_vstart_o = snap_vstart_q;
  assign snap_gen_o    = snap_gen_q;

endmodule : mosaic_vec_cfg

`default_nettype wire
`resetall
