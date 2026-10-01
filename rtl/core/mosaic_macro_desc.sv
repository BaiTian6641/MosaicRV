// ============================================================================
// mosaic_macro_desc -- the per-macro retire descriptor store (work package I-023).
//
// ------------------------------------------------------------------- why
//
// The retire payload `mosaic_retire` needs is not all in the ROB. The ROB holds
// the *identity* of a macro (its slot generation and its physical destination
// tag), its PC, how many children it has and which have completed -- and
// nothing else. It deliberately does not hold the architectural destination
// `rd`, the `reg_write` bit, or the memory/CSR class flags, because those are
// retire-time facts and the ROB is a delivered, verified package (I-016) whose
// slot has been compared field by field against an independent shadow in
// CASE=rob.out_of_order_children.
//
// Widening the ROB's slot for four retire-only fields would re-open that case:
// every field of the shadow, every sweep of the observation port, and the
// documented width contract would all have to be re-derived, for a functional
// gain of zero -- the ROB would hold the same information, one array further
// from the consumer that needs it. So the retire-only fields live here, keyed by
// the ROB index the allocation already returns, and the ROB is untouched.
//
// The array also carries the physical generation of the destination, because
// the ROB stores only the 7-bit tag and the PRF's identity is (tag, generation):
// without the generation the completion path could not name the destination it
// is writing, and the value stash (mosaic_wb_arbiter) could not be read back for
// the retire event.
//
// --------------------------------------------------------- stale reads
//
// A read of an entry that no live macro owns is impossible to mistake for a
// live one, and it is the ROB that makes it so: every read port is indexed by
// the ROB's own `head_index` / `head1_index`, and the top only assembles a
// payload for a lane whose `head_valid` is high. The array itself is
// deliberately **not reset** -- it is DEPTH x WIDTH of storage and the project
// rule (rtl/common/mosaic_ram.sv) puts validity in control bits outside it.
//
// The validity bit here is `live`: set by the dispatch write that allocates the
// macro, cleared by the retire acknowledgement that removes it, and cleared in
// bulk by the flush walk. It has exactly one meaning -- "a macro is in the ROB
// under this index" -- and it is what the flush walk uses to decide which
// destinations to release. It is *not* a second opinion about ROB occupancy:
// this store never gates the retire decision (the ROB's own validity does), and
// the only cross-check either can make is the conservation counter `o_live_ctr`.
//
// ------------------------------------------------------------------- ports
//
// Two write ports (dispatch allocates one macro per lane per cycle) and two
// read ports (the two retire lanes). The three extra ports are the recovery
// path and are stated rather than implied:
//
//   * `clr_*`   the retire acknowledgement releases the entry. Separate from
//               the writes so a same-cycle clear and write to one index can be
//               ordered explicitly: the write wins, because the write is the
//               new macro and the clear is the old one.
//   * `walk_*`  the flush walk. On a squashing redirect every *live* entry
//               belongs to a macro that is being discarded, and each one's
//               destination tag has to be handed back to mosaic_rename's free
//               list -- rename's own journal restore is not used (see
//               mosaic_core.sv, "recovery"), so the release has to come from
//               somewhere that knows the tags. This store is that place: it
//               already holds (tag, generation) per live macro.
//
// The walk steps one index per cycle and pulses `walk_free_valid` for each live
// entry that owns a physical destination; the caller wires that pulse straight
// onto rename's `free_*` port. The live bit is cleared as the walk passes, so a
// completed walk leaves the store empty and the next allocation starts clean.
// A macro that writes x0 (or writes no register at all) is live but owns no
// destination, so it is cleared without a pulse.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */

localparam int unsigned MD_ENTRIES = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned MD_IDX_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned MD_TAG_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
localparam int unsigned MD_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;
localparam int unsigned MD_RD_W    = 5;
// The walk counter names every index and then one past the end, so it says
// "finished" as well as naming a slot.
localparam int unsigned MD_WALK_W  = $clog2(MD_ENTRIES + 1);

