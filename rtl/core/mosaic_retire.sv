// ============================================================================
// mosaic_retire -- work package I-017: in-order retirement, the committed-map
// commit, and the CSR side effects at the architectural boundary.
//
// Sized entirely from the generated configuration package. Nothing here names a
// width: MOSAIC_RETIRE_WIDTH / MOSAIC_INT_PRF_TAG_W / MOSAIC_ROB_ENTRIES /
// MOSAIC_XLEN are the geometry file's values, so a profile change changes the
// hardware and the testbench's shadow model together and there is no second copy
// to drift. There is deliberately no width parameter with an independent
// default: a caller-supplied default would be a second geometry.
//
// ------------------------------------------------- what this unit is allowed
//
// Retirement is the **only** place where speculative execution becomes
// architecturally visible, and this module is the only writer of the committed
// state. Four rules, and every line of behaviour below is one of them:
//
//   1. **In order, oldest first, up to MOSAIC_RETIRE_WIDTH per cycle.** Never a
//      later instruction to fill a free slot.
//   2. **Never over an instruction that is not retirable.** `rob_ready` is the
//      buffer's own `head_ready` (valid && complete && !exc && closed). It is
//      *consumed*, never recomputed here. A second definition of "done" inside
//      this file would be a second rule to drift from the first, and the drift
//      is silent: it shows up as an instruction retiring over an unfinished
//      one.
//   3. **The committed map moves exactly once per retired instruction, and only
//      there.** A squashed, killed or still-executing instruction never reaches
//      the commit port.
//   4. **An exceptional instruction traps; it never retires normally.** It
//      produces a trap event carrying `cause` and `tval`, no register write, no
//      CSR write, no store authorisation, and it drops itself and everything
//      younger out of the buffer in the same cycle.
//
// ------------------------------------------------------ cycle semantics
//
// Everything is combinational in the cycle it describes and registered at the
// end of it. The request offered in cycle N is answered in cycle N and its
// effect is visible in cycle N+1.
//
//   * `retire_req[i]` is the request to pop lane i. `rob_ack[i]` says whether
//     the buffer actually popped it, and **the events are driven from the ack,
//     not from the request**: what commits is what left the buffer, not what
//     was wished out of it.
//   * Lane i can only retire on top of lane i-1 in the same cycle. A pop
//     acknowledged for lane 1 without lane 0 is dropped and reported as
//     `o_order_fault`, because an unordered retire is the failure this unit
//     exists to prevent and it must be loud even if the buffer should never
//     produce it.
//   * A `flush_valid` cycle retires **nothing**. Recovery owns that cycle and
//     the buffer is being torn down; one bubble costs nothing and removes the
//     question of which of the two won.
//   * The trap path is the one exception to the flush rule: a trapping
//     instruction at the head is architecturally final, so it takes priority
//     over a recovery flush. Dropping it would execute straight past a fault.
//
// ------------------------------------------------- the boundary with I-018
//
// **This module holds no speculative state.** Its registers are `retire_seq`,
// `mcycle`, `minstret` and the CSR file, and every one of them is
// *architectural*: a squash has nothing here to undo, and a restore has nothing
// here to write. That is the whole contract between I-017 and I-018, and it is
// why:
//
//   * recovery (I-018) needs to restore **nothing into** this module, and this
//     module needs from recovery **exactly one rule**: a `flush_valid` cycle
//     retires nothing, which is enforced here rather than assumed;
//   * what this module offers recovery is *the committed state to restore to*:
//     `o_retire_seq`, the CSR file, and `trap_flush` -- the statement that
//     everything at and above a trapping entry has already gone, so recovery
//     never has to reason about a buffer that a trap half emptied.
//
// The converse is the other half of the same boundary: the speculative inputs
// this module reads (the buffer's head view, the execution payload) belong to
// I-018's world and are restored by rename's checkpoint, never by anything here.
//
// ------------------------------------------------------------- the CSR file
//
// I-017 owns the CSR *file* at the boundary, which is three registers: the two
// counters the card names (`mcycle`, `minstret`) and `mscratch`, a plain
// register with no side effect of its own, so "a CSR write becomes visible when
// the instruction retires" can be tested on something that is not also a
// counter and the two effects cannot be confused.
//
// The visibility rule is stated once and implemented once: **a CSR write is
// applied by the retirement of its instruction and by nothing else.** A write
// whose instruction is in the buffer but not complete is invisible; a write
// whose instruction is squashed never happens; and because every CSR read is a
// read of the pre-edge state, a write that retires in cycle N is first read in
// cycle N+1 -- visible from the cycle after retirement, never before it.
//
// An address outside the three registers is **reported**, not absorbed
// (`o_csr_unsupported`), and forwarded to I-019 by the event stream. Silently
// dropping a CSR write would leave a software-visible register that never
// changes, which is indistinguishable from a core that has no CSR unit at all.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_RETIRE_MUTANT_<n> injects exactly one defect to prove the unit test
// can detect it. The shipping build defines none of them; the table with real
// output is in results/reports/I-017-retire.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names a subset; the rest belong to other modules
// and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

