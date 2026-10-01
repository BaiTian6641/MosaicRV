// Simulation wrapper for CASE=rename.single_width_ownership (I-013) and
// CASE=rename.same_cycle_chain (I-014).
//
// `mosaic_rename` is a stateful single-clock module with combinational source
// reads and a combinational allocation answer (it describes the edge that is
// about to happen). This wrapper adds no timing of its own: it passes every
// port straight through -- including the second allocation lane and lane 1's two
// source ports, which I-014 added for a two-wide group. Which case is being run
// is the driver's decision (it branches on `--case`); the hardware is the same
// either way, and a group of one is presented by holding `alloc2_req` low.
//
// It does two things of its own, both about geometry:
//
//   * The observation outputs are declared with widths **derived from the
//     generated configuration package**, not written out by hand. A hand-written
//     "96 entries" in a testbench is a second copy of the geometry that a
//     profile change would silently contradict, and a mismatch there shows up as
//     a slice of the mask the driver never looks at -- i.e. as a test that got
//     weaker without failing.
//
//   * The elaborated localparams are read back out of the instance as outputs.
//     The C++ shadow model sizes itself from those and from nothing else, so
//     `sim/unit/tb_rename.cpp` contains no depth, no tag width and no bank
//     count. The driver cannot disagree with the hardware about how big it is,
//     and the first check after reset makes ignoring these ports a failure.
//
// There is deliberately no clock generation, no reset generation and no
// `$display` in here: the C++ side owns the clock, the reset schedule and all
// result reporting, per sim/common/sim_common.h.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off MODDUP */
// The generated header declares one localparam per configuration knob for the
// whole project. This wrapper needs the register-file subset; the rest belong to
// other modules and are unused *here* by construction, not by omission.
//
// MODDUP: `mosaic_rename.sv` includes the same header and the generated file
// carries no include guard, so two includes in one compilation unit are a
// duplicate package declaration and nothing else -- the bodies are identical,
// so there is nothing for the duplicate to disagree about. The alternative, a
// locally-defined guard macro, would make the *second* includer silently miss
// the package if the first were ever removed, which is a worse failure than the
// warning.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on MODDUP */
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_ENTRIES   = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
localparam int unsigned TB_TAG_W     = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned TB_GEN_W     = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned TB_ARCH_REGS = mosaic_cfg_pkg::MOSAIC_ARCH_INT_REGS;
localparam int unsigned TB_BANKS     = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;
localparam int unsigned TB_ROWS      = TB_ENTRIES / TB_BANKS;
localparam int unsigned TB_JOURNAL   = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned TB_MAP_W     = TB_TAG_W + TB_GEN_W;
// The undo window's length register is one bit wider than an index, because it
// has to be able to say "full" as well as name every entry -- the same rule the
// DUT derives `REN_JLEN_W` from.
localparam int unsigned TB_JLEN_W    = $clog2(TB_JOURNAL + 1);

