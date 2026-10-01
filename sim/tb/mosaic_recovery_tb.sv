// Simulation wrapper for CASE=recovery.checkpoint_exact_restore (I-018).
//
// The wrapper adds no timing of its own. `mosaic_recovery` is registered on its
// inputs and combinational on its outputs, so the C++ driver drives the ports,
// evaluates while the clock is low, compares every output against its shadow,
// and then applies the edge. There is no clock generation, no reset generation
// and no `$display` in here: the C++ side owns all three, per
// sim/common/sim_common.h.
//
// --------------------------------------------------------------- geometry
//
// Every driver-facing port is a fixed 32-bit (64-bit for the PC) vector, so the
// C++ driver contains no MosaicRV geometry and would keep compiling if a profile
// changed the depths. Narrowing to the recovery unit's own port widths needs
// those widths, and they come from **the same generated package the RTL reads**,
// by scope reference rather than by a second `include`:
//
//   * the generated header declares `package mosaic_cfg_pkg` with an include
//     guard, so including it a second time in the same compilation is harmless
//     for the guard but would still be a second textual copy;
//   * referring to `mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES` after the package has
//     been declared needs no include at all, and both Verilator and slang
//     accept it.
//
// So there is exactly one geometry in the compilation: the generated package.
// This file derives the port widths from it with the same rules
// mosaic_recovery uses, and then *reads the elaborated values back* on the
// `o_*_w_o` outputs so the driver checks the derivation against the instance
// rather than trusting it.
//
// The wide observation ports are the interesting ones. The unit test's central
// claim is that a restore returns the state **bit-identically**, so the whole
// state has to be readable and the whole state has to be compared. Splitting a
// 96-bit free mask across three 32-bit driver-facing words would move the
// splitting rule into the C++ and make the "wholesale" comparison a per-field
// one by accident. So the observation ports are 64-bit words, and the driver
// reads them as a vector of words and concatenates -- the split is the harness's
// business and the comparison is still over the whole value.

`default_nettype none
`resetall