// Widths are declared at file scope because a module's port list cannot see
// declarations inside its own body. They are derived from the generated package
// and nothing else.
localparam int unsigned RET_TAG_W = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;    // 7
localparam int unsigned RET_GEN_W = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;    // 7
localparam int unsigned RET_XLEN  = mosaic_cfg_pkg::MOSAIC_XLEN;             // 64
localparam int unsigned RET_ROB   = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;      // 64

// One lane minimum: `$clog2(1)` is zero bits and a zero-bit port is not a
// thing, so a profile that retired one instruction per cycle still elaborates a
// well-formed interface.
localparam int unsigned RET_WIDTH = (mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH <= 1)
                                    ? 1 : mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH;
localparam int unsigned RET_ID_W  = RET_TAG_W + RET_GEN_W;

// The event sequence number's width is the one the retire_event contract names:
// it must exceed twice the reorder-buffer depth, so a wrapped number is never
// confused with a live one. The counter itself is full width and monotonic.
localparam int unsigned RET_SEQ_W  = $clog2(2 * RET_ROB + 1);
localparam int unsigned RET_CNT_W  = $clog2(RET_WIDTH + 1);
localparam int unsigned RET_RD_W   = 5;
localparam int unsigned RET_CSR_W  = 12;
localparam int unsigned RET_SIZE_W = 3;

// The addresses this file implements, taken from config/csr/mode_m.json. They
// are written here rather than imported because the CSR unit (I-019) will
// implement these three plus the rest, and two copies of an address is a drift
// risk the tests below are the check for.
localparam logic [RET_CSR_W-1:0] CSR_MCYCLE   = 12'hB00;
localparam logic [RET_CSR_W-1:0] CSR_MINSTRET = 12'hB02;
localparam logic [RET_CSR_W-1:0] CSR_MSCRATCH = 12'h340;

