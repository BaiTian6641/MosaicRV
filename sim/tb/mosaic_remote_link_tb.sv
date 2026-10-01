// Simulation wrapper for CASE=remote.kill_with_delayed_response (work package
// I-028).
//
// `mosaic_remote_link` is a stateful single-clock module whose two payload
// channels are registered in both directions and whose observation ports expose
// every register it owns. This wrapper adds no timing of its own: it passes
// every port straight through, so the C++ driver owns the clock, the reset
// schedule, the remote unit's behaviour and all reporting, per
// sim/common/sim_common.h.
//
// It does two things of its own, both about geometry:
//
//   * The payload widths are **derived from the frozen identity widths** in
//     `mosaic_uop_pkg` rather than written out by hand. A hand-written "228
//     bits" here would be a second copy of an arithmetic the RTL derives from
//     the same package, and a profile change would then show up as a silently
//     truncated connection rather than as a build failure. The package is
//     `include`d rather than depended on from another file's scope so that this
//     file elaborates on its own; it carries its own include guard, and it
//     includes the generated packages, so nothing else needs to be included
//     here.
//
//   * The widths this file computes are exported as `tb_*` ports. The C++
//     driver compares them against the widths the DUT reports from its own
//     parameter list, so a disagreement between the wrapper's arithmetic and
//     the RTL's is caught at time zero instead of by a slice nobody looks at.
//
// Every observation port the RTL exposes is forwarded, including the two
// pipeline vectors, so the driver can compare the *whole* unit state each cycle
// rather than the projection that happens to be visible on the boundary.
//
// There is deliberately no clock generation, no reset generation and no
// `$display` in here: the C++ side owns all of that.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The package declares one localparam per configuration knob; this wrapper
// names a handful of them and the rest belong to modules that are not here.
`include "mosaic_uop_pkg.sv"
/* verilator lint_on UNUSEDPARAM */

// Fully qualified package references, never `import`: one form everywhere, and
// the form Yosys can parse.
localparam int unsigned TB_ID_W   = mosaic_uop_pkg::HART_W + mosaic_uop_pkg::ROB_INDEX_W
                                  + mosaic_uop_pkg::ROB_GEN_W + mosaic_uop_pkg::UOP_INDEX_W;
localparam int unsigned TB_DST_W  = mosaic_uop_pkg::TAG_W + mosaic_uop_pkg::GEN_W;
localparam int unsigned TB_OP_W   = 4;   // mosaic_pkg::alu_op_e, sixteen encodings
localparam int unsigned TB_WORD_W = mosaic_uop_pkg::XLEN;
localparam int unsigned TB_ENTRIES = mosaic_cfg_pkg::MOSAIC_IQ_ENTRIES;
localparam int unsigned TB_LINK_W  = (TB_ENTRIES <= 1) ? 1 : $clog2(TB_ENTRIES);

// Request: identity, destination, opcode, immediate, source 1, source 2. The
// link id is written by the link and travels above all of it. Response: the
// same identity and destination, the value, the fault bit, and the link id.
localparam int unsigned TB_REQ_BODY_W = TB_ID_W + TB_DST_W + TB_OP_W + 3 * TB_WORD_W;
localparam int unsigned TB_REQ_W      = TB_REQ_BODY_W + TB_LINK_W;
localparam int unsigned TB_RSP_W      = TB_ID_W + TB_DST_W + TB_WORD_W + 1 + TB_LINK_W;

// The pipeline depth, mirroring the RTL's own build-time switch. The mutant
// builds change the depth in the RTL, and this file has to *size a port* for
// it, so the choice is repeated here -- and then reported to the driver, which
// compares it with the depth the DUT elaborated. A silent disagreement is not
// possible: it fails the geometry phase.
`ifdef MOSAIC_REMOTE_LINK_MUTANT_ONE_CYCLE
  localparam int unsigned TB_PIPE_DEPTH = 1;