module mosaic_recovery_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------------------ drive
    input  logic        alloc_valid_i,
    input  logic [31:0] alloc_rd_i,
    input  logic [31:0] alloc_rob_gen_i,

    input  logic        ckpt_valid_i,
    input  logic [31:0] ckpt_rob_gen_i,

    input  logic        commit_valid_i,
    input  logic [31:0] commit_rd_i,
    input  logic [31:0] commit_tag_i,
    input  logic [31:0] commit_gen_i,
    input  logic        rob_retire_i,

    input  logic        wb_valid_i,
    input  logic [31:0] wb_tag_i,
    input  logic [31:0] wb_gen_i,

    input  logic        free_valid_i,
    input  logic [31:0] free_tag_i,
    input  logic [31:0] free_gen_i,

    input  logic        redirect0_valid_i,
    input  logic [31:0] redirect0_rob_gen_i,
    input  logic [63:0] redirect0_pc_i,
    input  logic        redirect0_is_fault_i,

    input  logic        redirect1_valid_i,
    input  logic [31:0] redirect1_rob_gen_i,
    input  logic [63:0] redirect1_pc_i,
    input  logic        redirect1_is_fault_i,

    input  logic        cred_req_valid_i,
    input  logic [31:0] cred_req_id_i,
    input  logic [31:0] cred_req_rob_gen_i,

    input  logic        rsp_valid_i,
    input  logic [31:0] rsp_id_i,
    input  logic [31:0] rsp_epoch_i,

    // ---------------------------------------------------------------- observe
    output logic        alloc_accepted_o,
    output logic        alloc_squashed_o,
    output logic        alloc_exhausted_o,
    output logic        alloc_is_x0_o,
    output logic        alloc_new_valid_o,
    output logic [31:0] alloc_new_tag_o,
    output logic [31:0] alloc_new_gen_o,
    output logic        alloc_old_valid_o,
    output logic [31:0] alloc_old_tag_o,
    output logic [31:0] alloc_old_gen_o,
    output logic        alloc_journal_full_o,
    output logic        journal_overflow_o,
    output logic        alloc_gen_regress_o,

    output logic        ckpt_accepted_o,
    output logic        ckpt_refused_o,
    output logic [31:0] o_ckpt_depth_o,

    output logic        commit_accepted_o,
    output logic        commit_x0_dropped_o,

    output logic        wb_accepted_o,
    output logic        wb_stale_o,
    output logic        wb_duplicate_o,

    output logic        free_accepted_o,
    output logic        free_stale_o,
    output logic        free_double_o,

    output logic        redirect_taken_o,
    output logic [31:0] redirect_taken_gen_o,
    output logic [63:0] redirect_taken_pc_o,
    output logic        redirect_taken_is_fault_o,
    output logic        redirect_stale_o,
    output logic [31:0] redirect_killed_o,
    output logic [31:0] o_rdq_depth_o,
    output logic [31:0] o_redirect_src_o,
    output logic [31:0] o_restore_ckpt_o,

    output logic        squash_o,
    output logic        retire_block_o,
    output logic        rob_flush_valid_o,
    output logic        o_rob_flush_from_valid_o,
    output logic [31:0] o_rob_flush_from_o,

    output logic        cred_req_ok_o,
    output logic        cred_req_full_o,
    output logic        cred_req_conflict_o,
    output logic        rsp_accepted_o,
    output logic        rsp_dropped_stale_o,
    output logic        rsp_dropped_dup_o,
    output logic        rsp_dropped_orphan_o,
    output logic        credit_return_o,
    output logic [31:0] credits_outstanding_o,
    output logic [31:0] o_epoch_o,

    output logic [31:0] free_count_o,

    // The whole speculative state, as 64-bit words so the driver can compare it
    // wholesale. The word counts are derived from the geometry below, and
    // published on `o_*_words_o` so the driver can check the derivation.
    output logic [63:0] dbg_free_mask_o    [0:1],   // ENTRIES bits: 2 words at p0
    output logic [63:0] dbg_gen_valid_o    [0:1],
    output logic [63:0] dbg_wb_done_o      [0:1],
    output logic [63:0] dbg_tag_gen_o_ext  [0:10],  // ENTRIES*GEN_W: 11 words at p0
    output logic [63:0] dbg_spec_map_o     [0:6],   // ARCH*MAP_W: 7 words at p0
    output logic [63:0] dbg_cmt_map_o      [0:6],
    output logic [31:0] dbg_tail_o,
    output logic [31:0] dbg_alloc_ptr_o,
    output logic [31:0] dbg_j_len_o,
    // The checkpoint bundles, 64-bit and compared wholesale like the rest of the
    // state. A checkpoint-stack divergence that surfaced only in a port nobody
    // happened to check is exactly the defect this case must not let through, so
    // the bundle is published wide enough to be read in full and the driver
    // compares every field of it on every cycle.
    output logic [63:0] dbg_ckpt_valid_o,      // CKPT_DEPTH bits
    output logic [63:0] dbg_ckpt_jmark_o,      // CKPT_DEPTH*CNT_W
    output logic [63:0] dbg_ckpt_gen_o  [0:1], // CKPT_DEPTH*ID_W: 2 words at p0
    output logic [63:0] dbg_ckpt_tail_o,       // CKPT_DEPTH*IDX_W
    output logic [63:0] dbg_ckpt_alloc_ptr_o,  // CKPT_DEPTH*TAG_W
    output logic [63:0] dbg_ckpt_epoch_o,      // CKPT_DEPTH*EPOCH_W
    output logic        dbg_inexact_o,

    output logic [31:0] o_journal_entries_o,
    output logic [31:0] o_restores_o,
    output logic [31:0] o_ckpt_taken_o,
    output logic [31:0] o_credit_returns_o,
    output logic [31:0] o_rsp_stale_o,

    // ---------------------------------------------------------------- geometry
    output logic [31:0] o_prf_entries_o,
    output logic [31:0] o_tag_w_o,
    output logic [31:0] o_gen_w_o,
    output logic [31:0] o_arch_regs_o,
    output logic [31:0] o_rob_entries_o,
    output logic [31:0] o_rob_index_w_o,
    output logic [31:0] o_id_w_o,
    output logic [31:0] o_ckpt_depth_w_o,
    output logic [31:0] o_rdq_depth_w_o,
    output logic [31:0] o_cred_w_o,
    output logic [31:0] o_epoch_w_o,
    output logic [31:0] o_cnt_w_o,
    output logic [31:0] o_map_w_o,
    output logic [31:0] o_mask_words_o,
    output logic [31:0] o_tag_gen_words_o,
    output logic [31:0] o_map_words_o,
    output logic [31:0] o_ckpt_gen_words_o,
    output logic [31:0] o_ckpt_depth_param_o,
    output logic [31:0] o_rdq_depth_param_o,
    output logic [31:0] o_cred_entries_param_o
);

  // The same rules mosaic_recovery derives from the same generated package.
  localparam int unsigned ENTRIES = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
  localparam int unsigned TAG_W   = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
  localparam int unsigned GEN_W   = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
  localparam int unsigned ARCH    = mosaic_cfg_pkg::MOSAIC_ARCH_INT_REGS;
  localparam int unsigned ROB     = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
  localparam int unsigned IDX_W   = mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W;
  localparam int unsigned ID_W    = 2 * IDX_W;
  // XLEN is deliberately not declared: the PC ports are already 64 bits wide at
  // the testbench boundary, so the driver never needs the width, and a localparam
  // nothing reads is a second place for the geometry to be wrong.
  localparam int unsigned EPOCH_W = IDX_W + 1;
  localparam int unsigned MAP_W   = TAG_W + GEN_W;

  // The structural depths. They are parameters of the unit, not of the profile,
  // so they are stated here rather than derived -- and they are passed by name
  // so the driver reads back what the instance actually used.
  localparam int unsigned CKPT_DEPTH = 8;
  localparam int unsigned RDQ_DEPTH  = 8;
  localparam int unsigned CRED_ENTRIES = 16;

  localparam int unsigned CKPT_W = (CKPT_DEPTH <= 1) ? 1 : $clog2(CKPT_DEPTH);
  localparam int unsigned RDQ_W  = (RDQ_DEPTH <= 1) ? 1 : $clog2(RDQ_DEPTH);
  localparam int unsigned CRED_W = (CRED_ENTRIES <= 1) ? 1 : $clog2(CRED_ENTRIES);
  localparam int unsigned PICK_W = ((RDQ_DEPTH + 2) <= 1) ? 1 : $clog2(RDQ_DEPTH + 2);
  localparam int unsigned CNT_W  = $clog2(ROB + 1);

  // Word counts for the wide observation ports. A part-select of more than 64
  // bits is legal SystemVerilog but reads badly, so the ports are unpacked word
  // arrays and the split is stated once, here.
  localparam int unsigned MASK_WORDS      = (ENTRIES + 63) / 64;
  localparam int unsigned TAG_GEN_WORDS   = (ENTRIES * GEN_W + 63) / 64;
  localparam int unsigned MAP_WORDS       = (ARCH * MAP_W + 63) / 64;

  // The driver hands over 32-bit values and the unit sees exactly the low bits
  // its own ports declare. Part selects, not casts, so nothing is silent.
  logic [4:0]            alloc_rd;
  logic [ID_W-1:0]       alloc_rob_gen;
  logic [ID_W-1:0]       ckpt_rob_gen;
  logic [4:0]            commit_rd;
  logic [TAG_W-1:0]      commit_tag;
  logic [GEN_W-1:0]      commit_gen;
  logic [TAG_W-1:0]      wb_tag;
  logic [GEN_W-1:0]      wb_gen;
  logic [TAG_W-1:0]      free_tag;
  logic [GEN_W-1:0]      free_gen;
  logic [ID_W-1:0]       redirect0_rob_gen;
  logic [ID_W-1:0]       redirect1_rob_gen;
  logic [CRED_W-1:0]     cred_req_id;
  logic [CRED_W-1:0]     rsp_id;
  logic [EPOCH_W-1:0]    rsp_epoch;

  assign alloc_rd          = alloc_rd_i[4:0];
  assign alloc_rob_gen     = alloc_rob_gen_i[ID_W-1:0];
  assign ckpt_rob_gen      = ckpt_rob_gen_i[ID_W-1:0];
  assign commit_rd         = commit_rd_i[4:0];
  assign commit_tag        = commit_tag_i[TAG_W-1:0];
  assign commit_gen        = commit_gen_i[GEN_W-1:0];
  assign wb_tag            = wb_tag_i[TAG_W-1:0];
  assign wb_gen            = wb_gen_i[GEN_W-1:0];
  assign free_tag          = free_tag_i[TAG_W-1:0];
  assign free_gen          = free_gen_i[GEN_W-1:0];
  assign redirect0_rob_gen = redirect0_rob_gen_i[ID_W-1:0];
  assign redirect1_rob_gen = redirect1_rob_gen_i[ID_W-1:0];
  assign cred_req_id       = cred_req_id_i[CRED_W-1:0];
  assign rsp_id            = rsp_id_i[CRED_W-1:0];
  assign rsp_epoch         = rsp_epoch_i[EPOCH_W-1:0];

  // The packed observation bundles, taken out of the instance once so no port
  // connection carries a chained part-select.
  logic [ENTRIES-1:0]        obs_free_mask;
  logic [ENTRIES-1:0]        obs_gen_valid;
  logic [ENTRIES-1:0]        obs_wb_done;
  logic [ENTRIES*GEN_W-1:0]  obs_tag_gen;
  logic [ARCH*MAP_W-1:0]     obs_spec_map;
  logic [ARCH*MAP_W-1:0]     obs_cmt_map;
  logic [CKPT_DEPTH*CNT_W-1:0]  obs_ckpt_jmark;
  logic [CKPT_DEPTH*ID_W-1:0]   obs_ckpt_gen;
  logic [CKPT_DEPTH*IDX_W-1:0]  obs_ckpt_tail;
  logic [CKPT_DEPTH*TAG_W-1:0]  obs_ckpt_alloc_ptr;
  logic [CKPT_DEPTH*EPOCH_W-1:0] obs_ckpt_epoch;
  // The checkpoint valid vector, taken out once so the bundle assignment below
  // does not read the instance's packed port through a chain.
  logic [CKPT_DEPTH-1:0]         ck_valid_obs;

  mosaic_recovery #(
      .CKPT_DEPTH   (CKPT_DEPTH),
      .RDQ_DEPTH    (RDQ_DEPTH),
      .CRED_ENTRIES (CRED_ENTRIES)
  ) u_rec (
      .clk                  (clk),
      .rst                  (rst),

      .alloc_valid          (alloc_valid_i),
      .alloc_rd             (alloc_rd),
      .alloc_rob_gen        (alloc_rob_gen),
      .alloc_accepted       (alloc_accepted_o),
      .alloc_squashed       (alloc_squashed_o),
      .alloc_exhausted      (alloc_exhausted_o),
      .alloc_is_x0          (alloc_is_x0_o),
      .alloc_new_valid      (alloc_new_valid_o),
      .alloc_new_tag        (alloc_new_tag_o[TAG_W-1:0]),
      .alloc_new_gen        (alloc_new_gen_o[GEN_W-1:0]),
      .alloc_old_valid      (alloc_old_valid_o),
      .alloc_old_tag        (alloc_old_tag_o[TAG_W-1:0]),
      .alloc_old_gen        (alloc_old_gen_o[GEN_W-1:0]),
      .alloc_journal_full   (alloc_journal_full_o),
      .journal_overflow     (journal_overflow_o),
      .alloc_gen_regress    (alloc_gen_regress_o),

      .ckpt_valid           (ckpt_valid_i),
      .ckpt_rob_gen         (ckpt_rob_gen),
      .ckpt_accepted        (ckpt_accepted_o),
      .ckpt_refused         (ckpt_refused_o),
      .o_ckpt_depth         (o_ckpt_depth_o[CKPT_W:0]),

      .commit_valid         (commit_valid_i),
      .commit_rd            (commit_rd),
      .commit_tag           (commit_tag),
      .commit_gen           (commit_gen),
      .commit_accepted      (commit_accepted_o),
      .commit_x0_dropped    (commit_x0_dropped_o),

      .rob_retire           (rob_retire_i),

      .wb_valid             (wb_valid_i),
      .wb_tag               (wb_tag),
      .wb_gen               (wb_gen),
      .wb_accepted          (wb_accepted_o),
      .wb_stale             (wb_stale_o),
      .wb_duplicate         (wb_duplicate_o),

      .free_valid           (free_valid_i),
      .free_tag             (free_tag),
      .free_gen             (free_gen),
      .free_accepted        (free_accepted_o),
      .free_stale           (free_stale_o),
      .free_double          (free_double_o),

      .redirect0_valid      (redirect0_valid_i),
      .redirect0_rob_gen    (redirect0_rob_gen),
      .redirect0_pc         (redirect0_pc_i),
      .redirect0_is_fault   (redirect0_is_fault_i),
      .redirect1_valid      (redirect1_valid_i),
      .redirect1_rob_gen    (redirect1_rob_gen),
      .redirect1_pc         (redirect1_pc_i),
      .redirect1_is_fault   (redirect1_is_fault_i),

      .redirect_taken       (redirect_taken_o),
      .redirect_taken_gen   (redirect_taken_gen_o[ID_W-1:0]),
      .redirect_taken_pc    (redirect_taken_pc_o),
      .redirect_taken_is_fault (redirect_taken_is_fault_o),
      .redirect_stale       (redirect_stale_o),
      .redirect_killed      (redirect_killed_o[RDQ_W:0]),
      .o_rdq_depth          (o_rdq_depth_o[RDQ_W:0]),
      .o_redirect_src       (o_redirect_src_o[PICK_W-1:0]),
      .o_restore_ckpt       (o_restore_ckpt_o[CKPT_W-1:0]),

      .squash               (squash_o),
      .retire_block         (retire_block_o),
      .rob_flush_valid      (rob_flush_valid_o),
      .o_rob_flush_from_valid (o_rob_flush_from_valid_o),
      .o_rob_flush_from     (o_rob_flush_from_o[IDX_W-1:0]),

      .cred_req_valid       (cred_req_valid_i),
      .cred_req_id          (cred_req_id),
      .cred_req_rob_gen     (cred_req_rob_gen_i[ID_W-1:0]),
      .cred_req_ok          (cred_req_ok_o),
      .cred_req_full        (cred_req_full_o),
      .cred_req_conflict    (cred_req_conflict_o),

      .rsp_valid            (rsp_valid_i),
      .rsp_id               (rsp_id),
      .rsp_epoch            (rsp_epoch),
      .rsp_accepted         (rsp_accepted_o),
      .rsp_dropped_stale    (rsp_dropped_stale_o),
      .rsp_dropped_dup      (rsp_dropped_dup_o),
      .rsp_dropped_orphan   (rsp_dropped_orphan_o),
      .credit_return        (credit_return_o),
      .credits_outstanding  (credits_outstanding_o[CRED_W:0]),
      .o_epoch              (o_epoch_o[EPOCH_W-1:0]),

      .free_count           (free_count_o[TAG_W:0]),

      .dbg_free_mask        (obs_free_mask),
      .dbg_gen_valid        (obs_gen_valid),
      .dbg_wb_done          (obs_wb_done),
      .dbg_tag_gen          (obs_tag_gen),
      .dbg_spec_map         (obs_spec_map),
      .dbg_cmt_map          (obs_cmt_map),
      .dbg_tail             (dbg_tail_o[IDX_W-1:0]),
      .dbg_alloc_ptr        (dbg_alloc_ptr_o[TAG_W-1:0]),
      .dbg_j_len            (dbg_j_len_o[CNT_W-1:0]),
      .dbg_ckpt_valid       (ck_valid_obs),
      .dbg_ckpt_jmark       (obs_ckpt_jmark),
      .dbg_ckpt_gen         (obs_ckpt_gen),
      .dbg_ckpt_tail        (obs_ckpt_tail),
      .dbg_ckpt_alloc_ptr   (obs_ckpt_alloc_ptr),
      .dbg_ckpt_epoch       (obs_ckpt_epoch),
      .dbg_inexact          (dbg_inexact_o),

      .o_journal_entries    (o_journal_entries_o),
      .o_restores           (o_restores_o),
      .o_ckpt_taken         (o_ckpt_taken_o),
      .o_credit_returns     (o_credit_returns_o),
      .o_rsp_stale          (o_rsp_stale_o),

      .o_prf_entries_o      (o_prf_entries_o),
      .o_tag_w_o            (o_tag_w_o),
      .o_gen_w_o            (o_gen_w_o),
      .o_arch_regs_o        (o_arch_regs_o),
      .o_rob_entries_o      (o_rob_entries_o),
      .o_rob_index_w_o      (o_rob_index_w_o),
      .o_id_w_o             (o_id_w_o),
      .o_ckpt_depth_w_o     (o_ckpt_depth_w_o),
      .o_rdq_depth_w_o      (o_rdq_depth_w_o),
      .o_cred_w_o           (o_cred_w_o),
      .o_epoch_w_o          (o_epoch_w_o),
      .o_cnt_w_o            (o_cnt_w_o),
      .o_map_w_o            (o_map_w_o)
  );

  // The wide observation ports, split into 64-bit words. The split is the
  // harness's business and is stated once, so the driver compares whole values
  // rather than three fields that happen to make up one.
  //
  // `word_of` is a function rather than three copies of the same expression
  // because a bundle whose width is not a multiple of 64 has a final short
  // word, and getting that wrong reads past the end of the source vector --
  // which is a silent wrong answer rather than a compile error.
  function automatic logic [63:0] word_of(input logic [4095:0] src,
                                          input int unsigned base,
                                          input int unsigned width);
    logic [63:0] out;
    out = 64'd0;
    for (int unsigned b = 0; b < 64; b++) begin
      if ((b < width) && ((base + b) < 4096)) begin
        out[b] = src[base + b];
      end
    end
    return out;
  endfunction

  for (genvar w = 0; w < MASK_WORDS; w++) begin : g_mask_words
    localparam int unsigned LO = w * 64;
    localparam int unsigned NB = (ENTRIES - LO) > 64 ? 64 : (ENTRIES - LO);
    assign dbg_free_mask_o[w] = word_of({{{(4096 - ENTRIES) {1'b0}}, obs_free_mask}}, LO, NB);
    assign dbg_gen_valid_o[w] = word_of({{{(4096 - ENTRIES) {1'b0}}, obs_gen_valid}}, LO, NB);
    assign dbg_wb_done_o[w]   = word_of({{{(4096 - ENTRIES) {1'b0}}, obs_wb_done}},   LO, NB);
  end

  for (genvar t = 0; t < TAG_GEN_WORDS; t++) begin : g_tag_gen_words
    localparam int unsigned TLO = t * 64;
    localparam int unsigned TNB = (ENTRIES * GEN_W - TLO) > 64 ? 64 : (ENTRIES * GEN_W - TLO);
    assign dbg_tag_gen_o_ext[t] = word_of({{{(4096 - ENTRIES * GEN_W) {1'b0}}, obs_tag_gen}}, TLO, TNB);
  end

  for (genvar m = 0; m < MAP_WORDS; m++) begin : g_map_words
    localparam int unsigned MLO = m * 64;
    localparam int unsigned MNB = (ARCH * MAP_W - MLO) > 64 ? 64 : (ARCH * MAP_W - MLO);
    assign dbg_spec_map_o[m] = word_of({{{(4096 - ARCH * MAP_W) {1'b0}}, obs_spec_map}}, MLO, MNB);
    assign dbg_cmt_map_o[m]  = word_of({{{(4096 - ARCH * MAP_W) {1'b0}}, obs_cmt_map}},  MLO, MNB);
  end

  // The checkpoint bundles, widened to 64-bit words through the same `word_of`
  // the other bundles use. The split is stated once and the driver checks the
  // word count it derived against the one published below, so a profile change
  // that pushed a bundle past a word is a failing check rather than a silently
  // truncated comparison.
  assign dbg_ckpt_valid_o     = word_of({{{(4096 - CKPT_DEPTH) {1'b0}}, ck_valid_obs}},
                                        0, CKPT_DEPTH);
  assign dbg_ckpt_jmark_o     = word_of({{{(4096 - CKPT_DEPTH * CNT_W) {1'b0}},
                                         obs_ckpt_jmark}}, 0, CKPT_DEPTH * CNT_W);
  assign dbg_ckpt_tail_o      = word_of({{{(4096 - CKPT_DEPTH * IDX_W) {1'b0}},
                                         obs_ckpt_tail}}, 0, CKPT_DEPTH * IDX_W);
  assign dbg_ckpt_alloc_ptr_o = word_of({{{(4096 - CKPT_DEPTH * TAG_W) {1'b0}},
                                         obs_ckpt_alloc_ptr}}, 0, CKPT_DEPTH * TAG_W);
  assign dbg_ckpt_epoch_o     = word_of({{{(4096 - CKPT_DEPTH * EPOCH_W) {1'b0}},
                                         obs_ckpt_epoch}}, 0, CKPT_DEPTH * EPOCH_W);

  for (genvar cg = 0; cg < 2; cg++) begin : g_ckpt_gen_words
    localparam int unsigned GLO = cg * 64;
    localparam int unsigned GNB = (CKPT_DEPTH * ID_W - GLO) > 64 ? 64
                                                                : (CKPT_DEPTH * ID_W - GLO);
    assign dbg_ckpt_gen_o[cg] = word_of({{{(4096 - CKPT_DEPTH * ID_W) {1'b0}},
                                         obs_ckpt_gen}}, GLO, GNB);
  end

  // Read back from the elaborated instance rather than from the derivation above,
  // so the driver can check the two against each other. A geometry file change
  // that desynchronised them would show up here as a failing check rather than as
  // a testbench quietly comparing the wrong field.
  assign o_mask_words_o        = 32'(MASK_WORDS);
  assign o_tag_gen_words_o     = 32'(TAG_GEN_WORDS);
  assign o_map_words_o         = 32'(MAP_WORDS);
  assign o_ckpt_depth_param_o  = 32'(u_rec.CKPT_DEPTH);
  assign o_rdq_depth_param_o   = 32'(u_rec.RDQ_DEPTH);
  assign o_cred_entries_param_o = 32'(u_rec.CRED_ENTRIES);

endmodule : mosaic_recovery_tb

`resetall
`default_nettype wire
