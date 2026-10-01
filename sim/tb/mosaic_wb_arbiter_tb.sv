// Simulation wrapper for CASE=wb.same_bank_many_producers (work package I-026).
//
// The wrapper adds no timing of its own. `mosaic_wb_arbiter` is registered on
// its inputs and combinational on its outputs, `mosaic_prf` is the same, so the
// C++ driver drives the ports, evaluates while the clock is low, compares every
// output against its shadow, and then applies the edge. There is no clock
// generation, no reset generation and no `$display` in here: the C++ side owns
// all three, per sim/common/sim_common.h.
//
// --------------------------------------------------------------- two DUTs
//
// The case is about the *pair*: the arbiter is the thing that decides what may
// be written and publishes the value-visible wakeup, and the register file is
// the thing that must actually hold the value before any consumer is told it
// exists. So this wrapper wires them together exactly as the core does -- the
// arbiter's `prf_wr_*` outputs are the register file's write ports -- and
// exports **both** sides to the driver:
//
//   * the arbiter's write-port presentation (`prf_wr_*`), so the driver can
//     check the ports carry the value the wakeup advertises in the same cycle;
//   * the register file's read ports, so the driver can read the value back out
//     of real storage with the matching (tag, generation) instead of trusting
//     the write port.
//
// A testbench that checked the arbiter against a C++ model of the register file
// would prove the two models agree. Reading the elaborated `mosaic_prf` back
// proves the value is where a consumer would find it.
//
// --------------------------------------------------------------- geometry
//
// Every driver-facing port is a fixed 32-bit (64-bit for data) vector, so the
// C++ driver contains no geometry and would keep compiling if a profile changed
// the depths. Narrowing to the DUTs' own port widths needs those widths, and
// they come from **the same generated packages the RTL reads**, by scope
// reference rather than by a second `include`:
//
//   * `mosaic_cfg_pkg.svh` and `mosaic_id_pkg.svh` are declared by the RTL files
//     this case compiles, so a `mosaic_cfg_pkg::MOSAIC_X` reference here needs no
//     include of its own;
//   * `mosaic_uop_pkg` is the frozen completion packet. The arbiter names it but
//     does not include it (the core build lists it as its own source), and the
//     case's RTL list is only the arbiter and the register file, so this wrapper
//     is where the packet is pulled in. It has an include guard, and the generated
//     header it in turn pulls in (which does *not*) is only reached once because
//     the arbiter's own copy of that header is deduplicated by the elaborator.
//
// The identity widths are **not** re-derived by any rule of this wrapper's own.
// A destination tag is narrowed with the tag contract width, a PRF generation
// with the PRF generation width and the rename generation with the *rename*
// generation width; the three are separate constants because they are separate
// contracts, and p0 does not make them interchangeable.

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_uop_pkg.sv"
/* verilator lint_on UNUSEDPARAM */

`default_nettype none
`resetall

