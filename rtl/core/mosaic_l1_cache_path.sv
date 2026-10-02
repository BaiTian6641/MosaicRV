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
  parameter int  ADDR_WIDTH   = 64,
  parameter int  CPU_DATA_WIDTH = 64,
  parameter int  ID_W         = 1,
  parameter int  EPOCH_W      = 1,
  localparam int INDEX_BITS   = $clog2(SETS)
) (
  input  logic                          clk,
  input  logic                          rst,

  // ------------------------------------------------------------- controls
  input  logic                          en_i,        // runtime cache enable
  input  logic                          flush_i,     // level: invalidate and hold off
  output logic                          flush_done,  // level: flush complete

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
  logic                    bridge_line_req_ready;
  logic                    bridge_line_rsp_valid;
  logic [LINE_BITS-1:0]    bridge_line_rsp_rdata;
  logic                    bridge_line_rsp_fault;
  logic                    bridge_mem_req_valid;
  logic                    bridge_mem_busy;
  mosaic_uop_pkg::mem_req_t bridge_mem_req;
  logic                    bridge_mem_resp_valid;

  /* verilator lint_off PINCONNECTEMPTY */
  mosaic_cache #(
    .CPU_DATA_WIDTH (CPU_DATA_WIDTH),
    .LINE_BYTES     (LINE_BYTES),
    .SETS           (SETS),
    .ADDR_WIDTH     (ADDR_WIDTH),
    .READ_ONLY      (IS_FETCH)
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
    .dbg_index      (dbg_index_i),
    .dbg_valid      (dbg_valid_o),
    .dbg_dirty      (dbg_dirty_o),
    .dbg_tag        (),
    .dbg_data       (),
    .ev_hit         (o_hit),
    .ev_miss        (o_miss),
    .ev_refill      (o_refill),
    .ev_writeback   (o_writeback),
    .ev_fault       (o_fault)
  );
  /* verilator lint_on PINCONNECTEMPTY */

  mosaic_cache_line_bridge #(
    .LINE_BYTES (LINE_BYTES),
    .ADDR_WIDTH (ADDR_WIDTH)
  ) u_bridge (
    .clk             (clk),
    .rst             (rst),
    .line_req_valid  (cache_mem_req_valid),
    .line_req_ready  (bridge_line_req_ready),
    .line_req_we     (cache_mem_req_we),
    .line_req_addr   (cache_mem_req_addr),
    .line_req_wdata  (cache_mem_req_wdata),
    .line_rsp_valid  (bridge_line_rsp_valid),
    .line_rsp_rdata  (bridge_line_rsp_rdata),
    .line_rsp_fault  (bridge_line_rsp_fault),
    .mem_req_valid   (bridge_mem_req_valid),
    .mem_req_ready   (mem_req_ready_i && !bypass_offer_c),
    .mem_req         (bridge_mem_req),
    .mem_rsp_valid   (bridge_mem_resp_valid),
    .mem_rsp         (mem_rsp_i),
    .o_busy          (bridge_mem_busy)
  );

  assign cache_mem_req_ready  = bridge_line_req_ready;
  assign cache_mem_resp_valid = bridge_line_rsp_valid;
  assign cache_mem_resp_rdata = bridge_line_rsp_rdata;
  assign cache_mem_resp_fault = bridge_line_rsp_fault;
  assign cache_flush_valid    = (state == S_FLUSH) && !flush_sent;

  // ------------------------------------------------------- classification
  assign cacheable_c = en_i && mosaic_cfg_pkg::mosaic_pa_cacheable(cpu_req_i.addr) &&
                       !cpu_req_i.amo;
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
    end else begin
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
