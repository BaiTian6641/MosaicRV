// Simulation wrapper for CASE=commit.head_block_and_dual (work package I-017).
//
// The wrapper adds no timing of its own. `mosaic_retire` is registered on its
// inputs and combinational on its outputs, so the C++ driver drives the ports,
// evaluates while the clock is low, compares every output against its shadow,
// and then applies the edge. There is no clock generation, no reset generation
// and no `$display` in here: the C++ side owns all three, per
// sim/common/sim_common.h.
//
// ------------------------------------------------------- what is instantiated
//
// Three modules, wired the way the real core wires them, and that is the point
// of the case rather than an accident of convenience:
//
//   * `mosaic_rob`     -- supplies the head view, the second head view and the
//                          two-lane pop acknowledgement. The DUT is therefore
//                          tested against the *real* queue's idea of
//                          `head_ready`, not against a driver's guess at it.
//   * `mosaic_rename`  -- the committed map. Retire drives its two commit lanes
//                          and the driver reads `dbg_cmt_map` back, so "the
//                          committed map moved exactly once per retired
//                          instruction" is checked against the state the map
//                          owner actually holds, and not against a shadow of
//                          retire's own output -- a shadow of its own output
//                          would agree with a retire that simply never committed
//                          anything.
//   * `mosaic_retire`  -- the unit under test.
//
// The trap path is wired as the design intends: `trap_flush` ORs into the ROB's
// `flush_valid`, so a trapping entry leaves the buffer in the cycle it traps and
// everything younger with it.
//
// Rename's allocation, writeback and free ports are tied inactive, and so are
// its recovery ports. That is deliberate on both counts: this card tests
// retirement, and letting allocation move the free set would make the committed
// map's free count move for a reason that has nothing to do with retirement,
// while tying `squash` low is what makes "retire never disturbs the speculative
// map" an assertion rather than a hope.
//
// --------------------------------------------------------------- geometry
//
// Every driver-facing port is a fixed 32-bit (64-bit for a PC) vector, so the
// C++ driver contains no retire geometry and would keep compiling if a profile
// changed a width. Narrowing to the units' own port widths needs those widths,
// and they come from **the same generated package the RTL reads**, by scope
// reference rather than by a second `include`:
//
//   * the generated header declares `package mosaic_cfg_pkg` with no include
//     guard, so including it in a second file in the same compilation is a
//     duplicate package declaration -- Verilator MODDUP and slang
//     -Wduplicate-definition, both errors here;
//   * referring to `mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH` after the package has
//     been declared needs no include at all, and both tools accept it.
//
// So there is exactly one geometry in the compilation. This file derives its
// widths with the same rules rtl/core/mosaic_retire.sv uses, then reads the
// elaborated values back out as `o_*_w` so the driver checks the two against each
// other instead of trusting the derivation.

`default_nettype none
`resetall

// ------------------------------------------------------ the wrapper's widths
//
// Declared at **file scope**, not inside the module, because several of them
// appear in the module's port list and SystemVerilog gives a port list no view of
// declarations in the body. Verilator accepts a forward reference here and slang
// rejects it, so the two tools together are what forces the file-scope placement.
//
// The widths come from the same generated package the RTL reads, by scope
// reference: the generated header declares `package mosaic_cfg_pkg` with no include
// guard, so a second `include in this compilation is a duplicate package
// declaration in both tools.
localparam int unsigned TB_RET_TAG_W  = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned TB_RET_GEN_W  = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned TB_RET_XLEN   = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned TB_RET_ROB    = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned TB_RET_RB_W   = mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W;
localparam int unsigned TB_RET_MAXU   = mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO;
localparam int unsigned TB_RET_ARCH   = mosaic_cfg_pkg::MOSAIC_ARCH_INT_REGS;
localparam int unsigned TB_RET_PRF    = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;

localparam int unsigned TB_RET_WIDTH  = (mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH <= 1)
                                     ? 1 : mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH;
localparam int unsigned TB_RET_ID_W   = TB_RET_TAG_W + TB_RET_GEN_W;
localparam int unsigned TB_RET_SEQ_W  = $clog2(2 * TB_RET_ROB + 1);
localparam int unsigned TB_RET_CNT_W  = $clog2(TB_RET_WIDTH + 1);
localparam int unsigned TB_RET_RD_W   = 5;
localparam int unsigned TB_RET_CSR_W  = 12;
localparam int unsigned TB_RET_SIZE_W = 3;
// The ROB's own derived widths, by the same rules rtl/core/mosaic_rob.sv uses:
// a $clog2 of one is zero bits, and a zero-bit part select is not a thing.
localparam int unsigned TB_ROB_CNT_W  = $clog2(TB_RET_MAXU + 1);
localparam int unsigned TB_ROB_UOP_W  = (TB_RET_MAXU <= 1) ? 1 : $clog2(TB_RET_MAXU);
localparam int unsigned TB_ROB_ID_W   = 2 * TB_RET_RB_W;   // both TAG_W and GEN_W
localparam int unsigned TB_ROB_OCC_W  = $clog2(TB_RET_ROB + 1);

// ----------------------------------------------- widths of the flat vectors
// Every driver-facing bus is 32 bits per lane (64 for a PC), so the driver
// holds no geometry. Truncating part selects rather than casts, so nothing is
// silent: a lane the geometry does not have simply does not exist.
localparam int unsigned LANE_W    = 32;