module mosaic_macro_desc (
    input  logic                              clk,
    input  logic                              rst,

    // -------------------------------------------------- two allocation writes
    input  logic [1:0]                        wr_valid,
    input  logic [1:0][MD_IDX_W-1:0]          wr_index,
    input  logic [1:0][MD_TAG_W-1:0]          wr_tag,
    input  logic [1:0][MD_GEN_W-1:0]          wr_gen,
    input  logic [1:0][MD_RD_W-1:0]           wr_rd,
    input  logic [1:0]                        wr_reg_we,
    input  logic [1:0]                        wr_is_store,

    // ------------------------------------------------------- two retire reads
    input  logic [MD_IDX_W-1:0]               rd_index0,
    input  logic [MD_IDX_W-1:0]               rd_index1,
    output logic                              rd_valid0,
    output logic [MD_TAG_W-1:0]               rd_tag0,
    output logic [MD_GEN_W-1:0]               rd_gen0,
    output logic [MD_RD_W-1:0]                rd_rd0,
    output logic                              rd_reg_we0,
    output logic                              rd_is_store0,
    output logic                              rd_valid1,
    output logic [MD_TAG_W-1:0]               rd_tag1,
    output logic [MD_GEN_W-1:0]               rd_gen1,
    output logic [MD_RD_W-1:0]                rd_rd1,
    output logic                              rd_reg_we1,
    output logic                              rd_is_store1,

    // ------------------------------------------------------ two retire clears
    input  logic [1:0]                        clr_valid,
    input  logic [1:0][MD_IDX_W-1:0]          clr_index,

    // ------------------------------------------------------- the flush walk
    input  logic                              walk_start,
    output logic                              walk_busy,
    output logic                              walk_free_valid,
    output logic [MD_TAG_W-1:0]               walk_free_tag,
    output logic [MD_GEN_W-1:0]               walk_free_gen,

    // ------------------------------------------------------------- counters
    output logic [31:0]                       o_write_ctr,
    output logic [31:0]                       o_free_ctr,
    output logic [31:0]                       o_live_ctr
);

  // ------------------------------------------------------------------ storage
  // Descriptor data is not reset; `live` is.
  logic [MD_TAG_W-1:0] tag_q     [MD_ENTRIES];
  logic [MD_GEN_W-1:0] gen_q     [MD_ENTRIES];
  logic [MD_RD_W-1:0]  rd_q      [MD_ENTRIES];
  logic                reg_we_q  [MD_ENTRIES];
  logic                store_q   [MD_ENTRIES];
  logic [MD_ENTRIES-1:0] live_q;

  logic [MD_WALK_W-1:0] walk_q;
  logic                 walk_busy_q;

  logic [31:0] write_ctr;
  logic [31:0] free_ctr;
  logic [31:0] live_ctr;

  // ------------------------------------------------------------------- reads
  // Combinational, gated on the macro's own live bit: a slot nobody has written
  // since reset reports a zeroed descriptor rather than whatever the storage
  // powered up with, so the retire payload assembly is total.
  always_comb begin
    rd_valid0     = live_q[rd_index0];
    rd_tag0       = rd_valid0 ? tag_q[rd_index0]    : {MD_TAG_W{1'b0}};
    rd_gen0       = rd_valid0 ? gen_q[rd_index0]    : {MD_GEN_W{1'b0}};
    rd_rd0        = rd_valid0 ? rd_q[rd_index0]     : {MD_RD_W{1'b0}};
    rd_reg_we0    = rd_valid0 && reg_we_q[rd_index0];
    rd_is_store0  = rd_valid0 && store_q[rd_index0];

    rd_valid1     = live_q[rd_index1];
    rd_tag1       = rd_valid1 ? tag_q[rd_index1]    : {MD_TAG_W{1'b0}};
    rd_gen1       = rd_valid1 ? gen_q[rd_index1]    : {MD_GEN_W{1'b0}};
    rd_rd1        = rd_valid1 ? rd_q[rd_index1]     : {MD_RD_W{1'b0}};
    rd_reg_we1    = rd_valid1 && reg_we_q[rd_index1];
    rd_is_store1  = rd_valid1 && store_q[rd_index1];
  end

  // ------------------------------------------------------------- next state
  // One whole next-state vector per field, so a same-cycle write and clear to
  // one index has a single, stated answer (the write wins) instead of two
  // non-blocking assignments racing. The walk clears as it passes; because the
  // walk is the only thing that runs while dispatch is stalled, no allocation
  // can be racing it.
  logic [MD_ENTRIES-1:0] live_d;
  logic                  walk_pulse;
  logic                  walk_last;

  always_comb begin
    live_d    = live_q;
    walk_pulse = walk_busy_q && live_q[walk_q[MD_IDX_W-1:0]];
    walk_last  = walk_busy_q && (walk_q == MD_WALK_W'(MD_ENTRIES - 1));

    if (walk_busy_q) begin
      live_d[walk_q[MD_IDX_W-1:0]] = 1'b0;
    end

    for (int unsigned p = 0; p < 2; p++) begin
      if (clr_valid[p]) begin
        live_d[clr_index[p]] = 1'b0;
      end
    end

    // Allocation last: it is the newer statement.
    for (int unsigned p = 0; p < 2; p++) begin
      if (wr_valid[p]) begin
        live_d[wr_index[p]] = 1'b1;
      end
    end
  end

  // `walk_free_valid` is the pulse the caller wires to rename's free port. It
  // is only for a live entry that owns a physical destination (`reg_we` and a
  // non-x0 rd, which dispatch has already folded into the write's `wr_tag`
  // validity by passing `reg_we_q` down). An x0 write is live and is walked but
  // releases nothing -- there was no tag to release.
  assign walk_free_valid = walk_pulse && reg_we_q[walk_q[MD_IDX_W-1:0]];
  assign walk_free_tag   = tag_q[walk_q[MD_IDX_W-1:0]];
  assign walk_free_gen   = gen_q[walk_q[MD_IDX_W-1:0]];
  assign walk_busy       = walk_busy_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      live_q      <= {MD_ENTRIES{1'b0}};
      walk_q      <= {MD_WALK_W{1'b0}};
      walk_busy_q <= 1'b0;
      write_ctr   <= 32'd0;
      free_ctr    <= 32'd0;
      live_ctr    <= 32'd0;
    end else begin
      live_q <= live_d;

      // A walk requested in the cycle a previous walk finishes restarts, so two
      // back-to-back flushes cannot drop the second walk.
      if (walk_start) begin
        walk_busy_q <= 1'b1;
        walk_q      <= {MD_WALK_W{1'b0}};
      end else if (walk_busy_q && (walk_last || (walk_q >= MD_WALK_W'(MD_ENTRIES)))) begin
        walk_busy_q <= 1'b0;
        walk_q      <= {MD_WALK_W{1'b0}};
      end else if (walk_busy_q) begin
        walk_q <= walk_q + MD_WALK_W'(1);
      end

      // Counters: allocation writes, releases pulsed by the walk, and the
      // population of the live vector. The last one is the conservation check a
      // leak would show up in first.
      write_ctr <= write_ctr + {31'd0, wr_valid[0]} + {31'd0, wr_valid[1]};
      free_ctr  <= free_ctr  + {31'd0, walk_free_valid};
      live_ctr  <= live_ctr + {31'd0, wr_valid[0]} + {31'd0, wr_valid[1]}
                            - {31'd0, clr_valid[0]} - {31'd0, clr_valid[1]}
                            - {31'd0, walk_pulse};
    end
  end

  assign o_write_ctr = write_ctr;
  assign o_free_ctr  = free_ctr;
  assign o_live_ctr  = live_ctr;

endmodule : mosaic_macro_desc

`default_nettype wire
