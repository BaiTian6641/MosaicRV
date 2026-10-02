// ============================================================================
// mosaic_multihart -- two architectural domains on one physical machine
// (work package I-064, case CASE=multihart.isolation).
//
// ---------------------------------------------------------------- the design
//
// The cheapest honest construction the card permits: instantiate the core's
// architectural state twice and share only what the plan says may be shared.
// Two `mosaic_core` instances each own their PC, rename tables, ROB, PRF, issue
// queues, execution lanes, load/store queues, CSRs, interrupt state, TLB/PTW and
// predictor -- that is the *static partition*. What is shared is the **memory
// service**: one hart-tagged request/response bus, one arbiter, one address
// space of physical memory behind it (the SoC the driver models). The L1 cache
// path inside each core is replicated and disabled in this configuration
// (`cache_en_i` low), so every access -- including a page-table walk -- reaches
// the shared service; a *shared* cache is I-065's coherence subject, not this
// package's. The report's replication/sharing table is the design.
//
// ------------------------------------------------------------- hart ownership
//
// The card's mechanism: **every request that leaves a hart carries its
// identity, every response is matched to that identity, and a mismatch is a
// detected error rather than a wrong answer.** The identity is the `hart` field
// of `mosaic_id_pkg::macro_id_t` -- the same field the store queue, the TLB, the
// MSHR and the chaining network already match on -- and each core places its own
// `HART_ID` into it (`mosaic_core`'s parameter). This module carries the same
// identity on the memory bus: a request is tagged `{hart, src}`, and a response
// is routed back to the slot that identity names. If a response names a slot
// that did not request it -- or names a slot whose instruction id/epoch does not
// match -- the response is *not delivered*, and `o_owner_mismatch_ctr` counts
// the error. That is the second of the card's two named fail modes ("hart ID
// reuse confuses a response") turned into a detector, and the mutant table
// falsifies it.
//
// ----------------------------------------------------------------- the slots
//
// Four slots, indexed {hart, src} with src 0 = data and src 1 = instruction:
//
//   slot 0  hart 0 data          slot 2  hart 1 data
//   slot 1  hart 0 instruction   slot 3  hart 1 instruction
//
// Each slot holds at most one outstanding request (each core port is itself
// single-outstanding), so a response's ownership is unambiguous and a mismatch
// is decidable. The arbiter rotates priority across the four slots, so neither
// hart can starve the other, and `o_contend_ctr` counts the cycles on which both
// harts had a request outstanding -- the direct statement that the two domains
// are *contending for the one shared service* rather than each having its own.
//
// ------------------------------------------------------------- not claimed
//
//  * No coherence protocol and no cross-hart ordering guarantee. A store by one
//    hart is not made visible to the other by any invalidation or ownership
//    state, and the two harts run on disjoint physical regions in this case.
//    I-065 owns shared-memory serialization and coherence.
//  * No SMT: the execution resources are *statically* partitioned (each hart its
//    own issue queues and lanes). Dynamic sharing/borrowing is I-066.
//  * The two harts share one clock and one reset domain; `hart_en_i` lets a
//    driver hold one in reset, which is how the per-hart reference runs are
//    taken, and that sharing is stated in the report rather than hidden.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
`include "mosaic_pkg.sv"
`include "mosaic_uop_pkg.sv"

localparam int unsigned MH_XLEN     = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned MH_RET_N    = mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH;
localparam int unsigned MH_ROB_N    = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned MH_FETCH_N  = mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING;
localparam int unsigned MH_REQ_ID_W = (MH_FETCH_N <= 1) ? 1 : $clog2(MH_FETCH_N);
localparam int unsigned MH_EPOCH_W  = (MH_ROB_N <= 1) ? 2 : $clog2(MH_ROB_N) + 1;
localparam int unsigned MH_SEQ_W    = $clog2(2 * MH_ROB_N + 1);
localparam int unsigned MH_SIZE_W   = 3;
localparam int unsigned MH_RD_W     = 5;
localparam int unsigned MH_CSR_W    = 12;
localparam int unsigned MH_HART_W   = mosaic_id_pkg::MOSAIC_ID_W_HART;
localparam int unsigned MH_MEM_ID_W = $bits(mosaic_uop_pkg::uop_id_t);
// The hart count this module is built for. The card starts at two; the
// construction is the two-hart one and a wider build is a later package.
localparam int unsigned MH_HARTS    = 2;

