// ============================================================================
// mosaic_iq -- work package I-022: the local issue queue of one cluster, with
// oldest-ready arbitration to that cluster's local functional unit.
//
// Sized entirely from the generated configuration package. Nothing here names a
// depth: MOSAIC_IQ_ENTRIES, MOSAIC_XLEN, MOSAIC_INT_PRF_TAG_W,
// MOSAIC_MAX_UOPS_PER_MACRO and MOSAIC_ROB_ENTRIES are the geometry file's
// values, and they are read as `localparam`s rather than as parameters with
// defaults, because a parameter default is a second place for the geometry to
// be written down and a parameter a caller may override is a second geometry.
// There is deliberately no `MOSAIC_CLUSTERS` here either: this module is one
// cluster's queue and knows nothing about how many of them the fabric has.
//
// ------------------------------------------------- what one entry holds, why
//
// One entry per **uop**, not per macro-instruction: the local FU consumes one uop
// at a time and a macro that expands into three children occupies three slots,
// each with its own identity. An entry carries
//
//   * `uop_id`  {rob_index, rob_gen, uop_index}. The ROB index alone is a
//               wrapping pointer and cannot name an in-flight uop; the
//               generation is what makes a kill aimed at a recycled slot
//               rejectable instead of destructive.
//   * `alu_op`  mosaic_pkg::alu_op_e, the operation the local FU runs.
//   * `imm`     its immediate, so the FU needs no second lookup.
//   * two sources, each {tag, generation, ready, value}. An operand is named by
//               **(tag, generation)** and never by tag alone; see below.
//   * one destination, {tag, generation}: the single physical register this uop
//               will write. There is exactly one, which is what makes
//               exactly-once ownership checkable rather than aspirational.
//
// The queue does not track *how many* sources a uop has. A source that does not
// exist is presented with its ready bit already set. That keeps one readiness
// rule in one place -- "ready" always means "this entry's slot holds the final
// value of that operand" -- so a zero-source ALU or a branch needs no second
// case in the selection logic and cannot be mis-selected by one.
//
// -------------------------------------------------------- the generation bit
//
// A physical tag is recycled: with MOSAIC_INT_PRF_ENTRIES = 96 the tag is 7 bits
// and names a different value before long. So a source is named by
// (tag, generation), and a broadcast is accepted for an entry only when *both*
// fields match. A broadcast whose tag matches but whose generation does not is
// a broadcast for a *previous* occupant of that physical register; applying it
// would install a stale value into a live uop, which is a wrong result, not a
// lost packet. It is rejected and the rejection is **counted** (`o_wu_stale`)
// rather than absorbed, so "the stale response was rejected" is an observation
// and not a claim.
//
// The generation width is derived rather than chosen. A broadcast is raised only
// after the value is durably written, and a tag cannot be recycled until its
// previous owner has retired, so a broadcast can be in flight for at most as
// many recycles of one tag as there are instructions in flight -- bounded by
// MOSAIC_ROB_ENTRIES. Hence the width must satisfy
//
//     2**TAG_GEN_W > MOSAIC_ROB_ENTRIES          (128 > 64 for p0)
//
// and the same width sizes the ROB generation inside the uop identity, so one
// number carries both meanings and there is nothing to keep in step.
//
// -------------------------------------------------------------------- ages
//
// Age is a wrapping counter, and a wrapping counter is only a usable total order
// if the true distance between any two live entries stays below half the
// modulus. The plan makes that explicit -- "the modulus of the scheduling age
// must exceed twice the largest distance two live entries can be separated by,
// and a modulus *equal* to that bound is a defect" -- so the modulus is derived
// from the depth and the inequality is written out here:
//
//     largest distance between two live entries = DEPTH - 1 = 7
//         (a queue of DEPTH entries holds DEPTH ages, so the extremes of a full
//          queue are DEPTH - 1 apart -- see the invariant below for why that
//          cannot grow)
//     required:  MOD > 2 * (DEPTH - 1)  =  14
//     chosen:    MOD = 2**AGE_W = 2**$clog2(4*DEPTH) = 2**5 = 32  >  14
//
// The bound is **structural**, not an assumption about the workload:
//
//   INVARIANT.  At every cycle the live entries' ages are exactly a contiguous
//               window {B, B+1, ..., B+n-1} modulo the modulus, for some base B
//               and some n <= DEPTH, and `age_ctr` is B + n -- the age the next
//               insertion takes. All arithmetic is modulo the modulus.
//
//   * An insertion appends at the top: it takes `age_ctr`, and `age_ctr` moves up
//     by one.
//   * A removal closes the hole it left: every surviving entry *younger* than a
//     removed entry steps down by one per removed entry below it, so the
//     survivors are again contiguous and the window does not grow.
//   * When the removed entry *was* the base, the survivors are **not** stepped
//     down for it: the window is allowed to slide up instead. That is the whole
//     mechanism by which the counter wraps, and it is the difference between an
//     age field that never wraps (and so never exercises the modular
//     comparison) and one that does.
//
// So the live ages stay inside a window of at most DEPTH values however long the
// machine runs and however badly the removals interleave, and the maximum true
// distance the modular comparison ever has to resolve is DEPTH - 1. That is what
// makes the inequality above a proof rather than a hope. A design that let the
// window grow -- by leaving a hole unfilled, which is the tempting "just let
// `age_ctr` free-run" simplification -- has an *unbounded* separation between
// two long-lived entries and no width saves it. That is the failure the bound
// is about, and it is what `MOSAIC_IQ_MUTANT_NARROW_AGE` is sized against: at a
// modulus of 8 a full window of 8 spans 7, which is more than half the modulus,
// so the extremes of a full queue are no longer ordered and the oldest entry
// stops being recognisable as the oldest.
//
// The window slides whenever the oldest entry leaves, so a queue that is
// issued steadily walks its window right around the modulus: the counter wraps
// after 32 grants for p0, and the unit test drives it across the wrap with a
// full queue live and checks that the selection is still the oldest one.
//
// ----------------------------------------------------------- the four ports
//
// insert  one uop per cycle from this cluster's dispatch. `ins_ready` is a
//         function of the registered occupancy alone: it is 0 exactly when the
//         queue is full. It does **not** consult `grant_ready`, so no
//         combinational path runs from the functional unit back to dispatch, and
//         a full queue costs one cycle of dispatch even in the cycle it drains.
//         The producer holds the payload while `ins_valid && !ins_ready`, which
//         is the rule every other channel in this tree follows.
//
// wakeup  a per-cycle **broadcast** of one durable result. Durable means the
//         value is written before the broadcast is raised, so an operand that
//         misses this cycle's broadcast is not lost: a uop dispatched later
//         takes the ready bit and the value from the operand collector, not from
//         this port. Nothing here is a speculative ready.
//
// grant   one issue per cycle to the local FU. `grant_valid` holds the chosen
//         entry and its payload is stable until the FU accepts it, and the entry
//         leaves the queue on the accepting edge and not before it. "An entry
//         must not disappear until the FU has taken it" is the card's blocking
//         rule; the per-slot `granted` bit is the register that enforces it,
//         because a granted entry is excluded from selection while it is
//         outstanding, so it can be neither lost nor issued twice. The single
//         documented exception is a kill, below.
//
// kill    removes the named macro's uops and, with `kill_younger`, every entry
//         younger than the oldest of them. Kill has priority over an
//         outstanding grant: a squashed uop must not issue, so a kill naming
//         the entry currently presented withdraws the grant in that same cycle.
//         The alternative -- let the FU accept a dead uop -- is the only way to
//         "issue a uop that is not live", which the card blocks. A consumer must
//         therefore tolerate a grant withdrawn by a simultaneous flush; a flush
//         is a global event it is handling anyway.
//
// A kill whose named macro is not in the queue removes nothing. That is the
// safe direction: a kill is a subtraction from what is resident, and a kill that
// matches nothing cannot have been meant to remove something that arrived after
// it. I-016's flush and this port are the two halves of one contract, and the
// integration owner wires them so that a squashed macro's uops are still in
// their queues or have already been granted.
//
// ------------------------------------------------------ same-cycle wakeup
//
// An entry inserted this cycle whose operand is produced this cycle is
// selectable this cycle, and the value it presents is the broadcast one rather
// than whatever the insert bus happened to carry. Both halves matter: readiness
// and payload. The offered entry is always the *youngest* -- its age is the top
// of the window by the invariant above -- so it is considered only when no
// resident entry is eligible, which is the correct oldest-ready answer. It is
// also why a naive "a same-cycle producer jumps the queue" rule is wrong: the
// producer is *older* than the consumer it feeds, so the consumer must not
// overtake it, and a directed test asserts exactly that ordering.
//
// ------------------------------------------------------- reset and storage
//
// `rst` is synchronous and active high. It clears the valid vector, the granted
// vector, the occupancy, the allocation pointer, the age counter and the
// counters. It clears **nothing else**: tags, generations, values, immediates
// and ages are entry data, and clearing DEPTH of them would give every entry a
// full reset, which is the rule rtl/common/mosaic_ram.sv documents against.
// Validity is held outside the entry arrays in an explicit packed vector,
// exactly as the RAM's worked example prescribes.
//
// The entry arrays are register state, not RAM. A slot is written by an
// insertion, written again by any number of broadcasts, and read by the grant,
// by the kill matcher and by the observation port in arbitrary order: that is
// multi-port register state, so these reads are combinational reads of
// registers, which is what the silicon does. This module is not "a RAM with a
// port bolted on" and does not claim to behave like one.
//
// Because reset is synchronous, the outputs still show the pre-reset state
// during the reset cycle itself and are clean from the first cycle after the
// reset edge. `ins_ready` and `grant_valid` are forced low while `rst` is high,
// so a transfer cannot be *reported* during a reset cycle: a producer that sees
// `ins_ready` low keeps its offer and the offer is taken the first cycle after
// reset.
//
// -------------------------------------------------------- exactly-once, etc.
//
// The invariants are exported rather than asserted in prose:
//
//     o_ins_total == o_grant_total + o_kill_total + o_count
//
// Every inserted uop leaves the queue exactly once: granted, killed, or still
// held. `o_wu_*` classify every broadcast into exactly one of matched / dup /
// stale / miss, so no broadcast is silently absorbed and no duplicate producer
// overwrites a value that is already there. `o_dst_conflict` is raised when two
// live entries name the same destination (tag, generation) -- one result, one
// destination, and a second producer for a destination is *reported*, not merged
// into the first, because two uops writing one physical register version is not
// a race to arbitrate but a producer that must not exist.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_IQ_MUTANT_<n> injects one defect used to prove the case can fail.
// The shipping build defines none of them; the table with real output is in
// results/reports/I-022-iq.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names five of them; the rest belong to other
// modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
// The age width, chosen by a macro rather than by `ifdef inside the parameter
// port list, which Verilator 5.052 does not parse. Both branches read the same
// generated constant, so the geometry still has exactly one home.
  // 2**AGE_W = 4*DEPTH = 32 > 2*(DEPTH-1) = 14. See the header for the
  // derivation, for why the bound is structural rather than a hope about the
  // workload, and for the elaboration guard below that rejects a width which does
  // not meet it.
  `define MOSAIC_IQ_AGE_W $clog2(4 * mosaic_cfg_pkg::MOSAIC_IQ_ENTRIES)


module mosaic_iq #(
  // Geometry, from the generated package only. `localparam` and not `parameter`
  // on purpose: see the note above the port list.
  localparam int unsigned DEPTH       = mosaic_cfg_pkg::MOSAIC_IQ_ENTRIES,
  localparam int unsigned XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN,
  localparam int unsigned TAG_W       = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W,
  localparam int unsigned ROB_INDEX_W = mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W,
  localparam int unsigned MAX_UOPS    = mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO,

  // The generation carried by every tag and by the uop identity. Derived, with
  // the bound written out in the header: 2**TAG_GEN_W must exceed the number of
  // instructions that can be in flight.
  localparam int unsigned TAG_GEN_W   =
      $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1,
  localparam int unsigned ROB_GEN_W   = TAG_GEN_W,
  localparam int unsigned UOP_W       = (MAX_UOPS <= 1) ? 1 : $clog2(MAX_UOPS),

  // See the `define above the module for the mutant and for the derivation.
  localparam int unsigned AGE_W        = `MOSAIC_IQ_AGE_W,

  // Derived, and therefore not overridable. The age modulus is written out in
  // the guard below rather than named here, so there is no second place for it.
  localparam int unsigned IDX_W    = (DEPTH <= 1) ? 1 : $clog2(DEPTH),
  localparam int unsigned CNT_W    = $clog2(DEPTH + 1),
  localparam int unsigned UOP_ID_W = ROB_INDEX_W + ROB_GEN_W + UOP_W
) (
  input  logic                     clk,
  input  logic                     rst,

  // ------------------------------------------------------------------ insert
  input  logic                     ins_valid,
  output logic                     ins_ready,
  input  logic [UOP_ID_W-1:0]      ins_uop,
  input  logic [3:0]               ins_alu_op,      // mosaic_pkg::alu_op_e
  input  logic [XLEN-1:0]          ins_imm,
  input  logic [TAG_W-1:0]         ins_src1_tag,
  input  logic [TAG_GEN_W-1:0]     ins_src1_gen,
  input  logic                     ins_src1_ready,
  input  logic [XLEN-1:0]          ins_src1_val,
  input  logic [TAG_W-1:0]         ins_src2_tag,
  input  logic [TAG_GEN_W-1:0]     ins_src2_gen,
  input  logic                     ins_src2_ready,
  input  logic [XLEN-1:0]          ins_src2_val,
  input  logic [TAG_W-1:0]         ins_dst_tag,
  input  logic [TAG_GEN_W-1:0]     ins_dst_gen,

  // ------------------------------------------------------------------ wakeup
  // One durable result, broadcast to every entry. Accepted for an entry only
  // when the tag *and* the generation match and the operand is not ready
  // already.
  input  logic                     wu_valid,
  input  logic [TAG_W-1:0]         wu_tag,
  input  logic [TAG_GEN_W-1:0]     wu_gen,
  input  logic [XLEN-1:0]          wu_val,

  // ------------------------------------------------------------------- grant
  output logic                     grant_valid,
  input  logic                     grant_ready,
  output logic [UOP_ID_W-1:0]      grant_uop,
  output logic [3:0]               grant_alu_op,
  output logic [XLEN-1:0]          grant_imm,
  output logic [XLEN-1:0]          grant_a,
  output logic [XLEN-1:0]          grant_b,
  output logic [TAG_W-1:0]         grant_dst_tag,
  output logic [TAG_GEN_W-1:0]     grant_dst_gen,
  output logic [IDX_W-1:0]         grant_index,     // slot presented; see below

  // -------------------------------------------------------------------- kill
  input  logic                     kill_valid,
  input  logic [ROB_INDEX_W-1:0]   kill_rob_index,
  input  logic [ROB_GEN_W-1:0]     kill_rob_gen,
  input  logic                     kill_younger,    // also drop younger entries

  // ------------------------------------------------------------------ status
  output logic [DEPTH-1:0]           o_occupied,      // one bit per slot

  output logic [CNT_W-1:0]         o_count,
  output logic                     o_full,
  output logic                     o_dst_conflict,  // two live entries, one dst
  output logic [AGE_W-1:0]         o_age_ctr,       // age the next insertion takes
  output logic [IDX_W-1:0]         o_alloc_index,   // slot an insert would take

  // ------------------------------------------------------------- observation
  // A combinational view of one slot by index; the unit test sweeps it over
  // every slot every cycle. A functional consumer has no business reading it:
  // the grant, the status words and the counters are the queue's interface and
  // this is a verification and debug port. Every field is a don't-care when
  // `obs_valid` is 0, because the slot holds no entry.
  input  logic [IDX_W-1:0]         obs_index,
  output logic                     obs_valid,
  output logic [AGE_W-1:0]         obs_age,
  output logic                     obs_ready,       // both sources ready as stored
  output logic                     obs_granted,     // presented, not yet accepted
  output logic [TAG_W-1:0]         obs_src1_tag,
  output logic [TAG_GEN_W-1:0]     obs_src1_gen,
  output logic [TAG_W-1:0]         obs_src2_tag,
  output logic [TAG_GEN_W-1:0]     obs_src2_gen,
  output logic [UOP_ID_W-1:0]      obs_uop,
  output logic [3:0]               obs_alu_op,
  output logic [XLEN-1:0]          obs_imm,
  output logic [TAG_W-1:0]         obs_dst_tag,
  output logic [TAG_GEN_W-1:0]     obs_dst_gen,
  output logic                     obs_src1_ready,
  output logic                     obs_src2_ready,
  output logic [XLEN-1:0]          obs_src1_val,
  output logic [XLEN-1:0]          obs_src2_val,

  // ---------------------------------------------------------------- counters
  output logic [31:0]              o_ins_total,
  output logic [31:0]              o_grant_total,
  output logic [31:0]              o_kill_total,
  output logic [31:0]              o_wu_total,
  output logic [31:0]              o_wu_matched,
  output logic [31:0]              o_wu_dup,
  output logic [31:0]              o_wu_stale,
  output logic [31:0]              o_wu_miss
);

  // ------------------------------------------------------------------ guards
  // The age modulus rule as an elaboration error rather than a comment. A width
  // at or below 2*(DEPTH-1) is the defect the plan calls out: the extremes of a
  // full window are then at or beyond half the modulus apart and the modular
  // comparison cannot order them.
  localparam bit AGE_MOD_TOO_SMALL = ((1 << AGE_W) <= 2 * (DEPTH - 1));

  // $clog2 can name indices past a smaller queue, so an entry count that does
  // not fill the index space is allowed; every index is range-checked before it
  // reaches an array. For the shipping geometry the check is a tautology, so it
  // is not elaborated at all: emitting a comparison Verilator can prove constant
  // is exactly the dead logic -Wall exists to report.
  localparam bit IDX_NEVER_TRUNCATES = (DEPTH == (1 << IDX_W));

  logic [IDX_W-1:0] obs_index_s;

  generate
    if (IDX_NEVER_TRUNCATES) begin : g_idx_never_truncates
      assign obs_index_s = obs_index;
    end else begin : g_idx_may_truncate
      always_comb begin
        obs_index_s = (obs_index < IDX_W'(DEPTH)) ? obs_index : {IDX_W{1'b0}};
      end
    end

    if (AGE_MOD_TOO_SMALL) begin : g_bad_age_modulus
      mosaic_iq_contract_violation u_age_modulus();
    end
    if (DEPTH < 2) begin : g_bad_depth
      mosaic_iq_contract_violation u_depth();
    end
    if (MAX_UOPS < 1) begin : g_bad_uops
      mosaic_iq_contract_violation u_uops();
    end
  endgenerate

  // --------------------------------------------------------------- helpers
  // "a is strictly older than b" in the wrapping age space.
  //
  // The forward distance from a to b is `b - a` modulo the modulus, and that is
  // a real age exactly when it is non-zero. So a is older than b when the
  // *other* difference, `a - b`, is non-zero and has its top bit set: a real
  // age d has `a - b = M - d`, which lands in the upper half of the modulus
  // for every d below M/2, and wraps to the lower half for every d above it.
  //
  // "Below M/2" is exactly what the modulus sizing in the header buys, and the
  // window invariant is what keeps every live pair inside it. A full window at
  // exactly M/2 is the ambiguous case the guard above rules out. Written the
  // other way round -- testing the top bit is clear -- the comparison is exactly
  // inverted, and an entry one step *younger* than its neighbour reads as the
  // older one.
  function automatic logic IsOlder(input logic [AGE_W-1:0] a,
                                   input logic [AGE_W-1:0] b);
    logic [AGE_W-1:0] gap;   // `dist` is a SystemVerilog keyword
    gap = a - b;
    IsOlder = (gap != {AGE_W{1'b0}}) && gap[AGE_W-1];
  endfunction

  function automatic logic [CNT_W-1:0] PopCount(input logic [DEPTH-1:0] mask);
    logic [CNT_W-1:0] n;
    n = {CNT_W{1'b0}};
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (mask[i]) n = n + CNT_W'(1);
    end
    return n;
  endfunction

  // The uop identity is {rob_index, rob_gen, uop_index}, most significant field
  // first, so a kill names the first two fields and therefore covers every uop
  // of a macro that is being squashed.
  // The uop_index field is deliberately not compared: a kill names a *macro*,
  // and every uop of that macro must go. Verilator reports those uncompared low
  // bits of `id` as unused, which is the point of the function, so the warning
  // is silenced here rather than worked around by comparing a field the kill
  // port does not carry.
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic UopIsMacro(input logic [UOP_ID_W-1:0] id,
                                      input logic [ROB_INDEX_W-1:0] index,
                                      input logic [ROB_GEN_W-1:0] gen);
    // Two independent slices, each exactly as wide as the value it is compared
    // against. The obvious one-expression form -- a range select followed by a
    // part-select inside it -- is legal SystemVerilog that Verilator accepts and
    // slang rejects, so it is not portable across the tools that read this
    // source. Slicing at a computed base rather than through a wider slice also
    // keeps both comparisons width-exact, instead of leaving a truncation implicit
    // in the assignment.
    // The uop_index field is deliberately not compared: a kill names a *macro*,
    // and every uop of that macro must go, so the low bits of `id` are genuinely
    // unused here -- hence the lint_off around this function.
    //
    // The prefix is taken as one flat slice and compared against the expected
    // {rob_gen, rob_index} built by concatenation. Two things that read more
    // naturally are not portable here, and each was found by one lint tool while
    // the other passed the same line: chaining a part-select inside a range select
    // is legal SystemVerilog that one tool accepts and the other rejects, and
    // comparing the wide slice directly against the narrower `gen` is a
    // width-expansion warning, which is an error under this project's gate. The
    // concatenation has plain widths and no replication, and the slice is
    // compared against something of the same width, so neither tool objects.
    // The order of the concatenation is {rob_index, rob_gen}, matching the slice
    // and the documented layout of the identity; the other order is the same
    // width and the same two values and matches the *wrong* field.
    logic [ROB_GEN_W+ROB_INDEX_W-1:0] expected;
    expected = {index, gen};
    UopIsMacro = (id[UOP_ID_W-1 -: (ROB_INDEX_W + ROB_GEN_W)] == expected);
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  // ------------------------------------------------------------------- state
  // Entry data. Deliberately not reset: see the reset contract above. `ent_age`
  // is in this list on purpose: it is meaningless without its valid bit, and
  // resetting it would be a second copy of information the valid vector already
  // carries.
  logic [UOP_ID_W-1:0] ent_uop     [0:DEPTH-1];
  logic [3:0]           ent_alu_op  [0:DEPTH-1];
  logic [XLEN-1:0]      ent_imm     [0:DEPTH-1];
  logic [TAG_W-1:0]     ent_s1_tag  [0:DEPTH-1];
  logic [TAG_W-1:0]     ent_s2_tag  [0:DEPTH-1];
`ifdef MOSAIC_IQ_MUTANT_NO_GEN_CHECK
  // The mutant drops the generation comparison from the wakeup match, which
  // makes the stored generations dead *by construction* -- that is the defect,
  // stated as a dead net rather than as a subtle one.
  /* verilator lint_off UNUSEDSIGNAL */
