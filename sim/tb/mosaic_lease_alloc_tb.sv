// Simulation wrapper for CASE=lease.conflict_and_cancel (work package I-024).
//
// `mosaic_lease_alloc` is a stateful single-clock module whose grant answer is
// combinational (it describes the edge that is about to happen). This wrapper
// adds no timing of its own: every port is passed straight through, and there is
// deliberately no clock generation, no reset generation and no `$display` --
// the C++ side owns the clock, the reset schedule and all result reporting,
// per sim/common/sim_common.h.
//
// About the widths. This wrapper repeats the module's parameter expressions to
// declare its own port widths, which is a second copy of a derivation, so it is
// worth saying why that is acceptable here and what catches it if it drifts:
//
//   * the widths are *expressions over the generated package*, not literals, so
//     they follow a profile change exactly as the module does;
//   * a wrapper whose port width disagrees with the instance's is an
//     elaboration error in Verilator, not a silently narrower comparison. The
//     failure is loud and immediate.
//
// The alternative -- reading the elaborated localparams back out through
// `o_req_count`, `o_pools`, `o_size_*` and friends -- is still done, and
// `sim/unit/tb_lease.cpp` asserts the geometry it was written for. Those ports
// exist so the *shadow model* sizes itself from the DUT rather than from a
// second copy of the geometry; they are not a substitute for the port list here,
// because a port list cannot be sized by an instance's localparam.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project, and it has an include guard, so this second include of it (the
// module includes it too) is a no-op rather than a duplicate declaration.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_REQ      = mosaic_cfg_pkg::MOSAIC_CLUSTERS
                                    + mosaic_cfg_pkg::MOSAIC_MULDIV_UNITS;
localparam int unsigned TB_POOLS    = 4;
localparam int unsigned TB_SIZE_ALU = mosaic_cfg_pkg::MOSAIC_CLUSTERS
                                    * mosaic_cfg_pkg::MOSAIC_ALU_PER_CLUSTER;
localparam int unsigned TB_SIZE_MD  = mosaic_cfg_pkg::MOSAIC_MULDIV_UNITS;
localparam int unsigned TB_SIZE_WB  = mosaic_cfg_pkg::MOSAIC_CLUSTERS
                                    * mosaic_cfg_pkg::MOSAIC_RESULT_FIFO;
localparam int unsigned TB_SIZE_NET = mosaic_cfg_pkg::MOSAIC_CLUSTERS;
localparam int unsigned TB_LEASES   = TB_SIZE_WB;
localparam int unsigned TB_GEN_W    = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1;

localparam int unsigned TB_M_WIDTH  = (TB_SIZE_ALU > TB_SIZE_MD)  ? TB_SIZE_ALU : TB_SIZE_MD;
localparam int unsigned TB_N_WIDTH  = (TB_SIZE_WB  > TB_SIZE_NET) ? TB_SIZE_WB  : TB_SIZE_NET;
localparam int unsigned TB_MAX_SZ   = (TB_M_WIDTH  > TB_N_WIDTH)  ? TB_M_WIDTH  : TB_N_WIDTH;

localparam int unsigned TB_SLOT_W   = $clog2(TB_MAX_SZ);
localparam int unsigned TB_ROW_W    = $clog2(TB_LEASES);
localparam int unsigned TB_IDX_W    = TB_ROW_W + 1;
localparam int unsigned TB_LEASE_W  = TB_IDX_W + TB_GEN_W;
localparam int unsigned TB_RR_W     = $clog2(TB_REQ);

