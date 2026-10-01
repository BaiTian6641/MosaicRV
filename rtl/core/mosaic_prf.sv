// ============================================================================
// mosaic_prf -- work package I-015.
//
// The integer physical register file: MOSAIC_INT_PRF_ENTRIES entries of
// MOSAIC_XLEN bits, split into MOSAIC_PRF_BANKS banks. Every port geometry --
// entry count, bank count, tag width, generation width, data width -- comes from
// the generated configuration and identity packages. There is no literal and no
// second copy of the geometry in this file, and no bank count that a profile
// change could contradict.
//
// ---------------------------------------------------------------- what this owns
//
// Storage and *admission*. A write port per bank, a read demand per bank with a
// zero-latency combinational response, and the rules that make a response mean
// something:
//
//   * a value is admitted only when its producer says its generation is live;
//   * a read of an entry that has never held a value says so, rather than
//     returning whatever the storage powered up with;
//   * a read that names the wrong generation gets the *stored* generation back,
//     so the consumer compares instead of trusting.
//
// What this module does **not** own is the operand collector: retrying a refused
// demand, collecting operands over several cycles and arbitrating between
// consumers belong to I-026/I-027. The PRF answers one offer per bank per cycle
// and forgets it. A demand that was not taken leaves no trace here -- there is no
// queue, no reorder buffer, no state a collector could accidentally rely on --
// because a second, private copy of the collection rules is how the collector
// and the register file would come to disagree.
//
// ------------------------------------------------------------ geometry: 96 / 4
//
// `MOSAIC_INT_PRF_ENTRIES = 96` over `MOSAIC_PRF_BANKS = 4` banks. 96 is
// deliberately not a power of two, so the decode has to be stated rather than
// assumed:
//
//     bank = tag % MOSAIC_PRF_BANKS          the home bank, 0..BANKS-1
//     row  = tag / MOSAIC_PRF_BANKS          the row inside that bank
//     rows = ceil(ENTRIES / BANKS)           storage depth per bank
//
// This is a real modulo, not a field split. The two agree only when the bank
// count is a power of two *and* the entry count is a multiple of the bank count
// squared; the moment either stops holding -- a 6-entry, 4-bank profile, say --
// a `tag[BANK_W+ROW_W-1:ROW_W]` split names banks that hold no entries and rows
// that belong to another bank, while `tag % BANKS` keeps every tag 0..95 in
// exactly one place. Because the geometry file says the bank count need not
// divide the entry count evenly, the divisor form is the one that stays correct,
// and it is written as a divisor so that a profile change cannot silently pick a
// different mapping. For p0 the *bank* selection happens to coincide with the
// low two bits of the tag; the *row* selection does not, which is what the
// row-slice mutant in the table below exercises.
//
// Rows per bank are `ceil(ENTRIES / BANKS)`, so a profile whose entry count is
// not a multiple of its bank count leaves the last row of the last bank
// unreachable rather than aliasing it onto a real entry. The reachability rule is
// `tag < ENTRIES`; a tag at or above the entry count has no home entry and is
// refused rather than wrapped, for the reason mosaic_rename.sv gives for its own
// out-of-range tags: wrapping an out-of-range index onto a valid one manufactures
// an alias onto somebody else's register. Rename never allocates such a tag
// (its free list only ever holds 0..95), so this is a guard rather than a path
// the core takes.
//
// ------------------------------------------------------ why a distinct valid bit
//
// The data and generation arrays are **not reset**. That is the rule
// rtl/common/mosaic_ram.sv documents and mosaic_rename.sv follows: reset cost is
// control state, not DEPTH x WIDTH of storage. What reset does clear is
// `valid_q`, one bit per entry, because an entry's *validity* is control state --
// and here it is the difference between a correct read and a wrong one.
//
// Without it, a read of an entry that has never been written returns zeroes (or,
// under a simulator that fills memory with unique values, garbage) and the
// consumer cannot tell that from a live value whose generation happens to be
// zero. That is the failure mode the card names: an uninitialised read covered up
// by an initialised zero. A generation alone cannot fix it, because generation 0
// is a real, assignable generation. So validity is a separate bit, reset to zero,
// and a read of an entry whose bit is clear raises `rsp_never_written_o` and
// reports no generation at all.
//
// -------------------------------------------------------- cycle semantics
//
// Everything combinational, everything describing the cycle it is presented in;
// the only registered state is the storage, the validity bits and the counters.
// A demand offered in cycle N is answered in cycle N, and the write offered in
// cycle N is visible to cycle N+1's read.
//
//   * `rd_ready_o[b]` is the **grant** for demand slot `b` in this cycle: the
//     demand is taken and its response is on `rsp_*` in the same cycle.
//     `rsp_valid_o[b]` equals it. There is no response register and therefore no
//     response slot that could be occupied: with a zero-latency port the slot is
//     the demand's own slot, and it is free exactly when the demand is granted.
//     A refused demand is not answered at all, and the consumer re-offers it.
//   * A bank serves **one** demand per cycle. If two (or more) offered demands
//     name the same bank, the lowest-numbered slot wins and the others are
//     refused with `rd_ready_o = 0`. The priority is a fixed index so the
//     outcome does not depend on wiring order; it is not a fairness scheme and
//     is not meant to be one -- fairness between consumers is the collector's
//     problem (I-026/I-027), and a register file that invented its own would be
//     wrong twice.
//   * **Write-through.** A read of an entry that a write port writes in the same
//     cycle returns the *new* value and the *new* generation, and its generation
//     comparison is against the new generation. The old value is never
//     presented: a consumer that acted on it would compute with a stale operand
//     in exactly the cycle the wakeup said the value had arrived.
//   * A write is **admitted** when all four of these hold: `wr_en_i[b]`,
//     `wr_gen_valid_i[b]`, the written tag's home bank is `b`, and the tag is in
//     range. The generation-valid input is the producer's statement that the
//     identity it is delivering has a live generation (mosaic_rename.sv's
//     `gen_valid`). A writeback that does not carry a live generation is refused
//     rather than stored, because admitting it would let a producer manufacture
//     validity out of a generation number that was never assigned -- and a read
//     of the entry would then report a value that no instruction ever computed.
//     The home-bank and range checks refuse a mis-routed write rather than
//     letting it land in another bank's row space, where a later read for the
//     tag it claimed would find unrelated data. A refused write changes nothing
//     and is not counted.
//   * `rsp_never_written_o[b]`: the entry has held no admitted value since reset
//     (or the tag has no home entry at all). The response carries zero data and
//     generation zero -- deterministic values, not storage contents -- and no
//     mismatch is raised, because the stored generation of an unwritten entry is
//     not an identity and comparing against it would report a coincidence.
//   * `rsp_gen_mismatch_o[b]`: the entry holds a value and the stored generation
//     is not the requested one. `rsp_gen_o` carries the **stored** generation, so
//     the consumer can act on the difference instead of trusting the response.
//   * `wr_gen_valid_i` is not needed to make an entry readable after it is
//     written once: an entry that has been admitted stays valid through every
//     later write, and a new incarnation of the same tag is distinguished by its
//     generation, never by clearing validity.
//
// ------------------------------------------------- counters and o_busy
//
// All five counters are 32-bit and wrap; they are saturating only in the sense
// that the test runs are far shorter than 2^32 cycles.
//
//   o_wr_ctr       admitted writes
//   o_rd_ctr       granted demands
//   o_conflict_ctr demands refused because another slot took their bank in the
//                  same cycle. With a zero-latency port that is the only possible
//                  refusal reason, so this counter and (offered - granted) are
//                  the same number seen from two sides.
//   o_mismatch_ctr responses whose stored generation did not match the request
//   o_invalid_ctr  responses for entries with no valid value since reset
//
// `o_busy` is high in a cycle in which some offered demand was refused, i.e.
// there is read work the register file could not take. It is the register file's
// contribution to back-pressure; the collector is what remembers the demand.
//
// ---------------------------------------------------------------- reset
//
// Synchronous, active high. Clears the validity bits and the counters, and
// nothing else: the data and generation storage is left alone.
//
// ------------------------------------------------------------- mutants
//
// -DMOSAIC_PRF_MUTANT_<n> injects exactly one defect to prove the unit test can
// detect it. The shipping build defines none of them, and the table with real
// output is in results/reports/I-015-prf.md:
//
//   NO_BANK_CONFLICT      two demands to one bank are both granted
//   DROP_BANK0_WRITE      bank 0's write port never applies
//   IGNORE_STORED_GEN     a read reports the requested generation and never a
//                         generation mismatch
//   NEVER_WRITTEN_VALID   an entry with no value since reset is reported valid
//   NO_WRITE_THROUGH      a same-cycle write is not visible to a same-cycle read
//   SLICE_ROW             the row is decoded from the tag's low bits instead of
//                         tag/BANKS (masked into range, so the defect is a wrong
//                         entry rather than an out-of-bounds array read)
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated headers declare one localparam per configuration knob for the
// whole project. This module names the register-file subset; the rest belong to
// other modules and are unused *here* by construction, not by omission.
//
// Both headers carry their own include guards, and `mosaic_id_pkg.svh` silences
// the unused-signal warnings its own project-wide helpers raise, so neither an
// include guard nor a duplicate-package waiver is needed here.
`include "mosaic_cfg_pkg.svh"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

// Widths are declared at file scope because a module's port list cannot see
// declarations inside its own body. Every one is derived from a generated
// package and nothing else.
//
// The `MPRF_` prefix is deliberate rather than decorative. These are
// compilation-unit-scope names shared with every file elaborated alongside this
// one, and a `PRF_`-prefixed name here collides with package-scope geometry that
// other files declare for the same register file (`mosaic_uop_pkg.sv` has its
// own `PRF_ENTRIES`): Verilator then reports the package's declaration as
// hiding this one, and VARHIDDEN is an error under the project's -Wall lint
// gate -- from a file that did nothing wrong. The prefix keeps the two scopes
// from meeting, so renaming it back to `PRF_...` is a lint failure waiting for
// the next module that mentions both.
localparam int unsigned MPRF_ENTRIES = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;  // 96
localparam int unsigned MPRF_TAG_W   = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;    // 7
localparam int unsigned MPRF_BANKS   = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;        // 4
localparam int unsigned MPRF_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;      // 8
localparam int unsigned MPRF_XLEN    = mosaic_cfg_pkg::MOSAIC_XLEN;             // 64
// Rows per bank. ceil(), not a floor: a profile whose entry count does not
// divide evenly leaves the surplus rows of the last bank unreachable rather than
// dropping real entries or aliasing them.
localparam int unsigned MPRF_ROWS    = (MPRF_ENTRIES + MPRF_BANKS - 1) / MPRF_BANKS;
// A degenerate geometry (one bank, or one row per bank) has a zero-bit decode
// field, and a zero-width vector is not a legal declaration. The width is
// clamped to one bit and the decode is still exact, because a one-bank decode
// compares against zero and a one-row row is always row zero.
localparam int unsigned MPRF_BANK_W  = (MPRF_BANKS > 1) ? $clog2(MPRF_BANKS) : 1;
localparam int unsigned MPRF_ROW_W   = (MPRF_ROWS > 1) ? $clog2(MPRF_ROWS) : 1;

module mosaic_prf (
    input  logic                                             clk_i,
    input  logic                                             rst_i,

    // ------------------------------------------------- one write port per bank
    // The writeback arbiter (I-026) routes each result to its tag's home bank
    // and presents it here. `wr_gen_valid_i[b]` is the producer's statement that
    // the identity on this port carries a live generation; a write without it is
    // refused (see the header).
    input  logic [MPRF_BANKS-1:0]                             wr_en_i,
    input  logic [MPRF_BANKS-1:0]                             wr_gen_valid_i,
    input  logic [MPRF_BANKS*MPRF_TAG_W-1:0]                   wr_tag_i,
    input  logic [MPRF_BANKS*MPRF_GEN_W-1:0]                   wr_gen_i,
    input  logic [MPRF_BANKS*MPRF_XLEN-1:0]                    wr_data_i,

    // ------------------------------------------------- one read demand per bank
    // Up to MPRF_BANKS demands per cycle. `rd_ready_o[b]` is the grant for slot b
    // in this cycle, and `rsp_valid_o[b]` the response; a refused demand is
    // re-offered by the consumer.
    input  logic [MPRF_BANKS-1:0]                             rd_valid_i,
    output logic [MPRF_BANKS-1:0]                             rd_ready_o,
    input  logic [MPRF_BANKS*MPRF_TAG_W-1:0]                   rd_tag_i,
    input  logic [MPRF_BANKS*MPRF_GEN_W-1:0]                   rd_gen_i,
    output logic [MPRF_BANKS-1:0]                             rsp_valid_o,
    output logic [MPRF_BANKS*MPRF_TAG_W-1:0]                   rsp_tag_o,
    output logic [MPRF_BANKS*MPRF_GEN_W-1:0]                   rsp_gen_o,
    output logic [MPRF_BANKS*MPRF_XLEN-1:0]                    rsp_data_o,
    output logic [MPRF_BANKS-1:0]                             rsp_gen_mismatch_o,
    output logic [MPRF_BANKS-1:0]                             rsp_never_written_o,

    // ------------------------------------------------ testbench-visible status
    output logic [31:0]                                      o_wr_ctr,
    output logic [31:0]                                      o_rd_ctr,
    output logic [31:0]                                      o_conflict_ctr,
    output logic [31:0]                                      o_mismatch_ctr,
    output logic [31:0]                                      o_invalid_ctr,
    output logic                                             o_busy
);

  // -------------------------------------------------------------- the storage
  // Data and generation are not reset; validity is, and lives outside them.
  logic [MPRF_ROWS-1:0]  valid_q [MPRF_BANKS];
  logic [MPRF_GEN_W-1:0] gen_q   [MPRF_BANKS][MPRF_ROWS];
  logic [MPRF_XLEN-1:0]  data_q  [MPRF_BANKS][MPRF_ROWS];

  // ------------------------------------------------------------- write decode
  logic [MPRF_BANK_W-1:0] wr_home_bank [MPRF_BANKS];
  logic [MPRF_ROW_W-1:0]  wr_row_addr  [MPRF_BANKS];
  logic                  wr_applied   [MPRF_BANKS];

  // -------------------------------------------------------------- read decode
  logic [MPRF_BANK_W-1:0] rd_bank  [MPRF_BANKS];
  logic [MPRF_ROW_W-1:0]  rd_row   [MPRF_BANKS];
  logic                  rd_range [MPRF_BANKS];

  // Combinational temporaries, declared here rather than inside the always_comb
  // so that both Verilator and Yosys see only declarations they already handle
  // elsewhere in this tree.
  logic [MPRF_BANK_W-1:0] arb_bank;
  logic [MPRF_ROW_W-1:0]  arb_row;
  logic                  arb_conflict;
  logic                  arb_bypass;

  // --------------------------------------------------------- counter deltas
  logic [31:0] wr_hits;
  logic [31:0] rd_grants;
  logic [31:0] rd_refusals;
  logic [31:0] mismatch_hits;
  logic [31:0] invalid_hits;
  logic        busy_f;

  // ------------------------------------------------------------ the counters
  logic [31:0] wr_ctr_q;
  logic [31:0] rd_ctr_q;
  logic [31:0] conflict_ctr_q;
  logic [31:0] mismatch_ctr_q;
  logic [31:0] invalid_ctr_q;

  // ------------------------------------------------------------------ decode
  // Pure functions of the geometry: the home bank is a modulo and the row is the
  // quotient, exactly as the header states. Both are constant-divisor operations
  // on a constant divisor, so both synthesise to wiring and a small adder.
  function automatic logic [MPRF_BANK_W-1:0] bank_of(input logic [MPRF_TAG_W-1:0] tag);
    int unsigned t;
    begin
      t       = int'(tag);
      bank_of = MPRF_BANK_W'(t % MPRF_BANKS);
    end
  endfunction

  function automatic logic [MPRF_ROW_W-1:0] row_of(input logic [MPRF_TAG_W-1:0] tag);
    int unsigned t;
    begin
      t = int'(tag);
`ifdef MOSAIC_PRF_MUTANT_SLICE_ROW
      // MUTANT: the row taken from the low bits of the tag, which is the
      // field-split decode the header rejects: the row stops being the tag's
      // quotient and becomes its low bits, so the entry a tag names is wrong.
      // The mask into range keeps the mutant a *behavioural* defect -- a wrong
      // entry -- rather than an out-of-bounds array read, which would be
      // undefined behaviour in the simulator and would prove nothing about the
      // decode.
      row_of = MPRF_ROW_W'((t & ((1 << MPRF_ROW_W) - 1)) % MPRF_ROWS);
