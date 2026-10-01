// ============================================================================
// mosaic_ram -- portable synchronous RAM abstraction (work package I-006)
//
// One write port, one read port, fixed one-cycle read latency, byte-granular
// writes, and *no* reset on the storage array. Nothing in this file names a
// vendor primitive, an FPGA block, or a board: the whole point of the module
// is that a platform RAM with a different mixed-port mode is absorbed by a
// wrapper that implements exactly the contract written below, and by nothing
// else anywhere in the tree.
//
// ------------------------------------------------------------------ contract
//
// Timing (all edges are rising edges of `clk`; everything is sampled on the
// edge and observable in the cycle that follows it):
//
//   * Write port. `we && !rst` at cycle N commits `wdata` to `waddr` at the
//     edge ending cycle N. `wmask[i]` selects byte lane i
//     (lane i == bits [8*i +: 8]). Lanes whose mask bit is 0 keep their
//     previous content. `wmask == 0` writes nothing at all. With
//     BYTE_ENABLE_PORTS == 0 `wmask` is ignored and every lane is written.
//
//   * Read port. `raddr` presented in cycle N produces `rdata` in cycle N+1,
//     i.e. READ_LATENCY is exactly 1 and never more. The read port has no
//     enable and never stalls: back-to-back reads issue one per cycle and
//     return one per cycle, in order.
//
//   * `rvalid` is the read-port valid. It is high in every cycle whose
//     predecessor cycle was a non-reset cycle, and low in every cycle whose
//     predecessor cycle was a reset cycle. A read issued while `rst` is high
//     returns neither data nor valid; the port is dead during reset.
//
//   * `rst` is synchronous. It gates the control state and the write port.
//     It does *not* touch the array (see "Reset strategy").
//
// ----------------------------------------------------------------- collision
//
// Same-address same-cycle read/write (read `raddr` == write `waddr` in the
// same cycle) resolves to READ-FIRST, byte by byte:
//
//   * a byte lane of `rdata` whose `wmask` bit is 1 returns the byte value
//     that was in the array *before* this cycle's write;
//   * a byte lane whose `wmask` bit is 0 returns the byte that was in the
//     array before this cycle's write as well (it was not modified).
//
// So the read issued in cycle N always observes the array as it was at the
// beginning of cycle N. There is no undefined mode, no "new data" mode, and
// no platform-dependent tie-break.
//
// Why read-first is the contract for this project's first consumer: the
// integer PRF reads its two source operands and writes its one destination
// physical register in the same cycle. `add x5, x5, x3` writes and reads the
// same physical register in the same cycle, and RISC-V requires the source
// operand to be the *old* value. Read-first yields that for free, with no
// bypass network in the PRF and no special case for rd == rs1 / rd == rs2. A
// "write-first" (new-data) mode would silently corrupt every rd == rs
// instruction; a "no-change" mode would make the result depend on the array's
// previous contents and would be untestable on a real board.
//
// Portability: this is exactly the read-during-write = "old data" mode that
// FPGA block RAMs offer (READ_FIRST in Xilinx/AMD, and the equivalent in
// Gowin and Lattice). An ASIC macro whose only mixed-port mode is write-first
// or pass-through needs one line inside the platform wrapper -- an explicit
// bypass register or an external arbiter that serialises the two ports -- and
// no change to this contract, to any consumer, or to any test.
//
// ------------------------------------------------------------ reset strategy
//
// The array has no reset and no initial value in hardware. The only register
// cleared by `rst` is `rvalid`, a single control bit. `rdata` is a plain
// pipeline register that is not reset either, because `rvalid` masks it while
// it holds a meaningless value.
//
// That is sufficient because validity is tracked *outside* the array: the
// owner of the RAM (the PRF free list, the cache tag array, a boot ROM loader)
// holds the valid bit per entry and clears it explicitly. A consumer must
// never read an entry it has not written since reset; the array's power-up
// contents are undefined by design, and resetting a DEPTH x DATA_WIDTH array
// would turn an inferred block RAM into DEPTH x DATA_WIDTH flip-flops, which
// is precisely the "unexpected full-FF reset" this card blocks.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_RAM_MUTANT=n selects a deliberately broken variant used to prove
// the unit test can detect the corresponding bug. The shipping build defines
// none of them; see results/reports/I-006-ram.md.
// ============================================================================

