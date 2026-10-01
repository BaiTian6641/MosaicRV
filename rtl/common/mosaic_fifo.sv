// MosaicRV work package I-005 -- `mosaic_fifo`.
//
// A dependency-free valid/ready FIFO. Every downstream packet channel in the
// core (operand buffers, result queues, memory-request credits, completion
// notifications) uses this one module rather than re-inventing handshake logic,
// so the contract below is the project's single definition of "a transfer".
//
// Contract
// --------
//   * A transfer on the input port happens on exactly those rising edges where
//     `in_valid && in_ready` are both high. Nothing else moves data in.
//   * A transfer on the output port happens on exactly those rising edges where
//     `out_valid && out_ready` are both high. Nothing else moves data out.
//   * `in_ready` is 0 if and only if the FIFO is full, i.e. `count == DEPTH`.
//     `out_valid` is 1 if and only if the FIFO is non-empty, i.e. `count != 0`.
//   * Payload stability: while `out_valid` is high and `out_ready` is low,
//     `out_valid` and `out_payload` do not change, for any number of cycles.
//     This is structural -- `out_payload` is the storage word at `rd_ptr`, and
//     `rd_ptr` only advances on an output transfer, while a concurrent write can
//     only land on `mem[wr_ptr]`, which is a different word whenever the FIFO is
//     not full.
//   * When `out_valid` is 0, `out_payload` is a don't-care: it reads a storage
//     word that no transfer has put there. Consumers must gate on `out_valid`.
//   * Reset: `rst` is synchronous and active high. On a rising edge with `rst`
//     high, `count`, `rd_ptr` and `wr_ptr` clear. The storage array is *not*
//     reset -- reset cost is three state words regardless of `DEPTH`, and the
//     array is data, not control state. Because reset is synchronous, the
//     outputs still show the pre-reset state for the reset cycle itself; they
//     are clean from the first cycle after the reset edge. Consumers must not
//     treat a transfer during a reset cycle as a transfer.
//   * There is no flush/cancel port. Discarding buffered data is done with
//     `rst`, which is the only event that drops items, so every discarded item
//     is accounted for exactly once as "cancelled" by the consumer's
//     conservation accounting.
//
// Timing shape
// ------------
//   * `in_ready`, `out_valid`, `out_payload` and `count` are all functions of
//     registers only (`count`, `rd_ptr`) plus, for `out_payload`, the storage
//     word selected by a register. No combinational path leaves this module
//     towards either neighbour, and no combinational path enters it from
//     `out_ready`. A downstream stage that needs same-cycle accept-and-emit
//     where this FIFO has a bubble uses `mosaic_skid_buffer`; this FIFO trades
//     that bubble for a fully registered input ready. The unit test asserts the
//     "no combinational dependence on out_ready" property directly.
//   * `mem` is written by the clock and read combinationally. For the small
//     depths used for packet channels this maps to distributed RAM or flops.
//     A block-RAM (registered read) variant is a *different module*: it would
//     add a cycle of read latency and so change the output handshake. It is
//     deliberately not a mode of this one.
//
// Parameters
// ----------
//   WIDTH  payload width in bits, must be >= 1.
//   DEPTH  number of entries, must be >= 1. `DEPTH == 1` is supported: the
//          pointer width is forced to 1 and the wrap is an explicit comparison
//          against `DEPTH - 1`, never a zero-width increment that wraps.
//
// Mutation hooks
// --------------
//   The shipping build defines none of the `MOSAIC_FIFO_MUTANT_*` macros. Each
//   one injects exactly one broken behaviour so the unit test can be shown to
//   detect it; the mutant table is in results/reports/I-005-fifo.md. They exist
//   to prove the test has teeth and have no place in any other build.

