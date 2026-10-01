// ============================================================================
// mosaic_predictor -- work package I-021.
//
// A bimodal direction predictor, a branch target buffer and a return-address
// stack, sized entirely from the generated configuration package. Nothing here
// has a hardcoded depth: `MOSAIC_BPU_ENTRIES` / `MOSAIC_BTB_ENTRIES` /
// `MOSAIC_RAS_ENTRIES` are the geometry file's values, so changing
// config/geometry/<profile>.json changes the hardware and the test's shadow
// model together, and there is no second copy to drift.
//
// ------------------------------------------------ what is advisory, and what
// ------------------------------------------------ makes the core correct
//
// This module has **no architectural state**. Every output is a *hint* to the
// fetch unit, and every hint is re-derived from execution:
//
//   pred_taken      advisory direction. Wrong => a mispredict, recovered.
//   pred_target     advisory next PC. Wrong => a mispredict, recovered.
//   pred_redirect   advisory "take the predicted target now".
//   pred_btb_hit    advisory: the BTB supplied pred_target.
//   pred_btb_miss   **report**: the query needed a target and none was
//                   available, so pred_target is the sequential pc + 4 and
//                   fetch must not treat it as a prediction. This is the one
//                   output a fetch unit cannot reconstruct on its own: it must
//                   be told, because "I have no target" and "my target is the
//                   fall-through address" are otherwise the same 64-bit value.
//   pred_ras_valid      pred_target came from the RAS.
//   pred_ras_underflow  a return was predicted with an empty RAS. Reported, not
//                   silently answered with a garbage address.
//   ras_overflow    a resolved call arrived with the RAS already full; the push
//                   was dropped. Reported.
//   ras_underflow   a resolved return arrived with the RAS empty; the pop was
//                   dropped. Reported.
//
// A wrong prediction therefore cannot change architectural behaviour: the
// resolved target comes from mosaic_branch_target at execute and the resolved
// outcome from mosaic_branch_cmp, and `upd_mispredict` tells the core the
// guess was wrong so it can squash and refetch. What the predictor must never
// do is be *silently* wrong, which is why every miss, every RAS anomaly and
// every mispredict is an output rather than an internal detail.
//
// ---------------------------------------------------------- timing contract
//
// Two independent ports, both sampled on the rising edge of `clk`:
//
//   * The query port (q_*) is **combinational**. A PC presented in cycle N
//     produces pred_* in cycle N. It reads the bimodal counter, the BTB entry
//     and the RAS top directly; there is no read latency and nothing to stall.
//     Fetch presents the PC it is fetching and redirects on the same cycle.
//   * The update port (upd_*) is a single-entry, at-most-one-event-per-cycle
//     resolve interface. `upd_valid` with any of the class bits set is one
//     resolved control transfer, applied at the edge ending that cycle. Two
//     updates in one cycle is not a supported event; the core serialises them.
//     This is a deliberate simplicity, not an oversight: I-021 is the first
//     predictor, and a multi-entry update port would be untested width.
//
// `rst` is synchronous and active high. It clears the valid bits and the RAS
// pointers. It does **not** clear any array contents: the bimodal counters,
// the BTB tags and targets and the RAS stack memory are data, and validity is
// tracked outside them in explicit valid-bit vectors. That is the rule
// mosaic_ram.sv documents, and it is what keeps these structures inferable as
// RAM rather than DEPTH*WIDTH flip-flops with a reset.
//
// Because reset is synchronous, the outputs still show the pre-reset state
// during the reset cycle itself and are clean from the first cycle after the
// reset edge.
//
// ------------------------------------------------------- index truncation
//
// The index of any structure is the PC's bits [IDX_W:1]. Bit 0 is dropped
// because every instruction address is 4-byte aligned and bit 0 is therefore
// always zero, so including it would halve the table for nothing.
//
// IDX_W is `$clog2(DEPTH)`, which for a DEPTH that is **not** a power of two
// means the index can name entries that do not exist. That decision is made
// explicitly rather than left implicit:
//
//   * An index >= DEPTH is treated as a guaranteed **miss**: the BTB never
//     returns a target for it, and the direction table never trains it.
//   * An update landing on such an index is **dropped**, not wrapped. Wrapping
//     would alias one PC's tag onto a different PC's entry and manufacture a
//     false hit, which is the one outcome a predictor must never produce.
//   * The consequence is a loss of coverage, never a loss of correctness: with
//     DEPTH=3 a whole quarter of the address space can never be allocated.
//     Those PCs take the sequential path and are reported as misses every
//     time, which costs fetch bandwidth and nothing else, because a prediction
//     is advisory and a miss is reported.
//
// For a power-of-two DEPTH -- every profile in config/geometry/ today -- the
// range check folds to a constant and costs nothing.
//
// -------------------------------------------------------------- RAS policy
//
// The RAS is updated on **resolve**, not on prediction, and read on prediction.
// The consequence is that the RAS lags the fetch stream by the resolve latency
// and therefore never holds speculative state of its own. The checkpoint and
// flush ports exist for the one case that still needs them: a control transfer
// that resolved speculatively and was later squashed by an older mispredict.
//
//   * `ckpt_valid` saves the pre-edge stack pointer into `ras_ckpt`.
//   * `flush` restores the stack pointer from `ras_ckpt` and discards any
//     update offered in the same cycle.
//
// The caller must assert `ckpt_valid` in the cycle of the resolve whose RAS
// effect it may later need to undo, and `flush` when that resolve turns out to
// have been squashed. It must **not** assert `flush` for a transfer it has
// already committed, because the RAS update of a committed call is real and
// undoing it would lose a return address. That ordering is the core's
// responsibility (I-018 recovery); it is stated here because the RAS is
// useless without it.
//
// Defined edge cases, both reported and both tested:
//
//   * **Push with a full stack** (ras_sp == RAS_ENTRIES): the push is dropped
//     and `ras_overflow` pulses. The stack saturates. It does not wrap, does
//     not overwrite an older entry, and does not move the pointer, because all
//     three of those turn a lost push into a *plausible wrong address*, and a
//     plausible wrong address is what a return-address stack must never emit.
//     The oldest RAS_ENTRIES entries survive; the newest is lost.
//   * **Pop with an empty stack** (ras_sp == 0): the pop is dropped,
//     `ras_underflow` pulses, the pointer stays at 0, and the target comes
//     from the BTB instead. The stack never underflows past zero and never
//     reads a stale entry.
//
// ------------------------------------------------------------- mispredicts
//
// The direction counter trains on every resolved conditional branch, taken or
// not, so a branch that changes behaviour is followed rather than frozen. The
// BTB is rewritten from the resolved target on every resolving transfer,
// including an indirect jump whose target changes between executions -- the
// resolved value is always authoritative, so a return or indirect jump that
// goes somewhere new is self-correcting on its next resolve.
//
// Mispredict-driven reallocation (giving a hard-to-predict branch a second
// entry on repeated mispredicts) is **not** implemented. The card makes it
// optional and it would need an eviction policy that nothing in I-021 can
// currently justify. Every entry here is allocated by the same rule, so the
// miss behaviour the aliasing test measures is not polluted by a second
// allocation path.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_PREDICTOR_MUTANT_<n> injects one defect to prove the case can fail.
// The shipping build defines none of them; the table with real output is in
// results/reports/I-021-predictor.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names three of them; the rest belong to other
// modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_predictor #(
    // Depth overrides exist so a test can exercise a non-power-of-two depth;
    // the defaults are the generated geometry and the shipping build uses them.
    parameter int unsigned XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN,
    parameter int unsigned BPU_ENTRIES = mosaic_cfg_pkg::MOSAIC_BPU_ENTRIES,
    parameter int unsigned BTB_ENTRIES = mosaic_cfg_pkg::MOSAIC_BTB_ENTRIES,
    parameter int unsigned RAS_ENTRIES = mosaic_cfg_pkg::MOSAIC_RAS_ENTRIES
) (
    input  logic                     clk,
    input  logic                     rst,

    // ------------------------------------------------------------ query port
    // Combinational: the PC presented now yields pred_* now.
    input  logic [XLEN-1:0]          q_pc,
    input  logic                     q_valid,       // a prediction is wanted
    input  logic                     q_is_branch,   // conditional: direction matters
    input  logic                     q_is_jump,     // unconditional: target matters
    input  logic                     q_is_return,   // indirect jump via the link register

    // ---------------------------------------------------------- update port
    // One resolved control transfer per cycle, applied at the rising edge.
    input  logic                     upd_valid,
    input  logic [XLEN-1:0]          upd_pc,
    input  logic                     upd_is_branch,
    input  logic                     upd_is_jump,     // includes returns
    input  logic                     upd_is_call,     // writes x1 or x5
    input  logic                     upd_is_return,
    input  logic                     upd_taken,
    input  logic [XLEN-1:0]          upd_target,

    // ------------------------------------------------- RAS recovery control
    input  logic                     ckpt_valid,   // save the pre-edge RAS pointer
    input  logic                     flush,         // restore it; drops this cycle's update

    // ------------------------------------------------------- query outputs
    output logic                     pred_taken,
    output logic [XLEN-1:0]          pred_target,
    output logic                     pred_redirect,
    output logic                     pred_btb_hit,
    output logic                     pred_btb_miss,
    output logic                     pred_ras_valid,
    output logic                     pred_ras_underflow,

    // ------------------------------------------------------ update outputs
    output logic                     ras_overflow,
    output logic                     ras_underflow
);

  // --------------------------------------------------------------- geometry
  // $clog2 of 1 is 0, which would produce a zero-width part select; the guard
  // forces one bit so a one-entry structure never relies on a degenerate
  // width. Same convention as mosaic_fifo's PtrW.
  localparam int unsigned BPU_IDX_W = (BPU_ENTRIES <= 1) ? 1 : $clog2(BPU_ENTRIES);
  localparam int unsigned BTB_IDX_W = (BTB_ENTRIES <= 1) ? 1 : $clog2(BTB_ENTRIES);
  // The stack pointer must represent DEPTH itself ("full"), not just 0..DEPTH-1.
  localparam int unsigned RAS_SP_W  = $clog2(RAS_ENTRIES + 1);
  // The RAS data array is addressed by an entry number in 0..RAS_ENTRIES-1,
  // which is one bit narrower than the pointer that also has to say "full".
  localparam int unsigned RAS_IDX_W = (RAS_ENTRIES <= 1) ? 1 : $clog2(RAS_ENTRIES);

  localparam logic [1:0] KIND_BRANCH = 2'd1;   // conditional: BTB holds its target
  localparam logic [1:0] KIND_JUMP   = 2'd2;   // unconditional / indirect

  // ------------------------------------------------------------------ state
  // Data arrays: no reset, no initial value. See "reset contract" in the header.
  logic [1:0]        bpu_ctr    [BPU_ENTRIES];  // 2-bit saturating, msb = taken
  logic [XLEN-1:0]   btb_tag    [BTB_ENTRIES];
  logic [XLEN-1:0]   btb_target [BTB_ENTRIES];
  logic [1:0]        btb_kind   [BTB_ENTRIES];
  logic [XLEN-1:0]   ras_mem    [RAS_ENTRIES];

  // Control state: this is what reset is allowed to touch, and it is the only
  // thing it touches. One bit per entry rather than a cleared array, per the
  // mosaic_ram.sv rule.
  logic [BPU_ENTRIES-1:0] bpu_valid;
  logic [BTB_ENTRIES-1:0] btb_valid;

  logic [RAS_SP_W-1:0] ras_sp;      // speculative top, 0..RAS_ENTRIES
  logic [RAS_SP_W-1:0] ras_ckpt;    // recovery point, written by ckpt_valid

  // ------------------------------------------------------- query index decode
  // Bits [IDX_W:1]; bit 0 is always zero for an instruction address.
  logic [BPU_IDX_W-1:0] q_bpu_idx;
  logic [BTB_IDX_W-1:0] q_btb_idx;

  assign q_bpu_idx = q_pc[BPU_IDX_W:1];
  assign q_btb_idx = q_pc[BTB_IDX_W:1];

  // The non-power-of-two policy from the header: an index past the end of the
  // array is never allocated and never hits.
  //
  // The comparison is elaborated only when the depth actually truncates. For a
  // power-of-two depth the range check would be a tautology, and emitting a
  // comparison Verilator can prove constant is exactly the dead logic -Wall
  // exists to report, so that arm assigns a literal instead.
  logic q_bpu_idx_ok;
  logic q_btb_idx_ok;

  generate
    if ((BPU_ENTRIES == (1 << BPU_IDX_W)) && (BTB_ENTRIES == (1 << BTB_IDX_W))) begin : g_idx_never_truncates
      assign q_bpu_idx_ok = 1'b1;
      assign q_btb_idx_ok = 1'b1;
    end else begin : g_idx_may_truncate
      assign q_bpu_idx_ok = (q_bpu_idx < BPU_IDX_W'(BPU_ENTRIES));
      assign q_btb_idx_ok = (q_btb_idx < BTB_IDX_W'(BTB_ENTRIES));
    end
  endgenerate

  // ------------------------------------------------------ update index decode
  logic [BPU_IDX_W-1:0] u_bpu_idx;
  logic [BTB_IDX_W-1:0] u_btb_idx;

  assign u_bpu_idx = upd_pc[BPU_IDX_W:1];
  assign u_btb_idx = upd_pc[BTB_IDX_W:1];


  logic u_bpu_idx_ok;
  logic u_btb_idx_ok;

  // Same elaboration guard on the update side: an update whose index is past
  // the end of the array is dropped rather than wrapped.
  generate
    if ((BPU_ENTRIES == (1 << BPU_IDX_W)) && (BTB_ENTRIES == (1 << BTB_IDX_W))) begin : g_u_idx_never_truncates
      assign u_bpu_idx_ok = 1'b1;
      assign u_btb_idx_ok = 1'b1;
    end else begin : g_u_idx_may_truncate
      assign u_bpu_idx_ok = (u_bpu_idx < BPU_IDX_W'(BPU_ENTRIES));
      assign u_btb_idx_ok = (u_btb_idx < BTB_IDX_W'(BTB_ENTRIES));
    end
  endgenerate

  // ------------------------------------------------------------- BTB lookup
  // The kind is stored so a call's target can never be served for a
  // conditional branch at the same index, and vice versa. Without it, two PCs
  // that alias are distinguished only by tag, and a tag collision would hand
  // fetch a target of the wrong class.
  logic [1:0] q_kind;
  assign q_kind = q_is_branch ? KIND_BRANCH : KIND_JUMP;

  logic btb_tag_ok;
  logic btb_kind_ok;
  logic btb_hit;

  always_comb begin
    btb_tag_ok  = (btb_tag[q_btb_idx]  == q_pc);
    btb_kind_ok = (btb_kind[q_btb_idx] == q_kind);
`ifdef MOSAIC_PREDICTOR_MUTANT_NO_BTB_TAG
    // NEGATIVE CONTROL 1: the tag comparison is removed, so an index hit is
    // taken as a hit regardless of which PC is in the entry. Two branches that
    // collide in the index then share one target, and the second one is wrong
    // with no miss reported.
    btb_hit = btb_valid[q_btb_idx];
`else
    btb_hit = btb_valid[q_btb_idx] ? (btb_tag_ok && btb_kind_ok) : 1'b0;
`endif
    // An index past the end of the array is a miss whatever the entry holds.
    btb_hit = btb_hit && q_btb_idx_ok;
  end

  // ------------------------------------------------------ direction lookup
  // A 2-bit saturating counter, weakly taken when the msb is set. An entry with
  // no valid bit is not taken: an untrained table must never hand out an
  // optimistic guess, or the first fetch of every cold branch pays for it.
  logic bpu_entry_taken;

  always_comb begin
    bpu_entry_taken = (bpu_valid[q_bpu_idx] && q_bpu_idx_ok)
                    ? bpu_ctr[q_bpu_idx][1]
                    : 1'b0;
  end

  // --------------------------------------------------------------- RAS read
  logic ras_top_valid;
  logic [RAS_IDX_W-1:0] ras_rd_idx;

  assign ras_top_valid = (ras_sp != {RAS_SP_W{1'b0}});
  // Guard the subtraction so an empty stack never forms a negative entry
  // number: on an empty stack the read address is 0 and the value is discarded
  // by ras_top_valid anyway.
  assign ras_rd_idx = ras_top_valid ? RAS_IDX_W'(ras_sp - RAS_SP_W'(1))
                                    : {RAS_IDX_W{1'b0}};

  // --------------------------------------------------------- query results
  logic use_ras;

  always_comb begin
    // A return prefers the RAS. If the stack is empty the target falls through
    // to the BTB, which is a real second chance rather than a dead end.
    use_ras = q_valid && q_is_jump && q_is_return && ras_top_valid;

    pred_taken = q_valid && (q_is_branch || q_is_jump)
               ? (q_is_branch ? bpu_entry_taken : 1'b1)
               : 1'b0;

    pred_redirect = q_valid && (q_is_branch || q_is_jump)
                  && (use_ras || (btb_hit && (q_is_jump || bpu_entry_taken)));

    if (use_ras)      pred_target = ras_mem[ras_rd_idx];
    else if (btb_hit) pred_target = btb_target[q_btb_idx];
    // No target available: the sequential address. Always safe, and always
    // paired with pred_btb_miss below so it cannot be mistaken for a
    // prediction.
    else              pred_target = q_pc + XLEN'(4);

    pred_btb_hit   = btb_hit;
    pred_ras_valid = use_ras;

    // The report. A query that needed a target and did not get one says so,
    // including the case where the bimodal table said "taken" but the BTB had
    // nothing: pred_taken is then a direction with no address to go to, and
    // without this bit fetch could not tell that apart from a real prediction.
    pred_btb_miss = q_valid && (q_is_branch || q_is_jump)
                  && !use_ras && !btb_hit;

    // An empty RAS on a return is reported even when the BTB then supplies a
    // usable target, because the caller is tracking RAS anomalies for the
    // performance counters and a silent underflow would make MPKI wrong.
    pred_ras_underflow = q_valid && q_is_jump && q_is_return && !ras_top_valid;
  end

  // -------------------------------------------------------- update decoding
  logic upd_is_ctrl;
  logic upd_trains_direction;
  logic upd_writes_btb;
  logic upd_ras_push;
  logic upd_ras_pop;
  logic ras_is_full;
  logic ras_is_empty;

  assign ras_is_full  = (ras_sp == RAS_SP_W'(RAS_ENTRIES));
  assign ras_is_empty = (ras_sp == {RAS_SP_W{1'b0}});

  always_comb begin
    upd_is_ctrl          = upd_valid && (upd_is_branch || upd_is_jump);
    // A not-taken conditional branch still trains: its counter has to move down
    // for the table to follow a branch that changes behaviour.
    upd_trains_direction = upd_is_ctrl && upd_is_branch;
    // A not-taken branch has no interesting target, so it does not allocate.
    upd_writes_btb       = upd_is_ctrl && (upd_is_jump || upd_taken);
    upd_ras_push         = upd_is_ctrl && upd_is_call;
    upd_ras_pop          = upd_is_ctrl && upd_is_return;
  end

  // Edge reports, combinational, describing what this edge will do. Both are
  // suppressed by flush, because a flushed update does not happen at all and
  // reporting an anomaly for a discarded event would corrupt the counters.
  always_comb begin
`ifdef MOSAIC_PREDICTOR_MUTANT_NO_RAS_UNDERFLOW_REPORT
    // NEGATIVE CONTROL 5: a return on an empty stack does not report the
    // underflow. The pop is still dropped, so no wrong address is produced, but
    // the anomaly is invisible and the RAS-underflow counter reads zero.
    ras_overflow  = upd_ras_push && !flush && ras_is_full;
    ras_underflow = 1'b0;
`else
    ras_overflow  = upd_ras_push && !flush && ras_is_full;
    ras_underflow = upd_ras_pop  && !flush && ras_is_empty;
`endif
  end

  // ------------------------------------------------------------ next state
  // The counter's *first* training write is an absolute value, not an
  // increment. The array is never reset, so an untrained entry holds whatever
  // the silicon powered up with; incrementing that would make the first
  // prediction after reset depend on power-up contents rather than on the
  // resolved outcome. Writing 01/10 on the first observation instead makes the
  // first trained state a function of the branch alone, and every later update
  // is a saturating step from a value this module itself wrote.
  logic bpu_entry_is_valid;
  logic [1:0] bpu_ctr_next;

  always_comb begin
    bpu_entry_is_valid = bpu_valid[u_bpu_idx] && u_bpu_idx_ok;

    if (!bpu_entry_is_valid) begin
      // First observation: weakly not-taken or weakly taken, never saturated.
      bpu_ctr_next = upd_taken ? 2'b10 : 2'b01;
    end else if (upd_taken) begin
      bpu_ctr_next = (bpu_ctr[u_bpu_idx] == 2'b11) ? 2'b11
                                                   : (bpu_ctr[u_bpu_idx] + 2'd1);
    end else begin
      bpu_ctr_next = (bpu_ctr[u_bpu_idx] == 2'b00) ? 2'b00
                                                   : (bpu_ctr[u_bpu_idx] - 2'd1);
    end
  end

  // The store address for a RAS push is the entry the pointer currently names,
  // and only when the stack is not full. Derived from the same pointer the
  // push increments, so the stored word and the pointer can never disagree.
  logic ras_push_stores;
  logic [RAS_IDX_W-1:0] ras_push_idx;

  assign ras_push_stores = upd_ras_push && !flush && !ras_is_full;
  assign ras_push_idx    = RAS_IDX_W'(ras_sp);

  // RAS pointer movement for this edge.
  logic [RAS_SP_W-1:0] ras_sp_next;
  always_comb begin
`ifdef MOSAIC_PREDICTOR_MUTANT_NO_RAS_FLUSH
    // NEGATIVE CONTROL 2: a mispredict-driven flush is ignored, so a squashed
    // call's speculative push stays on the stack and every later return reads
    // one entry too deep.
    ras_sp_next = ras_sp;
    if (upd_ras_push && !ras_is_full)      ras_sp_next = ras_sp + RAS_SP_W'(1);
    else if (upd_ras_pop && !ras_is_empty) ras_sp_next = ras_sp - RAS_SP_W'(1);
`else
    if (flush)                             ras_sp_next = ras_ckpt;
    else if (upd_ras_push && !ras_is_full) ras_sp_next = ras_sp + RAS_SP_W'(1);
    else if (upd_ras_pop && !ras_is_empty) ras_sp_next = ras_sp - RAS_SP_W'(1);
    else                                   ras_sp_next = ras_sp;
`endif
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      // Control state only. Every data array above is deliberately untouched:
      // clearing them would turn DEPTH*WIDTH storage into resettable
      // flip-flops and is exactly what this project's RAM rule forbids.
`ifdef MOSAIC_PREDICTOR_MUTANT_NO_RESET_VALID
      // NEGATIVE CONTROL 4: reset clears the RAS pointers but forgets the valid
      // bits, so pre-reset BTB entries and trained counters survive and the
      // predictor is not in a defined state after reset.
      ras_sp   <= {RAS_SP_W{1'b0}};
      ras_ckpt <= {RAS_SP_W{1'b0}};
`else
      bpu_valid <= {BPU_ENTRIES{1'b0}};
      btb_valid <= {BTB_ENTRIES{1'b0}};
      ras_sp    <= {RAS_SP_W{1'b0}};
      ras_ckpt  <= {RAS_SP_W{1'b0}};
`endif
    end else begin
      // The checkpoint reads the pre-edge pointer, so a caller that wants a
      // transfer's RAS effect to be undoable asserts ckpt_valid in the same
      // cycle as that transfer's resolve.
      if (ckpt_valid) ras_ckpt <= ras_sp;

      ras_sp <= ras_sp_next;

      if (upd_trains_direction && u_bpu_idx_ok && !flush) begin
`ifdef MOSAIC_PREDICTOR_MUTANT_NO_TRAIN
        // NEGATIVE CONTROL 3: the direction table is never trained, so the
        // prediction never changes no matter what the branch does.
`else
        bpu_ctr[u_bpu_idx]    <= bpu_ctr_next;
        bpu_valid[u_bpu_idx] <= 1'b1;
`endif
      end

      if (upd_writes_btb && u_btb_idx_ok && !flush) begin
        btb_tag[u_btb_idx]    <= upd_pc;
        btb_target[u_btb_idx] <= upd_target;
        btb_kind[u_btb_idx]   <= upd_is_branch ? KIND_BRANCH : KIND_JUMP;
        btb_valid[u_btb_idx]  <= 1'b1;
      end

      // The RAS push needs the return address, which is the link value the
      // instruction writes. It is produced here from the resolved PC rather
      // than taken on a port, so the RAS cannot be pushed with an address that
      // disagrees with mosaic_branch_target's `link`.
      if (ras_push_stores) begin
        ras_mem[ras_push_idx] <= upd_pc + XLEN'(4);
      end
    end
  end

endmodule : mosaic_predictor

`resetall
`default_nettype wire
