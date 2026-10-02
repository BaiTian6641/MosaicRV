// Simulation wrapper for CASE=rob.out_of_order_children (work package I-016).
//
// The wrapper adds no timing of its own. `mosaic_rob` is registered on its
// inputs and combinational on its outputs, so the C++ driver drives the ports,
// evaluates while the clock is low, compares every output against its shadow,
// and then applies the edge. There is no clock generation, no reset generation
// and no `$display` in here: the C++ side owns all three, per
// sim/common/sim_common.h.
//
// --------------------------------------------------------------- geometry
//
// Every driver-facing port is a fixed 32-bit (64-bit for the PC) vector, so the
// C++ driver contains no ROB geometry and would keep compiling if a profile
// changed the depths. Narrowing to the ROB's own port widths needs those widths,
// and they come from **the same generated packages the RTL reads**, by scope
// reference rather than by a second `include`:
//
//   * the generated header declares `package mosaic_cfg_pkg` with no include
//     guard, so including it in a second file in the same compilation is a
//     duplicate package declaration (Verilator MODDUP, slang
//     -Wduplicate-definition) -- both are errors here, not warnings;
//   * referring to `mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES` after the package has
//     been declared needs no include at all, and both tools accept it. Verilator
//     requires the declaring file to come first on the command line, which
//     tools/run_unit.py already guarantees for every case.
//
// So there is exactly one geometry in the compilation: the generated packages.
// This file derives its port widths from them, and then *reads the elaborated
// values back* as `o_*_w` outputs so the driver can check the two against each
// other instead of trusting the derivation.
//
// The identity widths are **not** re-derived by any rule of this wrapper's own.
// A wrapper that wrote `ID_W = 2 * INDEX_W` would repeat whatever formula the
// RTL used, so it would keep agreeing with a ROB whose generation width had
// drifted from the I-002 contract -- which is exactly how this wrapper failed to
// catch a 12-bit ROB generation against a 7-bit contract. `TAG_W` and `GEN_W`
// therefore each name their own contract constant, and a tag port is narrowed
// with `TAG_W` while a generation port is narrowed with `GEN_W`; the two are not
// interchangeable even where p0 happens to give them the same value.

`default_nettype none
`resetall