`endif
  logic [TAG_GEN_W-1:0] ent_s1_gen  [0:DEPTH-1];
  logic [TAG_GEN_W-1:0] ent_s2_gen  [0:DEPTH-1];
`ifdef MOSAIC_IQ_MUTANT_NO_GEN_CHECK
  /* verilator lint_on UNUSEDSIGNAL */
`endif
  logic [XLEN-1:0]      ent_s1_val  [0:DEPTH-1];
  logic [XLEN-1:0]      ent_s2_val  [0:DEPTH-1];
  logic [TAG_W-1:0]     ent_dst_tag [0:DEPTH-1];
  logic [TAG_GEN_W-1:0] ent_dst_gen [0:DEPTH-1];
  logic                 ent_s1_rdy  [0:DEPTH-1];
  logic                 ent_s2_rdy  [0:DEPTH-1];
  logic [AGE_W-1:0]     ent_age     [0:DEPTH-1];

  // Validity, and "presented to the FU and not yet accepted", live outside the
  // entry arrays: one bit per slot each, the explicit packed vector the RAM
  // prescribes.
  logic [DEPTH-1:0] slot_valid;
  logic [DEPTH-1:0] slot_granted;

  logic [IDX_W-1:0] alloc_ptr;
  logic [CNT_W-1:0] occ_cnt;
  logic [AGE_W-1:0] age_ctr;

  logic [31:0] ins_total_q;
  logic [31:0] grant_total_q;
  logic [31:0] kill_total_q;
  logic [31:0] wu_total_q;
  logic [31:0] wu_matched_q;
  logic [31:0] wu_dup_q;
  logic [31:0] wu_stale_q;
  logic [31:0] wu_miss_q;

  // The conservation counters are the registers themselves: there is exactly
  // one place a count can live, so a counter cannot be exported one value behind
  // the state it is counting.
  assign o_ins_total   = ins_total_q;
  assign o_grant_total = grant_total_q;
  assign o_kill_total  = kill_total_q;
  assign o_wu_total    = wu_total_q;
  assign o_wu_matched  = wu_matched_q;
  assign o_wu_dup      = wu_dup_q;
  assign o_wu_stale    = wu_stale_q;
  assign o_wu_miss     = wu_miss_q;

  // ----------------------------------------------------------------- insert
  // `ins_ready` looks at the registered occupancy and nothing else: no
  // combinational path from `grant_ready`, and `rst` forces the refusal so that
  // no transfer can be reported during a reset cycle.
  assign o_full    = (occ_cnt == CNT_W'(DEPTH));
  assign ins_ready = !rst && !o_full;
  assign o_occupied = slot_valid;
  assign o_count    = occ_cnt;
  assign o_age_ctr  = age_ctr;

  // The slot an accepted insertion takes: the first free one at or after
  // `alloc_ptr`, wrapping by explicit comparison so a DEPTH that is not a power
  // of two aliases nothing. The scan is bounded by DEPTH, which is a
  // compile-time constant, so there is no unbounded loop in this file.
  logic             alloc_found;
  logic [IDX_W-1:0] alloc_slot;

  always_comb begin
    logic [IDX_W-1:0] probe;
    alloc_found = 1'b0;
    alloc_slot  = {IDX_W{1'b0}};
    probe       = alloc_ptr;
    for (int unsigned k = 0; k < DEPTH; k++) begin
      if (!alloc_found && !slot_valid[probe]) begin
        alloc_found = 1'b1;
        alloc_slot  = probe;
      end
      probe = (probe == IDX_W'(DEPTH - 1)) ? {IDX_W{1'b0}} : (probe + IDX_W'(1));
    end
  end

  logic ins_fire;
  assign ins_fire      = ins_valid && ins_ready;
  assign o_alloc_index = alloc_found ? alloc_slot : alloc_ptr;

  // ----------------------------------------------------------------- wakeup
  // A hit needs the tag *and* the generation, and needs the source not to be
  // ready already: a second broadcast for an operand that already holds its
  // value is a duplicate producer for that destination and must not overwrite
  // it. `dup` counts that case and `stale` counts the tag-matched,
  // generation-mismatched one, so both are observable refusals.
  logic [DEPTH-1:0] wu_hit1;
  logic [DEPTH-1:0] wu_hit2;
  logic [DEPTH-1:0] wu_dup1;
  logic [DEPTH-1:0] wu_dup2;
  logic [DEPTH-1:0] wu_tag_seen;   // this tag on some live source, any gen

`ifdef MOSAIC_IQ_MUTANT_NO_GEN_CHECK
  // NEGATIVE CONTROL 2: the generation is not compared. A broadcast for a
  // previous occupant of a recycled tag is then taken as the current one, and a
  // stale value is installed into a live uop -- a wrong result, silently.
  always_comb begin
    for (int unsigned i = 0; i < DEPTH; i++) begin
      wu_hit1[i] = wu_valid && slot_valid[i] && !ent_s1_rdy[i] &&
                   (ent_s1_tag[i] == wu_tag);
      wu_hit2[i] = wu_valid && slot_valid[i] && !ent_s2_rdy[i] &&
                   (ent_s2_tag[i] == wu_tag);
      wu_dup1[i] = wu_valid && slot_valid[i] && ent_s1_rdy[i] &&
                   (ent_s1_tag[i] == wu_tag);
      wu_dup2[i] = wu_valid && slot_valid[i] && ent_s2_rdy[i] &&
                   (ent_s2_tag[i] == wu_tag);
    end
  end
  always_comb begin
    for (int unsigned i = 0; i < DEPTH; i++) begin
      wu_tag_seen[i] = wu_valid && slot_valid[i] &&
                       ((ent_s1_tag[i] == wu_tag) || (ent_s2_tag[i] == wu_tag));
    end
  end