module mosaic_multihart #(
    // Each hart's reset PC, so the two domains can run different programs. The
    // defaults are the platform's reset vector, which keeps a single-hart
    // reading of this module booting where it always did.
    parameter logic [63:0] HART0_RESET_PC = mosaic_cfg_pkg::MOSAIC_RESET_VECTOR,
    parameter logic [63:0] HART1_RESET_PC = mosaic_cfg_pkg::MOSAIC_RESET_VECTOR
) (
    input  logic                          clk,
    input  logic                          rst,
    // Per-hart enable. Low holds that hart in reset (its requests are masked off
    // the shared bus), which is how the driver takes each hart's solo reference
    // run. Both high is the concurrent run.
    input  logic [MH_HARTS-1:0]           hart_en_i,

    // ================================================== the shared memory bus
    // One request channel, one response channel, every packet hart-tagged. This
    // is the shared resource contract: the arbiter inside this module is the
    // only thing that decides who gets the port, and the tag is the only thing
    // that decides where the answer goes.
    output logic                          mem_req_valid,
    input  logic                          mem_req_ready,
    output logic [MH_HART_W-1:0]          mem_req_hart,
    output logic                          mem_req_src,       // 0 = data, 1 = instruction
    output mosaic_uop_pkg::mem_req_t      mem_req,
    output logic [MH_REQ_ID_W-1:0]        mem_req_id,
    output logic [MH_EPOCH_W-1:0]         mem_req_epoch,

    input  logic                          mem_rsp_valid,
    output logic                          mem_rsp_ready,
    input  logic [MH_HART_W-1:0]          mem_rsp_hart,
    input  logic                          mem_rsp_src,
    input  mosaic_uop_pkg::mem_rsp_t      mem_rsp,
    input  logic [MH_REQ_ID_W-1:0]        mem_rsp_id,
    input  logic [MH_EPOCH_W-1:0]         mem_rsp_epoch,
    input  logic [2:0]                    mem_rsp_len,

    // ================================================== shared-service evidence
    output logic [31:0]                   o_req_ctr,          // accepted requests
    output logic [31:0]                   o_req_h0_ctr,
    output logic [31:0]                   o_req_h1_ctr,
    output logic [31:0]                   o_req_imem_ctr,
    output logic [31:0]                   o_req_dmem_ctr,
    // Cycles on which both harts had a request outstanding at once: the direct
    // statement that they are contending for the one shared service.
    output logic [31:0]                   o_contend_ctr,
    // A response that named a slot which did not request it. Zero in the
    // shipping machine; non-zero is the detected ownership error.
    output logic [31:0]                   o_owner_mismatch_ctr,
    // The identity geometry the case reads rather than re-deriving: the hart
    // field width and the full uop-identity width, so the driver can extract the
    // hart from a core's own packet identity without guessing a width.
    output logic [31:0]                   o_geom_hart_w,
    output logic [31:0]                   o_geom_mem_id_w,
    output logic [31:0]                   o_geom_ret_n,
    output logic [31:0]                   o_geom_seq_w,

    // ================================================== per-hart evidence (h0)
    output logic [MH_RET_N-1:0]                    h0_ev_valid,
    output logic [MH_RET_N-1:0]                    h0_ev_trap,
    output logic [MH_RET_N*MH_SEQ_W-1:0]           h0_ev_seq,
    output logic [MH_RET_N*MH_XLEN-1:0]            h0_ev_pc,
    output logic [MH_RET_N*MH_SIZE_W-1:0]          h0_ev_len,
    output logic [MH_RET_N*32-1:0]                 h0_ev_insn,
    output logic [MH_RET_N-1:0]                    h0_ev_reg_we,
    output logic [MH_RET_N*MH_RD_W-1:0]            h0_ev_rd,
    output logic [MH_RET_N*MH_XLEN-1:0]            h0_ev_value,
    output logic [MH_RET_N-1:0]                    h0_ev_store,
    output logic [MH_RET_N*MH_XLEN-1:0]            h0_ev_store_addr,
    output logic [MH_RET_N*MH_XLEN-1:0]            h0_ev_store_data,
    output logic [MH_RET_N*MH_SIZE_W-1:0]          h0_ev_store_size,
    output logic [MH_RET_N-1:0]                    h0_ev_csr_we,
    output logic [MH_RET_N*MH_CSR_W-1:0]           h0_ev_csr_addr,
    output logic [MH_RET_N*MH_XLEN-1:0]            h0_ev_csr_value,
    output logic [MH_RET_N*MH_XLEN-1:0]            h0_ev_trap_cause,
    output logic [MH_RET_N*MH_XLEN-1:0]            h0_ev_trap_tval,
    output logic [31:0]                            h0_commit_ctr,
    output logic [31:0]                            h0_redirect_ctr,
    output logic                                   h0_redirect_valid,
    output logic [MH_XLEN-1:0]                     h0_redirect_pc,
    output logic [MH_XLEN-1:0]                     h0_fetch_pc,
    output logic [31:0]                            h0_rob_occupied,
    output logic                                   h0_stopped,
    output logic                                   h0_trap_valid,
    output logic [MH_XLEN-1:0]                     h0_trap_cause,
    output logic [MH_XLEN-1:0]                     h0_trap_tval,
    output logic [MH_XLEN-1:0]                     h0_trap_epc,
    output logic [1:0]                             h0_priv,
    output logic [MH_XLEN-1:0]                     h0_satp,
    output logic [MH_XLEN-1:0]                     h0_mstatus,
    output logic [MH_XLEN-1:0]                     h0_mepc,
    output logic [MH_XLEN-1:0]                     h0_mcause,
    output logic [MH_XLEN-1:0]                     h0_mtval,
    output logic [MH_MEM_ID_W-1:0]                 h0_dmem_id,
    // The cycle hart 0's data request was accepted by the shared service: the
    // cycle the identity below describes a real transaction, so the case can
    // require its hart field to name hart 0 rather than reading it blind.
    output logic                                   h0_dmem_id_valid,

    // ================================================== per-hart evidence (h1)
    output logic [MH_RET_N-1:0]                    h1_ev_valid,
    output logic [MH_RET_N-1:0]                    h1_ev_trap,
    output logic [MH_RET_N*MH_SEQ_W-1:0]           h1_ev_seq,
    output logic [MH_RET_N*MH_XLEN-1:0]            h1_ev_pc,
    output logic [MH_RET_N*MH_SIZE_W-1:0]          h1_ev_len,
    output logic [MH_RET_N*32-1:0]                 h1_ev_insn,
    output logic [MH_RET_N-1:0]                    h1_ev_reg_we,
    output logic [MH_RET_N*MH_RD_W-1:0]            h1_ev_rd,
    output logic [MH_RET_N*MH_XLEN-1:0]            h1_ev_value,
    output logic [MH_RET_N-1:0]                    h1_ev_store,
    output logic [MH_RET_N*MH_XLEN-1:0]            h1_ev_store_addr,
    output logic [MH_RET_N*MH_XLEN-1:0]            h1_ev_store_data,
    output logic [MH_RET_N*MH_SIZE_W-1:0]          h1_ev_store_size,
    output logic [MH_RET_N-1:0]                    h1_ev_csr_we,
    output logic [MH_RET_N*MH_CSR_W-1:0]           h1_ev_csr_addr,
    output logic [MH_RET_N*MH_XLEN-1:0]            h1_ev_csr_value,
    output logic [MH_RET_N*MH_XLEN-1:0]            h1_ev_trap_cause,
    output logic [MH_RET_N*MH_XLEN-1:0]            h1_ev_trap_tval,
    output logic [31:0]                            h1_commit_ctr,
    output logic [31:0]                            h1_redirect_ctr,
    output logic                                   h1_redirect_valid,
    output logic [MH_XLEN-1:0]                     h1_redirect_pc,
    output logic [MH_XLEN-1:0]                     h1_fetch_pc,
    output logic [31:0]                            h1_rob_occupied,
    output logic                                   h1_stopped,
    output logic                                   h1_trap_valid,
    output logic [MH_XLEN-1:0]                     h1_trap_cause,
    output logic [MH_XLEN-1:0]                     h1_trap_tval,
    output logic [MH_XLEN-1:0]                     h1_trap_epc,
    output logic [1:0]                             h1_priv,
    output logic [MH_XLEN-1:0]                     h1_satp,
    output logic [MH_XLEN-1:0]                     h1_mstatus,
    output logic [MH_XLEN-1:0]                     h1_mepc,
    output logic [MH_XLEN-1:0]                     h1_mcause,
    output logic [MH_XLEN-1:0]                     h1_mtval,
    output logic [MH_MEM_ID_W-1:0]                 h1_dmem_id,
    output logic                                   h1_dmem_id_valid
);

  // The two cores have far more evidence outputs than this package consumes
  // (every module-level observation the single-hart cases read). They are
  // deliberately left unconnected here rather than wired to dead nets: the
  // package that owns each observation owns its case, and this module only
  // carries the ones multihart.isolation reads.
  /* verilator lint_off PINCONNECTEMPTY */

  // -------------------------------------------------------------------------
  // Per-hart reset: the shared `rst` OR "this hart is disabled". A disabled
  // hart is held in reset, so it has no architectural state and issues nothing.
  // -------------------------------------------------------------------------
  logic h0_rst, h1_rst;
  assign h0_rst = rst | ~hart_en_i[0];
