// Simulation wrapper for CASE=rvv.descriptor_legality (work package I-051) and
// CASE=rvv.vset_boundaries (work package I-052).
//
// `mosaic_vec_desc` is a combinational legality query plus one descriptor
// register and `mosaic_vec_cfg` is the vset/CSR state, so this wrapper adds no
// timing of its own: the clock and the reset schedule belong to the C++ driver
// (sim/common/sim_common.h). It exists for three reasons, all of them about
// keeping one copy of a fact:
//
//   * The macro-identity widths are taken from the generated identity package
//     (mosaic_id_pkg) and read back as outputs, the way mosaic_fpu_tb does, so a
//     profile that changes the ROB geometry moves the wrapper with it instead of
//     letting a hand-written width disagree.
//   * VLEN/ELEN are passed as the frozen profile constants 128/64. They are not
//     read from the generated config package because that package emits
//     MOSAIC_VLEN only for profiles that declare a vector geometry, and this
//     module must lint and build under every profile; the p2/p3 profile files
//     declare the same 128/64, which the case asserts through vlenb.
//   * The 128-bit element bitmap is split into two 64-bit halves so the driver
//     reads plain 64-bit words instead of a Verilator wide value.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off MODDUP */
`include "mosaic_id_pkg.svh"
/* verilator lint_on MODDUP */
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_ROB_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned TB_ROB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned TB_UOP_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;

module mosaic_vec_tb (
    input  logic                       clk,
    input  logic                       rst,

    // legality query
    input  logic [63:0]                q_vtype,
    input  logic [4:0]                 q_op_class,
    input  logic [4:0]                 q_vd,
    input  logic [4:0]                 q_vs1,
    input  logic [4:0]                 q_vs2,
    input  logic [4:0]                 q_vs3,
    input  logic                       q_mask_en,
    input  logic [3:0]                 q_lane_count,

    output logic                       o_vtype_legal,
    output logic                       o_cfg_legal,
    output logic                       o_illegal,
    output logic [3:0]                 o_reason,
    output logic [2:0]                 o_sew_log2,
    output logic signed [3:0]          o_lmul_exp,
    output logic signed [3:0]          o_emul_src_exp,
    output logic signed [3:0]          o_emul_dst_exp,
    output logic [7:0]                 o_elem_count,
    output logic [7:0]                 o_vlen,
    output logic [7:0]                 o_vlenb,
    output logic [3:0]                 o_lane_count,
    output logic [7:0]                 o_class_count,

    // descriptor allocate
    input  logic                       alloc_valid,
    output logic                       alloc_ready,
    input  logic [63:0]                alloc_vtype,
    input  logic [7:0]                 alloc_vl,
    input  logic [6:0]                 alloc_vstart,
    input  logic [4:0]                 alloc_vd,
    input  logic [3:0]                 alloc_mask_ver,
    input  logic [TB_ROB_INDEX_W-1:0]  alloc_rob_index,
    input  logic [TB_ROB_GEN_W-1:0]    alloc_rob_gen,
    input  logic [TB_UOP_INDEX_W-1:0]  alloc_uop_index,

    // element progress
    input  logic                       elem_done_valid,
    input  logic [6:0]                 elem_done_index,

    // fault progress
    input  logic                       fault_valid,
    input  logic [6:0]                 fault_elem,
    input  logic [3:0]                 fault_code,

    // release
    input  logic                       desc_release,

    // descriptor state
    output logic                       o_valid,
    output logic [TB_ROB_INDEX_W-1:0]  o_macro_rob_index,
    output logic [TB_ROB_GEN_W-1:0]    o_macro_rob_gen,
    output logic [TB_UOP_INDEX_W-1:0]  o_macro_uop_index,
    output logic [63:0]                o_desc_vtype,
    output logic [7:0]                 o_desc_vl,
    output logic [6:0]                 o_desc_vstart,
    output logic [4:0]                 o_desc_vd,
    output logic [3:0]                 o_desc_mask_ver,
    output logic [63:0]                o_elem_bitmap_lo,
    output logic [63:0]                o_elem_bitmap_hi,
    output logic [7:0]                 o_prefix,
    output logic [7:0]                 o_elems_done_ctr,
    output logic                       o_fault_valid,
    output logic [6:0]                 o_fault_elem,
    output logic [3:0]                 o_fault_code,
    output logic                       o_accepting_elems,
    output logic [7:0]                 o_rob_entries_used,
    output logic [15:0]                o_alloc_ctr,
    output logic [15:0]                o_release_ctr,

    // ---- vector configuration unit (I-052) --------------------------------
    input  logic                       cfg_vset_valid,
    input  logic [1:0]                 cfg_vset_kind,
    input  logic [4:0]                 cfg_vset_rd,
    input  logic [4:0]                 cfg_vset_rs1,
    input  logic [63:0]                cfg_vset_rs1_val,
    input  logic [63:0]                cfg_vset_rs2_val,
    input  logic [4:0]                 cfg_vset_uimm,
    input  logic [10:0]                cfg_vset_vtypei,
    input  logic                       cfg_vset_vs_off,
    output logic                       cfg_vset_illegal,
    output logic                       cfg_vset_commit,
    output logic                       cfg_vset_rd_we,
    output logic [63:0]                cfg_vset_rd_val,
    output logic [63:0]                cfg_vtype,
    output logic [63:0]                cfg_vl,
    output logic [63:0]                cfg_vstart,
    output logic [63:0]                cfg_vxrm,
    output logic [63:0]                cfg_vxsat,
    output logic [63:0]                cfg_vcsr,
    output logic [63:0]                cfg_vlenb,
    output logic [63:0]                cfg_vlmax,
    output logic                       cfg_vill,
    output logic [15:0]                cfg_gen,
    input  logic                       cfg_snap_capture,
    output logic                       cfg_snap_valid,
    output logic [63:0]                cfg_snap_vtype,
    output logic [63:0]                cfg_snap_vl,
    output logic [63:0]                cfg_snap_vstart,
    output logic [15:0]                cfg_snap_gen,
    input  logic                       cfg_replay_valid,
    input  logic [15:0]                cfg_replay_gen,
    output logic                       cfg_replay_ok,
    output logic [63:0]                cfg_replay_vtype,
    output logic [63:0]                cfg_replay_vl,
    output logic [63:0]                cfg_replay_vstart,
    input  logic                       cfg_exec_valid,
    input  logic                       cfg_exec_vtype_dep,
    output logic                       cfg_exec_illegal,
    input  logic                       cfg_csr_valid,
    input  logic [11:0]                cfg_csr_addr,
    input  logic                       cfg_csr_write,
    input  logic [63:0]                cfg_csr_wdata,
    input  logic [1:0]                 cfg_csr_priv,
    input  logic                       cfg_csr_vs_off,
    output logic                       cfg_csr_ready,
    output logic                       cfg_csr_illegal,
    output logic [63:0]                cfg_csr_rdata,
    output logic                       cfg_csr_commit,

    // read-back of the elaborated identity widths
    output logic [31:0]                o_rob_index_w,
    output logic [31:0]                o_rob_gen_w,
    output logic [31:0]                o_uop_index_w,

    // ------------- vector execution unit (I-054) ---------------------------
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [16:0]                alu_caps_i,
    input  logic                       alu_exec_valid_i,
    input  logic [4:0]                 alu_family_i,
    input  logic [3:0]                 alu_op_i,
    input  logic [1:0]                 alu_form_i,
    input  logic [4:0]                 alu_vd_i,
    input  logic [4:0]                 alu_vs1_i,
    input  logic [4:0]                 alu_vs2_i,
    input  logic [63:0]                alu_scalar_i,
    input  logic                       alu_mask_en_i,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic                       alu_busy_o,
    output logic                       alu_done_o,
    output logic                       alu_illegal_o,
    output logic                       alu_trap_o,
    output logic [6:0]                 alu_trap_elem_o,
    output logic                       alu_sat_o,
    output logic [7:0]                 alu_elems_o,
    output logic [7:0]                 alu_cur_o,
    output logic [2:0]                 alu_state_o,
    output logic [3:0]                 alu_step_o,
    output logic                       alu_rd_valid_o,
    output logic [4:0]                 alu_rd_base_o,
    output logic [6:0]                 alu_rd_elem_o,
    output logic [2:0]                 alu_rd_sew_o,
    output logic [3:0]                 alu_rd_lmul_o,
    output logic [63:0]                alu_acc_o,
    output logic                       alu_trace_valid_o,
    output logic [7:0]                 alu_trace_elem_o,
    output logic [31:0]                alu_src_rd_ctr_o,

    // direct element lane
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic                       el_valid_i,
    input  logic [4:0]                 el_family_i,
    input  logic [3:0]                 el_op_i,
    input  logic [1:0]                 el_form_i,
    input  logic [63:0]                el_vs2_i,
    input  logic [63:0]                el_vs1_i,
    input  logic [63:0]                el_acc_i,
    input  logic                       el_mask_i,
    input  logic [63:0]                el_scalar_i,
    input  logic [7:0]                 el_index_i,
    input  logic                       el_pfx_i,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic [63:0]                el_result_o,
    output logic                       el_mres_o,
    output logic                       el_sat_o,
    output logic                       el_illegal_o,
    output logic                       el_trap_o,
    output logic                       el_access_o,
    output logic                       el_write_o,
    output logic [7:0]                 el_rd2_o,
    output logic                       el_pfx_o,

    // driver-owned VRF port
    input  logic                       mem_owner_i,
    input  logic                       mem_rd_valid_i,
    input  logic [4:0]                 mem_rd_base_i,
    input  logic [6:0]                 mem_rd_elem_i,
    input  logic [2:0]                 mem_rd_sew_i,
    input  logic [3:0]                 mem_rd_lmul_i,
    input  logic [15:0]                mem_rd_tag_i,
    output logic                       mem_rd_gnt_o,
    output logic                       mem_rd_rsp_valid_o,
    output logic [15:0]                mem_rd_rsp_tag_o,
    output logic [63:0]                mem_rd_rsp_data_o,
    input  logic                       mem_wr_valid_i,
    input  logic [4:0]                 mem_wr_base_i,
    input  logic [6:0]                 mem_wr_elem_i,
    input  logic [2:0]                 mem_wr_sew_i,
    input  logic [3:0]                 mem_wr_lmul_i,
    input  logic [63:0]                mem_wr_data_i,
    output logic                       mem_wr_gnt_o,

    // VRF status read-back
    output logic [31:0]                vrf_rd_gnt_ctr_o,
    output logic [31:0]                vrf_rd_bad_ctr_o,
    output logic [31:0]                vrf_wr_gnt_ctr_o,
    output logic [31:0]                vrf_rd_latency_o,
    output logic [31:0]                vrf_rows_o,
    output logic [31:0]                vrf_banks_o
);

  logic [127:0] elem_bitmap;

  mosaic_vec_desc #(
      .VLEN        (128),
      .ELEN        (64),
      .ROB_INDEX_W (TB_ROB_INDEX_W),
      .ROB_GEN_W   (TB_ROB_GEN_W),
      .UOP_INDEX_W (TB_UOP_INDEX_W)
  ) u_vec_desc (
      .clk_i                 (clk),
      .rst_i                 (rst),

      .vtype_i               (q_vtype),
      .op_class_i            (q_op_class),
      .vd_i                  (q_vd),
      .vs1_i                 (q_vs1),
      .vs2_i                 (q_vs2),
      .vs3_i                 (q_vs3),
      .mask_en_i             (q_mask_en),
      .lane_count_i          (q_lane_count),

      .o_vtype_legal_o       (o_vtype_legal),
      .o_cfg_legal_o         (o_cfg_legal),
      .o_illegal_o           (o_illegal),
      .o_reason_o            (o_reason),
      .o_sew_log2_o          (o_sew_log2),
      .o_lmul_exp_o          (o_lmul_exp),
      .o_emul_src_exp_o      (o_emul_src_exp),
      .o_emul_dst_exp_o      (o_emul_dst_exp),
      .o_elem_count_o        (o_elem_count),
      .o_vlen_o              (o_vlen),
      .o_vlenb_o             (o_vlenb),
      .o_lane_count_o        (o_lane_count),
      .o_class_count_o       (o_class_count),

      .alloc_valid_i         (alloc_valid),
      .alloc_ready_o         (alloc_ready),
      .alloc_vtype_i         (alloc_vtype),
      .alloc_vl_i            (alloc_vl),
      .alloc_vstart_i        (alloc_vstart),
      .alloc_vd_i            (alloc_vd),
      .alloc_mask_ver_i      (alloc_mask_ver),
      .alloc_rob_index_i     (alloc_rob_index),
      .alloc_rob_gen_i       (alloc_rob_gen),
      .alloc_uop_index_i     (alloc_uop_index),

      .elem_done_valid_i     (elem_done_valid),
      .elem_done_index_i     (elem_done_index),

      .fault_valid_i         (fault_valid),
      .fault_elem_i          (fault_elem),
      .fault_code_i          (fault_code),

      .release_i             (desc_release),

      .o_valid_o             (o_valid),
      .o_macro_rob_index_o   (o_macro_rob_index),
      .o_macro_rob_gen_o     (o_macro_rob_gen),
      .o_macro_uop_index_o   (o_macro_uop_index),
      .o_vtype_o             (o_desc_vtype),
      .o_vl_o                (o_desc_vl),
      .o_vstart_o            (o_desc_vstart),
      .o_vd_o                (o_desc_vd),
      .o_mask_ver_o          (o_desc_mask_ver),
      .o_elem_bitmap_o       (elem_bitmap),
      .o_prefix_o            (o_prefix),
      .o_elems_done_ctr_o    (o_elems_done_ctr),
      .o_fault_valid_o       (o_fault_valid),
      .o_fault_elem_o        (o_fault_elem),
      .o_fault_code_o        (o_fault_code),
      .o_accepting_elems_o   (o_accepting_elems),
      .o_rob_entries_used_o  (o_rob_entries_used),
      .o_alloc_ctr_o         (o_alloc_ctr),
      .o_release_ctr_o       (o_release_ctr)
  );

  assign o_elem_bitmap_lo = elem_bitmap[63:0];
  assign o_elem_bitmap_hi = elem_bitmap[127:64];

  assign o_rob_index_w = 32'(TB_ROB_INDEX_W);
  assign o_rob_gen_w   = 32'(TB_ROB_GEN_W);
  assign o_uop_index_w = 32'(TB_UOP_INDEX_W);

  // VLEN/ELEN are the frozen profile constants 128/64, for the same reason the
  // descriptor uses them: this module must lint and build under every profile.
  mosaic_vec_cfg #(
      .VLEN (128),
      .ELEN (64)
  ) u_vec_cfg (
      .clk_i             (clk),
      .rst_i             (rst),

      .vset_valid_i      (cfg_vset_valid),
      .vset_kind_i       (cfg_vset_kind),
      .vset_rd_i         (cfg_vset_rd),
      .vset_rs1_i        (cfg_vset_rs1),
      .vset_rs1_val_i    (cfg_vset_rs1_val),
      .vset_rs2_val_i    (cfg_vset_rs2_val),
      .vset_uimm_i       (cfg_vset_uimm),
      .vset_vtypei_i     (cfg_vset_vtypei),
      .vset_vs_off_i     (cfg_vset_vs_off),

      .vset_illegal_o    (cfg_vset_illegal),
      .vset_commit_o     (cfg_vset_commit),
      .vset_rd_we_o      (cfg_vset_rd_we),
      .vset_rd_val_o     (cfg_vset_rd_val),

      .o_vtype_o         (cfg_vtype),
      .o_vl_o            (cfg_vl),
      .o_vstart_o        (cfg_vstart),
      .o_vxrm_o          (cfg_vxrm),
      .o_vxsat_o         (cfg_vxsat),
      .o_vcsr_o          (cfg_vcsr),
      .o_vlenb_o         (cfg_vlenb),
      .o_vlmax_o         (cfg_vlmax),
      .o_vill_o          (cfg_vill),
      .o_cfg_gen_o       (cfg_gen),

      .snap_capture_i    (cfg_snap_capture),
      .snap_valid_o      (cfg_snap_valid),
      .snap_vtype_o      (cfg_snap_vtype),
      .snap_vl_o         (cfg_snap_vl),
      .snap_vstart_o     (cfg_snap_vstart),
      .snap_gen_o        (cfg_snap_gen),

      .replay_valid_i    (cfg_replay_valid),
      .replay_gen_i      (cfg_replay_gen),
      .replay_ok_o       (cfg_replay_ok),
      .replay_vtype_o    (cfg_replay_vtype),
      .replay_vl_o       (cfg_replay_vl),
      .replay_vstart_o   (cfg_replay_vstart),

      .exec_valid_i      (cfg_exec_valid),
      .exec_vtype_dep_i  (cfg_exec_vtype_dep),
      .exec_illegal_o    (cfg_exec_illegal),

      .csr_valid_i       (cfg_csr_valid),
      .csr_addr_i        (cfg_csr_addr),
      .csr_write_i       (cfg_csr_write),
      .csr_wdata_i       (cfg_csr_wdata),
      .csr_priv_i        (cfg_csr_priv),
      .csr_vs_off_i      (cfg_csr_vs_off),
      .csr_ready_o       (cfg_csr_ready),
      .csr_illegal_o     (cfg_csr_illegal),
      .csr_rdata_o       (cfg_csr_rdata),
      .csr_commit_o      (cfg_csr_commit)
  );

  // ==========================================================================
  // I-054: the vector execution unit. `mosaic_vec_alu` reads and writes the
  // banked VRF through one read slot and one write slot; the driver borrows
  // that same slot through `mem_owner_i` so it can prime and inspect the file
  // without a second copy of the storage. The two owners are never active at
  // once: the ALU is idle whenever the driver owns the slot.
  //
  // The ALU's configuration is the snapshot I-052 hands out (`snap_*`), not a
  // second decode of `vtype`: SEW/LMUL, vl, vstart and vxrm all arrive from the
  // configuration unit.
  // ==========================================================================

  localparam int unsigned TB_LANES = 8;

  /* verilator lint_off UNUSEDSIGNAL */
  logic [TB_LANES-1:0]       vrf_rd_valid_f;
  logic [TB_LANES*5-1:0]     vrf_rd_base_f;
  logic [TB_LANES*7-1:0]     vrf_rd_elem_f;
  logic [TB_LANES*3-1:0]     vrf_rd_sew_f;
  logic [TB_LANES*4-1:0]     vrf_rd_lmul_f;
  logic [TB_LANES*16-1:0]    vrf_rd_tag_f;
  logic [TB_LANES-1:0]       vrf_wr_valid_f;
  logic [TB_LANES*5-1:0]     vrf_wr_base_f;
  logic [TB_LANES*7-1:0]     vrf_wr_elem_f;
  logic [TB_LANES*3-1:0]     vrf_wr_sew_f;
  logic [TB_LANES*4-1:0]     vrf_wr_lmul_f;
  logic [TB_LANES*64-1:0]    vrf_wr_data_f;
  logic [TB_LANES-1:0]       vrf_rd_gnt_f;
  logic [TB_LANES-1:0]       vrf_rd_rsp_valid_f;
  logic [TB_LANES*16-1:0]    vrf_rd_rsp_tag_f;
  logic [TB_LANES*64-1:0]    vrf_rd_rsp_data_f;
  logic [TB_LANES-1:0]       vrf_wr_gnt_f;
  /* verilator lint_on UNUSEDSIGNAL */

  logic        unused_q_valid;
  logic [4:0]  unused_q_phys, unused_q_bank0, unused_q_bank1;
  logic [1:0]  unused_q_row, unused_q_nbanks;
  logic [6:0]  unused_q_lo;
  logic [7:0]  unused_q_hi;
  logic [31:0] unused_rd_conf, unused_wr_conf, unused_wr_hazard, unused_wr_bad;
  logic        unused_vrf_busy;
  logic [31:0] unused_vlen, unused_elen, unused_vregs, unused_bank_w, unused_regs_per_row;
  logic [31:0] unused_lane_max, unused_rd_ports, unused_wr_ports, unused_plat;
  logic        alu_rd_valid;
  logic [4:0]  alu_rd_base;
  logic [6:0]  alu_rd_elem;
  logic [2:0]  alu_rd_sew;
  logic [3:0]  alu_rd_lmul;
  logic [15:0] alu_rd_tag;
  logic        alu_rd_gnt;
  logic        alu_rd_rsp_valid;
  logic [15:0] alu_rd_rsp_tag;
  logic [63:0] alu_rd_rsp_data;
  logic        alu_wr_valid;
  logic [4:0]  alu_wr_base;
  logic [6:0]  alu_wr_elem;
  logic [2:0]  alu_wr_sew;
  logic [3:0]  alu_wr_lmul;
  logic [63:0] alu_wr_data;
  logic        alu_wr_gnt;

  always_comb begin
    vrf_rd_valid_f          = '0;
    vrf_rd_base_f           = '0;
    vrf_rd_elem_f           = '0;
    vrf_rd_sew_f            = '0;
    vrf_rd_lmul_f           = '0;
    vrf_rd_tag_f            = '0;
    vrf_wr_valid_f          = '0;
    vrf_wr_base_f           = '0;
    vrf_wr_elem_f           = '0;
    vrf_wr_sew_f            = '0;
    vrf_wr_lmul_f           = '0;
    vrf_wr_data_f           = '0;

    vrf_rd_valid_f[0]       = mem_owner_i ? mem_rd_valid_i : alu_rd_valid;
    vrf_rd_base_f[4:0]      = mem_owner_i ? mem_rd_base_i  : alu_rd_base;
    vrf_rd_elem_f[6:0]      = mem_owner_i ? mem_rd_elem_i  : alu_rd_elem;
    vrf_rd_sew_f[2:0]       = mem_owner_i ? mem_rd_sew_i   : alu_rd_sew;
    vrf_rd_lmul_f[3:0]      = mem_owner_i ? mem_rd_lmul_i  : alu_rd_lmul;
    vrf_rd_tag_f[15:0]      = mem_owner_i ? mem_rd_tag_i   : alu_rd_tag;

    vrf_wr_valid_f[0]       = mem_owner_i ? mem_wr_valid_i : alu_wr_valid;
    vrf_wr_base_f[4:0]      = mem_owner_i ? mem_wr_base_i  : alu_wr_base;
    vrf_wr_elem_f[6:0]      = mem_owner_i ? mem_wr_elem_i  : alu_wr_elem;
    vrf_wr_sew_f[2:0]       = mem_owner_i ? mem_wr_sew_i   : alu_wr_sew;
    vrf_wr_lmul_f[3:0]      = mem_owner_i ? mem_wr_lmul_i  : alu_wr_lmul;
    vrf_wr_data_f[63:0]     = mem_owner_i ? mem_wr_data_i  : alu_wr_data;
  end

  assign alu_rd_gnt       = vrf_rd_gnt_f[0];
  assign alu_rd_rsp_valid = vrf_rd_rsp_valid_f[0];
  assign alu_rd_rsp_tag   = vrf_rd_rsp_tag_f[15:0];
  assign alu_rd_rsp_data  = vrf_rd_rsp_data_f[63:0];
  assign alu_wr_gnt       = vrf_wr_gnt_f[0];

  assign mem_rd_gnt_o       = vrf_rd_gnt_f[0];
  assign mem_rd_rsp_valid_o = vrf_rd_rsp_valid_f[0];
  assign mem_rd_rsp_tag_o   = vrf_rd_rsp_tag_f[15:0];
  assign mem_rd_rsp_data_o  = vrf_rd_rsp_data_f[63:0];
  assign mem_wr_gnt_o       = vrf_wr_gnt_f[0];
  assign alu_rd_valid_o     = alu_rd_valid;
  assign alu_rd_base_o      = alu_rd_base;
  assign alu_rd_elem_o      = alu_rd_elem;
  assign alu_rd_sew_o       = alu_rd_sew;
  assign alu_rd_lmul_o      = alu_rd_lmul;
  assign alu_wr_valid_o     = alu_wr_valid;

  mosaic_vrf #(
      .VLEN       (128),
      .ELEN       (64),
      .VREGS      (32),
      .BANK_W     (32),
      .BANKS      (32),
      .LANES_MAX  (TB_LANES),
      .RD_PORTS   (2),
      .WR_PORTS   (1),
      .RD_LATENCY (1)
  ) u_vec_vrf (
      .clk_i            (clk),
      .rst_i            (rst),

      .lane_count_i     (4'd1),
      .plat_i           (2'd0),

      .rd_valid_i       (vrf_rd_valid_f),
      .rd_base_i        (vrf_rd_base_f),
      .rd_elem_i        (vrf_rd_elem_f),
      .rd_sew_i         (vrf_rd_sew_f),
      .rd_lmul_i        (vrf_rd_lmul_f),
      .rd_tag_i         (vrf_rd_tag_f),
      .rd_gnt_o         (vrf_rd_gnt_f),
      .rd_rsp_valid_o   (vrf_rd_rsp_valid_f),
      .rd_rsp_tag_o     (vrf_rd_rsp_tag_f),
      .rd_rsp_data_o    (vrf_rd_rsp_data_f),

      .wr_valid_i       (vrf_wr_valid_f),
      .wr_base_i        (vrf_wr_base_f),
      .wr_elem_i        (vrf_wr_elem_f),
      .wr_sew_i         (vrf_wr_sew_f),
      .wr_lmul_i        (vrf_wr_lmul_f),
      .wr_data_i        (vrf_wr_data_f),
      .wr_gnt_o         (vrf_wr_gnt_f),

      .q_valid_i        (1'b0),
      .q_base_i         (5'd0),
      .q_elem_i         (7'd0),
      .q_sew_i          (3'd0),
      .q_lmul_i         (4'd0),
      .o_q_valid_o      (unused_q_valid),
      .o_q_phys_reg_o   (unused_q_phys),
      .o_q_row_o        (unused_q_row),
      .o_q_lo_bit_o     (unused_q_lo),
      .o_q_hi_bit_o     (unused_q_hi),
      .o_q_nbanks_o     (unused_q_nbanks),
      .o_q_bank0_o      (unused_q_bank0),
      .o_q_bank1_o      (unused_q_bank1),

      .o_rd_gnt_ctr     (vrf_rd_gnt_ctr_o),
      .o_rd_conflict_ctr (unused_rd_conf),
      .o_rd_bad_ctr     (vrf_rd_bad_ctr_o),
      .o_wr_gnt_ctr     (vrf_wr_gnt_ctr_o),
      .o_wr_conflict_ctr (unused_wr_conf),
      .o_wr_hazard_ctr  (unused_wr_hazard),
      .o_wr_bad_ctr     (unused_wr_bad),
      .o_busy           (unused_vrf_busy),

      .o_vlen_o         (unused_vlen),
      .o_elen_o         (unused_elen),
      .o_vregs_o        (unused_vregs),
      .o_banks_o        (vrf_banks_o),
      .o_bank_w_o       (unused_bank_w),
      .o_rows_o         (vrf_rows_o),
      .o_regs_per_row_o (unused_regs_per_row),
      .o_lane_max_o     (unused_lane_max),
      .o_rd_latency_o   (vrf_rd_latency_o),
      .o_rd_ports_o     (unused_rd_ports),
      .o_wr_ports_o     (unused_wr_ports),
      .o_plat_o         (unused_plat)
  );

  mosaic_vec_alu #(
      .VLEN (128),
      .ELEN (64),
      .NFAM (17)
  ) u_vec_alu (
      .clk_i              (clk),
      .rst_i              (rst),
      .caps_i             (alu_caps_i),

      .cfg_vtype_i        (cfg_snap_vtype),
      .cfg_vl_i           (cfg_snap_vl[7:0]),
      .cfg_vstart_i       (cfg_snap_vstart[6:0]),
      .cfg_vxrm_i         (cfg_vxrm[1:0]),

      .e_valid_i          (el_valid_i),
      .e_family_i         (el_family_i),
      .e_op_i             (el_op_i),
      .e_form_i           (el_form_i),
      .e_vs2_i            (el_vs2_i),
      .e_vs1_i            (el_vs1_i),
      .e_acc_i            (el_acc_i),
      .e_mask_i           (el_mask_i),
      .e_scalar_i         (el_scalar_i),
      .e_index_i          (el_index_i),
      .e_pfx_i            (el_pfx_i),
      .e_result_o         (el_result_o),
      .e_mres_o           (el_mres_o),
      .e_sat_o            (el_sat_o),
      .e_illegal_o        (el_illegal_o),
      .e_trap_o           (el_trap_o),
      .e_access_o         (el_access_o),
      .e_write_o          (el_write_o),
      .e_rd2_o            (el_rd2_o),
      .e_pfx_o            (el_pfx_o),

      .exec_valid_i       (alu_exec_valid_i),
      .exec_family_i      (alu_family_i),
      .exec_op_i          (alu_op_i),
      .exec_form_i        (alu_form_i),
      .exec_vd_i          (alu_vd_i),
      .exec_vs1_i         (alu_vs1_i),
      .exec_vs2_i         (alu_vs2_i),
      .exec_scalar_i      (alu_scalar_i),
      .exec_mask_en_i     (alu_mask_en_i),
      .exec_busy_o        (alu_busy_o),
      .exec_done_o        (alu_done_o),
      .exec_illegal_o     (alu_illegal_o),
      .exec_trap_o        (alu_trap_o),
      .exec_trap_elem_o   (alu_trap_elem_o),
      .exec_sat_o         (alu_sat_o),
      .exec_elems_o       (alu_elems_o),
      .exec_cur_o         (alu_cur_o),
      .exec_state_o       (alu_state_o),
      .exec_step_o        (alu_step_o),
      .exec_acc_o         (alu_acc_o),
      .exec_trace_valid_o (alu_trace_valid_o),
      .exec_trace_elem_o  (alu_trace_elem_o),
      .exec_src_rd_ctr_o  (alu_src_rd_ctr_o),

      .vrf_rd_valid_o     (alu_rd_valid),
      .vrf_rd_base_o      (alu_rd_base),
      .vrf_rd_elem_o      (alu_rd_elem),
      .vrf_rd_sew_o       (alu_rd_sew),
      .vrf_rd_lmul_o      (alu_rd_lmul),
      .vrf_rd_tag_o       (alu_rd_tag),
      .vrf_rd_gnt_i       (alu_rd_gnt),
      .vrf_rd_rsp_valid_i (alu_rd_rsp_valid),
      .vrf_rd_rsp_tag_i   (alu_rd_rsp_tag),
      .vrf_rd_rsp_data_i  (alu_rd_rsp_data),

      .vrf_wr_valid_o     (alu_wr_valid),
      .vrf_wr_base_o      (alu_wr_base),
      .vrf_wr_elem_o      (alu_wr_elem),
      .vrf_wr_sew_o       (alu_wr_sew),
      .vrf_wr_lmul_o      (alu_wr_lmul),
      .vrf_wr_data_o      (alu_wr_data),
      .vrf_wr_gnt_i       (alu_wr_gnt)
  );

endmodule : mosaic_vec_tb

`resetall
`default_nettype wire