`else
  always_comb begin
    for (int unsigned i = 0; i < DEPTH; i++) begin
      wu_hit1[i]     = wu_valid && slot_valid[i] && !ent_s1_rdy[i] &&
                       (ent_s1_tag[i] == wu_tag) && (ent_s1_gen[i] == wu_gen);
      wu_hit2[i]     = wu_valid && slot_valid[i] && !ent_s2_rdy[i] &&
                       (ent_s2_tag[i] == wu_tag) && (ent_s2_gen[i] == wu_gen);
      wu_dup1[i]     = wu_valid && slot_valid[i] && ent_s1_rdy[i] &&
                       (ent_s1_tag[i] == wu_tag) && (ent_s1_gen[i] == wu_gen);
      wu_dup2[i]     = wu_valid && slot_valid[i] && ent_s2_rdy[i] &&
                       (ent_s2_tag[i] == wu_tag) && (ent_s2_gen[i] == wu_gen);
      wu_tag_seen[i] = wu_valid && slot_valid[i] &&
                       ((ent_s1_tag[i] == wu_tag) || (ent_s2_tag[i] == wu_tag));
    end
  end
`endif

  // The offered entry's own hit, generation check included: a broadcast for a
  // stale generation must not make a *newly inserted* entry ready either.
  logic ins_hit1;
  logic ins_hit2;
  logic ins_dup1;
  logic ins_dup2;

