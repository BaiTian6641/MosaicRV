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
  output logic         iev_refill,
  output logic         iev_writeback,
  output logic         iev_fault
);

  localparam int CPU_DATA_WIDTH = 64;
  localparam int LINE_BYTES     = 32;
  localparam int SETS           = 8;
  localparam int ADDR_WIDTH     = 32;
  localparam int LINE_BITS      = LINE_BYTES * 8;
  localparam int TAG_WIDTH      = ADDR_WIDTH - $clog2(LINE_BYTES) - $clog2(SETS);
  localparam int INDEX_BITS     = $clog2(SETS);

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
    .dbg_index (ddbg_index), .dbg_valid (ddbg_valid), .dbg_dirty (ddbg_dirty),
    .dbg_tag (ddbg_tag), .dbg_data (ddbg_data),
    .ev_hit (dev_hit), .ev_miss (dev_miss), .ev_refill (dev_refill),
    .ev_writeback (dev_writeback), .ev_fault (dev_fault)
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
    .dbg_index (idbg_index), .dbg_valid (idbg_valid), .dbg_dirty (idbg_dirty),
    .dbg_tag (idbg_tag), .dbg_data (idbg_data),
    .ev_hit (iev_hit), .ev_miss (iev_miss), .ev_refill (iev_refill),
    .ev_writeback (iev_writeback), .ev_fault (iev_fault)
  );

  // Elaboration guard against the driver's mirrored geometry.
  generate
    if ((CPU_DATA_WIDTH != 64) || (LINE_BYTES != 32) || (SETS != 8) ||
        (ADDR_WIDTH != 32) || (LINE_BITS != 256) || (TAG_WIDTH != 24) ||
        (INDEX_BITS != 3)) begin : g_geometry_mismatch
      mosaic_cache_tb_geometry_mismatch u_geometry ();
    end
  endgenerate

endmodule

`resetall
