// ============================================================================
// mosaic_cache -- a small blocking direct-mapped L1 (work package I-042)
//
// One CPU-side request port, one memory-side line port, one flush port. The
// cache is *blocking*: it accepts exactly one CPU request at a time and
// completes it (hit, or miss + refill, or miss + dirty writeback + refill)
// before it can accept the next one. There is no MSHR, no outstanding-miss
// table and no reordering -- that is work package I-043's job, and the card's
// design trade-off is explicitly "small blocking cache first, correctness
// before throughput".
//
// The same module is both the L1I and the L1D. `READ_ONLY` selects which:
//
//   * READ_ONLY = 0  (L1D)  stores are legal and the write policy below applies;
//   * READ_ONLY = 1  (L1I)  there is no store path through an instruction cache
//                           in the p0 core, and a write request is answered with
//                           a fault and changes nothing.
//
// ------------------------------------------------------------- write policy
//
// *** WRITE-BACK, WRITE-ALLOCATE. *** This is a rule, not an emergent
// behaviour, and every clause of it is exercised by CASE=cache.refill_evict_fault:
//
//   1. A store that hits its line merges the byte lanes selected by `wmask`
//      into the line and marks the line dirty. It does NOT go to memory.
//   2. A store that misses allocates: the line is refilled from memory first
//      (write-allocate), then the store bytes are merged and the line is
//      marked dirty.
//   3. A dirty line is written back to memory exactly when it is evicted by a
//      refill that needs its way, or by an explicit flush. A clean line is
//      simply dropped.
//   4. A read-only cache never marks a line dirty; its flush only drops tags.
//
// The alternative, write-through, would make stores visible in memory
// immediately and would need no writeback, but it would also put every store
// on the (slow, ordered) memory port. Write-back is chosen here because the
// card asks for a *stated* policy and because write-back is the policy the
// dirty-eviction and self-modify cases are built to falsify.
//
// ------------------------------------------------- valid bits vs. data RAM
//
// `rst` clears exactly `valid`, `dirty`, the control registers and the trace
// pulses. It does NOT touch `data_mem` or `tag_mem`. Validity is tracked
// outside the arrays, so a `SETS x (LINE_BYTES*8)` reset -- which would turn an
// inferrable block RAM into `SETS x LINE_BYTES x 8` reset flops -- is neither
// needed nor present. This is the same reset contract as the RAM wrapper
// (I-006, rtl/common/mosaic_ram.sv), and it is asserted by the testbench: after
// a reset every line must read back invalid, and the contents of `data_mem`
// are never observed until a refill has installed the line.
//
// -------------------------------------------------------- refill error handling
//
// The memory port can fail a refill (`mem_resp_fault`). On a failed refill the
// cache MUST NOT mark the line valid and MUST NOT answer the CPU request with
// data: it raises `cpu_resp_fault` for that request, leaves the tag/valid/data
// untouched, and pulses `ev_fault`. A subsequent request to the same line
// therefore misses again and tries the refill afresh. A cache that installed a
// faulted line, or that answered a faulted request with data, would invent a
// value that the architectural reference never produced; that is the card's
// first named Fail mode and the `MOSAIC_CACHE_MUTANT_FAULT_VALID` control.
//
// -------------------------------------------------------------- traceability
//
// Refill and eviction are visible as one-cycle event pulses, so an
// architectural trace can show *why* a load returned what it did:
//
//   ev_hit        a CPU request completed out of the cache;
//   ev_miss       a CPU request did not find its line;
//   ev_refill     a clean line was fetched from memory;
//   ev_writeback  a dirty line was written back to memory on eviction/flush;
//   ev_fault      a refill faulted and the request was answered with a fault.
//
// A line is only ever installed (`ev_refill`) after a *successful* refill, and
// `ev_writeback` only ever carries the line the cache held. Both invariants are
// checked by the testbench, not merely documented.
//
// ------------------------------------------------------------- memory port
//
// One request/response channel for whole lines:
//
//   * request: `mem_req_valid` / `mem_req_we` / `mem_req_addr` (line aligned) /
//     `mem_req_wdata`; accepted on an edge where `mem_req_valid && mem_req_ready`.
//     A writeback is complete when accepted. A read's data arrives later as
//     `mem_resp_valid` with `mem_resp_rdata` / `mem_resp_fault`, never in the
//     same cycle the request was accepted (the read latency is at least one
//     cycle, exactly like mosaic_ram's).
//   * `mem_req_ready` may be low for an unbounded number of cycles; the FSM
//     holds the request until it is accepted and holds the CPU in the meantime.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_CACHE_MUTANT_* selects a deliberately broken variant used to prove
// the unit test detects the corresponding bug. The shipping build defines none
// of them. See results/reports/I-042-cache.md for the control table.
// ============================================================================