`ifdef MOSAIC_IQ_MUTANT_NO_SAME_CYCLE_WAKEUP
  // NEGATIVE CONTROL 3: the insert path is not connected to the broadcast at
  // all. An entry dispatched this cycle whose operand is produced this cycle
  // neither becomes ready this cycle nor receives the value, so it waits a whole
  // cycle for a broadcast that has already gone by. The value is not lost
  // forever -- the tag is durable -- but the same-cycle enqueue-and-wakeup case
  // the card names does not happen.
  assign ins_hit1 = 1'b0;
  assign ins_hit2 = 1'b0;
`else
  assign ins_hit1 = wu_valid && ins_valid && !ins_src1_ready &&
                    (ins_src1_tag == wu_tag) && (ins_src1_gen == wu_gen);
  assign ins_hit2 = wu_valid && ins_valid && !ins_src2_ready &&
                    (ins_src2_tag == wu_tag) && (ins_src2_gen == wu_gen);
`endif

  assign ins_dup1 = wu_valid && ins_valid && ins_src1_ready &&
                    (ins_src1_tag == wu_tag) && (ins_src1_gen == wu_gen);
  assign ins_dup2 = wu_valid && ins_valid && ins_src2_ready &&
                    (ins_src2_tag == wu_tag) && (ins_src2_gen == wu_gen);

  // The entry this cycle's insert would create, with this cycle's broadcast
  // already applied. This is the same-cycle enqueue-and-wakeup path: an operand
  // produced in the same cycle the uop is dispatched is ready in that cycle.
  logic ins_ready_now;
  assign ins_ready_now = ins_fire &&
                         (ins_src1_ready || ins_hit1) &&
                         (ins_src2_ready || ins_hit2);

  // Broadcast classification. Mutually exclusive and ordered, so a broadcast
  // that both advances one entry and duplicates another is counted once, as the
  // advance it is:
  //   matched  some not-ready source of a live entry -- or of the entry being
  //            offered this cycle -- took the value.
  //   dup      some ready source already holds this exact (tag, generation);
  //            the value is already there and is left alone.
  //   stale    nothing advanced, but a live entry names this tag with a
  //            different generation: a broadcast for a previous occupant.
  //   miss     nothing in the queue recognises the tag at all.
  logic wu_fire;
  logic wu_any_hit;
  logic wu_any_dup;
  logic wu_any_seen;

  assign wu_fire     = wu_valid && !rst;
  assign wu_any_hit  = (|wu_hit1) || (|wu_hit2) || (ins_fire && (ins_hit1 || ins_hit2));
  assign wu_any_dup  = (|wu_dup1) || (|wu_dup2) || (ins_fire && (ins_dup1 || ins_dup2));
  assign wu_any_seen = (|wu_tag_seen);

  // ------------------------------------------------------------------- kill
  // The reference is the *oldest* live uop of the named macro: "everything
  // younger than the squashed macro" is everything younger than its oldest uop,
  // and the macro's own younger uops are caught by the identity match. This
  // relies on in-order dispatch, which is what "younger" means; I-016 owns that
  // ordering and this port is the subtraction side of it.
  logic            kill_ref_found;
  logic [AGE_W-1:0] kill_ref_age;
  logic [DEPTH-1:0] kill_mask;

  always_comb begin
    kill_ref_found = 1'b0;
    kill_ref_age   = {AGE_W{1'b0}};
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (slot_valid[i] && UopIsMacro(ent_uop[i], kill_rob_index, kill_rob_gen) &&
          (!kill_ref_found || IsOlder(ent_age[i], kill_ref_age))) begin
        kill_ref_found = 1'b1;
        kill_ref_age   = ent_age[i];
      end
    end
  end

  always_comb begin
    for (int unsigned i = 0; i < DEPTH; i++) begin
      kill_mask[i] = kill_valid && kill_ref_found && slot_valid[i] &&
                     (UopIsMacro(ent_uop[i], kill_rob_index, kill_rob_gen) ||
                      (kill_younger && IsOlder(kill_ref_age, ent_age[i])));
    end
  end

  // -------------------------------------------------------------- selection
  // Readiness *this* cycle, i.e. after this cycle's broadcast.
  logic [DEPTH-1:0] entry_ready;
  always_comb begin
    for (int unsigned i = 0; i < DEPTH; i++) begin
      entry_ready[i] = slot_valid[i] && (ent_s1_rdy[i] || wu_hit1[i]) &&
                                       (ent_s2_rdy[i] || wu_hit2[i]);
    end
  end

  // An entry that has been presented and not yet accepted is not eligible.
  // That is what stops it being issued twice, and it is what makes "hold the
  // grant until the FU accepts it" a property of the arbitration and not only of
  // the output wires.
  logic [DEPTH-1:0] eligible;

  always_comb begin
    for (int unsigned i = 0; i < DEPTH; i++) eligible[i] = entry_ready[i] && !slot_granted[i];
  end

  // Oldest-ready, by age, over the resident entries. A lowest-index scan is
  // simpler and wrong: a slot index says nothing about age, and the allocation
  // pointer cycles, so the oldest live entry is regularly not the lowest slot.
  logic            win_found;
  logic [IDX_W-1:0] win_idx;

  always_comb begin
    logic [AGE_W-1:0] best_age;
    win_found = 1'b0;
    win_idx   = {IDX_W{1'b0}};
    best_age  = {AGE_W{1'b0}};
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (eligible[i] && (!win_found || IsOlder(ent_age[i], best_age))) begin
        win_found = 1'b1;
        win_idx   = IDX_W'(i);
        best_age  = ent_age[i];
      end
    end
  end

  // The entry currently presented and not yet accepted. At most one exists, by
  // construction: a fresh selection is only made while none is outstanding.
  logic            pend_found;
  logic [IDX_W-1:0] pend_idx;

  always_comb begin
    pend_found = 1'b0;
    pend_idx   = {IDX_W{1'b0}};
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (slot_granted[i]) begin
        pend_found = 1'b1;
        pend_idx   = IDX_W'(i);
      end
    end
  end

  // A kill naming the presented entry withdraws the grant: a squashed uop must
  // not issue. This is the one documented exception to "grant_valid holds until
  // accepted", and it exists because the alternative issues a uop that is not
  // live.
  logic pend_killed;
  assign pend_killed = pend_found && kill_mask[pend_idx];

  // The offered entry is the youngest of all, by the window invariant, so it is
  // considered only when no resident entry is eligible.
  logic win_is_ins;
  assign win_is_ins = !pend_found && !win_found && ins_ready_now;

  assign grant_valid = !rst && (pend_found ? !pend_killed : (win_found || ins_ready_now));

  // Which entry is presented, and whether its payload is read from the store or
  // from the insert bus.
  logic             grant_from_ins;
  logic [IDX_W-1:0] grant_idx;

`ifdef MOSAIC_IQ_MUTANT_UNSTABLE_GRANT
  // NEGATIVE CONTROL 4: the outstanding grant is not held. An entry offered on
  // the insert port takes the grant away even while one is already outstanding
  // and unaccepted, so the functional unit is offered a different entry each
  // cycle of a stall, the entry it was looking at is never issued, and it leaks
  // in the queue. An earlier version of this mutant only removed the
  // `granted` exclusion from the arbitration, which is *not* a defect: the
  // outstanding entry is still the oldest ready one, so oldest-ready selection
  // re-picks it and the mutant was vacuous. The hold's observable consequence is
  // that a fresh candidate must not overtake an outstanding grant, and that is
  // what is broken here.
  assign grant_from_ins = ins_ready_now;
