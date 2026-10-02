// Simulation wrapper for CASE=bypass.local_raw_chain (work package I-027).
//
// The wrapper adds no timing of its own: every pin of `mosaic_cluster_bypass` is
// registered on the input side and combinational on the output side, so the C++
// driver drives the ports, evaluates with the clock low, compares the whole
// output surface against its shadow, and then applies the edge. There is no
// clock generation, no reset generation and no `$display` here: the C++ side
// owns all three, per sim/common/sim_common.h.
//
// ------------------------------------------------------------ flattened pins
//
// Every driver-facing port is a fixed-width vector (32 bits, 64 for data), so
// the driver contains no geometry and would keep compiling if a profile changed
// the identity widths. The narrowing to the module's own port widths uses the
// **same generated packages the RTL reads**, by scope reference rather than by
// a second definition:
//
//   * `mosaic_cfg_pkg.svh` and `mosaic_id_pkg.svh` are included by the module
//     this case compiles, and this file includes them as well so the widths are
//     named here too; each is guarded, so the second include is a no-op.
//
// The identity widths are not re-derived by any rule of this wrapper's own: a
// destination tag is narrowed with the PRF tag width, a generation with the PRF
// generation width and the ROB identity with the ROB's own widths. p0 does not
// make them interchangeable, which is why they are separate constants.
//
// ---------------------------------------------------------------- geometry
//
// The module has no geometry logic worth reading back (its slot is one entry),
// but the driver still refuses to hardcode widths, so the widths it drives are
// exported as read-back outputs computed from the packages above. A geometry
// change is then a runtime failure in the driver rather than a silent
// truncation of an identity field.

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

`default_nettype none
`resetall

