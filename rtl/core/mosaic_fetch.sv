// ============================================================================
// mosaic_fetch -- work package I-009.
//
// The instruction fetch request engine: a **bounded** table of outstanding
// fetch requests, an epoch that a redirect advances, and a response path that
// classifies every arriving response as live, stale or squashed before anything
// downstream is allowed to see it.
//
// ------------------------------------------------- the one rule that matters
//
// The credit rule, stated once and nowhere else in this file:
//
//     every issued request occupies exactly one request slot until exactly one
//     event returns its credit -- the response that accepts it, or the drop
//     that discards it -- and the slot is reusable only after that event.
//
// Two consequences follow directly and are the properties the testbench
// asserts every cycle:
//
//     issued == accepted + credit_returned_by_drops + outstanding
//     outstanding <= MOSAIC_FETCH_OUTSTANDING
//
// "Exactly one" is the whole difficulty. Not returning the credit at all leaks
// a slot and eventually stops fetch forever; returning it twice lets one
// response pay for two requests, which is how a live response for a
// *different* request ends up matched to a slot that has already been reused.
// Both look like "the wrong instruction reached the decoder", so the module
// makes each release a single, local, unambiguous event rather than a net
// somewhere else in the design.
//
// ------------------------------------------------- why the epoch exists, and
// ------------------------------------------------- why it is this wide
//
// A request id alone cannot survive a redirect. A redirect retires every
// in-flight request; if it also returned their credits, the slots would be
// immediately reusable and a late response for slot 2 would be
// indistinguishable from the response for whatever new request now holds slot 2.
// That is the first counterexample in config/contracts/interfaces.json, and it
// is an ABA bug: the id repeats, the request does not.
//
// So every request carries the epoch it was issued under, and every response
// echoes it back:
//
//     EPOCH_W = $clog2(MOSAIC_ROB_ENTRIES) + 1
//
// which is the `epoch` identity field of the `fetch_decode` contract
// (config/contracts/interfaces.json: `expr: clog2(rob_entries)+1`). For p0 that
// is $clog2(64)+1 = 7 bits, a modulus of 128.
//
// The width is justified against config/contracts/counters.json rather than
// picked. The epoch is a generation compared for **equality only** -- nothing
// ever asks which of two epochs is older -- so it is an `identity` counter and
// the binding rule is "modulus strictly exceeds the number of values that can
// be live at once". The live population here is at most one epoch per
// outstanding request plus the current epoch:
//
//     max_live = MOSAIC_FETCH_OUTSTANDING + 1 = 5      (p0)
//
// so a modulus of 8 would satisfy the arithmetic. 128 does, with the margin the
// contract's own `memory_transaction_generation` entry asks for on the shared
// id space: 2*(fetch_outstanding+lq_entries+sq_entries+mshrs)+1 = 45 with at most
// 21 live owners. tools/check_contracts.py proves those inequalities; the
// fetch epoch is the fetch-side instance of the same identity rule and is
// strictly wider than its own live set requires.
//
// The arithmetic bound is necessary but it is not the only defence, and this
// module does not rely on it alone. Each slot also carries a `cancelled` bit,
// set by the redirect that retired it and cleared only when its late response
// is dropped. A slot that has been through a redirect is therefore never
// matchable again, whatever the epoch counter happens to say -- so wrapping the
// counter cannot turn a squashed instruction into a delivered one. What the
// epoch still does, and what the `cancelled` bit cannot, is tell a *recycled*
// slot's late response (`stale`: the epoch has been superseded, no credit is
// owed because the slot has moved on) from a response for an id that was never
// outstanding in this epoch at all (`squashed`, a protocol error). Both are
// dropped; only the first still owes credit, and the two are counted apart so
// a protocol error is visible instead of invisible.
// ------------------------------------------------- the cancel policy, and what
// ------------------------------------------------- it costs
// A redirect does **not** return the credits of the requests it retires. It
// marks their slots cancelled, and each of those credits comes back when that
// request's late response arrives and is dropped. One event returns one credit,
// which is what makes the identity above a property of a single statement rather
// than of a balance that has to be maintained across two paths.
//
// The cost is real and is not hidden: after a redirect, fetch can be at the
// outstanding bound with every slot cancelled, and it cannot issue until the late
// responses drain -- up to MOSAIC_FETCH_OUTSTANDING response latencies of stall.
// That is the card's own trade ("bounded outstanding fetch, bandwidth sacrificed
// for recovery clarity"), and the fallback if a future responder cannot promise
// what this one does is the card's too: fall back to one fetch at a time.
//
// The precondition that makes the policy safe is therefore stated rather than
// assumed: **every issued request produces exactly one response**. It is the
// response rule of the fetch_decode contract, and without it a cancelled request
// would hold its credit forever. The alternative -- returning the credit at the
// redirect and refusing to return it again at the drop -- is the design this one
// deliberately is not, because then "returned exactly once" would have to be
// maintained across two statements and a late response would arrive at a slot
// that had already been handed to somebody else.
//
// ------------------------------------------------------ what is advisory
//
// `pred_next_pc` and `pred_squashed` are **hints**, and nothing else in this
// module's outputs is. They come from the owned mosaic_predictor instance and
// are consumed by whoever drives the request port. The rules that make a wrong
// hint harmless are:
//
//   * `pred_btb_miss` is the predictor's report that it needed a target and had
//     none. On a miss the next PC falls through to `pred_pc + 4`, because on a
//     miss `pred_target` *is* `pred_pc + 4` and treating it as a prediction
//     would be indistinguishable from a real one.
//   * `pred_squashed` pulses when a redirect in this cycle voids the hint. The
//     resolved target comes from execution; a wrong hint costs a squash.
//   * `redirect_valid` is authoritative and has no hint attached. It is the
//     only thing that advances the epoch, cancels slots or stops delivery.
//
// This module owns exactly one predictor instance per hart and forwards the
// predictor's update and RAS-recovery ports unchanged. It does not sniff
// instruction classes out of the encoding: `pred_is_branch` / `pred_is_jump` /
// `pred_is_return` are inputs, because mosaic_decoder (I-010) is the one decoder
// in this design and a second one here would be a second opinion about which
// encodings are control transfers.
//
// ------------------------------------------------- 16-bit instructions
//
// I-041 turns this module's old refusal into the correct handling, so the rules
// are stated here once:
//
//   * the instruction's *encoding* decides its length: `rsp_data[1:0] == 11` is
//     a 32-bit instruction and a length of 4 bytes; anything else is a 16-bit
//     compressed instruction and a length of 2. `rsp_len` is the memory's
//     report and is *checked* against that, not trusted: a 32-bit encoding
//     returned in two bytes is a malformed response and is reported illegal.
//   * `out_len` is the instruction's own length, 2 or 4. It is what the core
//     advances the program counter by and what the retire event records, so a
//     compressed instruction's PC is never "the previous PC plus four".
//   * `out_bits` is the instruction's own bits: for a 16-bit instruction the
//     upper half of the fetched word is *not* carried, because those bits are
//     the next instruction's encoding.
//   * `out_pc` is the PC recorded in the request slot, never an address derived
//     from the response payload, so a 32-bit instruction that straddled a fetch
//     line or a page is still attributed to the address it started at.
//
// The decompression itself is not here: the decoder (mosaic_decoder) expands a
// 16-bit encoding to its base-ISA equivalent, so the rest of the pipeline needs
// no knowledge of C. Fetch establishes only what the front end needs to move the
// PC by the right amount and what the event record needs to name the
// instruction.
//
// ------------------------------------------------------------------ faults
//
// `rsp_fault` reports an instruction access fault on that request. It consumes
// its slot exactly like a delivered instruction does (the credit is returned
// once) and is reported as `out_fault` with cause `mosaic_pkg::EXC_INSN_ACCESS`.
// It is not turned into an instruction and it does not become an architectural
// trap here: taking the trap is I-018's job, on the reported cause.
//
// ------------------------------------------------------------------ mutants
//
// `MOSAIC_FETCH_MUTANT_<n>` blocks are **off in the shipping build**. Each one
// injects exactly one defect named in work package I-009's acceptance criteria;
// the table with real output is in results/reports/I-009-fetch.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names three of them; the rest belong to other
// modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