module mosaic_rob_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------------------ drive
    input  logic [31:0] alloc_num_uops_i,
    input  logic [31:0] alloc_tag_i,
    input  logic [63:0] alloc_pc_i,
    input  logic        alloc_valid_i,
    input  logic        alloc_exc_i,
    input  logic        alloc_open_i,

    input  logic [31:0] close_index_i,
    input  logic [31:0] close_gen_i,
    input  logic        close_valid_i,

    input  logic [31:0] cmp_index_i,
    input  logic [31:0] cmp_gen_i,
    input  logic [31:0] cmp_uop_i,
    input  logic        cmp_valid_i,
    input  logic        cmp_exc_i,

    input  logic        retire_req_i,
    input  logic        flush_valid_i,
    input  logic [31:0] obs_index_i,

    // ---------------------------------------------------------------- observe
    output logic        alloc_ok_o,
    output logic        alloc_refused_o,
    output logic        alloc_full_o,
    output logic        alloc_bad_uops_o,
    output logic [31:0] alloc_index_o,
    output logic [31:0] alloc_gen_o,

    output logic        close_ok_o,
    output logic        close_stale_o,

    output logic        cmp_accepted_o,
    output logic        cmp_duplicate_o,
    output logic        cmp_stale_o,
    output logic        cmp_bad_uop_o,

    output logic        retire_ack_o,
    output logic        head_valid_o,
    output logic        head_ready_o,
    output logic        head_replay_o,
    output logic        head_complete_o,
    output logic        head_exc_o,
    output logic        head_closed_o,
    output logic [31:0] head_index_o,
    output logic [31:0] head_gen_o,
    output logic [31:0] head_tag_o,
    output logic [63:0] head_pc_o,
    output logic [31:0] head_num_uops_o,
    output logic [31:0] head_done_mask_o,
    output logic [31:0] head_done_cnt_o,

    output logic        obs_valid_o,
    output logic [31:0] obs_gen_o,
    output logic [31:0] obs_tag_o,
    output logic [63:0] obs_pc_o,
    output logic [31:0] obs_num_uops_o,
    output logic [31:0] obs_done_mask_o,
    output logic [31:0] obs_done_cnt_o,
    output logic        obs_exc_o,
    output logic        obs_closed_o,

    output logic [31:0] o_head_ptr_o,
    output logic [31:0] o_alloc_ptr_o,
    output logic [31:0] o_occupied_o,
    output logic [31:0] o_free_o,
    output logic [31:0] o_alloc_total_o,
    output logic [31:0] o_retired_total_o,
    output logic [31:0] o_squashed_total_o,
    output logic [31:0] o_gen_counter_o,

    // ---------------------------------------------------------------- geometry
    output logic [31:0] o_rob_entries_o,
    output logic [31:0] o_index_w_o,
    output logic [31:0] o_max_uops_o,
    output logic [31:0] o_tag_w_o,
    output logic [31:0] o_gen_w_o,
    output logic [31:0] o_pc_w_o,
    output logic [31:0] o_num_uops_w_o,
    output logic [31:0] o_occ_w_o,

    // ------------------------------------------- I-017: the second retire lane
    // Lane 1's qualification view, exported for observation. The case does not
    // read it (it drives lane 1 inactive) but a floating *output* is harmless
    // while a floating *input* would be random, and pinning every new pin is
    // what keeps this build warning-free.
    output logic        head1_valid_o,
    output logic        head1_ready_o,
    output logic        head1_replay_o,
    output logic        head1_complete_o,
    output logic        head1_exc_o,
    output logic        head1_closed_o,
    output logic [31:0] head1_index_o,
    output logic [31:0] head1_gen_o,
    output logic [31:0] head1_tag_o,
    output logic [63:0] head1_pc_o,
    output logic [31:0] head1_num_uops_o,
    output logic [31:0] head1_done_mask_o,
    output logic [31:0] head1_done_cnt_o,
    output logic        retire_ack_next_o
);


  // --------------------------------------------------------------- I-017 pins
  //
  // `mosaic_rob` gained a second retirement lane and the `head1_*` view beside
  // it (see the "second head view" block in rtl/core/mosaic_rob.sv). This
  // wrapper drives lane 1 **inactive** and exports the view, so this case keeps
  // testing exactly what it tested before: single-lane retirement. The lane is
  // exercised where its contract lives -- CASE=commit.head_block_and_dual
  // (sim/tb/mosaic_retire_tb.sv), which instantiates the same `mosaic_rob` and
  // drives both lanes against an independent shadow of the two-lane pop.
  //
  // Tying `retire_req_next` to a constant rather than to an undriven input is
  // deliberate: the runner builds with `--x-initial unique`, so a pin left
  // floating would be a random value every run rather than a reproducible
  // zero.
  // The second lane's view, declared here and exported to the observation side.

  // The same contract widths mosaic_rob takes from the same generated package.
  // TAG_W and GEN_W are separate constants on purpose: a port that carries a tag
  // is narrowed with TAG_W and a port that carries a generation with GEN_W, so
  // a tag and a generation can never be silently interchanged, and neither is
  // re-derived by a formula this wrapper invented.
  localparam int unsigned ENTRIES = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
  localparam int unsigned INDEX_W = mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W;
  localparam int unsigned MAX_UOPS = mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO;
  localparam int unsigned TAG_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
  localparam int unsigned GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;

  localparam int unsigned CNT_W = $clog2(MAX_UOPS + 1);
  localparam int unsigned UOP_W = (MAX_UOPS <= 1) ? 1 : $clog2(MAX_UOPS);
  localparam int unsigned OCC_W = $clog2(ENTRIES + 1);
  localparam int unsigned BIT_W = MAX_UOPS;

  // The driver hands over 32-bit values and the ROB sees exactly the low bits its
  // own ports declare. Truncating part selects, not casts, so nothing is silent.
  logic [CNT_W-1:0]    alloc_num_uops;
  logic [TAG_W-1:0]    alloc_tag;
  logic [INDEX_W-1:0]  close_index;
  logic [GEN_W-1:0]    close_gen;
  logic [INDEX_W-1:0]  cmp_index;
  logic [GEN_W-1:0]    cmp_gen;
  logic [UOP_W-1:0]    cmp_uop;
  logic [INDEX_W-1:0]  obs_index;
  logic [BIT_W-1:0]    head_done_mask;
  logic [BIT_W-1:0]    obs_done_mask;
  logic [63:0]         head1_pc;

  // The second lane's view, declared here and exported to the observation side.
  logic                retire_req_next;
  logic [INDEX_W-1:0]  head1_index;
  logic [GEN_W-1:0]    head1_gen;
  logic [TAG_W-1:0]    head1_tag;
  logic [CNT_W-1:0]    head1_num_uops;
  logic [BIT_W-1:0]    head1_done_mask;
  logic [CNT_W-1:0]    head1_done_cnt;

  // Tying the request to a constant rather than leaving the pin floating is
  // deliberate: the runner builds with `--x-initial unique`, so an undriven
  // input would be a random value every run instead of a reproducible zero.
  assign retire_req_next = 1'b0;
  assign alloc_num_uops = alloc_num_uops_i[CNT_W-1:0];
  assign alloc_tag      = alloc_tag_i[TAG_W-1:0];
  assign close_index    = close_index_i[INDEX_W-1:0];
  assign close_gen      = close_gen_i[GEN_W-1:0];
  assign cmp_index      = cmp_index_i[INDEX_W-1:0];
  assign cmp_gen        = cmp_gen_i[GEN_W-1:0];
  assign cmp_uop        = cmp_uop_i[UOP_W-1:0];
  assign obs_index      = obs_index_i[INDEX_W-1:0];

  mosaic_rob u_rob (
      .clk              (clk),
      .rst              (rst),

      .alloc_valid      (alloc_valid_i),
      .alloc_tag        (alloc_tag),
      .alloc_pc         (alloc_pc_i),
      .alloc_num_uops   (alloc_num_uops),
      .alloc_exc        (alloc_exc_i),
      .alloc_open       (alloc_open_i),
      .alloc_ok         (alloc_ok_o),
      .alloc_refused    (alloc_refused_o),
      .alloc_full       (alloc_full_o),
      .alloc_bad_uops   (alloc_bad_uops_o),
      .alloc_index      (alloc_index_o[INDEX_W-1:0]),
      .alloc_gen        (alloc_gen_o[GEN_W-1:0]),
      // The second allocation port (added with the two-wide front end). This
      // wrapper drives one allocation at a time, so lane 1 is tied off; the port
      // is connected rather than left dangling so the module's full interface is
      // visible here.
      .alloc2_valid    (1'b0),
      .alloc2_tag      ({TAG_W{1'b0}}),
      .alloc2_pc       (64'd0),
      .alloc2_num_uops (CNT_W'(1)),
      .alloc2_exc      (1'b0),
      .alloc2_open     (1'b0),
      .alloc2_ok       (),
      .alloc2_refused  (),
      .alloc2_bad_uops (),
      .alloc2_index    (),
      .alloc2_gen      (),

      .close_valid      (close_valid_i),
      .close_index      (close_index),
      .close_gen        (close_gen),
      .close_ok         (close_ok_o),
      .close_stale      (close_stale_o),

      .cmp_valid        (cmp_valid_i),
      .cmp_index        (cmp_index),
      .cmp_gen          (cmp_gen),
      .cmp_uop          (cmp_uop),
      .cmp_exc          (cmp_exc_i),
      .cmp_accepted     (cmp_accepted_o),
      .cmp_duplicate    (cmp_duplicate_o),
      .cmp_stale        (cmp_stale_o),
      .cmp_bad_uop      (cmp_bad_uop_o),

      .retire_req       (retire_req_i),
      .retire_ack       (retire_ack_o),
      .head_valid       (head_valid_o),
      .head_ready       (head_ready_o),
      .head_replay      (head_replay_o),
      .head_complete    (head_complete_o),
      .head_exc         (head_exc_o),
      .head_closed      (head_closed_o),
      .head_index       (head_index_o[INDEX_W-1:0]),
      .head_gen         (head_gen_o[GEN_W-1:0]),
      .head_tag         (head_tag_o[TAG_W-1:0]),
      .head_pc          (head_pc_o),
      .head_num_uops    (head_num_uops_o[CNT_W-1:0]),
      .head_done_mask   (head_done_mask),
      .head_done_cnt    (head_done_cnt_o[CNT_W-1:0]),
      .head1_valid      (head1_valid_o),
      .head1_ready      (head1_ready_o),
      .head1_replay     (head1_replay_o),
      .head1_complete   (head1_complete_o),
      .head1_exc        (head1_exc_o),
      .head1_closed     (head1_closed_o),
      .head1_index      (head1_index),
      .head1_gen        (head1_gen),
      .head1_tag        (head1_tag),
      .head1_pc         (head1_pc_o),
      .head1_num_uops   (head1_num_uops),
      .head1_done_mask  (head1_done_mask),
      .head1_done_cnt   (head1_done_cnt),

      .retire_req_next  (retire_req_next),
      .retire_ack_next  (retire_ack_next_o),

      .flush_valid      (flush_valid_i),

      .obs_index        (obs_index),
      .obs_valid        (obs_valid_o),
      .obs_gen          (obs_gen_o[GEN_W-1:0]),
      .obs_tag          (obs_tag_o[TAG_W-1:0]),
      .obs_pc           (obs_pc_o),
      .rd_index_i       ({INDEX_W{1'b0}}),
      .rd_pc_o          (),
      .obs_num_uops     (obs_num_uops_o[CNT_W-1:0]),
      .obs_done_mask    (obs_done_mask),
      .obs_done_cnt     (obs_done_cnt_o[CNT_W-1:0]),
      .obs_exc          (obs_exc_o),
      .obs_closed       (obs_closed_o),

      .o_head_ptr       (o_head_ptr_o[INDEX_W-1:0]),
      .o_alloc_ptr      (o_alloc_ptr_o[INDEX_W-1:0]),
      .o_occupied       (o_occupied_o[OCC_W-1:0]),
      .o_free           (o_free_o[OCC_W-1:0]),
      .o_alloc_total    (o_alloc_total_o),
      .o_retired_total  (o_retired_total_o),
      .o_squashed_total (o_squashed_total_o),
      .o_gen_counter    (o_gen_counter_o)
  );

  // MAX_UOPS is 8 for p0 and need not be a power of two in a later profile, so
  // the two bitmap ports are zero-extended explicitly rather than relying on a
  // width that happens to line up.
  assign head_done_mask_o = {{(32 - BIT_W) {1'b0}}, head_done_mask};
  assign obs_done_mask_o  = {{(32 - BIT_W) {1'b0}}, obs_done_mask};

  // Read back from the elaborated instance rather than from the derivation above,
  // so the driver can check the two against each other. A geometry file change
  // that desynchronised them would show up here as a failing check rather than
  // as a testbench quietly comparing the wrong field.
  assign o_rob_entries_o = 32'(u_rob.ROB_ENTRIES);
  assign o_index_w_o     = 32'(u_rob.ROB_INDEX_W);
  assign o_max_uops_o    = 32'(u_rob.MAX_UOPS);
  assign o_tag_w_o       = 32'(u_rob.TAG_W);
  assign o_gen_w_o       = 32'(u_rob.GEN_W);
  assign o_pc_w_o        = 32'(u_rob.XLEN);
  assign o_num_uops_w_o  = 32'(u_rob.CNT_W);
  assign o_occ_w_o       = 32'(u_rob.OCC_W);

endmodule : mosaic_rob_tb

`resetall
`default_nettype wire