`else
  assign grant_from_ins = !pend_found && win_is_ins;
`endif
  assign grant_idx      = pend_found ? pend_idx : (win_found ? win_idx : alloc_slot);

  logic grant_fire;
  logic grant_taken;
  assign grant_fire = grant_valid && grant_ready;


  // An entry accepted by the FU leaves the queue on this edge, and not before.
`ifdef MOSAIC_IQ_MUTANT_DROP_ON_GRANT
  // NEGATIVE CONTROL 5: the entry is removed as soon as it is *presented*, so a
  // grant the functional unit never accepts loses the uop. This is the card's
  // blocking rule, "grant 未被 FU 接受就移除 entry", stated as a defect.
  assign grant_taken = grant_fire || grant_valid;
`else
  assign grant_taken = grant_fire;
`endif

  // The offered entry leaves the queue in the same cycle it was offered, so it
  // never becomes resident and must not be counted a second time.
  logic ins_taken;
  assign ins_taken = ins_fire && grant_taken && grant_from_ins;

  logic [DEPTH-1:0] grant_rm_mask;
  always_comb begin
    grant_rm_mask = {DEPTH{1'b0}};
    if (grant_taken && !grant_from_ins) grant_rm_mask[grant_idx] = 1'b1;
  end

  // A resident entry leaves once. An entry that is both granted and killed in
  // the same cycle has left by the grant, so it is counted as granted; the
  // conservation identity below is exact because of that choice.
  logic [DEPTH-1:0] rm_mask;
  logic [DEPTH-1:0] kill_only_mask;
  always_comb begin
    kill_only_mask = kill_mask & ~grant_rm_mask;
    rm_mask        = grant_rm_mask | kill_mask;
  end

  logic [CNT_W-1:0] rm_count;
  assign rm_count = PopCount(rm_mask);

  // ---------------------------------------------------------------- the ages
  // The whole of the age machinery, and it is two rules:
  //
  //   1. A removal closes the hole it left. Every surviving entry steps down by
  //      one for each removed entry that was *older* than it, so the live ages
  //      are a contiguous window again and the window never grows.
  //   2. When the removed entry was the window's **base** -- its oldest live
  //      entry -- the survivors are *not* stepped down for it. They keep their
  //      ages, the window base slides up by one, and that slide is the only way
  //      the age counter ever wraps.
  //
  // Rule 2 is what a "just let `age_ctr` free-run" simplification gets wrong, and
  // the difference is not cosmetic: without the slide the age field never wraps
  // at all, so the modular comparison is never exercised and a width that is too
  // narrow passes every test that does not happen to build a full window. With
  // the slide, a queue that issues one entry per cycle walks its base right
  // around the modulus in 32 cycles and the narrow-modulus defect appears.
  //
  // `base_removed` is the base leaving; `rm_count` is every resident leaving.
  logic oldest_slot_found;
  logic [IDX_W-1:0] oldest_slot;
  logic base_removed;

  always_comb begin
    logic [AGE_W-1:0] best_age;
    oldest_slot_found = 1'b0;
    oldest_slot       = {IDX_W{1'b0}};
    best_age         = {AGE_W{1'b0}};
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (slot_valid[i] && (!oldest_slot_found || IsOlder(ent_age[i], best_age))) begin
        oldest_slot_found = 1'b1;
        oldest_slot       = IDX_W'(i);
        best_age          = ent_age[i];
      end
    end
  end

  assign base_removed = oldest_slot_found && rm_mask[oldest_slot];

  logic [AGE_W-1:0] ent_age_next [0:DEPTH-1];
  always_comb begin
    for (int unsigned i = 0; i < DEPTH; i++) begin
      logic [CNT_W-1:0] older_removed;
      older_removed = {CNT_W{1'b0}};
      for (int unsigned j = 0; j < DEPTH; j++) begin
        if (rm_mask[j] && IsOlder(ent_age[j], ent_age[i])) begin
          older_removed = older_removed + CNT_W'(1);
        end
      end
      // Rule 2: the base's own removal is not a hole below anybody, because
      // there is nobody below it. Subtracting it would drag the whole window
      // back down and pin the base forever.
`ifdef MOSAIC_IQ_MUTANT_NARROW_AGE
      // NEGATIVE CONTROL 1: the hole a removal leaves is NOT closed. The live ages
      // are no longer a contiguous window, so the distance between two long-lived
      // entries grows without bound and the modular comparison -- which is only
      // exact below half the modulus -- eventually orders them wrongly. This is
      // the failure the plan's modulus bound exists to prevent, injected as the
      // behaviour rather than as a width change, because a width change cannot be
      // expressed by a testbench with literal port widths and would fail at
      // elaboration for the wrong reason. The width form of the same defect is
      // caught by the `AGE_MOD_TOO_SMALL` elaboration guard, which is a build
      // error by design.
      ent_age_next[i] = ent_age[i] - AGE_W'(older_removed);
