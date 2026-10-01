// ============================================================================
// Simulation wrapper for CASE=fabric.fixed_two_cluster (work package I-023).
//
// The wrapper adds no timing of its own. It flattens mosaic_core's ports to
// plain vectors so the C++ driver contains no core geometry, and it exposes
// every signal the case asserts on. It also instantiates a **second** DUT: a
// standalone `mosaic_redirect_arb`, driven directly by the driver, so the
// oldest-wins rule can be examined with two simultaneous requests -- something
// the core itself never produces, because a branch is a barrier and only one
// can be outstanding (see mosaic_core.sv).
//
// Geometry is taken from the generated packages by scope reference, never
// re-derived by a formula this wrapper invented (the same rule
// sim/tb/mosaic_rob_tb.sv documents).
// ============================================================================

`default_nettype none
`resetall

localparam int unsigned TB_XLEN     = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned TB_TAG_W    = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
localparam int unsigned TB_IDX_W    = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned TB_RGEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned TB_UOP_W    = 3;
localparam int unsigned TB_UOP_ID_W = TB_IDX_W + TB_RGEN_W + TB_UOP_W;
localparam int unsigned TB_FETCH_N  = mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING;
localparam int unsigned TB_REQ_ID_W = (TB_FETCH_N <= 1) ? 1 : $clog2(TB_FETCH_N);
localparam int unsigned TB_EPOCH_W  = ((mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES <= 1)
                                       ? 2 : $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1);
localparam int unsigned TB_RET_N    = mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH;
localparam int unsigned TB_RET_ID_W = 2 * TB_TAG_W;
localparam int unsigned TB_SEQ_W    = $clog2(2 * mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES + 1);
localparam int unsigned TB_RD_W     = 5;
localparam int unsigned TB_SIZE_W   = 3;
localparam int unsigned TB_OCC_W    = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES + 1);
// Readiness probes: the same generated geometry the core and rename derive, so
// the wrapper never invents a width.
localparam int unsigned TB_ARCH_N   = mosaic_cfg_pkg::MOSAIC_ARCH_INT_REGS;
localparam int unsigned TB_PRF_N    = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
localparam int unsigned TB_MAP_W    = 2 * TB_TAG_W;