module mosaic_retire_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------ ROB stimulus
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

    input  logic [31:0] obs_index_i,
    input  logic        rob_flush_i,

    // ----------------------------- execution payload, one field set per lane
    // The per-lane masks are driver-driven rather than a single "the payload is
    // visible" bit, so the case can present lane 1's payload while withholding
    // lane 0's -- which is the only way to reach the partial-width case at all.
    input  logic [31:0] pay_valid_i,
    input  logic [31:0] pay_reg_we_i,
    input  logic [TB_RET_WIDTH*32-1:0] pay_rd_i,
    input  logic [TB_RET_WIDTH*64-1:0] pay_value_i,
    input  logic [31:0] pay_csr_we_i,
    input  logic [TB_RET_WIDTH*32-1:0] pay_csr_addr_i,
    input  logic [TB_RET_WIDTH*64-1:0] pay_csr_value_i,
    input  logic [31:0] pay_is_store_i,
    input  logic [TB_RET_WIDTH*64-1:0] pay_store_addr_i,
    input  logic [TB_RET_WIDTH*64-1:0] pay_store_data_i,
    input  logic [TB_RET_WIDTH*32-1:0] pay_store_size_i,
    input  logic [TB_RET_WIDTH*64-1:0] pay_exc_cause_i,
    input  logic [TB_RET_WIDTH*64-1:0] pay_exc_tval_i,

    // recovery's one-word interface to this unit, plus the CSR read port
    input  logic        flush_valid_i,
    input  logic        csr_rd_valid_i,
    input  logic [31:0] csr_rd_addr_i,

    // --------------------------------------------------- observe: the retire
    output logic [TB_RET_WIDTH*32-1:0] retire_req_o,
    output logic        trap_flush_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_valid_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_trap_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_seq_o,
    output logic [TB_RET_WIDTH*64-1:0] ev_pc_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_id_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_reg_we_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_rd_o,
    output logic [TB_RET_WIDTH*64-1:0] ev_value_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_csr_we_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_csr_addr_o,
    output logic [TB_RET_WIDTH*64-1:0] ev_csr_value_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_store_o,
    output logic [TB_RET_WIDTH*64-1:0] ev_store_addr_o,
    output logic [TB_RET_WIDTH*64-1:0] ev_store_data_o,
    output logic [TB_RET_WIDTH*32-1:0] ev_store_size_o,
    output logic [TB_RET_WIDTH*64-1:0] ev_trap_cause_o,
    output logic [TB_RET_WIDTH*64-1:0] ev_trap_tval_o,
    output logic        trap_valid_o,
    output logic [63:0] trap_pc_o,
    output logic [63:0] trap_cause_o,
    output logic [63:0] trap_tval_o,
    output logic [TB_RET_WIDTH*32-1:0] commit_valid_o,
    output logic [TB_RET_WIDTH*32-1:0] commit_rd_o,
    output logic [TB_RET_WIDTH*32-1:0] commit_tag_o,
    output logic [TB_RET_WIDTH*32-1:0] commit_gen_o,
    output logic [63:0] csr_rd_data_o,
    output logic        csr_rd_unsupported_o,
    output logic [63:0] o_retire_seq_o,
    output logic [63:0] o_minstret_o,
    output logic [63:0] o_mcycle_o,
    output logic [63:0] o_mscratch_o,
    output logic [TB_RET_WIDTH*32-1:0] o_exc_queued_o,
    output logic [31:0] o_event_count_o,
    output logic        o_x0_retired_o,
    output logic [TB_RET_WIDTH*32-1:0] o_pay_missing_o,
    output logic [TB_RET_WIDTH*32-1:0] o_csr_unsupported_o,
    output logic        o_order_fault_o,

    // ----------------------------------------------------- observe: the ROB
    output logic        rob_retire_ack_o,
    output logic        rob_retire_ack_next_o,
    output logic        rob_head_valid_o,
    output logic        rob_head_ready_o,
    output logic        rob_head_exc_o,
    output logic        rob_head_closed_o,
    output logic        rob_cmp_accepted_o,
    output logic        rob_cmp_stale_o,
    output logic        rob_cmp_duplicate_o,
    output logic        rob_head_complete_o,
    output logic        rob_head1_complete_o,
    output logic        rob_head1_closed_o,
    output logic [63:0] rob_head_pc_o,
    output logic [31:0] rob_head_tag_o,
    output logic [31:0] rob_head_gen_o,
    output logic [31:0] rob_head_index_o,
    output logic        rob_head1_valid_o,
    output logic        rob_head1_ready_o,
    output logic        rob_head1_exc_o,
    output logic [63:0] rob_head1_pc_o,
    output logic [31:0] rob_head1_tag_o,
    output logic [31:0] rob_head1_gen_o,
    output logic [31:0] rob_head1_index_o,
    output logic        rob_obs_valid_o,
    output logic [31:0] rob_obs_gen_o,
    output logic [31:0] rob_obs_tag_o,
    output logic [31:0] rob_obs_num_uops_o,
    output logic [31:0] rob_obs_done_mask_o,
    output logic        rob_obs_exc_o,
    output logic        rob_obs_closed_o,
    output logic [31:0] rob_occupied_o,
    output logic [31:0] rob_alloc_total_o,
    output logic        rob_alloc_ok_o,
    output logic        rob_alloc_full_o,
    output logic        rob_alloc_bad_o,
    output logic [31:0] rob_alloc_index_o,
    output logic [31:0] rob_alloc_gen_o,
    output logic [31:0] rob_retired_total_o,
    output logic [31:0] rob_squashed_total_o,
    // --------------------------------------------- observe: the committed map
    // One 32-bit word per architectural register: the committed tag, then the
    // committed generation, in two parallel arrays so the driver never has to
    // know how the map is packed inside the module.
    output logic [TB_RET_ARCH*32-1:0] o_cmt_tag_o,
    output logic [TB_RET_ARCH*32-1:0] o_cmt_gen_o,
    output logic [TB_RET_ARCH*32-1:0] o_spec_tag_o,
    output logic [TB_RET_ARCH*32-1:0] o_spec_gen_o,
    output logic [31:0] o_rename_free_o,
    output logic        commit_accepted_o,
    output logic        commit2_accepted_o,
    // ------------------------------------------------------- read-back geometry
    output logic [31:0] o_retire_width_o,
    output logic [31:0] o_rob_entries_o,
    output logic [31:0] o_rob_index_w_o,
    output logic [31:0] o_rob_id_w_o,
    output logic [31:0] o_max_uops_o,
    output logic [31:0] o_prf_entries_o,
    output logic [31:0] o_tag_w_o,
    output logic [31:0] o_arch_regs_o
);
  // The same rules rtl/core/mosaic_retire.sv derives from the same generated
  // package. Widths only; the driver reads the elaborated values back and checks
  // the two against each other.
  // ------------------------------------------------------------------ the ROB
  logic [TB_ROB_CNT_W-1:0]  rob_alloc_num_uops;
  logic [TB_ROB_ID_W-1:0]   rob_alloc_tag;
  logic [TB_RET_RB_W-1:0]   rob_close_index;
  logic [TB_ROB_ID_W-1:0]   rob_close_gen;
  logic [TB_RET_RB_W-1:0]   rob_cmp_index;
  logic [TB_ROB_ID_W-1:0]   rob_cmp_gen;
  logic [TB_ROB_UOP_W-1:0]  rob_cmp_uop;
  logic [TB_RET_RB_W-1:0]   rob_obs_index;
  logic [TB_RET_MAXU-1:0]   rob_head_done_mask;
  logic [TB_RET_MAXU-1:0]   rob_obs_done_mask;
  logic [TB_RET_MAXU-1:0]   rob_head1_done_mask;
  assign rob_alloc_num_uops = alloc_num_uops_i[TB_ROB_CNT_W-1:0];
  assign rob_alloc_tag      = alloc_tag_i[TB_ROB_ID_W-1:0];
  assign rob_close_index    = close_index_i[TB_RET_RB_W-1:0];
  assign rob_close_gen      = close_gen_i[TB_ROB_ID_W-1:0];
  assign rob_cmp_index      = cmp_index_i[TB_RET_RB_W-1:0];
  assign rob_cmp_gen        = cmp_gen_i[TB_ROB_ID_W-1:0];
  assign rob_cmp_uop        = cmp_uop_i[TB_ROB_UOP_W-1:0];
  assign rob_obs_index      = obs_index_i[TB_RET_RB_W-1:0];
  // The ROB's flush is the OR of the two driver recovery flushes
  // (`rob_flush_i` into the queue, `flush_valid_i` into the retire path and
  // hence the ROB) plus the retire unit's own trap flush: a trapping entry
  // leaves the buffer in the cycle it traps, and everything younger with it
  // (the header contract above says exactly this, and phase (c) proves it by
  // demanding the buffer come back empty after the trap).
  logic n_trap_flush_o;
  logic rob_flush;
  assign rob_flush = rob_flush_i | flush_valid_i | n_trap_flush_o;
  // The retire unit's view of the two heads. `rob_ready[0]` *is* the queue's
  // `head_ready` and `rob_ready[1]` is the same predicate one slot on, so the
  // unit cannot hold a private opinion about what is finished.
  logic [TB_RET_WIDTH-1:0] rob_valid;
  logic [TB_RET_WIDTH-1:0] rob_ready;
  logic [TB_RET_WIDTH-1:0] rob_exc;
  logic [TB_RET_WIDTH-1:0] rob_ack;
  logic [TB_RET_WIDTH*TB_RET_ID_W-1:0]  rob_id;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0]  rob_pc;

  assign rob_valid[0] = rob_head_valid_o;
  assign rob_ready[0] = rob_head_ready_o;
  assign rob_exc[0]   = rob_head_exc_o;
  assign rob_pc[TB_RET_XLEN-1:0] = rob_head_pc_o;
  // Explicit slices rather than a concatenation that is then implicitly
  // truncated: an implicit narrowing is a silent one, and a silent narrowing is
  // how a driver ends up comparing a field the DUT never produced.
  assign rob_id[TB_RET_ID_W-1:0] = {rob_head_gen_o[TB_RET_GEN_W-1:0],
                                     rob_head_tag_o[TB_RET_TAG_W-1:0]};

  generate
    if (TB_RET_WIDTH > 1) begin : g_lane1
      assign rob_valid[1] = rob_head1_valid_o;
      assign rob_ready[1] = rob_head1_ready_o;
      assign rob_exc[1]   = rob_head1_exc_o;
      assign rob_pc[TB_RET_XLEN +: TB_RET_XLEN] = rob_head1_pc_o;
      assign rob_id[TB_RET_ID_W +: TB_RET_ID_W] =
          {rob_head1_gen_o[TB_RET_GEN_W-1:0], rob_head1_tag_o[TB_RET_TAG_W-1:0]};
    end
  endgenerate

  // The payload bus, narrowed from the flat vectors.
  logic [TB_RET_WIDTH-1:0]            pay_valid;
  logic [TB_RET_WIDTH-1:0]            pay_reg_we;
  logic [TB_RET_WIDTH*TB_RET_RD_W-1:0]   pay_rd;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0]   pay_value;
  logic [TB_RET_WIDTH-1:0]            pay_csr_we;
  logic [TB_RET_WIDTH*TB_RET_CSR_W-1:0]  pay_csr_addr;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0]   pay_csr_value;
  logic [TB_RET_WIDTH-1:0]            pay_is_store;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0]   pay_store_addr;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0]   pay_store_data;
  logic [TB_RET_WIDTH*TB_RET_SIZE_W-1:0] pay_store_size;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0]   pay_exc_cause;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0]   pay_exc_tval;

  assign pay_valid      = pay_valid_i[TB_RET_WIDTH-1:0];
  assign pay_reg_we     = pay_reg_we_i[TB_RET_WIDTH-1:0];
  assign pay_csr_we     = pay_csr_we_i[TB_RET_WIDTH-1:0];
  assign pay_is_store   = pay_is_store_i[TB_RET_WIDTH-1:0];
  assign pay_rd[TB_RET_RD_W-1:0]        = pay_rd_i[TB_RET_RD_W-1:0];
  assign pay_csr_addr[TB_RET_CSR_W-1:0]  = pay_csr_addr_i[TB_RET_CSR_W-1:0];
  assign pay_store_size[TB_RET_SIZE_W-1:0] = pay_store_size_i[TB_RET_SIZE_W-1:0];
  assign pay_value[TB_RET_XLEN-1:0]      = pay_value_i[TB_RET_XLEN-1:0];
  assign pay_csr_value[TB_RET_XLEN-1:0]  = pay_csr_value_i[TB_RET_XLEN-1:0];
  assign pay_store_addr[TB_RET_XLEN-1:0] = pay_store_addr_i[TB_RET_XLEN-1:0];
  assign pay_store_data[TB_RET_XLEN-1:0] = pay_store_data_i[TB_RET_XLEN-1:0];
  assign pay_exc_cause[TB_RET_XLEN-1:0]  = pay_exc_cause_i[TB_RET_XLEN-1:0];
  assign pay_exc_tval[TB_RET_XLEN-1:0]   = pay_exc_tval_i[TB_RET_XLEN-1:0];

  generate
    if (TB_RET_WIDTH > 1) begin : g_lane1_payload
      assign pay_rd[TB_RET_RD_W +: TB_RET_RD_W]          = pay_rd_i[LANE_W +: TB_RET_RD_W];
      assign pay_csr_addr[TB_RET_CSR_W +: TB_RET_CSR_W]   = pay_csr_addr_i[LANE_W +: TB_RET_CSR_W];
      assign pay_store_size[TB_RET_SIZE_W +: TB_RET_SIZE_W] = pay_store_size_i[LANE_W +: TB_RET_SIZE_W];
      assign pay_value[TB_RET_XLEN +: TB_RET_XLEN]        = pay_value_i[64 +: TB_RET_XLEN];
      assign pay_csr_value[TB_RET_XLEN +: TB_RET_XLEN]    = pay_csr_value_i[64 +: TB_RET_XLEN];
      assign pay_store_addr[TB_RET_XLEN +: TB_RET_XLEN]   = pay_store_addr_i[64 +: TB_RET_XLEN];
      assign pay_store_data[TB_RET_XLEN +: TB_RET_XLEN]   = pay_store_data_i[64 +: TB_RET_XLEN];
      assign pay_exc_cause[TB_RET_XLEN +: TB_RET_XLEN]    = pay_exc_cause_i[64 +: TB_RET_XLEN];
      assign pay_exc_tval[TB_RET_XLEN +: TB_RET_XLEN]     = pay_exc_tval_i[64 +: TB_RET_XLEN];
    end
  endgenerate

  logic [TB_RET_CSR_W-1:0] csr_rd_addr;
  assign csr_rd_addr = csr_rd_addr_i[TB_RET_CSR_W-1:0];

  logic [TB_RET_WIDTH*(1)-1:0] n_retire_req;
  logic [TB_RET_WIDTH*(1)-1:0] n_ev_valid;
  logic [TB_RET_WIDTH*(1)-1:0] n_ev_trap;
  logic [TB_RET_WIDTH*(TB_RET_SEQ_W)-1:0] n_ev_seq;
  logic [TB_RET_WIDTH*(TB_RET_ID_W)-1:0] n_ev_id;
  logic [TB_RET_WIDTH*(1)-1:0] n_ev_reg_we;
  logic [TB_RET_WIDTH*(TB_RET_RD_W)-1:0] n_ev_rd;
  logic [TB_RET_WIDTH*(1)-1:0] n_ev_csr_we;
  logic [TB_RET_WIDTH*(TB_RET_CSR_W)-1:0] n_ev_csr_addr;
  logic [TB_RET_WIDTH*(1)-1:0] n_ev_store;
  logic [TB_RET_WIDTH*(TB_RET_SIZE_W)-1:0] n_ev_store_size;
  logic [TB_RET_WIDTH*(1)-1:0] n_commit_valid;
  logic [TB_RET_WIDTH*(TB_RET_RD_W)-1:0] n_commit_rd;
  logic [TB_RET_WIDTH*(TB_RET_TAG_W)-1:0] n_commit_tag;
  logic [TB_RET_WIDTH*(TB_RET_GEN_W)-1:0] n_commit_gen;
  logic [TB_RET_WIDTH*(1)-1:0] n_o_exc_queued;
  logic [TB_RET_WIDTH*(1)-1:0] n_o_pay_missing;
  logic [TB_RET_WIDTH*(1)-1:0] n_o_csr_unsupported;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0] n_ev_pc;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0] n_ev_value;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0] n_ev_csr_value;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0] n_ev_store_addr;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0] n_ev_store_data;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0] n_ev_trap_cause;
  logic [TB_RET_WIDTH*TB_RET_XLEN-1:0] n_ev_trap_tval;
  logic n_trap_valid_o;
  logic n_csr_rd_unsupported_o;
  logic n_o_x0_retired_o;
  logic n_o_order_fault_o;
  logic [TB_RET_XLEN-1:0] n_trap_pc_o;
  logic [TB_RET_XLEN-1:0] n_trap_cause_o;
  logic [TB_RET_XLEN-1:0] n_trap_tval_o;
  logic [TB_RET_XLEN-1:0] n_csr_rd_data_o;
  logic [TB_RET_XLEN-1:0] n_o_retire_seq_o;
  logic [TB_RET_XLEN-1:0] n_o_minstret_o;
  logic [TB_RET_XLEN-1:0] n_o_mcycle_o;
  logic [TB_RET_XLEN-1:0] n_o_mscratch_o;
  logic [TB_RET_CNT_W-1:0] n_o_event_count;


  // ------------------------------------------------- packing out to the driver

  //

  // Every adaptation between the unit's narrow port and the driver's 32-bit

  // lane word is an explicit zero-extended slice. An implicit assignment

  // between different widths silently truncates, and a silent truncation lets

  // the driver compare a value the DUT never produced against a zero that

  // happens to match -- the comparison would pass for the wrong reason.

  generate

    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack
      assign retire_req_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_retire_req[i*(1) +: (1)]};
      assign ev_valid_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_ev_valid[i*(1) +: (1)]};
      assign ev_trap_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_ev_trap[i*(1) +: (1)]};
      assign ev_seq_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_SEQ_W)) {1'b0}}, n_ev_seq[i*(TB_RET_SEQ_W) +: (TB_RET_SEQ_W)]};
      assign ev_id_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_ID_W)) {1'b0}}, n_ev_id[i*(TB_RET_ID_W) +: (TB_RET_ID_W)]};
      assign ev_reg_we_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_ev_reg_we[i*(1) +: (1)]};
      assign ev_rd_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_RD_W)) {1'b0}}, n_ev_rd[i*(TB_RET_RD_W) +: (TB_RET_RD_W)]};
      assign ev_csr_we_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_ev_csr_we[i*(1) +: (1)]};
      assign ev_csr_addr_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_CSR_W)) {1'b0}}, n_ev_csr_addr[i*(TB_RET_CSR_W) +: (TB_RET_CSR_W)]};
      assign ev_store_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_ev_store[i*(1) +: (1)]};
      assign ev_store_size_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_SIZE_W)) {1'b0}}, n_ev_store_size[i*(TB_RET_SIZE_W) +: (TB_RET_SIZE_W)]};
      assign commit_valid_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_commit_valid[i*(1) +: (1)]};
      assign commit_rd_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_RD_W)) {1'b0}}, n_commit_rd[i*(TB_RET_RD_W) +: (TB_RET_RD_W)]};
      assign commit_tag_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_TAG_W)) {1'b0}}, n_commit_tag[i*(TB_RET_TAG_W) +: (TB_RET_TAG_W)]};
      assign commit_gen_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(TB_RET_GEN_W)) {1'b0}}, n_commit_gen[i*(TB_RET_GEN_W) +: (TB_RET_GEN_W)]};
      assign o_exc_queued_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_o_exc_queued[i*(1) +: (1)]};
      assign o_pay_missing_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_o_pay_missing[i*(1) +: (1)]};
      assign o_csr_unsupported_o[i*LANE_W +: LANE_W] =
          {{(LANE_W-(1)) {1'b0}}, n_o_csr_unsupported[i*(1) +: (1)]};
    end
  endgenerate
  generate
    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack_ev_pc
      assign ev_pc_o[i*64 +: 64] = n_ev_pc[i*TB_RET_XLEN +: TB_RET_XLEN];
    end
  endgenerate
  generate
    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack_ev_value
      assign ev_value_o[i*64 +: 64] = n_ev_value[i*TB_RET_XLEN +: TB_RET_XLEN];
    end
  endgenerate
  generate
    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack_ev_csr_value
      assign ev_csr_value_o[i*64 +: 64] = n_ev_csr_value[i*TB_RET_XLEN +: TB_RET_XLEN];
    end
  endgenerate
  generate
    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack_ev_store_addr
      assign ev_store_addr_o[i*64 +: 64] = n_ev_store_addr[i*TB_RET_XLEN +: TB_RET_XLEN];
    end
  endgenerate
  generate
    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack_ev_store_data
      assign ev_store_data_o[i*64 +: 64] = n_ev_store_data[i*TB_RET_XLEN +: TB_RET_XLEN];
    end
  endgenerate
  generate
    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack_ev_trap_cause
      assign ev_trap_cause_o[i*64 +: 64] = n_ev_trap_cause[i*TB_RET_XLEN +: TB_RET_XLEN];
    end
  endgenerate
  generate
    for (genvar i = 0; i < TB_RET_WIDTH; i++) begin : g_pack_ev_trap_tval
      assign ev_trap_tval_o[i*64 +: 64] = n_ev_trap_tval[i*TB_RET_XLEN +: TB_RET_XLEN];
    end
  endgenerate
  assign trap_flush_o = n_trap_flush_o;
  assign trap_valid_o = n_trap_valid_o;
  assign csr_rd_unsupported_o = n_csr_rd_unsupported_o;
  assign o_x0_retired_o = n_o_x0_retired_o;
  assign o_order_fault_o = n_o_order_fault_o;
  assign trap_pc_o = n_trap_pc_o;
  assign trap_cause_o = n_trap_cause_o;
  assign trap_tval_o = n_trap_tval_o;
  assign csr_rd_data_o = n_csr_rd_data_o;
  assign o_retire_seq_o = n_o_retire_seq_o;
  assign o_minstret_o = n_o_minstret_o;
  assign o_mcycle_o = n_o_mcycle_o;
  assign o_mscratch_o = n_o_mscratch_o;
  assign o_event_count_o = {{(LANE_W-TB_RET_CNT_W) {1'b0}}, n_o_event_count};

  mosaic_retire u_retire (
      .clk                (clk),
      .rst                (rst),

      .rob_valid          (rob_valid),
      .rob_ready          (rob_ready),
      .rob_exc            (rob_exc),
      .rob_ack            (rob_ack),
      .rob_id             (rob_id),
      .rob_pc             (rob_pc),

      .pay_valid          (pay_valid),
      .pay_reg_we         (pay_reg_we),
      .pay_rd             (pay_rd),
      .pay_value          (pay_value),
      .pay_csr_we         (pay_csr_we),
      .pay_csr_addr       (pay_csr_addr),
      .pay_csr_value      (pay_csr_value),
      .pay_is_store       (pay_is_store),
      .pay_store_addr     (pay_store_addr),
      .pay_store_data     (pay_store_data),
      .pay_store_size     (pay_store_size),
      .pay_exc_cause      (pay_exc_cause),
      .pay_exc_tval       (pay_exc_tval),

      .flush_valid        (flush_valid_i),

      .retire_req         (n_retire_req),
      .trap_flush         (n_trap_flush_o),

      .ev_valid           (n_ev_valid),
      .ev_trap            (n_ev_trap),
      .ev_seq             (n_ev_seq),
      .ev_pc              (n_ev_pc),
      .ev_id              (n_ev_id),
      .ev_reg_we          (n_ev_reg_we),
      .ev_rd              (n_ev_rd),
      .ev_value           (n_ev_value),
      .ev_csr_we          (n_ev_csr_we),
      .ev_csr_addr        (n_ev_csr_addr),
      .ev_csr_value       (n_ev_csr_value),
      .ev_store           (n_ev_store),
      .ev_store_addr      (n_ev_store_addr),
      .ev_store_data      (n_ev_store_data),
      .ev_store_size      (n_ev_store_size),
      .ev_trap_cause      (n_ev_trap_cause),
      .ev_trap_tval       (n_ev_trap_tval),

      .trap_valid         (n_trap_valid_o),
      .trap_pc            (n_trap_pc_o),
      .trap_cause         (n_trap_cause_o),
      .trap_tval          (n_trap_tval_o),

      .commit_valid       (n_commit_valid),
      .commit_rd          (n_commit_rd),
      .commit_tag         (n_commit_tag),
      .commit_gen         (n_commit_gen),

      .csr_rd_valid       (csr_rd_valid_i),
      .csr_rd_addr        (csr_rd_addr),
      .csr_rd_data        (n_csr_rd_data_o),
      .csr_rd_unsupported (n_csr_rd_unsupported_o),

      .o_retire_seq       (n_o_retire_seq_o),
      .o_minstret         (n_o_minstret_o),
      .o_mcycle           (n_o_mcycle_o),
      .o_mscratch         (n_o_mscratch_o),
      .o_exc_queued       (n_o_exc_queued),
      .o_event_count      (n_o_event_count),
      .o_x0_retired       (n_o_x0_retired_o),
      .o_pay_missing      (n_o_pay_missing),
      .o_csr_unsupported  (n_o_csr_unsupported),
      .o_order_fault      (n_o_order_fault_o)
  );

  // The acks come back from the ROB: the unit emits what was *acknowledged*,
  // not what it requested, and this is the wire that proves the two agree.
  assign rob_ack[0] = rob_retire_ack_o;
  generate
    if (TB_RET_WIDTH > 1) begin : g_lane1_ack
      assign rob_ack[1] = rob_retire_ack_next_o;
    end
  endgenerate

  // --------------------------------------------------------- the committed map
  logic [TB_RET_TAG_W-1:0] commit_tag_lane0;
  logic [TB_RET_GEN_W-1:0] commit_gen_lane0;
  logic [TB_RET_TAG_W-1:0] commit_tag_lane1;
  logic [TB_RET_GEN_W-1:0] commit_gen_lane1;

  assign commit_tag_lane0 = commit_tag_o[TB_RET_TAG_W-1:0];
  assign commit_gen_lane0 = commit_gen_o[TB_RET_GEN_W-1:0];

  // Lane 1's rename feeds come from the PACKED driver words (commit_tag_o[1]),
  // not from the retire nets: the retire unit's narrow commit port and the
  // 32-bit driver word are different signals after the zero-extending pack.
  // Reading lane 1's tag from the narrow net aliases lane 0's tag into lane 1
  // whenever TAG_W != 32, so a two-wide retire commits lane 0 twice and lane
  // 1's architectural register never moves.
  generate
    if (TB_RET_WIDTH > 1) begin : g_lane1_commit
      assign commit_tag_lane1 = commit_tag_o[LANE_W +: TB_RET_TAG_W];
      assign commit_gen_lane1 = commit_gen_o[LANE_W +: TB_RET_GEN_W];
    end
  endgenerate


  // The driver-facing 32-bit words built from the ROB's narrow nets. Only the
  // fields the driver actually reads are packed; the rest are connected to
  // narrow nets and left there, because packing a value nobody reads is a
  // truncation with no observer.


  logic [31:0] rob_head_num_uops;
  logic [31:0] rob_head_done_cnt;
  logic [31:0] rob_head1_num_uops;
  logic [31:0] rob_head1_done_cnt;
  // Every ROB output the driver does not compare is still connected, to a
  // declared net. A dangling output is a `PINMISSING` warning, and this build
  // treats warnings as errors.
  logic [TB_ROB_ID_W-1:0]  rob_alloc_gen_n;
  logic [TB_RET_RB_W-1:0]  rob_alloc_index_n;
  logic                 rob_alloc_refused;
  logic                 rob_close_ok;
  logic                 rob_close_stale;
  logic                 rob_cmp_accepted;
  logic                 rob_cmp_duplicate;
  logic                 rob_cmp_stale;
  logic                 rob_cmp_bad_uop;
  logic                 rob_head_replay;
  logic                 rob_head_complete;
  logic                 rob_head1_replay;
  logic                 rob_head1_complete;
  logic                 rob_head1_closed;
  logic                 rob_obs_valid;
  logic [TB_ROB_ID_W-1:0]  rob_obs_gen_n;
  logic [TB_ROB_ID_W-1:0]  rob_obs_tag_n;
  logic [TB_RET_XLEN-1:0]  rob_obs_pc;
  logic                 rob_obs_exc;
  logic                 rob_obs_closed;
  logic [TB_RET_RB_W-1:0]  rob_head_ptr_n;
  logic [TB_RET_RB_W-1:0]  rob_alloc_ptr_n;
  logic [TB_ROB_OCC_W-1:0] rob_free;
  logic [31:0]          rob_gen_counter;
  // -------------------------------------- the ROB's observation nets
  //
  // The ROB's ports are narrower than the driver's 32-bit words. Every
  // adaptation is an explicit zero-extended slice rather than an implicit
  // assignment between widths, because an implicit one truncates silently
  // and a silent truncation lets a driver compare a field against a value
  // that was never produced.
  logic [TB_RET_RB_W-1:0]  rob_head_index_n;
  logic [TB_ROB_ID_W-1:0]   rob_head_gen_n;
  logic [TB_ROB_ID_W-1:0]   rob_head_tag_n;
  logic [TB_RET_RB_W-1:0]  rob_head1_index_n;
  logic [TB_ROB_ID_W-1:0]   rob_head1_gen_n;
  logic [TB_ROB_ID_W-1:0]   rob_head1_tag_n;
  logic [TB_ROB_OCC_W-1:0]  rob_occupied_n;
  logic [TB_ROB_CNT_W-1:0]  rob_head_numuops_n;
  logic [TB_ROB_CNT_W-1:0]  rob_head_donecnt_n;
  logic [TB_ROB_CNT_W-1:0]  rob_head1_numuops_n;
  logic [TB_ROB_CNT_W-1:0]  rob_head1_donecnt_n;
  logic [TB_ROB_CNT_W-1:0]  rob_obs_numuops_n;
  logic [TB_ROB_CNT_W-1:0]  rob_obs_donecnt_n;

  assign rob_head_index_o  = {{(32-TB_RET_RB_W) {1'b0}}, rob_head_index_n};
  assign rob_alloc_index_o = {{(32-TB_RET_RB_W) {1'b0}}, rob_alloc_index_n};
  assign rob_alloc_gen_o   = {{(32-TB_ROB_ID_W) {1'b0}}, rob_alloc_gen_n};
  // The completion and head-state reports the driver cross-checks: whether the
  // buffer accepted this cycle's completion (and why not), and whether each
  // head is complete and closed. Without them a completion the buffer files
  // as stale is invisible, and the shadow alone marks the entry done.
  assign rob_cmp_accepted_o  = rob_cmp_accepted;
  assign rob_cmp_stale_o     = rob_cmp_stale;
  assign rob_cmp_duplicate_o = rob_cmp_duplicate;
  assign rob_head_complete_o = rob_head_complete;
  assign rob_head1_complete_o = rob_head1_complete;
  assign rob_head1_closed_o  = rob_head1_closed;
  assign rob_head_tag_o    = {{(32-TB_ROB_ID_W) {1'b0}}, rob_head_tag_n};
  assign rob_head_gen_o    = {{(32-TB_ROB_ID_W) {1'b0}}, rob_head_gen_n};
  assign rob_head1_gen_o   = {{(32-TB_ROB_ID_W) {1'b0}}, rob_head1_gen_n};
  assign rob_head1_tag_o   = {{(32-TB_ROB_ID_W) {1'b0}}, rob_head1_tag_n};
  assign rob_occupied_o    = {{(32-TB_ROB_OCC_W) {1'b0}}, rob_occupied_n};
  // The slot observation the driver reconciles against: valid, generation,
  // tag, child count, done mask, exception, and closed -- everything the
  // shadow tracks per entry, read straight from the buffer's own arrays.
  assign rob_obs_valid_o     = rob_obs_valid;
  assign rob_obs_gen_o       = {{(32-TB_ROB_ID_W) {1'b0}}, rob_obs_gen_n};
  assign rob_obs_tag_o       = {{(32-TB_ROB_ID_W) {1'b0}}, rob_obs_tag_n};
  assign rob_obs_num_uops_o  = {{(32-TB_ROB_CNT_W) {1'b0}}, rob_obs_numuops_n};
  assign rob_obs_done_mask_o = {{(32-TB_RET_MAXU) {1'b0}}, rob_obs_done_mask};
  assign rob_obs_exc_o       = rob_obs_exc;
  assign rob_obs_closed_o    = rob_obs_closed;
  assign rob_head_num_uops = {{(32-TB_ROB_CNT_W) {1'b0}}, rob_head_numuops_n};
  assign rob_head_done_cnt = {{(32-TB_ROB_CNT_W) {1'b0}}, rob_head_donecnt_n};
  assign rob_head1_num_uops = {{(32-TB_ROB_CNT_W) {1'b0}}, rob_head1_numuops_n};
  assign rob_head1_done_cnt = {{(32-TB_ROB_CNT_W) {1'b0}}, rob_head1_donecnt_n};

  mosaic_rob u_rob (
      .clk              (clk),
      .rst              (rst),

      .alloc_valid      (alloc_valid_i),
      .alloc_tag        (rob_alloc_tag),
      .alloc_pc         (alloc_pc_i),
      .alloc_num_uops   (rob_alloc_num_uops),
      .alloc_exc        (alloc_exc_i),
      .alloc_open       (alloc_open_i),
      .alloc_ok         (rob_alloc_ok_o),
      .alloc_refused    (rob_alloc_refused),
      .alloc_full       (rob_alloc_full_o),
      .alloc_bad_uops   (rob_alloc_bad_o),
      .alloc_index      (rob_alloc_index_n),
      .alloc_gen        (rob_alloc_gen_n),

      .close_valid      (close_valid_i),
      .close_index      (rob_close_index),
      .close_gen        (rob_close_gen),
      .close_ok         (rob_close_ok),
      .close_stale      (rob_close_stale),

      .cmp_valid        (cmp_valid_i),
      .cmp_index        (rob_cmp_index),
      .cmp_gen          (rob_cmp_gen),
      .cmp_uop          (rob_cmp_uop),
      .cmp_exc          (cmp_exc_i),
      .cmp_accepted     (rob_cmp_accepted),
      .cmp_duplicate    (rob_cmp_duplicate),
      .cmp_stale        (rob_cmp_stale),
      .cmp_bad_uop      (rob_cmp_bad_uop),

      // The retire unit drives both lanes of the pop.
      .retire_req       (n_retire_req[0]),
      .retire_ack       (rob_retire_ack_o),
      .retire_req_next  (n_retire_req[1]),
      .retire_ack_next  (rob_retire_ack_next_o),

      .head_valid       (rob_head_valid_o),
      .head_ready       (rob_head_ready_o),
      .head_replay      (rob_head_replay),
      .head_complete    (rob_head_complete),
      .head_exc         (rob_head_exc_o),
      .head_closed      (rob_head_closed_o),
      .head_index       (rob_head_index_n),
      .head_gen         (rob_head_gen_n),
      .head_tag         (rob_head_tag_n),
      .head_pc          (rob_head_pc_o),
      .head_num_uops    (rob_head_numuops_n),
      .head_done_mask   (rob_head_done_mask),
      .head_done_cnt    (rob_head_donecnt_n),

      .head1_valid      (rob_head1_valid_o),
      .head1_ready      (rob_head1_ready_o),
      .head1_replay     (rob_head1_replay),
      .head1_complete   (rob_head1_complete),
      .head1_exc        (rob_head1_exc_o),
      .head1_closed     (rob_head1_closed),
      .head1_index      (rob_head1_index_n),
      .head1_gen        (rob_head1_gen_n),
      .head1_tag        (rob_head1_tag_n),
      .head1_pc         (rob_head1_pc_o),
      .head1_num_uops   (rob_head1_numuops_n),
      .head1_done_mask  (rob_head1_done_mask),
      .head1_done_cnt   (rob_head1_donecnt_n),

      .flush_valid      (rob_flush),

      .obs_index        (rob_obs_index),
      .obs_valid        (rob_obs_valid),
      .obs_gen          (rob_obs_gen_n),
      .obs_tag          (rob_obs_tag_n),
      .obs_pc           (rob_obs_pc),
      .obs_num_uops     (rob_obs_numuops_n),
      .obs_done_mask    (rob_obs_done_mask),
      .obs_done_cnt     (rob_obs_donecnt_n),
      .obs_exc          (rob_obs_exc),
      .obs_closed       (rob_obs_closed),

      .o_head_ptr       (rob_head_ptr_n),
      .o_alloc_ptr      (rob_alloc_ptr_n),
      .o_occupied       (rob_occupied_n),
      .o_free           (rob_free),
      .o_alloc_total    (rob_alloc_total_o),
      .o_retired_total  (rob_retired_total_o),
      .o_squashed_total (rob_squashed_total_o),
      .o_gen_counter    (rob_gen_counter)
  );


  logic                     ren_alloc_accepted;
  logic                     ren_alloc_exhausted;
  logic                     ren_alloc_squashed;
  logic                     ren_alloc_is_x0;
  logic                     ren_alloc_new_valid;
  logic [TB_RET_TAG_W-1:0]     ren_alloc_new_tag;
  logic [TB_RET_GEN_W-1:0]     ren_alloc_new_gen;
  logic                     ren_alloc_old_valid;
  logic [TB_RET_TAG_W-1:0]     ren_alloc_old_tag;
  logic [TB_RET_GEN_W-1:0]     ren_alloc_old_gen;
  logic                     ren_rs1_is_x0;
  logic                     ren_rs2_is_x0;
  logic [TB_RET_TAG_W-1:0]     ren_rs1_tag;
  logic [TB_RET_TAG_W-1:0]     ren_rs2_tag;
  logic [TB_RET_GEN_W-1:0]     ren_rs1_gen;
  logic [TB_RET_GEN_W-1:0]     ren_rs2_gen;
  logic                     ren_wb_accepted;
  logic                     ren_wb_stale;
  logic                     ren_wb_duplicate;
  logic                     ren_free_accepted;
  logic                     ren_free_stale;
  logic                     ren_free_double;
  logic                     ren_commit_x0_dropped;
  logic                     ren_commit2_x0_dropped;
  logic                     ren_squash_accepted;
  logic                     ren_squash_underflow;
  logic                     ren_journal_overflow;
  logic [TB_RET_TAG_W:0]     ren_free_count;
  assign o_rename_free_o = {{(32-(TB_RET_TAG_W+1)) {1'b0}}, ren_free_count};
  logic [TB_RET_PRF-1:0]       ren_free_mask;
  logic [TB_RET_PRF-1:0]       ren_gen_valid;
  logic [TB_RET_PRF-1:0]       ren_wb_done;
  logic [TB_RET_PRF*TB_RET_GEN_W-1:0] ren_tag_gen;
  logic [TB_RET_ARCH*(TB_RET_TAG_W+TB_RET_GEN_W)-1:0] ren_spec_map;
  logic [TB_RET_ARCH*(TB_RET_TAG_W+TB_RET_GEN_W)-1:0] ren_cmt_map;
  // I-014 added a second allocation lane and a second source-read pair. This
  // card tests retirement, so lane 1's allocation is tied inactive
  // (`alloc2_req`/`rs3_addr`/`rs4_addr` = 0), which is exactly the single-width
  // machine: every lane-1 output is then a refusal/idle and the committed map
  // behaves as it did before. The outputs are still connected, because an
  // omitted pin is a build error here (Verilator PINMISSING under -Wall) and
  // because leaving an input unconnected would let it float.
  logic                       ren_alloc2_accepted;
  logic                       ren_alloc2_exhausted;
  logic                       ren_alloc2_squashed;
  logic                       ren_alloc2_is_x0;
  logic                       ren_alloc2_new_valid;
  logic [TB_RET_TAG_W-1:0]    ren_alloc2_new_tag;
  logic [TB_RET_GEN_W-1:0]    ren_alloc2_new_gen;
  logic                       ren_alloc2_old_valid;
  logic [TB_RET_TAG_W-1:0]    ren_alloc2_old_tag;
  logic [TB_RET_GEN_W-1:0]    ren_alloc2_old_gen;
  logic                       ren_rs1_ready;
  logic                       ren_rs2_ready;
  logic                       ren_rs3_is_x0;
  logic                       ren_rs4_is_x0;
  logic                       ren_rs3_ready;
  logic                       ren_rs4_ready;
  logic                       ren_rs3_bypass;
  logic                       ren_rs4_bypass;
  logic [TB_RET_TAG_W-1:0]    ren_rs3_tag;
  logic [TB_RET_TAG_W-1:0]    ren_rs4_tag;
  logic [TB_RET_GEN_W-1:0]    ren_rs3_gen;
  logic [TB_RET_GEN_W-1:0]    ren_rs4_gen;
  // The undo-window depth's width is $clog2(entries+1) by the module's own rule;
  // derived here from the same generated geometry rather than hardcoded.
  localparam int unsigned TB_RET_JLEN_W = $clog2(TB_RET_ROB + 1);
  logic [TB_RET_JLEN_W-1:0]   ren_j_len;

  // The per-register slices the packing below reads, declared here because
  // SystemVerilog requires a declaration before use and a generate block cannot
  // declare an unpacked array at module scope.
  localparam int unsigned MAP_W = TB_RET_TAG_W + TB_RET_GEN_W;
  logic [TB_RET_ARCH-1:0][TB_RET_TAG_W-1:0] map_tag;
  logic [TB_RET_ARCH-1:0][TB_RET_GEN_W-1:0] map_gen;
  logic [TB_RET_ARCH-1:0][TB_RET_TAG_W-1:0] map_stag;
  logic [TB_RET_ARCH-1:0][TB_RET_GEN_W-1:0] map_sgen;
  mosaic_rename u_ren (
      .clk               (clk),
      .rst               (rst),

      // Allocation, writeback and free are idle: this card tests retirement, and
      // an allocation moving the free set would make the committed map's free
      // count move for a reason that has nothing to do with retiring.
      .alloc_req         (1'b0),
      .alloc_rd          (5'd0),
      .alloc_accepted    (ren_alloc_accepted),
      .alloc_exhausted   (ren_alloc_exhausted),
      .alloc_squashed    (ren_alloc_squashed),
      .alloc_is_x0       (ren_alloc_is_x0),
      .alloc_new_valid   (ren_alloc_new_valid),
      .alloc_new_tag     (ren_alloc_new_tag),
      .alloc_new_gen     (ren_alloc_new_gen),
      .alloc_old_valid   (ren_alloc_old_valid),
      .alloc_old_tag     (ren_alloc_old_tag),
      .alloc_old_gen     (ren_alloc_old_gen),

      // Lane 1 of the allocation group is inactive, so every `alloc2_*` output
      // is the module's refusal/idle answer and the group is single-width.
      .alloc2_req        (1'b0),
      .alloc2_rd         (5'd0),
      .alloc2_accepted   (ren_alloc2_accepted),
      .alloc2_exhausted  (ren_alloc2_exhausted),
      .alloc2_squashed   (ren_alloc2_squashed),
      .alloc2_is_x0      (ren_alloc2_is_x0),
      .alloc2_new_valid  (ren_alloc2_new_valid),
      .alloc2_new_tag    (ren_alloc2_new_tag),
      .alloc2_new_gen    (ren_alloc2_new_gen),
      .alloc2_old_valid  (ren_alloc2_old_valid),
      .alloc2_old_tag    (ren_alloc2_old_tag),
      .alloc2_old_gen    (ren_alloc2_old_gen),

      .rs1_addr          (5'd0),
      .rs2_addr          (5'd0),
      .rs1_is_x0         (ren_rs1_is_x0),
      .rs2_is_x0         (ren_rs2_is_x0),
      .rs1_ready         (ren_rs1_ready),
      .rs2_ready         (ren_rs2_ready),
      .rs1_tag           (ren_rs1_tag),
      .rs2_tag           (ren_rs2_tag),
      .rs1_gen           (ren_rs1_gen),
      .rs2_gen           (ren_rs2_gen),
      // The second source-read pair is addressed at x0 for the same reason: no
      // read, no bypass, no readiness dependency to model in this card.
      .rs3_addr          (5'd0),
      .rs4_addr          (5'd0),
      .rs3_is_x0         (ren_rs3_is_x0),
      .rs4_is_x0         (ren_rs4_is_x0),
      .rs3_ready         (ren_rs3_ready),
      .rs4_ready         (ren_rs4_ready),
      .rs3_bypass        (ren_rs3_bypass),
      .rs4_bypass        (ren_rs4_bypass),
      .rs3_tag           (ren_rs3_tag),
      .rs4_tag           (ren_rs4_tag),
      .rs3_gen           (ren_rs3_gen),
      .rs4_gen           (ren_rs4_gen),

      .wb_valid          (1'b0),
      .wb_tag            ('0),
      .wb_gen            ('0),
      .wb_accepted       (ren_wb_accepted),
      .wb_stale          (ren_wb_stale),
      .wb_duplicate      (ren_wb_duplicate),

      .free_valid        (1'b0),
      .free_tag          ('0),
      .free_gen          ('0),
      .free_accepted     (ren_free_accepted),
      .free_stale        (ren_free_stale),
      .free_double       (ren_free_double),

      // The two commit lanes, straight from the retire unit and in lane order.
      // All four lane-1 fields are sliced at LANE_W: the driver words are
      // 32-bit-per-lane vectors, so bit 1 is lane 1's valid while bit 32 is
      // lane 0's rd[1]. Reading valid/rd/tag/gen through mixed strides commits
      // lane 0 under lane 1's name on every two-wide retire.
      .commit_valid      (commit_valid_o[0]),
      .commit_rd         (commit_rd_o[TB_RET_RD_W-1:0]),
      .commit_tag        (commit_tag_lane0),
      .commit_gen        (commit_gen_lane0),
      .commit_accepted   (commit_accepted_o),
      .commit_x0_dropped (ren_commit_x0_dropped),
      .commit2_valid     (commit_valid_o[LANE_W]),
      .commit2_rd        (commit_rd_o[LANE_W +: TB_RET_RD_W]),
      .commit2_tag       (commit_tag_lane1),
      .commit2_gen       (commit_gen_lane1),
      .commit2_accepted  (commit2_accepted_o),
      .commit2_x0_dropped(ren_commit2_x0_dropped),

      // Recovery is I-018's card, so `squash` is tied low here. That is what
      // makes "retire never disturbs the speculative map" checkable at all.
      .ckpt_valid        (1'b0),
      .squash            (1'b0),
      .squash_accepted   (ren_squash_accepted),
      .squash_underflow  (ren_squash_underflow),
      .journal_overflow  (ren_journal_overflow),

      .free_count        (ren_free_count),

      .dbg_free_mask     (ren_free_mask),
      .dbg_gen_valid     (ren_gen_valid),
      .dbg_wb_done       (ren_wb_done),
      .dbg_tag_gen       (ren_tag_gen),
      .dbg_spec_map      (ren_spec_map),
      .dbg_cmt_map       (ren_cmt_map),
      .dbg_j_len         (ren_j_len)
  );



  // ------------------------------------------------------ repacking the maps
  //
  // The map is packed as {gen, tag} per architectural register inside the module.
  // It is unpacked here into two flat arrays of one 32-bit word per register, so
  // the driver compares the state and not the packing. The unpacking is written
  // with explicit slices rather than a variable-index loop, because a chained
  // part select passes Verilator and slang rejects it.
  generate
    for (genvar a = 0; a < TB_RET_ARCH; a++) begin : g_map_flat
      // Sliced into a named narrow net and then zero-extended, rather than a
      // part-select fed straight into a 32-bit concatenation: the second form
      // reads as though it widened when it did not, and the widening is exactly
      // what the driver is relying on.
      assign map_tag[a]  = ren_cmt_map[a*MAP_W +: TB_RET_TAG_W];
      assign map_gen[a]  = ren_cmt_map[a*MAP_W + MAP_W-1 -: TB_RET_GEN_W];
      assign map_stag[a] = ren_spec_map[a*MAP_W +: TB_RET_TAG_W];
      assign map_sgen[a] = ren_spec_map[a*MAP_W + MAP_W-1 -: TB_RET_GEN_W];

      assign o_cmt_tag_o[a*LANE_W +: LANE_W]  =
          {{(LANE_W-TB_RET_TAG_W) {1'b0}}, map_tag[a]};
      assign o_cmt_gen_o[a*LANE_W +: LANE_W]  =
          {{(LANE_W-TB_RET_GEN_W) {1'b0}}, map_gen[a]};
      assign o_spec_tag_o[a*LANE_W +: LANE_W] =
          {{(LANE_W-TB_RET_TAG_W) {1'b0}}, map_stag[a]};
      assign o_spec_gen_o[a*LANE_W +: LANE_W] =
          {{(LANE_W-TB_RET_GEN_W) {1'b0}}, map_sgen[a]};
    end
  endgenerate

  // --------------------------------------------------------- read-back geometry
  //
  // Read from the elaborated `mosaic_rob` **instance parameters** rather than
  // from the derivation above, so the driver can check the two against each
  // other: a geometry file change that desynchronised them shows up as a failing
  // check rather than as a testbench quietly comparing the wrong field.
  //
  // The retire unit's and rename's widths are *not* read back this way, and the
  // reason is worth stating rather than papering over: `RET_WIDTH`, `RET_TAG_W`
  // and friends are file-scope `localparam`s, not module parameters, so there is
  // no `u_retire.RET_WIDTH` to read. They are therefore reported from the same
  // package this file already derives them from -- still one geometry, not two --
  // and the driver's cross-checks on them are behavioural rather than
  // structural: it proves the retire width by observing that two events really
  // do come out in one cycle, which a wrong width cannot fake.
  assign o_rob_entries_o  = 32'(u_rob.ROB_ENTRIES);
  assign o_rob_index_w_o  = 32'(u_rob.ROB_INDEX_W);
  assign o_rob_id_w_o     = 32'(u_rob.GEN_W);
  assign o_max_uops_o     = 32'(u_rob.MAX_UOPS);
  assign o_retire_width_o = 32'(TB_RET_WIDTH);
  assign o_prf_entries_o  = 32'(TB_RET_PRF);
  assign o_tag_w_o        = 32'(TB_RET_TAG_W);
  assign o_arch_regs_o    = 32'(TB_RET_ARCH);

endmodule : mosaic_retire_tb

`resetall
`default_nettype wire