`else
      if (base_removed) older_removed = older_removed - CNT_W'(1);
      ent_age_next[i] = ent_age[i] - AGE_W'(older_removed);
`endif
    end
  end

  // The age an entry inserted this cycle takes: the top of the window *after*
  // this cycle's removals have closed their holes and after any base slide.
  // There is no combinational loop: the removal count and the base slide depend
  // on the resident selection, which never depends on the offered entry's age --
  // the offered entry is always the youngest and is considered only when nothing
  // resident is eligible.
  logic [AGE_W-1:0] ins_age;
  logic [AGE_W-1:0] age_ctr_next;
  // Whether the insertion path advances the allocator and takes an age.
  // Shipping: only a real insertion that became resident. The mutant also
  // advances on a *refused* insert -- `ins_valid` with `ins_ready` low, i.e. a
  // full queue -- which is exactly the "does a refused insert advance the
  // allocation pointer?" defect this work package set out to rule out. It is a
  // separate net so the defect is stated once and both update sites use it.
  logic ins_adv;
`ifdef MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES
  assign ins_adv = (ins_fire && !ins_taken) || (ins_valid && !ins_ready);
`else
  assign ins_adv = (ins_fire && !ins_taken);
`endif
  assign ins_age      = age_ctr - AGE_W'(rm_count) + AGE_W'(base_removed);
  assign age_ctr_next = ins_age + AGE_W'(ins_adv ? 1 : 0);

  // ---------------------------------------------------------- grant payload
  always_comb begin
    if (grant_from_ins) begin
      grant_uop     = ins_uop;
      grant_alu_op  = ins_alu_op;
      grant_imm     = ins_imm;
      grant_a       = ins_hit1 ? wu_val : ins_src1_val;
      grant_b       = ins_hit2 ? wu_val : ins_src2_val;
      grant_dst_tag = ins_dst_tag;
      grant_dst_gen = ins_dst_gen;
    end else begin
      grant_uop     = ent_uop[grant_idx];
      grant_alu_op  = ent_alu_op[grant_idx];
      grant_imm     = ent_imm[grant_idx];
      grant_a       = wu_hit1[grant_idx] ? wu_val : ent_s1_val[grant_idx];
      grant_b       = wu_hit2[grant_idx] ? wu_val : ent_s2_val[grant_idx];
      grant_dst_tag = ent_dst_tag[grant_idx];
      grant_dst_gen = ent_dst_gen[grant_idx];
    end
  end

  // The presented slot. Meaningful when `grant_valid` is high, which is the only
  // time a consumer may look at the grant at all; otherwise it is the current
  // winner's slot, or the allocation pointer when there is no winner.
  assign grant_index = grant_valid ? grant_idx : (win_found ? win_idx : alloc_ptr);

  // -------------------------------------------------- duplicate destinations
  // Exactly-once ownership in hardware: no two live entries may name the same
  // destination (tag, generation). A second producer for a destination is
  // reported here, not merged into the first -- and the offered entry is
  // included, so a conflict with a uop being dispatched this cycle is reported
  // in the cycle it is created.
  logic dst_conflict;
  always_comb begin
    dst_conflict = 1'b0;
    for (int unsigned i = 0; i < DEPTH; i++) begin
      for (int unsigned j = 0; j < DEPTH; j++) begin
        if (slot_valid[i] && slot_valid[j] && (i != j) &&
            (ent_dst_tag[i] == ent_dst_tag[j]) &&
            (ent_dst_gen[i] == ent_dst_gen[j])) begin
          dst_conflict = 1'b1;
        end
      end
    end
    if (ins_fire) begin
      for (int unsigned i = 0; i < DEPTH; i++) begin
        if (slot_valid[i] && (ent_dst_tag[i] == ins_dst_tag) &&
            (ent_dst_gen[i] == ins_dst_gen)) begin
          dst_conflict = 1'b1;
        end
      end
    end
  end

`ifdef MOSAIC_IQ_MUTANT_NO_DST_CHECK
  // NEGATIVE CONTROL 6: the duplicate-destination detector is dead, so a second
  // producer for a live destination is neither reported nor prevented.
  // Written as a mask rather than a constant so the detector is still
  // elaborated: a constant would be dead code the linter is right to remove.
  assign o_dst_conflict = dst_conflict && 1'b0;
