// ============================================================================
// mosaic_cache_line_bridge -- the line/word width adapter between an L1 cache
// and the core's memory service.
//
// The cache (I-042, rtl/core/mosaic_cache.sv) holds whole *lines*: its memory
// port carries LINE_BITS of data in one beat. The core's memory service carries
// at most one doubleword (`mosaic_uop_pkg::mem_req_t`, size <= SZ_DBL). This
// module is the only place those two widths meet. It is deliberately small and
// has no policy of its own:
//
//   * a line **read** is one request, and it is turned into
//     LINE_BYTES/8 doubleword reads issued in order; the beats are assembled in
//     arrival order and presented as one full line. A fault on any beat ends the
//     read immediately and is reported as a fault on the whole line -- the cache
//     then refuses to install the line, which is I-042's rule that a failed
//     refill must not mark a line valid.
//   * a line **write** (a dirty writeback) is one request, and it is turned into
//     LINE_BYTES/8 doubleword writes with all strobes set. A writeback has no
//     response on this port -- the cache's contract is that a writeback is
//     complete when accepted -- so the bridge merely refuses the next request
//     (`line_req_ready` low) until its beats are done, which is what keeps a
//     disjoint `imem`/`dmem` refill from overtaking a writeback in progress.
//
// The bridge accepts one line request at a time; `line_req_ready` is high only
// in its idle state. That is the same "blocking, one outstanding" trade the
// cache makes, and it is all the cache needs: the cache itself holds the
// request (and the CPU) until the bridge takes it.
//
// The doubleword request is always `size = SZ_DBL` with the doubleword lanes
// aligned to a naturally aligned 8-byte address, which is what the endpoint's
// lane convention (the byte at `addr` is lane `addr[2:0]`) permits for a whole
// aligned doubleword. No claim is made about a *narrower* access here: the
// bridge only ever issues doublewords, and the cache's CPU-side sub-word
// handling is the cache's own.
// ============================================================================

`default_nettype none
`resetall

`ifndef MOSAIC_CACHE_LINE_BRIDGE_SV_
`define MOSAIC_CACHE_LINE_BRIDGE_SV_

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
`include "mosaic_pkg.sv"
`include "mosaic_uop_pkg.sv"

module mosaic_cache_line_bridge #(
  parameter int LINE_BYTES = 32,   // must be a multiple of 8 (the beat width)
  parameter int ADDR_WIDTH = 64,
  localparam int LINE_BITS = LINE_BYTES * 8,
  localparam int BEATS     = LINE_BYTES / 8
) (
  input  logic                            clk,
  input  logic                            rst,

  // --------------------------------------------------- toward the cache (line)
  input  logic                            line_req_valid,
  output logic                            line_req_ready,
  input  logic                            line_req_we,
  input  logic [ADDR_WIDTH-1:0]           line_req_addr,
  input  logic [LINE_BITS-1:0]            line_req_wdata,
  output logic                            line_rsp_valid,   // one cycle, reads only
  output logic [LINE_BITS-1:0]            line_rsp_rdata,
  output logic                            line_rsp_fault,

  // --------------------------------------- toward memory (doubleword, mem_req_t)
  output logic                            mem_req_valid,
  input  logic                            mem_req_ready,
  output mosaic_uop_pkg::mem_req_t        mem_req,
  input  logic                            mem_rsp_valid,
  input  mosaic_uop_pkg::mem_rsp_t        mem_rsp,

  // The bridge is mid-line when this is high (including between a writeback's
  // beats, before the next line request is taken).
  output logic                            o_busy
);

  localparam int CNT_W = (BEATS <= 1) ? 1 : $clog2(BEATS);

  typedef enum logic [2:0] {
    ST_IDLE  = 3'd0,
    ST_RD    = 3'd1,   // offering one doubleword read
    ST_RD_W  = 3'd2,   // waiting for it
    ST_WR    = 3'd3,   // offering one doubleword write
    ST_WR_W  = 3'd4,   // waiting for its acknowledgement
    ST_RESP  = 3'd5    // one cycle presenting the assembled line
  } state_e;

  state_e state;
  logic [ADDR_WIDTH-1:0] addr_r;
  logic [LINE_BITS-1:0]  wdata_r;
  logic [LINE_BITS-1:0]  line_acc;
  logic [CNT_W-1:0]      beat;
  logic                  fault_r;

  assign line_req_ready = (state == ST_IDLE);
  assign o_busy         = (state != ST_IDLE);

  assign line_rsp_valid = (state == ST_RESP);
  assign line_rsp_rdata = line_acc;
  assign line_rsp_fault = fault_r;

  assign mem_req_valid = (state == ST_RD) || (state == ST_WR);
  always_comb begin
    mem_req.we     = (state == ST_WR);
    mem_req.addr   = addr_r + ADDR_WIDTH'(beat) * ADDR_WIDTH'(8);
    mem_req.size   = mosaic_pkg::SZ_DBL;
    mem_req.wstrb  = (state == ST_WR) ? 8'hff : 8'h00;
    mem_req.wdata  = (state == ST_WR) ? wdata_r[beat*64 +: 64] : 64'h0;
    // The bridge is never atomic: it carries line traffic, not an instruction's
    // access. The fields are driven so the packet is never half-assigned.
    mem_req.amo    = 1'b0;
    mem_req.amo_op = mosaic_pkg::AMO_ADD;
    mem_req.aq     = 1'b0;
    mem_req.rl     = 1'b0;
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      state    <= ST_IDLE;
      addr_r   <= '0;
      wdata_r  <= '0;
      line_acc <= '0;
      beat     <= '0;
      fault_r  <= 1'b0;
    end else begin
      case (state)
        ST_IDLE: begin
          if (line_req_valid) begin
            addr_r  <= line_req_addr;
            wdata_r <= line_req_wdata;
            beat    <= '0;
            fault_r <= 1'b0;
            state   <= line_req_we ? ST_WR : ST_RD;
          end
        end

        ST_RD: begin
          if (mem_req_ready) state <= ST_RD_W;
        end

        ST_RD_W: begin
          if (mem_rsp_valid) begin
            if (mem_rsp.fault) begin
              // A faulted beat abandons the whole line: the cache must not
              // install it, and the data assembled so far is meaningless.
              fault_r <= 1'b1;
              state   <= ST_RESP;
            end else begin
              line_acc[beat*64 +: 64] <= mem_rsp.rdata;
              if (beat == CNT_W'(BEATS - 1)) begin
                state <= ST_RESP;
              end else begin
                beat  <= beat + 1'b1;
                state <= ST_RD;
              end
            end
          end
        end

        ST_WR: begin
          if (mem_req_ready) state <= ST_WR_W;
        end

        ST_WR_W: begin
          if (mem_rsp_valid) begin
            if (beat == CNT_W'(BEATS - 1)) begin
              // A writeback owes its consumer no response; the cache treats
              // acceptance as completion. Returning to IDLE releases the next
              // line request, which is what orders a following refill after the
              // bytes it is replacing.
              state <= ST_IDLE;
            end else begin
              beat  <= beat + 1'b1;
              state <= ST_WR;
            end
          end
        end

        ST_RESP: begin
          state <= ST_IDLE;
        end

        default: state <= ST_IDLE;
      endcase
    end
  end

endmodule

`endif  // MOSAIC_CACHE_LINE_BRIDGE_SV_
`resetall