/* verilator lint_off PINCONNECTEMPTY */
module mosaic_core_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------- instruction memory port
    input  logic        imem_req_ready_i,
    input  logic        imem_rsp_valid_i,
    input  logic [31:0] imem_rsp_rdata_i,
    input  logic        imem_rsp_fault_i,
    input  logic [TB_REQ_ID_W-1:0]  imem_rsp_id_i,
    input  logic [TB_EPOCH_W-1:0]   imem_rsp_epoch_i,
    input  logic [2:0]  imem_rsp_len_i,
    output logic        imem_req_valid_o,
    output logic        imem_req_we_o,
    output logic [TB_XLEN-1:0] imem_req_addr_o,
    output logic [2:0]  imem_req_size_o,
    output logic [7:0]  imem_req_wstrb_o,
    output logic [TB_XLEN-1:0] imem_req_wdata_o,
    output logic [TB_REQ_ID_W-1:0] imem_req_id_o,
    output logic [TB_EPOCH_W-1:0]  imem_req_epoch_o,
    output logic        imem_rsp_ready_o,

    // ------------------------------------------------------- data memory port
    input  logic        dmem_req_ready_i,
    input  logic        dmem_rsp_valid_i,
    input  logic [TB_XLEN-1:0] dmem_rsp_rdata_i,
    input  logic        dmem_rsp_fault_i,
    output logic        dmem_req_valid_o,
    output logic        dmem_req_we_o,
    output logic [TB_XLEN-1:0] dmem_req_addr_o,
    output logic [2:0]  dmem_req_size_o,
    output logic [7:0]  dmem_req_wstrb_o,
    output logic [TB_XLEN-1:0] dmem_req_wdata_o,
    output logic        dmem_rsp_ready_o,

    // ------------------------------------------------------ retire event stream
    output logic [TB_RET_N-1:0]              ev_valid_o,
    output logic [TB_RET_N-1:0]              ev_trap_o,
    output logic [TB_RET_N*TB_SEQ_W-1:0]     ev_seq_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_pc_o,
    output logic [TB_RET_N*TB_RET_ID_W-1:0]  ev_id_o,
    output logic [TB_RET_N-1:0]              ev_reg_we_o,
    output logic [TB_RET_N*TB_RD_W-1:0]      ev_rd_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_value_o,
    output logic [TB_RET_N-1:0]              ev_store_o,
    output logic [TB_RET_N*TB_SIZE_W-1:0]    ev_store_size_o,

    // ---------------------------------------------------------------- evidence
    output logic [31:0] o_commit_o,
    output logic [31:0] o_unsupported_o,
    output logic [31:0] o_illegal_o,
    output logic [31:0] o_redirect_o,
    output logic [31:0] o_recovering_o,
    output logic [31:0] o_stop_o,
    output logic        o_stopped_o,
    output logic [31:0] o_cycle_o,
    output logic [31:0] o_c0_count_o,
    output logic [31:0] o_c1_count_o,
    output logic        o_c0_grant_valid_o,
    output logic        o_c1_grant_valid_o,
    output logic [TB_UOP_ID_W-1:0] o_c0_grant_uop_o,
    output logic [TB_UOP_ID_W-1:0] o_c1_grant_uop_o,
    output logic [31:0] o_c0_alu_o,
    output logic [31:0] o_c1_alu_o,
    output logic [31:0] o_c0_br_o,
    output logic [31:0] o_c1_br_o,
    output logic [31:0] o_muldiv_o,
    output logic        o_wb_pub_valid_o,
    output logic [TB_IDX_W-1:0]  o_wb_pub_index_o,
    output logic [TB_RGEN_W-1:0] o_wb_pub_gen_o,
    output logic [TB_XLEN-1:0]   o_wb_pub_value_o,
    output logic [31:0] o_wb_wr_o,
    output logic [31:0] o_wb_wake_o,
    output logic [31:0] o_wb_stale_o,
    output logic [31:0] o_wb_dup_o,
    output logic [31:0] o_wb_collision_o,
    output logic [31:0] o_wb_drop_o,
    output logic [TB_TAG_W:0] o_free_count_o,
    output logic [31:0] o_squash_nc_o,
    output logic [31:0] o_squash_under_o,
    output logic [31:0] o_journal_ovf_o,
    output logic        o_rename_boundary_o,
    output logic [31:0] o_rob_occupied_o,
    output logic [31:0] o_rob_free_o,
    output logic [31:0] o_desc_live_o,
    output logic [31:0] o_redir_act_o,
    output logic [31:0] o_redir_wait_o,
    output logic [31:0] o_redir_dead_o,
    output logic        o_redirect_valid_o,
    output logic [TB_XLEN-1:0] o_redirect_pc_o,
    output logic [TB_XLEN-1:0] o_fetch_pc_o,
    output logic [31:0] o_squash_acc_o,
    output logic [31:0] o_ckpt_o,
    output logic [63:0] o_dbg_redir_o,
    output logic [127:0] o_dbg_fetch_o,

    // ------------------------------- standalone redirect arbiter (directed test)
    input  logic        arb_req_valid0_i,
    input  logic        arb_req_valid1_i,
    input  logic [TB_XLEN-1:0] arb_req_pc0_i,
    input  logic [TB_XLEN-1:0] arb_req_pc1_i,
    input  logic [TB_IDX_W-1:0]  arb_req_idx0_i,
    input  logic [TB_IDX_W-1:0]  arb_req_idx1_i,
    input  logic [TB_RGEN_W-1:0] arb_req_gen0_i,
    input  logic [TB_RGEN_W-1:0] arb_req_gen1_i,
    input  logic        arb_req_taken0_i,
    input  logic        arb_req_taken1_i,
    input  logic        arb_head_valid_i,
    input  logic [TB_IDX_W-1:0]  arb_head_index_i,
    input  logic [TB_RGEN_W-1:0] arb_head_gen_i,
    input  logic [TB_OCC_W-1:0]  arb_head_occupied_i,
    input  logic        arb_head_retire_i,
    output logic        arb_ack0_o,
    output logic        arb_ack1_o,
    output logic        arb_redirect_valid_o,
    output logic [TB_XLEN-1:0] arb_redirect_pc_o,
    output logic        arb_act_valid_o,
    output logic        arb_act_taken_o,
    output logic [31:0] arb_act_ctr_o,
    output logic [31:0] arb_wait_ctr_o,
    output logic [31:0] arb_dead_ctr_o,

    // ------------------------------------------------------- memory path (LSU)
    // Everything the integrated memory path did, so CASE=core.mem_program can
    // state it rather than infer it from the final image.
    output logic [31:0] o_mem_lq_alloc_o,
    output logic [31:0] o_mem_lq_issue_o,
    output logic [31:0] o_mem_lq_done_o,
    output logic [31:0] o_mem_lq_replay_o,
    output logic [31:0] o_mem_lq_blocked_o,
    output logic [31:0] o_mem_lq_fwd_bytes_o,
    output logic [31:0] o_mem_lq_mem_bytes_o,
    output logic [31:0] o_mem_lq_fault_o,
    output logic [31:0] o_mem_lq_query_mismatch_o,
    output logic [31:0] o_mem_lq_occupied_o,
    output logic [31:0] o_mem_sq_alloc_o,
    output logic [31:0] o_mem_sq_commit_o,
    output logic [31:0] o_mem_sq_commit2_o,
    output logic [31:0] o_mem_sq_commit_stale_o,
    output logic [31:0] o_mem_sq_drain_o,
    output logic [31:0] o_mem_sq_squash_o,
    output logic [31:0] o_mem_sq_spared_o,
    output logic [31:0] o_mem_sq_occupied_o,
    output logic [31:0] o_mem_sq_auth_o,
    output logic [31:0] o_mem_sq_fault_o,
    output logic [31:0] o_mem_lsu_txn_o,
    output logic [31:0] o_mem_lsu_misaligned_o,
    output logic [31:0] o_mem_lsu_access_fault_o,
    output logic [31:0] o_mem_ins_stall_o,
    output logic        o_mem_squash_valid_o,

    // ------------------------------------------------------------ debug probes
    output logic        o_dbg_deliver_valid_o,
    output logic [TB_XLEN-1:0] o_dbg_deliver_pc_o,
    output logic [31:0] o_dbg_deliver_bits_o,
    output logic        o_dbg_head_valid_o,
    output logic        o_dbg_head_complete_o,
    output logic [TB_IDX_W-1:0] o_dbg_head_index_o,
    output logic [TB_XLEN-1:0]  o_dbg_head_pc_o,
    output logic [4:0]  o_dbg_desc_rd0_o,
    output logic [4:0]  o_dbg_desc_rd1_o,
    output logic [31:0] o_dbg_alloc_ctr_o,
    output logic [31:0] o_dbg_ins_ctr_o,

    // ------------------------------------------- readiness state (diagnosis)
    // What rename believes, so a case that stops making progress names the
    // stalled mapping instead of only that nothing retired.
    output logic [TB_ARCH_N*TB_MAP_W-1:0] o_dbg_spec_map_o,
    output logic [TB_PRF_N-1:0] o_dbg_gen_valid_o,
    output logic [TB_PRF_N-1:0] o_dbg_wb_done_o,

    // ------------------------------------------------------------- geometry
    // Taken from the generated packages and from the elaborated core, never
    // re-derived by a formula: the driver must decode identities and build PC
    // sequences with the DUT's own widths and reset vector (the convention
    // sim/tb/mosaic_rob_tb.sv documents).
    output logic [31:0] o_geom_xlen_o,
    output logic [31:0] o_geom_clusters_o,
    output logic [31:0] o_geom_retire_width_o,
    output logic [31:0] o_geom_rob_entries_o,
    output logic [31:0] o_geom_rob_index_w_o,
    output logic [31:0] o_geom_rob_gen_w_o,
    output logic [31:0] o_geom_uop_index_w_o,
    output logic [31:0] o_geom_uop_id_w_o,
    output logic [31:0] o_geom_prf_entries_o,
    output logic [31:0] o_geom_prf_tag_w_o,
    output logic [31:0] o_geom_int_gen_w_o,
    output logic [31:0] o_geom_iq_entries_o,
    output logic [31:0] o_geom_occ_w_o,
    output logic [31:0] o_geom_req_id_w_o,
    output logic [31:0] o_geom_epoch_w_o,
    output logic [31:0] o_geom_fetch_outstanding_o,
    output logic [31:0] o_geom_seq_w_o,
    output logic [31:0] o_geom_ret_id_w_o,
    output logic [TB_XLEN-1:0] o_geom_reset_vector_o
);

  // ------------------------------------------------------------------ core
  mosaic_uop_pkg::mem_req_t imem_req;
  mosaic_uop_pkg::mem_rsp_t imem_rsp;
  mosaic_uop_pkg::mem_req_t dmem_req;
  mosaic_uop_pkg::mem_rsp_t dmem_rsp;

  assign imem_rsp.rdata = {{(TB_XLEN-32){1'b0}}, imem_rsp_rdata_i};
  assign imem_rsp.fault = imem_rsp_fault_i;
  assign dmem_rsp.rdata = dmem_rsp_rdata_i;
  assign dmem_rsp.fault = dmem_rsp_fault_i;

  assign imem_req_valid_o = imem_req_valid_int;
  assign imem_req_we_o    = imem_req.we;
  assign imem_req_addr_o  = imem_req.addr;
  assign imem_req_size_o  = imem_req.size;
  assign imem_req_wstrb_o = imem_req.wstrb;
  assign imem_req_wdata_o = imem_req.wdata;
  assign imem_req_id_o    = imem_req_id_int;
  assign imem_req_epoch_o = imem_req_epoch_int;

  assign dmem_req_valid_o = dmem_req_valid_int;
  assign dmem_req_we_o    = dmem_req.we;
  assign dmem_req_addr_o  = dmem_req.addr;
  assign dmem_req_size_o  = dmem_req.size;
  assign dmem_req_wstrb_o = dmem_req.wstrb;
  assign dmem_req_wdata_o = dmem_req.wdata;

  logic        imem_req_valid_int;
  logic [TB_REQ_ID_W-1:0] imem_req_id_int;
  logic [TB_EPOCH_W-1:0]  imem_req_epoch_int;
  logic        dmem_req_valid_int;

  mosaic_core u_core (
      .clk             (clk),
      .rst             (rst),
      .imem_req_valid  (imem_req_valid_int),
      .imem_req        (imem_req),
      .imem_req_ready  (imem_req_ready_i),
      .imem_req_id     (imem_req_id_int),
      .imem_req_epoch  (imem_req_epoch_int),
      .imem_rsp_valid  (imem_rsp_valid_i),
      .imem_rsp_ready  (imem_rsp_ready_o),
      .imem_rsp        (imem_rsp),
      .imem_rsp_id     (imem_rsp_id_i),
      .imem_rsp_epoch  (imem_rsp_epoch_i),
      .imem_rsp_len    (imem_rsp_len_i),
      .dmem_req_valid  (dmem_req_valid_int),
      .dmem_req        (dmem_req),
      .dmem_req_ready  (dmem_req_ready_i),
      .dmem_rsp_valid  (dmem_rsp_valid_i),
      .dmem_rsp_ready  (dmem_rsp_ready_o),
      .dmem_rsp        (dmem_rsp),
      .ev_valid        (ev_valid_o),
      .ev_trap         (ev_trap_o),
      .ev_seq          (ev_seq_o),
      .ev_pc           (ev_pc_o),
      .ev_id           (ev_id_o),
      .ev_reg_we       (ev_reg_we_o),
      .ev_rd           (ev_rd_o),
      .ev_value        (ev_value_o),
      .ev_store        (ev_store_o),
      .ev_store_addr   (),
      .ev_store_data   (),
      .ev_store_size   (ev_store_size_o),
      .o_commit_ctr    (o_commit_o),
      .o_unsupported_ctr(o_unsupported_o),
      .o_illegal_ctr   (o_illegal_o),
      .o_redirect_ctr  (o_redirect_o),
      .o_recovering_ctr(o_recovering_o),
      .o_stop_ctr      (o_stop_o),
      .o_stopped       (o_stopped_o),
      .o_cycle_ctr     (o_cycle_o),
      .o_c0_count      (o_c0_count_o),
      .o_c1_count      (o_c1_count_o),
      .o_c0_grant_valid(o_c0_grant_valid_o),
      .o_c1_grant_valid(o_c1_grant_valid_o),
      .o_c0_grant_uop  (o_c0_grant_uop_o),
      .o_c1_grant_uop  (o_c1_grant_uop_o),
      .o_c0_alu_ctr    (o_c0_alu_o),
      .o_c1_alu_ctr    (o_c1_alu_o),
      .o_c0_branch_ctr (o_c0_br_o),
      .o_c1_branch_ctr (o_c1_br_o),
      .o_muldiv_ctr    (o_muldiv_o),
      .o_wb_pub_valid  (o_wb_pub_valid_o),
      .o_wb_pub_index  (o_wb_pub_index_o),
      .o_wb_pub_gen    (o_wb_pub_gen_o),
      .o_wb_pub_value  (o_wb_pub_value_o),
      .o_wb_wr_ctr     (o_wb_wr_o),
      .o_wb_wake_ctr   (o_wb_wake_o),
      .o_wb_stale_ctr  (o_wb_stale_o),
      .o_wb_dup_ctr    (o_wb_dup_o),
      .o_wb_collision_ctr(o_wb_collision_o),
      .o_wb_drop_ctr   (o_wb_drop_o),
      .o_free_count    (o_free_count_o),
      .o_squash_not_committed_ctr(o_squash_nc_o),
      .o_squash_underflow_ctr(o_squash_under_o),
      .o_journal_overflow_ctr(o_journal_ovf_o),
      .o_rename_boundary(o_rename_boundary_o),
      .o_rob_occupied  (o_rob_occupied_o),
      .o_rob_free      (o_rob_free_o),
      .o_desc_live     (o_desc_live_o),
      .o_redir_act_ctr (o_redir_act_o),
      .o_redir_wait_ctr(o_redir_wait_o),
      .o_redir_dead_ctr(o_redir_dead_o),
      .o_redirect_valid(o_redirect_valid_o),
      .o_redirect_pc   (o_redirect_pc_o),
      .o_fetch_pc      (o_fetch_pc_o),
      .o_squash_acc_ctr(o_squash_acc_o),
      .o_ckpt_ctr      (o_ckpt_o),
      .o_mem_lq_alloc  (o_mem_lq_alloc_o),
      .o_mem_lq_issue  (o_mem_lq_issue_o),
      .o_mem_lq_done   (o_mem_lq_done_o),
      .o_mem_lq_replay (o_mem_lq_replay_o),
      .o_mem_lq_blocked(o_mem_lq_blocked_o),
      .o_mem_lq_fwd_bytes(o_mem_lq_fwd_bytes_o),
      .o_mem_lq_mem_bytes(o_mem_lq_mem_bytes_o),
      .o_mem_lq_fault  (o_mem_lq_fault_o),
      .o_mem_lq_query_mismatch(o_mem_lq_query_mismatch_o),
      .o_mem_lq_occupied(o_mem_lq_occupied_o),
      .o_mem_sq_alloc  (o_mem_sq_alloc_o),
      .o_mem_sq_commit (o_mem_sq_commit_o),
      .o_mem_sq_commit2(o_mem_sq_commit2_o),
      .o_mem_sq_commit_stale(o_mem_sq_commit_stale_o),
      .o_mem_sq_drain  (o_mem_sq_drain_o),
      .o_mem_sq_squash (o_mem_sq_squash_o),
      .o_mem_sq_spared (o_mem_sq_spared_o),
      .o_mem_sq_occupied(o_mem_sq_occupied_o),
      .o_mem_sq_auth   (o_mem_sq_auth_o),
      .o_mem_sq_fault  (o_mem_sq_fault_o),
      .o_mem_lsu_txn   (o_mem_lsu_txn_o),
      .o_mem_lsu_misaligned(o_mem_lsu_misaligned_o),
      .o_mem_lsu_access_fault(o_mem_lsu_access_fault_o),
      .o_mem_ins_stall (o_mem_ins_stall_o),
      .o_mem_squash_valid(o_mem_squash_valid_o),
      .o_dbg_redir_bundle(o_dbg_redir_o),
      .o_dbg_fetch_state(o_dbg_fetch_o),
      .o_dbg_deliver_valid(o_dbg_deliver_valid_o),
      .o_dbg_deliver_pc (o_dbg_deliver_pc_o),
      .o_dbg_deliver_bits(o_dbg_deliver_bits_o),
      .o_dbg_head_valid (o_dbg_head_valid_o),
      .o_dbg_head_complete(o_dbg_head_complete_o),
      .o_dbg_head_index (o_dbg_head_index_o),
      .o_dbg_head_pc    (o_dbg_head_pc_o),
      .o_dbg_desc_rd0   (o_dbg_desc_rd0_o),
      .o_dbg_desc_rd1   (o_dbg_desc_rd1_o),
      .o_dbg_alloc_ctr  (o_dbg_alloc_ctr_o),
      .o_dbg_ins_ctr    (o_dbg_ins_ctr_o),
      .o_dbg_spec_map   (o_dbg_spec_map_o),
      .o_dbg_gen_valid  (o_dbg_gen_valid_o),
      .o_dbg_wb_done    (o_dbg_wb_done_o)
  );

  // ------------------------------------------- standalone redirect arbiter DUT
  mosaic_redirect_arb u_arb (
      .clk            (clk),
      .rst            (rst),
      .req_valid      ({arb_req_valid1_i, arb_req_valid0_i}),
      .req_pc         ({arb_req_pc1_i, arb_req_pc0_i}),
      .req_rob_index  ({arb_req_idx1_i, arb_req_idx0_i}),
      .req_rob_gen    ({arb_req_gen1_i, arb_req_gen0_i}),
      .req_taken      ({arb_req_taken1_i, arb_req_taken0_i}),
      .req_ack        ({arb_ack1_o, arb_ack0_o}),
      .head_valid     (arb_head_valid_i),
      .head_index     (arb_head_index_i),
      .head_gen       (arb_head_gen_i),
      .head_occupied  (arb_head_occupied_i),
      .head_retire    (arb_head_retire_i),
      // The standalone instance is the single-lane directed test: it is driven
      // with one head view and ties the second low, so the properties it checks
      // are the ones it always checked. The two-lane rule is exercised through
      // the core, where a real two-wide retirement puts the branch in lane 1
      // (CASE=core.corpus_branch).
      .head1_valid    (1'b0),
      .head1_index    ({TB_IDX_W{1'b0}}),
      .head1_gen      ({TB_RGEN_W{1'b0}}),
      .head1_retire   (1'b0),
      .redirect_valid (arb_redirect_valid_o),
      .redirect_pc    (arb_redirect_pc_o),
      .o_act_valid    (arb_act_valid_o),
      .o_act_taken    (arb_act_taken_o),
      .o_req_ctr      (),
      .o_act_ctr      (arb_act_ctr_o),
      .o_drop_ctr     (),
      .o_dead_ctr     (arb_dead_ctr_o),
      .o_wait_ctr     (arb_wait_ctr_o),
      .o_nothing_ctr  ()
  );

  // Geometry, straight from the generated packages (and, for the widths the
  // core derives, from the elaborated core's own localparams).
  assign o_geom_xlen_o         = 32'(TB_XLEN);
  assign o_geom_clusters_o     = 32'(mosaic_cfg_pkg::MOSAIC_CLUSTERS);
  assign o_geom_retire_width_o = 32'(TB_RET_N);
  assign o_geom_rob_entries_o  = 32'(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES);
  assign o_geom_rob_index_w_o  = 32'(mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX);
  assign o_geom_rob_gen_w_o    = 32'(mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN);
  assign o_geom_uop_index_w_o  = 32'(mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX);
  assign o_geom_uop_id_w_o     = 32'(TB_UOP_ID_W);
  assign o_geom_prf_entries_o  = 32'(mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES);
  assign o_geom_prf_tag_w_o    = 32'(mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG);
  assign o_geom_int_gen_w_o    = 32'(mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W);
  assign o_geom_iq_entries_o   = 32'(mosaic_cfg_pkg::MOSAIC_IQ_ENTRIES);
  assign o_geom_occ_w_o        = 32'(TB_OCC_W);
  assign o_geom_req_id_w_o     = 32'(TB_REQ_ID_W);
  assign o_geom_epoch_w_o      = 32'(TB_EPOCH_W);
  assign o_geom_fetch_outstanding_o = 32'(mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING);
  assign o_geom_seq_w_o        = 32'(TB_SEQ_W);
  assign o_geom_ret_id_w_o     = 32'(TB_RET_ID_W);
  assign o_geom_reset_vector_o = mosaic_cfg_pkg::MOSAIC_RESET_VECTOR;

endmodule : mosaic_core_tb
/* verilator lint_on PINCONNECTEMPTY */

`default_nettype wire