module mosaic_cluster_bypass_tb (
    input  logic        clk,
    input  logic        rst,

    // --------------------------------------------------------------- arming
    input  logic        bp_en_i,

    // ------------------------------------------------------------- producer
    input  logic        p_valid_i,
    input  logic        p_authorised_i,
    input  logic [31:0] p_tag_i,
    input  logic [31:0] p_gen_i,
    input  logic [31:0] p_rob_index_i,
    input  logic [31:0] p_rob_gen_i,
    input  logic [31:0] p_uop_index_i,
    input  logic [63:0] p_value_i,

    // --------------------------------------------------------------- cancel
    input  logic        flush_i,

    // ---------------------------------------------------- durable wakeup (PRF)
    input  logic        w_valid_i,
    input  logic [31:0] w_tag_i,
    input  logic [31:0] w_gen_i,
    input  logic [63:0] w_val_i,

    // ------------------------------------------------------------- candidate
    input  logic        c_valid_i,
    input  logic [31:0] c_s1_tag_i,
    input  logic [31:0] c_s1_gen_i,
    input  logic        c_s1_need_i,
    input  logic [31:0] c_s2_tag_i,
    input  logic [31:0] c_s2_gen_i,
    input  logic        c_s2_need_i,

    // ---------------------------------------------------------------- observe
    output logic        bp_s1_hit_o,
    output logic        bp_s2_hit_o,
    output logic        fb_s1_sel_o,
    output logic        fb_s2_sel_o,
    output logic        s1_rdy_o,
    output logic        s2_rdy_o,
    output logic [63:0] s1_val_o,
    output logic [63:0] s2_val_o,
    output logic [31:0] s1_src_o,
    output logic [31:0] s2_src_o,

    output logic        slot_valid_o,
    output logic [31:0] slot_tag_o,
    output logic [31:0] slot_gen_o,
    output logic [31:0] slot_rob_index_o,
    output logic [31:0] slot_rob_gen_o,
    output logic [31:0] slot_uop_index_o,

    output logic        o_slot_captured_o,
    output logic        o_unauth_o,
    output logic [31:0] o_hit_ctr_o,
    output logic [31:0] o_miss_ctr_o,
    output logic [31:0] o_unauth_ctr_o,
    output logic [31:0] o_id_reject_ctr_o,
    output logic [31:0] o_flush_ctr_o,

    // --------------------------------------------------------------- geometry
    output logic [31:0] o_xlen_o,
    output logic [31:0] o_tag_w_o,
    output logic [31:0] o_pgen_w_o,
    output logic [31:0] o_idx_w_o,
    output logic [31:0] o_rgen_w_o,
    output logic [31:0] o_uop_w_o
);

  localparam int unsigned XLEN   = mosaic_cfg_pkg::MOSAIC_XLEN;             // 64
  localparam int unsigned TAG_W  = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;      // 7
  localparam int unsigned PGEN_W = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;      // 8
  localparam int unsigned IDX_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;    // 6
  localparam int unsigned RGEN_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;      // 7
  localparam int unsigned UOP_W  = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;    // 3

  logic [1:0]          s1_src;
  logic [1:0]          s2_src;
  logic [TAG_W-1:0]    slot_tag_n;
  logic [PGEN_W-1:0]   slot_gen_n;
  logic [IDX_W-1:0]    slot_rob_index_n;
  logic [RGEN_W-1:0]   slot_rob_gen_n;
  logic [UOP_W-1:0]    slot_uop_index_n;

  // The module's widths are `localparam`s taken from the generated packages, so
  // they are not overridable here on purpose: a second place to write the
  // geometry down is a second geometry.
  mosaic_cluster_bypass u_bypass (
      .clk            (clk),
      .rst            (rst),
      .bp_en          (bp_en_i),

      .p_valid        (p_valid_i),
      .p_authorised   (p_authorised_i),
      .p_tag          (TAG_W'(p_tag_i)),
      .p_gen          (PGEN_W'(p_gen_i)),
      .p_rob_index    (IDX_W'(p_rob_index_i)),
      .p_rob_gen      (RGEN_W'(p_rob_gen_i)),
      .p_uop_index    (UOP_W'(p_uop_index_i)),
      .p_value        (XLEN'(p_value_i)),

      .flush          (flush_i),

      .w_valid        (w_valid_i),
      .w_tag          (TAG_W'(w_tag_i)),
      .w_gen          (PGEN_W'(w_gen_i)),
      .w_val          (XLEN'(w_val_i)),

      .c_valid        (c_valid_i),
      .c_s1_tag       (TAG_W'(c_s1_tag_i)),
      .c_s1_gen       (PGEN_W'(c_s1_gen_i)),
      .c_s1_need      (c_s1_need_i),
      .c_s2_tag       (TAG_W'(c_s2_tag_i)),
      .c_s2_gen       (PGEN_W'(c_s2_gen_i)),
      .c_s2_need      (c_s2_need_i),

      .bp_s1_hit      (bp_s1_hit_o),
      .bp_s2_hit      (bp_s2_hit_o),
      .fb_s1_sel      (fb_s1_sel_o),
      .fb_s2_sel      (fb_s2_sel_o),
      .s1_rdy         (s1_rdy_o),
      .s2_rdy         (s2_rdy_o),
      .s1_val         (s1_val_o),
      .s2_val         (s2_val_o),
      .s1_src         (s1_src),
      .s2_src         (s2_src),

      .slot_valid     (slot_valid_o),
      .slot_tag       (slot_tag_n),
      .slot_gen       (slot_gen_n),
      .slot_rob_index (slot_rob_index_n),
      .slot_rob_gen   (slot_rob_gen_n),
      .slot_uop_index (slot_uop_index_n),

      .o_slot_captured(o_slot_captured_o),
      .o_unauth       (o_unauth_o),
      .o_hit_ctr      (o_hit_ctr_o),
      .o_miss_ctr     (o_miss_ctr_o),
      .o_unauth_ctr   (o_unauth_ctr_o),
      .o_id_reject_ctr(o_id_reject_ctr_o),
      .o_flush_ctr    (o_flush_ctr_o)
  );

  assign s1_src_o = 32'(s1_src);
  assign s2_src_o = 32'(s2_src);

  assign slot_tag_o       = 32'(slot_tag_n);
  assign slot_gen_o       = 32'(slot_gen_n);
  assign slot_rob_index_o = 32'(slot_rob_index_n);
  assign slot_rob_gen_o   = 32'(slot_rob_gen_n);
  assign slot_uop_index_o = 32'(slot_uop_index_n);

  assign o_xlen_o   = 32'(XLEN);
  assign o_tag_w_o  = 32'(TAG_W);
  assign o_pgen_w_o = 32'(PGEN_W);
  assign o_idx_w_o  = 32'(IDX_W);
  assign o_rgen_w_o = 32'(RGEN_W);
  assign o_uop_w_o  = 32'(UOP_W);

endmodule : mosaic_cluster_bypass_tb

`default_nettype wire
