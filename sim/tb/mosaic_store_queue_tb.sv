// ============================================================================
// mosaic_store_queue_tb -- simulation wrapper for
// CASE=store.wrong_path_visibility (work package I-034).
//
// Simulation-only glue. It contains no behaviour of its own: every port is
// either driven by sim/unit/tb_store.cpp or wired straight out of the DUTs.
// There is no clock generation, no reset generation and no `$display` in here,
// per sim/common/sim_common.h -- the C++ side owns all three.
//
// ------------------------------------------------------ the two modules wired
//
// The case is a *joint* statement about the store queue and the memory
// endpoint, so this wrapper instantiates both and connects them the way the
// core will:
//
//     mosaic_store_queue.drain_req_o  ->  mosaic_lsu_endpoint.req_i
//     mosaic_lsu_endpoint.rsp_o       ->  mosaic_store_queue.drain_rsp_i
//
// The endpoint is the memory side and the authority on "non-faulting": it traps
// a misaligned store before the memory sees it and reports an access fault from
// the memory response. Putting it in the wrapper rather than writing a second
// memory interface here is what makes the wrong-path check mean what it says --
// the queue cannot reach memory except through the endpoint, so "zero memory
// transactions" is a statement about the real path.
//
// The queue's own drain port is exported flat *and* left observable in the same
// cycle, so the driver can compare what the queue offered against what it
// should have offered, independently of whether the endpoint took it.
//
// ---------------------------------------------------- why the ports are flat
//
// The DUT's packet ports are packed structs (`mosaic_uop_pkg::lsu_req_t`,
// `lsu_rsp_t`, `uop_id_t`). A Verilator C++ driver can only poke those as raw
// bit vectors, so it would have to reproduce their layout -- a second
// transcription of rtl/core/mosaic_uop_pkg.sv, which is exactly the duplication
// that file exists to prevent. So this wrapper takes flat driver-facing ports
// and assembles/flattens the packets field by field. No assignment patterns
// (`'{...}`): Yosys cannot parse them.
//
// `mosaic_uop_pkg` is *used* here (its types) but deliberately not included:
// it is compiled earlier in the same command, which is how the endpoint's own
// wrapper reads it too. `mosaic_cfg_pkg.svh` is included because the geometry
// constants it names must be the generated ones.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per knob for the whole project;
// this wrapper names the LSU subset and the rest are unused here.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_ENTRIES     = mosaic_cfg_pkg::MOSAIC_SQ_ENTRIES;
localparam int unsigned TB_XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned TB_ROB_INDEX_W = mosaic_uop_pkg::ROB_INDEX_W;
localparam int unsigned TB_ROB_GEN_W   = mosaic_uop_pkg::ROB_GEN_W;
localparam int unsigned TB_ID_W        = $bits(mosaic_uop_pkg::uop_id_t);
localparam int unsigned TB_CNT_W       = $clog2(TB_ENTRIES + 1);
localparam int unsigned TB_IDX_W       = (TB_ENTRIES <= 1) ? 1 : $clog2(TB_ENTRIES);
localparam int unsigned TB_STRB_W      = TB_XLEN / 8;
localparam int unsigned TB_SIZE_W      = 3;
// The observation word, re-expressed from the same field order the RTL header
// documents: data_valid, addr_valid, data, size, imm, base, id.
localparam int unsigned TB_ENTRY_W     = 5 + 3 * TB_XLEN + TB_ID_W;

