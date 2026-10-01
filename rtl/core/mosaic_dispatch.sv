// ============================================================================
// mosaic_dispatch -- the front of the machine (work package I-023).
//
// One decoded macro per cycle is allocated -- a rename group of one, a ROB
// entry of one -- and then inserted into its cluster's issue queue when that
// queue can take it. The two steps are decoupled, because the allocation is
// irrevocable (rename's free list, the ROB occupancy and the descriptor store
// all move) while the insert may have to wait for issue-queue space. The macro
// waits in a small dispatch queue whose depth is argued below.
//
// ------------------------------------------------------------ why one, not two
//
// The task's shape is a two-wide group. `mosaic_rob` has a **single** allocation
// port, so at most one ROB entry can be created per cycle, and an allocation
// that is half in rename and half in the ROB is exactly the atomicity the
// two-wide rename group exists to prevent. The core therefore presents rename
// with groups of one and allocates one entry per cycle, and the two-cluster
// affinity is applied across the cycle pair: the first macro of a fetched pair
// goes to cluster 0 and the second to cluster 1 (see `target_cluster` below).
// Reaching two macro allocations per cycle needs a second ROB allocation port
// from I-016; it is reported rather than worked around.
//
// ------------------------------------------------------- unsupported macros
//
// This package does **not** service UOP_LOAD, UOP_STORE or UOP_SYSTEM, and not
// fence/fence.i. The memory path is I-033..I-038 and the CSR/trap path is
// I-019/I-020. Dispatching one of them into an execution unit that cannot
// complete it would leave a macro in the ROB that never completes -- a hang
// dressed up as execution. So they are refused, loudly and counted, and the
// refusal happens **before the group is presented to rename**: nothing is
// allocated, nothing is leaked, and the machine stops cleanly at that
// instruction instead of executing something wrong. That ordering is the whole
// reason the check sits here rather than at the issue queue: rename has already
// allocated by the time the issue queue could have refused.
//
// ---------------------------------------------------------------- operands
//
// The issue queue has exactly two operand slots per uop, and the ISA's second
// ALU operand is not always a register. The fold is done here, at the one place
// that knows both the decode and the immediate, and it follows
// mosaic_bringup_core.sv -- the ISA reference whose case passes:
//
//   * `alu_b = uses_rs2 ? rs2_val : imm`, so an instruction that does not use
//     rs2 gets its immediate in the second slot as a ready constant;
//   * `alu_a = is_auipc ? pc : (uses_rs1 ? rs1_val : 0)`, so AUIPC's first
//     operand is the macro's own PC, also folded here as a ready constant;
//   * a source the instruction does not use is addressed as x0, which rename
//     reports ready with value zero.
//
// A source whose producer has already written (`rsN_ready`) has its final value
// in the PRF, and the value is read there **at insert time**, not at allocation:
//
//   * the readiness of a captured (tag, generation) is re-decided every cycle
//     through the writeback arbiter's ready table, so a source that becomes
//     durable while the macro waits for issue-queue space is inserted ready;
//   * a source that becomes durable in exactly the insert cycle is covered by
//     the issue queue's *same-cycle* wakeup, which it applies to the entry it
//     is taking in that cycle (verified in mosaic_iq.sv: `grant_a = wu_hit1 ?
//     wu_val : ins_src1_val`, and the insert path matches the broadcast), so
//     the entry never arrives not-ready for a wakeup that already happened;
//   * a source that becomes durable later is covered by the broadcast.
//
// Latching readiness at allocation instead (`MOSAIC_DISPATCH_MUTANT_LATCH_READY`)
// loses the middle case: the macro is inserted not-ready after the broadcast it
// needed has already gone, and it waits forever.
//
// ------------------------------------------------------------- back-pressure
//
// If the target issue queue cannot take the macro, it stays in the queue and
// the same head is re-offered next cycle. The ROB entry and the rename
// allocation are not rolled back, so a macro waiting to insert can delay the
// retirement of *itself and everything younger* -- never of anything older.
// Depth 4 cannot deadlock: insertion is strictly in allocation order, so a
// consumer is never in an issue queue before its producer. Every source of a
// not-ready entry therefore names a producer that is either already inserted or
// already written, and the induction bottoms out at the oldest queued macro,
// whose sources are by construction already durable. A full dispatch queue
// stalls allocation; it cannot stall the drain of work that is already in the
// machine.
//
// -------------------------------------------------------------- the barrier
//
// `barrier` (driven by the core) stops allocation while a branch is unresolved.
// It is the conservative recovery this package ships; see mosaic_core.sv.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
`include "mosaic_pkg.sv"
`include "mosaic_uop_pkg.sv"

