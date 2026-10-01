// MosaicRV work package I-005 -- `mosaic_skid_buffer`.
//
// A one-deep *fall-through* elastic buffer. It exists for exactly one reason:
// to give a producer a cycle of slack so that a downstream stall of one stage
// cannot back up into the producer in the same cycle.
//
// Contract
// --------
//   * A transfer on the input port happens on exactly those rising edges where
//     `in_valid && in_ready`. A transfer on the output port happens on exactly
//     those rising edges where `out_valid && out_ready`.
//   * `out_valid = valid_r || in_valid` and
//     `out_payload = valid_r ? payload_r : in_payload`:
//       - while the buffer holds nothing, the input is presented straight
//         through, so a producer and a consumer can transfer in the *same*
//         cycle with no latency at all. This is the fall-through property;
//       - once an item is held, the output is that item and nothing else.
//   * `in_ready = !valid_r || out_ready`. The three consequences, which are the
//     properties the unit test asserts directly:
//       1. with the output stalled and the buffer empty, `in_ready` is still 1,
//          so one more item is absorbed by downstream backpressure;
//       2. the item the output presents is the first-in item;
//       3. while that item is held and the output is still stalled, `in_ready`
//          is 0, so no second item is taken.
//   * Payload stability: while `out_valid` is high and `out_ready` is low,
//     `out_valid` and `out_payload` do not change, for any number of cycles.
//   * When `out_valid` is 0, `out_payload` is a don't-care.
//   * Reset is synchronous and active high: it clears the occupancy bit only.
//     `payload_r` is data, not control state, and is not reset.
//
// When this module may be used
// ----------------------------
//   Use it when a producer must not be stalled in the same cycle its consumer
//   stalls -- i.e. when the design wants one item of decoupling across a single
//   pipeline stage.
//
// Do NOT use it:
//   * to bridge clock domains. This is a single-domain elastic buffer with no
//     synchroniser; a CDC crossing needs a handshake or an async FIFO (see the
//     reset/CDC obligations in docs/platform-plan.md, I-024).
//   * to absorb more than one cycle of downstream stall. It holds exactly one
//     item; a burst of N stalled cycles still reaches the producer after the
//     first one. For N items of slack use `mosaic_fifo` with `DEPTH >= N`.
//   * to hide an arbitrarily long combinational ready path. The ready path
//     `out_ready -> in_ready` is combinational *through this module*, and the
//     payload path `in_payload -> out_payload` is combinational *through* it
//     too. One instance therefore costs exactly one stage of ready/payload
//     combinational delay. Chaining instances multiplies that. It is legal only
//     where the registered timing budget has room for the single extra stage it
//     introduces; where it does not, use `mosaic_fifo`, whose input ready is a
//     function of a register only and adds no combinational path at all.
//   * as a credit source. It stores no credit; it only avoids a bubble.
//
// Relationship to `mosaic_fifo`
// ---------------------------
//   At DEPTH == 1 the two have identical functional behaviour. They are not
//   interchangeable in timing: `mosaic_fifo`'s `in_ready` is a function of the
//   registered `count` alone, so it never contributes to a ready path, but it
//   cannot accept and emit in the same cycle that it drains. `mosaic_skid_buffer`
//   trades the registered ready for that same-cycle accept-and-emit.

`default_nettype none

module mosaic_skid_buffer #(
    parameter int unsigned WIDTH = 32
) (
    input  logic                 clk,
    input  logic                 rst,

    input  logic                 in_valid,
    output logic                 in_ready,
    input  logic [WIDTH-1:0]     in_payload,

    output logic                 out_valid,
    input  logic                 out_ready,
    output logic [WIDTH-1:0]     out_payload
);

  logic             valid_r;
  logic [WIDTH-1:0] payload_r;

  // Present the held item if there is one, otherwise pass the input straight
  // through. This is the zero-latency property; it is also what makes the
  // payload path combinational, which bounds where this module may sit.
  always_comb begin
    out_valid   = valid_r || in_valid;
    out_payload = valid_r ? payload_r : in_payload;
  end

  // Absorb an item whenever the buffer is empty or is being drained this cycle.
  always_comb in_ready = !valid_r || out_ready;

  always_ff @(posedge clk) begin
    if (rst) begin
      valid_r <= 1'b0;
    end else if (out_ready) begin
      // The held item leaves this cycle. The only item that must be kept is the
      // one the producer offers now *and* that was held rather than presented
      // by fall-through: if valid_r was 0 the new item is already being taken
      // by the consumer combinationally, so re-latching it would duplicate it.
      valid_r <= valid_r && in_valid;
      if (valid_r && in_valid) begin
        payload_r <= in_payload;
      end
    end else begin
      // Output stalled. Keep holding whatever is already held, and absorb the
      // producer's item only when there is room for it -- which is exactly when
      // in_ready is high. Capturing the payload while in_ready is low would
      // take an item the producer never agreed to hand over, and would then
      // present it in place of the item that was legitimately held.
      valid_r <= valid_r || in_valid;
      if (!valid_r && in_valid) begin
        payload_r <= in_payload;
      end
    end
  end

endmodule

`default_nettype wire