`ifdef MOSAIC_MH_MUTANT_GLOBAL_REDIRECT
  // NEGATIVE CONTROL: one hart's redirect is applied to the other. The
  // non-redirecting hart is reset whenever hart 0 redirects, which is the
  // wrapper-level form of "a global redirect clears the other hart" -- the
  // card's first named fail mode. CASE=multihart.isolation's "hart 1's stream
  // is unaffected" check must catch it.
  assign h1_rst = rst | ~hart_en_i[1] | h0_redirect_valid_w;
`else
  assign h1_rst = rst | ~hart_en_i[1];
`endif

  // -------------------------------------------------------------------------
  // The two cores: independent architectural state, each with its own HART_ID
  // and reset PC.
  // -------------------------------------------------------------------------
  logic                     h0_imem_req_valid, h0_imem_req_ready;
  mosaic_uop_pkg::mem_req_t h0_imem_req;
  logic [MH_REQ_ID_W-1:0]   h0_imem_req_id;
  logic [MH_EPOCH_W-1:0]    h0_imem_req_epoch;
  logic                     h0_imem_rsp_valid, h0_imem_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t h0_imem_rsp;
  logic [MH_REQ_ID_W-1:0]   h0_imem_rsp_id;
  logic [MH_EPOCH_W-1:0]    h0_imem_rsp_epoch;
  logic [2:0]               h0_imem_rsp_len;

  logic                     h0_dmem_req_valid, h0_dmem_req_ready;
  mosaic_uop_pkg::mem_req_t h0_dmem_req;
  logic                     h0_dmem_rsp_valid, h0_dmem_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t h0_dmem_rsp;

  logic                     h1_imem_req_valid, h1_imem_req_ready;
  mosaic_uop_pkg::mem_req_t h1_imem_req;
  logic [MH_REQ_ID_W-1:0]   h1_imem_req_id;
  logic [MH_EPOCH_W-1:0]    h1_imem_req_epoch;
  logic                     h1_imem_rsp_valid, h1_imem_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t h1_imem_rsp;
  logic [MH_REQ_ID_W-1:0]   h1_imem_rsp_id;
  logic [MH_EPOCH_W-1:0]    h1_imem_rsp_epoch;
  logic [2:0]               h1_imem_rsp_len;

  logic                     h1_dmem_req_valid, h1_dmem_req_ready;
  mosaic_uop_pkg::mem_req_t h1_dmem_req;
  logic                     h1_dmem_rsp_valid, h1_dmem_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t h1_dmem_rsp;

  logic                     h0_redirect_valid_w, h1_redirect_valid_w;
  mosaic_core #(
      .HART_ID   (0),
      .RESET_PC  (HART0_RESET_PC)
  ) u_h0 (
      .clk                  (clk),
      .rst                  (h0_rst),
      .fab_dyn_i            (1'b0),
      .cache_en_i           (1'b0),
      .loc_llb_en_i         (1'b0),
      .loc_pf_en_i          (1'b0),
      .loc_pf_conf_thresh_i (2'b0),
      .loc_snoop_all_i      (1'b0),
      .lane_quota_req_i     (1'b0),
      .lane_quota_val_i     (4'd0),
      .vec_coalesce_dis_i   (1'b0),
      .imem_req_valid       (h0_imem_req_valid),
      .imem_req             (h0_imem_req),
      .imem_req_ready       (h0_imem_req_ready),
      .imem_req_id          (h0_imem_req_id),
      .imem_req_epoch       (h0_imem_req_epoch),
      .imem_rsp_valid       (h0_imem_rsp_valid),
      .imem_rsp_ready       (h0_imem_rsp_ready),
      .imem_rsp             (h0_imem_rsp),
      .imem_rsp_id          (h0_imem_rsp_id),
      .imem_rsp_epoch       (h0_imem_rsp_epoch),
      .imem_rsp_len         (h0_imem_rsp_len),
      .dmem_req_valid       (h0_dmem_req_valid),
      .dmem_req             (h0_dmem_req),
      .dmem_req_ready       (h0_dmem_req_ready),
      .dmem_rsp_valid       (h0_dmem_rsp_valid),
      .dmem_rsp_ready       (h0_dmem_rsp_ready),
      .dmem_rsp             (h0_dmem_rsp),
      .ext_write_valid      (1'b0),
      .ext_write_addr       (64'd0),
      .ext_write_bytes      (4'd0),
      .o_mem_res_valid      (),
      .o_mem_res_granule    (),
      .o_mem_lr_ctr         (),
      .o_mem_sc_ok_ctr      (),
      .o_mem_sc_fail_ctr    (),
      .o_mem_res_ext_inval_ctr(),
      .o_mem_dmem_kind      (),
      .o_loc_llb_hit        (),
      .o_loc_llb_miss       (),
      .o_loc_llb_bypass     (),
      .o_loc_llb_fill       (),
      .o_loc_llb_fill_refused(),
      .o_loc_llb_inv        (),
      .o_loc_mem_line       (),
      .o_loc_pf_issued      (),
      .o_loc_pf_useful      (),
      .o_loc_pf_useless     (),
      .o_loc_pf_late        (),
      .o_loc_pf_cancelled   (),
      .o_loc_pf_admitted    (),
      .o_loc_pf_fill        (),
      .o_loc_pf_fill_refused(),
      .o_loc_pf_mem         (),
      .ev_valid             (h0_ev_valid),
      .ev_trap              (h0_ev_trap),
      .ev_seq               (h0_ev_seq),
      .ev_pc                (h0_ev_pc),
      .ev_len               (h0_ev_len),
      .ev_insn              (h0_ev_insn),
      .ev_id                (),
      .ev_reg_we            (h0_ev_reg_we),
      .ev_rd                (h0_ev_rd),
      .ev_value             (h0_ev_value),
      .ev_store             (h0_ev_store),
      .ev_store_addr        (h0_ev_store_addr),
      .ev_store_data        (h0_ev_store_data),
      .ev_store_size        (h0_ev_store_size),
      .ev_csr_we            (h0_ev_csr_we),
      .ev_csr_addr          (h0_ev_csr_addr),
      .ev_csr_value         (h0_ev_csr_value),
      .ev_trap_cause        (h0_ev_trap_cause),
      .ev_trap_tval         (h0_ev_trap_tval),
      .o_mem_lq_alloc       (),
      .o_mem_lq_issue       (),
      .o_mem_lq_done        (),
      .o_mem_lq_replay      (),
      .o_mem_lq_blocked     (),
      .o_mem_lq_fwd_bytes   (),
      .o_mem_lq_mem_bytes   (),
      .o_mem_lq_fault       (),
      .o_mem_lq_query_mismatch(),
      .o_mem_lq_occupied    (),
      .o_mem_sq_alloc       (),
      .o_mem_sq_commit      (),
      .o_mem_sq_commit2     (),
      .o_mem_sq_commit_stale(),
      .o_mem_sq_drain       (),
      .o_mem_sq_squash      (),
      .o_mem_sq_spared      (),
      .o_mem_sq_occupied    (),
      .o_mem_sq_auth        (),
      .o_mem_sq_fault       (),
      .o_mem_lsu_txn        (),
      .o_mem_lsu_misaligned (),
      .o_mem_lsu_access_fault(),
      .o_mem_lsu_busy       (),
      .o_mem_ins_stall      (),
      .o_mem_squash_valid   (),
      .o_mem_dev_txn        (),
      .o_mem_ram_txn        (),
      .o_mem_dev_wait       (),
      .o_mem_dev_hold       (),
      .o_mem_dmem_dev       (),
      .o_mem_dmem_id        (h0_dmem_id),
      .o_dbg_mmio           (),
      .o_commit_ctr         (h0_commit_ctr),
      .o_unsupported_ctr    (),
      .o_illegal_ctr        (),
      .o_redirect_ctr       (h0_redirect_ctr),
      .o_recovering_ctr     (),
      .o_stop_ctr           (),
      .o_stopped            (h0_stopped),
      .o_cycle_ctr          (),
      .o_c0_count           (),
      .o_c1_count           (),
      .o_c0_grant_valid     (),
      .o_c1_grant_valid     (),
      .o_c0_grant_uop       (),
      .o_c1_grant_uop       (),
      .o_c0_alu_ctr         (),
      .o_c1_alu_ctr         (),
      .o_c0_branch_ctr      (),
      .o_c1_branch_ctr      (),
      .o_muldiv_ctr         (),
      .o_wb_pub_valid       (),
      .o_wb_pub_index       (),
      .o_wb_pub_gen         (),
      .o_wb_pub_value       (),
      .o_wb_wr_ctr          (),
      .o_wb_wake_ctr        (),
      .o_wb_stale_ctr       (),
      .o_wb_dup_ctr         (),
      .o_wb_collision_ctr   (),
      .o_wb_drop_ctr        (),
      .o_free_count         (),
      .o_squash_not_committed_ctr(),
      .o_squash_underflow_ctr(),
      .o_journal_overflow_ctr(),
      .o_rob_occupied       (h0_rob_occupied),
      .o_rob_free           (),
      .o_desc_live          (),
      .o_rename_boundary    (),
      .o_redir_act_ctr      (),
      .o_redir_wait_ctr     (),
      .o_redir_dead_ctr     (),
      .o_redirect_valid     (h0_redirect_valid_w),
      .o_redirect_pc        (h0_redirect_pc),
      .o_fetch_pc           (h0_fetch_pc),
      .o_squash_acc_ctr     (),
      .o_ckpt_ctr           (),
      .o_dbg_redir_bundle   (),
      .o_dbg_fetch_state    (),
      .irq_soft_i           (1'b0),
      .irq_timer_i          (1'b0),
      .irq_ext_i            (1'b0),
      .mtime_i              (64'd0),
      .o_csr_mstatus        (h0_mstatus),
      .o_csr_mtvec          (),
      .o_csr_mepc           (h0_mepc),
      .o_csr_mcause         (h0_mcause),
      .o_csr_mtval          (h0_mtval),
      .o_csr_mscratch       (),
      .o_csr_mie            (),
      .o_csr_mip            (),
      .o_csr_fcsr           (),
      .o_csr_fflags         (),
      .o_csr_frm            (),
      .o_fp_issue_ctr       (),
      .o_fp_commit_ctr      (),
      .o_fp_flags_ctr       (),
      .o_fp_merge_ctr       (),
      .o_vec_vtype          (),
      .o_vec_vl             (),
      .o_vec_vstart         (),
      .o_vec_vcsr           (),
      .o_vec_vlenb          (),
      .o_vec_vlmax          (),
      .o_vec_vill           (),
      .o_vec_macro_ctr      (),
      .o_vec_elem_ctr       (),
      .o_vec_trap_ctr       (),
      .o_vec_retire_ctr     (),
      .o_vec_fault_ctr      (),
      .o_vec_lsu_req_ctr    (),
      .o_vec_chain_accept_ctr(),
      .o_vec_chain_refuse_ctr(),
      .o_vec_desc_alloc_ctr (),
      .o_vec_desc_release_ctr(),
      .o_vec_alu_elems      (),
      .o_vec_alu_src_rd_ctr (),
      .o_vec_alu_acc        (),
      .o_vec_vrf_rd_ctr     (),
      .o_vec_vrf_wr_ctr     (),
      .o_vec_vrf_bad_ctr    (),
      .o_vec_vrf_rows       (),
      .o_vec_vrf_banks      (),
      .o_vec_dbg0           (),
      .o_vec_dbg1           (),
      .o_vec_dbg2           (),
      .o_lane_quota         (),
      .o_lane_req_quota     (),
      .o_lane_gen           (),
      .o_lane_busy          (),
      .o_lane_stop_admit    (),
      .o_lane_macro_live    (),
      .o_lane_publish_ctr   (),
      .o_lane_ack_req_ctr   (),
      .o_lane_ack_ctr       (),
      .o_lane_req_mid_macro_ctr(),
      .o_lane_pub_mid_macro_ctr(),
      .o_lane_abort_ctr     (),
      .o_lane_elem_ctr      (),
      .o_vec_lsu_merge_ctr  (),
      .o_lane_quota_max     (),
      .o_lane_quota_reset   (),
      .o_geom_prf_banks     (),
      .o_geom_cache_line_bytes(),
      .o_geom_cache_sets    (),
      .o_geom_cache_ways    (),
      .o_csr_wr_ctr         (),
      .o_csr_illegal_wr_ctr (),
      .o_csr_trap_ctr       (),
      .o_csr_mret_ctr       (),
      .o_trap_valid         (h0_trap_valid),
      .o_trap_is_irq        (),
      .o_trap_cause         (h0_trap_cause),
      .o_trap_tval          (h0_trap_tval),
      .o_trap_epc           (h0_trap_epc),
      .o_trap_target        (),
      .o_mret_valid         (),
      .o_mret_target        (),
      .o_priv               (h0_priv),
      .o_medeleg            (),
      .o_mideleg            (),
      .o_sret_valid         (),
      .o_sret_target        (),
      .o_sstatus            (),
      .o_stvec              (),
      .o_sepc               (),
      .o_scause             (),
      .o_stval              (),
      .o_sscratch           (),
      .o_satp               (h0_satp),
      .o_csr_sret_ctr       (),
      .o_csr_trap_s_ctr     (),
      .o_csr_priv_illegal_ctr(),
      .o_csr_priv_change_ctr(),
      .o_pmp_query_ctr      (),
      .o_pmp_deny_ctr       (),
      .o_pmp_fetch_deny_ctr (),
      .o_pmp_locked_ctr     (),
      .o_pmp_store_deny_ctr (),
      .o_pmp_data_matched   (),
      .o_pmp_data_locked    (),
      .o_pmp_fetch_matched  (),
      .o_pmp_fetch_locked   (),
      .o_csr_pmp_sel        (),
      .o_pmp_cfg            (),
      .o_pmp_addr           (),
      .o_irq_valid          (),
      .o_irq_cause          (),
      .o_irq_ctr            (),
      .o_wfi_halt           (),
      .o_spurious_wake_ctr  (),
      .o_halt_cycles        (),
      .o_sys_exec_ctr       (),
      .o_exc_capture_ctr    (),
      .o_exc_gen_mismatch_ctr(),
      .o_trap_irq_ctr       (),
      .o_sys_redirect_ctr   (),
      .o_tlb_hit_ctr        (),
      .o_tlb_miss_ctr       (),
      .o_tlb_install_ctr    (),
      .o_tlb_walk_ctr       (),
      .o_tlb_stale_ctr      (),
      .o_tlb_sfence_ctr     (),
      .o_tlb_satp_flush_ctr (),
      .o_tlb_gen            (),
      .o_dbg_deliver_valid  (),
      .o_dbg_deliver_pc     (),
      .o_dbg_deliver_bits   (),
      .o_dbg_head_valid     (),
      .o_dbg_head_complete  (),
      .o_dbg_head_index     (),
      .o_dbg_head_pc        (),
      .o_dbg_desc_rd0       (),
      .o_dbg_desc_rd1       (),
      .o_dbg_alloc_ctr      (),
      .o_dbg_ins_ctr        (),
      .o_dbg_spec_map       (),
      .o_dbg_gen_valid      (),
      .o_dbg_wb_done        (),
      .o_fab_unit_issues    (),
      .o_fab_reason_ctr     (),
      .o_fab_grant_ctr      (),
      .o_fab_stall_ctr      (),
      .o_fab_reject_ctr     (),
      .o_fab_units          (),
      .o_fab_classes        (),
      .o_fab_age_w          (),
      .o_fab_occ_w          (),
      .o_fab_unit_w         (),
      .o_fab_alloc_bank     (),
      .o_fab_bp_captured_ctr(),
      .o_fab_bp_hit_ctr     (),
      .o_fab_bp_unauth_ctr  (),
      .o_fab_bp_flush_ctr   ()
  );

  assign h0_redirect_valid = h0_redirect_valid_w;

  mosaic_core #(
      .HART_ID   (1),
      .RESET_PC  (HART1_RESET_PC)
  ) u_h1 (
      .clk                  (clk),
      .rst                  (h1_rst),
      .fab_dyn_i            (1'b0),
      .cache_en_i           (1'b0),
      .loc_llb_en_i         (1'b0),
      .loc_pf_en_i          (1'b0),
      .loc_pf_conf_thresh_i (2'b0),
      .loc_snoop_all_i      (1'b0),
      .lane_quota_req_i     (1'b0),
      .lane_quota_val_i     (4'd0),
      .vec_coalesce_dis_i   (1'b0),
      .imem_req_valid       (h1_imem_req_valid),
      .imem_req             (h1_imem_req),
      .imem_req_ready       (h1_imem_req_ready),
      .imem_req_id          (h1_imem_req_id),
      .imem_req_epoch       (h1_imem_req_epoch),
      .imem_rsp_valid       (h1_imem_rsp_valid),
      .imem_rsp_ready       (h1_imem_rsp_ready),
      .imem_rsp             (h1_imem_rsp),
      .imem_rsp_id          (h1_imem_rsp_id),
      .imem_rsp_epoch       (h1_imem_rsp_epoch),
      .imem_rsp_len         (h1_imem_rsp_len),
      .dmem_req_valid       (h1_dmem_req_valid),
      .dmem_req             (h1_dmem_req),
      .dmem_req_ready       (h1_dmem_req_ready),
      .dmem_rsp_valid       (h1_dmem_rsp_valid),
      .dmem_rsp_ready       (h1_dmem_rsp_ready),
      .dmem_rsp             (h1_dmem_rsp),
      .ext_write_valid      (1'b0),
      .ext_write_addr       (64'd0),
      .ext_write_bytes      (4'd0),
      .o_mem_res_valid      (),
      .o_mem_res_granule    (),
      .o_mem_lr_ctr         (),
      .o_mem_sc_ok_ctr      (),
      .o_mem_sc_fail_ctr    (),
      .o_mem_res_ext_inval_ctr(),
      .o_mem_dmem_kind      (),
      .o_loc_llb_hit        (),
      .o_loc_llb_miss       (),
      .o_loc_llb_bypass     (),
      .o_loc_llb_fill       (),
      .o_loc_llb_fill_refused(),
      .o_loc_llb_inv        (),
      .o_loc_mem_line       (),
      .o_loc_pf_issued      (),
      .o_loc_pf_useful      (),
      .o_loc_pf_useless     (),
      .o_loc_pf_late        (),
      .o_loc_pf_cancelled   (),
      .o_loc_pf_admitted    (),
      .o_loc_pf_fill        (),
      .o_loc_pf_fill_refused(),
      .o_loc_pf_mem         (),
      .ev_valid             (h1_ev_valid),
      .ev_trap              (h1_ev_trap),
      .ev_seq               (h1_ev_seq),
      .ev_pc                (h1_ev_pc),
      .ev_len               (h1_ev_len),
      .ev_insn              (h1_ev_insn),
      .ev_id                (),
      .ev_reg_we            (h1_ev_reg_we),
      .ev_rd                (h1_ev_rd),
      .ev_value             (h1_ev_value),
      .ev_store             (h1_ev_store),
      .ev_store_addr        (h1_ev_store_addr),
      .ev_store_data        (h1_ev_store_data),
      .ev_store_size        (h1_ev_store_size),
      .ev_csr_we            (h1_ev_csr_we),
      .ev_csr_addr          (h1_ev_csr_addr),
      .ev_csr_value         (h1_ev_csr_value),
      .ev_trap_cause        (h1_ev_trap_cause),
      .ev_trap_tval         (h1_ev_trap_tval),
      .o_mem_lq_alloc       (),
      .o_mem_lq_issue       (),
      .o_mem_lq_done        (),
      .o_mem_lq_replay      (),
      .o_mem_lq_blocked     (),
      .o_mem_lq_fwd_bytes   (),
      .o_mem_lq_mem_bytes   (),
      .o_mem_lq_fault       (),
      .o_mem_lq_query_mismatch(),
      .o_mem_lq_occupied    (),
      .o_mem_sq_alloc       (),
      .o_mem_sq_commit      (),
      .o_mem_sq_commit2     (),
      .o_mem_sq_commit_stale(),
      .o_mem_sq_drain       (),
      .o_mem_sq_squash      (),
      .o_mem_sq_spared      (),
      .o_mem_sq_occupied    (),
      .o_mem_sq_auth        (),
      .o_mem_sq_fault       (),
      .o_mem_lsu_txn        (),
      .o_mem_lsu_misaligned (),
      .o_mem_lsu_access_fault(),
      .o_mem_lsu_busy       (),
      .o_mem_ins_stall      (),
      .o_mem_squash_valid   (),
      .o_mem_dev_txn        (),
      .o_mem_ram_txn        (),
      .o_mem_dev_wait       (),
      .o_mem_dev_hold       (),
      .o_mem_dmem_dev       (),
      .o_mem_dmem_id        (h1_dmem_id),
      .o_dbg_mmio           (),
      .o_commit_ctr         (h1_commit_ctr),
      .o_unsupported_ctr    (),
      .o_illegal_ctr        (),
      .o_redirect_ctr       (h1_redirect_ctr),
      .o_recovering_ctr     (),
      .o_stop_ctr           (),
      .o_stopped            (h1_stopped),
      .o_cycle_ctr          (),
      .o_c0_count           (),
      .o_c1_count           (),
      .o_c0_grant_valid     (),
      .o_c1_grant_valid     (),
      .o_c0_grant_uop       (),
      .o_c1_grant_uop       (),
      .o_c0_alu_ctr         (),
      .o_c1_alu_ctr         (),
      .o_c0_branch_ctr      (),
      .o_c1_branch_ctr      (),
      .o_muldiv_ctr         (),
      .o_wb_pub_valid       (),
      .o_wb_pub_index       (),
      .o_wb_pub_gen         (),
      .o_wb_pub_value       (),
      .o_wb_wr_ctr          (),
      .o_wb_wake_ctr        (),
      .o_wb_stale_ctr       (),
      .o_wb_dup_ctr         (),
      .o_wb_collision_ctr   (),
      .o_wb_drop_ctr        (),
      .o_free_count         (),
      .o_squash_not_committed_ctr(),
      .o_squash_underflow_ctr(),
      .o_journal_overflow_ctr(),
      .o_rob_occupied       (h1_rob_occupied),
      .o_rob_free           (),
      .o_desc_live          (),
      .o_rename_boundary    (),
      .o_redir_act_ctr      (),
      .o_redir_wait_ctr     (),
      .o_redir_dead_ctr     (),
      .o_redirect_valid     (h1_redirect_valid_w),
      .o_redirect_pc        (h1_redirect_pc),
      .o_fetch_pc           (h1_fetch_pc),
      .o_squash_acc_ctr     (),
      .o_ckpt_ctr           (),
      .o_dbg_redir_bundle   (),
      .o_dbg_fetch_state    (),
      .irq_soft_i           (1'b0),
      .irq_timer_i          (1'b0),
      .irq_ext_i            (1'b0),
      .mtime_i              (64'd0),
      .o_csr_mstatus        (h1_mstatus),
      .o_csr_mtvec          (),
      .o_csr_mepc           (h1_mepc),
      .o_csr_mcause         (h1_mcause),
      .o_csr_mtval          (h1_mtval),
      .o_csr_mscratch       (),
      .o_csr_mie            (),
      .o_csr_mip            (),
      .o_csr_fcsr           (),
      .o_csr_fflags         (),
      .o_csr_frm            (),
      .o_fp_issue_ctr       (),
      .o_fp_commit_ctr      (),
      .o_fp_flags_ctr       (),
      .o_fp_merge_ctr       (),
      .o_vec_vtype          (),
      .o_vec_vl             (),
      .o_vec_vstart         (),
      .o_vec_vcsr           (),
      .o_vec_vlenb          (),
      .o_vec_vlmax          (),
      .o_vec_vill           (),
      .o_vec_macro_ctr      (),
      .o_vec_elem_ctr       (),
      .o_vec_trap_ctr       (),
      .o_vec_retire_ctr     (),
      .o_vec_fault_ctr      (),
      .o_vec_lsu_req_ctr    (),
      .o_vec_chain_accept_ctr(),
      .o_vec_chain_refuse_ctr(),
      .o_vec_desc_alloc_ctr (),
      .o_vec_desc_release_ctr(),
      .o_vec_alu_elems      (),
      .o_vec_alu_src_rd_ctr (),
      .o_vec_alu_acc        (),
      .o_vec_vrf_rd_ctr     (),
      .o_vec_vrf_wr_ctr     (),
      .o_vec_vrf_bad_ctr    (),
      .o_vec_vrf_rows       (),
      .o_vec_vrf_banks      (),
      .o_vec_dbg0           (),
      .o_vec_dbg1           (),
      .o_vec_dbg2           (),
      .o_lane_quota         (),
      .o_lane_req_quota     (),
      .o_lane_gen           (),
      .o_lane_busy          (),
      .o_lane_stop_admit    (),
      .o_lane_macro_live    (),
      .o_lane_publish_ctr   (),
      .o_lane_ack_req_ctr   (),
      .o_lane_ack_ctr       (),
      .o_lane_req_mid_macro_ctr(),
      .o_lane_pub_mid_macro_ctr(),
      .o_lane_abort_ctr     (),
      .o_lane_elem_ctr      (),
      .o_vec_lsu_merge_ctr  (),
      .o_lane_quota_max     (),
      .o_lane_quota_reset   (),
      .o_geom_prf_banks     (),
      .o_geom_cache_line_bytes(),
      .o_geom_cache_sets    (),
      .o_geom_cache_ways    (),
      .o_csr_wr_ctr         (),
      .o_csr_illegal_wr_ctr (),
      .o_csr_trap_ctr       (),
      .o_csr_mret_ctr       (),
      .o_trap_valid         (h1_trap_valid),
      .o_trap_is_irq        (),
      .o_trap_cause         (h1_trap_cause),
      .o_trap_tval          (h1_trap_tval),
      .o_trap_epc           (h1_trap_epc),
      .o_trap_target        (),
      .o_mret_valid         (),
      .o_mret_target        (),
      .o_priv               (h1_priv),
      .o_medeleg            (),
      .o_mideleg            (),
      .o_sret_valid         (),
      .o_sret_target        (),
      .o_sstatus            (),
      .o_stvec              (),
      .o_sepc               (),
      .o_scause             (),
      .o_stval              (),
      .o_sscratch           (),
      .o_satp               (h1_satp),
      .o_csr_sret_ctr       (),
      .o_csr_trap_s_ctr     (),
      .o_csr_priv_illegal_ctr(),
      .o_csr_priv_change_ctr(),
      .o_pmp_query_ctr      (),
      .o_pmp_deny_ctr       (),
      .o_pmp_fetch_deny_ctr (),
      .o_pmp_locked_ctr     (),
      .o_pmp_store_deny_ctr (),
      .o_pmp_data_matched   (),
      .o_pmp_data_locked    (),
      .o_pmp_fetch_matched  (),
      .o_pmp_fetch_locked   (),
      .o_csr_pmp_sel        (),
      .o_pmp_cfg            (),
      .o_pmp_addr           (),
      .o_irq_valid          (),
      .o_irq_cause          (),
      .o_irq_ctr            (),
      .o_wfi_halt           (),
      .o_spurious_wake_ctr  (),
      .o_halt_cycles        (),
      .o_sys_exec_ctr       (),
      .o_exc_capture_ctr    (),
      .o_exc_gen_mismatch_ctr(),
      .o_trap_irq_ctr       (),
      .o_sys_redirect_ctr   (),
      .o_tlb_hit_ctr        (),
      .o_tlb_miss_ctr       (),
      .o_tlb_install_ctr    (),
      .o_tlb_walk_ctr       (),
      .o_tlb_stale_ctr      (),
      .o_tlb_sfence_ctr     (),
      .o_tlb_satp_flush_ctr (),
      .o_tlb_gen            (),
      .o_dbg_deliver_valid  (),
      .o_dbg_deliver_pc     (),
      .o_dbg_deliver_bits   (),
      .o_dbg_head_valid     (),
      .o_dbg_head_complete  (),
      .o_dbg_head_index     (),
      .o_dbg_head_pc        (),
      .o_dbg_desc_rd0       (),
      .o_dbg_desc_rd1       (),
      .o_dbg_alloc_ctr      (),
      .o_dbg_ins_ctr        (),
      .o_dbg_spec_map       (),
      .o_dbg_gen_valid      (),
      .o_dbg_wb_done        (),
      .o_fab_unit_issues    (),
      .o_fab_reason_ctr     (),
      .o_fab_grant_ctr      (),
      .o_fab_stall_ctr      (),
      .o_fab_reject_ctr     (),
      .o_fab_units          (),
      .o_fab_classes        (),
      .o_fab_age_w          (),
      .o_fab_occ_w          (),
      .o_fab_unit_w         (),
      .o_fab_alloc_bank     (),
      .o_fab_bp_captured_ctr(),
      .o_fab_bp_hit_ctr     (),
      .o_fab_bp_unauth_ctr  (),
      .o_fab_bp_flush_ctr   ()
  );

  assign h1_redirect_valid = h1_redirect_valid_w;

  // -------------------------------------------------------------------------
  // The shared-service arbiter and the ownership router.
  // -------------------------------------------------------------------------
  // Slot index {hart, src}: 0 = h0 data, 1 = h0 inst, 2 = h1 data, 3 = h1 inst.
  // The index carries the *whole* hart field, so a response naming a hart this
  // machine does not have indexes a slot that is never busy -- which is how the
  // ownership check rejects it instead of silently folding it onto hart 0.
  localparam int unsigned MH_SLOT_W = MH_HART_W + 1;
  localparam int unsigned MH_SLOT_N = 1 << MH_SLOT_W;
  logic [3:0] slot_req;
  logic [3:0] slot_req_masked;
  logic [MH_SLOT_N-1:0] slot_busy;
  logic [MH_REQ_ID_W-1:0] slot_id    [0:MH_SLOT_N-1];
  logic [MH_EPOCH_W-1:0]  slot_epoch [0:MH_SLOT_N-1];
  // A matched response is captured here and freed from the bus immediately, so
  // a core that is not ready for its response this cycle can never hold the one
  // shared response channel (and with it the other hart). One buffer per slot
  // suffices: each core port is single-outstanding.
  logic [3:0]             rsp_pend;   // only the four real slots
  logic [MH_XLEN-1:0]     rsp_rdata [0:3];
  logic                   rsp_fault [0:3];
  logic [MH_REQ_ID_W-1:0] rsp_id    [0:3];
  logic [MH_EPOCH_W-1:0]  rsp_epoch [0:3];
  logic [2:0]             rsp_len   [0:3];

  assign slot_req[0] = h0_dmem_req_valid && hart_en_i[0];
  assign slot_req[1] = h0_imem_req_valid && hart_en_i[0];
  assign slot_req[2] = h1_dmem_req_valid && hart_en_i[1];
  assign slot_req[3] = h1_imem_req_valid && hart_en_i[1];

`ifdef MOSAIC_MH_MUTANT_ONE_HART_BUS
  // NEGATIVE CONTROL: the arbiter never grants hart 1, so the second hart's
  // requests never reach the shared service. The "both harts used the same
  // service" check (`o_req_h1_ctr > 0`) must catch it, and hart 1 stops making
  // progress. It is the control on the sharing claim itself.
  assign slot_req_masked = slot_req & 4'b0011;
