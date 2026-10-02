// ============================================================================
// mosaic_load_queue_tb -- simulation wrapper for CASE=lsu.byte_forwarding
// (work package I-035).
//
// Simulation-only glue. It contains no behaviour of its own: every port is
// either driven by sim/unit/tb_load.cpp or wired straight out of the DUTs.
// There is no clock generation, no reset generation and no `$display` in here,
// per sim/common/sim_common.h -- the C++ side owns all three.
//
// ------------------------------------------------------ the three modules wired
//
// The case is a joint statement about the load queue, the store queue and the
// memory endpoint, so this wrapper instantiates all of them and connects them
// the way the core will:
//
//     mosaic_load_queue.sq_query_addr_o/size_o -> mosaic_store_queue.fwd_query_*
//     mosaic_store_queue.fwd_valid_o/data_o/blocked_o -> mosaic_load_queue.sq_query_*_i
//     mosaic_store_queue.o_entry_pay/o_count  -> mosaic_load_queue.sq_entry_pay_i/sq_count_i
//     mosaic_load_queue  <-> mosaic_lsu_endpoint (load)  <-> memory
//     mosaic_store_queue <-> mosaic_lsu_endpoint (store) <-> memory
//
// Two endpoint instances, one per port. The endpoint holds one transaction at a
// time, and the load queue's load and the store queue's store are two
// independent producers; arbitrating them onto one shared memory port is the
// core's business and is not in this package. Both endpoints' downstream ports
// are driven by the same C++ memory model, i.e. one address space, so a load can
// observe what a committed store wrote and never sees a store's bytes before
// they left the queue.
//
// The store queue's *entry view* is wired to the load queue as well as its
// forwarding query, because the query cannot express program order (the store
// queue's own header says so). See rtl/core/mosaic_load_queue.sv's header for
// the rule each port implements and the tension that choice records.
//
// ---------------------------------------------------- why the ports are flat
//
// The DUTs' packet ports are packed structs (`mosaic_uop_pkg::lsu_req_t`,
// `lsu_rsp_t`, `uop_id_t`). A Verilator C++ driver can only poke those as raw
// bit vectors, so it would have to reproduce their layout -- a second
// transcription of rtl/core/mosaic_uop_pkg.sv, which is exactly the duplication
// that file exists to prevent. So this wrapper takes flat driver-facing ports
// and assembles/flattens the packets field by field. No assignment patterns
// (`'{...}`): Yosys cannot parse them.
//
// `mosaic_uop_pkg` is *used* here (its types) but deliberately not included: it
// is compiled earlier in the same command. `mosaic_cfg_pkg.svh` is included
// because the geometry constants it names must be the generated ones.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per knob for the whole project;
// this wrapper names the LSU subset and the rest are unused here.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_XLEN     = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned TB_ID_W     = $bits(mosaic_uop_pkg::uop_id_t);
localparam int unsigned TB_STRB_W   = TB_XLEN / 8;
localparam int unsigned TB_SIZE_W   = 3;
// This wrapper's own re-expression of the store queue's observation layout:
// data_valid, addr_valid, data, size, imm, base, id.
localparam int unsigned TB_SQ_ENTRY_W = 5 + 3 * TB_XLEN + TB_ID_W;
// This wrapper's own re-expression of the load queue's observation layout:
// size, signed, imm, base, id.
localparam int unsigned TB_LQ_ENTRY_W = 4 + 2 * TB_XLEN + TB_ID_W;
localparam int unsigned TB_LQ_ENTRIES = mosaic_cfg_pkg::MOSAIC_LQ_ENTRIES;
localparam int unsigned TB_SQ_ENTRIES = mosaic_cfg_pkg::MOSAIC_SQ_ENTRIES;
localparam int unsigned TB_CNT_W      = $clog2(TB_LQ_ENTRIES + 1);
localparam int unsigned TB_IDX_W      = (TB_LQ_ENTRIES <= 1) ? 1 : $clog2(TB_LQ_ENTRIES);
localparam int unsigned TB_SQ_CNT_W   = $clog2(TB_SQ_ENTRIES + 1);

