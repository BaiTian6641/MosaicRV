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
// without the generation the retire payload could not be read back from the
// durable value stash, which is keyed by ROB index.
//
// --------------------------------------------------------- stale reads
//
// A read of an entry that no live macro owns is impossible to mistake for a
// live one, and it is the ROB that makes it so: both read ports are indexed by
// the ROB's own `head_index` / `head1_index`, and the top only assembles a
// payload for a lane whose `head_valid` is high. Every allocation writes its
// descriptor in the same cycle it allocates the ROB entry, so a live ROB head
// always has a written descriptor. The array itself is deliberately **not
// reset** -- it is DEPTH x WIDTH of storage and the project rule
// (rtl/common/mosaic_ram.sv) puts validity in control bits outside it.
//
// `live` is that validity: set by the allocation write and cleared by the
// retire acknowledgement. It is not a second opinion about ROB occupancy -- this
// store never gates the retire decision (the ROB's own validity does) -- it is
// the pairing counter `o_live_ctr`: allocations and retire-clears must be equal
// and opposite, and a mismatch is the first sign of a retire path losing or
// double-counting an entry.
//
// ------------------------------------------------------------------- ports
//
// Two write ports (the two-wide allocation the ROB will eventually allow) and
// two read ports (the two retire lanes). The clear ports are separate from the
// writes so that a same-cycle clear and write to one index has a single, stated
// order: the write wins, because the write is the new macro and the clear is
// the old one.
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

    // ------------------------------------------------------------- counters
    output logic [31:0]                       o_write_ctr,
    output logic [31:0]                       o_clear_ctr,
    output logic [31:0]                       o_live_ctr
);

  // ------------------------------------------------------------------ storage
  // Descriptor data is not reset; `live` is.
  logic [MD_TAG_W-1:0]   tag_q     [0:MD_ENTRIES-1];
  logic [MD_GEN_W-1:0]   gen_q     [0:MD_ENTRIES-1];
  logic [MD_RD_W-1:0]    rd_q      [0:MD_ENTRIES-1];
  logic                  reg_we_q  [0:MD_ENTRIES-1];
  logic                  store_q   [0:MD_ENTRIES-1];
  logic [MD_ENTRIES-1:0] live_q;

  logic [31:0] write_ctr;
  logic [31:0] clear_ctr;
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
  // One whole next-state vector, so a same-cycle write and clear to one index
  // has a single answer (the write wins) instead of two non-blocking
  // assignments racing.
  logic [MD_ENTRIES-1:0] live_d;

  always_comb begin
    live_d = live_q;
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

  always_ff @(posedge clk) begin
    if (rst) begin
      live_q    <= {MD_ENTRIES{1'b0}};
      write_ctr <= 32'd0;
      clear_ctr <= 32'd0;
      live_ctr  <= 32'd0;
    end else begin
      live_q <= live_d;

      for (int unsigned p = 0; p < 2; p++) begin
        if (wr_valid[p]) begin
          tag_q[wr_index[p]]    <= wr_tag[p];
          gen_q[wr_index[p]]    <= wr_gen[p];
          rd_q[wr_index[p]]     <= wr_rd[p];
          reg_we_q[wr_index[p]] <= wr_reg_we[p];
          store_q[wr_index[p]]  <= wr_is_store[p];
        end
      end

      write_ctr <= write_ctr + {31'd0, wr_valid[0]} + {31'd0, wr_valid[1]};
      clear_ctr <= clear_ctr + {31'd0, clr_valid[0]} + {31'd0, clr_valid[1]};
      live_ctr  <= live_ctr + {31'd0, wr_valid[0]} + {31'd0, wr_valid[1]}
                            - {31'd0, clr_valid[0]} - {31'd0, clr_valid[1]};
    end
  end

  assign o_write_ctr = write_ctr;
  assign o_clear_ctr = clear_ctr;
  assign o_live_ctr  = live_ctr;

endmodule : mosaic_macro_desc

`default_nettype wire
