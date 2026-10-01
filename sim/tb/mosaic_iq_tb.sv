// ============================================================================
// mosaic_iq_tb -- unit-test wrapper for CASE=iq.wakeup_insert_select, work
// package I-022.
//
// This is simulation-only glue and it contains no behaviour of its own. It
// instantiates **two** `mosaic_iq` instances, one per cluster:
//
//   c0  the queue under test
//   c1  an identical second queue, driven identically in the directed phases
//       and differently in the randomised phase
//
// Two instances buy three things at once. The card says "initial: local FUs
// only", and MOSAIC_CLUSTERS = 2 is the geometry that claim has to be true in,
// so the two queues must be genuinely independent: a wakeup aimed at cluster 0
// must not make anything ready in cluster 1, and a kill in cluster 0 must not
// free a slot in cluster 1. A single instance cannot demonstrate that, because
// there is no other queue to *not* disturb. And because both instances run the
// same directed stimulus through the same C++ shadow, a defect that depends on
// a particular occupancy is visible in one instance while the other still
// disagrees with its own shadow, so a bug cannot cancel out between them.
//
// Everything else is a straight pass-through: every port is driven by
// sim/unit/tb_iq.cpp or comes straight out of a DUT output. There is no clock
// generation, no reset generation and no `$display` in here; the C++ side owns
// the clock, the reset schedule and all result reporting, per
// sim/common/sim_common.h.
// ============================================================================

`default_nettype none
`resetall

// The generated package is *not* included here. rtl/core/mosaic_iq.sv already
// includes it, and a second `include of a header with no include guard is a
// duplicate package declaration. The geometry this wrapper needs is read back
// out of the instances below and exported on `o_*_entries` etc, so the C++
// driver learns the sizes from the hardware that exists rather than from a
// second transcription of them. See the note above those outputs.