module mosaic_lease_alloc_tb (
    input  logic                                       clk,
    input  logic                                       rst,

    input  logic [TB_REQ-1:0]                          req_valid,
    input  logic [TB_REQ*TB_POOLS-1:0]                 req_mask,
    output logic [TB_REQ-1:0]                          req_ready,
    output logic [TB_REQ*TB_LEASE_W-1:0]               grant_id,
    output logic [TB_REQ*TB_POOLS*TB_SLOT_W-1:0]       grant_slot,

    input  logic                                       rel_valid,
    input  logic [TB_LEASE_W-1:0]                      rel_id,
    output logic                                       rel_ok,
    output logic                                       rel_stale,
    output logic                                       rel_repeat,
    output logic                                       rel_after_cancel,

    input  logic                                       can_valid,
    input  logic [TB_LEASE_W-1:0]                      can_id,
    output logic                                       can_ok,
    output logic                                       can_stale,
    output logic                                       can_repeat,
    output logic                                       can_after_release,

    output logic [TB_POOLS*TB_MAX_SZ-1:0]              o_occ,
    output logic [31:0]                                o_occ_count_alu,
    output logic [31:0]                                o_occ_count_md,
    output logic [31:0]                                o_occ_count_wb,
    output logic [31:0]                                o_occ_count_net,
    output logic [TB_LEASES-1:0]                       o_lease_live,
    output logic [TB_LEASES-1:0]                       o_lease_used,
    output logic [TB_LEASES-1:0]                       o_lease_end_kind,
    output logic [TB_LEASES*TB_GEN_W-1:0]              o_lease_gen,
    output logic [TB_LEASES*TB_POOLS-1:0]              o_lease_mask,
    output logic [TB_LEASES*TB_POOLS*TB_SLOT_W-1:0]    o_lease_slot,
    output logic [TB_RR_W-1:0]                         o_rr,
    output logic [31:0]                                o_grant_count,
    output logic [31:0]                                o_rel_ok_count,
    output logic [31:0]                                o_can_ok_count,
    output logic [31:0]                                o_rel_stale_count,
    output logic [31:0]                                o_can_stale_count,
    output logic [31:0]                                o_rel_repeat_count,
    output logic [31:0]                                o_can_repeat_count,
    output logic [31:0]                                o_rel_after_cancel_count,
    output logic [31:0]                                o_can_after_release_count,
    output logic [31:0]                                o_live_count,

    output logic [31:0]                                o_req_count,
    output logic [31:0]                                o_pools,
    output logic [31:0]                                o_leases,
    output logic [31:0]                                o_max_sz,
    output logic [31:0]                                o_gen_w,
    output logic [31:0]                                o_slot_w,
    output logic [31:0]                                o_lease_w,
    output logic [31:0]                                o_size_alu,
    output logic [31:0]                                o_size_md,
    output logic [31:0]                                o_size_wb,
    output logic [31:0]                                o_size_net,
    output logic [31:0]                                o_dbg0,
    output logic [31:0]                                o_dbg1
);

  mosaic_lease_alloc u_lease (
      .clk                    (clk),
      .rst                    (rst),
      .req_valid              (req_valid),
      .req_mask               (req_mask),
      .req_ready              (req_ready),
      .grant_id               (grant_id),
      .grant_slot             (grant_slot),
      .rel_valid              (rel_valid),
      .rel_id                 (rel_id),
      .rel_ok                 (rel_ok),
      .rel_stale              (rel_stale),
      .rel_repeat             (rel_repeat),
      .rel_after_cancel       (rel_after_cancel),
      .can_valid              (can_valid),
      .can_id                 (can_id),
      .can_ok                 (can_ok),
      .can_stale              (can_stale),
      .can_repeat             (can_repeat),
      .can_after_release      (can_after_release),
      .o_occ                  (o_occ),
      .o_occ_count_alu        (o_occ_count_alu),
      .o_occ_count_md         (o_occ_count_md),
      .o_occ_count_wb         (o_occ_count_wb),
      .o_occ_count_net        (o_occ_count_net),
      .o_lease_live           (o_lease_live),
      .o_lease_used           (o_lease_used),
      .o_lease_end_kind       (o_lease_end_kind),
      .o_lease_gen            (o_lease_gen),
      .o_lease_mask           (o_lease_mask),
      .o_lease_slot           (o_lease_slot),
      .o_rr                   (o_rr),
      .o_grant_count          (o_grant_count),
      .o_rel_ok_count         (o_rel_ok_count),
      .o_can_ok_count         (o_can_ok_count),
      .o_rel_stale_count      (o_rel_stale_count),
      .o_can_stale_count      (o_can_stale_count),
      .o_rel_repeat_count     (o_rel_repeat_count),
      .o_can_repeat_count     (o_can_repeat_count),
      .o_rel_after_cancel_count (o_rel_after_cancel_count),
      .o_can_after_release_count(o_can_after_release_count),
      .o_live_count           (o_live_count),
      .o_req_count            (o_req_count),
      .o_pools                (o_pools),
      .o_leases               (o_leases),
      .o_max_sz               (o_max_sz),
      .o_gen_w                (o_gen_w),
      .o_slot_w               (o_slot_w),
      .o_lease_w              (o_lease_w),
      .o_size_alu             (o_size_alu),
      .o_size_md              (o_size_md),
      .o_size_wb              (o_size_wb),
      .o_size_net             (o_size_net),
      .o_dbg0                 (o_dbg0),
      .o_dbg1                 (o_dbg1)
  );

endmodule : mosaic_lease_alloc_tb

`resetall
`default_nettype wire