module mosaic_rename_tb (
    input  logic                                       clk,
    input  logic                                       rst,

    // allocation (I-014: a group of up to two macros, one per lane)
    input  logic                                       alloc_req,
    input  logic [4:0]                                 alloc_rd,
    output logic                                       alloc_accepted,
    output logic                                       alloc_exhausted,
    output logic                                       alloc_squashed,
    output logic                                       alloc_is_x0,
    output logic                                       alloc_new_valid,
    output logic [TB_TAG_W-1:0]                        alloc_new_tag,
    output logic [TB_GEN_W-1:0]                        alloc_new_gen,
    output logic                                       alloc_old_valid,
    output logic [TB_TAG_W-1:0]                        alloc_old_tag,
    output logic [TB_GEN_W-1:0]                        alloc_old_gen,
    input  logic                                       alloc2_req,
    input  logic [4:0]                                 alloc2_rd,
    output logic                                       alloc2_accepted,
    output logic                                       alloc2_exhausted,
    output logic                                       alloc2_squashed,
    output logic                                       alloc2_is_x0,
    output logic                                       alloc2_new_valid,
    output logic [TB_TAG_W-1:0]                        alloc2_new_tag,
    output logic [TB_GEN_W-1:0]                        alloc2_new_gen,
    output logic                                       alloc2_old_valid,
    output logic [TB_TAG_W-1:0]                        alloc2_old_tag,
    output logic [TB_GEN_W-1:0]                        alloc2_old_gen,

    // source reads
    input  logic [4:0]                                 rs1_addr,
    input  logic [4:0]                                 rs2_addr,
    output logic                                       rs1_is_x0,
    output logic                                       rs2_is_x0,
    output logic                                       rs1_ready,
    output logic                                       rs2_ready,
    output logic [TB_TAG_W-1:0]                        rs1_tag,
    output logic [TB_TAG_W-1:0]                        rs2_tag,
    output logic [TB_GEN_W-1:0]                        rs1_gen,
    output logic [TB_GEN_W-1:0]                        rs2_gen,
    // lane 1's two sources, plus the same-cycle bypass and readiness view
    input  logic [4:0]                                 rs3_addr,
    input  logic [4:0]                                 rs4_addr,
    output logic                                       rs3_is_x0,
    output logic                                       rs4_is_x0,
    output logic                                       rs3_ready,
    output logic                                       rs4_ready,
    output logic                                       rs3_bypass,
    output logic                                       rs4_bypass,
    output logic [TB_TAG_W-1:0]                        rs3_tag,
    output logic [TB_TAG_W-1:0]                        rs4_tag,
    output logic [TB_GEN_W-1:0]                        rs3_gen,
    output logic [TB_GEN_W-1:0]                        rs4_gen,

    // writeback
    input  logic                                       wb_valid,
    input  logic [TB_TAG_W-1:0]                        wb_tag,
    input  logic [TB_GEN_W-1:0]                        wb_gen,
    output logic                                       wb_accepted,
    output logic                                       wb_stale,
    output logic                                       wb_duplicate,

    // free
    input  logic                                       free_valid,
    input  logic [TB_TAG_W-1:0]                        free_tag,
    input  logic [TB_GEN_W-1:0]                        free_gen,
    output logic                                       free_accepted,
    output logic                                       free_stale,
    output logic                                       free_double,

    // commit
    input  logic                                       commit_valid,
    input  logic [4:0]                                 commit_rd,
    input  logic [TB_TAG_W-1:0]                        commit_tag,
    input  logic [TB_GEN_W-1:0]                        commit_gen,
    output logic                                       commit_accepted,
    output logic                                       commit_x0_dropped,

    // ------------------------------------------------- I-017: second commit lane
    // `mosaic_rename` gained a second commit lane for two-wide retirement. I-014
    // needs it driven: the WAW release contract of a two-wide rename group is
    // precisely a statement about what the two commit lanes release, and it can
    // only be checked against the hardware by presenting both lanes. The
    // single-width case (CASE=rename.single_width_ownership) leaves the lane
    // inactive on every cycle, so it still tests exactly what it tested before.
    input  logic                                       commit2_valid,
    input  logic [4:0]                                 commit2_rd,
    input  logic [TB_TAG_W-1:0]                        commit2_tag,
    input  logic [TB_GEN_W-1:0]                        commit2_gen,
    output logic                                       commit2_accepted,
    output logic                                       commit2_x0_dropped,

    // recovery
    input  logic                                       ckpt_valid,
    input  logic                                       squash,
    output logic                                       squash_accepted,
    output logic                                       squash_underflow,
    output logic                                       journal_overflow,

    // occupancy
    output logic [TB_TAG_W:0]                          free_count,

    // whole-state observation
    output logic [TB_ENTRIES-1:0]                      dbg_free_mask,
    output logic [TB_ENTRIES-1:0]                      dbg_gen_valid,
    output logic [TB_ENTRIES-1:0]                      dbg_wb_done,
    output logic [TB_ENTRIES*TB_GEN_W-1:0]             dbg_tag_gen,
    output logic [TB_ARCH_REGS*TB_MAP_W-1:0]           dbg_spec_map,
    output logic [TB_ARCH_REGS*TB_MAP_W-1:0]           dbg_cmt_map,
    output logic [TB_JLEN_W-1:0]                       dbg_j_len,

    // the elaborated geometry, read back from the DUT instance
    output logic [31:0]                                o_entries,
    output logic [31:0]                                o_tag_w,
    output logic [31:0]                                o_gen_w,
    output logic [31:0]                                o_arch_regs,
    output logic [31:0]                                o_banks,
    output logic [31:0]                                o_bank_rows,
    output logic [31:0]                                o_journal,
    output logic [31:0]                                o_jlen_w
);

  mosaic_rename u_ren (
      .clk              (clk),
      .rst              (rst),

      .alloc_req        (alloc_req),
      .alloc_rd         (alloc_rd),
      .alloc_accepted   (alloc_accepted),
      .alloc_exhausted  (alloc_exhausted),
      .alloc_squashed   (alloc_squashed),
      .alloc_is_x0      (alloc_is_x0),
      .alloc_new_valid  (alloc_new_valid),
      .alloc_new_tag    (alloc_new_tag),
      .alloc_new_gen    (alloc_new_gen),
      .alloc_old_valid  (alloc_old_valid),
      .alloc_old_tag    (alloc_old_tag),
      .alloc_old_gen    (alloc_old_gen),

      .alloc2_req       (alloc2_req),
      .alloc2_rd        (alloc2_rd),
      .alloc2_accepted  (alloc2_accepted),
      .alloc2_exhausted (alloc2_exhausted),
      .alloc2_squashed  (alloc2_squashed),
      .alloc2_is_x0     (alloc2_is_x0),
      .alloc2_new_valid (alloc2_new_valid),
      .alloc2_new_tag   (alloc2_new_tag),
      .alloc2_new_gen   (alloc2_new_gen),
      .alloc2_old_valid (alloc2_old_valid),
      .alloc2_old_tag   (alloc2_old_tag),
      .alloc2_old_gen   (alloc2_old_gen),

      .rs1_addr         (rs1_addr),
      .rs2_addr         (rs2_addr),
      .rs1_is_x0        (rs1_is_x0),
      .rs2_is_x0        (rs2_is_x0),
      .rs1_ready        (rs1_ready),
      .rs2_ready        (rs2_ready),
      .rs1_tag          (rs1_tag),
      .rs2_tag          (rs2_tag),
      .rs1_gen          (rs1_gen),
      .rs2_gen          (rs2_gen),

      .rs3_addr         (rs3_addr),
      .rs4_addr         (rs4_addr),
      .rs3_is_x0        (rs3_is_x0),
      .rs4_is_x0        (rs4_is_x0),
      .rs3_ready        (rs3_ready),
      .rs4_ready        (rs4_ready),
      .rs3_bypass       (rs3_bypass),
      .rs4_bypass       (rs4_bypass),
      .rs3_tag          (rs3_tag),
      .rs4_tag          (rs4_tag),
      .rs3_gen          (rs3_gen),
      .rs4_gen          (rs4_gen),

      .wb_valid         (wb_valid),
      .wb_tag           (wb_tag),
      .wb_gen           (wb_gen),
      .wb_accepted      (wb_accepted),
      .wb_stale         (wb_stale),
      .wb_duplicate     (wb_duplicate),

      .free_valid       (free_valid),
      .free_tag         (free_tag),
      .free_gen         (free_gen),
      .free_accepted    (free_accepted),
      .free_stale       (free_stale),
      .free_double      (free_double),

      .commit_valid     (commit_valid),
      .commit_rd        (commit_rd),
      .commit_tag       (commit_tag),
      .commit_gen       (commit_gen),
      .commit_accepted  (commit_accepted),
      .commit_x0_dropped(commit_x0_dropped),

      // The second commit lane, driven by the driver: the WAW release contract
      // is a statement about what the two lanes release, so the driver has to
      // present both.
      .commit2_valid    (commit2_valid),
      .commit2_rd       (commit2_rd),
      .commit2_tag      (commit2_tag),
      .commit2_gen      (commit2_gen),
      .commit2_accepted (commit2_accepted),
      .commit2_x0_dropped(commit2_x0_dropped),

      .ckpt_valid       (ckpt_valid),
      .squash           (squash),
      .squash_accepted  (squash_accepted),
      .squash_underflow (squash_underflow),
      .journal_overflow (journal_overflow),

      .free_count       (free_count),

      .dbg_free_mask    (dbg_free_mask),
      .dbg_gen_valid    (dbg_gen_valid),
      .dbg_wb_done      (dbg_wb_done),
      .dbg_tag_gen      (dbg_tag_gen),
      .dbg_spec_map     (dbg_spec_map),
      .dbg_cmt_map      (dbg_cmt_map),
      .dbg_j_len        (dbg_j_len)
  );
  // The geometry, straight from the generated package this file already
  // included to size its ports. There is no second copy: the RTL derives the
  // same numbers from the same localparams, and because the two port lists are
  // connected by name, a profile that changed either side would fail to build
  // on a width mismatch rather than quietly narrow an observation the driver
  // reads.
  assign o_entries   = 32'(TB_ENTRIES);
  assign o_tag_w     = 32'(TB_TAG_W);
  assign o_gen_w     = 32'(TB_GEN_W);
  assign o_arch_regs = 32'(TB_ARCH_REGS);
  assign o_banks     = 32'(TB_BANKS);
  assign o_bank_rows = 32'(TB_ROWS);
  assign o_journal   = 32'(TB_JOURNAL);
  assign o_jlen_w    = 32'(TB_JLEN_W);

endmodule : mosaic_rename_tb

`resetall
`default_nettype wire