// `mosaic_pkg` is included rather than imported-and-assumed-present for a
// concrete reason: the unit-test build for this work package compiles this
// file on its own, and an unresolved package reference is an elaboration error
// there rather than a warning. The package carries its own include guard, so
// including it here is a no-op when a build also passes the file on the command
// line -- which is what tools/lint_rtl.py does, and what the bringup case does.
//
// The alternative -- copying EXC_INSN_ACCESS and EXC_ILLEGAL_INSN into this
// file -- is rejected on purpose: a fault cause that fetch reports and the
// decoder expects is exactly the sort of constant that must have one home.
`include "mosaic_pkg.sv"

module mosaic_fetch #(
    // Depth overrides exist so a test can exercise a non-power-of-two geometry;
    // the defaults are the generated geometry and the shipping build uses them.
    parameter int unsigned XLEN             = mosaic_cfg_pkg::MOSAIC_XLEN,
    parameter int unsigned FETCH_OUTSTANDING = mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING,
    parameter int unsigned ROB_ENTRIES      = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES,
    // Derived widths, declared here because the port declarations below already
    // need them. None of them re-declares a MOSAIC_* knob: each is computed from
    // one, so the generated package stays the only source of the geometry, and
    // none is meant to be overridden on its own -- every use below derives from
    // the same expression, so a profile change moves all of them together.
    parameter int unsigned ID_W     = (FETCH_OUTSTANDING <= 1) ? 1 : $clog2(FETCH_OUTSTANDING),
    // The count has to name "full" as well as 0..FETCH_OUTSTANDING-1, so it is
    // one bit wider than an index.
    parameter int unsigned CNT_W    = $clog2(FETCH_OUTSTANDING + 1),
    // The contract's `epoch` identity field, derived exactly as the header
    // derives it. $clog2(1) is 0, which would make a zero-width port; the floor
    // of 2 is the contract's own `min_bits`.
    parameter int unsigned EPOCH_W = ((ROB_ENTRIES <= 1) ? 2 : $clog2(ROB_ENTRIES) + 1)
) (
    input  logic                     clk,
    input  logic                     rst,          // synchronous, active high

    // ------------------------------------------------------ request issue
    // `req_valid && req_ready` is one issued request. `req_id` and `req_epoch`
    // are valid in that cycle only and describe the request that was taken;
    // they are meaningless when `req_ready` is low, which is the refusal the
    // caller must honour rather than retry in place.
    input  logic                     req_valid,
    input  logic [XLEN-1:0]          req_pc,
    output logic                     req_ready,
    output logic [ID_W-1:0]          req_id,
    output logic [EPOCH_W-1:0]       req_epoch,

    // ------------------------------------------------------ response intake
    // Responses may arrive in any order. The payload must be stable while
    // `rsp_valid && !rsp_ready`, per the fetch_decode transfer rule.
    input  logic                     rsp_valid,
    output logic                     rsp_ready,
    // `rsp_squashed` pulses on a response that matched no outstanding request of
    // the current epoch: a protocol error, reported rather than swallowed.
    output logic                     rsp_squashed,
    input  logic [ID_W-1:0]          rsp_id,
    input  logic [EPOCH_W-1:0]       rsp_epoch,
    input  logic [31:0]              rsp_data,
    input  logic [2:0]               rsp_len,      // bytes of the instruction
    input  logic                     rsp_fault,

    // ----------------------------------------------------------- redirect
    // Authoritative. No hint accompanies it and nothing can delay it.
    input  logic                     redirect_valid,
    input  logic [XLEN-1:0]          redirect_pc,

    // ------------------------------------------ predictor query (advisory)
    input  logic                     pred_valid,
    input  logic [XLEN-1:0]          pred_pc,
    input  logic                     pred_is_branch,
    input  logic                     pred_is_jump,
    input  logic                     pred_is_return,

    // ------------------------------- predictor update and RAS recovery
    // Forwarded unchanged to the owned predictor instance. The core (I-018 /
    // I-021 integration) drives them; this module only owns the wiring, and
    // must not swallow a resolve that trains the predictor.
    input  logic                     upd_valid,
    input  logic [XLEN-1:0]          upd_pc,
    input  logic                     upd_is_branch,
    input  logic                     upd_is_jump,
    input  logic                     upd_is_call,
    input  logic                     upd_is_return,
    input  logic                     upd_is_taken,
    input  logic [XLEN-1:0]          upd_target,
    input  logic                     ckpt_valid,
    input  logic                     flush,

    // ------------------------------------------------- predictor reports
    // All advisory. `pred_next_pc` is the only one that is *derived* here: it
    // is the sequential fall-through on a reported miss, and the redirect is
    // squashed by `pred_squashed` rather than silently applied.
    output logic                     pred_next_valid,
    output logic [XLEN-1:0]          pred_next_pc,
    output logic                     pred_squashed,
    output logic                     pred_taken,
    output logic                     pred_btb_hit,
    output logic                     pred_btb_miss,
    output logic                     pred_ras_valid,
    output logic                     pred_ras_underflow,
    output logic                     ras_overflow,
    output logic                     ras_underflow,

    // ------------------------------------------- delivered instruction event
    // Exactly one of `out_valid` / `out_illegal` / `out_fault` is high when any
    // of them is, and the group holds its payload stable until `out_ready`.
    output logic                     out_valid,
    input  logic                     out_ready,
    output logic [XLEN-1:0]          out_pc,
    output logic [31:0]              out_bits,
    output logic [2:0]               out_len,
    output logic                     out_illegal,
    output logic                     out_fault,
    output logic [63:0]              out_cause,

    // ------------------------------------------------------- observability
    output logic [CNT_W-1:0]         outstanding_count,
    output logic [CNT_W-1:0]         cancel_pending,
    output logic [EPOCH_W-1:0]       epoch_now,
    output logic [31:0]              issued_count,
    output logic [31:0]              accept_count,      // live responses taken
    output logic [31:0]              drop_count,        // every non-live response
    output logic [31:0]              stale_drop_count,
    output logic [31:0]              squashed_drop_count,
    output logic [31:0]              credit_drop_count, // drops that returned credit
    output logic [31:0]              delivered_count,
    output logic [31:0]              fault_count,
    output logic [31:0]              illegal_count,
    output logic [31:0]              deny_count,        // issues refused, not dropped
    output logic [31:0]              cancel_count,
    output logic [XLEN-1:0]          fetch_pc,
    // ---------------------------------- the response being accepted this cycle
    // I-041: the sequential program counter is `the answered request's PC + the
    // answered instruction's own length`, and only this module knows that
    // length (it is a property of the encoding, which the encoding rule here
    // owns). `o_rsp_live` says a live response is being accepted at this edge
    // and `o_rsp_len` is its instruction's length in bytes, so the requester can
    // issue the *next* request in the same cycle instead of stalling a cycle
    // per instruction. Both are exactly the signals the output register below
    // is built from -- not a second derivation.
    output logic                     o_rsp_live,
    output logic [2:0]               o_rsp_len,
    // ------------------------------------------------ observation for a case
    // The response classification and the output register, so a case can say
    // *why* an instruction did or did not reach the decoder instead of only that
    // the retirement stream diverged. Purely an observation port.
    output logic [127:0]             o_dbg_state
);

  // --------------------------------------------------------------- geometry
  // Every package reference is fully qualified. mosaic_alu's header documents
  // that a module-scope `import mosaic_pkg::*` raises IMPORTSTAR on the strict
  // warning set, so there is no import here and nothing it could shadow. The
  // three widths the ports need are derived in the parameter port list, from the
  // generated package and nothing else.

  // What kind of thing was delivered on the output port. `OUT_INSN` is the
  // only kind a decoder may consume; the other two are reports that carry a
  // cause and nothing else.
  localparam logic [1:0] OUT_INSN    = 2'd1;
  localparam logic [1:0] OUT_ILLEGAL = 2'd2;
  localparam logic [1:0] OUT_FAULT   = 2'd3;

  logic [XLEN-1:0] pred_target_c;   // the predictor's raw advisory target
  logic            pred_redirect;   // the predictor's advisory "go here now"

  // ------------------------------------------------------- predictor: owned
  // One instance per hart, forwarded ports only. `pred_btb_miss` is the one
  // output this module cannot reconstruct on its own: on a miss the predictor
  // returns pc+4, which is bit-identical to a real fall-through target.
  mosaic_predictor #(
      .XLEN        (XLEN),
      .BPU_ENTRIES (mosaic_cfg_pkg::MOSAIC_BPU_ENTRIES),
      .BTB_ENTRIES (mosaic_cfg_pkg::MOSAIC_BTB_ENTRIES),
      .RAS_ENTRIES (mosaic_cfg_pkg::MOSAIC_RAS_ENTRIES)
  ) u_pred (
      .clk               (clk),
      .rst               (rst),

      .q_pc              (pred_pc),
      .q_valid           (pred_valid),
      .q_is_branch       (pred_is_branch),
      .q_is_jump         (pred_is_jump),
      .q_is_return       (pred_is_return),

      .upd_valid         (upd_valid),
      .upd_pc            (upd_pc),
      .upd_is_branch     (upd_is_branch),
      .upd_is_jump       (upd_is_jump),
      .upd_is_call       (upd_is_call),
      .upd_is_return     (upd_is_return),
      .upd_taken         (upd_is_taken),
      .upd_target        (upd_target),

      .ckpt_valid        (ckpt_valid),
      .flush             (flush),

      .pred_taken        (pred_taken),
      .pred_target       (pred_target_c),
      .pred_redirect     (pred_redirect),
      .pred_btb_hit      (pred_btb_hit),
      .pred_btb_miss     (pred_btb_miss),
      .pred_ras_valid    (pred_ras_valid),
      .pred_ras_underflow(pred_ras_underflow),

      .ras_overflow      (ras_overflow),
      .ras_underflow     (ras_underflow)
  );

  // Every report the predictor makes is forwarded to the caller, including the
  // two RAS edge reports: they describe an event at *this* edge, so a unit that
  // did not sample them would lose them. Fetch holds no state a RAS anomaly can
  // corrupt and takes no action on them -- they exist here so the core's MPKI
  // and RAS counters can see them, not so that fetch reacts.
  //
  // `pred_redirect` is the one report consumed locally, in the next-PC rule
  // below. Nothing the predictor says is swallowed by this module.

  // -------------------------------------------------------------- slot state
  // `slot_busy` is the credit. It is the only thing that says whether a slot
  // is occupied, and `outstanding_count` is derived from it, so the counter the
  // testbench watches cannot disagree with the state that enforces the bound.
  //
  // `slot_cancelled` marks a request that a redirect retired. The credit is
  // deliberately **not** returned at the redirect: it is returned when the
  // late response arrives and is dropped, which is what makes "returned exactly
  // once" checkable at a single place. The cost is stated in the report -- a
  // redirect does not instantly free slots, so fetch can stall for up to
  // MOSAIC_FETCH_OUTSTANDING response latencies -- and the benefit is that a
  // slot can never be handed to a new request while an old one is still
  // answering for it, which is the only way an ABA survives the epoch counter.
  logic [XLEN-1:0]        slot_pc      [FETCH_OUTSTANDING];
  logic [EPOCH_W-1:0]     slot_epoch   [FETCH_OUTSTANDING];
  logic [FETCH_OUTSTANDING-1:0] slot_busy;
  logic [FETCH_OUTSTANDING-1:0] slot_cancelled;

  logic [EPOCH_W-1:0]     epoch_q;      // the epoch in force *this* cycle
  logic [XLEN-1:0]        fetch_pc_q;

  // ------------------------------------------------------- response classify
  logic rsp_id_ok;
  logic rsp_slot_owns;   // this slot still holds the request this response is for
  logic rsp_live;        // ... and that request has not been retired
  logic rsp_stale;
  logic rsp_fire;
  logic rsp_retire;      // this response frees the slot it belongs to, exactly once
  logic rsp_release;     // ... and it did so by being dropped rather than accepted
  logic out_can_take;

  assign rsp_fire    = rsp_valid && rsp_ready;
  // One expression, one meaning: a response the slot owns ends that slot's
  // occupancy at this edge, whether it delivered an instruction or was dropped.
  // Delivering and dropping are two reasons to retire a request; they are not
  // two places where a credit is returned.
  assign rsp_retire  = rsp_fire && rsp_slot_owns;
  assign rsp_release = rsp_retire && !rsp_live;

  // The index range check only matters when FETCH_OUTSTANDING is not a power of
  // two. For a power of two the check folds to a constant, and a comparison that
  // the tool can prove constant is exactly the dead logic the strict warning set
  // exists to report, so that arm assigns a literal instead.
  generate
    if (FETCH_OUTSTANDING == (1 << ID_W)) begin : g_id_never_truncates
      assign rsp_id_ok = 1'b1;
    end else begin : g_id_may_truncate
      assign rsp_id_ok = (rsp_id < ID_W'(FETCH_OUTSTANDING));
    end
  endgenerate

  // "Owns" is deliberately *not* the same question as "is live". A redirect
  // retires a request without freeing its slot, so a slot cancelled a cycle ago
  // still owns the response that is on its way -- and that response is what has
  // to return the credit. Folding the cancelled bit into this test would drop
  // the response on the floor with the credit still held, and the request would
  // leak its slot forever. The two questions are therefore kept apart: ownership
  // decides *whose* credit this is, liveness decides *whether it is delivered*.
  assign rsp_slot_owns = rsp_id_ok && slot_busy[rsp_id] &&
                         (slot_epoch[rsp_id] == rsp_epoch);

  // A response that arrives on a redirect cycle is stale even though its epoch
  // is the current one: the redirect is retiring that epoch at this very edge.
  // Comparing against the *registered* epoch instead of the next one is the
  // mistake that lets a squashed instruction reach the decoder on the cycle the
  // redirect happens, which is exactly the cycle a mispredict test looks at.
  //
  // A cancelled slot is not live either, however current its epoch looks. That
  // is what makes the classification independent of the epoch counter wrapping:
  // once a request has been through a redirect, nothing that happens to the
  // counter can make it deliverable again.
`ifdef MOSAIC_FETCH_MUTANT_ACCEPT_STALE
  // NEGATIVE CONTROL 1: the response is accepted on the strength of the slot
  // alone. The cancelled bit, the epoch comparison and the redirect guard are
  // all gone, so a response from before the redirect is delivered at the old PC.
  assign rsp_live = rsp_slot_owns;
`else
  assign rsp_live = rsp_slot_owns && !slot_cancelled[rsp_id] && !redirect_valid &&
                    (rsp_epoch == epoch_now);
`endif

  // The epoch a redirect will install at this edge. A response is compared
  // against the epoch in force *now* and is separately refused on a redirect
  // cycle, which is the same thing said in a form that cannot be forgotten:
  // `epoch_next` is only ever a register input, never a comparison operand for
  // accepting an instruction.
  logic [EPOCH_W-1:0] epoch_next;
  assign epoch_next = epoch_q + EPOCH_W'(redirect_valid ? 1 : 0);
  assign epoch_now  = epoch_q;

  // The three classes partition every accepted response, and the partition is
  // what makes the credit exactly-once: a response that the slot owns always
  // pays that credit back (by delivering, or by being dropped), and a response
  // the slot does not own never pays anything, because that credit was already
  // returned when the slot was released or reused.
  assign rsp_stale    = !rsp_live && (rsp_slot_owns || (rsp_epoch != epoch_now));
  assign rsp_squashed = !rsp_live && !rsp_slot_owns && (rsp_epoch == epoch_now);

  // -------------------------------------------------------------- issue path
  // A slot that is free now, or is being freed by this cycle's response, can
  // take the new request: the credit returned on this edge is the credit spent
  // on it. That is why `req_ready` may be high while `outstanding_count` is at
  // the bound -- the machine is at its limit, not over it.
  logic [FETCH_OUTSTANDING-1:0] slot_free_after;
  logic free_found;
  logic [ID_W-1:0] alloc_id;

  for (genvar g = 0; g < int'(FETCH_OUTSTANDING); g++) begin : g_free
    assign slot_free_after[g] = !slot_busy[g] ||
                                (rsp_retire && (rsp_id == ID_W'(g)));
  end

  always_comb begin
    free_found = 1'b0;
    alloc_id   = '0;
    for (int unsigned i = 0; i < FETCH_OUTSTANDING; i++) begin
      if (!free_found && slot_free_after[i]) begin
        free_found = 1'b1;
        alloc_id   = ID_W'(i);
      end
    end
  end

  logic req_fire;
  assign req_fire = req_valid && req_ready;

`ifdef MOSAIC_FETCH_MUTANT_IGNORE_BOUND
  // NEGATIVE CONTROL 3: the bound is not enforced. Every slot is reported free
  // whatever the table holds, so the machine keeps accepting requests past
  // MOSAIC_FETCH_OUTSTANDING and overwrites the request in slot 0 rather than
  // refusing the issue.
  assign req_ready = !redirect_valid;
`else
  // A redirect refuses the issue port outright: the requester is about to be
  // told a different PC, and a request taken in the same cycle would be issued
  // under an epoch that is being retired at this edge. Refusing is reported
  // through `deny_count` and is visible as `req_ready` low, never silent.
  assign req_ready = !redirect_valid && free_found;
`endif

  assign req_id    = alloc_id;
  assign req_epoch = epoch_now;

  // ------------------------------------------------------------- output port
  // The delivered event lives in a register so the fetch_decode rule "payload
  // stable while valid && !ready" holds without the requester having to hold its
  // own response. A response is accepted (rsp_ready) only when the register can
  // take it, so nothing is ever silently overwritten.
  logic out_reg_valid;
  logic [1:0]      out_reg_kind;
  logic [XLEN-1:0] out_reg_pc;
  logic [31:0]     out_reg_bits;
  logic [2:0]      out_reg_len;
  logic [63:0]     out_reg_cause;

  assign out_can_take = !out_reg_valid || out_ready;
  assign rsp_ready    = out_can_take;

  assign out_valid   = out_reg_valid && (out_reg_kind == OUT_INSN);
  assign out_illegal = out_reg_valid && (out_reg_kind == OUT_ILLEGAL);
  assign out_fault   = out_reg_valid && (out_reg_kind == OUT_FAULT);
  assign out_pc      = out_reg_pc;
  assign out_bits    = out_reg_bits;
  assign out_len     = out_reg_len;
  assign out_cause   = out_reg_cause;
  assign fetch_pc    = fetch_pc_q;

  // ---------------------------------------------------- the instruction length
  // RISC-V states the length of an instruction in its own first two bits: 11 is
  // a 32-bit (or longer) encoding, anything else is a 16-bit compressed one.
  // Fetch therefore reads the *encoding*, not the byte count the memory
  // reported, and the reported count is checked against it: a 32-bit encoding
  // returned in two bytes is a malformed response, not an instruction with half
  // its bits missing, and it is refused as illegal rather than delivered.
  //
  // This is the change I-041 makes. Before it, a 16-bit encoding was reported
  // illegal here (the C extension was p1); the refusal is replaced by correct
  // handling, and the *length* and the *original bits* now travel with the
  // instruction so the retire event can carry them and the PC advance can use
  // them. Nothing is re-derived downstream from the encoding.
  logic rsp_is_16bit;
  logic rsp_is_32bit;
  assign rsp_is_16bit = (rsp_data[1:0] != 2'b11);
  assign rsp_is_32bit = !rsp_is_16bit && (rsp_len == 3'd4);

  // The instruction's own length in bytes, and its own bits: a 16-bit
  // instruction's upper half must not be carried along, because those bits
  // belong to the instruction *after* it in memory and an event record that
  // showed them would be showing a different instruction's encoding.
  logic [2:0]  rsp_insn_len;
  logic [31:0] rsp_insn_bits;
`ifdef MOSAIC_FETCH_MUTANT_DELIVER_16BIT
  // NEGATIVE CONTROL for I-041: a 16-bit instruction is handed on as though it
  // were a 32-bit one -- length four, the whole fetched word as its bits. The
  // decoder then reads the following instruction's bytes as this instruction's
  // upper half, the PC advances four bytes instead of two, and the event record
  // reports a length that is not the instruction's. CASE=compressed.cross_boundary
  // names the first of those (the length at the first compressed retirement),
  // and CASE=fetch.redirect_late_response names it at the delivery itself.
  assign rsp_insn_len  = 3'd4;
  assign rsp_insn_bits = rsp_data;
`else
  assign rsp_insn_len  = rsp_is_16bit ? 3'd2 : 3'd4;
  assign rsp_insn_bits = rsp_is_16bit ? {16'b0, rsp_data[15:0]} : rsp_data;
`endif

  logic rsp_is_insn;
  assign rsp_is_insn = rsp_is_16bit || rsp_is_32bit;

  logic [1:0] rsp_kind;
  assign rsp_kind = rsp_fault ? OUT_FAULT : (rsp_is_insn ? OUT_INSN : OUT_ILLEGAL);

  // The accepted response's own length, for the requester's sequential PC. Gated
  // on the response being live, so a stale or squashed response -- or one
  // arriving on a redirect cycle -- never contributes a length.
  assign o_rsp_live = rsp_fire && rsp_live;
  assign o_rsp_len  = rsp_insn_len;

  // ---------------------------------------------------------- predictor glue
  // The advisory next PC, and the only prediction this module acts on.
  //
  // The rule is the predictor header's: `pred_btb_miss` is the report that a
  // target was needed and none was available, and on that report the next PC
  // is the sequential `pred_pc + 4`. It is written as an explicit choice rather
  // than by trusting that `pred_target` happens to hold the same value, because
  // the two being equal is exactly what makes the miss indistinguishable from a
  // real fall-through prediction -- the miss bit is the only thing that says
  // which one it is.
  //
  // A prediction is a hint and the resolved target comes from execution, so a
  // wrong answer here costs a squash. What must not happen is a *silent* wrong
  // answer, which is why `pred_squashed` exists: a redirect in this cycle voids
  // the hint rather than letting it be applied and then contradicted.
  logic [XLEN-1:0] pred_next_pc_c;
  assign pred_squashed   = pred_valid && redirect_valid;
  assign pred_next_valid = pred_valid && !redirect_valid;
  assign pred_next_pc_c  = pred_btb_miss ? (pred_pc + XLEN'(4))
                            : (pred_redirect ? pred_target_c : (pred_pc + XLEN'(4)));
  assign pred_next_pc    = pred_next_pc_c;

  // ------------------------------------------------------------- counters
  logic [CNT_W-1:0] outstanding_count_c;
  logic [CNT_W-1:0] cancel_pending_c;

  always_comb begin
    int unsigned n_busy;
    int unsigned n_cancelled;
    n_busy      = 0;
    n_cancelled = 0;
    for (int unsigned i = 0; i < FETCH_OUTSTANDING; i++) begin
      if (slot_busy[i]) begin
        n_busy      = n_busy + 1;
        n_cancelled = n_cancelled + (slot_cancelled[i] ? 1 : 0);
      end
    end
    outstanding_count_c = CNT_W'(n_busy);
    cancel_pending_c    = CNT_W'(n_cancelled);
  end

  assign outstanding_count = outstanding_count_c;
  assign cancel_pending    = cancel_pending_c;

  // ------------------------------------------------------------ next state
  always_ff @(posedge clk) begin
    if (rst) begin
      // Control state only. The slot tables hold no data that has to be reset:
      // a slot's epoch and PC are meaningless without `slot_busy`, which is
      // what the reset contract in mosaic_ram.sv calls a valid vector.
      slot_busy      <= '0;
      slot_cancelled <= '0;
      epoch_q          <= '0;
      fetch_pc_q     <= '0;
      out_reg_valid  <= 1'b0;
      out_reg_kind   <= OUT_INSN;
      out_reg_pc     <= '0;
      out_reg_bits   <= '0;
      out_reg_len    <= 3'd0;
      out_reg_cause  <= '0;
      issued_count        <= 32'd0;
      accept_count        <= 32'd0;
      drop_count          <= 32'd0;
      stale_drop_count    <= 32'd0;
      squashed_drop_count <= 32'd0;
      credit_drop_count   <= 32'd0;
      delivered_count     <= 32'd0;
      fault_count         <= 32'd0;
      illegal_count       <= 32'd0;
      deny_count          <= 32'd0;
      cancel_count        <= 32'd0;
    end else begin
      // --------------------------------------------------------- the epoch
      if (redirect_valid) begin
        epoch_q <= epoch_next;
        // A redirect also names the PC to fetch from now on. The requester
        // owns the sequential stream; this is where a redirect hands the new
        // stream its starting point.
        fetch_pc_q <= redirect_pc;
      end

      // ------------------------------------------------------ issue accounting
      if (req_valid && !req_ready) begin
        // Reported, never silent: a refusal the caller cannot see is a lost
        // request that no counter will ever account for.
        deny_count <= deny_count + 32'd1;
      end
      if (req_fire) begin
        issued_count <= issued_count + 32'd1;
      end

      // ------------------------------------------------------ response path
      if (rsp_fire) begin
        if (rsp_live) begin
          accept_count <= accept_count + 32'd1;
          out_reg_valid <= 1'b1;
          out_reg_kind  <= rsp_kind;
          out_reg_pc    <= slot_pc[rsp_id];
          // The instruction's own bits and its own length -- never the fetch
          // window's byte count, and never a 16-bit instruction's upper half.
          out_reg_bits  <= rsp_insn_bits;
          out_reg_len   <= rsp_insn_len;
`ifdef MOSAIC_FETCH_MUTANT_FAULT_AS_INSN
          // NEGATIVE CONTROL 6: a *faulting* response is delivered as an
          // ordinary instruction -- no fault event, no cause, the fault bit
          // swallowed -- which is precisely how a core ends up executing at an
          // address that faulted. A response that did not fault is unaffected,
          // so the mutation is exactly the one defect and nothing else.
          if (rsp_fault) begin
            out_reg_kind  <= OUT_INSN;
            out_reg_cause <= 64'd0;
          end else begin
            out_reg_kind  <= rsp_kind;
            out_reg_cause <= rsp_is_insn ? 64'd0 : mosaic_pkg::EXC_ILLEGAL_INSN;
          end
`else
          out_reg_cause <= rsp_fault ? mosaic_pkg::EXC_INSN_ACCESS
                       : (rsp_is_insn ? 64'd0 : mosaic_pkg::EXC_ILLEGAL_INSN);
`endif
          if (rsp_kind == OUT_FAULT) begin
            fault_count <= fault_count + 32'd1;
          end else if (rsp_kind == OUT_ILLEGAL) begin
            illegal_count <= illegal_count + 32'd1;
          end else begin
            delivered_count <= delivered_count + 32'd1;
          end
        end else begin
          drop_count <= drop_count + 32'd1;
          if (rsp_stale) begin
            stale_drop_count <= stale_drop_count + 32'd1;
          end else begin
            squashed_drop_count <= squashed_drop_count + 32'd1;
          end
          // The credit is returned here, once, for a stale response that the
          // slot still owns. There is no other place in this file where a slot
          // is freed by a discarded response.
          if (rsp_release) begin
            credit_drop_count <= credit_drop_count + 32'd1;
          end
        end
      end

      // The output register's own lifecycle, written once and independently of
      // whether a response fired this cycle, because the two are independent
      // events:
      //
      //   * a live response installs a new instruction and must win over a
      //     drain in the same cycle (the consumer is taking *this* instruction);
      //   * a redirect retires whatever the register holds. That instruction
      //     was fetched after the branch the redirect came from, so it is
      //     wrong-path by construction, and delivering it afterwards would put
      //     it in front of the decoder as though it were on the correct path;
      //   * otherwise it drains when the consumer takes it.
      //
      // A dropped (non-live) response must not hold the register: it says
      // nothing about the instruction already in it, and folding the two into
      // one branch (the form this replaces) makes the register hold its
      // instruction for ever, re-delivering it every cycle the consumer is
      // ready. CASE=core.corpus_branch found exactly that, as an unbounded
      // stream of one wrong-path PC retiring.
`ifdef MOSAIC_FETCH_MUTANT_OUT_REG_DRAG
      // NEGATIVE CONTROL 9: a dropped response in the same cycle as a drain
      // suppresses the drain, so the instruction in the register is delivered
      // again and again. This is the re-delivery defect on its own, with the
      // redirect flush above intact.
      if (rsp_fire && !rsp_live) begin
        out_reg_valid <= out_reg_valid;
      end else if (rsp_fire && rsp_live) begin
        out_reg_valid <= 1'b1;
      end else if (redirect_valid || (out_reg_valid && out_ready)) begin
        out_reg_valid <= 1'b0;
      end
`elsif MOSAIC_FETCH_MUTANT_OUT_REG_NO_REDIRECT_FLUSH
      // NEGATIVE CONTROL 8: the redirect does not retire the instruction the
      // register holds, so a pre-redirect (wrong-path) instruction is delivered
      // to the decoder after the redirect. CASE=core.corpus_branch's
      // per-instruction comparison against the reference names it.
      if (rsp_fire && rsp_live) begin
        out_reg_valid <= 1'b1;
      end else if (out_reg_valid && out_ready) begin
        out_reg_valid <= 1'b0;
      end
`else
      if (rsp_fire && rsp_live) begin
        out_reg_valid <= 1'b1;
      end else if (redirect_valid || (out_reg_valid && out_ready)) begin
        out_reg_valid <= 1'b0;
      end
`endif

      // ---------------------------------------------------------- the slots
      // One release per slot per cycle, one issue per cycle, and the issue wins
      // when the two name the same slot: that is the credit returned by a
      // response being spent by the request accepted in the same cycle, and the
      // request must not be lost to the release that funded it.
      for (int unsigned i = 0; i < FETCH_OUTSTANDING; i++) begin
        if (req_fire && (alloc_id == ID_W'(i))) begin
          slot_busy[i]      <= 1'b1;
          slot_cancelled[i] <= 1'b0;
          slot_epoch[i]     <= epoch_q;
          slot_pc[i]        <= req_pc;
        end else if (rsp_retire && (rsp_id == ID_W'(i))) begin
          // An accepted response frees its slot here, exactly as a dropped one
          // does. This is the only statement in the file that frees a slot,
          // which is what makes "returned exactly once" a property of one line
          // rather than of an argument spread across two paths.
          slot_busy[i]      <= 1'b0;
          slot_cancelled[i] <= 1'b0;
        end else if (redirect_valid) begin
          // Everything still in flight is retired by this redirect and keeps
          // its credit until its late response is dropped above.
          slot_cancelled[i] <= slot_busy[i];
        end
      end

      if (redirect_valid) begin
        cancel_count <= cancel_count + {{(32 - CNT_W){1'b0}}, outstanding_count_c};
      end

`ifdef MOSAIC_FETCH_MUTANT_DOUBLE_CREDIT
      // NEGATIVE CONTROL 2: a stale drop returns a second credit, for a slot
      // that does not own the response. One drop, two slots freed: the ledger
      // no longer balances and a live request's slot can vanish underneath it.
      if (rsp_release && (rsp_id != ID_W'(0))) begin
        slot_busy[0]      <= 1'b0;
        slot_cancelled[0] <= 1'b0;
      end
`endif

`ifdef MOSAIC_FETCH_MUTANT_NO_EPOCH_ADVANCE
      // NEGATIVE CONTROL 4: a redirect does not advance the epoch. The per-slot
      // cancelled bit still stops the response being delivered, so what this
      // actually breaks is the contract itself -- two epochs now share a value,
      // and a request stamped with the pre-redirect epoch is indistinguishable
      // from one stamped after it.
      if (redirect_valid) begin
        epoch_q <= epoch_q;
      end
`endif

`ifdef MOSAIC_FETCH_MUTANT_CANCEL_RELEASES_CREDIT
      // NEGATIVE CONTROL 7: the redirect returns the credit for every slot it
      // cancels, instead of the credit being returned when the late response is
      // dropped. Nothing looks wrong at the redirect; the hole appears later,
      // when the response arrives and finds a slot that has already been reused.
      if (redirect_valid) begin
        for (int unsigned i = 0; i < FETCH_OUTSTANDING; i++) begin
          if (slot_busy[i]) begin
            slot_busy[i] <= 1'b0;
          end
        end
      end
`endif
    end
  end

  // ------------------------------------------------------------- observation
  always_comb begin
    o_dbg_state           = 128'd0;
    o_dbg_state[0]        = out_reg_valid;
    o_dbg_state[1]        = rsp_valid;
    o_dbg_state[2]        = rsp_ready;
    o_dbg_state[3]        = rsp_fire;
    o_dbg_state[4]        = rsp_live;
    o_dbg_state[5]        = rsp_stale;
    o_dbg_state[6]        = rsp_slot_owns;
    o_dbg_state[7]        = redirect_valid;
    o_dbg_state[11:8]     = 4'(rsp_id);
    o_dbg_state[18:12]    = rsp_epoch;
    o_dbg_state[25:19]    = epoch_now;
    o_dbg_state[26]       = req_ready;
    o_dbg_state[27]       = req_valid;
    o_dbg_state[28]       = req_fire;
    o_dbg_state[32:29]    = slot_busy;
    o_dbg_state[36:33]    = slot_cancelled;
    o_dbg_state[43:37]    = slot_epoch[0];
    o_dbg_state[44]       = rsp_kind[1];
    o_dbg_state[45]       = rsp_fault;
    o_dbg_state[46]       = rsp_is_32bit;
    o_dbg_state[63:47]    = 17'd0;
    o_dbg_state[95:64]    = out_reg_pc[31:0];
    o_dbg_state[127:96]   = rsp_data;
  end

endmodule : mosaic_fetch

`resetall
`default_nettype wire