`else
  localparam int unsigned TB_PIPE_DEPTH = (mosaic_cfg_pkg::MOSAIC_REMOTE_LATENCY < 1)
                                          ? 1 : mosaic_cfg_pkg::MOSAIC_REMOTE_LATENCY;
`endif

module mosaic_remote_link_tb (
    input  logic                            clk,
    input  logic                            rst,

    // home request port
    input  logic                            req_valid,
    output logic                            req_ready,
    input  logic [TB_REQ_BODY_W-1:0]        req_payload,

    // remote request port
    output logic                            rem_req_valid,
    input  logic                            rem_req_ready,
    output logic [TB_REQ_W-1:0]             rem_req_payload,

    // remote response port
    input  logic                            rem_rsp_valid,
    output logic                            rem_rsp_ready,
    input  logic [TB_RSP_W-1:0]             rem_rsp_payload,

    // home response port
    output logic                            rsp_valid,
    input  logic                            rsp_ready,
    output logic [TB_RSP_W-1:0]             rsp_payload,

    // kill / flush
    input  logic                            kill_valid,
    input  logic [TB_ID_W-1:0]              kill_ident,
    input  logic                            flush_valid,

    // credit accounts
    output logic [31:0]                     o_issued_ctr,
    output logic [31:0]                     o_returned_ctr,
    output logic [31:0]                     o_matched_ctr,
    output logic [31:0]                     o_killed_ctr,
    output logic [31:0]                     o_outstanding,
    output logic [31:0]                     o_stale_rsp_ctr,
    output logic [31:0]                     o_alias_rsp_ctr,
    output logic [31:0]                     o_delivered_ctr,
    output logic [31:0]                     o_kill_missed_ctr,
    output logic [31:0]                     o_flush_ctr,
    output logic [31:0]                     o_req_stall_ctr,
    output logic [31:0]                     o_rsp_stall_ctr,

    // whole state
    output logic [TB_ENTRIES-1:0]           o_entry_valid,
    output logic [TB_ENTRIES*TB_ID_W-1:0]   o_entry_id,
    output logic [TB_ENTRIES*TB_DST_W-1:0]  o_entry_dst,
    output logic [TB_PIPE_DEPTH-1:0]        o_req_pipe_valid,
    output logic [TB_PIPE_DEPTH*TB_REQ_W-1:0] o_req_pipe,
    output logic [TB_PIPE_DEPTH-1:0]        o_rsp_pipe_valid,
    output logic [TB_PIPE_DEPTH*TB_RSP_W-1:0] o_rsp_pipe,

    // elaborated geometry, as the DUT reports it
    output logic [31:0]                     o_entries,
    output logic [31:0]                     o_latency,
    output logic [31:0]                     o_pipe_depth,
    output logic [31:0]                     o_id_w,
    output logic [31:0]                     o_dst_w,
    output logic [31:0]                     o_op_w,
    output logic [31:0]                     o_word_w,
    output logic [31:0]                     o_link_id_w,
    output logic [31:0]                     o_req_body_w,
    output logic [31:0]                     o_req_w,
    output logic [31:0]                     o_rsp_w,
    output logic [31:0]                     o_cnt_w,
    output logic [31:0]                     o_req_id_lo,
    output logic [31:0]                     o_req_dst_lo,
    output logic [31:0]                     o_req_op_lo,
    output logic [31:0]                     o_req_imm_lo,
    output logic [31:0]                     o_req_s1_lo,
    output logic [31:0]                     o_req_s2_lo,
    output logic [31:0]                     o_req_lid_lo,
    output logic [31:0]                     o_rsp_id_lo,
    output logic [31:0]                     o_rsp_dst_lo,
    output logic [31:0]                     o_rsp_val_lo,
    output logic [31:0]                     o_rsp_flt_lo,
    output logic [31:0]                     o_rsp_lid_lo,

    // geometry as this file computed it, for the driver to compare
    output logic [31:0]                     tb_req_body_w,
    output logic [31:0]                     tb_req_w,
    output logic [31:0]                     tb_rsp_w,
    output logic [31:0]                     tb_pipe_depth,
    output logic [31:0]                     tb_entries,
    output logic [31:0]                     tb_id_w,
    output logic [31:0]                     tb_dst_w
);

  mosaic_remote_link u_link (
      .clk              (clk),
      .rst              (rst),

      .req_valid        (req_valid),
      .req_ready        (req_ready),
      .req_payload      (req_payload),

      .rem_req_valid    (rem_req_valid),
      .rem_req_ready    (rem_req_ready),
      .rem_req_payload  (rem_req_payload),

      .rem_rsp_valid    (rem_rsp_valid),
      .rem_rsp_ready    (rem_rsp_ready),
      .rem_rsp_payload  (rem_rsp_payload),

      .rsp_valid        (rsp_valid),
      .rsp_ready        (rsp_ready),
      .rsp_payload      (rsp_payload),

      .kill_valid       (kill_valid),
      .kill_ident       (kill_ident),
      .flush_valid      (flush_valid),

      .o_issued_ctr     (o_issued_ctr),
      .o_returned_ctr   (o_returned_ctr),
      .o_matched_ctr    (o_matched_ctr),
      .o_killed_ctr     (o_killed_ctr),
      .o_outstanding    (o_outstanding),
      .o_stale_rsp_ctr  (o_stale_rsp_ctr),
      .o_alias_rsp_ctr  (o_alias_rsp_ctr),
      .o_delivered_ctr  (o_delivered_ctr),
      .o_kill_missed_ctr(o_kill_missed_ctr),
      .o_flush_ctr      (o_flush_ctr),
      .o_req_stall_ctr  (o_req_stall_ctr),
      .o_rsp_stall_ctr  (o_rsp_stall_ctr),

      .o_entry_valid    (o_entry_valid),
      .o_entry_id       (o_entry_id),
      .o_entry_dst      (o_entry_dst),
      .o_req_pipe_valid (o_req_pipe_valid),
      .o_req_pipe       (o_req_pipe),
      .o_rsp_pipe_valid (o_rsp_pipe_valid),
      .o_rsp_pipe       (o_rsp_pipe),

      .o_entries        (o_entries),
      .o_latency        (o_latency),
      .o_pipe_depth     (o_pipe_depth),
      .o_id_w           (o_id_w),
      .o_dst_w          (o_dst_w),
      .o_op_w           (o_op_w),
      .o_word_w         (o_word_w),
      .o_link_id_w      (o_link_id_w),
      .o_req_body_w     (o_req_body_w),
      .o_req_w          (o_req_w),
      .o_rsp_w          (o_rsp_w),
      .o_cnt_w          (o_cnt_w),
      .o_req_id_lo      (o_req_id_lo),
      .o_req_dst_lo     (o_req_dst_lo),
      .o_req_op_lo      (o_req_op_lo),
      .o_req_imm_lo     (o_req_imm_lo),
      .o_req_s1_lo      (o_req_s1_lo),
      .o_req_s2_lo      (o_req_s2_lo),
      .o_req_lid_lo     (o_req_lid_lo),
      .o_rsp_id_lo      (o_rsp_id_lo),
      .o_rsp_dst_lo     (o_rsp_dst_lo),
      .o_rsp_val_lo     (o_rsp_val_lo),
      .o_rsp_flt_lo     (o_rsp_flt_lo),
      .o_rsp_lid_lo     (o_rsp_lid_lo)
  );

  assign tb_req_body_w = 32'(TB_REQ_BODY_W);
  assign tb_req_w      = 32'(TB_REQ_W);
  assign tb_rsp_w      = 32'(TB_RSP_W);
  assign tb_pipe_depth = 32'(TB_PIPE_DEPTH);
  assign tb_entries    = 32'(TB_ENTRIES);
  assign tb_id_w       = 32'(TB_ID_W);
  assign tb_dst_w      = 32'(TB_DST_W);

endmodule : mosaic_remote_link_tb

`resetall
`default_nettype wire