module mosaic_retire (
    input  logic                            clk,
    input  logic                            rst,

    // ------------------------------------------------ the buffer's head view
    // One lane per retiring slot. Lane 0 is the head; lane i is the entry `i`
    // positions behind it. `rob_ready` is the buffer's own `head_ready`, handed
    // over rather than recomputed: this module defines nothing about what
    // "finished" means.
    input  logic [RET_WIDTH-1:0]            rob_valid,
    input  logic [RET_WIDTH-1:0]            rob_ready,
    input  logic [RET_WIDTH-1:0]            rob_exc,
    input  logic [RET_WIDTH-1:0]            rob_ack,
    input  logic [RET_WIDTH*RET_ID_W-1:0]   rob_id,    // {gen, tag}
    input  logic [RET_WIDTH*RET_XLEN-1:0]   rob_pc,

    // -------------------------------------- what the instruction did execute
    // The payload bus: what the retiring instruction's architectural effects
    // are. It is an input because the effects belong to execution; what this
    // module adds is *when* they may be believed. A payload belonging to an
    // instruction that is not retirable is not looked at, and a payload whose
    // instruction is squashed is discarded with it.
    input  logic [RET_WIDTH-1:0]            pay_valid,
    input  logic [RET_WIDTH-1:0]            pay_reg_we,
    input  logic [RET_WIDTH*RET_RD_W-1:0]   pay_rd,
    input  logic [RET_WIDTH*RET_XLEN-1:0]   pay_value,
    input  logic [RET_WIDTH-1:0]            pay_csr_we,
    input  logic [RET_WIDTH*RET_CSR_W-1:0]  pay_csr_addr,
    input  logic [RET_WIDTH*RET_XLEN-1:0]   pay_csr_value,
    input  logic [RET_WIDTH-1:0]            pay_is_store,
    input  logic [RET_WIDTH*RET_XLEN-1:0]   pay_store_addr,
    input  logic [RET_WIDTH*RET_XLEN-1:0]   pay_store_data,
    input  logic [RET_WIDTH*RET_SIZE_W-1:0] pay_store_size,
    input  logic [RET_WIDTH*RET_XLEN-1:0]   pay_exc_cause,
    input  logic [RET_WIDTH*RET_XLEN-1:0]   pay_exc_tval,

    // Recovery owns a flush cycle outright: nothing retires in it.
    input  logic                            flush_valid,

    // --------------------------------------------------------- to the buffer
    output logic [RET_WIDTH-1:0]            retire_req,
    // The trap path: the trapping entry and everything younger leave the buffer
    // in this cycle. This is the only statement I-017 makes about a squash.
    output logic                            trap_flush,

    // ---------------------------------------------------- the retire events
    // The architectural record, oldest lane first. Lane order *is* program
    // order; there is no sequence arbitration downstream to get wrong.
    output logic [RET_WIDTH-1:0]            ev_valid,
    output logic [RET_WIDTH-1:0]            ev_trap,
    output logic [RET_WIDTH*RET_SEQ_W-1:0]  ev_seq,
    output logic [RET_WIDTH*RET_XLEN-1:0]   ev_pc,
    output logic [RET_WIDTH*RET_ID_W-1:0]   ev_id,
    output logic [RET_WIDTH-1:0]            ev_reg_we,
    output logic [RET_WIDTH*RET_RD_W-1:0]   ev_rd,
    output logic [RET_WIDTH*RET_XLEN-1:0]   ev_value,
    output logic [RET_WIDTH-1:0]            ev_csr_we,
    output logic [RET_WIDTH*RET_CSR_W-1:0]  ev_csr_addr,
    output logic [RET_WIDTH*RET_XLEN-1:0]   ev_csr_value,
    output logic [RET_WIDTH-1:0]            ev_store,
    output logic [RET_WIDTH*RET_XLEN-1:0]   ev_store_addr,
    output logic [RET_WIDTH*RET_XLEN-1:0]   ev_store_data,
    output logic [RET_WIDTH*RET_SIZE_W-1:0] ev_store_size,
    output logic [RET_WIDTH*RET_XLEN-1:0]   ev_trap_cause,
    output logic [RET_WIDTH*RET_XLEN-1:0]   ev_trap_tval,

    // ---------------------------------------------------- trap handoff (I-019)
    output logic                            trap_valid,
    output logic [RET_XLEN-1:0]             trap_pc,
    output logic [RET_XLEN-1:0]             trap_cause,
    output logic [RET_XLEN-1:0]             trap_tval,

    // ------------------------------------------- committed-map commit (rename)
    // One lane per retired instruction that wrote an architectural register.
    // Lane order is program order, which is what makes two commits to the same
    // rd in one cycle install the younger mapping and release the older tag.
    output logic [RET_WIDTH-1:0]            commit_valid,
    output logic [RET_WIDTH*RET_RD_W-1:0]   commit_rd,
    output logic [RET_WIDTH*RET_TAG_W-1:0]  commit_tag,
    output logic [RET_WIDTH*RET_GEN_W-1:0]  commit_gen,

    // -------------------------------------------------------------- CSR reads
    // Combinational, and a read of the **pre-edge** state: a CSR write that
    // retires in cycle N is first visible in cycle N+1.
    input  logic                            csr_rd_valid,
    input  logic [RET_CSR_W-1:0]            csr_rd_addr,
    output logic [RET_XLEN-1:0]             csr_rd_data,
    output logic                            csr_rd_unsupported,

    // ------------------------------------------------------ counters, reports
    output logic [RET_XLEN-1:0]             o_retire_seq,
    output logic [RET_XLEN-1:0]             o_minstret,
    output logic [RET_XLEN-1:0]             o_mcycle,
    output logic [RET_WIDTH-1:0]            o_exc_queued,     // exceptional, not the trapping head
    output logic [RET_XLEN-1:0]             o_mscratch,
    output logic [RET_CNT_W-1:0]            o_event_count,
    output logic                            o_x0_retired,      // retired, wrote x0
    output logic [RET_WIDTH-1:0]            o_pay_missing,     // retirable, no payload
    output logic [RET_WIDTH-1:0]            o_csr_unsupported, // write to a CSR not here
    output logic                            o_order_fault      // unordered ack seen
);

  // ------------------------------------------------------------------- state
  // Every one of these is architectural. There is no speculative register in
  // this module, which is the whole of the I-017/I-018 contract; see the header.
  logic [RET_XLEN-1:0] retire_seq;
  logic [RET_XLEN-1:0] mcycle;
  logic [RET_XLEN-1:0] minstret;
  logic [RET_XLEN-1:0] mscratch;

  // ---------------------------------------------------------------- the trap
  // A trap is decided on the head alone and is not gated on `flush_valid`: an
  // exception that has reached the head is architecturally final, and a
  // recovery flush in the same cycle would otherwise drop the trapping
  // instruction and execute straight past the fault.
  logic head_trap;

  assign head_trap   = rob_valid[0] && rob_exc[0];
  assign trap_flush  = head_trap;

  assign trap_valid  = head_trap;
  assign trap_pc     = head_trap ? rob_pc[RET_XLEN-1:0] : {RET_XLEN{1'b0}};
  assign trap_cause  = head_trap ? pay_exc_cause[RET_XLEN-1:0] : {RET_XLEN{1'b0}};
  assign trap_tval   = head_trap ? pay_exc_tval[RET_XLEN-1:0]  : {RET_XLEN{1'b0}};

  // --------------------------------------------------------------- requests
  // A lane is requested when its entry exists, the buffer says it is retirable,
  // its payload is visible, and this cycle is not a recovery flush. Lane i
  // additionally requires lane i-1 to have been requested in the same cycle,
  // because "the entry behind the one we are popping" only means anything while
  // we really are popping the one in front of it.
  //
  // The three conditions that are not local to one lane -- the buffer's own
  // readiness, the order rule, and the recovery-flush rule -- are named once
  // here and used everywhere they apply, so a negative control below is a
  // one-line change rather than a second copy of the rule that could disagree
  // with the first.
  logic [RET_WIDTH-1:0] gate_ready;
  logic [RET_WIDTH-1:0] gate_flush_ok;

`ifdef MOSAIC_RETIRE_MUTANT_RETIRE_OVER_BLOCKED_HEAD
  // NEGATIVE CONTROL 1: the buffer's `head_ready` is not consulted. An entry
  // that is still executing, is not closed, or has squashed instructions under
  // it now retires, and the committed map moves for a result that does not
  // exist yet. This is the card's "execution completion mistaken for
  // retirement" and "a younger instruction steps over a pending fault" in one
  // edit, because in this module they are the same mistake.
  assign gate_ready = {RET_WIDTH{1'b1}};
`else
  assign gate_ready = rob_ready;
`endif

`ifdef MOSAIC_RETIRE_MUTANT_RETIRE_IN_FLUSH
  // NEGATIVE CONTROL 3: a recovery flush no longer owns its cycle, so an
  // instruction the redirect has just erased commits on the way out.
  assign gate_flush_ok = {RET_WIDTH{1'b1}};
`else
  assign gate_flush_ok = {RET_WIDTH{!flush_valid}};
`endif

  logic [RET_WIDTH-1:0] req_q;
  logic [RET_WIDTH-1:0] req_prior;

  // `order_ok` is the prefix AND written out once: lane i is requested only if
  // *every* lane in front of it was requested in this cycle, not merely if
  // lane i-1 exists. It is a separate signal from `req_q` so the negative
  // control below replaces the rule rather than the whole request expression.
  logic [RET_WIDTH-1:0] order_ok;

`ifdef MOSAIC_RETIRE_MUTANT_SECOND_LANE_UNORDERED
  // NEGATIVE CONTROL 2: the order rule is gone. A younger, complete entry is
  // requested -- and therefore committed -- over an older one that is still
  // blocked, which is "never retire a later entry to fill the width" broken.
  assign order_ok = {RET_WIDTH{1'b1}};
`else
  always_comb begin
    order_ok = '0;
    order_ok[0] = 1'b1;
    for (int unsigned i = 1; i < RET_WIDTH; i++) begin
      order_ok[i] = order_ok[i-1] && req_q[i-1];
    end
  end
`endif

  always_comb begin
    req_q     = '0;
    req_prior = '0;
    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      if (i == 0) begin
        req_q[0] = rob_valid[0] && gate_ready[0] && pay_valid[0] &&
                   gate_flush_ok[0] && !head_trap;
      end else begin
        // `req_prior` is the lane in front, and `order_ok` says that lane in
        // front was itself requested -- so the two lanes cannot race and lane i
        // is never popped on its own.
        req_prior[i] = req_q[i-1];
        req_q[i]     = order_ok[i] && rob_valid[i] && gate_ready[i] &&
                       pay_valid[i] && gate_flush_ok[i];
      end
    end
  end

  assign retire_req = req_q;

  // ------------------------------------------------------- what actually left
  // Events follow the acknowledgement, not the request: a request the buffer
  // refused would otherwise commit an instruction that is still in it.
  logic [RET_WIDTH-1:0] lane_retire;
  logic                 order_fault_q;

  always_comb begin
    lane_retire   = '0;
    order_fault_q = 1'b0;
    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      if (i == 0) begin
        lane_retire[0] = rob_ack[0];
      end else begin
        // An ack for lane i with no ack for lane i-1 is an out-of-order retire.
        // It is dropped and reported: out-of-order retirement is the failure
        // this module exists to prevent, and a silent drop would leave a
        // committed map nobody can account for.
        if (rob_ack[i] && !lane_retire[i-1]) begin
          order_fault_q = 1'b1;
        end else begin
          lane_retire[i] = rob_ack[i];
        end
      end
    end
  end

  assign o_order_fault = order_fault_q;

  // ------------------------------------------------ NEGATIVE CONTROL 4 (above)
  // "A trap emitted as an ordinary retire": the trapping entry takes the
  // ordinary retirement path, so its payload reaches the committed map and the
  // CSR file and its event carries no trap flag. That is the ABA hazard
  // config/contracts/interfaces.json records against the retire event -- a trap
  // emitted as a normal retire is a phantom instruction to a reference checker
  // -- and it is one condition, so it is one named signal.
  logic trap_as_retire;

`ifdef MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL
  assign trap_as_retire = 1'b1;
`else
  assign trap_as_retire = 1'b0;
`endif

  // The lanes whose architectural effects are applied this cycle: the lanes
  // that actually left the buffer, and -- only under NEGATIVE CONTROL 4 -- the
  // trapping lane as though it were an ordinary retirement.
  logic [RET_WIDTH-1:0] lane_effect;

  assign lane_effect = lane_retire | (head_trap ? {RET_WIDTH{trap_as_retire}}
                                                 : {RET_WIDTH{1'b0}});

  // ------------------------------------------------ NEGATIVE CONTROL 5 (above)
  // "Committed map updated on squash": the commit is taken from the payload bus
  // instead of from the retirement, so an instruction installs its mapping as
  // soon as its payload is on the wire -- while it is still in the buffer, and
  // therefore also when it is squashed out of it.
  logic [RET_WIDTH-1:0] gate_commit;

`ifdef MOSAIC_RETIRE_MUTANT_COMMIT_ON_SQUASH
  assign gate_commit = pay_valid;
`else
  assign gate_commit = lane_effect;
`endif

  // ------------------------------------------------- NEGATIVE CONTROL 6 (above)
  // "A CSR write visible before its instruction retires": the CSR file follows
  // the payload bus rather than the retirement.
  logic [RET_WIDTH-1:0] gate_csr;

`ifdef MOSAIC_RETIRE_MUTANT_CSR_AT_EXECUTE
  assign gate_csr = pay_valid;
`else
  assign gate_csr = lane_effect;
`endif


  // ----------------------------------------------------------------- events
  logic [RET_WIDTH-1:0] ev_valid_q;
  logic [RET_CNT_W-1:0] ev_count;

  always_comb begin
    // The trap is an event in its own right, and it occupies lane 0's slot:
    // the trap path drops everything younger, so there is nothing behind it.
    ev_valid_q      = lane_retire;
    ev_valid_q[0]   = head_trap || lane_retire[0];

    ev_count = RET_CNT_W'(0);
    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      ev_count = ev_count + RET_CNT_W'(ev_valid_q[i]);
    end
  end

  assign ev_valid       = ev_valid_q;
  assign o_event_count  = ev_count;

  // ----------------------------------------------- NEGATIVE CONTROL 7 (above)
  // "minstret counts executed rather than retired": the counter follows the
  // instructions sitting in the buffer instead of the events emitted, so a
  // squashed instruction and an instruction still in flight both count.
  logic [RET_XLEN-1:0] instret_delta;

`ifdef MOSAIC_RETIRE_MUTANT_INSTRET_COUNTS_EXEC
  assign instret_delta = RET_XLEN'(|rob_valid);
`else
  assign instret_delta = RET_XLEN'(ev_count);
`endif

  always_comb begin
    ev_trap       = '0;
    ev_trap[0]    = head_trap && !trap_as_retire;

    ev_seq        = '0;
    ev_pc         = '0;
    ev_id         = '0;
    ev_reg_we     = lane_effect & pay_reg_we;
    ev_rd         = '0;
    ev_value      = '0;
    ev_csr_we     = lane_effect & pay_csr_we;
    ev_csr_addr   = '0;
    ev_csr_value  = '0;
    ev_store      = lane_effect & pay_is_store;
    ev_store_addr = '0;
    ev_store_data = '0;
    ev_store_size = '0;
    ev_trap_cause = '0;
    ev_trap_tval  = '0;

    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      // Lane order is event order, so lane i's sequence number is the counter
      // plus i: the younger instruction of a two-wide retire carries the larger
      // number, which is the property a difftest checks first.
      ev_seq[i*RET_SEQ_W +: RET_SEQ_W] = retire_seq[RET_SEQ_W-1:0] + RET_SEQ_W'(i);

      if (ev_valid_q[i]) begin
        ev_pc[i*RET_XLEN +: RET_XLEN] = rob_pc[i*RET_XLEN +: RET_XLEN];
        ev_id[i*RET_ID_W  +: RET_ID_W]  = rob_id[i*RET_ID_W  +: RET_ID_W];
      end

      // Every payload field is gated on the lane actually having retired, so an
      // instruction that is present but not retired contributes nothing to any
      // field. That one gate is what makes "a squashed instruction never
      // touches the committed state" true by construction rather than by
      // review -- including the fields a squashed instruction's payload would
      // otherwise have supplied.
      if (lane_effect[i]) begin
        ev_rd[i*RET_RD_W +: RET_RD_W]             = pay_rd[i*RET_RD_W +: RET_RD_W];
        ev_value[i*RET_XLEN +: RET_XLEN]          = pay_value[i*RET_XLEN +: RET_XLEN];
        ev_csr_addr[i*RET_CSR_W +: RET_CSR_W]     = pay_csr_addr[i*RET_CSR_W +: RET_CSR_W];
        ev_csr_value[i*RET_XLEN +: RET_XLEN]      = pay_csr_value[i*RET_XLEN +: RET_XLEN];
        ev_store_addr[i*RET_XLEN +: RET_XLEN]     = pay_store_addr[i*RET_XLEN +: RET_XLEN];
        ev_store_data[i*RET_XLEN +: RET_XLEN]     = pay_store_data[i*RET_XLEN +: RET_XLEN];
        ev_store_size[i*RET_SIZE_W +: RET_SIZE_W] = pay_store_size[i*RET_SIZE_W +: RET_SIZE_W];
      end

      // A trap event carries the cause and the faulting value, and nothing
      // else: a trap is not a retire of an ordinary instruction, and the two
      // must never be confused by a reference checker reading the stream.
      if (ev_trap[i]) begin
        ev_trap_cause[i*RET_XLEN +: RET_XLEN] = pay_exc_cause[i*RET_XLEN +: RET_XLEN];
        ev_trap_tval[i*RET_XLEN  +: RET_XLEN]  = pay_exc_tval[i*RET_XLEN  +: RET_XLEN];
      end
    end
  end

  // -------------------------------------------------- the committed-map commit
  // One commit per retired instruction that wrote an architectural register,
  // and no other source of a commit exists: an instruction that is still
  // executing, one that was squashed, and one that traps all leave
  // `commit_valid` low. A write to x0 retires and commits nothing -- x0 is not
  // a physical register, so there is no mapping to install and no tag to
  // release -- but it is counted, because the instruction did retire.
  logic [RET_WIDTH-1:0] x0_retired_q;

  // Scratch used to name one lane's destination identity. It is declared outside
  // the block rather than inside the loop body because SystemVerilog requires
  // declarations at the top of a block, and a declaration that Verilator cannot
  // place is a declaration one of the two tools will reject.
  logic [RET_ID_W-1:0] commit_id;
  logic [RET_RD_W-1:0] commit_rd_scratch;

  always_comb begin
    commit_valid = '0;
    commit_rd    = '0;
    commit_tag   = '0;
    commit_gen   = '0;
    x0_retired_q = '0;

    commit_id       = '0;
    commit_rd_scratch = '0;

    // The destination identity is the one the entry carries, not one the
    // payload supplies: a payload that disagreed with the entry's own identity
    // would commit a mapping the instruction never allocated.
    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      commit_id         = rob_id[i*RET_ID_W +: RET_ID_W];
      commit_rd_scratch = pay_rd[i*RET_RD_W +: RET_RD_W];

      if (gate_commit[i] && pay_reg_we[i]) begin
        commit_rd[i*RET_RD_W +: RET_RD_W]    = commit_rd_scratch;
        commit_tag[i*RET_TAG_W +: RET_TAG_W] = commit_id[RET_TAG_W-1:0];
        commit_gen[i*RET_GEN_W +: RET_GEN_W] = commit_id[RET_ID_W-1 -: RET_GEN_W];

        if (commit_rd_scratch == {RET_RD_W{1'b0}}) begin
          x0_retired_q[i] = 1'b1;
        end else begin
          commit_valid[i] = 1'b1;
        end
      end
    end
  end

  assign o_x0_retired = |x0_retired_q;

  // --------------------------------------------------------------- reporting
  // A retirable entry whose payload has not arrived is neither an error nor a
  // stall forever: it is a one-cycle wait, and saying so is what lets a caller
  // distinguish "not ready yet" from "never coming".
  always_comb begin
    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      o_pay_missing[i] = rob_valid[i] && rob_ready[i] && !pay_valid[i] &&
                         (i == 0 ? !(flush_valid || head_trap) : req_prior[i]);
    end
  end

  // An exceptional entry behind the head blocks nothing -- it is waiting for
  // the head to clear, and it traps when it gets there. Reported so a stall on
  // a trapping instruction can be told apart from a stall on an ordinary one.
  always_comb begin
    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      o_exc_queued[i] = rob_exc[i] && !(i == 0 && head_trap);
    end
  end

  // A retiring CSR write to an address this file does not implement is reported
  // and still forwarded on the event stream. It is not applied here, and it is
  // not absorbed: those are different outcomes and only one of them is visible
  // to software.
  logic [RET_WIDTH-1:0] csr_is_mcycle_q;
  logic [RET_WIDTH-1:0] csr_is_minstret_q;
  logic [RET_WIDTH-1:0] csr_is_mscratch_q;

  always_comb begin
    csr_is_mcycle_q   = '0;
    csr_is_minstret_q = '0;
    csr_is_mscratch_q = '0;
    o_csr_unsupported = '0;

    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      if (gate_csr[i] && pay_csr_we[i]) begin
        case (pay_csr_addr[i*RET_CSR_W +: RET_CSR_W])
          CSR_MCYCLE:   begin csr_is_mcycle_q[i]   = 1'b1; end
          CSR_MINSTRET: begin csr_is_minstret_q[i] = 1'b1; end
          CSR_MSCRATCH: begin csr_is_mscratch_q[i] = 1'b1; end
          default:      begin o_csr_unsupported[i] = 1'b1; end
        endcase
      end
    end
  end

  // -------------------------------------------------------------- next state
  // One block, one copy of the pre-edge values, and the writes applied in a
  // stated order. One statement doing `minstret + 1` and another doing
  // `minstret = value` would make the result depend on the simulator's ordering
  // of two non-blocking assignments to the same element -- correct in one
  // simulator and not in another, which is exactly the class of defect this
  // testbench cannot see.
  logic [RET_XLEN-1:0] retire_seq_q;
  logic [RET_XLEN-1:0] mcycle_q;
  logic [RET_XLEN-1:0] minstret_q;
  logic [RET_XLEN-1:0] mscratch_q;

  always_comb begin
    // 1. The counters move first.
    //    `mcycle` counts core clock cycles, so it advances in every cycle out
    //    of reset, a recovery flush included: a cycle in which nothing retired
    //    was still a cycle.
    //    `minstret` counts retired instructions and nothing else: the traps
    //    included (a trapping instruction is architecturally final at its head,
    //    and the architectural record has to show it), and no squashed
    //    instruction, no instruction still in the buffer, and no cycle that
    //    retired nothing.
    retire_seq_q = retire_seq + RET_XLEN'(ev_count);
    mcycle_q     = mcycle + RET_XLEN'(1);
    minstret_q   = minstret + instret_delta;

    // 2. A CSR write *is* a retirement, and it lands after the increment. The
    //    reading is that a write to a counter leaves the value the instruction
    //    wrote, not that value plus the cycle the instruction retired in; the
    //    counter resumes counting from the next cycle.
    //    Two writes to one CSR in one cycle resolve oldest-first, so the
    //    younger instruction's value is the one that survives.
    mscratch_q = mscratch;
    for (int unsigned i = 0; i < RET_WIDTH; i++) begin
      if (csr_is_mcycle_q[i])   mcycle_q   = pay_csr_value[i*RET_XLEN +: RET_XLEN];
      if (csr_is_minstret_q[i]) minstret_q = pay_csr_value[i*RET_XLEN +: RET_XLEN];
      if (csr_is_mscratch_q[i]) mscratch_q = pay_csr_value[i*RET_XLEN +: RET_XLEN];
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      retire_seq <= {RET_XLEN{1'b0}};
      mcycle     <= {RET_XLEN{1'b0}};
      minstret   <= {RET_XLEN{1'b0}};
      mscratch   <= {RET_XLEN{1'b0}};
    end else begin
      retire_seq <= retire_seq_q;
      mcycle     <= mcycle_q;
      minstret   <= minstret_q;
      mscratch   <= mscratch_q;
    end
  end

  // ------------------------------------------------------------------ status
  assign o_retire_seq = retire_seq;
  assign o_minstret   = minstret;
  assign o_mcycle     = mcycle;
  assign o_mscratch   = mscratch;

  // -------------------------------------------------------------- CSR read port
  // Pre-edge state, gated on `csr_rd_valid` so an unaddressed read reports
  // nothing rather than reporting a default that looks like a real register.
  always_comb begin
    csr_rd_data        = {RET_XLEN{1'b0}};
    csr_rd_unsupported = 1'b0;
    if (csr_rd_valid) begin
      case (csr_rd_addr)
        CSR_MCYCLE:   csr_rd_data = mcycle;
        CSR_MINSTRET: csr_rd_data = minstret;
        CSR_MSCRATCH: csr_rd_data = mscratch;
        default:      csr_rd_unsupported = 1'b1;
      endcase
    end
  end

endmodule : mosaic_retire

`resetall
`default_nettype wire