`else
      row_of = MPRF_ROW_W'(t / MPRF_BANKS);
`endif
    end
  endfunction

  function automatic logic in_range(input logic [MPRF_TAG_W-1:0] tag);
    in_range = (int'(tag) < MPRF_ENTRIES);
  endfunction

  // ---------------------------------------------------------- write admission
  always_comb begin
    for (int b = 0; b < MPRF_BANKS; b++) begin
      wr_home_bank[b] = bank_of(wr_tag_i[b*MPRF_TAG_W +: MPRF_TAG_W]);
      wr_row_addr[b]  = row_of(wr_tag_i[b*MPRF_TAG_W +: MPRF_TAG_W]);
      wr_applied[b]   = wr_en_i[b]
                     && wr_gen_valid_i[b]
                     && (int'(wr_home_bank[b]) == b)
                     && in_range(wr_tag_i[b*MPRF_TAG_W +: MPRF_TAG_W]);
`ifdef MOSAIC_PRF_MUTANT_DROP_BANK0_WRITE
      // MUTANT: bank 0's write port is silently dead.
      if (b == 0) wr_applied[b] = 1'b0;
`endif
    end
  end

  // ----------------------------------------------------------- read decoding
  always_comb begin
    for (int s = 0; s < MPRF_BANKS; s++) begin
      rd_bank[s]  = bank_of(rd_tag_i[s*MPRF_TAG_W +: MPRF_TAG_W]);
      rd_row[s]   = row_of(rd_tag_i[s*MPRF_TAG_W +: MPRF_TAG_W]);
      rd_range[s] = in_range(rd_tag_i[s*MPRF_TAG_W +: MPRF_TAG_W]);
    end
  end

  // -------------------------------------------------- arbitration and response
  // Slot order is the priority order: a demand is granted when no earlier slot
  // that was itself granted names the same bank. `rd_ready_o` is read here as it
  // is being built, which is well defined because this is one always_comb and
  // the scan visits the slots in order.
  always_comb begin
    arb_bank     = '0;
    arb_row      = '0;
    arb_conflict = 1'b0;
    arb_bypass   = 1'b0;

    for (int s = 0; s < MPRF_BANKS; s++) begin
      rd_ready_o[s]                       = 1'b0;
      rsp_valid_o[s]                      = 1'b0;
      rsp_tag_o[s*MPRF_TAG_W +: MPRF_TAG_W] = rd_tag_i[s*MPRF_TAG_W +: MPRF_TAG_W];
      rsp_gen_o[s*MPRF_GEN_W +: MPRF_GEN_W] = '0;
      rsp_data_o[s*MPRF_XLEN +: MPRF_XLEN]  = '0;
      rsp_gen_mismatch_o[s]               = 1'b0;
      rsp_never_written_o[s]              = 1'b0;
    end

    for (int s = 0; s < MPRF_BANKS; s++) begin
      if (rd_valid_i[s]) begin
        arb_bank     = rd_bank[s];
        arb_row      = rd_row[s];
        arb_conflict = 1'b0;
        for (int p = 0; p < s; p++) begin
          if (rd_ready_o[p] && (rd_bank[p] == arb_bank)) arb_conflict = 1'b1;
        end
`ifdef MOSAIC_PRF_MUTANT_NO_BANK_CONFLICT
        // MUTANT: a bank conflict is ignored, so two slots are granted to the
        // same bank in one cycle and the second overwrites the first's response.
        arb_conflict = 1'b0;
`endif
        if (!arb_conflict) begin
          rd_ready_o[s]  = 1'b1;
          rsp_valid_o[s] = 1'b1;

          // Write-through: an admitted write to this bank in this cycle that
          // targets the very same row is the value this read must see. `wr_row_addr`
          // is indexed by the *read* slot's bank, which is what makes the
          // comparison a same-entry comparison rather than a same-bank one.
`ifdef MOSAIC_PRF_MUTANT_NO_WRITE_THROUGH
          arb_bypass = 1'b0;
`else
          arb_bypass = wr_applied[arb_bank] && (wr_row_addr[arb_bank] == arb_row);
`endif

          if (rd_range[s] && (arb_bypass || valid_q[arb_bank][arb_row])) begin
            if (arb_bypass) begin
              rsp_gen_o[s*MPRF_GEN_W +: MPRF_GEN_W] =
                  wr_gen_i[arb_bank*MPRF_GEN_W +: MPRF_GEN_W];
              rsp_data_o[s*MPRF_XLEN +: MPRF_XLEN] =
                  wr_data_i[arb_bank*MPRF_XLEN +: MPRF_XLEN];
`ifdef MOSAIC_PRF_MUTANT_IGNORE_STORED_GEN
              rsp_gen_mismatch_o[s] = 1'b0;
`else
              rsp_gen_mismatch_o[s] =
                  (wr_gen_i[arb_bank*MPRF_GEN_W +: MPRF_GEN_W] !=
                   rd_gen_i[s*MPRF_GEN_W +: MPRF_GEN_W]);
`endif
            end else begin
              rsp_data_o[s*MPRF_XLEN +: MPRF_XLEN] =
                  data_q[arb_bank][arb_row];
`ifdef MOSAIC_PRF_MUTANT_IGNORE_STORED_GEN
              // MUTANT: the response echoes the requested generation and never
              // reports a mismatch, so a stale read looks like a live one. The
              // data is still the stored data, so the defect is exactly "the
              // generation is not checked", not "the value is lost".
              rsp_gen_o[s*MPRF_GEN_W +: MPRF_GEN_W] =
                  rd_gen_i[s*MPRF_GEN_W +: MPRF_GEN_W];
              rsp_gen_mismatch_o[s] = 1'b0;
`else
              rsp_gen_o[s*MPRF_GEN_W +: MPRF_GEN_W] =
                  gen_q[arb_bank][arb_row];
              rsp_gen_mismatch_o[s] =
                  (gen_q[arb_bank][arb_row] != rd_gen_i[s*MPRF_GEN_W +: MPRF_GEN_W]);
`endif
            end
          end else begin
`ifdef MOSAIC_PRF_MUTANT_NEVER_WRITTEN_VALID
            // MUTANT: an entry with no value since reset is reported as an
            // ordinary valid read of zero.
            rsp_never_written_o[s] = 1'b0;
`else
            rsp_never_written_o[s] = 1'b1;
`endif
          end
        end
      end
    end
  end

  // ------------------------------------------------------------ the storage
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      for (int b = 0; b < MPRF_BANKS; b++) begin
        valid_q[b] <= '0;
      end
    end else begin
      for (int b = 0; b < MPRF_BANKS; b++) begin
        if (wr_applied[b]) begin
          valid_q[b][wr_row_addr[b]] <= 1'b1;
          gen_q[b][wr_row_addr[b]]   <= wr_gen_i[b*MPRF_GEN_W +: MPRF_GEN_W];
          data_q[b][wr_row_addr[b]]  <= wr_data_i[b*MPRF_XLEN +: MPRF_XLEN];
        end
      end
    end
  end

  // ------------------------------------------------------------ the counters
  always_comb begin
    wr_hits       = 32'd0;
    rd_grants     = 32'd0;
    rd_refusals   = 32'd0;
    mismatch_hits = 32'd0;
    invalid_hits  = 32'd0;
    for (int b = 0; b < MPRF_BANKS; b++) begin
      wr_hits       = wr_hits + (wr_applied[b] ? 32'd1 : 32'd0);
      rd_grants     = rd_grants + (rd_ready_o[b] ? 32'd1 : 32'd0);
      rd_refusals   = rd_refusals + ((rd_valid_i[b] && !rd_ready_o[b]) ? 32'd1 : 32'd0);
      mismatch_hits = mismatch_hits + (rsp_gen_mismatch_o[b] ? 32'd1 : 32'd0);
      invalid_hits  = invalid_hits + (rsp_never_written_o[b] ? 32'd1 : 32'd0);
    end
    busy_f = (rd_refusals != 32'd0);
  end

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      wr_ctr_q       <= 32'd0;
      rd_ctr_q       <= 32'd0;
      conflict_ctr_q <= 32'd0;
      mismatch_ctr_q <= 32'd0;
      invalid_ctr_q  <= 32'd0;
    end else begin
      wr_ctr_q       <= wr_ctr_q       + wr_hits;
      rd_ctr_q       <= rd_ctr_q       + rd_grants;
      conflict_ctr_q <= conflict_ctr_q + rd_refusals;
      mismatch_ctr_q <= mismatch_ctr_q + mismatch_hits;
      invalid_ctr_q  <= invalid_ctr_q  + invalid_hits;
    end
  end

  assign o_wr_ctr       = wr_ctr_q;
  assign o_rd_ctr       = rd_ctr_q;
  assign o_conflict_ctr = conflict_ctr_q;
  assign o_mismatch_ctr = mismatch_ctr_q;
  assign o_invalid_ctr  = invalid_ctr_q;
  assign o_busy         = busy_f;

endmodule : mosaic_prf

`resetall
`default_nettype wire