localparam int unsigned DSP_XLEN   = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned DSP_TAG_W  = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
localparam int unsigned DSP_PGEN_W = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;
localparam int unsigned DSP_IGEN_W = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned DSP_IDX_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned DSP_RGEN_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned DSP_UOP_W  = 3;
localparam int unsigned DSP_UOP_ID_W = DSP_IDX_W + DSP_RGEN_W + DSP_UOP_W;
localparam int unsigned DSP_BANKS  = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;
// Depth 4: one allocation in flight plus the insert latencies of two clusters
// and the few cycles a bank conflict on the operand read can add. The argument
// that the depth cannot deadlock is in the header.
localparam int unsigned DSP_DEPTH  = 4;
localparam int unsigned DSP_CNT_W  = $clog2(DSP_DEPTH + 1);
localparam int unsigned DSP_QW     = (DSP_DEPTH <= 1) ? 1 : $clog2(DSP_DEPTH);

// The two-wide input interface is kept and the CSR/memory control bits of
// `decode_ctl_t` are not read here: this package services ALU, branch and
// MUL/DIV macros only, and the second lane cannot be allocated until
// mosaic_rob offers a second allocation port (see the header). Both are stated
// rather than silently narrowed, so the day the ROB widens, the interface is
// already there.
/* verilator lint_off UNUSEDSIGNAL */
module mosaic_dispatch (
    input  logic                        clk,
    input  logic                        rst,

    // ------------------------------------------------------ decoded macros in
    // Lane 0 is the older macro. Lane 1 is offered in the same cycle but is
    // allocated on a later cycle, because the ROB has one allocation port.
    input  logic [1:0]                  dec_valid,
    input  mosaic_pkg::decode_ctl_t     dec_ctl0,
    input  mosaic_pkg::decode_ctl_t     dec_ctl1,
    input  logic [DSP_XLEN-1:0]         dec_pc0,
    input  logic [DSP_XLEN-1:0]         dec_pc1,

    // ----------------------------------------------------- rename allocation
    output logic                        alloc_req,
    output logic [4:0]                  alloc_rd,
    input  logic                        alloc_accepted,
    input  logic                        alloc_exhausted,
    input  logic                        alloc_squashed,
    input  logic                        alloc_is_x0,
    input  logic                        alloc_new_valid,
    input  logic [DSP_TAG_W-1:0]        alloc_new_tag,
    input  logic [DSP_IGEN_W-1:0]       alloc_new_gen,

    // -------------------------------------------------- rename source reads
    output logic [4:0]                  rs1_addr,
    output logic [4:0]                  rs2_addr,
    input  logic                        rs1_is_x0,
    input  logic                        rs2_is_x0,
    input  logic [DSP_TAG_W-1:0]        rs1_tag,
    input  logic [DSP_IGEN_W-1:0]       rs1_gen,
    input  logic [DSP_TAG_W-1:0]        rs2_tag,
    input  logic [DSP_IGEN_W-1:0]       rs2_gen,

    // ---------------------------------------------------------- ROB allocate
    // "the ROB has room for one more entry". A single bit, not the occupancy
    // count: the request is gated on it, and the ROB's own `alloc_ok` is the
    // answer that matters.
    input  logic                        rob_free_any,
    output logic                        rob_alloc_valid,
    output logic [DSP_TAG_W-1:0]        rob_alloc_tag,
    output logic [DSP_XLEN-1:0]         rob_alloc_pc,
    output logic [3:0]                  rob_alloc_num_uops,
    output logic                        rob_alloc_exc,
    output logic                        rob_alloc_open,
    input  logic                        rob_alloc_ok,
    input  logic                        rob_alloc_refused,
    input  logic [DSP_IDX_W-1:0]        rob_alloc_index,
    input  logic [DSP_RGEN_W-1:0]       rob_alloc_gen,

    // ----------------------------------------------------- descriptor store
    output logic                        desc_wr_valid,
    output logic [DSP_IDX_W-1:0]        desc_wr_index,
    output logic [DSP_TAG_W-1:0]        desc_wr_tag,
    output logic [DSP_PGEN_W-1:0]       desc_wr_gen,
    output logic [4:0]                  desc_wr_rd,
    output logic                        desc_wr_reg_we,

    // ------------------------------------------- ready table query (arbiter)
    output logic [1:0]                  rq_valid,
    output logic [1:0][DSP_TAG_W-1:0]   rq_tag,
    output logic [1:0][DSP_IGEN_W-1:0]  rq_gen,
    input  logic [1:0]                  rq_written,

    // ------------------------------------------------- PRF operand read ports
    output logic [DSP_BANKS-1:0]             prf_rd_valid,
    output logic [DSP_BANKS*DSP_TAG_W-1:0]   prf_rd_tag,
    output logic [DSP_BANKS*DSP_PGEN_W-1:0]  prf_rd_gen,
    input  logic [DSP_BANKS-1:0]             prf_rsp_valid,
    input  logic [DSP_BANKS-1:0]             prf_rsp_gen_mismatch,
    input  logic [DSP_BANKS-1:0]             prf_rsp_never_written,
    input  logic [DSP_BANKS*DSP_XLEN-1:0]    prf_rsp_data,

    // ------------------------------------------- cluster insert (one per cluster)
    output logic                        c0_ins_valid,
    input  logic                        c0_ins_ready,
    output logic [DSP_UOP_ID_W-1:0]     c0_ins_uop,
    output mosaic_uop_pkg::uop_meta_t   c0_ins_meta,
    output logic [DSP_XLEN-1:0]         c0_ins_imm,
    output logic [DSP_TAG_W-1:0]        c0_ins_src1_tag,
    output logic [DSP_IGEN_W-1:0]       c0_ins_src1_gen,
    output logic                        c0_ins_src1_ready,
    output logic [DSP_XLEN-1:0]         c0_ins_src1_val,
    output logic [DSP_TAG_W-1:0]        c0_ins_src2_tag,
    output logic [DSP_IGEN_W-1:0]       c0_ins_src2_gen,
    output logic                        c0_ins_src2_ready,
    output logic [DSP_XLEN-1:0]         c0_ins_src2_val,
    output logic [DSP_TAG_W-1:0]        c0_ins_dst_tag,
    output logic [DSP_IGEN_W-1:0]       c0_ins_dst_gen,

    output logic                        c1_ins_valid,
    input  logic                        c1_ins_ready,
    output logic [DSP_UOP_ID_W-1:0]     c1_ins_uop,
    output mosaic_uop_pkg::uop_meta_t   c1_ins_meta,
    output logic [DSP_XLEN-1:0]         c1_ins_imm,
    output logic [DSP_TAG_W-1:0]        c1_ins_src1_tag,
    output logic [DSP_IGEN_W-1:0]       c1_ins_src1_gen,
    output logic                        c1_ins_src1_ready,
    output logic [DSP_XLEN-1:0]         c1_ins_src1_val,
    output logic [DSP_TAG_W-1:0]        c1_ins_src2_tag,
    output logic [DSP_IGEN_W-1:0]       c1_ins_src2_gen,
    output logic                        c1_ins_src2_ready,
    output logic [DSP_XLEN-1:0]         c1_ins_src2_val,
    output logic [DSP_TAG_W-1:0]        c1_ins_dst_tag,
    output logic [DSP_IGEN_W-1:0]       c1_ins_dst_gen,

    // ------------------------------------------------------------ control
    input  logic                        recovering,
    input  logic                        barrier,
    output logic                        stop,           // unsupported macro seen
    // The lane-0 macro left the input this cycle: it was allocated, or it was
    // refused as unsupported and the machine is stopping at it. The decode
    // buffer pops on this, so a refused macro is consumed exactly once.
    output logic                        o_take,

    // ------------------------------------------------------------ counters
    output logic [31:0]                 o_alloc_ctr,
    output logic [31:0]                 o_ins_ctr,
    output logic [31:0]                 o_unsupported_ctr,
    output logic [31:0]                 o_illegal_ctr,
    output logic [31:0]                 o_exhausted_ctr,
    output logic [31:0]                 o_squashed_ctr,
    output logic [31:0]                 o_stall_ctr,
    output logic [31:0]                 o_src_read_ctr,
    output logic [31:0]                 o_src_conflict_ctr,
    output logic [31:0]                 o_src_bad_ctr,
    output logic [31:0]                 o_rob_full_ctr,
    output logic [31:0]                 o_queue_stall_ctr,
    output logic [DSP_CNT_W-1:0]        o_queue_cnt,
    output logic                        o_queue_full
);
/* verilator lint_on UNUSEDSIGNAL */

  // --------------------------------------------------------------------------
  // Declarations. Every internal signal is declared here, before any block that
  // reads it: SystemVerilog requires declaration before use in compilation
  // order, and which of the two project linters rejects the reverse order is
  // not something to rely on.
  // --------------------------------------------------------------------------
  typedef struct packed {
    logic [DSP_UOP_ID_W-1:0] id;
    mosaic_uop_pkg::uop_meta_t meta;
    logic [DSP_XLEN-1:0]     imm;
    logic                    cluster;
    logic [DSP_TAG_W-1:0]    dst_tag;
    logic [DSP_IGEN_W-1:0]   dst_gen;
    logic                    dst_x0;
    logic [DSP_TAG_W-1:0]    s1_tag;
    logic [DSP_IGEN_W-1:0]   s1_gen;
    logic                    s1_x0;
    logic                    s1_const;   // value supplied by dispatch (AUIPC's PC)
    logic [DSP_XLEN-1:0]     s1_cval;
    logic [DSP_TAG_W-1:0]    s2_tag;
    logic [DSP_IGEN_W-1:0]   s2_gen;
    logic                    s2_x0;
    logic                    s2_const;   // value supplied by dispatch (the immediate)
    logic [DSP_XLEN-1:0]     s2_cval;
  } disp_ent_t;

  disp_ent_t            q_mem [0:DSP_DEPTH-1];
  logic [DSP_CNT_W-1:0] q_cnt;
  disp_ent_t            head;
  logic                 head_valid;
  logic [DSP_CNT_W-1:0] q_cnt_next;
  logic [DSP_QW-1:0]    push_at;
  logic                 queue_has_room;
  logic                 head_fire;
  logic                 l0_unsupported;
  logic                 l0_illegal;
  logic                 alloc_now;
  logic                 alloc_ok;
  logic                 stop_q;
  logic                 aff_toggle;
  logic                 target_cluster;
  logic [DSP_TAG_W-1:0] rs1_tag_v, rs2_tag_v;
  logic [DSP_IGEN_W-1:0] rs1_gen_v, rs2_gen_v;
  logic                 s1_needs_read, s2_needs_read;
  logic [1:0][1:0]      bank_of_src;
  logic                 s1_present, s2_present;
  logic                 s1_value_ok, s2_value_ok, s1_conflict, s2_conflict;
  logic                 s1_bad, s2_bad;
  logic                 ins_ready_sel;
  logic                 ins_ok;
  logic [DSP_XLEN-1:0]  s1_val_sel, s2_val_sel;
  logic [DSP_UOP_ID_W-1:0] ins_uop_v;
  mosaic_uop_pkg::uop_meta_t ins_meta_v;
  logic [DSP_XLEN-1:0]  ins_imm_v;
  logic [DSP_TAG_W-1:0] ins_dst_tag_v;
  logic [DSP_IGEN_W-1:0] ins_dst_gen_v;
  mosaic_uop_pkg::uop_meta_t new_meta;

  logic [31:0] alloc_ctr, ins_ctr, unsup_ctr, illegal_ctr, exhausted_ctr, squashed_ctr;
  logic [31:0] stall_ctr, src_read_ctr, src_conflict_ctr, src_bad_ctr;
  logic [31:0] rob_full_ctr, queue_stall_ctr;

  // --------------------------------------------------------------------------
  // Classification and the unsupported refusal
  // --------------------------------------------------------------------------
  assign stop = stop_q;
  assign o_take = alloc_ok || l0_unsupported;

  always_comb begin
    l0_illegal     = dec_valid[0] && !dec_ctl0.valid;
    l0_unsupported = dec_valid[0] && (!dec_ctl0.valid ||
                                      (dec_ctl0.mem_kind != mosaic_pkg::MEM_NONE) ||
                                      dec_ctl0.is_system || dec_ctl0.is_miscmem);
  end

  // --------------------------------------------------------------------------
  // Allocation
  // --------------------------------------------------------------------------
  assign queue_has_room = (q_cnt < DSP_CNT_W'(DSP_DEPTH));
  assign alloc_now      = dec_valid[0] && !l0_unsupported && !recovering && !stop_q &&
                          !barrier && queue_has_room && rob_free_any;
  assign alloc_ok       = alloc_now && alloc_accepted && rob_alloc_ok;

  assign alloc_req = alloc_now;
  assign alloc_rd  = dec_ctl0.rd;

  assign rob_alloc_valid    = alloc_now && alloc_accepted;
  assign rob_alloc_tag      = alloc_is_x0 ? {DSP_TAG_W{1'b0}} : alloc_new_tag;
  assign rob_alloc_pc       = dec_pc0;
  assign rob_alloc_num_uops = 4'd1;
  assign rob_alloc_exc      = 1'b0;
  assign rob_alloc_open     = 1'b0;

  // The descriptor store carries the identity of the physical destination, so
  // `reg_we` here means "this macro owns a physical register". An x0 write is
  // architecturally a write but owns nothing, and the flush path keys on this
  // bit, so an x0 write must not be able to release tag 0.
  assign desc_wr_valid  = alloc_ok;
  assign desc_wr_index  = rob_alloc_index;
  assign desc_wr_tag    = alloc_is_x0 ? {DSP_TAG_W{1'b0}} : alloc_new_tag;
  assign desc_wr_gen    = {{(DSP_PGEN_W - DSP_IGEN_W){1'b0}}, alloc_new_gen};
  assign desc_wr_rd     = dec_ctl0.rd;
  assign desc_wr_reg_we = alloc_ok && alloc_new_valid && dec_ctl0.reg_write && !alloc_is_x0;

  // --------------------------------------------------------------------------
  // The meta
  // --------------------------------------------------------------------------
  always_comb begin
    new_meta.class_ = mosaic_uop_pkg::UOP_ALU;
    if (dec_ctl0.is_muldiv) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_MULDIV;
    end else if (dec_ctl0.mem_kind == mosaic_pkg::MEM_LOAD) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_LOAD;
    end else if (dec_ctl0.mem_kind == mosaic_pkg::MEM_STORE) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_STORE;
    end else if (dec_ctl0.is_branch || dec_ctl0.is_jal || dec_ctl0.is_jalr) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_BRANCH;
    end else if (dec_ctl0.is_system || dec_ctl0.is_miscmem) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_SYSTEM;
    end
    new_meta.pc          = dec_pc0;
    new_meta.alu_op      = dec_ctl0.alu_op;
    new_meta.md_op       = dec_ctl0.md_op;
    new_meta.md_w        = dec_ctl0.md_w;
    new_meta.br_funct    = dec_ctl0.branch_funct;
    new_meta.is_jal      = dec_ctl0.is_jal;
    new_meta.is_jalr     = dec_ctl0.is_jalr;
    new_meta.writes_link = dec_ctl0.writes_link;
    new_meta.mem_size    = dec_ctl0.mem_size;
    new_meta.mem_signed  = dec_ctl0.mem_signed;
    new_meta.is_fence    = dec_ctl0.is_miscmem && !dec_ctl0.is_fence_i;
    new_meta.is_fence_i  = dec_ctl0.is_fence_i;
  end

  // --------------------------------------------------------------------------
  // Operands at allocation
  // --------------------------------------------------------------------------
  // A source the instruction does not use is addressed as x0, so rename reports
  // it ready with value zero and the issue queue sees exactly one readiness
  // rule.
  assign rs1_addr = dec_ctl0.uses_rs1 ? dec_ctl0.rs1 : 5'd0;
  assign rs2_addr = dec_ctl0.uses_rs2 ? dec_ctl0.rs2 : 5'd0;

  assign rs1_tag_v = rs1_tag;
  assign rs2_tag_v = rs2_tag;
  assign rs1_gen_v = rs1_gen;
  assign rs2_gen_v = rs2_gen;

  // ------------------------------------------------------- cluster affinity
  // Fixed, deterministic, and stated rather than emergent: the first macro of a
  // fetched pair goes to cluster 0 and the second to cluster 1, and a MUL/DIV
  // macro always goes to cluster 0's queue because that queue's grant is the
  // one routed to the shared unit.
  always_comb begin
    target_cluster = dec_ctl0.is_muldiv ? 1'b0 : aff_toggle;
  end

  // --------------------------------------------------------------------------
  // Queue next state
  // --------------------------------------------------------------------------
  assign push_at = DSP_QW'(head_fire ? (q_cnt - DSP_CNT_W'(1)) : q_cnt);

  always_comb begin
    q_cnt_next = q_cnt;
    if (head_fire) q_cnt_next = q_cnt_next - DSP_CNT_W'(1);
    if (alloc_ok)  q_cnt_next = q_cnt_next + DSP_CNT_W'(1);
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      q_cnt <= {DSP_CNT_W{1'b0}};
      for (int unsigned i = 0; i < DSP_DEPTH; i++) begin
        q_mem[i].dst_x0  <= 1'b0;
        q_mem[i].s1_x0   <= 1'b0;
        q_mem[i].s1_const<= 1'b0;
        q_mem[i].s2_x0   <= 1'b0;
        q_mem[i].s2_const<= 1'b0;
      end
    end else begin
      q_cnt <= q_cnt_next;
      // Shift down on a pop, then place the new entry at the tail.
      if (head_fire) begin
        for (int unsigned i = 0; i < DSP_DEPTH-1; i++) begin
          q_mem[i] <= q_mem[i+1];
        end
      end
      if (alloc_ok) begin
        q_mem[push_at].id       <= {rob_alloc_index, rob_alloc_gen, {DSP_UOP_W{1'b0}}};
        q_mem[push_at].meta     <= new_meta;
        q_mem[push_at].imm      <= dec_ctl0.imm;
        q_mem[push_at].cluster  <= target_cluster;
        q_mem[push_at].dst_tag  <= alloc_is_x0 ? {DSP_TAG_W{1'b0}} : alloc_new_tag;
        q_mem[push_at].dst_gen  <= alloc_new_gen;
        q_mem[push_at].dst_x0   <= alloc_is_x0;
        q_mem[push_at].s1_tag   <= rs1_is_x0 ? {DSP_TAG_W{1'b0}} : rs1_tag_v;
        q_mem[push_at].s1_gen   <= rs1_is_x0 ? {DSP_IGEN_W{1'b0}} : rs1_gen_v;
        q_mem[push_at].s1_x0    <= rs1_is_x0;
        q_mem[push_at].s1_const <= dec_ctl0.is_auipc;
        q_mem[push_at].s1_cval  <= dec_pc0;
        q_mem[push_at].s2_tag   <= rs2_is_x0 ? {DSP_TAG_W{1'b0}} : rs2_tag_v;
        q_mem[push_at].s2_gen   <= rs2_is_x0 ? {DSP_IGEN_W{1'b0}} : rs2_gen_v;
        q_mem[push_at].s2_x0    <= rs2_is_x0;
        // `alu_b = uses_rs2 ? rs2_val : imm`, as mosaic_bringup_core.sv states
        // it for the ISA reference.
        q_mem[push_at].s2_const <= !dec_ctl0.uses_rs2;
        q_mem[push_at].s2_cval  <= dec_ctl0.imm;
      end
    end
  end

  assign head_valid = (q_cnt != {DSP_CNT_W{1'b0}});
  assign head       = q_mem[0];

  // --------------------------------------------------------------------------
  // Insert-time readiness
  // --------------------------------------------------------------------------
  assign s1_needs_read = head_valid && !head.s1_x0 && !head.s1_const && rq_written[0];
  assign s2_needs_read = head_valid && !head.s2_x0 && !head.s2_const && rq_written[1];

  assign rq_valid[0] = head_valid && !head.s1_x0 && !head.s1_const;
  assign rq_tag[0]   = head.s1_tag;
  assign rq_gen[0]   = head.s1_gen;
  assign rq_valid[1] = head_valid && !head.s2_x0 && !head.s2_const;
  assign rq_tag[1]   = head.s2_tag;
  assign rq_gen[1]   = head.s2_gen;

  always_comb begin
    bank_of_src[0] = 2'(32'(head.s1_tag) % 32'(DSP_BANKS));
    bank_of_src[1] = 2'(32'(head.s2_tag) % 32'(DSP_BANKS));
  end

  always_comb begin
    s1_present = s1_needs_read;
    // Slot 0 wins a bank conflict; the second source is re-offered next cycle.
    s2_present = s2_needs_read && (!s1_present || (bank_of_src[1] != bank_of_src[0]));
  end

  always_comb begin
    prf_rd_valid = {DSP_BANKS{1'b0}};
    prf_rd_tag   = {(DSP_BANKS*DSP_TAG_W){1'b0}};
    prf_rd_gen   = {(DSP_BANKS*DSP_PGEN_W){1'b0}};
    if (s1_present) begin
      prf_rd_valid[bank_of_src[0]] = 1'b1;
      prf_rd_tag[bank_of_src[0]*DSP_TAG_W +: DSP_TAG_W] = head.s1_tag;
      prf_rd_gen[bank_of_src[0]*DSP_PGEN_W +: DSP_PGEN_W] =
          {{(DSP_PGEN_W - DSP_IGEN_W){1'b0}}, head.s1_gen};
    end
    if (s2_present) begin
      prf_rd_valid[bank_of_src[1]] = 1'b1;
      prf_rd_tag[bank_of_src[1]*DSP_TAG_W +: DSP_TAG_W] = head.s2_tag;
      prf_rd_gen[bank_of_src[1]*DSP_PGEN_W +: DSP_PGEN_W] =
          {{(DSP_PGEN_W - DSP_IGEN_W){1'b0}}, head.s2_gen};
    end
  end

  always_comb begin
    s1_value_ok = head.s1_x0 || head.s1_const || !rq_written[0] ||
                  (prf_rsp_valid[bank_of_src[0]] &&
                   !prf_rsp_gen_mismatch[bank_of_src[0]] &&
                   !prf_rsp_never_written[bank_of_src[0]]);
    s2_value_ok = head.s2_x0 || head.s2_const || !rq_written[1] ||
                  (prf_rsp_valid[bank_of_src[1]] &&
                   !prf_rsp_gen_mismatch[bank_of_src[1]] &&
                   !prf_rsp_never_written[bank_of_src[1]]);
    // A source the table calls written but whose PRF response is refused or
    // wrong is an internal inconsistency: the write that set the table also
    // wrote the register file, so it cannot happen. It is counted and the
    // insert stalls rather than being papered over with a wrong value.
    s1_conflict = s1_needs_read && !s1_present;
    s2_conflict = s2_needs_read && !s2_present;
    s1_bad      = s1_needs_read && s1_present &&
                  (!prf_rsp_valid[bank_of_src[0]] ||
                   prf_rsp_gen_mismatch[bank_of_src[0]] ||
                   prf_rsp_never_written[bank_of_src[0]]);
    s2_bad      = s2_needs_read && s2_present &&
                  (!prf_rsp_valid[bank_of_src[1]] ||
                   prf_rsp_gen_mismatch[bank_of_src[1]] ||
                   prf_rsp_never_written[bank_of_src[1]]);
  end

  always_comb begin
    ins_ready_sel = head.cluster ? c1_ins_ready : c0_ins_ready;
  end

  assign ins_ok    = head_valid && !recovering && s1_value_ok && s2_value_ok &&
                     ins_ready_sel;
  assign head_fire = ins_ok;

  // --------------------------------------------------------------------------
  // Insert bus
  // --------------------------------------------------------------------------
  always_comb begin
    s1_val_sel = prf_rsp_data[bank_of_src[0]*DSP_XLEN +: DSP_XLEN];
    s2_val_sel = prf_rsp_data[bank_of_src[1]*DSP_XLEN +: DSP_XLEN];
  end

  always_comb begin
    ins_uop_v     = head.id;
    ins_meta_v    = head.meta;
    ins_imm_v     = head.imm;
    // The issue queue's destination tag 0 means "no physical register" -- tag 0
    // is x0's committed mapping and rename never allocates it -- which is the
    // same convention the cluster uses to flag the completion x0.
    ins_dst_tag_v = head.dst_x0 ? {DSP_TAG_W{1'b0}} : head.dst_tag;
    ins_dst_gen_v = head.dst_x0 ? {DSP_IGEN_W{1'b0}} : head.dst_gen;
  end

  always_comb begin
    c0_ins_valid     = ins_ok && !head.cluster;
    c0_ins_uop       = ins_uop_v;
    c0_ins_meta      = ins_meta_v;
    c0_ins_imm       = ins_imm_v;
    c0_ins_src1_tag  = head.s1_tag;
    c0_ins_src1_gen  = head.s1_gen;
    c0_ins_src1_ready = head.s1_x0 || head.s1_const || rq_written[0];
    c0_ins_src1_val  = head.s1_const ? head.s1_cval
                                     : (head.s1_x0 ? {DSP_XLEN{1'b0}} : s1_val_sel);
    c0_ins_src2_tag  = head.s2_tag;
    c0_ins_src2_gen  = head.s2_gen;
    c0_ins_src2_ready = head.s2_x0 || head.s2_const || rq_written[1];
    c0_ins_src2_val  = head.s2_const ? head.s2_cval
                                     : (head.s2_x0 ? {DSP_XLEN{1'b0}} : s2_val_sel);
    c0_ins_dst_tag   = ins_dst_tag_v;
    c0_ins_dst_gen   = ins_dst_gen_v;

    c1_ins_valid     = ins_ok && head.cluster;
    c1_ins_uop       = ins_uop_v;
    c1_ins_meta      = ins_meta_v;
    c1_ins_imm       = ins_imm_v;
    c1_ins_src1_tag  = head.s1_tag;
    c1_ins_src1_gen  = head.s1_gen;
    c1_ins_src1_ready = head.s1_x0 || head.s1_const || rq_written[0];
    c1_ins_src1_val  = head.s1_const ? head.s1_cval
                                     : (head.s1_x0 ? {DSP_XLEN{1'b0}} : s1_val_sel);
    c1_ins_src2_tag  = head.s2_tag;
    c1_ins_src2_gen  = head.s2_gen;
    c1_ins_src2_ready = head.s2_x0 || head.s2_const || rq_written[1];
    c1_ins_src2_val  = head.s2_const ? head.s2_cval
                                     : (head.s2_x0 ? {DSP_XLEN{1'b0}} : s2_val_sel);
    c1_ins_dst_tag   = ins_dst_tag_v;
    c1_ins_dst_gen   = ins_dst_gen_v;
  end

  // --------------------------------------------------------------------------
  // Affinity toggle and counters
  // --------------------------------------------------------------------------
  always_ff @(posedge clk) begin
    if (rst) begin
      aff_toggle <= 1'b0;
    end else if (alloc_ok) begin
`ifdef MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER
      // NEGATIVE CONTROL: the affinity never alternates, so every macro is
      // inserted into cluster 0's queue and the second cluster is never used.
      // CASE=fabric.fixed_two_cluster's fabric controls -- cluster 1 executed
      // at least one ALU uop, and the two clusters executed disjoint uops
      // covering the program -- must fail under it. It is what proves those
      // controls measure the *second cluster* and not merely that the machine
      // executes instructions at all. Built with
      // -DMOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER and recorded in
      // results/reports/I-023-core.md.
      aff_toggle <= 1'b0;
`else
      aff_toggle <= ~aff_toggle;
`endif
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      alloc_ctr        <= 32'd0;
      ins_ctr          <= 32'd0;
      unsup_ctr        <= 32'd0;
      illegal_ctr      <= 32'd0;
      exhausted_ctr    <= 32'd0;
      squashed_ctr     <= 32'd0;
      stall_ctr        <= 32'd0;
      src_read_ctr     <= 32'd0;
      src_conflict_ctr <= 32'd0;
      src_bad_ctr      <= 32'd0;
      rob_full_ctr     <= 32'd0;
      queue_stall_ctr  <= 32'd0;
      stop_q           <= 1'b0;
    end else begin
      if (alloc_ok) alloc_ctr <= alloc_ctr + 32'd1;
      if (head_fire) ins_ctr <= ins_ctr + 32'd1;
      if (l0_unsupported) begin
        unsup_ctr <= unsup_ctr + 32'd1;
        stop_q    <= 1'b1;
      end
      if (l0_illegal) illegal_ctr <= illegal_ctr + 32'd1;
      if (alloc_now && alloc_exhausted) exhausted_ctr <= exhausted_ctr + 32'd1;
      if (alloc_now && alloc_squashed)  squashed_ctr  <= squashed_ctr + 32'd1;
      if (alloc_now && alloc_accepted && rob_alloc_refused)
        exhausted_ctr <= exhausted_ctr + 32'd1;
      if (dec_valid[0] && !l0_unsupported && !recovering && !stop_q && !barrier &&
          !rob_free_any) rob_full_ctr <= rob_full_ctr + 32'd1;
      if (head_valid && !recovering && !ins_ok) stall_ctr <= stall_ctr + 32'd1;
      if (s1_needs_read || s2_needs_read) src_read_ctr <= src_read_ctr + 32'd1;
      if (s1_conflict || s2_conflict) src_conflict_ctr <= src_conflict_ctr + 32'd1;
      if (s1_bad || s2_bad) src_bad_ctr <= src_bad_ctr + 32'd1;
      if (!queue_has_room && dec_valid[0] && !l0_unsupported && !stop_q && !barrier)
        queue_stall_ctr <= queue_stall_ctr + 32'd1;
    end
  end

  assign o_alloc_ctr       = alloc_ctr;
  assign o_ins_ctr         = ins_ctr;
  assign o_unsupported_ctr = unsup_ctr;
  assign o_illegal_ctr     = illegal_ctr;
  assign o_exhausted_ctr   = exhausted_ctr;
  assign o_squashed_ctr    = squashed_ctr;
  assign o_stall_ctr       = stall_ctr;
  assign o_src_read_ctr    = src_read_ctr;
  assign o_src_conflict_ctr= src_conflict_ctr;
  assign o_src_bad_ctr     = src_bad_ctr;
  assign o_rob_full_ctr    = rob_full_ctr;
  assign o_queue_stall_ctr = queue_stall_ctr;
  assign o_queue_cnt       = q_cnt;
  assign o_queue_full      = !queue_has_room;

endmodule : mosaic_dispatch

`default_nettype wire