`else
  assign o_dst_conflict = dst_conflict;
`endif

  // ----------------------------------------------------------- observation
  assign obs_valid      = slot_valid[obs_index_s];
  assign obs_age        = ent_age[obs_index_s];
  assign obs_ready      = ent_s1_rdy[obs_index_s] && ent_s2_rdy[obs_index_s];
  assign obs_granted    = slot_granted[obs_index_s];
  assign obs_src1_tag   = ent_s1_tag[obs_index_s];
  assign obs_src1_gen   = ent_s1_gen[obs_index_s];
  assign obs_src2_tag   = ent_s2_tag[obs_index_s];
  assign obs_src2_gen   = ent_s2_gen[obs_index_s];
  assign obs_uop        = ent_uop[obs_index_s];
  assign obs_alu_op     = ent_alu_op[obs_index_s];
  assign obs_imm        = ent_imm[obs_index_s];
  assign obs_dst_tag    = ent_dst_tag[obs_index_s];
  assign obs_dst_gen    = ent_dst_gen[obs_index_s];
  assign obs_src1_ready = ent_s1_rdy[obs_index_s];
  assign obs_src2_ready = ent_s2_rdy[obs_index_s];
  assign obs_src1_val   = ent_s1_val[obs_index_s];
  assign obs_src2_val   = ent_s2_val[obs_index_s];

  // ------------------------------------------------------------- sequential
  always_ff @(posedge clk) begin
    if (rst) begin
      slot_valid    <= {DEPTH{1'b0}};
      slot_granted  <= {DEPTH{1'b0}};
      occ_cnt       <= {CNT_W{1'b0}};
      alloc_ptr     <= {IDX_W{1'b0}};
      age_ctr       <= {AGE_W{1'b0}};
      ins_total_q   <= 32'd0;
      grant_total_q <= 32'd0;
      kill_total_q  <= 32'd0;
      wu_total_q    <= 32'd0;
      wu_matched_q  <= 32'd0;
      wu_dup_q      <= 32'd0;
      wu_stale_q    <= 32'd0;
      wu_miss_q     <= 32'd0;
    end else begin
      for (int unsigned i = 0; i < DEPTH; i++) begin
        // The insert writes the whole entry; an existing entry only has its
        // broadcast-hit fields written. `alloc_slot` is a free slot and a live
        // slot is never free, so the two never both target slot i.
        if (ins_fire && !ins_taken && (alloc_slot == IDX_W'(i))) begin
          ent_uop[i]     <= ins_uop;
          ent_alu_op[i]  <= ins_alu_op;
          ent_imm[i]     <= ins_imm;
          ent_s1_tag[i]  <= ins_src1_tag;
          ent_s1_gen[i]  <= ins_src1_gen;
          ent_s2_tag[i]  <= ins_src2_tag;
          ent_s2_gen[i]  <= ins_src2_gen;
          ent_dst_tag[i] <= ins_dst_tag;
          ent_dst_gen[i] <= ins_dst_gen;
          // A broadcast aimed at the entry being inserted is applied to it, so
          // a value that arrives before the entry does is not lost.
          ent_s1_val[i]  <= ins_hit1 ? wu_val : ins_src1_val;
          ent_s2_val[i]  <= ins_hit2 ? wu_val : ins_src2_val;
          ent_s1_rdy[i]  <= ins_src1_ready || ins_hit1;
          ent_s2_rdy[i]  <= ins_src2_ready || ins_hit2;
        end else begin
          // A broadcast aimed at a resident entry. A hit is only ever raised
          // from not-ready, so the stored value of a satisfied operand cannot be
          // overwritten by a duplicate producer.
          if (wu_hit1[i]) begin
            ent_s1_val[i] <= wu_val;
            ent_s1_rdy[i] <= 1'b1;
          end
          if (wu_hit2[i]) begin
            ent_s2_val[i] <= wu_val;
            ent_s2_rdy[i] <= 1'b1;
          end
        end

        // Age. An insertion lands at the top of the renumbered window; every
        // other slot is pulled down by the number of removals older than it.
        // The two cannot both target this slot: `alloc_slot` is free and
        // `rm_mask` only ever has bits for live slots.
        if (ins_fire && !ins_taken && (alloc_slot == IDX_W'(i))) begin
          ent_age[i] <= ins_age;
        end else begin
          ent_age[i] <= ent_age_next[i];
        end

        if (rm_mask[i]) begin
          slot_valid[i]   <= 1'b0;
          slot_granted[i] <= 1'b0;
        end else if (ins_fire && !ins_taken && (alloc_slot == IDX_W'(i))) begin
          slot_valid[i] <= 1'b1;
          // Presented in the cycle it was created and not accepted: it becomes
          // the outstanding grant from here on.
          if (grant_valid && grant_from_ins) slot_granted[i] <= 1'b1;
        end else if (grant_valid && !pend_found && !grant_from_ins &&
                     (win_idx == IDX_W'(i))) begin
          slot_granted[i] <= 1'b1;
        end
      end

      // Occupancy: an insertion that is granted straight out of the insert port
      // never becomes resident, so it neither adds nor removes.
      if (ins_fire && !ins_taken && (rm_count == {CNT_W{1'b0}})) begin
        occ_cnt <= occ_cnt + CNT_W'(1);
      end else if (ins_fire && !ins_taken) begin
        occ_cnt <= occ_cnt + CNT_W'(1) - rm_count;
      end else if (rm_count != {CNT_W{1'b0}}) begin
        occ_cnt <= occ_cnt - rm_count;
      end

      // `ins_adv`, not `ins_fire && !ins_taken`: see the declaration above. In
      // the shipping build the two are the same expression, so this is the one
      // place the mutant's defect reaches the allocation pointer.
      if (ins_adv) begin
        alloc_ptr <= (alloc_slot == IDX_W'(DEPTH - 1)) ? {IDX_W{1'b0}}
                                                        : (alloc_slot + IDX_W'(1));
      end
      age_ctr <= age_ctr_next;

      if (ins_fire) ins_total_q <= ins_total_q + 32'd1;
      if (grant_taken) grant_total_q <= grant_total_q + 32'd1;
      if (kill_only_mask != {DEPTH{1'b0}}) begin
        kill_total_q <= kill_total_q + 32'(PopCount(kill_only_mask));
      end
      if (wu_fire) begin
        wu_total_q <= wu_total_q + 32'd1;
        if (wu_any_hit) begin
          wu_matched_q <= wu_matched_q + 32'd1;
        end else if (wu_any_dup) begin
          wu_dup_q <= wu_dup_q + 32'd1;
        end else if (wu_any_seen) begin
          wu_stale_q <= wu_stale_q + 32'd1;
        end else begin
          wu_miss_q <= wu_miss_q + 32'd1;
        end
      end
    end
  end

endmodule : mosaic_iq

`resetall
`default_nettype wire
