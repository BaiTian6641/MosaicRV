// ============================================================================
// mosaic_cache_tb -- unit-test wrapper for the I-042 blocking L1 cache.
//
// Two independent cache instances, both I-042's `mosaic_cache`:
//
//   u_dcache  READ_ONLY=0  the data cache: loads, stores, write-back/write-allocate
//   u_icache  READ_ONLY=1  the instruction cache: reads only, a write faults
//
// The wrapper contains no behaviour of its own. Every port of both instances is
// a top-level port of this module, so `sim/unit/tb_cache.cpp` drives the caches
// directly and services their line ports from an independent memory model. The
// geometry here is mirrored by hard-coded constants in that driver; the generate
// guard below turns a disagreement into an elaboration error rather than into a
// test that silently checks the wrong number of sets.
// ============================================================================

// The V-062 case (CASE=cache.coalescer_boundaries) also drives the I-042
// access path `mosaic_l1_cache_path` from this wrapper, to show that MMIO and
// AMO requests are refused the cache. Its dependency files are included here
// under their own include guards, so an entry whose source list predates the
// instance still builds: a case that lists them separately is unaffected (the
// guard makes the include a no-op), and a case that does not gets them without
// a registry change. `mosaic_cache`/`mosaic_mshr` are not included because every
// entry that builds this wrapper already lists them (and they carry no guard).
`include "mosaic_llb.sv"
`include "mosaic_prefetch.sv"
`include "mosaic_cache_line_bridge.sv"
`include "mosaic_locality_path.sv"
`include "mosaic_l1_cache_path.sv"

`default_nettype none
`resetall

module mosaic_cache_tb (
  input  logic         clk,
  input  logic         rst,

  // ------------------------------------------------------- data cache (L1D)
  input  logic         dc_req_valid,
  input  logic         dc_req_we,
  input  logic [31:0]  dc_req_addr,
  input  logic [63:0]  dc_req_wdata,
  input  logic [7:0]   dc_req_wmask,
  output logic         dc_req_ready,
  output logic         dc_resp_valid,
  output logic [63:0]  dc_resp_rdata,
  output logic         dc_resp_fault,

  output logic         dm_req_valid,
  output logic         dm_req_we,
  output logic [31:0]  dm_req_addr,
  output logic [255:0] dm_req_wdata,
  input  logic         dm_req_ready,
  input  logic         dm_resp_valid,
  input  logic [255:0] dm_resp_rdata,
  input  logic         dm_resp_fault,

  input  logic         dfl_valid,
  output logic         dfl_ready,
  output logic         dfl_done,
  output logic         dfl_busy,

  input  logic [2:0]   ddbg_index,
  output logic         ddbg_valid,
  output logic         ddbg_dirty,
  output logic [23:0]  ddbg_tag,
  output logic [255:0] ddbg_data,

  output logic         dev_hit,
  output logic         dev_miss,
  output logic         dev_coalesce,
  output logic [2:0]   dev_outstanding,
  output logic         dev_refill,
  output logic         dev_writeback,
  output logic         dev_fault,

  // ------------------------------------------------ instruction cache (L1I)
  input  logic         ic_req_valid,
  input  logic         ic_req_we,
  input  logic [31:0]  ic_req_addr,
  input  logic [63:0]  ic_req_wdata,
  input  logic [7:0]   ic_req_wmask,
  output logic         ic_req_ready,
  output logic         ic_resp_valid,
  output logic [63:0]  ic_resp_rdata,
  output logic         ic_resp_fault,

  output logic         im_req_valid,
  output logic         im_req_we,
  output logic [31:0]  im_req_addr,
  output logic [255:0] im_req_wdata,
  input  logic         im_req_ready,
  input  logic         im_resp_valid,
  input  logic [255:0] im_resp_rdata,
  input  logic         im_resp_fault,

  input  logic         ifl_valid,
  output logic         ifl_ready,
  output logic         ifl_done,
  output logic         ifl_busy,

  input  logic [2:0]   idbg_index,
  output logic         idbg_valid,
  output logic         idbg_dirty,
  output logic [23:0]  idbg_tag,
  output logic [255:0] idbg_data,

  output logic         iev_hit,
  output logic         iev_miss,
  output logic         iev_coalesce,
  output logic [2:0]   iev_outstanding,
  output logic         iev_refill,
  output logic         iev_writeback,
  output logic         iev_fault,

  // ------------------------------------------- non-blocking read L1 (I-043)
  // `mosaic_mshr`: multiple outstanding misses, same-line coalescing and
  // cancellation. A separate instance with its own arrays and memory port, so
  // it cannot disturb the two blocking caches above.
  input  logic         nb_req_valid,
  input  logic [31:0]  nb_req_addr,
  input  logic [2:0]   nb_req_id,
  output logic         nb_req_ready,

  input  logic         nb_cancel_valid,
  input  logic [2:0]   nb_cancel_id,

  output logic         nb_resp_valid,
  output logic [2:0]   nb_resp_id,
  output logic [63:0]  nb_resp_rdata,
  output logic         nb_resp_fault,

  output logic         nb_mem_req_valid,
  output logic [31:0]  nb_mem_req_addr,
  input  logic         nb_mem_req_ready,
  input  logic         nb_mem_resp_valid,
  input  logic [31:0]  nb_mem_resp_addr,
  input  logic [255:0] nb_mem_resp_rdata,
  input  logic         nb_mem_resp_fault,

  input  logic [2:0]   nb_dbg_index,
  output logic         nb_dbg_valid,
  output logic [23:0]  nb_dbg_tag,
  output logic [255:0] nb_dbg_data,
  output logic [2:0]   nb_dbg_outstanding,
  output logic [5:0]   nb_dbg_waiters,

  output logic         nb_ev_hit,
  output logic         nb_ev_miss,
  output logic         nb_ev_coalesce,
  output logic         nb_ev_refill,
  output logic         nb_ev_fault,
  output logic         nb_ev_cancel,
  output logic         nb_ev_drop,

  // ------------------------------------------ L1 cache path (I-042, V-062)
  // The width-adapted L1 access path that carries the platform map's
  // cacheability rule and the atomic bypass. Only the V-062 bypass phase drives
  // it; every other campaign holds `lp_en_i` low, which makes the module a wire
  // and leaves this instance inert.
  input  logic         lp_en_i,
  input  logic         lp_flush_i,
  output logic         lp_flush_done_o,
  input  logic         lp_inv_valid_i,
  input  logic [63:0]  lp_inv_pa_i,
  input  logic         lp_cpu_req_valid_i,
  output logic         lp_cpu_req_ready_o,
  input  logic         lp_cpu_req_we_i,
  input  logic [63:0]  lp_cpu_req_addr_i,
  input  logic [2:0]   lp_cpu_req_size_i,
  input  logic [7:0]   lp_cpu_req_wstrb_i,
  input  logic [63:0]  lp_cpu_req_wdata_i,
  input  logic         lp_cpu_req_amo_i,
  output logic         lp_cpu_rsp_valid_o,
  output logic [63:0]  lp_cpu_rsp_rdata_o,
  output logic         lp_cpu_rsp_fault_o,
  output logic         lp_mem_req_valid_o,
  input  logic         lp_mem_req_ready_i,
  output logic         lp_mem_req_we_o,
  output logic [63:0]  lp_mem_req_addr_o,
  output logic [2:0]   lp_mem_req_size_o,
  output logic [7:0]   lp_mem_req_wstrb_o,
  output logic [63:0]  lp_mem_req_wdata_o,
  output logic         lp_mem_req_amo_o,
  input  logic         lp_mem_rsp_valid_i,
  output logic         lp_mem_rsp_ready_o,
  input  logic [63:0]  lp_mem_rsp_rdata_i,
  input  logic         lp_mem_rsp_fault_i,
  output logic         lp_hit_o,
  output logic         lp_miss_o,
  output logic         lp_refill_o,
  output logic         lp_writeback_o,
  output logic         lp_fault_o,
  output logic [31:0]  lp_cpu_txn_o,
  output logic [31:0]  lp_mem_beat_o,
  output logic [31:0]  lp_line_txn_o,
  output logic [31:0]  lp_bypass_txn_o
);

  localparam int CPU_DATA_WIDTH = 64;
  localparam int LINE_BYTES     = 32;
  localparam int SETS           = 8;
  localparam int ADDR_WIDTH     = 32;
  localparam int LINE_BITS      = LINE_BYTES * 8;
  localparam int TAG_WIDTH      = ADDR_WIDTH - $clog2(LINE_BYTES) - $clog2(SETS);
  localparam int INDEX_BITS     = $clog2(SETS);
  // The non-blocking instance's geometry, mirrored by the driver's constants.
  localparam int MSHR_ENTRIES   = 4;
  localparam int MSHR_ID_WIDTH  = 3;
  localparam int MSHR_OUT_BITS  = $clog2(MSHR_ENTRIES + 1);                        // 3
  localparam int MSHR_WT_BITS   = $clog2(MSHR_ENTRIES * (1 << MSHR_ID_WIDTH) + 1); // 6

  mosaic_cache #(
    .CPU_DATA_WIDTH (CPU_DATA_WIDTH),
    .LINE_BYTES     (LINE_BYTES),
    .SETS           (SETS),
    .ADDR_WIDTH     (ADDR_WIDTH),
    .READ_ONLY      (1'b0)
  ) u_dcache (
    .clk (clk), .rst (rst),
    .cpu_req_valid (dc_req_valid), .cpu_req_we (dc_req_we), .cpu_req_addr (dc_req_addr),
    .cpu_req_wdata (dc_req_wdata), .cpu_req_wmask (dc_req_wmask),
    .cpu_req_ready (dc_req_ready), .cpu_resp_valid (dc_resp_valid),
    .cpu_resp_rdata (dc_resp_rdata), .cpu_resp_fault (dc_resp_fault),
    .mem_req_valid (dm_req_valid), .mem_req_we (dm_req_we), .mem_req_addr (dm_req_addr),
    .mem_req_wdata (dm_req_wdata), .mem_req_ready (dm_req_ready),
    .mem_resp_valid (dm_resp_valid), .mem_resp_rdata (dm_resp_rdata),
    .mem_resp_fault (dm_resp_fault),
    .flush_valid (dfl_valid), .flush_ready (dfl_ready), .flush_done (dfl_done),
    .flush_busy (dfl_busy),
    .inv_valid (1'b0), .inv_addr (32'd0),
    .dbg_index (ddbg_index), .dbg_way (1'b0), .dbg_valid (ddbg_valid),
    .dbg_dirty (ddbg_dirty), .dbg_tag (ddbg_tag), .dbg_data (ddbg_data),
    .ev_hit (dev_hit), .ev_miss (dev_miss), .ev_coalesce (dev_coalesce),
    .ev_refill (dev_refill), .ev_writeback (dev_writeback), .ev_fault (dev_fault),
    .o_outstanding (dev_outstanding)
  );

  mosaic_cache #(
    .CPU_DATA_WIDTH (CPU_DATA_WIDTH),
    .LINE_BYTES     (LINE_BYTES),
    .SETS           (SETS),
    .ADDR_WIDTH     (ADDR_WIDTH),
    .READ_ONLY      (1'b1)
  ) u_icache (
    .clk (clk), .rst (rst),
    .cpu_req_valid (ic_req_valid), .cpu_req_we (ic_req_we), .cpu_req_addr (ic_req_addr),
    .cpu_req_wdata (ic_req_wdata), .cpu_req_wmask (ic_req_wmask),
    .cpu_req_ready (ic_req_ready), .cpu_resp_valid (ic_resp_valid),
    .cpu_resp_rdata (ic_resp_rdata), .cpu_resp_fault (ic_resp_fault),
    .mem_req_valid (im_req_valid), .mem_req_we (im_req_we), .mem_req_addr (im_req_addr),
    .mem_req_wdata (im_req_wdata), .mem_req_ready (im_req_ready),
    .mem_resp_valid (im_resp_valid), .mem_resp_rdata (im_resp_rdata),
    .mem_resp_fault (im_resp_fault),
    .flush_valid (ifl_valid), .flush_ready (ifl_ready), .flush_done (ifl_done),
    .flush_busy (ifl_busy),
    .inv_valid (1'b0), .inv_addr (32'd0),
    .dbg_index (idbg_index), .dbg_way (1'b0), .dbg_valid (idbg_valid),
    .dbg_dirty (idbg_dirty), .dbg_tag (idbg_tag), .dbg_data (idbg_data),
    .ev_hit (iev_hit), .ev_miss (iev_miss), .ev_coalesce (iev_coalesce),
    .ev_refill (iev_refill), .ev_writeback (iev_writeback), .ev_fault (iev_fault),
    .o_outstanding (iev_outstanding)
  );

  mosaic_mshr #(
    .CPU_DATA_WIDTH (CPU_DATA_WIDTH),
    .LINE_BYTES     (LINE_BYTES),
    .SETS           (SETS),
    .ADDR_WIDTH     (ADDR_WIDTH),
    .MSHR_ENTRIES   (MSHR_ENTRIES),
    .ID_WIDTH       (MSHR_ID_WIDTH)
  ) u_mshr (
    .clk (clk), .rst (rst),
    .req_valid (nb_req_valid), .req_addr (nb_req_addr), .req_id (nb_req_id),
    .req_ready (nb_req_ready),
    .cancel_valid (nb_cancel_valid), .cancel_id (nb_cancel_id),
    .resp_valid (nb_resp_valid), .resp_id (nb_resp_id),
    .resp_rdata (nb_resp_rdata), .resp_fault (nb_resp_fault),
    .mem_req_valid (nb_mem_req_valid), .mem_req_addr (nb_mem_req_addr),
    .mem_req_ready (nb_mem_req_ready),
    .mem_resp_valid (nb_mem_resp_valid), .mem_resp_addr (nb_mem_resp_addr),
    .mem_resp_rdata (nb_mem_resp_rdata), .mem_resp_fault (nb_mem_resp_fault),
    .flush_valid (1'b0), .flush_ready (), .flush_done (), .flush_busy (),
    .dbg_index (nb_dbg_index), .dbg_way (1'b0), .dbg_valid (nb_dbg_valid),
    .dbg_tag (nb_dbg_tag),
    .dbg_data (nb_dbg_data), .dbg_outstanding (nb_dbg_outstanding),
    .dbg_waiters (nb_dbg_waiters),
    .ev_hit (nb_ev_hit), .ev_miss (nb_ev_miss), .ev_coalesce (nb_ev_coalesce),
    .ev_refill (nb_ev_refill), .ev_fault (nb_ev_fault), .ev_cancel (nb_ev_cancel),
    .ev_drop (nb_ev_drop)
  );

  // ------------------------------------------------ L1 access path (V-062)
  // The I-042 integration wrapper, driven only by the V-062 bypass phase. With
  // `lp_en_i` low it is a wire, so it cannot disturb the three instances above.
  mosaic_uop_pkg::mem_req_t lp_cpu_req;
  mosaic_uop_pkg::mem_rsp_t lp_cpu_rsp;
  mosaic_uop_pkg::mem_req_t lp_mem_req;
  mosaic_uop_pkg::mem_rsp_t lp_mem_rsp;
  always_comb begin
    lp_cpu_req.we     = lp_cpu_req_we_i;
    lp_cpu_req.addr   = lp_cpu_req_addr_i;
    lp_cpu_req.size   = lp_cpu_req_size_i;
    lp_cpu_req.wstrb  = lp_cpu_req_wstrb_i;
    lp_cpu_req.wdata  = lp_cpu_req_wdata_i;
    lp_cpu_req.amo    = lp_cpu_req_amo_i;
    lp_cpu_req.amo_op = mosaic_pkg::AMO_ADD;
    lp_cpu_req.aq     = 1'b0;
    lp_cpu_req.rl     = 1'b0;
  end
  assign lp_cpu_rsp_rdata_o = lp_cpu_rsp.rdata;
  assign lp_cpu_rsp_fault_o = lp_cpu_rsp.fault;
  assign lp_mem_req_we_o    = lp_mem_req.we;
  assign lp_mem_req_addr_o  = lp_mem_req.addr;
  assign lp_mem_req_size_o  = lp_mem_req.size;
  assign lp_mem_req_wstrb_o = lp_mem_req.wstrb;
  assign lp_mem_req_wdata_o = lp_mem_req.wdata;
  assign lp_mem_req_amo_o   = lp_mem_req.amo;
  assign lp_mem_rsp.rdata   = lp_mem_rsp_rdata_i;
  assign lp_mem_rsp.fault   = lp_mem_rsp_fault_i;

  mosaic_l1_cache_path #(
    .IS_FETCH       (1'b0),
    .LINE_BYTES     (32),
    .SETS           (8),
    .ADDR_WIDTH     (64),
    .CPU_DATA_WIDTH (64),
    .ID_W           (1),
    .EPOCH_W        (1)
  ) u_l1_path (
    .clk (clk), .rst (rst),
    .en_i (lp_en_i), .flush_i (lp_flush_i), .flush_done (lp_flush_done_o),
    .inv_valid_i (lp_inv_valid_i), .inv_pa_i (lp_inv_pa_i),
    .cpu_req_valid_i (lp_cpu_req_valid_i), .cpu_req_ready_o (lp_cpu_req_ready_o),
    .cpu_req_i (lp_cpu_req), .cpu_req_id_i (1'b0), .cpu_req_epoch_i (1'b0),
    .cpu_rsp_valid_o (lp_cpu_rsp_valid_o), .cpu_rsp_ready_i (1'b1),
    .cpu_rsp_o (lp_cpu_rsp), .cpu_rsp_id_o (), .cpu_rsp_epoch_o (), .cpu_rsp_len_o (),
    .mem_req_valid_o (lp_mem_req_valid_o), .mem_req_ready_i (lp_mem_req_ready_i),
    .mem_req_o (lp_mem_req), .mem_req_id_o (), .mem_req_epoch_o (),
    .mem_rsp_valid_i (lp_mem_rsp_valid_i), .mem_rsp_ready_o (lp_mem_rsp_ready_o),
    .mem_rsp_i (lp_mem_rsp), .mem_rsp_id_i (1'b0), .mem_rsp_epoch_i (1'b0),
    .mem_rsp_len_i (3'd0),
    .o_hit (lp_hit_o), .o_miss (lp_miss_o), .o_refill (lp_refill_o),
    .o_writeback (lp_writeback_o), .o_fault (lp_fault_o),
    .o_cpu_txn (lp_cpu_txn_o), .o_mem_beat (lp_mem_beat_o),
    .o_line_txn (lp_line_txn_o), .o_bypass_txn (lp_bypass_txn_o),
    .loc_llb_en_i (1'b0), .loc_pf_en_i (1'b0), .loc_pf_conf_thresh_i (2'd0),
    .loc_vpn_i (27'd0), .loc_asid_i (16'd0), .loc_pc_i (64'd0),
    .loc_inv_store_valid_i (1'b0), .loc_inv_store_pa_i (64'd0),
    .loc_inv_snoop_valid_i (1'b0), .loc_inv_snoop_pa_i (64'd0),
    .loc_inv_snoop_all_i (1'b0),
    .loc_fence_valid_i (1'b0), .loc_fence_kind_i (2'd0), .loc_fence_vpn_i (27'd0),
    .loc_fence_has_vpn_i (1'b0), .loc_fence_asid_i (16'd0),
    .loc_fence_has_asid_i (1'b0), .loc_ctx_flush_valid_i (1'b0),
    .o_loc_llb_hit (), .o_loc_llb_miss (), .o_loc_llb_bypass (),
    .o_loc_llb_fill (), .o_loc_llb_fill_refused (), .o_loc_llb_inv (),
    .o_loc_mem_line (),
    .o_loc_pf_issued (), .o_loc_pf_useful (), .o_loc_pf_useless (),
    .o_loc_pf_late (), .o_loc_pf_cancelled (), .o_loc_pf_admitted (),
    .o_loc_pf_fill (), .o_loc_pf_fill_refused (), .o_loc_pf_mem (),
    .dbg_index_i (3'd0), .dbg_valid_o (), .dbg_dirty_o ()
  );

  // Elaboration guard against the driver's mirrored geometry.
  generate
    if ((CPU_DATA_WIDTH != 64) || (LINE_BYTES != 32) || (SETS != 8) ||
        (ADDR_WIDTH != 32) || (LINE_BITS != 256) || (TAG_WIDTH != 24) ||
        (INDEX_BITS != 3) || (MSHR_ENTRIES != 4) || (MSHR_ID_WIDTH != 3) ||
        (MSHR_OUT_BITS != 3) || (MSHR_WT_BITS != 6)) begin : g_geometry_mismatch
      mosaic_cache_tb_geometry_mismatch u_geometry ();
    end
  endgenerate

endmodule

`resetall