module mosaic_wb_arbiter_tb (
    input  logic        clk,
    input  logic        rst,

    // ================================================================ producers
    // One flattened completion per producer. `wb0` is cluster 0, `wb1` is
    // cluster 1, `wb2` is the shared MUL/DIV unit; the case drives 0 and 1.
    input  logic        wb_valid0_i,
    input  logic [31:0] wb0_rob_index_i,
    input  logic [31:0] wb0_rob_gen_i,
    input  logic [31:0] wb0_uop_index_i,
    input  logic [31:0] wb0_tag_i,
    input  logic [31:0] wb0_gen_i,
    input  logic        wb0_x0_i,
    input  logic        wb0_value_valid_i,
    input  logic [63:0] wb0_value_i,
    input  logic        wb0_exc_valid_i,
    input  logic        wb0_is_store_i,
    input  logic        wb0_is_load_i,

    input  logic        wb_valid1_i,
    input  logic [31:0] wb1_rob_index_i,
    input  logic [31:0] wb1_rob_gen_i,
    input  logic [31:0] wb1_uop_index_i,
    input  logic [31:0] wb1_tag_i,
    input  logic [31:0] wb1_gen_i,
    input  logic        wb1_x0_i,
    input  logic        wb1_value_valid_i,
    input  logic [63:0] wb1_value_i,
    input  logic        wb1_exc_valid_i,
    input  logic        wb1_is_store_i,
    input  logic        wb1_is_load_i,

    input  logic        wb_valid2_i,
    input  logic [31:0] wb2_rob_index_i,
    input  logic [31:0] wb2_rob_gen_i,
    input  logic [31:0] wb2_uop_index_i,
    input  logic [31:0] wb2_tag_i,
    input  logic [31:0] wb2_gen_i,
    input  logic        wb2_x0_i,
    input  logic        wb2_value_valid_i,
    input  logic [63:0] wb2_value_i,
    input  logic        wb2_exc_valid_i,
    input  logic        wb2_is_store_i,
    input  logic        wb2_is_load_i,

    // ----------------------------------------------------- rename writeback
    input  logic        ren_wb_accepted_i,
    input  logic        ren_wb_stale_i,
    input  logic        ren_wb_duplicate_i,

    // ------------------------------------------------------- ROB completion
    input  logic        rob_cmp_accepted_i,
    input  logic        rob_cmp_duplicate_i,
    input  logic        rob_cmp_stale_i,
    input  logic        rob_cmp_bad_uop_i,

    // ------------------------------------------------------- ready-table query
    input  logic [3:0]  q_valid_i,
    input  logic [31:0] q_tag0_i,
    input  logic [31:0] q_tag1_i,
    input  logic [31:0] q_tag2_i,
    input  logic [31:0] q_tag3_i,
    input  logic [31:0] q_gen0_i,
    input  logic [31:0] q_gen1_i,
    input  logic [31:0] q_gen2_i,
    input  logic [31:0] q_gen3_i,

    // ------------------------------------------------- durable value stash (read)
    input  logic [31:0] stash_rd0_i,
    input  logic [31:0] stash_rd1_i,

    // --------------------------------------------------- register-file read ports
    input  logic [3:0]  prf_rd_valid_i,
    input  logic [31:0] prf_rd_tag0_i,
    input  logic [31:0] prf_rd_tag1_i,
    input  logic [31:0] prf_rd_tag2_i,
    input  logic [31:0] prf_rd_tag3_i,
    input  logic [31:0] prf_rd_gen0_i,
    input  logic [31:0] prf_rd_gen1_i,
    input  logic [31:0] prf_rd_gen2_i,
    input  logic [31:0] prf_rd_gen3_i,

    // ================================================================ observe
    output logic        wb_ready0_o,
    output logic        wb_ready1_o,
    output logic        wb_ready2_o,

    output logic        prf_wr_en0_o,
    output logic        prf_wr_en1_o,
    output logic        prf_wr_en2_o,
    output logic        prf_wr_en3_o,
    output logic        prf_wr_gen_valid0_o,
    output logic        prf_wr_gen_valid1_o,
    output logic        prf_wr_gen_valid2_o,
    output logic        prf_wr_gen_valid3_o,
    output logic [31:0] prf_wr_tag0_o,
    output logic [31:0] prf_wr_tag1_o,
    output logic [31:0] prf_wr_tag2_o,
    output logic [31:0] prf_wr_tag3_o,
    output logic [31:0] prf_wr_gen0_o,
    output logic [31:0] prf_wr_gen1_o,
    output logic [31:0] prf_wr_gen2_o,
    output logic [31:0] prf_wr_gen3_o,
    output logic [63:0] prf_wr_data0_o,
    output logic [63:0] prf_wr_data1_o,
    output logic [63:0] prf_wr_data2_o,
    output logic [63:0] prf_wr_data3_o,

    output logic        ren_wb_valid_o,
    output logic [31:0] ren_wb_tag_o,
    output logic [31:0] ren_wb_gen_o,

    output logic        rob_cmp_valid_o,
    output logic [31:0] rob_cmp_index_o,
    output logic [31:0] rob_cmp_gen_o,
    output logic [31:0] rob_cmp_uop_o,
    output logic        rob_cmp_exc_o,

    output logic        wu_valid_o,
    output logic [31:0] wu_tag_o,
    output logic [31:0] wu_gen_o,
    output logic [63:0] wu_val_o,

    output logic [3:0]  q_written_o,

    output logic        stash_valid0_o,
    output logic [63:0] stash_value0_o,
    output logic        stash_valid1_o,
    output logic [63:0] stash_value1_o,

    output logic [31:0] o_wr_ctr_o,
    output logic [31:0] o_wake_ctr_o,
    output logic [31:0] o_stale_ctr_o,
    output logic [31:0] o_dup_ctr_o,
    output logic [31:0] o_rob_stale_ctr_o,
    output logic [31:0] o_rob_dup_ctr_o,
    output logic [31:0] o_rob_ok_ctr_o,
    output logic [31:0] o_collision_ctr_o,
    output logic [31:0] o_drop_ctr_o,
    output logic [31:0] o_pub_ctr_o,
    output logic [31:0] o_wide_gen_ctr_o,
    output logic [31:0] o_rob_bad_ctr_o,
    output logic [31:0] o_store_ctr_o,
    output logic [31:0] o_load_ctr_o,

    output logic        o_pub_valid_o,
    output logic [31:0] o_pub_index_o,
    output logic [31:0] o_pub_gen_o,
    output logic [63:0] o_pub_value_o,

    // ------------------------------------------------- register-file read side
    output logic [3:0]  prf_rd_ready_o,
    output logic [3:0]  prf_rsp_valid_o,
    output logic [31:0] prf_rsp_tag0_o,
    output logic [31:0] prf_rsp_tag1_o,
    output logic [31:0] prf_rsp_tag2_o,
    output logic [31:0] prf_rsp_tag3_o,
    output logic [31:0] prf_rsp_gen0_o,
    output logic [31:0] prf_rsp_gen1_o,
    output logic [31:0] prf_rsp_gen2_o,
    output logic [31:0] prf_rsp_gen3_o,
    output logic [63:0] prf_rsp_data0_o,
    output logic [63:0] prf_rsp_data1_o,
    output logic [63:0] prf_rsp_data2_o,
    output logic [63:0] prf_rsp_data3_o,
    output logic [3:0]  prf_rsp_gen_mismatch_o,
    output logic [3:0]  prf_rsp_never_written_o,

    output logic [31:0] prf_o_wr_ctr_o,
    output logic [31:0] prf_o_rd_ctr_o,
    output logic [31:0] prf_o_conflict_ctr_o,
    output logic [31:0] prf_o_mismatch_ctr_o,
    output logic [31:0] prf_o_invalid_ctr_o,
    output logic        prf_o_busy_o,

    // ---------------------------------------------------------------- geometry
    output logic [31:0] o_banks_o,
    output logic [31:0] o_prf_entries_o,
    output logic [31:0] o_prf_tag_w_o,
    output logic [31:0] o_prf_gen_w_o,
    output logic [31:0] o_igen_w_o,
    output logic [31:0] o_xlen_o,
    output logic [31:0] o_rob_entries_o,
    output logic [31:0] o_rob_index_w_o,
    output logic [31:0] o_rob_gen_w_o,
    output logic [31:0] o_uop_w_o,
    output logic [31:0] o_cmp_uop_w_o
);

  // The contract widths, each named from the package that owns it. A tag port is
  // narrowed with TAG_W, a PRF generation with PGEN_W and the rename/ready-table
  // generation with IGEN_W; none of the three is re-derived from another.
  localparam int unsigned TAG_W  = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;      // 7
  localparam int unsigned PGEN_W = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;      // 8
  localparam int unsigned IGEN_W = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;    // 7
  localparam int unsigned IDX_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;    // 6
  localparam int unsigned RGEN_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;      // 7
  localparam int unsigned UOP_W  = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;    // 3
  localparam int unsigned BANKS  = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;        // 4
  localparam int unsigned XLEN   = mosaic_cfg_pkg::MOSAIC_XLEN;             // 64

  // --------------------------------------------------------- assembled events
  mosaic_uop_pkg::wb_event_t ev0;
  mosaic_uop_pkg::wb_event_t ev1;
  mosaic_uop_pkg::wb_event_t ev2;

  always_comb begin
    ev0.id.hart        = 1'b0;
    ev0.id.rob_index   = IDX_W'(wb0_rob_index_i);
    ev0.id.rob_gen     = RGEN_W'(wb0_rob_gen_i);
    ev0.id.uop_index   = UOP_W'(wb0_uop_index_i);
    ev0.dst.tag        = TAG_W'(wb0_tag_i);
    ev0.dst.gen        = PGEN_W'(wb0_gen_i);
    ev0.dst.x0         = wb0_x0_i;
    ev0.value_valid    = wb0_value_valid_i;
    ev0.value          = wb0_value_i;
    ev0.exc.valid      = wb0_exc_valid_i;
    ev0.exc.cause      = '0;
    ev0.exc.tval       = '0;
    ev0.is_store       = wb0_is_store_i;
    ev0.is_load        = wb0_is_load_i;

    ev1.id.hart        = 1'b0;
    ev1.id.rob_index   = IDX_W'(wb1_rob_index_i);
    ev1.id.rob_gen     = RGEN_W'(wb1_rob_gen_i);
    ev1.id.uop_index   = UOP_W'(wb1_uop_index_i);
    ev1.dst.tag        = TAG_W'(wb1_tag_i);
    ev1.dst.gen        = PGEN_W'(wb1_gen_i);
    ev1.dst.x0         = wb1_x0_i;
    ev1.value_valid    = wb1_value_valid_i;
    ev1.value          = wb1_value_i;
    ev1.exc.valid      = wb1_exc_valid_i;
    ev1.exc.cause      = '0;
    ev1.exc.tval       = '0;
    ev1.is_store       = wb1_is_store_i;
    ev1.is_load        = wb1_is_load_i;

    ev2.id.hart        = 1'b0;
    ev2.id.rob_index   = IDX_W'(wb2_rob_index_i);
    ev2.id.rob_gen     = RGEN_W'(wb2_rob_gen_i);
    ev2.id.uop_index   = UOP_W'(wb2_uop_index_i);
    ev2.dst.tag        = TAG_W'(wb2_tag_i);
    ev2.dst.gen        = PGEN_W'(wb2_gen_i);
    ev2.dst.x0         = wb2_x0_i;
    ev2.value_valid    = wb2_value_valid_i;
    ev2.value          = wb2_value_i;
    ev2.exc.valid      = wb2_exc_valid_i;
    ev2.exc.cause      = '0;
    ev2.exc.tval       = '0;
    ev2.is_store       = wb2_is_store_i;
    ev2.is_load        = wb2_is_load_i;
  end

  // ------------------------------------------------------------- ready table
  logic [3:0][TAG_W-1:0]  q_tag;
  logic [3:0][IGEN_W-1:0] q_gen;
  always_comb begin
    q_tag[0] = TAG_W'(q_tag0_i);
    q_tag[1] = TAG_W'(q_tag1_i);
    q_tag[2] = TAG_W'(q_tag2_i);
    q_tag[3] = TAG_W'(q_tag3_i);
    q_gen[0] = IGEN_W'(q_gen0_i);
    q_gen[1] = IGEN_W'(q_gen1_i);
    q_gen[2] = IGEN_W'(q_gen2_i);
    q_gen[3] = IGEN_W'(q_gen3_i);
  end

  // ------------------------------------------------------- arbiter write port
  logic [BANKS-1:0]            arb_wr_en;
  logic [BANKS-1:0]            arb_wr_gen_valid;
  logic [BANKS*TAG_W-1:0]      arb_wr_tag;
  logic [BANKS*PGEN_W-1:0]     arb_wr_gen;
  logic [BANKS*XLEN-1:0]       arb_wr_data;

  // -------------------------------------------------- register-file read ports
  logic [BANKS-1:0]            rd_valid;
  logic [BANKS*TAG_W-1:0]      rd_tag;
  logic [BANKS*PGEN_W-1:0]     rd_gen;

  logic [BANKS*TAG_W-1:0]      rsp_tag;
  logic [BANKS*PGEN_W-1:0]     rsp_gen;
  logic [BANKS*XLEN-1:0]       rsp_data;

  always_comb begin
    rd_valid[0] = prf_rd_valid_i[0];
    rd_valid[1] = prf_rd_valid_i[1];
    rd_valid[2] = prf_rd_valid_i[2];
    rd_valid[3] = prf_rd_valid_i[3];
    rd_tag[0*TAG_W +: TAG_W] = TAG_W'(prf_rd_tag0_i);
    rd_tag[1*TAG_W +: TAG_W] = TAG_W'(prf_rd_tag1_i);
    rd_tag[2*TAG_W +: TAG_W] = TAG_W'(prf_rd_tag2_i);
    rd_tag[3*TAG_W +: TAG_W] = TAG_W'(prf_rd_tag3_i);
    rd_gen[0*PGEN_W +: PGEN_W] = PGEN_W'(prf_rd_gen0_i);
    rd_gen[1*PGEN_W +: PGEN_W] = PGEN_W'(prf_rd_gen1_i);
    rd_gen[2*PGEN_W +: PGEN_W] = PGEN_W'(prf_rd_gen2_i);
    rd_gen[3*PGEN_W +: PGEN_W] = PGEN_W'(prf_rd_gen3_i);
  end

  mosaic_wb_arbiter u_arb (
      .clk              (clk),
      .rst              (rst),

      .wb_ev0           (ev0),
      .wb_valid0        (wb_valid0_i),
      .wb_ready0        (wb_ready0_o),
      .wb_ev1           (ev1),
      .wb_valid1        (wb_valid1_i),
      .wb_ready1        (wb_ready1_o),
      .wb_ev2           (ev2),
      .wb_valid2        (wb_valid2_i),
      .wb_ready2        (wb_ready2_o),

      .prf_wr_en        (arb_wr_en),
      .prf_wr_gen_valid (arb_wr_gen_valid),
      .prf_wr_tag       (arb_wr_tag),
      .prf_wr_gen       (arb_wr_gen),
      .prf_wr_data      (arb_wr_data),

      .ren_wb_valid     (ren_wb_valid_o),
      .ren_wb_tag       (ren_wb_tag_o[TAG_W-1:0]),
      .ren_wb_gen       (ren_wb_gen_o[IGEN_W-1:0]),
      .ren_wb_accepted  (ren_wb_accepted_i),
      .ren_wb_stale     (ren_wb_stale_i),
      .ren_wb_duplicate (ren_wb_duplicate_i),

      .rob_cmp_valid    (rob_cmp_valid_o),
      .rob_cmp_index    (rob_cmp_index_o[IDX_W-1:0]),
      .rob_cmp_gen      (rob_cmp_gen_o[RGEN_W-1:0]),
      .rob_cmp_uop      (rob_cmp_uop_o[UOP_W-1:0]),
      .rob_cmp_exc      (rob_cmp_exc_o),
      .rob_cmp_accepted (rob_cmp_accepted_i),
      .rob_cmp_duplicate(rob_cmp_duplicate_i),
      .rob_cmp_stale    (rob_cmp_stale_i),
      .rob_cmp_bad_uop  (rob_cmp_bad_uop_i),

      .o_store_ctr      (o_store_ctr_o),
      .o_load_ctr       (o_load_ctr_o),

      .wu_valid         (wu_valid_o),
      .wu_tag           (wu_tag_o[TAG_W-1:0]),
      .wu_gen           (wu_gen_o[IGEN_W-1:0]),
      .wu_val           (wu_val_o),

      .q_valid          (q_valid_i),
      .q_tag            (q_tag),
      .q_gen            (q_gen),
      .q_written        (q_written_o),

      .stash_rd0        (stash_rd0_i[IDX_W-1:0]),
      .stash_rd1        (stash_rd1_i[IDX_W-1:0]),
      .stash_valid0     (stash_valid0_o),
      .stash_value0     (stash_value0_o),
      .stash_valid1     (stash_valid1_o),
      .stash_value1     (stash_value1_o),

      .o_wr_ctr         (o_wr_ctr_o),
      .o_wake_ctr       (o_wake_ctr_o),
      .o_stale_ctr      (o_stale_ctr_o),
      .o_dup_ctr        (o_dup_ctr_o),
      .o_rob_stale_ctr  (o_rob_stale_ctr_o),
      .o_rob_dup_ctr    (o_rob_dup_ctr_o),
      .o_rob_ok_ctr     (o_rob_ok_ctr_o),
      .o_collision_ctr  (o_collision_ctr_o),
      .o_drop_ctr       (o_drop_ctr_o),
      .o_pub_ctr        (o_pub_ctr_o),
      .o_wide_gen_ctr   (o_wide_gen_ctr_o),
      .o_rob_bad_ctr    (o_rob_bad_ctr_o),
      .o_pub_valid      (o_pub_valid_o),
      .o_pub_index      (o_pub_index_o[IDX_W-1:0]),
      .o_pub_gen        (o_pub_gen_o[RGEN_W-1:0]),
      .o_pub_value      (o_pub_value_o)
  );

  mosaic_prf u_prf (
      .clk_i                (clk),
      .rst_i                (rst),

      .wr_en_i              (arb_wr_en),
      .wr_gen_valid_i       (arb_wr_gen_valid),
      .wr_tag_i             (arb_wr_tag),
      .wr_gen_i             (arb_wr_gen),
      .wr_data_i            (arb_wr_data),

      .rd_valid_i           (rd_valid),
      .rd_ready_o           (prf_rd_ready_o),
      .rd_tag_i             (rd_tag),
      .rd_gen_i             (rd_gen),
      .rsp_valid_o          (prf_rsp_valid_o),
      .rsp_tag_o            (rsp_tag),
      .rsp_gen_o            (rsp_gen),
      .rsp_data_o           (rsp_data),
      .rsp_gen_mismatch_o   (prf_rsp_gen_mismatch_o),
      .rsp_never_written_o  (prf_rsp_never_written_o),

      .o_wr_ctr             (prf_o_wr_ctr_o),
      .o_rd_ctr             (prf_o_rd_ctr_o),
      .o_conflict_ctr       (prf_o_conflict_ctr_o),
      .o_mismatch_ctr       (prf_o_mismatch_ctr_o),
      .o_invalid_ctr        (prf_o_invalid_ctr_o),
      .o_busy               (prf_o_busy_o)
  );

  // The register-file response buses are sliced bank by bank from the same nets
  // the register file drives, so the driver observes exactly what a consumer
  // would see. The slice widths are the contract's, not a re-derivation.
  assign prf_rsp_tag0_o = 32'(rsp_tag[0*TAG_W +: TAG_W]);
  assign prf_rsp_tag1_o = 32'(rsp_tag[1*TAG_W +: TAG_W]);
  assign prf_rsp_tag2_o = 32'(rsp_tag[2*TAG_W +: TAG_W]);
  assign prf_rsp_tag3_o = 32'(rsp_tag[3*TAG_W +: TAG_W]);
  assign prf_rsp_gen0_o = 32'(rsp_gen[0*PGEN_W +: PGEN_W]);
  assign prf_rsp_gen1_o = 32'(rsp_gen[1*PGEN_W +: PGEN_W]);
  assign prf_rsp_gen2_o = 32'(rsp_gen[2*PGEN_W +: PGEN_W]);
  assign prf_rsp_gen3_o = 32'(rsp_gen[3*PGEN_W +: PGEN_W]);
  assign prf_rsp_data0_o = rsp_data[0*XLEN +: XLEN];
  assign prf_rsp_data1_o = rsp_data[1*XLEN +: XLEN];
  assign prf_rsp_data2_o = rsp_data[2*XLEN +: XLEN];
  assign prf_rsp_data3_o = rsp_data[3*XLEN +: XLEN];

  // The arbiter's write port, bank by bank, from the same net that feeds the
  // register file: the driver checks the presentation and the storage is written
  // from exactly what it checks.
  assign prf_wr_en0_o       = arb_wr_en[0];
  assign prf_wr_en1_o       = arb_wr_en[1];
  assign prf_wr_en2_o       = arb_wr_en[2];
  assign prf_wr_en3_o       = arb_wr_en[3];
  assign prf_wr_gen_valid0_o = arb_wr_gen_valid[0];
  assign prf_wr_gen_valid1_o = arb_wr_gen_valid[1];
  assign prf_wr_gen_valid2_o = arb_wr_gen_valid[2];
  assign prf_wr_gen_valid3_o = arb_wr_gen_valid[3];
  assign prf_wr_tag0_o = 32'(arb_wr_tag[0*TAG_W +: TAG_W]);
  assign prf_wr_tag1_o = 32'(arb_wr_tag[1*TAG_W +: TAG_W]);
  assign prf_wr_tag2_o = 32'(arb_wr_tag[2*TAG_W +: TAG_W]);
  assign prf_wr_tag3_o = 32'(arb_wr_tag[3*TAG_W +: TAG_W]);
  assign prf_wr_gen0_o = 32'(arb_wr_gen[0*PGEN_W +: PGEN_W]);
  assign prf_wr_gen1_o = 32'(arb_wr_gen[1*PGEN_W +: PGEN_W]);
  assign prf_wr_gen2_o = 32'(arb_wr_gen[2*PGEN_W +: PGEN_W]);
  assign prf_wr_gen3_o = 32'(arb_wr_gen[3*PGEN_W +: PGEN_W]);
  assign prf_wr_data0_o = arb_wr_data[0*XLEN +: XLEN];
  assign prf_wr_data1_o = arb_wr_data[1*XLEN +: XLEN];
  assign prf_wr_data2_o = arb_wr_data[2*XLEN +: XLEN];
  assign prf_wr_data3_o = arb_wr_data[3*XLEN +: XLEN];

  // Geometry exported for the driver to check against its own assumptions. The
  // values come from the same generated packages the DUTs read, which is the
  // single geometry in this compilation; the DUTs' own file-scope widths are not
  // hierarchical, so a `u_arb.WBA_*` reference would not be legal here and the
  // package constant is what those widths were defined from.
  assign o_banks_o       = 32'(mosaic_cfg_pkg::MOSAIC_PRF_BANKS);
  assign o_prf_entries_o = 32'(mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES);
  assign o_prf_tag_w_o   = 32'(mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG);
  assign o_prf_gen_w_o   = 32'(mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN);
  assign o_igen_w_o      = 32'(mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W);
  assign o_xlen_o        = 32'(mosaic_cfg_pkg::MOSAIC_XLEN);
  assign o_rob_entries_o = 32'(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES);
  assign o_rob_index_w_o = 32'(mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX);
  assign o_rob_gen_w_o   = 32'(mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN);
  assign o_uop_w_o       = 32'(mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX);
  assign o_cmp_uop_w_o   = 32'(mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX);

endmodule : mosaic_wb_arbiter_tb

`resetall
`default_nettype wire
