// Simulation wrapper for CASE=rvv.descriptor_legality (work package I-051).
//
// `mosaic_vec_desc` is a combinational legality query plus one descriptor
// register, so this wrapper adds no timing of its own: the clock and the reset
// schedule belong to the C++ driver (sim/common/sim_common.h). It exists for
// three reasons, all of them about keeping one copy of a fact:
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

    // read-back of the elaborated identity widths
    output logic [31:0]                o_rob_index_w,
    output logic [31:0]                o_rob_gen_w,
    output logic [31:0]                o_uop_index_w
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

endmodule : mosaic_vec_tb

`resetall
`default_nettype wire