/* verilator lint_off UNUSEDSIGNAL */
// The driver-facing ports are fixed 32-bit vectors, so the C++ driver carries
// no packet geometry, while the packet fields they are narrowed to are a few
// bits wide. The unused upper bits are the price of that convention and are
// deliberate, not an omission; the widths they are narrowed to are re-exported
// in the geometry block below, and the driver checks them.
module mosaic_store_queue_tb (
    input  logic                        clk,
    input  logic                        rst,

    // ------------------------------------------------------------- allocation
    input  logic                        alloc_valid,
    output logic                        alloc_ready,
    input  logic [31:0]                 alloc_id,
    input  logic [63:0]                 alloc_base,
    input  logic [63:0]                 alloc_imm,
    input  logic [31:0]                 alloc_size,
    input  logic                        alloc_addr_valid,
    input  logic [63:0]                 alloc_data,
    input  logic                        alloc_data_valid,

    // ---------------------------------------------------------- late operand
    input  logic                        fill_valid,
    input  logic [31:0]                 fill_id,
    input  logic [63:0]                 fill_base,
    input  logic [63:0]                 fill_imm,
    input  logic                        fill_addr_valid,
    input  logic [63:0]                 fill_data,
    input  logic                        fill_data_valid,
    output logic                        fill_hit,
    output logic                        fill_stale,

    // ------------------------------------------------------ commit / squash
    input  logic                        commit_valid,
    input  logic [31:0]                 commit_id,
    output logic                        commit_ok,
    output logic                        commit_stale,
    input  logic                        squash_valid,
    input  logic                        squash_all,
    input  logic [31:0]                 squash_from_index,
    input  logic [31:0]                 squash_tail_index,
    input  logic [31:0]                 squash_gen,

    // ------------------------------------------------------- queue drain port
    output logic                        drain_req_valid,
    output logic                        drain_req_ready,
    output logic [31:0]                 drain_req_id,
    output logic [63:0]                 drain_req_base,
    output logic [63:0]                 drain_req_imm,
    output logic [31:0]                 drain_req_size,
    output logic [63:0]                 drain_req_data,

    // ----------------------------------------------- endpoint memory interface
    output logic                        mem_req_valid,
    input  logic                        mem_req_ready,
    output logic                        mem_req_we,
    output logic [63:0]                 mem_req_addr,
    output logic [31:0]                 mem_req_size,
    output logic [31:0]                 mem_req_wstrb,
    output logic [63:0]                 mem_req_wdata,
    input  logic                        mem_rsp_valid,
    output logic                        mem_rsp_ready,
    input  logic [63:0]                 mem_rsp_rdata,
    input  logic                        mem_rsp_fault,

    // --------------------------------------------------- forwarding query
    input  logic [63:0]                 fwd_query_addr,
    input  logic [31:0]                 fwd_query_size,
    output logic                        fwd_valid,
    output logic [63:0]                 fwd_data,
    output logic                        fwd_blocked,

    // --------------------------------------------------------------- status
    output logic [TB_CNT_W-1:0]         o_count,
    output logic [TB_CNT_W-1:0]         o_auth_cnt,
    output logic [TB_ENTRIES-1:0]       o_occ,
    output logic [TB_ENTRIES-1:0]       o_entry_authorised,
    output logic [31:0]                 o_alloc_ctr,
    output logic [31:0]                 o_fill_ctr,
    output logic [31:0]                 o_fill_stale_ctr,
    output logic [31:0]                 o_commit_ctr,
    output logic [31:0]                 o_commit_stale_ctr,
    output logic [31:0]                 o_drain_ctr,
    output logic [31:0]                 o_fault_ctr,
    output logic [31:0]                 o_squash_ctr,
    output logic [31:0]                 o_squash_spared_ctr,
    output logic [63:0]                 o_last_fault_tval,
    output logic [63:0]                 o_last_fault_cause,
    output logic [31:0]                 o_last_fault_id,
    output logic [TB_ENTRIES*TB_ENTRY_W-1:0] o_entry_pay,

    // ------------------------------------------------ endpoint observability
    output logic [31:0]                 o_ep_txn_ctr,
    output logic [31:0]                 o_ep_store_ctr,
    output logic [31:0]                 o_ep_misaligned_ctr,
    output logic [31:0]                 o_ep_rsp_ctr,
    output logic                        o_ep_busy,

    // ------------------------------------------------------------ geometry
    // Read out of the elaborated instances, so the driver sizes its shadow from
    // the hardware that exists. The `o_tb_*` set is this wrapper's own
    // re-expression; the driver requires the two to agree field by field, which
    // is what makes a profile change a build failure rather than a silently
    // narrowed port.
    output logic [31:0]                 o_dut_entries,
    output logic [31:0]                 o_dut_entry_w,
    output logic [31:0]                 o_dut_cnt_w,
    output logic [31:0]                 o_dut_idx_w,
    output logic [31:0]                 o_dut_xlen,
    output logic [31:0]                 o_dut_id_w,
    output logic [31:0]                 o_dut_rob_index_w,
    output logic [31:0]                 o_dut_rob_gen_w,
    output logic [31:0]                 o_tb_entries,
    output logic [31:0]                 o_tb_entry_w,
    output logic [31:0]                 o_tb_cnt_w,
    output logic [31:0]                 o_tb_idx_w,
    output logic [31:0]                 o_tb_xlen,
    output logic [31:0]                 o_tb_id_w,
    output logic [31:0]                 o_tb_rob_index_w,
    output logic [31:0]                 o_tb_rob_gen_w
);
/* verilator lint_on UNUSEDSIGNAL */

  mosaic_uop_pkg::lsu_req_t sq_drain_req_s;
  mosaic_uop_pkg::lsu_rsp_t sq_drain_rsp_s;
  mosaic_uop_pkg::lsu_rsp_t ep_rsp_s;
  mosaic_uop_pkg::mem_req_t ep_mem_req_s;
  mosaic_uop_pkg::mem_rsp_t ep_mem_rsp_s;

  logic ep_req_ready;
  logic ep_rsp_valid;
  logic sq_drain_rsp_ready;
  logic [TB_ID_W-1:0] last_fault_id_s;

  // The queue's drain request, flattened for the driver. It is driven *by* the
  // DUT and read *by* the endpoint, so the driver sees exactly the packet the
  // memory side saw, in the same cycle.
  assign drain_req_id   = {{(32 - TB_ID_W) {1'b0}}, sq_drain_req_s.id};
  assign drain_req_base = sq_drain_req_s.base;
  assign drain_req_imm  = sq_drain_req_s.imm;
  assign drain_req_size = {{(32 - TB_SIZE_W) {1'b0}}, sq_drain_req_s.size};
  assign drain_req_data = sq_drain_req_s.store_data;

  // The endpoint's response, flattened back to the queue.
  always_comb begin
    sq_drain_rsp_s = ep_rsp_s;
  end

  // The memory response the driver presents.
  always_comb begin
    ep_mem_rsp_s.rdata = mem_rsp_rdata;
    ep_mem_rsp_s.fault = mem_rsp_fault;
  end

  mosaic_store_queue u_sq (
      .clk                  (clk),
      .rst                  (rst),

      .alloc_valid_i        (alloc_valid),
      .alloc_ready_o        (alloc_ready),
      .alloc_id_i           (alloc_id[TB_ID_W-1:0]),
      .alloc_base_i         (alloc_base),
      .alloc_imm_i          (alloc_imm),
      .alloc_size_i         (alloc_size[TB_SIZE_W-1:0]),
      .alloc_addr_valid_i   (alloc_addr_valid),
      .alloc_data_i         (alloc_data),
      .alloc_data_valid_i   (alloc_data_valid),

      .fill_valid_i         (fill_valid),
      .fill_id_i            (fill_id[TB_ID_W-1:0]),
      .fill_base_i          (fill_base),
      .fill_imm_i           (fill_imm),
      .fill_addr_valid_i    (fill_addr_valid),
      .fill_data_i          (fill_data),
      .fill_data_valid_i    (fill_data_valid),
      .fill_hit_o           (fill_hit),
      .fill_stale_o         (fill_stale),

      .commit_valid_i       (commit_valid),
      .commit_id_i          (commit_id[TB_ID_W-1:0]),
      .commit_ok_o          (commit_ok),
      .commit_stale_o       (commit_stale),
      // The second authorisation port exists for the core, whose ROB retires two
      // instructions per cycle. This case drives one commit at a time, so a
      // second is held idle; the two-at-once rule is exercised through
      // CASE=core.mem_program, which retires consecutive stores two per cycle.
      .commit2_valid_i      (1'b0),
      .commit2_id_i         ({TB_ID_W{1'b0}}),
      .commit2_ok_o         (),
      .commit2_stale_o      (),

      .squash_valid_i       (squash_valid),
      .squash_all_i         (squash_all),
      .squash_from_index_i  (squash_from_index[TB_ROB_INDEX_W-1:0]),
      .squash_tail_index_i  (squash_tail_index[TB_ROB_INDEX_W-1:0]),
      .squash_gen_i         (squash_gen[TB_ROB_GEN_W-1:0]),

      .drain_req_valid_o    (drain_req_valid),
      .drain_req_ready_i    (ep_req_ready),
      .drain_req_o          (sq_drain_req_s),
      .drain_rsp_valid_i    (ep_rsp_valid),
      .drain_rsp_ready_o    (sq_drain_rsp_ready),
      .drain_rsp_i          (sq_drain_rsp_s),

      .fwd_query_addr_i     (fwd_query_addr),
      .fwd_query_size_i     (fwd_query_size[TB_SIZE_W-1:0]),
      .fwd_valid_o          (fwd_valid),
      .fwd_data_o           (fwd_data),
      .fwd_blocked_o        (fwd_blocked),

      .o_count              (o_count),
      .o_auth_cnt           (o_auth_cnt),
      .o_occ                (o_occ),
      .o_entry_authorised   (o_entry_authorised),
      .o_alloc_ctr          (o_alloc_ctr),
      .o_fill_ctr           (o_fill_ctr),
      .o_fill_stale_ctr     (o_fill_stale_ctr),
      .o_commit_ctr         (o_commit_ctr),
      .o_commit_stale_ctr   (o_commit_stale_ctr),
      .o_drain_ctr          (o_drain_ctr),
      .o_fault_ctr          (o_fault_ctr),
      .o_squash_ctr         (o_squash_ctr),
      .o_squash_spared_ctr  (o_squash_spared_ctr),
      .o_last_fault_tval    (o_last_fault_tval),
      .o_last_fault_cause   (o_last_fault_cause),
      .o_last_fault_id      (last_fault_id_s),
      .o_entry_pay          (o_entry_pay)
  );

  // The memory side. `drain_req_ready` is the endpoint's readiness (it holds one
  // transaction at a time), and the response is consumed by the queue
  // unconditionally.
  //
  // The endpoint's load-side observations are deliberately left unconnected: a
  // store queue never issues a load, so `o_load_ctr` cannot move, and the fault
  // cause/tval on the *load* path are the endpoint's own case's business. The
  // store-side counters this case cares about (transactions, stores, misaligned
  // traps, responses, busy) are exported below.
  /* verilator lint_off PINCONNECTEMPTY */
  mosaic_lsu_endpoint u_ep (
      .clk                (clk),
      .rst                (rst),

      .req_valid_i        (drain_req_valid),
      .req_ready_o        (ep_req_ready),
      .req_i              (sq_drain_req_s),
      .req_tval_i         (sq_drain_req_s.base + sq_drain_req_s.imm),
      // RAM addresses only (I-038); see mosaic_load_queue_tb.
      .req_dev_i          (1'b0),
      // No PMP unit stands in front of this standalone endpoint, so every
      // access is allowed; the address the endpoint computes is not observed
      // here (the case drives `req_i` and reads the memory port).
      .pmp_deny_i         (1'b0),
      .o_req_addr_o       (),

      .rsp_valid_o        (ep_rsp_valid),
      .rsp_ready_o        (sq_drain_rsp_ready),
      .rsp_o              (ep_rsp_s),

      .mem_req_valid_o    (mem_req_valid),
      .mem_req_ready_i    (mem_req_ready),
      .mem_req_o          (ep_mem_req_s),

      .mem_rsp_valid_i    (mem_rsp_valid),
      .mem_rsp_ready_o    (mem_rsp_ready),
      .mem_rsp_i          (ep_mem_rsp_s),

      // I-040: this case drives ordinary stores only; no second agent writes and
      // nothing is flushed, so the reservation stays clear.
      .ext_write_valid_i  (1'b0),
      .ext_write_addr_i   ({TB_XLEN{1'b0}}),
      .ext_write_bytes_i  (4'd0),
      .flush_i            (1'b0),

      .o_busy             (o_ep_busy),
      .o_load_ctr         (),
      .o_store_ctr        (o_ep_store_ctr),
      .o_txn_ctr          (o_ep_txn_ctr),
      .o_misaligned_ctr   (o_ep_misaligned_ctr),
      .o_access_fault_ctr (),
      .o_rsp_ctr          (o_ep_rsp_ctr),
      .o_last_fault_cause (),
      .o_last_fault_tval  (),
      .o_inflight_addr    (),
      .o_inflight_size    (),
      // The transaction identity and device attribute (I-038) are observed by
      // CASE=mmio.exactly_once on the integrated core; a store queue never
      // classifies an access here (the attribute is constant zero in this case).
      .o_txn_id           (),
      .o_txn_dev          (),
      // I-040's LR/SC observations; observed by CASE=lrsc.reservation_progress
      // on the integrated core, constant here.
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

  assign drain_req_ready = ep_req_ready;
  assign o_last_fault_id = {{(32 - TB_ID_W) {1'b0}}, last_fault_id_s};

  // The memory request the endpoint presents, flattened for the driver.
  assign mem_req_we    = ep_mem_req_s.we;
  assign mem_req_addr  = ep_mem_req_s.addr;
  assign mem_req_size  = {{(32 - TB_SIZE_W) {1'b0}}, ep_mem_req_s.size};
  assign mem_req_wstrb = {{(32 - TB_STRB_W) {1'b0}}, ep_mem_req_s.wstrb};
  assign mem_req_wdata = ep_mem_req_s.wdata;

  // Geometry read back from the instances, and the wrapper's own re-expression.
  assign o_dut_entries     = 32'(u_sq.ENTRIES);
  assign o_dut_entry_w     = 32'(u_sq.ENTRY_W);
  assign o_dut_cnt_w       = 32'(u_sq.CNT_W);
  assign o_dut_idx_w       = 32'(u_sq.IDX_W);
  assign o_dut_xlen        = 32'(u_sq.XLEN);
  assign o_dut_id_w        = 32'(u_sq.ID_W);
  assign o_dut_rob_index_w = 32'(u_sq.ROB_INDEX_W);
  assign o_dut_rob_gen_w   = 32'(u_sq.ROB_GEN_W);

  assign o_tb_entries     = 32'(TB_ENTRIES);
  assign o_tb_entry_w     = 32'(TB_ENTRY_W);
  assign o_tb_cnt_w       = 32'(TB_CNT_W);
  assign o_tb_idx_w       = 32'(TB_IDX_W);
  assign o_tb_xlen        = 32'(TB_XLEN);
  assign o_tb_id_w        = 32'(TB_ID_W);
  assign o_tb_rob_index_w = 32'(TB_ROB_INDEX_W);
  assign o_tb_rob_gen_w   = 32'(TB_ROB_GEN_W);

endmodule : mosaic_store_queue_tb

`resetall
`default_nettype wire
