// ============================================================================
// mosaic_l1_cache_path -- the integration wrapper that puts one L1 cache
// (I-042, rtl/core/mosaic_cache.sv) *in the access path*, behind the platform
// map's cacheability, with the width adapter (mosaic_cache_line_bridge) between
// its line port and the core's doubleword memory service.
//
// One module serves both sides of the split cache; `IS_FETCH` selects the
// instruction behaviour. It is instantiated twice in mosaic_core: between fetch
// and the instruction port, and between the LSU endpoint and the data port.
//
// -------------------------------------------------------------- the rules
//
// R1  **Cacheability is the platform map's, not a guess.** A request is sent to
//     the cache only when `en_i` is high AND the address is cacheable per
//     `mosaic_cfg_pkg::mosaic_pa_cacheable` (generated from the region's own
//     `cacheable` flag). Every other address -- MMIO, the boot ROM, an unmapped
//     address, and any atomic (whose read-modify-write cannot be split across a
//     cache) -- is *bypassed*: it is presented to the memory service unchanged,
//     with its own size and strobes, and its response is returned unchanged. That
//     is what keeps a device access a device access: it is never read as part of
//     a line and never installed.
//
// R2  **A bypass preserves the transaction, not just the data.** The bypass
//     request is the caller's exactly (`we`, `addr`, `size`, `wstrb`, `wdata`,
//     `amo`), so the memory service sees the same access it would have seen with
//     the cache absent. A device store therefore has exactly one side effect.
//
// R3  **The instruction side shifts, the data side does not.** The cache's CPU
//     port is a word port aligned to the doubleword; a fetch asks for four bytes
//     at a possibly 2-aligned PC. On the fetch side the returned doubleword is
//     shifted down by the byte offset and the instruction length is read from the
//     encoding (11 is four bytes, anything else two) -- the same rule
//     mosaic_fetch states. On the data side the doubleword is lane-aligned to the
//     address already, so it is passed through (the endpoint does the extraction).
//
// R4  **A flush is a level request with a level acknowledgement.** `flush_i` is
//     held high by the core for the whole window in which the instruction side
//     must be invalid, and the wrapper refuses CPU requests for that whole
//     window (`flush_done` is its acknowledgement). The wrapper performs the
//     cache flush once, when it is idle, so a refill already in flight completes
//     (and is then thrown away) instead of being installed after the flush.
//     `flush_done` rises only when the flush *and* the width adapter are idle, so
//     a writeback's bytes have reached the memory service before the core treats
//     the flush as complete -- the ordering the self-modifying-code case needs.
//     "The adapter is idle" is load-bearing and not decoration: a flush of a
//     dirty line is issued as *beats* through the memory service, each of which
//     is acknowledged, so the wrapper must keep accepting memory responses for
//     the whole of S_FLUSH and must not leave S_FLUSH until the adapter has
//     drained. Leaving when the cache's own flush completes -- the cache treats
//     acceptance of a writeback as completion, the adapter does not -- stops
//     accepting the acknowledgements of beats already in flight, and the fence
//     deadlocks with the front end held off forever. That was a real defect here,
//     caught by this case's self-modifying-code phase.
//
// R5  **Disabled means absent.** With `en_i` low the module is a wire: request,
//     response, and identity pass straight through with no added state. A profile
//     or a run with the cache off therefore has the exact timing it had before
//     this module existed.
// ============================================================================

`default_nettype none
`resetall

`ifndef MOSAIC_L1_CACHE_PATH_SV_
`define MOSAIC_L1_CACHE_PATH_SV_

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
`include "mosaic_pkg.sv"
`include "mosaic_uop_pkg.sv"

