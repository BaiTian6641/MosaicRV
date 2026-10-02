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
// The endpoint's transaction identity is the whole uop id (hart included), so it
// is wider than the wrapper's own `TB_UOP_ID_W`, which is the id without the
// hart. Take the width from the package that defines it rather than re-deriving
// it, so the two cannot drift.
localparam int unsigned TB_MEM_ID_W = $bits(mosaic_uop_pkg::uop_id_t);
localparam int unsigned TB_FETCH_N  = mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING;
localparam int unsigned TB_REQ_ID_W = (TB_FETCH_N <= 1) ? 1 : $clog2(TB_FETCH_N);
localparam int unsigned TB_EPOCH_W  = ((mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES <= 1)
                                       ? 2 : $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1);
localparam int unsigned TB_RET_N    = mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH;
localparam int unsigned TB_RET_ID_W = 2 * TB_TAG_W;
localparam int unsigned TB_SEQ_W    = $clog2(2 * mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES + 1);
localparam int unsigned TB_RD_W     = 5;
localparam int unsigned TB_SIZE_W   = 3;
localparam int unsigned TB_CSR_W    = 12;
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

    // I-090: the fabric strategy. Low is the fixed machine every case has always
    // built; high enables dynamic steering and the cluster bypass. It defaults
    // to 0 in every driver that predates the fabric, so their behaviour is
    // unchanged.
    input  logic        fab_dyn_i,

    // I-042: the integrated L1 caches' runtime enable. Low is the cacheless
    // machine every case before `cache.integrated_path` built; high puts the
    // instruction cache between fetch and the instruction port and the data
    // cache between the endpoint and the data port, with cacheability taken from
    // the platform map. The standalone cache-path DUT below also uses it.
    input  logic        cache_en_i,

    // ----------------------------------- standalone L1 cache path (directed)
    // A second instance of the *same* wrapper the core uses, exposed directly so
    // the directed control phases can drive refills, evictions, faults and a
    // device address without threading them through a program. Its memory side
    // is served by the driver, so it can fault a refill or drop a writeback on
    // purpose.
    input  logic        cb_cpu_req_valid_i,
    input  logic        cb_cpu_req_we_i,
    input  logic [TB_XLEN-1:0] cb_cpu_req_addr_i,
    input  logic [2:0]  cb_cpu_req_size_i,
    input  logic [7:0]  cb_cpu_req_wstrb_i,
    input  logic [TB_XLEN-1:0] cb_cpu_req_wdata_i,
    input  logic        cb_cpu_req_amo_i,
    output logic        cb_cpu_req_ready_o,
    output logic        cb_cpu_rsp_valid_o,
    output logic [TB_XLEN-1:0] cb_cpu_rsp_rdata_o,
    output logic        cb_cpu_rsp_fault_o,
    input  logic        cb_flush_i,
    output logic        cb_flush_done_o,
    output logic        cb_mem_req_valid_o,
    input  logic        cb_mem_req_ready_i,
    output logic        cb_mem_req_we_o,
    output logic [TB_XLEN-1:0] cb_mem_req_addr_o,
    output logic [2:0]  cb_mem_req_size_o,
    output logic [7:0]  cb_mem_req_wstrb_o,
    output logic [TB_XLEN-1:0] cb_mem_req_wdata_o,
    input  logic        cb_mem_rsp_valid_i,
    output logic        cb_mem_rsp_ready_o,
    input  logic [TB_XLEN-1:0] cb_mem_rsp_rdata_i,
    input  logic        cb_mem_rsp_fault_i,
    output logic        cb_hit_o,
    output logic        cb_miss_o,
    output logic        cb_refill_o,
    output logic        cb_writeback_o,
    output logic        cb_fault_o,
    output logic [31:0] cb_cpu_txn_o,
    output logic [31:0] cb_mem_beat_o,
    output logic [31:0] cb_bypass_txn_o,
    input  logic [2:0]  cb_dbg_index_i,
    output logic        cb_dbg_valid_o,
    output logic        cb_dbg_dirty_o,

    // ------------------------------------------------- instruction memory port
    input  logic        imem_req_ready_i,
    input  logic        imem_rsp_valid_i,
    // I-042: the instruction memory response is a full doubleword, because the
    // integrated instruction cache refills whole lines (doubleword beats). The
    // front end only ever reads [31:0], so a driver that drives a 32-bit value
    // sees exactly the behaviour it saw before the cache existed.
    input  logic [TB_XLEN-1:0] imem_rsp_rdata_i,
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
    // A extension (I-039): the atomic attribute of the data-port transaction, so
    // CASE=amo.linearization can require every AMO to appear as exactly one
    // atomic memory transaction carrying its operation and its aq/rl bits.
    output logic        dmem_req_amo_o,
    output logic [3:0]  dmem_req_amo_op_o,
    output logic        dmem_req_aq_o,
    output logic        dmem_req_rl_o,
    output logic        dmem_rsp_ready_o,

    // ------------------------------------------------------ retire event stream
    output logic [TB_RET_N-1:0]              ev_valid_o,
    output logic [TB_RET_N-1:0]              ev_trap_o,
    output logic [TB_RET_N*TB_SEQ_W-1:0]     ev_seq_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_pc_o,
    // I-041: the instruction's own length and bits, per lane.
    output logic [TB_RET_N*TB_SIZE_W-1:0]    ev_len_o,
    output logic [TB_RET_N*32-1:0]           ev_insn_o,
    output logic [TB_RET_N*TB_RET_ID_W-1:0]  ev_id_o,
    output logic [TB_RET_N-1:0]              ev_reg_we_o,
    output logic [TB_RET_N*TB_RD_W-1:0]      ev_rd_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_value_o,
    output logic [TB_RET_N-1:0]              ev_store_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_store_addr_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_store_data_o,
    output logic [TB_RET_N*TB_SIZE_W-1:0]    ev_store_size_o,
    output logic [TB_RET_N-1:0]              ev_csr_we_o,
    output logic [TB_RET_N*TB_CSR_W-1:0]     ev_csr_addr_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_csr_value_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_trap_cause_o,
    output logic [TB_RET_N*TB_XLEN-1:0]      ev_trap_tval_o,

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
    output logic        o_mem_lsu_busy_o,
    output logic [31:0] o_mem_ins_stall_o,
    output logic        o_mem_squash_valid_o,
    // ---------------------------------------------- the device path (I-038)
    // What the serializer classified, and the device attribute and identity of
    // the access the data port is carrying. CASE=mmio.exactly_once reads all of
    // them: the counts are its cross-check against the memory system's own
    // address-based classification, and the attribute/identity are read in the
    // cycle a transaction is accepted, so every access can be required to carry
    // the attribute its region demands and every identity can be required to
    // appear exactly once.
    output logic [31:0] o_mem_dev_txn_o,
    output logic [31:0] o_mem_ram_txn_o,
    output logic [31:0] o_mem_dev_wait_o,
    output logic [31:0] o_mem_dev_hold_o,
    output logic        o_mem_dmem_dev_o,
    output logic [TB_MEM_ID_W-1:0] o_mem_dmem_id_o,
    output logic [31:0] o_dbg_mmio_o,

    // ------------------------------------------------- LR/SC reservation (I-040)
    // The coherence notification a second agent's write arrives on, and the
    // reservation state the case requires an LR to establish and each
    // invalidation source to destroy.
    input  logic        ext_write_valid_i,
    input  logic [TB_XLEN-1:0] ext_write_addr_i,
    input  logic [3:0]  ext_write_bytes_i,
    output logic        o_mem_res_valid_o,
    output logic [TB_XLEN-1:0] o_mem_res_granule_o,
    output logic [31:0] o_mem_lr_ctr_o,
    output logic [31:0] o_mem_sc_ok_ctr_o,
    output logic [31:0] o_mem_sc_fail_ctr_o,
    output logic [31:0] o_mem_res_ext_inval_ctr_o,
    output logic [1:0]  o_mem_dmem_kind_o,

    // ------------------------------------------------- CSR / trap / interrupt
    input  logic        irq_soft_i,
    input  logic        irq_timer_i,
    input  logic        irq_ext_i,
    input  logic [TB_XLEN-1:0] mtime_i,
    output logic [TB_XLEN-1:0] o_csr_mstatus_o,
    output logic [TB_XLEN-1:0] o_csr_mtvec_o,
    output logic [TB_XLEN-1:0] o_csr_mepc_o,
    output logic [TB_XLEN-1:0] o_csr_mcause_o,
    output logic [TB_XLEN-1:0] o_csr_mtval_o,
    output logic [TB_XLEN-1:0] o_csr_mscratch_o,
    output logic [TB_XLEN-1:0] o_csr_mie_o,
    output logic [TB_XLEN-1:0] o_csr_mip_o,
    // F/D (I-050): the FP control state and FP evidence.
    output logic [TB_XLEN-1:0] o_csr_fcsr_o,
    output logic [4:0]         o_csr_fflags_o,
    output logic [2:0]         o_csr_frm_o,
    output logic [31:0]        o_fp_issue_ctr_o,
    output logic [31:0]        o_fp_commit_ctr_o,
    output logic [31:0]        o_fp_flags_ctr_o,
    output logic [31:0]        o_fp_merge_ctr_o,
    // ---------------------------------------------- the vector engine (I-059)
    output logic [63:0]        o_vec_vtype_o,
    output logic [63:0]        o_vec_vl_o,
    output logic [63:0]        o_vec_vstart_o,
    output logic [63:0]        o_vec_vcsr_o,
    output logic [63:0]        o_vec_vlenb_o,
    output logic [63:0]        o_vec_vlmax_o,
    output logic               o_vec_vill_o,
    output logic [31:0]        o_vec_macro_ctr_o,
    output logic [31:0]        o_vec_elem_ctr_o,
    output logic [31:0]        o_vec_trap_ctr_o,
    output logic [31:0]        o_vec_retire_ctr_o,
    output logic [31:0]        o_vec_fault_ctr_o,
    output logic [31:0]        o_vec_lsu_req_ctr_o,
    output logic [31:0]        o_vec_chain_accept_ctr_o,
    output logic [31:0]        o_vec_chain_refuse_ctr_o,
    output logic [31:0]        o_vec_desc_alloc_ctr_o,
    output logic [31:0]        o_vec_desc_release_ctr_o,
    output logic [31:0]        o_vec_alu_elems_o,
    output logic [31:0]        o_vec_alu_src_rd_ctr_o,
    output logic [63:0]        o_vec_alu_acc_o,
    output logic [31:0]        o_vec_vrf_rd_ctr_o,
    output logic [31:0]        o_vec_vrf_wr_ctr_o,
    output logic [31:0]        o_vec_vrf_bad_ctr_o,
    output logic [31:0]        o_vec_vrf_rows_o,
    output logic [31:0]        o_vec_vrf_banks_o,
    output logic [63:0]        o_vec_dbg0_o,
    output logic [63:0]        o_vec_dbg1_o,
    output logic [63:0]        o_vec_dbg2_o,
    output logic [31:0] o_csr_wr_o,
    output logic [31:0] o_csr_illegal_wr_o,
    output logic [31:0] o_csr_trap_o,
    output logic [31:0] o_csr_mret_o,
    output logic        o_trap_valid_o,
    output logic        o_trap_is_irq_o,
    output logic [TB_XLEN-1:0] o_trap_cause_o,
    output logic [TB_XLEN-1:0] o_trap_tval_o,
    output logic [TB_XLEN-1:0] o_trap_epc_o,
    output logic [TB_XLEN-1:0] o_trap_target_o,
    output logic        o_mret_valid_o,
    output logic [TB_XLEN-1:0] o_mret_target_o,
    // ------------------------------------------------- privilege evidence (I-044)
    output logic [1:0]  o_priv_o,
    output logic [TB_XLEN-1:0] o_medeleg_o,
    output logic [TB_XLEN-1:0] o_mideleg_o,
    output logic        o_sret_valid_o,
    output logic [TB_XLEN-1:0] o_sret_target_o,
    output logic [TB_XLEN-1:0] o_sstatus_o,
    output logic [TB_XLEN-1:0] o_stvec_o,
    output logic [TB_XLEN-1:0] o_sepc_o,
    output logic [TB_XLEN-1:0] o_scause_o,
    output logic [TB_XLEN-1:0] o_stval_o,
    output logic [TB_XLEN-1:0] o_sscratch_o,
    output logic [TB_XLEN-1:0] o_satp_o,
    output logic [31:0] o_csr_sret_ctr_o,
    output logic [31:0] o_csr_trap_s_ctr_o,
    output logic [31:0] o_csr_priv_illegal_ctr_o,
    output logic [31:0] o_csr_priv_change_ctr_o,
    // I-046: the core's translation cache, observed at the boundary.
    output logic [31:0] o_core_tlb_hit_ctr_o,
    output logic [31:0] o_core_tlb_miss_ctr_o,
    output logic [31:0] o_core_tlb_install_ctr_o,
    output logic [31:0] o_core_tlb_walk_ctr_o,
    output logic [31:0] o_core_tlb_stale_ctr_o,
    output logic [31:0] o_core_tlb_sfence_ctr_o,
    output logic [31:0] o_core_tlb_satp_flush_ctr_o,
    output logic [15:0] o_core_tlb_gen_o,
    output logic [31:0] o_pmp_query_ctr_o,
    output logic [31:0] o_pmp_deny_ctr_o,
    output logic [31:0] o_pmp_fetch_deny_ctr_o,
    output logic [31:0] o_pmp_locked_ctr_o,
    output logic [31:0] o_pmp_store_deny_ctr_o,
    output logic        o_pmp_data_matched_o,
    output logic        o_pmp_data_locked_o,
    output logic        o_pmp_fetch_matched_o,
    output logic        o_pmp_fetch_locked_o,
    output logic        o_csr_pmp_sel_o,
    output logic [mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_CFG_W-1:0]  o_pmp_cfg_o,
    output logic [mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_ADDR_W-1:0] o_pmp_addr_o,
    output logic        o_irq_valid_o,
    output logic [TB_XLEN-1:0] o_irq_cause_o,
    output logic [7:0]  o_irq_ctr_o,
    output logic        o_wfi_halt_o,
    output logic [7:0]  o_spurious_wake_o,
    output logic [7:0]  o_halt_cycles_o,
    output logic [31:0] o_sys_exec_o,
    output logic [31:0] o_exc_capture_o,
    output logic [31:0] o_exc_gen_mismatch_o,
    output logic [31:0] o_trap_irq_o,
    output logic [31:0] o_sys_redirect_o,

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

    // ---------------------------------------------------------- fabric (I-090)
    output logic [4*32-1:0] o_fab_unit_issues_o,
    output logic [6*32-1:0] o_fab_reason_ctr_o,
    output logic [31:0]     o_fab_grant_ctr_o,
    output logic [31:0]     o_fab_stall_ctr_o,
    output logic [31:0]     o_fab_reject_ctr_o,
    output logic [31:0]     o_fab_units_o,
    output logic [31:0]     o_fab_classes_o,
    output logic [1:0]      o_fab_alloc_bank_o,
    output logic [31:0]     o_fab_bp_captured_o,
    output logic [31:0]     o_fab_bp_hit_o,
    output logic [31:0]     o_fab_bp_unauth_o,
    output logic [31:0]     o_fab_bp_flush_o,

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
    output logic [TB_XLEN-1:0] o_geom_reset_vector_o,
    // I-044: the privilege/PMP geometry a driver must not re-derive. These are
    // the profile's own numbers, taken from the generated package rather than
    // from a formula in this wrapper: a profile with no `pmp` block in its
    // geometry has MOSAIC_PMP_ENTRIES == 0 and a profile whose CSR list has no
    // supervisor file has MOSAIC_CSR_HAS_S == 0, and a case that had to guess
    // either would be testing its own guess.
    output logic [31:0] o_geom_pmp_entries_o,
    output logic [31:0] o_geom_pmp_g_o,
    output logic [31:0] o_geom_pmp_grain_bytes_o,
    output logic [31:0] o_geom_pmp_cfg_count_o,
    output logic [31:0] o_geom_has_s_o,
    output logic [31:0] o_geom_has_u_o,
    output logic [31:0] o_geom_priv_least_o,

    // ------------------------------------------- standalone Sv39 walker (I-045)
    // A second DUT beside the core: the page-table walker on its own, so the
    // directed translation matrix can drive every level, permission, SUM/MXR
    // and fault case with the exact page tables the case builds and compare the
    // physical address and the cause/tval pair against an independent model of
    // the walk. The core's own instance is the *integrated* copy the program
    // checks exercise; this one is the unit under direct control.
    input  logic        ptw_xl_valid_i,
    output logic        ptw_xl_ready_o,
    input  logic [TB_XLEN-1:0] ptw_xl_va_i,
    input  logic [1:0]  ptw_xl_kind_i,
    input  logic [1:0]  ptw_xl_priv_i,
    input  logic [3:0]  ptw_xl_mode_i,
    input  logic [43:0] ptw_xl_ppn_i,
    input  logic        ptw_xl_sum_i,
    input  logic        ptw_xl_mxr_i,
    input  logic        ptw_xl_cancel_i,
    input  logic        ptw_xl_rsp_ready_i,
    output logic        ptw_xl_rsp_valid_o,
    output logic [TB_XLEN-1:0] ptw_xl_pa_o,
    output logic        ptw_xl_fault_o,
    output logic [3:0]  ptw_xl_cause_o,
    output logic [TB_XLEN-1:0] ptw_xl_tval_o,
    output logic [3:0]  ptw_xl_perms_o,
    output logic [2:0]  ptw_xl_attr_o,
    output logic        ptw_xl_bare_o,
    output logic        ptw_mem_req_valid_o,
    input  logic        ptw_mem_req_ready_i,
    output logic        ptw_mem_req_we_o,
    output logic [TB_XLEN-1:0] ptw_mem_req_addr_o,
    output logic [TB_XLEN-1:0] ptw_mem_req_wdata_o,
    output logic [7:0]  ptw_mem_req_wstrb_o,
    input  logic        ptw_mem_rsp_valid_i,
    output logic        ptw_mem_rsp_ready_o,
    input  logic [TB_XLEN-1:0] ptw_mem_rsp_rdata_i,
    input  logic        ptw_mem_rsp_fault_i,
    output logic        o_ptw_busy_o,
    output logic [31:0] o_ptw_walk_ctr_o,
    output logic [31:0] o_ptw_leaf_ctr_o,
    output logic [31:0] o_ptw_fault_ctr_o,
    output logic [31:0] o_ptw_ad_ctr_o,
    output logic [31:0] o_ptw_cancel_ctr_o,
    output logic [31:0] o_ptw_retry_ctr_o,
    output logic [31:0] o_ptw_bare_ctr_o,

    // -------------------------------------- standalone TLB (I-046)
    // A second translation-cache DUT beside the core, so the directed cache
    // matrix -- hit/miss, the SFENCE.VMA forms, ASIDs, satp, a cancelled walk,
    // the A/D rule -- can own the PTE port and observe the absence of a PTE
    // read, which is the only honest proof of a hit.
    input  logic        tlb_xl_valid_i,
    output logic        tlb_xl_ready_o,
    input  logic [TB_XLEN-1:0] tlb_xl_va_i,
    input  logic [1:0]  tlb_xl_kind_i,
    input  logic [1:0]  tlb_xl_priv_i,
    input  logic [3:0]  tlb_xl_mode_i,
    input  logic [43:0] tlb_xl_ppn_i,
    input  logic [15:0] tlb_xl_asid_i,
    input  logic        tlb_xl_sum_i,
    input  logic        tlb_xl_mxr_i,
    input  logic        tlb_xl_cancel_i,
    input  logic        tlb_xl_rsp_ready_i,
    output logic        tlb_xl_rsp_valid_o,
    output logic [TB_XLEN-1:0] tlb_xl_pa_o,
    output logic        tlb_xl_fault_o,
    output logic [3:0]  tlb_xl_cause_o,
    output logic [TB_XLEN-1:0] tlb_xl_tval_o,
    output logic [3:0]  tlb_xl_perms_o,
    output logic [2:0]  tlb_xl_attr_o,
    output logic        tlb_xl_bare_o,
    input  logic        tlb_sfence_valid_i,
    input  logic [TB_XLEN-1:0] tlb_sfence_va_i,
    input  logic        tlb_sfence_has_va_i,
    input  logic [15:0] tlb_sfence_asid_i,
    input  logic        tlb_sfence_has_asid_i,
    input  logic        tlb_satp_write_i,
    output logic        tlb_mem_req_valid_o,
    input  logic        tlb_mem_req_ready_i,
    output logic        tlb_mem_req_we_o,
    output logic [TB_XLEN-1:0] tlb_mem_req_addr_o,
    output logic [TB_XLEN-1:0] tlb_mem_req_wdata_o,
    output logic [7:0]  tlb_mem_req_wstrb_o,
    input  logic        tlb_mem_rsp_valid_i,
    output logic        tlb_mem_rsp_ready_o,
    input  logic [TB_XLEN-1:0] tlb_mem_rsp_rdata_i,
    input  logic        tlb_mem_rsp_fault_i,
    output logic        o_tlb_hit_o,
    output logic [31:0] o_tlb_hit_ctr_o,
    output logic [31:0] o_tlb_miss_ctr_o,
    output logic [31:0] o_tlb_perm_fault_ctr_o,
    output logic [31:0] o_tlb_install_ctr_o,
    output logic [31:0] o_tlb_evict_ctr_o,
    output logic [31:0] o_tlb_stale_ctr_o,
    output logic [31:0] o_tlb_sfence_ctr_o,
    output logic [31:0] o_tlb_satp_flush_ctr_o,
    output logic [31:0] o_tlb_cancel_ctr_o,
    output logic [15:0] o_tlb_gen_o,
    output logic [31:0] o_tlb_walk_ctr_o,
    output logic [31:0] o_tlb_ad_ctr_o
);

  // ------------------------------------------------------------------ core
  mosaic_uop_pkg::mem_req_t imem_req;
  mosaic_uop_pkg::mem_rsp_t imem_rsp;
  mosaic_uop_pkg::mem_req_t dmem_req;
  mosaic_uop_pkg::mem_rsp_t dmem_rsp;

  assign imem_rsp.rdata = imem_rsp_rdata_i;
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
  assign dmem_req_amo_o    = dmem_req.amo;
  assign dmem_req_amo_op_o = dmem_req.amo_op;
  assign dmem_req_aq_o     = dmem_req.aq;
  assign dmem_req_rl_o     = dmem_req.rl;

  logic        imem_req_valid_int;
  logic [TB_REQ_ID_W-1:0] imem_req_id_int;
  logic [TB_EPOCH_W-1:0]  imem_req_epoch_int;
  logic        dmem_req_valid_int;

  mosaic_core u_core (
      .clk             (clk),
      .rst             (rst),
      .fab_dyn_i       (fab_dyn_i),
      .cache_en_i      (cache_en_i),
      .irq_soft_i      (irq_soft_i),
      .irq_timer_i     (irq_timer_i),
      .irq_ext_i       (irq_ext_i),
      .mtime_i         (mtime_i),
      .o_csr_mstatus   (o_csr_mstatus_o),
      .o_csr_mtvec     (o_csr_mtvec_o),
      .o_csr_mepc      (o_csr_mepc_o),
      .o_csr_mcause    (o_csr_mcause_o),
      .o_csr_mtval     (o_csr_mtval_o),
      .o_csr_mscratch  (o_csr_mscratch_o),
      .o_csr_mie       (o_csr_mie_o),
      .o_csr_mip       (o_csr_mip_o),
      .o_csr_fcsr      (o_csr_fcsr_o),
      .o_csr_fflags    (o_csr_fflags_o),
      .o_csr_frm       (o_csr_frm_o),
      .o_fp_issue_ctr  (o_fp_issue_ctr_o),
      .o_fp_commit_ctr (o_fp_commit_ctr_o),
      .o_fp_flags_ctr  (o_fp_flags_ctr_o),
      .o_fp_merge_ctr  (o_fp_merge_ctr_o),
      .o_vec_vtype     (o_vec_vtype_o),
      .o_vec_vl        (o_vec_vl_o),
      .o_vec_vstart    (o_vec_vstart_o),
      .o_vec_vcsr      (o_vec_vcsr_o),
      .o_vec_vlenb     (o_vec_vlenb_o),
      .o_vec_vlmax     (o_vec_vlmax_o),
      .o_vec_vill      (o_vec_vill_o),
      .o_vec_macro_ctr (o_vec_macro_ctr_o),
      .o_vec_elem_ctr  (o_vec_elem_ctr_o),
      .o_vec_trap_ctr  (o_vec_trap_ctr_o),
      .o_vec_retire_ctr(o_vec_retire_ctr_o),
      .o_vec_fault_ctr (o_vec_fault_ctr_o),
      .o_vec_lsu_req_ctr(o_vec_lsu_req_ctr_o),
      .o_vec_chain_accept_ctr(o_vec_chain_accept_ctr_o),
      .o_vec_chain_refuse_ctr(o_vec_chain_refuse_ctr_o),
      .o_vec_desc_alloc_ctr(o_vec_desc_alloc_ctr_o),
      .o_vec_desc_release_ctr(o_vec_desc_release_ctr_o),
      .o_vec_alu_elems (o_vec_alu_elems_o),
      .o_vec_alu_src_rd_ctr(o_vec_alu_src_rd_ctr_o),
      .o_vec_alu_acc   (o_vec_alu_acc_o),
      .o_vec_vrf_rd_ctr(o_vec_vrf_rd_ctr_o),
      .o_vec_vrf_wr_ctr(o_vec_vrf_wr_ctr_o),
      .o_vec_vrf_bad_ctr(o_vec_vrf_bad_ctr_o),
      .o_vec_vrf_rows  (o_vec_vrf_rows_o),
      .o_vec_vrf_banks (o_vec_vrf_banks_o),
      .o_vec_dbg0      (o_vec_dbg0_o),
      .o_vec_dbg1      (o_vec_dbg1_o),
      .o_vec_dbg2      (o_vec_dbg2_o),
      .o_csr_wr_ctr    (o_csr_wr_o),
      .o_csr_illegal_wr_ctr(o_csr_illegal_wr_o),
      .o_csr_trap_ctr  (o_csr_trap_o),
      .o_csr_mret_ctr  (o_csr_mret_o),
      .o_trap_valid    (o_trap_valid_o),
      .o_trap_is_irq   (o_trap_is_irq_o),
      .o_trap_cause    (o_trap_cause_o),
      .o_trap_tval     (o_trap_tval_o),
      .o_trap_epc      (o_trap_epc_o),
      .o_trap_target   (o_trap_target_o),
      .o_mret_valid    (o_mret_valid_o),
      .o_mret_target   (o_mret_target_o),
      .o_priv          (o_priv_o),
      .o_medeleg       (o_medeleg_o),
      .o_mideleg       (o_mideleg_o),
      .o_sret_valid    (o_sret_valid_o),
      .o_sret_target   (o_sret_target_o),
      .o_sstatus       (o_sstatus_o),
      .o_stvec         (o_stvec_o),
      .o_sepc          (o_sepc_o),
      .o_scause        (o_scause_o),
      .o_stval         (o_stval_o),
      .o_sscratch      (o_sscratch_o),
      .o_satp          (o_satp_o),
      .o_csr_sret_ctr  (o_csr_sret_ctr_o),
      .o_csr_trap_s_ctr(o_csr_trap_s_ctr_o),
      .o_csr_priv_illegal_ctr(o_csr_priv_illegal_ctr_o),
      .o_csr_priv_change_ctr(o_csr_priv_change_ctr_o),
      .o_pmp_query_ctr (o_pmp_query_ctr_o),
      .o_pmp_deny_ctr  (o_pmp_deny_ctr_o),
      .o_pmp_fetch_deny_ctr(o_pmp_fetch_deny_ctr_o),
      .o_pmp_locked_ctr(o_pmp_locked_ctr_o),
      .o_pmp_store_deny_ctr(o_pmp_store_deny_ctr_o),
      .o_pmp_data_matched(o_pmp_data_matched_o),
      .o_pmp_data_locked(o_pmp_data_locked_o),
      .o_pmp_fetch_matched(o_pmp_fetch_matched_o),
      .o_pmp_fetch_locked(o_pmp_fetch_locked_o),
      .o_csr_pmp_sel   (o_csr_pmp_sel_o),
      .o_pmp_cfg       (o_pmp_cfg_o),
      .o_pmp_addr      (o_pmp_addr_o),
      .o_irq_valid     (o_irq_valid_o),
      .o_irq_cause     (o_irq_cause_o),
      .o_irq_ctr       (o_irq_ctr_o),
      .o_wfi_halt      (o_wfi_halt_o),
      .o_spurious_wake_ctr(o_spurious_wake_o),
      .o_halt_cycles   (o_halt_cycles_o),
      .o_sys_exec_ctr  (o_sys_exec_o),
      .o_exc_capture_ctr(o_exc_capture_o),
      .o_exc_gen_mismatch_ctr(o_exc_gen_mismatch_o),
      .o_trap_irq_ctr  (o_trap_irq_o),
      .o_sys_redirect_ctr(o_sys_redirect_o),
      .o_tlb_hit_ctr   (o_core_tlb_hit_ctr_o),
      .o_tlb_miss_ctr  (o_core_tlb_miss_ctr_o),
      .o_tlb_install_ctr(o_core_tlb_install_ctr_o),
      .o_tlb_walk_ctr  (o_core_tlb_walk_ctr_o),
      .o_tlb_stale_ctr (o_core_tlb_stale_ctr_o),
      .o_tlb_sfence_ctr(o_core_tlb_sfence_ctr_o),
      .o_tlb_satp_flush_ctr(o_core_tlb_satp_flush_ctr_o),
      .o_tlb_gen       (o_core_tlb_gen_o),
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
      .ev_len          (ev_len_o),
      .ev_insn         (ev_insn_o),
      .ev_id           (ev_id_o),
      .ev_reg_we       (ev_reg_we_o),
      .ev_rd           (ev_rd_o),
      .ev_value        (ev_value_o),
      .ev_store        (ev_store_o),
      .ev_store_addr   (ev_store_addr_o),
      .ev_store_data   (ev_store_data_o),
      .ev_store_size   (ev_store_size_o),
      .ev_csr_we       (ev_csr_we_o),
      .ev_csr_addr     (ev_csr_addr_o),
      .ev_csr_value    (ev_csr_value_o),
      .ev_trap_cause   (ev_trap_cause_o),
      .ev_trap_tval    (ev_trap_tval_o),
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
      .o_mem_lsu_busy  (o_mem_lsu_busy_o),
      .o_mem_ins_stall (o_mem_ins_stall_o),
      .o_mem_squash_valid(o_mem_squash_valid_o),
      .o_mem_dev_txn   (o_mem_dev_txn_o),
      .o_mem_ram_txn   (o_mem_ram_txn_o),
      .o_mem_dev_wait  (o_mem_dev_wait_o),
      .o_mem_dev_hold  (o_mem_dev_hold_o),
      .o_mem_dmem_dev  (o_mem_dmem_dev_o),
      .o_mem_dmem_id   (o_mem_dmem_id_o),
      .o_dbg_mmio      (o_dbg_mmio_o),
      .ext_write_valid (ext_write_valid_i),
      .ext_write_addr  (ext_write_addr_i),
      .ext_write_bytes (ext_write_bytes_i),
      .o_mem_res_valid (o_mem_res_valid_o),
      .o_mem_res_granule(o_mem_res_granule_o),
      .o_mem_lr_ctr    (o_mem_lr_ctr_o),
      .o_mem_sc_ok_ctr (o_mem_sc_ok_ctr_o),
      .o_mem_sc_fail_ctr(o_mem_sc_fail_ctr_o),
      .o_mem_res_ext_inval_ctr(o_mem_res_ext_inval_ctr_o),
      .o_mem_dmem_kind (o_mem_dmem_kind_o),
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
      .o_dbg_wb_done    (o_dbg_wb_done_o),
      // -------------------------------------------------------- fabric (I-090)
      .o_fab_unit_issues(o_fab_unit_issues_o),
      .o_fab_reason_ctr (o_fab_reason_ctr_o),
      .o_fab_grant_ctr  (o_fab_grant_ctr_o),
      .o_fab_stall_ctr  (o_fab_stall_ctr_o),
      .o_fab_reject_ctr (o_fab_reject_ctr_o),
      .o_fab_units      (o_fab_units_o),
      .o_fab_classes    (o_fab_classes_o),
      .o_fab_age_w      (),
      .o_fab_occ_w      (),
      .o_fab_unit_w     (),
      .o_fab_alloc_bank (o_fab_alloc_bank_o),
      .o_fab_bp_captured_ctr(o_fab_bp_captured_o),
      .o_fab_bp_hit_ctr (o_fab_bp_hit_o),
      .o_fab_bp_unauth_ctr(o_fab_bp_unauth_o),
      .o_fab_bp_flush_ctr(o_fab_bp_flush_o)
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
      // The standalone instance is the *cluster* directed test: the system/trap
      // request is tied off, because the trap path is exercised through the core
      // where a real exception or interrupt drives it.
      .sys_req_valid  (1'b0),
      .sys_req_pc     ({TB_XLEN{1'b0}}),
      .sys_req_rob_index({TB_IDX_W{1'b0}}),
      .sys_req_rob_gen({TB_RGEN_W{1'b0}}),
      .sys_req_act_now(1'b0),
      .o_sys_act      (),
      .o_sys_redirect (),
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
  assign o_geom_pmp_entries_o    = 32'(mosaic_cfg_pkg::MOSAIC_PMP_ENTRIES);
  assign o_geom_pmp_g_o          = 32'(mosaic_cfg_pkg::MOSAIC_PMP_G);
  assign o_geom_pmp_grain_bytes_o = 32'(mosaic_cfg_pkg::MOSAIC_PMP_GRAIN_BYTES);
  assign o_geom_pmp_cfg_count_o  = 32'(mosaic_cfg_pkg::MOSAIC_PMP_CFG_COUNT);
  assign o_geom_has_s_o          = mosaic_csr_pkg::MOSAIC_CSR_HAS_S ? 32'd1 : 32'd0;
  assign o_geom_has_u_o          = mosaic_csr_pkg::MOSAIC_CSR_HAS_U ? 32'd1 : 32'd0;
  assign o_geom_priv_least_o     = {30'd0, mosaic_csr_pkg::MOSAIC_PRIV_LEAST};

  // --------------------------------------------------------------------------
  // The standalone page-table walker (I-045). Its PTE port is driven directly by
  // the case's physical-memory model, which is also what builds the page tables
  // and can read them back to see the A/D bits the walk wrote.
  // --------------------------------------------------------------------------
  mosaic_ptw u_ptw (
      .clk             (clk),
      .rst             (rst),
      .xl_req_valid_i  (ptw_xl_valid_i),
      .xl_req_ready_o  (ptw_xl_ready_o),
      .xl_va_i         (ptw_xl_va_i),
      .xl_kind_i       (ptw_xl_kind_i),
      .xl_priv_i       (ptw_xl_priv_i),
      .xl_satp_mode_i  (ptw_xl_mode_i),
      .xl_satp_ppn_i   (ptw_xl_ppn_i),
      .xl_sum_i        (ptw_xl_sum_i),
      .xl_mxr_i        (ptw_xl_mxr_i),
      .xl_cancel_i     (ptw_xl_cancel_i),
      .xl_rsp_valid_o  (ptw_xl_rsp_valid_o),
      .xl_rsp_ready_i  (ptw_xl_rsp_ready_i),
      .xl_pa_o         (ptw_xl_pa_o),
      .xl_fault_o      (ptw_xl_fault_o),
      .xl_cause_o      (ptw_xl_cause_o),
      .xl_tval_o       (ptw_xl_tval_o),
      .xl_perms_o      (ptw_xl_perms_o),
      .xl_attr_o       (ptw_xl_attr_o),
      .xl_bare_o       (ptw_xl_bare_o),
      .pte_req_valid_o (ptw_mem_req_valid_o),
      .pte_req_ready_i (ptw_mem_req_ready_i),
      .pte_req_we_o    (ptw_mem_req_we_o),
      .pte_req_addr_o  (ptw_mem_req_addr_o),
      .pte_req_wdata_o (ptw_mem_req_wdata_o),
      .pte_req_wstrb_o (ptw_mem_req_wstrb_o),
      .pte_rsp_valid_i (ptw_mem_rsp_valid_i),
      .pte_rsp_ready_o (ptw_mem_rsp_ready_o),
      .pte_rsp_rdata_i (ptw_mem_rsp_rdata_i),
      .pte_rsp_fault_i (ptw_mem_rsp_fault_i),
      .o_busy          (o_ptw_busy_o),
      .o_walk_ctr      (o_ptw_walk_ctr_o),
      .o_bare_ctr      (o_ptw_bare_ctr_o),
      .o_leaf_ctr      (o_ptw_leaf_ctr_o),
      .o_fault_ctr     (o_ptw_fault_ctr_o),
      .o_ad_upd_ctr    (o_ptw_ad_ctr_o),
      .o_retry_ctr     (o_ptw_retry_ctr_o),
      .o_cancel_ctr    (o_ptw_cancel_ctr_o),
      .o_last_fault_cause (),
      .o_last_fault_tval  ()
  );

  // --------------------------------------------------------------------------
  // The standalone translation cache (I-046). It wraps its own walker; the
  // case's physical-memory model owns the PTE port, so "a hit did not read a
  // PTE" is observed directly rather than inferred.
  // --------------------------------------------------------------------------
  mosaic_tlb u_tlb (
      .clk             (clk),
      .rst             (rst),
      .xl_req_valid_i  (tlb_xl_valid_i),
      .xl_req_ready_o  (tlb_xl_ready_o),
      .xl_va_i         (tlb_xl_va_i),
      .xl_kind_i       (tlb_xl_kind_i),
      .xl_priv_i       (tlb_xl_priv_i),
      .xl_satp_mode_i  (tlb_xl_mode_i),
      .xl_satp_ppn_i   (tlb_xl_ppn_i),
      .xl_satp_asid_i  (tlb_xl_asid_i),
      .xl_sum_i        (tlb_xl_sum_i),
      .xl_mxr_i        (tlb_xl_mxr_i),
      .xl_cancel_i     (tlb_xl_cancel_i),
      .xl_rsp_valid_o  (tlb_xl_rsp_valid_o),
      .xl_rsp_ready_i  (tlb_xl_rsp_ready_i),
      .xl_pa_o         (tlb_xl_pa_o),
      .xl_fault_o      (tlb_xl_fault_o),
      .xl_cause_o      (tlb_xl_cause_o),
      .xl_tval_o       (tlb_xl_tval_o),
      .xl_perms_o      (tlb_xl_perms_o),
      .xl_attr_o       (tlb_xl_attr_o),
      .xl_bare_o       (tlb_xl_bare_o),
      .sfence_valid_i  (tlb_sfence_valid_i),
      .sfence_va_i     (tlb_sfence_va_i),
      .sfence_has_va_i (tlb_sfence_has_va_i),
      .sfence_asid_i   (tlb_sfence_asid_i),
      .sfence_has_asid_i(tlb_sfence_has_asid_i),
      .satp_write_i    (tlb_satp_write_i),
      .pte_req_valid_o (tlb_mem_req_valid_o),
      .pte_req_ready_i (tlb_mem_req_ready_i),
      .pte_req_we_o    (tlb_mem_req_we_o),
      .pte_req_addr_o  (tlb_mem_req_addr_o),
      .pte_req_wdata_o (tlb_mem_req_wdata_o),
      .pte_req_wstrb_o (tlb_mem_req_wstrb_o),
      .pte_rsp_valid_i (tlb_mem_rsp_valid_i),
      .pte_rsp_ready_o (tlb_mem_rsp_ready_o),
      .pte_rsp_rdata_i (tlb_mem_rsp_rdata_i),
      .pte_rsp_fault_i (tlb_mem_rsp_fault_i),
      .o_busy          (),
      .o_hit           (o_tlb_hit_o),
      .o_hit_ctr       (o_tlb_hit_ctr_o),
      .o_miss_ctr      (o_tlb_miss_ctr_o),
      .o_perm_fault_ctr(o_tlb_perm_fault_ctr_o),
      .o_install_ctr   (o_tlb_install_ctr_o),
      .o_evict_ctr     (o_tlb_evict_ctr_o),
      .o_stale_ctr     (o_tlb_stale_ctr_o),
      .o_sfence_ctr    (o_tlb_sfence_ctr_o),
      .o_satp_flush_ctr(o_tlb_satp_flush_ctr_o),
      .o_cancel_ctr    (o_tlb_cancel_ctr_o),
      .o_gen           (o_tlb_gen_o),
      .o_walk_ctr      (o_tlb_walk_ctr_o),
      .o_leaf_ctr      (),
      .o_fault_ctr     (),
      .o_ad_ctr        (o_tlb_ad_ctr_o),
      .o_walk_cancel_ctr()
  );

  // ==========================================================================
  // Standalone L1 cache path (I-042, directed control phases)
  // ==========================================================================
  // The same wrapper the core instantiates, driven directly. The directed
  // phases use it because they must fault a refill, evict a dirty line on
  // demand and present a device address -- conditions a bare-metal program
  // cannot produce deterministically. Its memory side is served by the driver,
  // which is why a refill can be made to fail.
  mosaic_uop_pkg::mem_req_t cb_cpu_req;
  mosaic_uop_pkg::mem_rsp_t cb_cpu_rsp;
  mosaic_uop_pkg::mem_req_t cb_mem_req;
  mosaic_uop_pkg::mem_rsp_t cb_mem_rsp;

  always_comb begin
    cb_cpu_req.we     = cb_cpu_req_we_i;
    cb_cpu_req.addr   = cb_cpu_req_addr_i;
    cb_cpu_req.size   = cb_cpu_req_size_i;
    cb_cpu_req.wstrb  = cb_cpu_req_wstrb_i;
    cb_cpu_req.wdata  = cb_cpu_req_wdata_i;
    cb_cpu_req.amo    = cb_cpu_req_amo_i;
    cb_cpu_req.amo_op = mosaic_pkg::AMO_ADD;
    cb_cpu_req.aq     = 1'b0;
    cb_cpu_req.rl     = 1'b0;
  end
  assign cb_cpu_rsp_rdata_o = cb_cpu_rsp.rdata;
  assign cb_cpu_rsp_fault_o = cb_cpu_rsp.fault;

  assign cb_mem_req_we_o    = cb_mem_req.we;
  assign cb_mem_req_addr_o  = cb_mem_req.addr;
  assign cb_mem_req_size_o  = cb_mem_req.size;
  assign cb_mem_req_wstrb_o = cb_mem_req.wstrb;
  assign cb_mem_req_wdata_o = cb_mem_req.wdata;
  assign cb_mem_rsp.rdata   = cb_mem_rsp_rdata_i;
  assign cb_mem_rsp.fault   = cb_mem_rsp_fault_i;

  mosaic_l1_cache_path #(
      .IS_FETCH      (1'b0),
      .LINE_BYTES    (32),
      .SETS          (8),
      .ADDR_WIDTH    (64),
      .CPU_DATA_WIDTH(64),
      .ID_W          (1),
      .EPOCH_W       (1)
  ) u_tb_cache_path (
      .clk             (clk),
      .rst             (rst),
      .en_i            (cache_en_i),
      .flush_i         (cb_flush_i),
      .flush_done      (cb_flush_done_o),
      .cpu_req_valid_i (cb_cpu_req_valid_i),
      .cpu_req_ready_o (cb_cpu_req_ready_o),
      .cpu_req_i       (cb_cpu_req),
      .cpu_req_id_i    (1'b0),
      .cpu_req_epoch_i (1'b0),
      .cpu_rsp_valid_o (cb_cpu_rsp_valid_o),
      .cpu_rsp_ready_i (1'b1),
      .cpu_rsp_o       (cb_cpu_rsp),
      .cpu_rsp_id_o    (),
      .cpu_rsp_epoch_o (),
      .cpu_rsp_len_o   (),
      .mem_req_valid_o (cb_mem_req_valid_o),
      .mem_req_ready_i (cb_mem_req_ready_i),
      .mem_req_o       (cb_mem_req),
      .mem_req_id_o    (),
      .mem_req_epoch_o (),
      .mem_rsp_valid_i (cb_mem_rsp_valid_i),
      .mem_rsp_ready_o (cb_mem_rsp_ready_o),
      .mem_rsp_i       (cb_mem_rsp),
      .mem_rsp_id_i    (1'b0),
      .mem_rsp_epoch_i (1'b0),
      .mem_rsp_len_i   (3'b0),
      .o_hit           (cb_hit_o),
      .o_miss          (cb_miss_o),
      .o_refill        (cb_refill_o),
      .o_writeback     (cb_writeback_o),
      .o_fault         (cb_fault_o),
      .o_cpu_txn       (cb_cpu_txn_o),
      .o_mem_beat      (cb_mem_beat_o),
      .o_line_txn      (),
      .o_bypass_txn    (cb_bypass_txn_o),
      .dbg_index_i     (cb_dbg_index_i),
      .dbg_valid_o     (cb_dbg_valid_o),
      .dbg_dirty_o     (cb_dbg_dirty_o)
  );

endmodule : mosaic_core_tb
/* verilator lint_on PINCONNECTEMPTY */

`default_nettype wire
