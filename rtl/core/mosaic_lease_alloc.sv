// ============================================================================
// mosaic_lease_alloc -- work package I-024, the atomic resource lease.
//
// A uop may only issue when *every* resource it will need is available at that
// moment: the functional unit, the slot it will occupy, and the credit that will
// carry its result back (plus, for a remote issue, the network credit). Issuing
// with only some of them is how a machine deadlocks: an FU accepts work it can
// never retire, or a variable-latency unit completes into a full queue and the
// result is lost. This module is the resource broker that makes "all or none" a
// property of the hardware rather than a rule every requester is trusted to obey.
//
// ------------------------------------------------------------------ contract
//
//   * A request names the *set* of resource classes it needs, as one bit per
//     class (`req_mask`). `req_ready[i]` rises in a cycle if and only if, at that
//     moment and for every class in the set, a slot is free and a lease record is
//     free. Either the whole set is reserved or nothing is.
//   * A grant hands back a *lease id* -- `{record index, generation}` -- and,
//     for each class in the set, the slot it reserved. The requester may not
//     infer the slot; it is told.
//   * A lease ends with exactly one terminal event: `release` (the result was
//     accepted) or `cancel` (the uop was flushed). Both return the same
//     resources. A second release for one grant, a release for a lease that was
//     never granted or whose generation has been superseded, and a cancel after
//     a release are each *detected and counted*; none of them is absorbed, and
//     none of them changes any resource.
//   * A refused request has no side effect at all -- not on availability, not on
//     the lease ledger, not on the round-robin pointer.
//
// The rest of this header argues why the implementation is shaped the way it is.
//
// ------------------------------------------- the four resource classes (p0)
//
// Four classes, each a pool of slots sized from the generated geometry:
//
//   | class | slots                        | p0 | why |
//   |-------|------------------------------|----|-----|
//   | ALU   | CLUSTERS * ALU_PER_CLUSTER   |  2 | one non-stalling ALU per cluster |
//   | MD    | MULDIV_UNITS                 |  1 | the shared iterative MUL/DIV |
//   | WB    | CLUSTERS * RESULT_FIFO       |  4 | the result credit that carries the value home |
//   | NET   | CLUSTERS                     |  2 | one remote-hop credit per cluster port |
//
// `MOSAIC_CLUSTERS + MOSAIC_MULDIV_UNITS` issue ports are served: port `i` for
// `i < MOSAIC_CLUSTERS` is cluster `i`'s issue port, and the last port is the
// shared MUL/DIV path's. Nothing ties a port to a class: a cluster port may also
// ask for the shared MUL/DIV, and *that* is where the contention this module
// exists for comes from -- two clusters issuing a multiply in one cycle contend
// for one MD slot, and exactly one of them wins.
//
// --------------------------------------------------- why the ledger is the
// --------------------------------------------------- only copy of occupancy
//
// The obvious implementation keeps one free-slot bitmap per pool, set and cleared
// as slots are taken and returned, and a separate lease table. That is two copies
// of the same fact, and the failure mode of two copies is the failure this module
// is meant to prevent: a slot marked occupied by a lease that no longer exists
// ("the FU is reserved and the credit never comes"), or a lease whose slot has
// been handed to somebody else. Both are *representable*, so both need an
// invariant and a test.
//
// Here the only state is the lease ledger: for each record, whether it is live
// and which slots it holds. Occupancy is *derived* from it (`occ_alu` and
// friends), so a slot is occupied exactly while some live lease holds it. Partial
// reservation is not representable -- there is no second place to record it --
// and "every grant occupied its whole set" is not an assertion about the RTL but
// a property of the data model. What the test compares against its shadow is the
// derivation, which the shadow maintains independently (it keeps both the
// per-lease reservation and a per-pool bitmap, and asserts they agree).
//
// The price is a wide OR per pool per cycle: `LEASES` terms per pool, unrolled.
// At p0 that is four terms; a design with hundreds of leases would need the
// per-pool bitmaps back and would have to earn the invariant by testing it.
//
// ------------------------------------------- the terminal event frees first
//
// Terminals are applied before grants in the same cycle. A lease that dies on
// this edge stops holding its slots on this edge, and the credit it returns can
// fund a grant on the same edge. The alternative -- an extra cycle of separation
// between returning a credit and spending it -- is not wrong, it is slower, and
// in a credit loop it inserts a dead cycle whose only purpose is to keep two
// orderings distinguishable. The order is checked, not assumed: the
// directed phase fills every pool, releases one credit, and offers a request
// needing exactly that credit in the same cycle.
//
// A grant may therefore reuse a record terminated by the same cycle's terminal.
// That is deliberate: the record is free from this edge, and the new epoch's
// generation is the old epoch's plus one, so the two ids are distinguishable.
//
// ------------------------------------------------------- why a generation
//
// A lease record index is a wrapping, recycled name. A terminal event produced
// for an old epoch would, without a generation, free the resources of whoever
// holds that record now -- the same ABA defect that made `mosaic_rename` put a
// generation on every physical tag and `mosaic_iq` put one on every wakeup. So
// every grant stamps the record with the next generation of that record, the
// terminal carries it back, and a terminal whose generation is not the record's
// current epoch is *stale*: counted, refused, and no resource moves.
//
// The classification is deliberately three-way, because the three cases are
// three different bugs in the caller and only one of them is fixed by re-reading
// the ledger:
//
//   * stale    -- the id names a record that was never granted, a generation
//                 that was never issued, or an epoch that has been superseded by
//                 a newer one. The caller has the wrong identity, or is aiming
//                 at a lease from a previous life of the record.
//   * repeat   -- the id *is* the record's current epoch and the record is
//                 already terminated by the same kind of event. The caller's
//                 lifetime accounting released (or cancelled) the same lease
//                 twice.
//   * crossed  -- the record's current epoch was terminated by the *other*
//                 event (`can_after_release`, `rel_after_cancel`). The two
//                 terminal paths disagree about whether this lease is still
//                 outstanding, which is the flush-versus-completion race.
//
// All three change nothing. They are counted separately because collapsing them
// into one "not the owner" report loses exactly the distinction the caller needs.
//
// The generation modulus is `2**GEN_W`. It is derived from the ROB window
// (`GEN_W = $clog2(MOSAIC_ROB_ENTRIES) + 1`, the same idiom `mosaic_iq` uses for
// its wakeup generation): a terminal is produced while its own uop is in flight,
// a uop in flight holds at most one lease, and at most `MOSAIC_ROB_ENTRIES` uops
// are in flight, so the population of a record is bounded by the ROB. The limit
// this leaves -- a terminal delayed across `2**GEN_W` grants *to the same record*
// would alias -- is stated in the report as a limit, and the directed phase
// exercises the wrap so that the wrap is modelled rather than hidden.
//
// ------------------------------------------------------------------ reset
//
// Synchronous, active high, and *cold*: the whole ledger returns to its initial
// state (every pool free, every record never-granted, every counter zero). It
// does not return live leases one at a time, and it deliberately re-establishes
// the initial ledger rather than trying to account for what was outstanding.
//
// The per-lease drain the architecture review describes (FREEZE, DRAIN,
// REVOKED, INSTALL) is a *reconfiguration* protocol: it exists so a lease can be
// handed to another owner without losing old work. It belongs to I-090, not here,
// and a module that pretended to implement it would need a state machine no test
// in this case could distinguish from a cold reset. What a reset must guarantee
// here is exactly what the case checks: the post-reset ledger equals the cold
// ledger whatever the pre-reset occupancy was, and no pre-reset lease id can
// free anything after it (`epoch_valid` is cleared, so every old id is stale).
//
// `lease_gen` and the slot/mask arrays are *not* reset: they are meaningless
// without `live`/`used`, which are. That is the rule `rtl/common/mosaic_ram.sv`
// states -- reset cost is control state, not storage.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_LEASE_MUTANT_<name> injects one defect used to prove the case can
// fail. The shipping build defines none of them. The table with real output,
// including the failure-count delta against the shipping build, is in
// results/reports/I-024-lease.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names six of them; the rest belong to other modules
// and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */


module mosaic_lease_alloc #(
  // Geometry, from the generated package only. `localparam` and not `parameter`
  // on purpose: a caller cannot override a geometry constant into a second
  // geometry that disagrees with the profile the rest of the core was built for.
  localparam int unsigned REQ_COUNT = mosaic_cfg_pkg::MOSAIC_CLUSTERS
                                    + mosaic_cfg_pkg::MOSAIC_MULDIV_UNITS,
  localparam int unsigned SIZE_ALU  = mosaic_cfg_pkg::MOSAIC_CLUSTERS
                                    * mosaic_cfg_pkg::MOSAIC_ALU_PER_CLUSTER,
  localparam int unsigned SIZE_MD   = mosaic_cfg_pkg::MOSAIC_MULDIV_UNITS,
  localparam int unsigned SIZE_WB   = mosaic_cfg_pkg::MOSAIC_CLUSTERS
                                    * mosaic_cfg_pkg::MOSAIC_RESULT_FIFO,
  localparam int unsigned SIZE_NET  = mosaic_cfg_pkg::MOSAIC_CLUSTERS,

  // The number of resource classes the mask names. This is the module's
  // interface, not a profile knob: the four classes are the ones the fabric
  // contract lists (FU, operand/result slot, result credit, network credit), and
  // a fifth would be a different module with a different mask port.
  localparam int unsigned POOLS     = 4,

  // Every live lease holds exactly one WB credit, for its whole life: the credit
  // is reserved at grant and returned at the terminal event. The number of
  // simultaneously live leases is therefore bounded by the WB pool, and that is
  // the size of the ledger -- a proof, not a guess at a comfortable number.
  localparam int unsigned LEASES    = SIZE_WB,

  // See the header: 2**GEN_W must exceed the number of uops that can be in
  // flight, because a terminal is produced while its own uop is in flight.
  localparam int unsigned GEN_W     = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1,

  // A uniform slot stride so that one loop can walk any pool: each pool's bitmap
  // is MAX_SZ bits with the real slots in the low bits, and the scan is told how
  // wide the pool really is, so the padding is never handed out.
  localparam int unsigned M_WIDTH   = (SIZE_ALU > SIZE_MD)  ? SIZE_ALU  : SIZE_MD,
  localparam int unsigned N_WIDTH   = (SIZE_WB  > SIZE_NET) ? SIZE_WB   : SIZE_NET,
  localparam int unsigned MAX_SZ    = (M_WIDTH  > N_WIDTH)  ? M_WIDTH   : N_WIDTH,

  localparam int unsigned SLOT_W    = $clog2(MAX_SZ),

  // The ledger index has two widths, and they are different questions. `ROW_W`
  // addresses a record and is what indexes the arrays. `IDX_W` is the width of
  // the index *field* in a lease id, and it is one bit wider on purpose: the id
  // can then name a record that does not exist. Without the spare encoding a
  // range check would be a tautology for a power-of-two ledger and the "unknown
  // lease id" report would be unreachable -- a report nothing could prove works.
  localparam int unsigned ROW_W     = $clog2(LEASES),
  localparam int unsigned IDX_W     = ROW_W + 1,
  localparam int unsigned LEASE_W   = IDX_W + GEN_W,
  localparam int unsigned RR_W      = $clog2(REQ_COUNT)
) (
  input  logic                                   clk,
  input  logic                                   rst,

  // ------------------------------------------------------------- requests
  // One bit per issue port; `req_mask[i*POOLS + p]` is the class `p` the port `i`
  // needs. Ports are unrelated to classes: the mask is the whole request.
  input  logic [REQ_COUNT-1:0]                   req_valid,
  input  logic [REQ_COUNT*POOLS-1:0]             req_mask,

  // The grant. `req_ready[i]` is combinational in this cycle's requests and the
  // pre-edge ledger, and it is a *commitment*: the resources are taken on this
  // edge. `grant_id` is `{record index, generation}` laid out exactly like a
  // terminal id (`[GEN_W-1:0]` = generation, above it the index), and
  // `grant_slot[i*POOLS*SLOT_W + p*SLOT_W +: SLOT_W]` is the slot reserved in
  // class `p` (meaningful where the mask bit is set).
  output logic [REQ_COUNT-1:0]                   req_ready,
  output logic [REQ_COUNT*LEASE_W-1:0]           grant_id,
  output logic [REQ_COUNT*POOLS*SLOT_W-1:0]      grant_slot,

  // ------------------------------------------------------- terminal events
  input  logic                                   rel_valid,
  input  logic [LEASE_W-1:0]                     rel_id,
  output logic                                   rel_ok,
  output logic                                   rel_stale,
  output logic                                   rel_repeat,
  output logic                                   rel_after_cancel,

  input  logic                                   can_valid,
  input  logic [LEASE_W-1:0]                     can_id,
  output logic                                   can_ok,
  output logic                                   can_stale,
  output logic                                   can_repeat,
  output logic                                   can_after_release,

  // ---------------------------------------------------------- observation
  // The whole ledger, so a test can compare every bit of the unit's state every
  // cycle instead of a projection of it. `o_occ` is the derived occupancy, with
  // the four pools laid out one MAX_SZ-wide field each, ALU in the low bits; the
  // padding of a field is zero and is not a slot. `o_occ_count` is a *separately
  // maintained* count per pool, so comparing it against the population of
  // `o_occ` checks the accounting rather than restating it.
  output logic [POOLS*MAX_SZ-1:0]                o_occ,
  output logic [31:0]                            o_occ_count_alu,
  output logic [31:0]                            o_occ_count_md,
  output logic [31:0]                            o_occ_count_wb,
  output logic [31:0]                            o_occ_count_net,
  output logic [LEASES-1:0]                      o_lease_live,
  output logic [LEASES-1:0]                      o_lease_used,
  output logic [LEASES-1:0]                      o_lease_end_kind,   // 0 = released, 1 = cancelled
  output logic [LEASES*GEN_W-1:0]                o_lease_gen,
  output logic [LEASES*POOLS-1:0]                o_lease_mask,
  output logic [LEASES*POOLS*SLOT_W-1:0]         o_lease_slot,
  output logic [RR_W-1:0]                        o_rr,
  output logic [31:0]                            o_grant_count,
  output logic [31:0]                            o_rel_ok_count,
  output logic [31:0]                            o_can_ok_count,
  output logic [31:0]                            o_rel_stale_count,
  output logic [31:0]                            o_can_stale_count,
  output logic [31:0]                            o_rel_repeat_count,
  output logic [31:0]                            o_can_repeat_count,
  output logic [31:0]                            o_rel_after_cancel_count,
  output logic [31:0]                            o_can_after_release_count,
  output logic [31:0]                            o_live_count,

  // --------------------------------------------------- geometry readback
  // Read out of the elaborated instance so the testbench sizes its shadow from
  // the DUT, never from a second copy of the geometry.
  output logic [31:0]                            o_req_count,
  output logic [31:0]                            o_pools,
  output logic [31:0]                            o_leases,
  output logic [31:0]                            o_max_sz,
  output logic [31:0]                            o_gen_w,
  output logic [31:0]                            o_slot_w,
  output logic [31:0]                            o_lease_w,
  output logic [31:0]                            o_size_alu,
  output logic [31:0]                            o_size_md,
  output logic [31:0]                            o_size_wb,
  output logic [31:0]                            o_size_net
);

  // ------------------------------------------------------------- the ledger
  // Live is "a grant is outstanding"; used is "this record has ever been
  // granted since reset", which is what separates "never granted" from
  // "terminated" in the classification; end_kind is how the current epoch of a
  // terminated record ended. gen is the generation of the record's current
  // epoch, so the next grant takes gen+1 and the epoch is never re-issued.
  logic [LEASES-1:0]            lease_live;
  logic [LEASES-1:0]            lease_used;
  logic [LEASES-1:0]            lease_end_kind;
  logic [LEASES*GEN_W-1:0]      lease_gen;
  logic [LEASES*POOLS-1:0]      lease_mask;
  logic [LEASES*POOLS*SLOT_W-1:0] lease_slot;

  logic [RR_W-1:0]              rr;

  logic [31:0]                  grant_count;
  logic [31:0]                  rel_ok_count;
  logic [31:0]                  can_ok_count;
  logic [31:0]                  rel_stale_count;
  logic [31:0]                  can_stale_count;
  logic [31:0]                  rel_repeat_count;
  logic [31:0]                  can_repeat_count;
  logic [31:0]                  rel_after_cancel_count;
  logic [31:0]                  can_after_release_count;
  logic [31:0]                  live_count;
  logic [31:0]                  occ_count_alu;
  logic [31:0]                  occ_count_md;
  logic [31:0]                  occ_count_wb;
  logic [31:0]                  occ_count_net;

  // ------------------------------------------------------- derived occupancy
  // The single source of truth, read back out of the ledger. A lease that dies
  // on this edge does *not* appear here yet -- this is the pre-edge view, which
  // is what the unit presents during the cycle under test -- while the
  // arbitration below uses the working copy that does.
  logic [MAX_SZ-1:0]            occ_alu;
  logic [MAX_SZ-1:0]            occ_md;
  logic [MAX_SZ-1:0]            occ_wb;
  logic [MAX_SZ-1:0]            occ_net;

  always_comb begin : occ_derive
    occ_alu = {MAX_SZ{1'b0}};
    occ_md  = {MAX_SZ{1'b0}};
    occ_wb  = {MAX_SZ{1'b0}};
    occ_net = {MAX_SZ{1'b0}};
    for (int unsigned r = 0; r < LEASES; r++) begin
      if (lease_live[r]) begin
        if (lease_mask[r*POOLS + 0])
          occ_alu[lease_slot[(r*POOLS + 0)*SLOT_W +: SLOT_W]] = 1'b1;
        if (lease_mask[r*POOLS + 1])
          occ_md[lease_slot[(r*POOLS + 1)*SLOT_W +: SLOT_W]] = 1'b1;
        if (lease_mask[r*POOLS + 2])
          occ_wb[lease_slot[(r*POOLS + 2)*SLOT_W +: SLOT_W]] = 1'b1;
        if (lease_mask[r*POOLS + 3])
          occ_net[lease_slot[(r*POOLS + 3)*SLOT_W +: SLOT_W]] = 1'b1;
      end
    end
  end

  assign o_occ = {occ_net, occ_wb, occ_md, occ_alu};

  // ------------------------------------------------------ terminal handling
  // The working view of the ledger that the classification and the arbitration
  // share: `live_w` dies where a terminal is accepted, so the freed slots are
  // visible to this edge's grants.
  logic [LEASES-1:0]            live_w;
  logic [LEASES-1:0]            used_w;
  logic [LEASES-1:0]            end_w;
  logic [LEASES*GEN_W-1:0]      gen_w;

  logic [IDX_W-1:0]             rel_idx_f;
  logic [GEN_W-1:0]             rel_gen_f;
  logic [IDX_W-1:0]             can_idx_f;
  logic [GEN_W-1:0]             can_gen_f;
  logic [ROW_W-1:0]             rel_idx;
  logic [ROW_W-1:0]             can_idx;

  // Frees the slots a dying lease held, in the working availability bitmaps.
  // One place, so "a terminal returns every resource it holds and nothing else"
  // is one piece of code rather than two that must agree.
  logic [MAX_SZ-1:0]            free_alu;
  logic [MAX_SZ-1:0]            free_md;
  logic [MAX_SZ-1:0]            free_wb;
  logic [MAX_SZ-1:0]            free_net;
  logic                         rel_frees;
  logic                         can_frees;

  // ------------------------------------------------------------ arbitration
  logic [REQ_COUNT-1:0]         ready_c;
  logic [REQ_COUNT*IDX_W-1:0]   grant_rec_c;
  logic [REQ_COUNT*GEN_W-1:0]   grant_gen_c;
  logic [REQ_COUNT*POOLS*SLOT_W-1:0] slot_c;
  logic [RR_W-1:0]              rr_c;

  // Delta counters, computed combinationally so that the registers below are
  // each written once per cycle. Writing them inside the grant loop would make
  // the result depend on which grant the loop happened to process last.
  logic [31:0]                  grant_n;
  logic [31:0]                  occ_delta_alu;
  logic [31:0]                  occ_delta_md;
  logic [31:0]                  occ_delta_wb;
  logic [31:0]                  occ_delta_net;

  logic                         rel_ok_c, rel_stale_c, rel_repeat_c, rel_cross_c;
  logic                         can_ok_c, can_stale_c, can_repeat_c, can_cross_c;

  // The first free slot of a pool, as `{found, slot}`. `width` is the pool's
  // real size and is a constant at every call site, so the padding above it can
  // never be selected.
  function automatic logic [SLOT_W:0] lowest_free(input logic [MAX_SZ-1:0] occupied,
                                                  input int unsigned       width);
    logic found;
    begin
      found = 1'b0;
      lowest_free = {(SLOT_W+1){1'b0}};
      for (int unsigned s = 0; s < MAX_SZ; s++) begin
        if ((s < width) && !occupied[s] && !found) begin
          found = 1'b1;
          lowest_free[SLOT_W-1:0] = s[SLOT_W-1:0];
        end
      end
      lowest_free[SLOT_W] = found;
    end
  endfunction

  // The first record with no live lease, as `{found, index}`. Separate from the
  // pool scan only because the index is a different width.
  function automatic logic [ROW_W:0] lowest_free_record(input logic [LEASES-1:0] live);
    logic found;
    begin
      found = 1'b0;
      lowest_free_record = {(ROW_W+1){1'b0}};
      for (int unsigned r = 0; r < LEASES; r++) begin
        if (!live[r] && !found) begin
          found = 1'b1;
          lowest_free_record[ROW_W-1:0] = r[ROW_W-1:0];
        end
      end
      lowest_free_record[ROW_W] = found;
    end
  endfunction

  always_comb begin : arb
    // Block-local: these describe the candidate under consideration and are
    // meaningless outside the loop, and a module-level signal would infer a
    // latch for every cycle in which no requester is valid.
    logic [31:0]     cand;
    logic            need_alu;
    logic            need_md;
    logic            need_wb;
    logic            need_net;
    logic            satisfied;
    logic [SLOT_W:0] pick_alu;
    logic [SLOT_W:0] pick_md;
    logic [SLOT_W:0] pick_wb;
    logic [SLOT_W:0] pick_net;
    logic [ROW_W:0]  pick_rec;

    // ---------------------------------------------------------- defaults
    rel_ok_c    = 1'b0;
    rel_stale_c = 1'b0;
    rel_repeat_c = 1'b0;
    rel_cross_c = 1'b0;
    can_ok_c    = 1'b0;
    can_stale_c = 1'b0;
    can_repeat_c = 1'b0;
    can_cross_c = 1'b0;
    rel_idx     = {ROW_W{1'b0}};
    can_idx     = {ROW_W{1'b0}};

    rel_idx_f   = rel_id[LEASE_W-1 -: IDX_W];
    rel_gen_f   = rel_id[GEN_W-1:0];
    can_idx_f   = can_id[LEASE_W-1 -: IDX_W];
    can_gen_f   = can_id[GEN_W-1:0];

    live_w      = lease_live;
    used_w      = lease_used;
    end_w       = lease_end_kind;
    gen_w       = lease_gen;

    // ------------------------------------------------- release, then cancel
    // The release is classified first, against the pre-edge ledger; the cancel
    // is classified against the state the release leaves behind. Two terminals
    // for the same lease in one cycle are therefore not a don't-care: the
    // release wins, and the cancel is reported as `can_after_release` rather
    // than silently dropped. `mosaic_fetch` states the same rule for a credit
    // returned and spent on one edge.
    if (rel_valid) begin
      if (rel_idx_f >= IDX_W'(LEASES)) begin
        rel_stale_c = 1'b1;
      end else if (!used_w[rel_idx_f[ROW_W-1:0]]) begin
        rel_stale_c = 1'b1;
      end else if (rel_gen_f != gen_w[rel_idx_f[ROW_W-1:0]*GEN_W +: GEN_W]) begin
        rel_stale_c = 1'b1;
      end else if (!live_w[rel_idx_f[ROW_W-1:0]]) begin
        if (end_w[rel_idx_f[ROW_W-1:0]]) rel_cross_c = 1'b1;
        else                             rel_repeat_c = 1'b1;
      end else begin
        rel_ok_c      = 1'b1;
        rel_idx       = rel_idx_f[ROW_W-1:0];
        live_w[rel_idx_f[ROW_W-1:0]] = 1'b0;
        end_w[rel_idx_f[ROW_W-1:0]]  = 1'b0;
      end
    end

    if (can_valid) begin
      if (can_idx_f >= IDX_W'(LEASES)) begin
        can_stale_c = 1'b1;
      end else if (!used_w[can_idx_f[ROW_W-1:0]]) begin
        can_stale_c = 1'b1;
      end else if (can_gen_f != gen_w[can_idx_f[ROW_W-1:0]*GEN_W +: GEN_W]) begin
        can_stale_c = 1'b1;
      end else if (!live_w[can_idx_f[ROW_W-1:0]]) begin
        if (end_w[can_idx_f[ROW_W-1:0]]) can_repeat_c = 1'b1;
        else                             can_cross_c  = 1'b1;
      end else begin
        can_ok_c      = 1'b1;
        can_idx       = can_idx_f[ROW_W-1:0];
        live_w[can_idx_f[ROW_W-1:0]] = 1'b0;
        end_w[can_idx_f[ROW_W-1:0]]  = 1'b1;
      end
    end

    // ------------------------------------------- working availability
    // Derived occupancy first, then the terminals' returns, then the grants.
    free_alu = ~occ_alu;
    free_md  = ~occ_md;
    free_wb  = ~occ_wb;
    free_net = ~occ_net;

    rel_frees = rel_ok_c;
    can_frees = can_ok_c;

`ifdef MOSAIC_LEASE_MUTANT_GRANT_BEFORE_TERMINAL
    // NEGATIVE CONTROL 6: a credit returned on this edge is not visible to this
    // edge's arbitration, so a grant that should be funded by a release in the
    // same cycle is refused and waits one cycle. The ledger still dies here, so
    // nothing leaks -- what is broken is the documented same-cycle order.
    rel_frees = 1'b0;
    can_frees = 1'b0;
`endif

    if (rel_frees) begin
      if (lease_mask[rel_idx*POOLS + 0])
        free_alu[lease_slot[(rel_idx*POOLS + 0)*SLOT_W +: SLOT_W]] = 1'b1;
      if (lease_mask[rel_idx*POOLS + 1])
        free_md[lease_slot[(rel_idx*POOLS + 1)*SLOT_W +: SLOT_W]] = 1'b1;
      if (lease_mask[rel_idx*POOLS + 2])
        free_wb[lease_slot[(rel_idx*POOLS + 2)*SLOT_W +: SLOT_W]] = 1'b1;
      if (lease_mask[rel_idx*POOLS + 3])
        free_net[lease_slot[(rel_idx*POOLS + 3)*SLOT_W +: SLOT_W]] = 1'b1;
    end
    if (can_frees) begin
      if (lease_mask[can_idx*POOLS + 0])
        free_alu[lease_slot[(can_idx*POOLS + 0)*SLOT_W +: SLOT_W]] = 1'b1;
      if (lease_mask[can_idx*POOLS + 1])
        free_md[lease_slot[(can_idx*POOLS + 1)*SLOT_W +: SLOT_W]] = 1'b1;
      if (lease_mask[can_idx*POOLS + 2])
        free_wb[lease_slot[(can_idx*POOLS + 2)*SLOT_W +: SLOT_W]] = 1'b1;
      if (lease_mask[can_idx*POOLS + 3])
        free_net[lease_slot[(can_idx*POOLS + 3)*SLOT_W +: SLOT_W]] = 1'b1;
    end

    // ------------------------------------------------------- round robin
    // The candidate order starts at the pointer and walks once round, so a
    // requester that lost a contended slot is reached first on a later cycle and
    // cannot be starved. The pointer follows the last grant of the cycle.
    ready_c      = {REQ_COUNT{1'b0}};
    grant_rec_c  = {(REQ_COUNT*IDX_W){1'b0}};
    grant_gen_c  = {(REQ_COUNT*GEN_W){1'b0}};
    slot_c       = {(REQ_COUNT*POOLS*SLOT_W){1'b0}};
    rr_c         = rr;
    grant_n      = 32'd0;
    // Defaults for the per-candidate values, so no path through the block
    // leaves one unassigned: they are only *used* under `req_valid`, but a
    // combinational block that does not assign them on every path infers
    // storage, and a latch here would remember the previous candidate.
    need_alu     = 1'b0;
    need_md      = 1'b0;
    need_wb      = 1'b0;
    need_net     = 1'b0;
    satisfied    = 1'b0;
    pick_alu     = {(SLOT_W+1){1'b0}};
    pick_md      = {(SLOT_W+1){1'b0}};
    pick_wb      = {(SLOT_W+1){1'b0}};
    pick_net     = {(SLOT_W+1){1'b0}};
    pick_rec     = {(ROW_W+1){1'b0}};
    occ_delta_alu = 32'd0;
    occ_delta_md  = 32'd0;
    occ_delta_wb  = 32'd0;
    occ_delta_net = 32'd0;

    for (int unsigned k = 0; k < REQ_COUNT; k++) begin
      cand = {{(32-RR_W){1'b0}}, rr};
      cand = cand + k;
      if (cand >= REQ_COUNT) cand = cand - REQ_COUNT;

      pick_alu = lowest_free(free_alu, SIZE_ALU);
      pick_md  = lowest_free(free_md,  SIZE_MD);
      pick_wb  = lowest_free(free_wb,  SIZE_WB);
      pick_net = lowest_free(free_net, SIZE_NET);

      if (req_valid[cand]) begin
        need_alu = req_mask[cand*POOLS + 0];
        need_md  = req_mask[cand*POOLS + 1];
        need_wb  = req_mask[cand*POOLS + 2];
        need_net = req_mask[cand*POOLS + 3];

        // All or none: every named class must have a slot, or nothing is taken.
        satisfied = (!need_alu || pick_alu[SLOT_W]) &&
                    (!need_md  || pick_md[SLOT_W])  &&
                    (!need_wb  || pick_wb[SLOT_W])  &&
                    (!need_net || pick_net[SLOT_W]);

        pick_rec = lowest_free_record(live_w);

`ifdef MOSAIC_LEASE_MUTANT_PARTIAL_GRANT
        // NEGATIVE CONTROL 1: the WB credit is not part of the test, so a uop is
        // launched with an FU slot reserved and no room for its result. This is
        // the deadlock the card's Fail criterion names: the FU acquires work it
        // can never hand back, and the credit may never come.
        satisfied = (!need_alu || pick_alu[SLOT_W]) &&
                    (!need_md  || pick_md[SLOT_W])  &&
                    (!need_net || pick_net[SLOT_W]);
`endif

        if (satisfied && pick_rec[ROW_W]) begin
          ready_c[cand] = 1'b1;
          grant_rec_c[cand*IDX_W +: IDX_W] = {{(IDX_W-ROW_W){1'b0}}, pick_rec[ROW_W-1:0]};
          // The epoch this grant creates: the record's current generation plus
          // one. The record is free, so its generation is the one its last epoch
          // ended with, and no other grant can take this record in this cycle.
          grant_gen_c[cand*GEN_W +: GEN_W] = gen_w[pick_rec[ROW_W-1:0]*GEN_W +: GEN_W] + 1'b1;

          if (need_alu) begin
            slot_c[(cand*POOLS + 0)*SLOT_W +: SLOT_W] = pick_alu[SLOT_W-1:0];
            free_alu[pick_alu[SLOT_W-1:0]] = 1'b0;
            occ_delta_alu = occ_delta_alu + 32'd1;
          end
          if (need_md) begin
            slot_c[(cand*POOLS + 1)*SLOT_W +: SLOT_W] = pick_md[SLOT_W-1:0];
            free_md[pick_md[SLOT_W-1:0]] = 1'b0;
            occ_delta_md = occ_delta_md + 32'd1;
          end
          if (need_wb) begin
            slot_c[(cand*POOLS + 2)*SLOT_W +: SLOT_W] = pick_wb[SLOT_W-1:0];
            free_wb[pick_wb[SLOT_W-1:0]] = 1'b0;
            occ_delta_wb = occ_delta_wb + 32'd1;
          end
          if (need_net) begin
            slot_c[(cand*POOLS + 3)*SLOT_W +: SLOT_W] = pick_net[SLOT_W-1:0];
            free_net[pick_net[SLOT_W-1:0]] = 1'b0;
            occ_delta_net = occ_delta_net + 32'd1;
          end

          live_w[pick_rec[ROW_W-1:0]] = 1'b1;
          grant_n = grant_n + 32'd1;
          rr_c = (cand == (REQ_COUNT - 1)) ? {RR_W{1'b0}} : cand[RR_W-1:0] + 1'b1;
        end
`ifdef MOSAIC_LEASE_MUTANT_REFUSAL_CONSUMES
        else begin
          // NEGATIVE CONTROL 2: a refused request still takes the class it was
          // refused for. The refusal now has a side effect, so the next cycle's
          // availability depends on a request that was never granted -- and the
          // slot is held by nobody, because no lease id was handed out.
          live_w[pick_rec[ROW_W-1:0]] = 1'b1;
        end
`endif
      end
    end
  end

  // ------------------------------------------------------------- registers
  assign req_ready = ready_c;

  // The terminal reports are combinational in this cycle's ids and the pre-edge
  // ledger: they describe the edge about to happen, so a test compares them in
  // the cycle under test, before the clock, exactly like `req_ready`.
  assign rel_ok           = rel_ok_c;
  assign rel_stale        = rel_stale_c;
  assign rel_repeat       = rel_repeat_c;
  assign rel_after_cancel = rel_cross_c;
  assign can_ok           = can_ok_c;
  assign can_stale        = can_stale_c;
  assign can_repeat       = can_repeat_c;
  assign can_after_release = can_cross_c;

  always_comb begin
    for (int unsigned i = 0; i < REQ_COUNT; i++) begin
      grant_id[i*LEASE_W +: LEASE_W] =
          {grant_rec_c[i*IDX_W +: IDX_W], grant_gen_c[i*GEN_W +: GEN_W]};
    end
  end

  assign grant_slot = slot_c;

  always_ff @(posedge clk) begin
    if (rst) begin
      rr                    <= {RR_W{1'b0}};
      lease_live            <= {LEASES{1'b0}};
      lease_used            <= {LEASES{1'b0}};
      lease_end_kind        <= {LEASES{1'b0}};
      grant_count           <= 32'd0;
      rel_ok_count          <= 32'd0;
      can_ok_count          <= 32'd0;
      rel_stale_count       <= 32'd0;
      can_stale_count       <= 32'd0;
      rel_repeat_count      <= 32'd0;
      can_repeat_count      <= 32'd0;
      rel_after_cancel_count <= 32'd0;
      can_after_release_count <= 32'd0;
      live_count            <= 32'd0;
      occ_count_alu         <= 32'd0;
      occ_count_md          <= 32'd0;
      occ_count_wb          <= 32'd0;
      occ_count_net         <= 32'd0;
      // Deliberately not reset: lease_gen, lease_mask and lease_slot are
      // meaningless without lease_live/lease_used, which are.
    end else begin
      rr <= rr_c;

      // The terminals first. A grant later in this block may reuse a record
      // freed here, and the later non-blocking assignment wins -- which is the
      // documented order, not an accident of statement placement.
      if (rel_ok_c) begin
        lease_live[rel_idx]     <= 1'b0;
        lease_end_kind[rel_idx] <= 1'b0;
      end
      if (can_ok_c) begin
        lease_live[can_idx]     <= 1'b0;
        lease_end_kind[can_idx] <= 1'b1;
      end

      for (int unsigned i = 0; i < REQ_COUNT; i++) begin
        if (ready_c[i]) begin
          lease_live[grant_rec_c[i*IDX_W +: ROW_W]]     <= 1'b1;
          lease_used[grant_rec_c[i*IDX_W +: ROW_W]]     <= 1'b1;
          lease_gen[(grant_rec_c[i*IDX_W +: ROW_W]*GEN_W) +: GEN_W]
                                                        <= grant_gen_c[i*GEN_W +: GEN_W];
          lease_mask[(grant_rec_c[i*IDX_W +: ROW_W]*POOLS) +: POOLS]
                                                        <= req_mask[i*POOLS +: POOLS];
          lease_slot[(grant_rec_c[i*IDX_W +: ROW_W]*POOLS*SLOT_W) +: POOLS*SLOT_W]
                                                        <= slot_c[i*POOLS*SLOT_W +: POOLS*SLOT_W];
        end
      end

      grant_count            <= grant_count + grant_n;
      rel_ok_count           <= rel_ok_count + {{31{1'b0}}, rel_ok_c};
      can_ok_count           <= can_ok_count + {{31{1'b0}}, can_ok_c};
      rel_stale_count        <= rel_stale_count + {{31{1'b0}}, rel_stale_c};
      can_stale_count        <= can_stale_count + {{31{1'b0}}, can_stale_c};
      rel_repeat_count       <= rel_repeat_count + {{31{1'b0}}, rel_repeat_c};
      can_repeat_count       <= can_repeat_count + {{31{1'b0}}, can_repeat_c};
      rel_after_cancel_count <= rel_after_cancel_count + {{31{1'b0}}, rel_cross_c};
      can_after_release_count <= can_after_release_count + {{31{1'b0}}, can_cross_c};

      live_count  <= live_count + grant_n
                   - {{31{1'b0}}, rel_ok_c} - {{31{1'b0}}, can_ok_c};

      // Returned before taken, like the availability itself, so the counters
      // track the same order the arbitration uses.
      occ_count_alu <= occ_count_alu + occ_delta_alu
                     - {{31{1'b0}}, (rel_frees && lease_mask[rel_idx*POOLS + 0])}
                     - {{31{1'b0}}, (can_frees && lease_mask[can_idx*POOLS + 0])};
      occ_count_md  <= occ_count_md + occ_delta_md
                     - {{31{1'b0}}, (rel_frees && lease_mask[rel_idx*POOLS + 1])}
                     - {{31{1'b0}}, (can_frees && lease_mask[can_idx*POOLS + 1])};
      occ_count_wb  <= occ_count_wb + occ_delta_wb
                     - {{31{1'b0}}, (rel_frees && lease_mask[rel_idx*POOLS + 2])}
                     - {{31{1'b0}}, (can_frees && lease_mask[can_idx*POOLS + 2])};
      occ_count_net <= occ_count_net + occ_delta_net
                     - {{31{1'b0}}, (rel_frees && lease_mask[rel_idx*POOLS + 3])}
                     - {{31{1'b0}}, (can_frees && lease_mask[can_idx*POOLS + 3])};
    end
  end

  // ---------------------------------------------------------- observation
  assign o_lease_live     = lease_live;
  assign o_lease_used     = lease_used;
  assign o_lease_end_kind = lease_end_kind;
  assign o_lease_gen      = lease_gen;
  assign o_lease_mask     = lease_mask;
  assign o_lease_slot     = lease_slot;
  assign o_rr             = rr;

  assign o_grant_count            = grant_count;
  assign o_rel_ok_count           = rel_ok_count;
  assign o_can_ok_count           = can_ok_count;
  assign o_rel_stale_count        = rel_stale_count;
  assign o_can_stale_count        = can_stale_count;
  assign o_rel_repeat_count       = rel_repeat_count;
  assign o_can_repeat_count       = can_repeat_count;
  assign o_rel_after_cancel_count = rel_after_cancel_count;
  assign o_can_after_release_count = can_after_release_count;
  assign o_live_count             = live_count;
  assign o_occ_count_alu          = occ_count_alu;
  assign o_occ_count_md           = occ_count_md;
  assign o_occ_count_wb           = occ_count_wb;
  assign o_occ_count_net          = occ_count_net;

  assign o_req_count = 32'(REQ_COUNT);
  assign o_pools     = 32'(POOLS);
  assign o_leases    = 32'(LEASES);
  assign o_max_sz    = 32'(MAX_SZ);
  assign o_gen_w     = 32'(GEN_W);
  assign o_slot_w    = 32'(SLOT_W);
  assign o_lease_w   = 32'(LEASE_W);
  assign o_size_alu = 32'(SIZE_ALU);
  assign o_size_md  = 32'(SIZE_MD);
  assign o_size_wb  = 32'(SIZE_WB);
  assign o_size_net = 32'(SIZE_NET);

  // ---------------------------------------------------------------- guards
  // The geometry rules this module's arithmetic depends on, as build errors
  // rather than as comments. Each names a module that does not exist, so a
  // profile that violates the rule fails to elaborate instead of passing a
  // comment that says it must not happen.
  localparam bit POOL_TOO_SMALL = (MAX_SZ < 2);        // SLOT_W would be 0
  localparam bit GEN_MOD_TOO_SMALL = ((1 << GEN_W) <= LEASES);
  localparam bit LEASES_TOO_SMALL = (LEASES < 2);      // ROW_W would be 0
  localparam bit NO_ISSUE_PORT = (REQ_COUNT < 1);

  generate
    if (POOL_TOO_SMALL) begin : g_bad_pool
      mosaic_lease_alloc_contract_violation u_pool_size();
    end
    if (GEN_MOD_TOO_SMALL) begin : g_bad_gen
      mosaic_lease_alloc_contract_violation u_gen_modulus();
    end
    if (LEASES_TOO_SMALL) begin : g_bad_leases
      mosaic_lease_alloc_contract_violation u_leases();
    end
    if (NO_ISSUE_PORT) begin : g_bad_ports
      mosaic_lease_alloc_contract_violation u_ports();
    end
  endgenerate

endmodule : mosaic_lease_alloc

`resetall
`default_nettype wire