module mosaic_l1_cache_path #(
  parameter bit  IS_FETCH     = 1'b0,
  parameter int  LINE_BYTES   = 32,
  parameter int  SETS         = 8,
  parameter int  WAYS         = 1,
  parameter int  MSHR_ENTRIES = 4,
  parameter int  ADDR_WIDTH   = 64,
  parameter int  CPU_DATA_WIDTH = 64,
  parameter int  ID_W         = 1,
  parameter int  EPOCH_W      = 1,
  localparam int INDEX_BITS   = $clog2(SETS),
  localparam int WAY_BITS     = (WAYS > 1) ? $clog2(WAYS) : 1
) (
  input  logic                          clk,
  input  logic                          rst,

  // ------------------------------------------------------------- controls
  input  logic                          en_i,        // runtime cache enable
  input  logic                          flush_i,     // level: invalidate and hold off
  output logic                          flush_done,  // level: flush complete
  // A bypassed atomic to a cacheable address: drop the cached line (writing a
  // dirty one back first). Only the data side acts on it; the instruction cache
  // is read-only and is flushed by FENCE.I.
  input  logic                          inv_valid_i,
  input  logic [ADDR_WIDTH-1:0]         inv_pa_i,

  // ----------------------------------------------------------- CPU request
  input  logic                          cpu_req_valid_i,
  output logic                          cpu_req_ready_o,
  input  mosaic_uop_pkg::mem_req_t      cpu_req_i,
  input  logic [ID_W-1:0]               cpu_req_id_i,
  input  logic [EPOCH_W-1:0]            cpu_req_epoch_i,

  // ---------------------------------------------------------- CPU response
  output logic                          cpu_rsp_valid_o,
  input  logic                          cpu_rsp_ready_i,
  output mosaic_uop_pkg::mem_rsp_t      cpu_rsp_o,
  output logic [ID_W-1:0]               cpu_rsp_id_o,
  output logic [EPOCH_W-1:0]            cpu_rsp_epoch_o,
  output logic [2:0]                    cpu_rsp_len_o,

  // -------------------------------------------------------- memory service
  output logic                          mem_req_valid_o,
  input  logic                          mem_req_ready_i,
  output mosaic_uop_pkg::mem_req_t      mem_req_o,
  output logic [ID_W-1:0]               mem_req_id_o,
  output logic [EPOCH_W-1:0]            mem_req_epoch_o,
  input  logic                          mem_rsp_valid_i,
  output logic                          mem_rsp_ready_o,
  input  mosaic_uop_pkg::mem_rsp_t      mem_rsp_i,
  input  logic [ID_W-1:0]               mem_rsp_id_i,
  input  logic [EPOCH_W-1:0]            mem_rsp_epoch_i,
  input  logic [2:0]                    mem_rsp_len_i,

  // ------------------------------------------------------------ evidence
  output logic                          o_hit,
  output logic                          o_miss,
  output logic                          o_refill,
  output logic                          o_writeback,
  output logic                          o_fault,
  output logic [31:0]                   o_cpu_txn,
  output logic [31:0]                   o_mem_beat,
  output logic [31:0]                   o_line_txn,
  output logic [31:0]                   o_bypass_txn,

  // --------------------------------------------------- the locality path (I-060)
  // The clean-copy locality buffer and the bounded prefetcher sit on the L1
  // *line* port when this side is the data side. They are runtime-switched and
  // default off, so a caller that leaves these ports at their defaults has the
  // machine it had before. See mosaic_locality_path.sv for the position, the
  // eight invalidation sources and the prefetcher's gate.
  input  logic                          loc_llb_en_i,
  input  logic                          loc_pf_en_i,
  input  logic [1:0]                    loc_pf_conf_thresh_i,
  // The in-flight access's context: the virtual page of the effective address
  // (the LLB's scoped-fence match and the prefetcher's alias label), the ASID
  // the translation ran under, and the access's control identity (PC) — the
  // prefetcher's stride table is keyed by it.
  input  logic [26:0]                   loc_vpn_i,
  input  logic [15:0]                   loc_asid_i,
  input  logic [63:0]                   loc_pc_i,
  // The invalidation sources driven from the core.
  input  logic                          loc_inv_store_valid_i,
  input  logic [63:0]                   loc_inv_store_pa_i,
  input  logic                          loc_inv_snoop_valid_i,
  input  logic [63:0]                   loc_inv_snoop_pa_i,
  input  logic                          loc_inv_snoop_all_i,
  input  logic                          loc_fence_valid_i,
  input  logic [1:0]                    loc_fence_kind_i,
  input  logic [26:0]                   loc_fence_vpn_i,
  input  logic                          loc_fence_has_vpn_i,
  input  logic [15:0]                   loc_fence_asid_i,
  input  logic                          loc_fence_has_asid_i,
  input  logic                          loc_ctx_flush_valid_i,
  // What the locality path did, so a case can require the structures to be
  // *used* rather than assume a switch implies an effect.
  output logic [31:0]                   o_loc_llb_hit,
  output logic [31:0]                   o_loc_llb_miss,
  output logic [31:0]                   o_loc_llb_bypass,
  output logic [31:0]                   o_loc_llb_fill,
  output logic [31:0]                   o_loc_llb_fill_refused,
  output logic [31:0]                   o_loc_llb_inv,
  output logic [31:0]                   o_loc_mem_line,
  output logic [31:0]                   o_loc_pf_issued,
  output logic [31:0]                   o_loc_pf_useful,
  output logic [31:0]                   o_loc_pf_useless,
  output logic [31:0]                   o_loc_pf_late,
  output logic [31:0]                   o_loc_pf_cancelled,
  output logic [31:0]                   o_loc_pf_admitted,
  output logic [31:0]                   o_loc_pf_fill,
  output logic [31:0]                   o_loc_pf_fill_refused,
  output logic [31:0]                   o_loc_pf_mem,

  // --------------------------------------------------------- state probe
  input  logic [INDEX_BITS-1:0]         dbg_index_i,
  output logic                          dbg_valid_o,
  output logic                          dbg_dirty_o
);

  localparam int LINE_BITS = LINE_BYTES * 8;

  // --------------------------------------------------------------- storage
  typedef enum logic [1:0] {
    S_IDLE  = 2'd0,
    S_CWAIT = 2'd1,   // a cache transaction is in flight
    S_BWAIT = 2'd2,   // a bypassed transaction is in flight
    S_FLUSH = 2'd3
  } state_e;

  state_e state;
  logic   flush_sent;
  logic   flush_ack_r;
  logic [2:0]            lat_off;
  logic [ID_W-1:0]       lat_id;
  logic [EPOCH_W-1:0]    lat_epoch;
  logic                  hold_valid;
  logic [CPU_DATA_WIDTH-1:0] hold_rdata;
  logic                  hold_fault;
  logic [2:0]            hold_len;
  logic [ID_W-1:0]       hold_id;
  logic [EPOCH_W-1:0]    hold_epoch;
  logic [31:0]           cpu_txn_r;
  logic [31:0]           mem_beat_r;
  logic [31:0]           line_txn_r;
  logic [31:0]           bypass_txn_r;

  // The request classification, declared before the cache instance because the
  // cache's port connections read part of it.
  logic cacheable_c;
  logic bypass_offer_c;
  logic cache_offer_c;
  logic req_block_c;
  logic flush_pending_c;

  // ------------------------------------------------------- cache and bridge
  logic                    cache_cpu_req_valid;
  logic                    cache_cpu_req_ready;
  logic                    cache_cpu_resp_valid;
  logic [CPU_DATA_WIDTH-1:0] cache_cpu_resp_rdata;
  logic                    cache_cpu_resp_fault;
  logic                    cache_mem_req_valid;
  logic                    cache_mem_req_we;
  logic [ADDR_WIDTH-1:0]   cache_mem_req_addr;
  logic [LINE_BITS-1:0]    cache_mem_req_wdata;
  logic                    cache_mem_req_ready;
  logic                    cache_mem_resp_valid;
  logic [LINE_BITS-1:0]    cache_mem_resp_rdata;
  logic                    cache_mem_resp_fault;
  logic                    cache_flush_valid;
  logic                    cache_flush_ready;
  logic                    cache_flush_done;
  logic                    bridge_mem_req_valid;
  logic                    bridge_mem_busy;
  mosaic_uop_pkg::mem_req_t bridge_mem_req;
  logic                    bridge_mem_resp_valid;

  // The cache's line port is the locality path's port; the locality path's
  // memory-side port is the bridge's. `loc_*` names the segment between them.
  logic                    loc_line_req_valid;
  logic                    loc_line_req_ready;
  logic                    loc_line_req_we;
  logic [ADDR_WIDTH-1:0]   loc_line_req_addr;
  logic [LINE_BITS-1:0]    loc_line_req_wdata;
  logic                    loc_line_rsp_valid;
  logic [LINE_BITS-1:0]    loc_line_rsp_rdata;
  logic                    loc_line_rsp_fault;

  // The context of the CPU access the cache is serving, latched when the cache
  // (or the bypass path) accepts it. The line request a miss produces comes
  // several cycles later, so the context cannot be read from the live request.
  logic                    cpu_accept_c;
  logic                    lat_is_load_q;
  logic [26:0]             lat_vpn_q;
  logic [15:0]             lat_asid_q;
  logic [3:0]              lat_perms_q;
  logic [63:0]             lat_pc_q;

  /* verilator lint_off PINCONNECTEMPTY */
  generate
    if (IS_FETCH) begin : g_icache
      // The L1I is I-043's verified non-blocking read L1 (`mosaic_mshr`) itself.
      // It keeps an outstanding-miss table of MSHR_ENTRIES entries, coalesces a
      // second request to a line already being fetched, and matches a refill to
      // its entry by the line address rather than by arrival order. The line
      // adapter this wrapper hands it is single-outstanding, so the read in
      // flight is the one last issued; its address is latched and replayed as
      // the response identity (which is what the MSHR matches on).
      logic                      mshr_req_ready;
      logic                      mshr_rsp_valid;
      logic [CPU_DATA_WIDTH-1:0] mshr_rsp_rdata;
      logic                      mshr_rsp_fault;
      logic                      mshr_mem_req_valid;
      logic [ADDR_WIDTH-1:0]     mshr_mem_req_addr;
      logic                      mshr_mem_req_ready;
      logic [ADDR_WIDTH-1:0]     mshr_rd_addr_q;
      logic                      mshr_flush_ready;
      logic                      mshr_flush_done;

      assign cache_cpu_req_ready  = mshr_req_ready;
      assign cache_cpu_resp_valid = mshr_rsp_valid;
      assign cache_cpu_resp_rdata = mshr_rsp_rdata;
      assign cache_cpu_resp_fault = mshr_rsp_fault;

      assign cache_mem_req_valid  = mshr_mem_req_valid;
      assign cache_mem_req_we     = 1'b0;
      assign cache_mem_req_addr   = mshr_mem_req_addr;
      assign cache_mem_req_wdata  = {LINE_BITS{1'b0}};
      assign mshr_mem_req_ready   = cache_mem_req_ready;

      always_ff @(posedge clk) begin
        if (mshr_mem_req_valid && mshr_mem_req_ready) mshr_rd_addr_q <= mshr_mem_req_addr;
      end

      assign cache_flush_ready    = mshr_flush_ready;
      assign cache_flush_done     = mshr_flush_done;
      // The instruction cache is read-only: a bypassed atomic cannot touch it,
      // and its own invalidate is the FENCE.I flush.
      /* verilator lint_off UNUSEDSIGNAL */
      logic unused_inv;
      assign unused_inv = inv_valid_i ^ (^inv_pa_i);
      /* verilator lint_on UNUSEDSIGNAL */
      // A read-only cache never writes back, and an instruction cache has no
      // store path: nothing drives these here.
      assign o_writeback          = 1'b0;
      assign dbg_dirty_o          = 1'b0;

      mosaic_mshr #(
        .CPU_DATA_WIDTH (CPU_DATA_WIDTH),
        .LINE_BYTES     (LINE_BYTES),
        .SETS           (SETS),
        .WAYS           (WAYS),
        .ADDR_WIDTH     (ADDR_WIDTH),
        .MSHR_ENTRIES   (MSHR_ENTRIES),
        .ID_WIDTH       (ID_W)
      ) u_mshr (
        .clk            (clk),
        .rst            (rst),
        .req_valid      (cache_cpu_req_valid),
        .req_addr       (cpu_req_i.addr),
        .req_id         (cpu_req_id_i),
        .req_ready      (mshr_req_ready),
        .cancel_valid   (1'b0),
        .cancel_id      ({ID_W{1'b0}}),
        .resp_valid     (mshr_rsp_valid),
        .resp_id        (),
        .resp_rdata     (mshr_rsp_rdata),
        .resp_fault     (mshr_rsp_fault),
        .mem_req_valid  (mshr_mem_req_valid),
        .mem_req_addr   (mshr_mem_req_addr),
        .mem_req_ready  (mshr_mem_req_ready),
        .mem_resp_valid (cache_mem_resp_valid),
        .mem_resp_addr  (mshr_rd_addr_q),
        .mem_resp_rdata (cache_mem_resp_rdata),
        .mem_resp_fault (cache_mem_resp_fault),
        .flush_valid    (cache_flush_valid),
        .flush_ready    (mshr_flush_ready),
        .flush_done     (mshr_flush_done),
        .flush_busy     (),
        .dbg_index      (dbg_index_i),
        .dbg_way        ({WAY_BITS{1'b0}}),
        .dbg_valid      (dbg_valid_o),
        .dbg_tag        (),
        .dbg_data       (),
        .dbg_outstanding(),
        .dbg_waiters    (),
        .ev_hit         (o_hit),
        .ev_miss        (o_miss),
        .ev_coalesce    (),
        .ev_refill      (o_refill),
        .ev_fault       (o_fault),
        .ev_cancel      (),
        .ev_drop        ()
      );
    end else begin : g_dcache
      mosaic_cache #(
        .CPU_DATA_WIDTH (CPU_DATA_WIDTH),
        .LINE_BYTES     (LINE_BYTES),
        .SETS           (SETS),
        .WAYS           (WAYS),
        .ADDR_WIDTH     (ADDR_WIDTH),
        .READ_ONLY      (1'b0),
        .MSHR_ENTRIES   (MSHR_ENTRIES)
      ) u_cache (
        .clk            (clk),
        .rst            (rst),
        .cpu_req_valid  (cache_cpu_req_valid),
        .cpu_req_we     (cpu_req_i.we),
        .cpu_req_addr   (cpu_req_i.addr),
        .cpu_req_wdata  (cpu_req_i.wdata),
        .cpu_req_wmask  (cpu_req_i.wstrb),
        .cpu_req_ready  (cache_cpu_req_ready),
        .cpu_resp_valid (cache_cpu_resp_valid),
        .cpu_resp_rdata (cache_cpu_resp_rdata),
        .cpu_resp_fault (cache_cpu_resp_fault),
        .mem_req_valid  (cache_mem_req_valid),
        .mem_req_we     (cache_mem_req_we),
        .mem_req_addr   (cache_mem_req_addr),
        .mem_req_wdata  (cache_mem_req_wdata),
        .mem_req_ready  (cache_mem_req_ready),
        .mem_resp_valid (cache_mem_resp_valid),
        .mem_resp_rdata (cache_mem_resp_rdata),
        .mem_resp_fault (cache_mem_resp_fault),
        .flush_valid    (cache_flush_valid),
        .flush_ready    (cache_flush_ready),
        .flush_done     (cache_flush_done),
        .flush_busy     (),
        .inv_valid      (inv_valid_i),
        .inv_addr       (inv_pa_i),
        .dbg_index      (dbg_index_i),
        .dbg_way        ({WAY_BITS{1'b0}}),
        .dbg_valid      (dbg_valid_o),
        .dbg_dirty      (dbg_dirty_o),
        .dbg_tag        (),
        .dbg_data       (),
        .ev_hit         (o_hit),
        .ev_miss        (o_miss),
        .ev_coalesce    (),
        .ev_refill      (o_refill),
        .ev_writeback   (o_writeback),
        .ev_fault       (o_fault),
        .o_outstanding  ()
      );
    end
  endgenerate
  /* verilator lint_on PINCONNECTEMPTY */

  mosaic_cache_line_bridge #(
    .LINE_BYTES (LINE_BYTES),
    .ADDR_WIDTH (ADDR_WIDTH)
  ) u_bridge (
    .clk             (clk),
    .rst             (rst),
    .line_req_valid  (loc_line_req_valid),
    .line_req_ready  (loc_line_req_ready),
    .line_req_we     (loc_line_req_we),
    .line_req_addr   (loc_line_req_addr),
    .line_req_wdata  (loc_line_req_wdata),
    .line_rsp_valid  (loc_line_rsp_valid),
    .line_rsp_rdata  (loc_line_rsp_rdata),
    .line_rsp_fault  (loc_line_rsp_fault),
    .mem_req_valid   (bridge_mem_req_valid),
    .mem_req_ready   (mem_req_ready_i && !bypass_offer_c),
    .mem_req         (bridge_mem_req),
    .mem_rsp_valid   (bridge_mem_resp_valid),
    .mem_rsp         (mem_rsp_i),
    .o_busy          (bridge_mem_busy)
  );

  // ------------------------------------------------------------ the locality
  // The line port runs through mosaic_locality_path on *both* sides. On the
  // instruction side the two switches are tied low through `IS_FETCH`, a
  // constant, so the structures are inert there -- the fetch side is a straight
  // wire in time as well as in function, because with both enables low every
  // expression below reduces to the bridge's own handshake.
  mosaic_locality_path #(
    .LINE_BYTES (LINE_BYTES),
    .ADDR_WIDTH (ADDR_WIDTH)
  ) u_locality (
    .clk                         (clk),
    .rst                         (rst),
    .llb_en_i                    (loc_llb_en_i && en_i && !IS_FETCH),
    .pf_en_i                     (loc_pf_en_i && en_i && !IS_FETCH),
    .pf_conf_thresh_i            (loc_pf_conf_thresh_i),
    .cache_line_req_valid        (cache_mem_req_valid),
    .cache_line_req_ready        (cache_mem_req_ready),
    .cache_line_req_we           (cache_mem_req_we),
    .cache_line_req_addr         (cache_mem_req_addr),
    .cache_line_req_wdata        (cache_mem_req_wdata),
    .cache_line_rsp_valid        (cache_mem_resp_valid),
    .cache_line_rsp_rdata        (cache_mem_resp_rdata),
    .cache_line_rsp_fault        (cache_mem_resp_fault),
    .bridge_line_req_valid       (loc_line_req_valid),
    .bridge_line_req_ready       (loc_line_req_ready),
    .bridge_line_req_we          (loc_line_req_we),
    .bridge_line_req_addr        (loc_line_req_addr),
    .bridge_line_req_wdata       (loc_line_req_wdata),
    .bridge_line_rsp_valid       (loc_line_rsp_valid),
    .bridge_line_rsp_rdata       (loc_line_rsp_rdata),
    .bridge_line_rsp_fault       (loc_line_rsp_fault),
    .ctx_is_load_i               (lat_is_load_q),
    .ctx_vpn_i                   (lat_vpn_q),
    .ctx_asid_i                  (lat_asid_q),
    .ctx_perms_i                 (lat_perms_q),
    .ctx_pc_i                    (lat_pc_q),
    .inv_store_valid_i           (loc_inv_store_valid_i),
    .inv_store_pa_i              (loc_inv_store_pa_i),
    .inv_snoop_valid_i           (loc_inv_snoop_valid_i),
    .inv_snoop_pa_i              (loc_inv_snoop_pa_i),
    .inv_snoop_all_i             (loc_inv_snoop_all_i),
    .fence_valid_i               (loc_fence_valid_i),
    .fence_kind_i                (loc_fence_kind_i),
    .fence_vpn_i                 (loc_fence_vpn_i),
    .fence_has_vpn_i             (loc_fence_has_vpn_i),
    .fence_asid_i                (loc_fence_asid_i),
    .fence_has_asid_i            (loc_fence_has_asid_i),
    .ctx_flush_valid_i           (loc_ctx_flush_valid_i),
    .o_llb_hit_o                 (o_loc_llb_hit),
    .o_llb_miss_o                (o_loc_llb_miss),
    .o_llb_bypass_o              (o_loc_llb_bypass),
    .o_llb_fill_o                (o_loc_llb_fill),
    .o_llb_fill_refused_o        (o_loc_llb_fill_refused),
    .o_llb_inv_o                 (o_loc_llb_inv),
    .o_mem_line_o                (o_loc_mem_line),
    .o_pf_issued_o               (o_loc_pf_issued),
    .o_pf_useful_o               (o_loc_pf_useful),
    .o_pf_useless_o              (o_loc_pf_useless),
    .o_pf_late_o                 (o_loc_pf_late),
    .o_pf_cancelled_o            (o_loc_pf_cancelled),
    .o_pf_admitted_o             (o_loc_pf_admitted),
    .o_pf_fill_o                 (o_loc_pf_fill),
    .o_pf_fill_refused_o         (o_loc_pf_fill_refused),
    .o_pf_mem_o                  (o_loc_pf_mem)
  );

  assign cache_flush_valid    = (state == S_FLUSH) && !flush_sent;

  // The access the cache takes this cycle. It is the same conjunction the S_IDLE
  // branch uses -- the cache's own ready on the cacheable path, the memory
  // service's on the bypass path -- so the context latched here is the context
  // of the access whose line request follows.
  assign cpu_accept_c = (state == S_IDLE) && !flush_pending_c && cpu_req_valid_i &&
                        !hold_valid && !req_block_c &&
                        (cacheable_c ? cache_cpu_req_ready : mem_req_ready_i);

  // ------------------------------------------------------- classification
`ifdef MOSAIC_CACHE_MUTANT_DEVICE_CACHED
  // NEGATIVE CONTROL: the platform map's cacheability rule is ignored and every
  // non-atomic access is treated as cacheable, so a UART access is read as part
  // of a 32-byte line and a device register is installed in the cache. The case
  // names it twice: the device accesses no longer reach memory once each, and a
  // device address appears on the memory side as a line refill.
  assign cacheable_c = en_i && !cpu_req_i.amo;
`else
  assign cacheable_c = en_i && mosaic_cfg_pkg::mosaic_pa_cacheable(cpu_req_i.addr) &&
                       !cpu_req_i.amo;
`endif
  assign req_block_c = flush_i;
  assign flush_pending_c = flush_i && !flush_ack_r;

  assign bypass_offer_c = (state == S_IDLE) && !req_block_c && !hold_valid &&
                          cpu_req_valid_i && en_i && !cacheable_c;
  assign cache_offer_c  = (state == S_IDLE) && !req_block_c && !hold_valid &&
                          cpu_req_valid_i && en_i && cacheable_c;
  assign cache_cpu_req_valid = cache_offer_c;

  // --------------------------------------------------------- memory mux
  mosaic_uop_pkg::mem_req_t wrap_mem_req;
  logic wrap_mem_req_valid;

  assign wrap_mem_req_valid = bypass_offer_c ? 1'b1 : bridge_mem_req_valid;
  always_comb begin
    wrap_mem_req = bridge_mem_req;
    if (bypass_offer_c) wrap_mem_req = cpu_req_i;
  end

  // ------------------------------------------------------- capture and hold
  logic                  cap_cached;
  logic [CPU_DATA_WIDTH-1:0] cap_word;
  logic [31:0]           cap_shifted;
  logic [CPU_DATA_WIDTH-1:0] cap_rdata;
  logic                  cap_fault;
  logic [2:0]            cap_len;
  logic                  capture_c;

  assign cap_cached = (state == S_CWAIT);
  assign capture_c  = ((state == S_CWAIT) && cache_cpu_resp_valid) ||
                      ((state == S_BWAIT) && mem_rsp_valid_i);
  assign cap_word   = cap_cached ? cache_cpu_resp_rdata : mem_rsp_i.rdata;
  assign cap_shifted= 32'(cap_word >> (8 * 3'(lat_off)));
  assign cap_fault  = cap_cached ? cache_cpu_resp_fault : mem_rsp_i.fault;
  // On the data side the doubleword is already lane-aligned to the address; on
  // the instruction side the cache returns the doubleword containing the PC and
  // the caller needs the four-byte window at the PC's byte offset.
  assign cap_rdata  = IS_FETCH ? {32'b0, (cap_cached ? cap_shifted : cap_word[31:0])}
                               : cap_word;
  // The length is the instruction encoding's own (11 is four bytes). On the data
  // side nothing reads it, but it is still computed from the same expression so
  // the signal feeding it is never a dead net.
  assign cap_len    = cap_cached ? ((cap_shifted[1:0] == 2'b11) ? 3'd4 : 3'd2)
                                 : mem_rsp_len_i;

  // --------------------------------------------------------------- outputs
  assign cpu_req_ready_o = en_i
    ? ((state == S_IDLE) && !req_block_c && !hold_valid
       ? (cacheable_c ? cache_cpu_req_ready : mem_req_ready_i)
       : 1'b0)
    : mem_req_ready_i;

  assign mem_req_valid_o = en_i ? wrap_mem_req_valid : cpu_req_valid_i;
  assign mem_req_o       = en_i ? wrap_mem_req       : cpu_req_i;
  assign mem_req_id_o    = en_i ? (bypass_offer_c ? cpu_req_id_i : {ID_W{1'b0}})
                                : cpu_req_id_i;
  assign mem_req_epoch_o = en_i ? (bypass_offer_c ? cpu_req_epoch_i : {EPOCH_W{1'b0}})
                                : cpu_req_epoch_i;
  assign mem_rsp_ready_o = en_i ? ((state == S_CWAIT) || (state == S_BWAIT) ||
                                   (state == S_FLUSH))
                                : cpu_rsp_ready_i;

  assign cpu_rsp_valid_o = en_i ? hold_valid : mem_rsp_valid_i;
  assign cpu_rsp_id_o    = en_i ? hold_id    : mem_rsp_id_i;
  assign cpu_rsp_epoch_o = en_i ? hold_epoch : mem_rsp_epoch_i;
  assign cpu_rsp_len_o   = en_i ? hold_len   : mem_rsp_len_i;
  always_comb begin
    cpu_rsp_o = mem_rsp_i;
    if (en_i) begin
      cpu_rsp_o.rdata = hold_rdata;
      cpu_rsp_o.fault = hold_fault;
    end
  end

  // The bridge waits for the memory service's acknowledgement of every beat it
  // issues, writebacks included, so a flush's writeback beats must be answered
  // while the wrapper is in S_FLUSH. Without this the flush's first writeback
  // beat is never acknowledged, `o_busy` never falls, `flush_done` never rises,
  // and the core's FENCE.I micro-FSM holds the front end off forever -- a
  // deadlock, not a slow path. Gating this on S_CWAIT alone is exactly that bug.
  assign bridge_mem_resp_valid = mem_rsp_valid_i && ((state == S_CWAIT) ||
                                                     (state == S_FLUSH));
  assign flush_done = en_i ? (flush_ack_r && !bridge_mem_busy) : flush_i;

  assign o_cpu_txn    = cpu_txn_r;
  assign o_mem_beat   = mem_beat_r;
  assign o_line_txn   = line_txn_r;
  assign o_bypass_txn = bypass_txn_r;

  // --------------------------------------------------------------- state
  always_ff @(posedge clk) begin
    if (rst) begin
      state        <= S_IDLE;
      flush_sent   <= 1'b0;
      flush_ack_r  <= 1'b0;
      lat_off      <= '0;
      lat_id       <= '0;
      lat_epoch    <= '0;
      hold_valid   <= 1'b0;
      hold_rdata   <= '0;
      hold_fault   <= 1'b0;
      hold_len     <= 3'd0;
      hold_id      <= '0;
      hold_epoch   <= '0;
      cpu_txn_r    <= 32'd0;
      mem_beat_r   <= 32'd0;
      line_txn_r   <= 32'd0;
      bypass_txn_r <= 32'd0;
      lat_is_load_q <= 1'b0;
      lat_vpn_q     <= 27'd0;
      lat_asid_q    <= 16'd0;
      lat_perms_q   <= 4'd0;
      lat_pc_q      <= 64'd0;
    end else begin
      // The context of the access just taken. The permission class of the
      // integrated path is the access's own read/write class ({x,w,r,u} with
      // x and u zero and r always set): it is the class the endpoint publishes,
      // it is what a store can never reuse a load's copy under, and the LLB's
      // key format is unchanged by it.
      if (cpu_accept_c) begin
        lat_is_load_q <= !cpu_req_i.we && !cpu_req_i.amo;
        lat_vpn_q     <= loc_vpn_i;
        lat_asid_q    <= loc_asid_i;
        lat_perms_q   <= {1'b0, (cpu_req_i.we || cpu_req_i.amo), 1'b1, 1'b0};
        lat_pc_q      <= loc_pc_i;
      end
      if (!flush_i) begin
        flush_sent  <= 1'b0;
        flush_ack_r <= 1'b0;
      end else if (cache_flush_done) begin
        flush_ack_r <= 1'b1;
      end

      if (capture_c) begin
        hold_valid <= 1'b1;
        hold_rdata <= cap_rdata;
        hold_fault <= cap_fault;
        hold_len   <= cap_len;
        hold_id    <= lat_id;
        hold_epoch <= lat_epoch;
      end else if (hold_valid && cpu_rsp_ready_i) begin
        hold_valid <= 1'b0;
      end

      if (en_i && cpu_req_valid_i && cpu_req_ready_o) cpu_txn_r <= cpu_txn_r + 32'd1;
      if (en_i && mem_req_valid_o && mem_req_ready_i)  mem_beat_r <= mem_beat_r + 32'd1;
      if (en_i && cache_mem_req_valid && cache_mem_req_ready) line_txn_r <= line_txn_r + 32'd1;
      if (en_i && bypass_offer_c && mem_req_ready_i) bypass_txn_r <= bypass_txn_r + 32'd1;

      case (state)
        S_IDLE: begin
          if (flush_pending_c) begin
            state      <= S_FLUSH;
            flush_sent <= 1'b0;
          end else if (cpu_req_valid_i && !hold_valid && !req_block_c) begin
            // The classification is stable while `valid && !ready`, so the
            // branch taken here is the branch the boundary saw.
            if (cacheable_c) begin
              if (cache_cpu_req_ready) begin
                lat_off   <= cpu_req_i.addr[2:0];
                lat_id    <= cpu_req_id_i;
                lat_epoch <= cpu_req_epoch_i;
                state     <= S_CWAIT;
              end
            end else if (mem_req_ready_i) begin
              lat_off   <= cpu_req_i.addr[2:0];
              lat_id    <= cpu_req_id_i;
              lat_epoch <= cpu_req_epoch_i;
              state     <= S_BWAIT;
            end
          end
        end

        S_CWAIT: begin
          if (cache_cpu_resp_valid) state <= S_IDLE;
        end

        S_BWAIT: begin
          if (mem_rsp_valid_i) state <= S_IDLE;
        end

        S_FLUSH: begin
          if (!flush_sent) begin
            if (cache_flush_ready) flush_sent <= 1'b1;
          end else if (flush_ack_r && !bridge_mem_busy) begin
            // The cache's flush is done (latched in `flush_ack_r`), and the width
            // adapter has drained the writebacks it issued through the memory
            // service. Only then is the flush complete: leaving earlier would
            // stop accepting memory responses while a writeback beat is still
            // awaiting its acknowledgement, and the beat would hang.
            state <= S_IDLE;
          end
          if (!flush_i) state <= S_IDLE;
        end

        default: state <= S_IDLE;
      endcase
    end
  end

endmodule

`endif  // MOSAIC_L1_CACHE_PATH_SV_
`resetall