`else
  assign slot_req_masked = slot_req;
`endif

  // Rotating priority across the four slots. The rotation is what keeps one
  // hart from starving the other on the one shared port.
  logic [3:0] cand, cand_rot;
  logic [1:0] rr_ptr, pick_rot, grant;
  logic       grant_valid;
  always_comb begin
    cand      = slot_req_masked & ~slot_busy[3:0] & ~rsp_pend[3:0];
    cand_rot  = (cand << rr_ptr) | (cand >> (4 - rr_ptr));
    grant_valid = |cand_rot;
    pick_rot  = 2'd0;
    if (cand_rot[1])      pick_rot = 2'd1;
    else if (cand_rot[2]) pick_rot = 2'd2;
    else if (cand_rot[3]) pick_rot = 2'd3;
    else                  pick_rot = 2'd0;
    // `cand_rot[i]` is `cand[(i - rr_ptr) mod 4]`, so the original slot index is
    // `(pick_rot - rr_ptr) mod 4`; 2-bit arithmetic wraps the subtraction.
    grant     = pick_rot - rr_ptr;
  end

  always_comb begin
    mem_req_valid = grant_valid;
    mem_req       = '0;
    mem_req_hart  = {MH_HART_W{1'b0}};
    mem_req_src   = 1'b0;
    mem_req_id    = {MH_REQ_ID_W{1'b0}};
    mem_req_epoch = {MH_EPOCH_W{1'b0}};
    case (grant)
      2'd0: begin
        mem_req       = h0_dmem_req;
        mem_req_src   = 1'b0;
        mem_req_hart  = MH_HART_W'(0);
      end
      2'd1: begin
        mem_req       = h0_imem_req;
        mem_req_src   = 1'b1;
        mem_req_hart  = MH_HART_W'(0);
        mem_req_id    = h0_imem_req_id;
        mem_req_epoch = h0_imem_req_epoch;
      end
      2'd2: begin
        mem_req       = h1_dmem_req;
        mem_req_src   = 1'b0;
        mem_req_hart  = MH_HART_W'(1);
      end
      default: begin
        mem_req       = h1_imem_req;
        mem_req_src   = 1'b1;
        mem_req_hart  = MH_HART_W'(1);
        mem_req_id    = h1_imem_req_id;
        mem_req_epoch = h1_imem_req_epoch;
      end
    endcase
`ifdef MOSAIC_MH_MUTANT_REUSE_TAG
    // NEGATIVE CONTROL: every request is tagged hart 0 -- a hart-ID reuse. A
    // response for hart 1's request then comes back naming hart 0's slot, so it
    // is either dropped (mismatch) or delivered to the wrong hart. The
    // ownership check and the per-hart stream comparison must catch it.
    mem_req_hart = MH_HART_W'(0);
`endif
  end

  // The core's ready for the granted slot, and the accept pulse that records the
  // outstanding request's identity.
  // The granted core is told its request was taken only when the shared bus
  // took it. A core whose slot was not granted sees ready low and holds its
  // request, exactly as the core's transport contract requires.
  assign h0_dmem_req_ready = (grant == 2'd0) && mem_req_valid && mem_req_ready;
  assign h0_imem_req_ready = (grant == 2'd1) && mem_req_valid && mem_req_ready;
  assign h1_dmem_req_ready = (grant == 2'd2) && mem_req_valid && mem_req_ready;
  assign h1_imem_req_ready = (grant == 2'd3) && mem_req_valid && mem_req_ready;
  logic accept_c;
  assign accept_c = mem_req_valid && mem_req_ready;

  assign o_geom_hart_w   = 32'(MH_HART_W);
  assign o_geom_mem_id_w = 32'(MH_MEM_ID_W);
  assign o_geom_ret_n    = 32'(MH_RET_N);
  assign o_geom_seq_w    = 32'(MH_SEQ_W);
  assign h0_dmem_id_valid = accept_c && (grant == 2'd0);
  assign h1_dmem_id_valid = accept_c && (grant == 2'd2);

  // Response routing. `rslot` is the slot the response's own identity names;
  // `rmatch` requires that slot to be waiting for exactly this request. A
  // response that names no such slot is a detected ownership error and is
  // consumed and dropped rather than delivered to the wrong hart.
  logic [MH_SLOT_W-1:0] rslot;
`ifdef MOSAIC_MH_MUTANT_IGNORE_HART
  // NEGATIVE CONTROL: the response's hart is ignored and every response is
  // routed to hart 0's slot of that source. The card's "hart-ID reuse must not
  // confuse a response" fail mode, injected at the matcher itself.
  assign rslot = MH_SLOT_W'(mem_rsp_src);
`else
  assign rslot = {mem_rsp_hart, mem_rsp_src};
`endif
  logic rmatch;
  assign rmatch = mem_rsp_valid && slot_busy[rslot] &&
                  (slot_id[rslot] == mem_rsp_id) &&
                  (slot_epoch[rslot] == mem_rsp_epoch);

  // The bus is *always* accepted: a matched response is buffered into its slot,
  // an unmatched one is consumed and counted. This is what makes the shared
  // response channel independent of any one core's readiness.
  assign mem_rsp_ready = 1'b1;

  // Present the buffered response to the core that owns the slot.
  assign h0_dmem_rsp_valid = rsp_pend[0];
  assign h0_imem_rsp_valid = rsp_pend[1];
  assign h1_dmem_rsp_valid = rsp_pend[2];
  assign h1_imem_rsp_valid = rsp_pend[3];

  assign h0_dmem_rsp.rdata = rsp_rdata[0];
  assign h0_dmem_rsp.fault = rsp_fault[0];
  assign h1_dmem_rsp.rdata = rsp_rdata[2];
  assign h1_dmem_rsp.fault = rsp_fault[2];

  assign h0_imem_rsp.rdata = rsp_rdata[1];
  assign h0_imem_rsp.fault = rsp_fault[1];
  assign h0_imem_rsp_id    = rsp_id[1];
  assign h0_imem_rsp_epoch = rsp_epoch[1];
  assign h0_imem_rsp_len   = rsp_len[1];

  assign h1_imem_rsp.rdata = rsp_rdata[3];
  assign h1_imem_rsp.fault = rsp_fault[3];
  assign h1_imem_rsp_id    = rsp_id[3];
  assign h1_imem_rsp_epoch = rsp_epoch[3];
  assign h1_imem_rsp_len   = rsp_len[3];

  // -------------------------------------------------------------------------
  // State: the outstanding-request slots and the evidence counters.
  // -------------------------------------------------------------------------
  integer si;
  always_ff @(posedge clk) begin
    if (rst) begin
      slot_busy          <= {MH_SLOT_N{1'b0}};
      rsp_pend           <= 4'b0000;
      rr_ptr             <= 2'd0;
      o_req_ctr          <= 32'd0;
      o_req_h0_ctr       <= 32'd0;
      o_req_h1_ctr       <= 32'd0;
      o_req_imem_ctr     <= 32'd0;
      o_req_dmem_ctr     <= 32'd0;
      o_contend_ctr      <= 32'd0;
      o_owner_mismatch_ctr <= 32'd0;
      for (si = 0; si < 4; si = si + 1) begin
        slot_id[si]    <= {MH_REQ_ID_W{1'b0}};
        slot_epoch[si] <= {MH_EPOCH_W{1'b0}};
        rsp_rdata[si]  <= {MH_XLEN{1'b0}};
        rsp_fault[si]  <= 1'b0;
        rsp_id[si]     <= {MH_REQ_ID_W{1'b0}};
        rsp_epoch[si]  <= {MH_EPOCH_W{1'b0}};
        rsp_len[si]    <= 3'b000;
      end
    end else begin
      if (accept_c) begin
        slot_busy[MH_SLOT_W'(grant)]  <= 1'b1;
        slot_id[MH_SLOT_W'(grant)]    <= mem_req_id;
        slot_epoch[MH_SLOT_W'(grant)] <= mem_req_epoch;
        rr_ptr            <= grant + 2'd1;
        o_req_ctr         <= o_req_ctr + 32'd1;
        if (mem_req_hart[0]) o_req_h1_ctr <= o_req_h1_ctr + 32'd1;
        else                 o_req_h0_ctr <= o_req_h0_ctr + 32'd1;
        if (mem_req_src)     o_req_imem_ctr <= o_req_imem_ctr + 32'd1;
        else                 o_req_dmem_ctr <= o_req_dmem_ctr + 32'd1;
      end
      if (mem_rsp_valid) begin
        if (rmatch) begin
          // An invalid hart (>= MH_HARTS) can never be `rmatch`, so the
          // clamp to the four real slots is unreachable there.
          rsp_pend[rslot[1:0]]  <= 1'b1;
          rsp_rdata[rslot[1:0]] <= mem_rsp.rdata;
          rsp_fault[rslot[1:0]] <= mem_rsp.fault;
          rsp_id[rslot[1:0]]    <= mem_rsp_id;
          rsp_epoch[rslot[1:0]] <= mem_rsp_epoch;
          rsp_len[rslot[1:0]]   <= mem_rsp_len;
          slot_busy[rslot]      <= 1'b0;
        end else begin
          o_owner_mismatch_ctr <= o_owner_mismatch_ctr + 32'd1;
        end
      end
      if (rsp_pend[0] && h0_dmem_rsp_ready) rsp_pend[0] <= 1'b0;
      if (rsp_pend[1] && h0_imem_rsp_ready) rsp_pend[1] <= 1'b0;
      if (rsp_pend[2] && h1_dmem_rsp_ready) rsp_pend[2] <= 1'b0;
      if (rsp_pend[3] && h1_imem_rsp_ready) rsp_pend[3] <= 1'b0;
      if ((|slot_busy[1:0]) && (|slot_busy[3:2])) begin
        o_contend_ctr <= o_contend_ctr + 32'd1;
      end
    end
  end

  /* verilator lint_on PINCONNECTEMPTY */

endmodule : mosaic_multihart

`default_nettype wire