`default_nettype none
`resetall

module mosaic_ram #(
  parameter int DATA_WIDTH        = 64,
  parameter int DEPTH             = 64,
  parameter int BYTE_ENABLE_PORTS = 1,
  // Derived, and therefore not overridable: a caller can change DEPTH or
  // DATA_WIDTH but can never create an address width that disagrees with the
  // array depth.
  localparam int ADDR_WIDTH   = $clog2(DEPTH),
  localparam int NUM_BYTES    = DATA_WIDTH / 8,
  localparam int READ_LATENCY = 1
) (
  input  logic                   clk,
  input  logic                   rst,

  input  logic [ADDR_WIDTH-1:0]  waddr,
  input  logic [DATA_WIDTH-1:0]  wdata,
  input  logic [NUM_BYTES-1:0]   wmask,
  input  logic                   we,

  input  logic [ADDR_WIDTH-1:0]  raddr,
  output logic [DATA_WIDTH-1:0]  rdata,
  output logic                   rvalid
);

  // The storage array: no reset, no initial block. See "reset strategy".
  logic [DATA_WIDTH-1:0] mem [DEPTH];

  // Elaboration guards. A false branch is never elaborated, so referencing a
  // module that does not exist is an elaboration-time error and not a
  // lint warning in the shipping configuration.
  generate
    if (READ_LATENCY != 1) begin : g_bad_read_latency
      mosaic_ram_contract_violation u_read_latency();
    end
    if ((1 << ADDR_WIDTH) != DEPTH) begin : g_bad_depth
      mosaic_ram_contract_violation u_depth();
    end
    if ((DATA_WIDTH % 8) != 0) begin : g_bad_data_width
      mosaic_ram_contract_violation u_data_width();
    end
  endgenerate

  // ---------------------------------------------------------------- write port
  always_ff @(posedge clk) begin
`ifdef MOSAIC_RAM_MUTANT_ARRAY_RESET
    // NEGATIVE CONTROL 4: resets the whole array. Prohibited by the card.
    if (rst) begin
      for (int unsigned i = 0; i < DEPTH; i++) mem[i] <= '0;
    end else
`endif
      if (!rst && we) begin
`ifdef MOSAIC_RAM_MUTANT_NO_BYTE_ENABLE
        // NEGATIVE CONTROL 2: ignores wmask and writes every lane.
        mem[waddr] <= wdata;
`else
        if (BYTE_ENABLE_PORTS != 0) begin
          for (int unsigned b = 0; b < NUM_BYTES; b++) begin
            if (wmask[b]) mem[waddr][8*b +: 8] <= wdata[8*b +: 8];
          end
        end else begin
          mem[waddr] <= wdata;
        end
`endif
      end
  end

  // ----------------------------------------------------------------- read port
  // `rdata` is a register. It samples the array on the edge, which is what
  // makes the read synchronous and what gives read-first collision semantics
  // for free: the non-blocking assignment observes the array as it was before
  // this edge, so a same-cycle write to `raddr` is not visible to this read.
`ifdef MOSAIC_RAM_MUTANT_COMB_READ
  // NEGATIVE CONTROL 1: combinational read, i.e. "simulation reads in zero
  // cycles, the board reads in one". Exactly the blocking failure the card
  // names.
  always_comb rdata = mem[raddr];
`else
  always_ff @(posedge clk) begin
    if (!rst) rdata <= mem[raddr];
  end
`endif

`ifdef MOSAIC_RAM_MUTANT_EARLY_VALID
  // NEGATIVE CONTROL 3: rvalid in the cycle the address is presented, i.e.
  // one cycle early relative to the data.
  assign rvalid = ~rst;
`else
  always_ff @(posedge clk) begin
    if (rst) rvalid <= 1'b0;
    else     rvalid <= 1'b1;
  end
`endif

endmodule

`resetall