`default_nettype none

module mosaic_fifo #(
    parameter int unsigned WIDTH = 32,
    parameter int unsigned DEPTH = 4
) (
    input  logic                         clk,
    input  logic                         rst,

    // Input port: a transfer happens when in_valid && in_ready.
    input  logic                         in_valid,
    output logic                         in_ready,
    input  logic [WIDTH-1:0]             in_payload,

    // Output port: a transfer happens when out_valid && out_ready.
    output logic                         out_valid,
    input  logic                         out_ready,
    output logic [WIDTH-1:0]             out_payload,

    // Entries currently held. Observable occupancy is part of the contract
    // because every downstream user's credit accounting is checked against it.
    output logic [$clog2(DEPTH+1)-1:0]   count
);

  // DEPTH == 1 would make $clog2(DEPTH) zero; force one pointer bit. The wrap
  // below is an explicit comparison, so a one-deep FIFO never relies on a
  // degenerate zero-width increment.
  localparam int unsigned PtrW = (DEPTH <= 1) ? 1 : $clog2(DEPTH);
  localparam int unsigned CntW = $clog2(DEPTH + 1);

  localparam logic [PtrW-1:0] LastPtr = PtrW'(DEPTH - 1);
  localparam logic [CntW-1:0] DepthC = CntW'(DEPTH);

`ifdef MOSAIC_FIFO_MUTANT_DROP_ON_FULL
  localparam bit MutDropOnFull   = 1'b1;
`else
  localparam bit MutDropOnFull   = 1'b0;
`endif
`ifdef MOSAIC_FIFO_MUTANT_PHANTOM_POP
  localparam bit MutPhantomPop   = 1'b1;
`else
  localparam bit MutPhantomPop   = 1'b0;
`endif
`ifdef MOSAIC_FIFO_MUTANT_UNSTABLE_OUT
  localparam bit MutUnstableOut  = 1'b1;
`else
  localparam bit MutUnstableOut  = 1'b0;
`endif

  // Data array: no reset. Reset cost is CntW + 2*PtrW state words, independent
  // of DEPTH.
  logic [WIDTH-1:0] mem [0:DEPTH-1];

  logic [PtrW-1:0] rd_ptr;
  logic [PtrW-1:0] wr_ptr;

  logic full;
  logic empty;
  logic push;   // input transfer, as seen on the wires
  logic pop;    // output transfer, as seen on the wires
  logic store;  // input transfer that actually writes mem

  assign full  = (count == DepthC);
  assign empty = (count == {CntW{1'b0}});

  // Mutation 1: `in_ready` is one entry too optimistic, so the producer observes
  // a completed transfer while the FIFO is full; `store` then discards it.
  // Every other build reports readiness from the registered count alone.
  always_comb in_ready = MutDropOnFull ? 1'b1 : !full;

  // Mutation 2: `out_valid` ignores emptiness and offers a phantom transfer.
  always_comb out_valid = MutPhantomPop ? 1'b1 : !empty;

  // The head of the queue is the storage word the read pointer selects. Under a
  // stall that word cannot be written: a write can only target mem[wr_ptr], and
  // wr_ptr only coincides with rd_ptr when the FIFO is full, which is exactly
  // when in_ready is 0. Payload stability is therefore structural.
  //
  // Mutation 3: the output mux is fed by the live input bus instead of the held
  // head word, so the presented item is re-sampled every cycle and moves while
  // out_valid is high and out_ready is low.
  always_comb out_payload = MutUnstableOut ? (in_valid ? in_payload : mem[rd_ptr])
                                            : mem[rd_ptr];

  assign push = in_valid && in_ready;
  assign pop  = out_valid && out_ready;

  assign store = MutDropOnFull ? (push && !full) : push;

  always_ff @(posedge clk) begin
    if (rst) begin
`ifdef MOSAIC_FIFO_MUTANT_STALE_RESET
      // Mutation 4: reset clears the write side but forgets the read side and
      // the occupancy, so a pre-reset item escapes the reset as a fresh, valid,
      // phantom transfer.
      wr_ptr <= {PtrW{1'b0}};
`else
      count  <= {CntW{1'b0}};
      rd_ptr <= {PtrW{1'b0}};
      wr_ptr <= {PtrW{1'b0}};
`endif
    end else begin
      if (store && !pop) begin
        count <= count + CntW'(1);
      end else if (!store && pop) begin
        count <= count - CntW'(1);
      end

      if (store) begin
        mem[wr_ptr] <= in_payload;
        wr_ptr      <= (wr_ptr == LastPtr) ? {PtrW{1'b0}} : (wr_ptr + PtrW'(1));
      end

      if (pop) begin
        rd_ptr <= (rd_ptr == LastPtr) ? {PtrW{1'b0}} : (rd_ptr + PtrW'(1));
      end
    end
  end

endmodule

`default_nettype wire