module mosaic_iq_tb (
  input  logic clk,
  input  logic rst,

  // ================= cluster 0: insert =======================================
  input  logic         c0_ins_valid,
  output logic         c0_ins_ready,
  input  logic [15:0]  c0_ins_uop,
  input  logic [3:0]   c0_ins_alu_op,
  input  logic [63:0]  c0_ins_imm,
  input  logic [6:0]   c0_ins_src1_tag,
  input  logic [6:0]   c0_ins_src1_gen,
  input  logic         c0_ins_src1_ready,
  input  logic [63:0]  c0_ins_src1_val,
  input  logic [6:0]   c0_ins_src2_tag,
  input  logic [6:0]   c0_ins_src2_gen,
  input  logic         c0_ins_src2_ready,
  input  logic [63:0]  c0_ins_src2_val,
  input  logic [6:0]   c0_ins_dst_tag,
  input  logic [6:0]   c0_ins_dst_gen,

  // ================= cluster 0: wakeup =======================================
  input  logic         c0_wu_valid,
  input  logic [6:0]   c0_wu_tag,
  input  logic [6:0]   c0_wu_gen,
  input  logic [63:0]  c0_wu_val,

  // ================= cluster 0: grant ========================================
  output logic         c0_grant_valid,
  input  logic         c0_grant_ready,
  output logic [15:0]  c0_grant_uop,
  output logic [3:0]   c0_grant_alu_op,
  output logic [63:0]  c0_grant_imm,
  output logic [63:0]  c0_grant_a,
  output logic [63:0]  c0_grant_b,
  output logic [6:0]   c0_grant_dst_tag,
  output logic [6:0]   c0_grant_dst_gen,
  output logic [2:0]   c0_grant_index,

  // ================= cluster 0: kill ========================================
  input  logic         c0_kill_valid,
  input  logic [5:0]   c0_kill_rob_index,
  input  logic [6:0]   c0_kill_rob_gen,
  input  logic         c0_kill_younger,

  // ================= cluster 0: status ======================================
  output logic [7:0]   c0_occupied,
  output logic [3:0]   c0_count,
  output logic         c0_full,
  output logic         c0_dst_conflict,
  output logic [4:0]   c0_age_ctr,
  output logic [2:0]   c0_alloc_index,

  // ================= cluster 0: observation ================================
  input  logic [2:0]   c0_obs_index,
  output logic         c0_obs_valid,
  output logic [4:0]   c0_obs_age,
  output logic         c0_obs_ready,
  output logic         c0_obs_granted,
  output logic [6:0]   c0_obs_src1_tag,
  output logic [6:0]   c0_obs_src1_gen,
  output logic [6:0]   c0_obs_src2_tag,
  output logic [6:0]   c0_obs_src2_gen,
  output logic [15:0]  c0_obs_uop,
  output logic [3:0]   c0_obs_alu_op,
  output logic [63:0]  c0_obs_imm,
  output logic [6:0]   c0_obs_dst_tag,
  output logic [6:0]   c0_obs_dst_gen,
  output logic         c0_obs_src1_ready,
  output logic         c0_obs_src2_ready,
  output logic [63:0]  c0_obs_src1_val,
  output logic [63:0]  c0_obs_src2_val,

  // ================= cluster 0: counters ====================================
  output logic [31:0]  c0_ins_total,
  output logic [31:0]  c0_grant_total,
  output logic [31:0]  c0_kill_total,
  output logic [31:0]  c0_wu_total,
  output logic [31:0]  c0_wu_matched,
  output logic [31:0]  c0_wu_dup,
  output logic [31:0]  c0_wu_stale,
  output logic [31:0]  c0_wu_miss,

  // ================= cluster 1: insert ======================================
  input  logic         c1_ins_valid,
  output logic         c1_ins_ready,
  input  logic [15:0]  c1_ins_uop,
  input  logic [3:0]   c1_ins_alu_op,
  input  logic [63:0]  c1_ins_imm,
  input  logic [6:0]   c1_ins_src1_tag,
  input  logic [6:0]   c1_ins_src1_gen,
  input  logic         c1_ins_src1_ready,
  input  logic [63:0]  c1_ins_src1_val,
  input  logic [6:0]   c1_ins_src2_tag,
  input  logic [6:0]   c1_ins_src2_gen,
  input  logic         c1_ins_src2_ready,
  input  logic [63:0]  c1_ins_src2_val,
  input  logic [6:0]   c1_ins_dst_tag,
  input  logic [6:0]   c1_ins_dst_gen,

  // ================= cluster 1: wakeup ======================================
  input  logic         c1_wu_valid,
  input  logic [6:0]   c1_wu_tag,
  input  logic [6:0]   c1_wu_gen,
  input  logic [63:0]  c1_wu_val,

  // ================= cluster 1: grant =======================================
  output logic         c1_grant_valid,
  input  logic         c1_grant_ready,
  output logic [15:0]  c1_grant_uop,
  output logic [3:0]   c1_grant_alu_op,
  output logic [63:0]  c1_grant_imm,
  output logic [63:0]  c1_grant_a,
  output logic [63:0]  c1_grant_b,
  output logic [6:0]   c1_grant_dst_tag,
  output logic [6:0]   c1_grant_dst_gen,
  output logic [2:0]   c1_grant_index,

  // ================= cluster 1: kill =======================================
  input  logic         c1_kill_valid,
  input  logic [5:0]   c1_kill_rob_index,
  input  logic [6:0]   c1_kill_rob_gen,
  input  logic         c1_kill_younger,

  // ================= cluster 1: status =====================================
  output logic [7:0]   c1_occupied,
  output logic [3:0]   c1_count,
  output logic         c1_full,
  output logic         c1_dst_conflict,
  output logic [4:0]   c1_age_ctr,
  output logic [2:0]   c1_alloc_index,

  // ================= cluster 1: observation ================================
  input  logic [2:0]   c1_obs_index,
  output logic         c1_obs_valid,
  output logic [4:0]   c1_obs_age,
  output logic         c1_obs_ready,
  output logic         c1_obs_granted,
  output logic [6:0]   c1_obs_src1_tag,
  output logic [6:0]   c1_obs_src1_gen,
  output logic [6:0]   c1_obs_src2_tag,
  output logic [6:0]   c1_obs_src2_gen,
  output logic [15:0]  c1_obs_uop,
  output logic [3:0]   c1_obs_alu_op,
  output logic [63:0]  c1_obs_imm,
  output logic [6:0]   c1_obs_dst_tag,
  output logic [6:0]   c1_obs_dst_gen,
  output logic         c1_obs_src1_ready,
  output logic         c1_obs_src2_ready,
  output logic [63:0]  c1_obs_src1_val,
  output logic [63:0]  c1_obs_src2_val,

  // ================= cluster 1: counters ====================================
  output logic [31:0]  c1_ins_total,
  output logic [31:0]  c1_grant_total,
  output logic [31:0]  c1_kill_total,
  output logic [31:0]  c1_wu_total,
  output logic [31:0]  c1_wu_matched,
  output logic [31:0]  c1_wu_dup,
  output logic [31:0]  c1_wu_stale,
  output logic [31:0]  c1_wu_miss,

  // ================= geometry readback =====================================
  // The elaborated geometry, read out of the instance hierarchy rather than out
  // of a second copy of the generated package. sim/unit/tb_iq.cpp asserts these
  // against the sizes it was written for, so a profile change that this wrapper
  // was not updated for fails loudly instead of quietly testing the wrong
  // number of entries.
  output logic [31:0]  o_iq_entries,
  output logic [31:0]  o_iq_clusters,
  output logic [31:0]  o_iq_xlen,
  output logic [31:0]  o_iq_tag_w,
  output logic [31:0]  o_iq_rob_index_w,
  output logic [31:0]  o_iq_age_w,
  output logic [31:0]  o_iq_uop_id_w,
  output logic [31:0]  o_iq_idx_w,
  output logic [31:0]  o_iq_cnt_w
);


  // The number of clusters instantiated below. This is the one number the
  // wrapper cannot read back, because it is what the readback is counted
  // against -- so it is written out, and `o_iq_clusters` reports the count the
  // loop actually produced so the driver can check it. The p0 profile's
  // MOSAIC_CLUSTERS is 2 and this is that number.
  localparam int unsigned TB_CLUSTERS = 2;

  // A generate loop rather than two hand-written instances, so the selector
  // wiring is written once and the instance list cannot drift from it. The
  // `c == 0` / `else` split inside is what maps the loop index onto the
  // per-cluster top-level pin groups.
  for (genvar c = 0; c < TB_CLUSTERS; c++) begin : g_cluster

    // Selector for the current cluster: driven from the top-level pins of the
    // matching prefix by the two assign blocks below.
    logic ins_valid_s, ins_ready_s;
    logic [15:0] ins_uop_s;
    logic [3:0] ins_alu_op_s;
    logic [63:0] ins_imm_s;
    logic [6:0] ins_src1_tag_s, ins_src2_tag_s, ins_dst_tag_s;
    logic [6:0] ins_src1_gen_s, ins_src2_gen_s, ins_dst_gen_s;
    logic ins_src1_ready_s, ins_src2_ready_s;
    logic [63:0] ins_src1_val_s, ins_src2_val_s;

    logic wu_valid_s;
    logic [6:0] wu_tag_s, wu_gen_s;
    logic [63:0] wu_val_s;

    logic grant_valid_s, grant_ready_s;
    logic [15:0] grant_uop_s;
    logic [3:0] grant_alu_op_s;
    logic [63:0] grant_imm_s, grant_a_s, grant_b_s;
    logic [6:0] grant_dst_tag_s, grant_dst_gen_s;
    logic [2:0] grant_index_s;

    logic kill_valid_s, kill_younger_s;
    logic [5:0] kill_rob_index_s;
    logic [6:0] kill_rob_gen_s;

    logic [7:0] occupied_s;         // one bit per slot; DEPTH = 8 for p0
    logic [3:0] count_s;
    logic full_s, dst_conflict_s;
    logic [4:0] age_ctr_s;
    logic [2:0] alloc_index_s;

    logic [2:0] obs_index_s;
    logic obs_valid_s, obs_ready_s, obs_granted_s;
    logic [6:0] obs_src1_tag_s, obs_src1_gen_s, obs_src2_tag_s, obs_src2_gen_s;
    logic [4:0] obs_age_s;
    logic [15:0] obs_uop_s;
    logic [3:0] obs_alu_op_s;
    logic [63:0] obs_imm_s;
    logic [6:0] obs_dst_tag_s, obs_dst_gen_s;
    logic obs_src1_ready_s, obs_src2_ready_s;
    logic [63:0] obs_src1_val_s, obs_src2_val_s;

    logic [31:0] ins_total_s, grant_total_s, kill_total_s;
    logic [31:0] wu_total_s, wu_matched_s, wu_dup_s, wu_stale_s, wu_miss_s;

    if (c == 0) begin : g_c0
      assign ins_valid_s    = c0_ins_valid;
      assign ins_uop_s      = c0_ins_uop;
      assign ins_alu_op_s   = c0_ins_alu_op;
      assign ins_imm_s      = c0_ins_imm;
      assign ins_src1_tag_s = c0_ins_src1_tag;
      assign ins_src1_gen_s = c0_ins_src1_gen;
      assign ins_src1_ready_s = c0_ins_src1_ready;
      assign ins_src1_val_s = c0_ins_src1_val;
      assign ins_src2_tag_s = c0_ins_src2_tag;
      assign ins_src2_gen_s = c0_ins_src2_gen;
      assign ins_src2_ready_s = c0_ins_src2_ready;
      assign ins_src2_val_s = c0_ins_src2_val;
      assign ins_dst_tag_s  = c0_ins_dst_tag;
      assign ins_dst_gen_s  = c0_ins_dst_gen;
      assign wu_valid_s     = c0_wu_valid;
      assign wu_tag_s       = c0_wu_tag;
      assign wu_gen_s       = c0_wu_gen;
      assign wu_val_s       = c0_wu_val;
      assign grant_ready_s  = c0_grant_ready;
      assign kill_valid_s   = c0_kill_valid;
      assign kill_rob_index_s = c0_kill_rob_index;
      assign kill_rob_gen_s = c0_kill_rob_gen;
      assign kill_younger_s = c0_kill_younger;
      assign obs_index_s    = c0_obs_index;
      assign c0_ins_ready   = ins_ready_s;
      assign c0_grant_valid = grant_valid_s;
      assign c0_grant_uop   = grant_uop_s;
      assign c0_grant_alu_op= grant_alu_op_s;
      assign c0_grant_imm   = grant_imm_s;
      assign c0_grant_a     = grant_a_s;
      assign c0_grant_b     = grant_b_s;
      assign c0_grant_dst_tag = grant_dst_tag_s;
      assign c0_grant_dst_gen = grant_dst_gen_s;
      assign c0_grant_index = grant_index_s;
      assign c0_occupied    = occupied_s;
      assign c0_count       = count_s;
      assign c0_full        = full_s;
      assign c0_dst_conflict= dst_conflict_s;
      assign c0_age_ctr     = age_ctr_s;
      assign c0_alloc_index = alloc_index_s;
      assign c0_obs_valid   = obs_valid_s;
      assign c0_obs_age     = obs_age_s;
      assign c0_obs_ready   = obs_ready_s;
      assign c0_obs_granted = obs_granted_s;
      assign c0_obs_src1_tag = obs_src1_tag_s;
      assign c0_obs_src1_gen = obs_src1_gen_s;
      assign c0_obs_src2_tag = obs_src2_tag_s;
      assign c0_obs_src2_gen = obs_src2_gen_s;
      assign c0_obs_uop     = obs_uop_s;
      assign c0_obs_alu_op  = obs_alu_op_s;
      assign c0_obs_imm     = obs_imm_s;
      assign c0_obs_dst_tag = obs_dst_tag_s;
      assign c0_obs_dst_gen = obs_dst_gen_s;
      assign c0_obs_src1_ready = obs_src1_ready_s;
      assign c0_obs_src2_ready = obs_src2_ready_s;
      assign c0_obs_src1_val = obs_src1_val_s;
      assign c0_obs_src2_val = obs_src2_val_s;
      assign c0_ins_total   = ins_total_s;
      assign c0_grant_total = grant_total_s;
      assign c0_kill_total  = kill_total_s;
      assign c0_wu_total    = wu_total_s;
      assign c0_wu_matched  = wu_matched_s;
      assign c0_wu_dup      = wu_dup_s;
      assign c0_wu_stale    = wu_stale_s;
      assign c0_wu_miss     = wu_miss_s;
    end else begin : g_c1
      assign ins_valid_s    = c1_ins_valid;
      assign ins_uop_s      = c1_ins_uop;
      assign ins_alu_op_s   = c1_ins_alu_op;
      assign ins_imm_s      = c1_ins_imm;
      assign ins_src1_tag_s = c1_ins_src1_tag;
      assign ins_src1_gen_s = c1_ins_src1_gen;
      assign ins_src1_ready_s = c1_ins_src1_ready;
      assign ins_src1_val_s = c1_ins_src1_val;
      assign ins_src2_tag_s = c1_ins_src2_tag;
      assign ins_src2_gen_s = c1_ins_src2_gen;
      assign ins_src2_ready_s = c1_ins_src2_ready;
      assign ins_src2_val_s = c1_ins_src2_val;
      assign ins_dst_tag_s  = c1_ins_dst_tag;
      assign ins_dst_gen_s  = c1_ins_dst_gen;
      assign wu_valid_s     = c1_wu_valid;
      assign wu_tag_s       = c1_wu_tag;
      assign wu_gen_s       = c1_wu_gen;
      assign wu_val_s       = c1_wu_val;
      assign grant_ready_s  = c1_grant_ready;
      assign kill_valid_s   = c1_kill_valid;
      assign kill_rob_index_s = c1_kill_rob_index;
      assign kill_rob_gen_s = c1_kill_rob_gen;
      assign kill_younger_s = c1_kill_younger;
      assign obs_index_s    = c1_obs_index;
      assign c1_ins_ready   = ins_ready_s;
      assign c1_grant_valid = grant_valid_s;
      assign c1_grant_uop   = grant_uop_s;
      assign c1_grant_alu_op= grant_alu_op_s;
      assign c1_grant_imm   = grant_imm_s;
      assign c1_grant_a     = grant_a_s;
      assign c1_grant_b     = grant_b_s;
      assign c1_grant_dst_tag = grant_dst_tag_s;
      assign c1_grant_dst_gen = grant_dst_gen_s;
      assign c1_grant_index = grant_index_s;
      assign c1_occupied    = occupied_s;
      assign c1_count       = count_s;
      assign c1_full        = full_s;
      assign c1_dst_conflict= dst_conflict_s;
      assign c1_age_ctr     = age_ctr_s;
      assign c1_alloc_index = alloc_index_s;
      assign c1_obs_valid   = obs_valid_s;
      assign c1_obs_age     = obs_age_s;
      assign c1_obs_ready   = obs_ready_s;
      assign c1_obs_granted = obs_granted_s;
      assign c1_obs_src1_tag = obs_src1_tag_s;
      assign c1_obs_src1_gen = obs_src1_gen_s;
      assign c1_obs_src2_tag = obs_src2_tag_s;
      assign c1_obs_src2_gen = obs_src2_gen_s;
      assign c1_obs_uop     = obs_uop_s;
      assign c1_obs_alu_op  = obs_alu_op_s;
      assign c1_obs_imm     = obs_imm_s;
      assign c1_obs_dst_tag = obs_dst_tag_s;
      assign c1_obs_dst_gen = obs_dst_gen_s;
      assign c1_obs_src1_ready = obs_src1_ready_s;
      assign c1_obs_src2_ready = obs_src2_ready_s;
      assign c1_obs_src1_val = obs_src1_val_s;
      assign c1_obs_src2_val = obs_src2_val_s;
      assign c1_ins_total   = ins_total_s;
      assign c1_grant_total = grant_total_s;
      assign c1_kill_total  = kill_total_s;
      assign c1_wu_total    = wu_total_s;
      assign c1_wu_matched  = wu_matched_s;
      assign c1_wu_dup      = wu_dup_s;
      assign c1_wu_stale    = wu_stale_s;
      assign c1_wu_miss     = wu_miss_s;
    end

    mosaic_iq u_iq (
      .clk           (clk),
      .rst           (rst),
      .ins_valid     (ins_valid_s),
      .ins_ready     (ins_ready_s),
      .ins_uop       (ins_uop_s),
      .ins_alu_op    (ins_alu_op_s),
      .ins_imm       (ins_imm_s),
      .ins_src1_tag  (ins_src1_tag_s),
      .ins_src1_gen  (ins_src1_gen_s),
      .ins_src1_ready(ins_src1_ready_s),
      .ins_src1_val  (ins_src1_val_s),
      .ins_src2_tag  (ins_src2_tag_s),
      .ins_src2_gen  (ins_src2_gen_s),
      .ins_src2_ready(ins_src2_ready_s),
      .ins_src2_val  (ins_src2_val_s),
      .ins_dst_tag   (ins_dst_tag_s),
      .ins_dst_gen   (ins_dst_gen_s),
      .wu_valid      (wu_valid_s),
      .wu_tag        (wu_tag_s),
      .wu_gen        (wu_gen_s),
      .wu_val        (wu_val_s),
      .grant_valid   (grant_valid_s),
      .grant_ready   (grant_ready_s),
      .grant_uop     (grant_uop_s),
      .grant_alu_op  (grant_alu_op_s),
      .grant_imm     (grant_imm_s),
      .grant_a       (grant_a_s),
      .grant_b       (grant_b_s),
      .grant_dst_tag (grant_dst_tag_s),
      .grant_dst_gen (grant_dst_gen_s),
      .grant_index   (grant_index_s),
      .kill_valid    (kill_valid_s),
      .kill_rob_index(kill_rob_index_s),
      .kill_rob_gen  (kill_rob_gen_s),
      .kill_younger  (kill_younger_s),
      .o_occupied    (occupied_s),
      .o_count       (count_s),
      .o_full        (full_s),
      .o_dst_conflict(dst_conflict_s),
      .o_age_ctr     (age_ctr_s),
      .o_alloc_index (alloc_index_s),
      .obs_index     (obs_index_s),
      .obs_valid     (obs_valid_s),
      .obs_age       (obs_age_s),
      .obs_ready     (obs_ready_s),
      .obs_granted   (obs_granted_s),
      .obs_src1_tag  (obs_src1_tag_s),
      .obs_src1_gen  (obs_src1_gen_s),
      .obs_src2_tag  (obs_src2_tag_s),
      .obs_src2_gen  (obs_src2_gen_s),
      .obs_uop       (obs_uop_s),
      .obs_alu_op    (obs_alu_op_s),
      .obs_imm       (obs_imm_s),
      .obs_dst_tag   (obs_dst_tag_s),
      .obs_dst_gen   (obs_dst_gen_s),
      .obs_src1_ready(obs_src1_ready_s),
      .obs_src2_ready(obs_src2_ready_s),
      .obs_src1_val  (obs_src1_val_s),
      .obs_src2_val  (obs_src2_val_s),
      .o_ins_total   (ins_total_s),
      .o_grant_total (grant_total_s),
      .o_kill_total  (kill_total_s),
      .o_wu_total    (wu_total_s),
      .o_wu_matched  (wu_matched_s),
      .o_wu_dup      (wu_dup_s),
      .o_wu_stale    (wu_stale_s),
      .o_wu_miss     (wu_miss_s)
    );
  end

  // Geometry readback. `g_cluster[0].u_iq` and `g_cluster[1].u_iq` are the same
  // module with the same generated localparams, so reading the first is reading
  // both; the loop count is reported separately so a profile with a different
  // MOSAIC_CLUSTERS is caught rather than assumed away.
  assign o_iq_entries    = 32'(g_cluster[0].u_iq.DEPTH);
  assign o_iq_clusters   = 32'(TB_CLUSTERS);
  assign o_iq_xlen       = 32'(g_cluster[0].u_iq.XLEN);
  assign o_iq_tag_w      = 32'(g_cluster[0].u_iq.TAG_W);
  assign o_iq_rob_index_w= 32'(g_cluster[0].u_iq.ROB_INDEX_W);
  assign o_iq_age_w      = 32'(g_cluster[0].u_iq.AGE_W);
  assign o_iq_uop_id_w   = 32'(g_cluster[0].u_iq.UOP_ID_W);
  assign o_iq_idx_w      = 32'(g_cluster[0].u_iq.IDX_W);
  assign o_iq_cnt_w      = 32'(g_cluster[0].u_iq.CNT_W);

endmodule : mosaic_iq_tb

`resetall
`default_nettype wire