/* verilator lint_off UNUSEDSIGNAL */
// The driver-facing ports are fixed 32-bit vectors, so the C++ driver carries no
// packet geometry, while the packet fields they are narrowed to are a few bits
// wide. The unused upper bits are the price of that convention and are
// deliberate, not an omission; the widths they are narrowed to are re-exported
// in the geometry block below and the driver checks them.
module mosaic_load_queue_tb (
    input  logic                        clk,
    input  logic                        rst,

    // ------------------------------------------------------ load allocation
    input  logic                        lq_alloc_valid,
    output logic                        lq_alloc_ready,
    input  logic [31:0]                 lq_alloc_id,
    input  logic [63:0]                 lq_alloc_base,
    input  logic [63:0]                 lq_alloc_imm,
    input  logic [31:0]                 lq_alloc_size,
    input  logic                        lq_alloc_signed,

    // ---------------------------------------------------------- load result
    output logic                        lq_result_valid,
    input  logic                        lq_result_ready,
    output logic [31:0]                 lq_result_id,
    output logic [63:0]                 lq_result_data,
    output logic [63:0]                 lq_result_cause,
    output logic [63:0]                 lq_result_tval,
    output logic                        lq_result_fault,
    output logic [31:0]                 lq_result_fwd_mask,

    // ------------------------------------------------ load endpoint => memory
    output logic                        lq_mem_req_valid,
    input  logic                        lq_mem_req_ready,
    output logic                        lq_mem_req_we,
    output logic [63:0]                 lq_mem_req_addr,
    output logic [31:0]                 lq_mem_req_size,
    output logic [31:0]                 lq_mem_req_wstrb,
    output logic [63:0]                 lq_mem_req_wdata,
    input  logic                        lq_mem_rsp_valid,
    output logic                        lq_mem_rsp_ready,
    input  logic [63:0]                 lq_mem_rsp_rdata,
    input  logic                        lq_mem_rsp_fault,

    // ------------------------------------------------------- store allocation
    input  logic                        sq_alloc_valid,
    output logic                        sq_alloc_ready,
    input  logic [31:0]                 sq_alloc_id,
    input  logic [63:0]                 sq_alloc_base,
    input  logic [63:0]                 sq_alloc_imm,
    input  logic [31:0]                 sq_alloc_size,
    input  logic                        sq_alloc_addr_valid,
    input  logic [63:0]                 sq_alloc_data,
    input  logic                        sq_alloc_data_valid,

    // ----------------------------------------------------------- late operand
    input  logic                        sq_fill_valid,
    input  logic [31:0]                 sq_fill_id,
    input  logic [63:0]                 sq_fill_base,
    input  logic [63:0]                 sq_fill_imm,
    input  logic                        sq_fill_addr_valid,
    input  logic [63:0]                 sq_fill_data,
    input  logic                        sq_fill_data_valid,
    output logic                        sq_fill_hit,
    output logic                        sq_fill_stale,

    // ------------------------------------------------------- commit / squash
    input  logic                        sq_commit_valid,
    input  logic [31:0]                 sq_commit_id,
    output logic                        sq_commit_ok,
    output logic                        sq_commit_stale,
    input  logic                        sq_squash_valid,
    input  logic                        sq_squash_all,
    input  logic [31:0]                 sq_squash_from,
    input  logic [31:0]                 sq_squash_tail,
    input  logic [31:0]                 sq_squash_gen,

    // ----------------------------------------------- store endpoint => memory
    output logic                        sq_mem_req_valid,
    input  logic                        sq_mem_req_ready,
    output logic                        sq_mem_req_we,
    output logic [63:0]                 sq_mem_req_addr,
    output logic [31:0]                 sq_mem_req_size,
    output logic [31:0]                 sq_mem_req_wstrb,
    output logic [63:0]                 sq_mem_req_wdata,
    input  logic                        sq_mem_rsp_valid,
    output logic                        sq_mem_rsp_ready,
    input  logic [63:0]                 sq_mem_rsp_rdata,
    input  logic                        sq_mem_rsp_fault,

    // ------------------------------------------------------- store drain port
    output logic                        sq_drain_req_valid,
    output logic                        sq_drain_req_ready,
    output logic [31:0]                 sq_drain_req_id,
    output logic [63:0]                 sq_drain_req_base,
    output logic [63:0]                 sq_drain_req_imm,
    output logic [31:0]                 sq_drain_req_size,
    output logic [63:0]                 sq_drain_req_data,

    // ---------------------------------------------------------- observability
    // The load queue's offer and the endpoint handshakes, flattened, so the
    // driver can model the queue's edge exactly (issue, response, completion)
    // without reaching into the wrapper.
    output logic                        o_lq_req_valid,
    output logic [31:0]                 o_lq_req_id,
    output logic [63:0]                 o_lq_req_base,
    output logic [63:0]                 o_lq_req_imm,
    output logic [31:0]                 o_lq_req_size,
    output logic                        o_lq_req_signed,
    output logic                        o_lq_ep_req_ready,
    output logic                        o_lq_ep_rsp_valid,
    output logic                        o_lq_rsp_ready,
    output logic                        o_sq_ep_req_ready,
    output logic [TB_CNT_W-1:0]         o_lq_count,
    output logic [TB_LQ_ENTRIES-1:0]    o_lq_occ,
    output logic [31:0]                 o_lq_alloc_ctr,
    output logic [31:0]                 o_lq_issue_ctr,
    output logic [31:0]                 o_lq_rsp_ctr,
    output logic [31:0]                 o_lq_done_ctr,
    output logic [31:0]                 o_lq_replay_ctr,
    output logic [31:0]                 o_lq_blocked_ctr,
    output logic [31:0]                 o_lq_fwd_byte_ctr,
    output logic [31:0]                 o_lq_mem_byte_ctr,
    output logic [31:0]                 o_lq_fault_ctr,
    output logic [31:0]                 o_lq_query_mismatch_ctr,
    output logic [63:0]                 o_lq_last_fault_cause,
    output logic [63:0]                 o_lq_last_fault_tval,
    output logic [TB_LQ_ENTRIES*TB_LQ_ENTRY_W-1:0] o_lq_entry_pay,

    output logic [TB_SQ_CNT_W-1:0]      o_sq_count,
    output logic [TB_SQ_CNT_W-1:0]      o_sq_auth_cnt,
    output logic [TB_SQ_ENTRIES-1:0]    o_sq_occ,
    output logic [TB_SQ_ENTRIES-1:0]    o_sq_entry_authorised,
    output logic [31:0]                 o_sq_alloc_ctr,
    output logic [31:0]                 o_sq_fill_ctr,
    output logic [31:0]                 o_sq_fill_stale_ctr,
    output logic [31:0]                 o_sq_commit_ctr,
    output logic [31:0]                 o_sq_commit_stale_ctr,
    output logic [31:0]                 o_sq_drain_ctr,
    output logic [31:0]                 o_sq_fault_ctr,
    output logic [31:0]                 o_sq_squash_ctr,
    output logic [31:0]                 o_sq_squash_spared_ctr,
    output logic [TB_SQ_ENTRIES*TB_SQ_ENTRY_W-1:0] o_sq_entry_pay,

    output logic                        o_sq_fwd_valid,
    output logic [63:0]                 o_sq_fwd_data,
    output logic                        o_sq_fwd_blocked,
    output logic [63:0]                 o_sq_query_addr,
    output logic [31:0]                 o_sq_query_size,

    // ------------------------------------------------------------ geometry
    // Read out of the elaborated instances, so the driver sizes its shadow from
    // the hardware that exists. The `o_tb_*` set is this wrapper's own
    // re-expression; the driver requires the two to agree field by field, which
    // is what makes a profile change a build failure rather than a silently
    // narrowed port.
    output logic [31:0]                 o_dut_lq_entries,
    output logic [31:0]                 o_dut_lq_entry_w,
    output logic [31:0]                 o_dut_lq_cnt_w,
    output logic [31:0]                 o_dut_lq_idx_w,
    output logic [31:0]                 o_dut_sq_entries,
    output logic [31:0]                 o_dut_sq_entry_w,
    output logic [31:0]                 o_dut_sq_cnt_w,
    output logic [31:0]                 o_dut_xlen,
    output logic [31:0]                 o_dut_id_w,
    output logic [31:0]                 o_dut_rob_index_w,
    output logic [31:0]                 o_dut_rob_gen_w,
    output logic [31:0]                 o_tb_lq_entries,
    output logic [31:0]                 o_tb_lq_entry_w,
    output logic [31:0]                 o_tb_lq_cnt_w,
    output logic [31:0]                 o_tb_lq_idx_w,
    output logic [31:0]                 o_tb_sq_entries,
    output logic [31:0]                 o_tb_sq_entry_w,
    output logic [31:0]                 o_tb_sq_cnt_w,
    output logic [31:0]                 o_tb_xlen,
    output logic [31:0]                 o_tb_id_w,
    output logic [31:0]                 o_tb_rob_index_w,
    output logic [31:0]                 o_tb_rob_gen_w
);
/* verilator lint_on UNUSEDSIGNAL */

  mosaic_uop_pkg::lsu_req_t lq_req_s;
  logic                     lq_req_valid_s;
  logic                     ep_lq_req_ready;
  mosaic_uop_pkg::lsu_rsp_t ep_lq_rsp_s;
  logic                     ep_lq_rsp_valid;
  logic                     lq_rsp_ready;
  mosaic_uop_pkg::mem_req_t ep_lq_mem_req_s;
  mosaic_uop_pkg::mem_rsp_t ep_lq_mem_rsp_s;

  mosaic_uop_pkg::lsu_req_t sq_drain_req_s;
  logic                     ep_sq_req_ready;
  mosaic_uop_pkg::lsu_rsp_t ep_sq_rsp_s;
  logic                     ep_sq_rsp_valid;
  logic                     sq_drain_rsp_ready;
  mosaic_uop_pkg::mem_req_t ep_sq_mem_req_s;
  mosaic_uop_pkg::mem_rsp_t ep_sq_mem_rsp_s;

  logic [63:0]                 lq_query_addr;
  logic [2:0]                  lq_query_size;
  logic                        sq_fwd_valid_s;
  logic [63:0]                 sq_fwd_data_s;
  logic                        sq_fwd_blocked_s;
  mosaic_uop_pkg::uop_id_t     lq_result_id_s;
  logic [TB_STRB_W-1:0]        lq_result_fwd_mask_s;
  logic [TB_ID_W-1:0]          sq_last_fault_id_s;

  // -------------------------------------------------------------- the load queue
  mosaic_load_queue u_lq (
      .clk                    (clk),
      .rst                    (rst),

      .alloc_valid_i          (lq_alloc_valid),
      .alloc_ready_o          (lq_alloc_ready),
      .alloc_id_i             (lq_alloc_id[TB_ID_W-1:0]),
      .alloc_base_i           (lq_alloc_base),
      .alloc_imm_i            (lq_alloc_imm),
      .alloc_size_i           (lq_alloc_size[TB_SIZE_W-1:0]),
      .alloc_signed_i         (lq_alloc_signed),
      // The destination carry-through is an observation the core uses to build
      // a completion; this case checks the value, the forwarding mask and the
      // byte counters, so the destination is tied to x0 (no register) and the
      // flush is held low. Both are exercised through the core.
      .alloc_dst_tag_i        ({7{1'b0}}),
      .alloc_dst_gen_i        ({7{1'b0}}),
      .alloc_dst_x0_i         (1'b1),
      // I-040: this case is the load queue's own forwarding policy and holds no
      // atomic macro, so the atomic record is idle and no head is atomic. The
      // LR/SC path has its own case.
      .atomic_id_i            ({TB_ID_W{1'b0}}),
      .atomic_valid_i         (1'b0),
      .flush_valid_i          (1'b0),

      .sq_entry_pay_i         (o_sq_entry_pay),
      .sq_count_i             (o_sq_count),

      .sq_query_addr_o        (lq_query_addr),
      .sq_query_size_o        (lq_query_size),
      .sq_query_valid_i       (sq_fwd_valid_s),
      .sq_query_data_i        (sq_fwd_data_s),
      .sq_query_blocked_i     (sq_fwd_blocked_s),

      .req_valid_o            (lq_req_valid_s),
      .req_ready_i            (ep_lq_req_ready),
      .req_o                  (lq_req_s),
      .rsp_valid_i            (ep_lq_rsp_valid),
      .rsp_ready_o            (lq_rsp_ready),
      .rsp_i                  (ep_lq_rsp_s),

      .result_valid_o         (lq_result_valid),
      .result_ready_i         (lq_result_ready),
      .result_id_o            (lq_result_id_s),
      .result_dst_tag_o       (),
      .result_dst_gen_o       (),
      .result_dst_x0_o        (),
      .result_data_o          (lq_result_data),
      .result_cause_o         (lq_result_cause),
      .result_tval_o          (lq_result_tval),
      .result_fault_o         (lq_result_fault),
      .result_fwd_mask_o      (lq_result_fwd_mask_s),

      .o_count                (o_lq_count),
      .o_occ                  (o_lq_occ),
      .o_alloc_ctr            (o_lq_alloc_ctr),
      .o_issue_ctr            (o_lq_issue_ctr),
      .o_rsp_ctr              (o_lq_rsp_ctr),
      .o_done_ctr             (o_lq_done_ctr),
      .o_replay_ctr           (o_lq_replay_ctr),
      .o_blocked_ctr          (o_lq_blocked_ctr),
      .o_fwd_byte_ctr         (o_lq_fwd_byte_ctr),
      .o_mem_byte_ctr         (o_lq_mem_byte_ctr),
      .o_fault_ctr            (o_lq_fault_ctr),
      .o_query_mismatch_ctr   (o_lq_query_mismatch_ctr),
      .o_last_fault_cause     (o_lq_last_fault_cause),
      .o_last_fault_tval      (o_lq_last_fault_tval),
      .o_entry_pay            (o_lq_entry_pay)
  );

  assign lq_result_id = {{(32 - TB_ID_W) {1'b0}}, lq_result_id_s};
  assign lq_result_fwd_mask = {{(32 - TB_STRB_W) {1'b0}}, lq_result_fwd_mask_s};

  // The load queue's offer and the endpoint handshakes, exported so the driver
  // can model the queue's edge (issue / response / completion) exactly.
  assign o_lq_req_valid    = lq_req_valid_s;
  assign o_lq_req_id       = {{(32 - TB_ID_W) {1'b0}}, lq_req_s.id};
  assign o_lq_req_base     = lq_req_s.base;
  assign o_lq_req_imm      = lq_req_s.imm;
  assign o_lq_req_size     = {{(32 - TB_SIZE_W) {1'b0}}, lq_req_s.size};
  assign o_lq_req_signed   = lq_req_s.signed_;
  assign o_lq_ep_req_ready = ep_lq_req_ready;
  assign o_lq_ep_rsp_valid = ep_lq_rsp_valid;
  assign o_lq_rsp_ready    = lq_rsp_ready;
  assign o_sq_ep_req_ready = ep_sq_req_ready;

  // ------------------------------------------------------------ the store queue
  mosaic_store_queue u_sq (
      .clk                  (clk),
      .rst                  (rst),

      .alloc_valid_i        (sq_alloc_valid),
      .alloc_ready_o        (sq_alloc_ready),
      .alloc_id_i           (sq_alloc_id[TB_ID_W-1:0]),
      .alloc_base_i         (sq_alloc_base),
      .alloc_imm_i          (sq_alloc_imm),
      .alloc_size_i         (sq_alloc_size[TB_SIZE_W-1:0]),
      .alloc_addr_valid_i   (sq_alloc_addr_valid),
      .alloc_data_i         (sq_alloc_data),
      .alloc_data_valid_i   (sq_alloc_data_valid),

      .fill_valid_i         (sq_fill_valid),
      .fill_id_i            (sq_fill_id[TB_ID_W-1:0]),
      .fill_base_i          (sq_fill_base),
      .fill_imm_i           (sq_fill_imm),
      .fill_addr_valid_i    (sq_fill_addr_valid),
      .fill_data_i          (sq_fill_data),
      .fill_data_valid_i    (sq_fill_data_valid),
      .fill_hit_o           (sq_fill_hit),
      .fill_stale_o         (sq_fill_stale),

      .commit_valid_i       (sq_commit_valid),
      .commit_id_i          (sq_commit_id[TB_ID_W-1:0]),
      .commit_ok_o          (sq_commit_ok),
      .commit_stale_o       (sq_commit_stale),
      // The second authorisation port is a core-integration requirement (the
      // ROB retires two instructions per cycle); this case drives one
      // authorisation at a time, so the second is held idle rather than
      // duplicated.
      .commit2_valid_i      (1'b0),
      .commit2_id_i         ({TB_ID_W{1'b0}}),
      .commit2_ok_o         (),
      .commit2_stale_o      (),

      .squash_valid_i       (sq_squash_valid),
      .squash_all_i         (sq_squash_all),
      .squash_from_index_i  (sq_squash_from[mosaic_uop_pkg::ROB_INDEX_W-1:0]),
      .squash_tail_index_i  (sq_squash_tail[mosaic_uop_pkg::ROB_INDEX_W-1:0]),
      .squash_gen_i         (sq_squash_gen[mosaic_uop_pkg::ROB_GEN_W-1:0]),

      .drain_req_valid_o    (sq_drain_req_valid),
      .drain_req_ready_i    (ep_sq_req_ready),
      .drain_req_o          (sq_drain_req_s),
      .drain_rsp_valid_i    (ep_sq_rsp_valid),
      .drain_rsp_ready_o    (sq_drain_rsp_ready),
      .drain_rsp_i          (ep_sq_rsp_s),

      .fwd_query_addr_i     (lq_query_addr),
      .fwd_query_size_i     (lq_query_size),
      .fwd_valid_o          (sq_fwd_valid_s),
      .fwd_data_o           (sq_fwd_data_s),
      .fwd_blocked_o        (sq_fwd_blocked_s),

      .o_count              (o_sq_count),
      .o_auth_cnt           (o_sq_auth_cnt),
      .o_occ                (o_sq_occ),
      .o_entry_authorised   (o_sq_entry_authorised),
      .o_alloc_ctr          (o_sq_alloc_ctr),
      .o_fill_ctr           (o_sq_fill_ctr),
      .o_fill_stale_ctr     (o_sq_fill_stale_ctr),
      .o_commit_ctr         (o_sq_commit_ctr),
      .o_commit_stale_ctr   (o_sq_commit_stale_ctr),
      .o_drain_ctr          (o_sq_drain_ctr),
      .o_fault_ctr          (o_sq_fault_ctr),
      .o_squash_ctr         (o_sq_squash_ctr),
      .o_squash_spared_ctr  (o_sq_squash_spared_ctr),
      .o_last_fault_tval    (o_sq_last_fault_tval_s),
      .o_last_fault_cause   (o_sq_last_fault_cause_s),
      .o_last_fault_id      (sq_last_fault_id_s),
      .o_entry_pay          (o_sq_entry_pay)
  );

  logic [63:0]        o_sq_last_fault_tval_s;
  logic [63:0]        o_sq_last_fault_cause_s;

  // The store queue's drain request, flattened for the driver. It is driven *by*
  // the DUT and read *by* the endpoint, so the driver sees exactly the packet the
  // memory side saw, in the same cycle.
  assign sq_drain_req_id   = {{(32 - TB_ID_W) {1'b0}}, sq_drain_req_s.id};
  assign sq_drain_req_base = sq_drain_req_s.base;
  assign sq_drain_req_imm  = sq_drain_req_s.imm;
  assign sq_drain_req_size = {{(32 - TB_SIZE_W) {1'b0}}, sq_drain_req_s.size};
  assign sq_drain_req_data = sq_drain_req_s.store_data;
  assign sq_drain_req_ready = ep_sq_req_ready;

  // --------------------------------------------------------------- endpoints
  /* verilator lint_off PINCONNECTEMPTY */
  mosaic_lsu_endpoint u_ep_lq (
      .clk                (clk),
      .rst                (rst),

      .req_valid_i        (lq_req_valid_s),
      .req_ready_o        (ep_lq_req_ready),
      .req_i              (lq_req_s),
      .req_tval_i         (lq_req_s.base + lq_req_s.imm),
      // This case uses ordinary RAM addresses only (I-038): the device
      // attribute is constant zero here. The attribute itself is covered by
      // CASE=mmio.exactly_once on the integrated core.
      .req_dev_i          (1'b0),
      // No PMP unit in this standalone case: every access is allowed; the
      // endpoint's computed address is not observed here.
      .pmp_deny_i         (1'b0),
      .o_req_addr_o       (),

      .rsp_valid_o        (ep_lq_rsp_valid),
      .rsp_ready_o        (lq_rsp_ready),
      .rsp_o              (ep_lq_rsp_s),

      .mem_req_valid_o    (lq_mem_req_valid),
      .mem_req_ready_i    (lq_mem_req_ready),
      .mem_req_o          (ep_lq_mem_req_s),

      .mem_rsp_valid_i    (lq_mem_rsp_valid),
      .mem_rsp_ready_o    (lq_mem_rsp_ready),
      .mem_rsp_i          (ep_lq_mem_rsp_s),

      // I-040: this case drives no LR/SC and no second agent.
      .ext_write_valid_i  (1'b0),
      .ext_write_addr_i   ({TB_XLEN{1'b0}}),
      .ext_write_bytes_i  (4'd0),
      .flush_i            (1'b0),

      .o_busy             (),
      .o_load_ctr         (),
      .o_store_ctr        (),
      .o_txn_ctr          (),
      .o_misaligned_ctr   (),
      .o_access_fault_ctr (),
      .o_rsp_ctr          (),
      .o_last_fault_cause (),
      .o_last_fault_tval  (),
      .o_inflight_addr    (),
      .o_inflight_size    (),
      // Observed by CASE=mmio.exactly_once on the integrated core (I-038).
      .o_txn_id           (),
      .o_txn_dev          (),
      // Observed by CASE=lrsc.reservation_progress on the integrated core
      // (I-040); this case drives no atomic access.
      .o_txn_kind         (),
      .o_res_valid        (),
      .o_res_granule      (),
      .o_lr_ctr           (),
      .o_sc_ok_ctr        (),
      .o_sc_fail_ctr      (),
      .o_res_set_ctr      (),
      .o_res_clear_ctr    (),
      .o_res_ext_inval_ctr(),
      .o_res_hit_ctr      (),
      .o_res_miss_ctr     ()
  );

  mosaic_lsu_endpoint u_ep_sq (
      .clk                (clk),
      .rst                (rst),

      .req_valid_i        (sq_drain_req_valid),
      .req_ready_o        (ep_sq_req_ready),
      .req_i              (sq_drain_req_s),
      .req_tval_i         (sq_drain_req_s.base + sq_drain_req_s.imm),
      .req_dev_i          (1'b0),
      .pmp_deny_i         (1'b0),
      .o_req_addr_o       (),

      .rsp_valid_o        (ep_sq_rsp_valid),
      .rsp_ready_o        (sq_drain_rsp_ready),
      .rsp_o              (ep_sq_rsp_s),

      .mem_req_valid_o    (sq_mem_req_valid),
      .mem_req_ready_i    (sq_mem_req_ready),
      .mem_req_o          (ep_sq_mem_req_s),

      .mem_rsp_valid_i    (sq_mem_rsp_valid),
      .mem_rsp_ready_o    (sq_mem_rsp_ready),
      .mem_rsp_i          (ep_sq_mem_rsp_s),

      // I-040: this case drives no LR/SC and no second agent.
      .ext_write_valid_i  (1'b0),
      .ext_write_addr_i   ({TB_XLEN{1'b0}}),
      .ext_write_bytes_i  (4'd0),
      .flush_i            (1'b0),

      .o_busy             (),
      .o_load_ctr         (),
      .o_store_ctr        (),
      .o_txn_ctr          (),
      .o_misaligned_ctr   (),
      .o_access_fault_ctr (),
      .o_rsp_ctr          (),
      .o_last_fault_cause (),
      .o_last_fault_tval  (),
      .o_inflight_addr    (),
      .o_inflight_size    (),
      // Observed by CASE=mmio.exactly_once on the integrated core (I-038).
      .o_txn_id           (),
      .o_txn_dev          (),
      // Observed by CASE=lrsc.reservation_progress on the integrated core
      // (I-040); this case drives no atomic access.
      .o_txn_kind         (),
      .o_res_valid        (),
      .o_res_granule      (),
      .o_lr_ctr           (),
      .o_sc_ok_ctr        (),
      .o_sc_fail_ctr      (),
      .o_res_set_ctr      (),
      .o_res_clear_ctr    (),
      .o_res_ext_inval_ctr(),
      .o_res_hit_ctr      (),
      .o_res_miss_ctr     ()
  );
  /* verilator lint_on PINCONNECTEMPTY */

  always_comb begin
    ep_lq_mem_rsp_s.rdata = lq_mem_rsp_rdata;
    ep_lq_mem_rsp_s.fault = lq_mem_rsp_fault;
  end

  always_comb begin
    ep_sq_mem_rsp_s.rdata = sq_mem_rsp_rdata;
    ep_sq_mem_rsp_s.fault = sq_mem_rsp_fault;
  end

  // The memory requests the endpoints present, flattened for the driver.
  assign lq_mem_req_we    = ep_lq_mem_req_s.we;
  assign lq_mem_req_addr  = ep_lq_mem_req_s.addr;
  assign lq_mem_req_size  = {{(32 - TB_SIZE_W) {1'b0}}, ep_lq_mem_req_s.size};
  assign lq_mem_req_wstrb = {{(32 - TB_STRB_W) {1'b0}}, ep_lq_mem_req_s.wstrb};
  assign lq_mem_req_wdata = ep_lq_mem_req_s.wdata;

  assign sq_mem_req_we    = ep_sq_mem_req_s.we;
  assign sq_mem_req_addr  = ep_sq_mem_req_s.addr;
  assign sq_mem_req_size  = {{(32 - TB_SIZE_W) {1'b0}}, ep_sq_mem_req_s.size};
  assign sq_mem_req_wstrb = {{(32 - TB_STRB_W) {1'b0}}, ep_sq_mem_req_s.wstrb};
  assign sq_mem_req_wdata = ep_sq_mem_req_s.wdata;

  // ------------------------------------------------------- the query, exported
  // The store queue's own answer to the load queue's drive, so the driver can
  // check it against the rule independently of whether the load queue used it.
  assign o_sq_query_addr  = lq_query_addr;
  assign o_sq_query_size  = {{(32 - TB_SIZE_W) {1'b0}}, lq_query_size};
  assign o_sq_fwd_valid   = sq_fwd_valid_s;
  assign o_sq_fwd_data    = sq_fwd_data_s;
  assign o_sq_fwd_blocked = sq_fwd_blocked_s;

  // ------------------------------------------------------------ geometry
  assign o_dut_lq_entries  = 32'(u_lq.ENTRIES);
  assign o_dut_lq_entry_w  = 32'(u_lq.ENTRY_W);
  assign o_dut_lq_cnt_w    = 32'(u_lq.CNT_W);
  assign o_dut_lq_idx_w    = 32'(u_lq.IDX_W);
  assign o_dut_sq_entries  = 32'(u_sq.ENTRIES);
  assign o_dut_sq_entry_w  = 32'(u_sq.ENTRY_W);
  assign o_dut_sq_cnt_w    = 32'(u_sq.CNT_W);
  assign o_dut_xlen        = 32'(u_lq.XLEN);
  assign o_dut_id_w        = 32'(u_lq.ID_W);
  assign o_dut_rob_index_w = 32'(mosaic_uop_pkg::ROB_INDEX_W);
  assign o_dut_rob_gen_w   = 32'(mosaic_uop_pkg::ROB_GEN_W);

  assign o_tb_lq_entries  = 32'(TB_LQ_ENTRIES);
  assign o_tb_lq_entry_w  = 32'(TB_LQ_ENTRY_W);
  assign o_tb_lq_cnt_w    = 32'(TB_CNT_W);
  assign o_tb_lq_idx_w    = 32'(TB_IDX_W);
  assign o_tb_sq_entries  = 32'(TB_SQ_ENTRIES);
  assign o_tb_sq_entry_w  = 32'(TB_SQ_ENTRY_W);
  assign o_tb_sq_cnt_w    = 32'(TB_SQ_CNT_W);
  assign o_tb_xlen        = 32'(TB_XLEN);
  assign o_tb_id_w        = 32'(TB_ID_W);
  assign o_tb_rob_index_w = 32'(mosaic_uop_pkg::ROB_INDEX_W);
  assign o_tb_rob_gen_w   = 32'(mosaic_uop_pkg::ROB_GEN_W);

endmodule : mosaic_load_queue_tb

`resetall
`default_nettype wire