`default_nettype none
`resetall

module mosaic_cache #(
  parameter int  CPU_DATA_WIDTH = 64,     // bits per CPU access (a word)
  parameter int  LINE_BYTES     = 32,     // bytes per cache line, power of two
  parameter int  SETS           = 8,      // number of sets (direct-mapped)
  parameter int  ADDR_WIDTH     = 32,     // physical address width
  parameter bit  READ_ONLY      = 1'b0,   // 1 = instruction cache
  // Derived, and therefore not overridable.
  localparam int CPU_BYTES   = CPU_DATA_WIDTH / 8,
  localparam int LINE_BITS   = LINE_BYTES * 8,
  localparam int OFFSET_BITS = $clog2(LINE_BYTES),
  localparam int INDEX_BITS  = $clog2(SETS),
  localparam int TAG_WIDTH   = ADDR_WIDTH - OFFSET_BITS - INDEX_BITS,
  localparam int WORD_BITS   = $clog2(CPU_BYTES)
) (
  input  logic                        clk,
  input  logic                        rst,

  // ------------------------------------------------------------ CPU request
  // `cpu_req_addr` is a byte address, but this port is word-granular: the byte
  // lane is carried by `cpu_req_wmask`, and the cache stores whole words. The
  // low log2(CPU_BYTES) address bits are therefore deliberately not decoded --
  // an aligned access has them zero, and a misaligned one is the LSU's fault,
  // not the cache's. Verilator's "unused bits" warning is disabled for exactly
  // that one line and nothing else.
  input  logic                        cpu_req_valid,
  input  logic                        cpu_req_we,
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic [ADDR_WIDTH-1:0]       cpu_req_addr,
  /* verilator lint_on UNUSEDSIGNAL */
  input  logic [CPU_DATA_WIDTH-1:0]   cpu_req_wdata,
  input  logic [CPU_BYTES-1:0]        cpu_req_wmask,
  output logic                        cpu_req_ready,
  output logic                        cpu_resp_valid,
  output logic [CPU_DATA_WIDTH-1:0]   cpu_resp_rdata,
  output logic                        cpu_resp_fault,

  // ----------------------------------------------------------- memory port
  output logic                        mem_req_valid,
  output logic                        mem_req_we,     // 0 = refill read, 1 = writeback
  output logic [ADDR_WIDTH-1:0]       mem_req_addr,   // line aligned
  output logic [LINE_BITS-1:0]        mem_req_wdata,
  input  logic                        mem_req_ready,
  input  logic                        mem_resp_valid,
  input  logic [LINE_BITS-1:0]        mem_resp_rdata,
  input  logic                        mem_resp_fault,

  // ----------------------------------------------------------------- flush
  // Drain every dirty line, then invalidate the whole cache. `flush_ready` is
  // high only while the cache is idle, so a flush is a valid/ready transfer.
  input  logic                        flush_valid,
  output logic                        flush_ready,
  output logic                        flush_done,
  output logic                        flush_busy,

  // -------------------------------------------------------- state inspection
  // Combinational view of one set, so a testbench can assert e.g. "this line is
  // NOT valid after a failed refill" without inferring it from a later access.
  input  logic [INDEX_BITS-1:0]       dbg_index,
  output logic                        dbg_valid,
  output logic                        dbg_dirty,
  output logic [TAG_WIDTH-1:0]        dbg_tag,
  output logic [LINE_BITS-1:0]        dbg_data,

  // ------------------------------------------------------------ trace events
  output logic                        ev_hit,
  output logic                        ev_miss,
  output logic                        ev_refill,
  output logic                        ev_writeback,
  output logic                        ev_fault
);

  //  ------------------------------------------------------------------ storage
  // The two arrays have NO reset and NO initial value: see "valid bits vs. data
  // RAM". `valid` and `dirty` are the only per-line control state and they ARE
  // cleared by `rst`.
  logic [LINE_BITS-1:0] data_mem [SETS];
  logic [TAG_WIDTH-1:0] tag_mem  [SETS];
  logic [SETS-1:0]      valid;
  logic [SETS-1:0]      dirty;

  // Elaboration guards: a disagreement between the geometry this module assumes
  // and the geometry a caller asked for is an elaboration error, not a silent
  // misbuild.
  generate
    if ((1 << OFFSET_BITS) != LINE_BYTES) begin : g_bad_line_bytes
      mosaic_cache_contract_violation u_line_bytes ();
    end
    if ((1 << INDEX_BITS) != SETS) begin : g_bad_sets
      mosaic_cache_contract_violation u_sets ();
    end
    if (TAG_WIDTH < 1) begin : g_bad_tag
      mosaic_cache_contract_violation u_tag ();
    end
    if ((LINE_BYTES % CPU_BYTES) != 0) begin : g_bad_words
      mosaic_cache_contract_violation u_words ();
    end
  endgenerate

  // ------------------------------------------------------------------ helpers
  // The direct-mapped decomposition of the incoming CPU address is written as
  // plain expressions rather than functions: Verilator flags every address bit
  // a narrow function does not look at, and a partial argument is not a defect
  // here. The bit fields are declared with the request context below.

  // ---------------------------------------------------------------- FSM state
  typedef enum logic [2:0] {
    ST_IDLE       = 3'd0,
    ST_WRITEBACK  = 3'd1,   // evicting a dirty victim before a refill
    ST_REFILL     = 3'd2,   // reading the wanted line from memory
    ST_RESP       = 3'd3,   // one cycle of CPU response
    ST_FLUSH_SCAN = 3'd4,   // walking every set during a flush
    ST_FLUSH_WB   = 3'd5,   // writing back one dirty line during a flush
    ST_FLUSH_DONE = 3'd6    // one-cycle flush completion pulse
  } state_e;

  state_e state;

  // Registered request context.
  // Split so every stored address bit is actually consumed (a full-width
  // register would leave the byte-within-word bits dead and warn).
  logic [ADDR_WIDTH-1:OFFSET_BITS]       req_line_r;  // line number
  logic [OFFSET_BITS-1:WORD_BITS]        req_word_r;  // word in line
  logic [CPU_DATA_WIDTH-1:0] req_wdata_r;
  logic [CPU_BYTES-1:0]      req_wmask_r;
  logic                      req_we_r;
  logic [INDEX_BITS-1:0]     idx_r;
  // One bit wider than a set index so the flush walk can represent SETS itself
  // and terminate; with a SETS-wide counter the increment from SETS-1 wraps to
  // zero and the flush never finishes. That cast is exact because SETS is
  // forced to a power of two by the elaboration guard below.
  logic [INDEX_BITS:0]       flush_idx;
  logic                      refill_sent;
  logic                      resp_fault_r;

  // Combinational view of the incoming CPU request.
  logic [INDEX_BITS-1:0]     req_index;
  logic [TAG_WIDTH-1:0]      req_tag;
  assign req_index = cpu_req_addr[OFFSET_BITS + INDEX_BITS - 1 : OFFSET_BITS];
  assign req_tag   = cpu_req_addr[ADDR_WIDTH-1 : OFFSET_BITS + INDEX_BITS];

  // ------------------------------------------------------- memory request view
  logic [ADDR_WIDTH-1:0] victim_addr;
  assign victim_addr = {tag_mem[idx_r], idx_r, {OFFSET_BITS{1'b0}}};

`ifdef MOSAIC_CACHE_MUTANT_WRONG_REFILL
  // MUTANT 3: the refill reads the line *after* the one that missed, so every
  // line is filled with its neighbour's bytes. The card's "wrong refill
  // address".
  logic [ADDR_WIDTH-1:0] refill_addr;
  assign refill_addr = {req_line_r, {OFFSET_BITS{1'b0}}} ^ LINE_BYTES;
`else
  logic [ADDR_WIDTH-1:0] refill_addr;
  assign refill_addr = {req_line_r, {OFFSET_BITS{1'b0}}};
`endif

  assign mem_req_valid = (state == ST_WRITEBACK) || (state == ST_FLUSH_WB) ||
                         ((state == ST_REFILL) && !refill_sent);
  assign mem_req_we    = (state == ST_WRITEBACK) || (state == ST_FLUSH_WB);
  assign mem_req_addr  = ((state == ST_WRITEBACK) || (state == ST_FLUSH_WB))
                           ? victim_addr : refill_addr;
`ifdef MOSAIC_CACHE_MUTANT_DIRTY_DROP
  // MUTANT 2: the eviction is issued, but with zeroed data, so the dirty bytes
  // never reach memory. The card's "eviction loses dirty bytes".
  assign mem_req_wdata = {LINE_BITS{1'b0}};
`else
  assign mem_req_wdata = data_mem[idx_r];
`endif

  // ------------------------------------------------------------------- outputs
  assign cpu_req_ready = (state == ST_IDLE) && !flush_valid;
  assign cpu_resp_valid = (state == ST_RESP);
  assign cpu_resp_fault = resp_fault_r;
  assign cpu_resp_rdata = (state == ST_RESP && !resp_fault_r)
                           ? data_mem[idx_r][req_word_r * CPU_DATA_WIDTH +: CPU_DATA_WIDTH]
                           : {CPU_DATA_WIDTH{1'b0}};
  assign flush_ready = (state == ST_IDLE);
  assign flush_busy  = (state == ST_FLUSH_SCAN) || (state == ST_FLUSH_WB);

  // ------------------------------------------------------------- debug view
  assign dbg_valid = valid[dbg_index];
  assign dbg_dirty = dirty[dbg_index];
  assign dbg_tag   = tag_mem[dbg_index];
  assign dbg_data  = data_mem[dbg_index];

  // --------------------------------------------------------------------- FSM
  always_ff @(posedge clk) begin
    // Event pulses are one cycle wide; cleared every cycle and set by the
    // transitions below.
    ev_hit       <= 1'b0;
    ev_miss      <= 1'b0;
    ev_refill    <= 1'b0;
    ev_writeback <= 1'b0;
    ev_fault     <= 1'b0;
    flush_done   <= 1'b0;

    if (rst) begin
      state       <= ST_IDLE;
      valid       <= {SETS{1'b0}};
      dirty       <= {SETS{1'b0}};
      refill_sent <= 1'b0;
      resp_fault_r <= 1'b0;
      idx_r       <= '0;
      flush_idx   <= '0;
      req_line_r  <= '0;
      req_word_r  <= '0;
      req_wdata_r <= '0;
      req_wmask_r <= '0;
      req_we_r    <= 1'b0;
      // data_mem and tag_mem are deliberately NOT reset (see header).
    end else begin
      case (state)
        ST_IDLE: begin
          if (flush_valid) begin
            flush_idx <= '0;
            state     <= ST_FLUSH_SCAN;
          end else if (cpu_req_valid) begin
            req_line_r  <= cpu_req_addr[ADDR_WIDTH-1:OFFSET_BITS];
            req_word_r  <= cpu_req_addr[OFFSET_BITS-1:WORD_BITS];
            req_wdata_r <= cpu_req_wdata;
            req_wmask_r <= cpu_req_wmask;
            req_we_r    <= cpu_req_we;
            idx_r       <= req_index;
            resp_fault_r <= 1'b0;

            if (valid[req_index] && (tag_mem[req_index] == req_tag)) begin
              // -------------------------------------------------------- hit
              ev_hit <= 1'b1;
              if (cpu_req_we && !READ_ONLY) begin
                for (int unsigned b = 0; b < CPU_BYTES; b++) begin
                  if (cpu_req_wmask[b]) begin
                    data_mem[req_index][(cpu_req_addr[OFFSET_BITS-1:WORD_BITS] * CPU_BYTES + b) * 8 +: 8]
                      <= cpu_req_wdata[8*b +: 8];
                  end
                end
                dirty[req_index] <= 1'b1;
              end
              if (cpu_req_we && READ_ONLY) begin
                resp_fault_r <= 1'b1;
              end
              state <= ST_RESP;
            end else if (cpu_req_we && READ_ONLY) begin
              // A store offered to an instruction cache is not a cache event at
              // all: answer the fault and touch nothing.
              resp_fault_r <= 1'b1;
              state        <= ST_RESP;
            end else begin
              // ------------------------------------------------------- miss
              ev_miss <= 1'b1;
              refill_sent <= 1'b0;
              if (!READ_ONLY && valid[req_index] && dirty[req_index]) begin
                state <= ST_WRITEBACK;
              end else begin
                state <= ST_REFILL;
              end
            end
          end
        end

        ST_WRITEBACK: begin
          // Victim line is dirty; write it back, then refill the wanted line.
          if (mem_req_ready) begin
            ev_writeback <= 1'b1;
            dirty[idx_r] <= 1'b0;
            refill_sent  <= 1'b0;
            state        <= ST_REFILL;
          end
        end

        ST_REFILL: begin
          if (!refill_sent) begin
            if (mem_req_ready) refill_sent <= 1'b1;
          end else if (mem_resp_valid) begin
            if (mem_resp_fault) begin
`ifdef MOSAIC_CACHE_MUTANT_FAULT_VALID
              // MUTANT 1: a faulted refill is still installed as if it had
              // succeeded. The card's "an error refill marked valid".
              data_mem[idx_r] <= mem_resp_rdata;
              tag_mem[idx_r]  <= req_line_r[ADDR_WIDTH-1:OFFSET_BITS+INDEX_BITS];
              valid[idx_r]    <= 1'b1;
              dirty[idx_r]    <= req_we_r && !READ_ONLY;
`endif
              resp_fault_r <= 1'b1;
              ev_fault     <= 1'b1;
              state        <= ST_RESP;
            end else begin
              data_mem[idx_r] <= mem_resp_rdata;
              tag_mem[idx_r]  <= req_line_r[ADDR_WIDTH-1:OFFSET_BITS+INDEX_BITS];
              valid[idx_r]    <= 1'b1;
              dirty[idx_r]    <= req_we_r && !READ_ONLY;
              if (req_we_r && !READ_ONLY) begin
                for (int unsigned b = 0; b < CPU_BYTES; b++) begin
                  if (req_wmask_r[b]) begin
                    data_mem[idx_r][(req_word_r * CPU_BYTES + b) * 8 +: 8]
                      <= req_wdata_r[8*b +: 8];
                  end
                end
              end
              resp_fault_r <= 1'b0;
              ev_refill    <= 1'b1;
              state        <= ST_RESP;
            end
          end
        end

        ST_RESP: begin
          state <= ST_IDLE;
        end

        ST_FLUSH_SCAN: begin
          if (flush_idx == (INDEX_BITS + 1)'(SETS)) begin
            state <= ST_FLUSH_DONE;
          end else if (valid[flush_idx[INDEX_BITS-1:0]] && dirty[flush_idx[INDEX_BITS-1:0]]) begin
            idx_r <= flush_idx[INDEX_BITS-1:0];
            state <= ST_FLUSH_WB;
          end else begin
            valid[flush_idx[INDEX_BITS-1:0]] <= 1'b0;
            dirty[flush_idx[INDEX_BITS-1:0]] <= 1'b0;
            flush_idx <= flush_idx + 1'b1;
          end
        end

        ST_FLUSH_WB: begin
          if (mem_req_ready) begin
            ev_writeback <= 1'b1;
            valid[idx_r] <= 1'b0;
            dirty[idx_r] <= 1'b0;
            flush_idx <= flush_idx + 1'b1;
            state     <= ST_FLUSH_SCAN;
          end
        end

        ST_FLUSH_DONE: begin
          flush_done <= 1'b1;
          state      <= ST_IDLE;
        end

        default: state <= ST_IDLE;
      endcase
    end
  end

endmodule

`resetall
