// ============================================================================
// mosaic_core -- the p0 out-of-order core (work package I-023).
//
// Fetch -> 2-wide decode buffer -> dispatch (one macro per cycle, see
// mosaic_dispatch.sv) -> two clusters -> writeback arbiter -> ROB -> retire ->
// commit to rename, with the redirect arbiter on the branch-resolution side.
//
// ------------------------------------------------------------ the data path
//
//   rename       allocation (free list, generations, speculative map) and the
//                source-read ports; commit installs the committed map and
//                releases the superseded mapping
//   rob          one entry per instruction, in-order retirement, the head view
//   macro_desc   the retire-only fields the ROB does not carry
//   clusters     two issue queues, two ALUs, two branch resolvers; cluster 0's
//                queue also routes to the shared MUL/DIV unit
//   wb_arbiter   the completion path: PRF writes, value-visible wakeup, rename
//                writeback, ROB completion, ready table, durable value stash
//   redirect_arb oldest wins, and only at the ROB head (see its header)
//
// --------------------------------------------------------------- recovery
//
// This package ships the **conservative recovery**: a branch is a barrier. From
// the cycle a branch is allocated until that branch has been resolved -- and, if
// it redirected, until the redirect has been applied -- dispatch allocates
// nothing behind it. So when a mispredicting branch redirects, there is nothing
// younger than it anywhere in the machine:
//
//   * the redirect arbiter only lets the branch act in the cycle it is the ROB
//     head and is being retired, so everything older has already committed;
//   * nothing younger was ever allocated, so the ROB flush discards only stale
//     slots, and the out-of-order machinery behind the branch is empty;
//   * rename's speculative map already equals its committed map, so **no squash
//     is needed and none is issued** (`ckpt_valid` and `squash` are tied low).
//     That matters: `mosaic_rename` now refuses a squash to a checkpoint that
//     was not taken at a committed boundary, and its undo journal is exact only
//     when no post-checkpoint allocation has committed -- a condition a machine
//     that retires inside the window cannot meet in general. Recovery here does
//     not depend on either. I-018's controller replaces this with a saved
//     speculative map, which removes the barrier and the drain entirely.
//
// The cost is frontend serialisation at every branch: a branch holds allocation
// until it resolves. That is the deliberate price of a recovery that cannot
// corrupt the map or the free list, and it is what the plan permits as the
// initial method (correctness before the performance pass).
//
// A pending redirect never holds retirement: the arbiter waits for the branch to
// reach the head, and older work keeps retiring until it does. Recovery holds
// dispatch and the frontend only while the clusters purge.
//
// ------------------------------------------------------- what is not here
//
// Loads, stores, fences, CSRs, traps, interrupts and the memory path are not
// part of this package; dispatch refuses those macros and stops the machine
// (see mosaic_dispatch.sv). The data port is brought out and driven to a
// never-requesting value so the SoC interface exists, but it is not exercised.
//
// The instruction-side port carries the fetch unit's request id and epoch
// unchanged and the fetch unit already refuses a response from a retired epoch
// (`rsp_live` in mosaic_fetch.sv requires the slot to own the response, the
// slot not to be cancelled, no redirect in the cycle, and the epoch to match),
// so a stale response cannot reach the decoder -- confirmed in the source, not
// assumed.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */

// The data port's ready/response inputs are unread: no macro this package
// dispatches issues a data request (loads and stores are refused by dispatch),
// so the port is brought out complete and left quiescent for I-033..I-038. The
// suppression is scoped to the port declaration below and is stated here rather
// than hidden by wiring the inputs to something they do not mean.
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_pkg.sv"
`include "mosaic_uop_pkg.sv"

localparam int unsigned CORE_XLEN    = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned CORE_TAG_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
localparam int unsigned CORE_PGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;
localparam int unsigned CORE_IGEN_W  = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned CORE_IDX_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned CORE_RGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned CORE_UOP_W   = 3;
localparam int unsigned CORE_UOP_ID_W = CORE_IDX_W + CORE_RGEN_W + CORE_UOP_W;
localparam int unsigned CORE_BANKS   = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;
localparam int unsigned CORE_ROB_N   = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned CORE_FETCH_N = mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING;
localparam int unsigned CORE_REQ_ID_W = (CORE_FETCH_N <= 1) ? 1 : $clog2(CORE_FETCH_N);
localparam int unsigned CORE_EPOCH_W = (CORE_ROB_N <= 1) ? 2 : $clog2(CORE_ROB_N) + 1;
localparam int unsigned CORE_RET_N   = mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH;
localparam int unsigned CORE_RET_ID_W = 2 * CORE_TAG_W;
localparam int unsigned CORE_SEQ_W   = $clog2(2 * CORE_ROB_N + 1);
localparam int unsigned CORE_RD_W    = 5;
localparam int unsigned CORE_CSR_W   = 12;
localparam int unsigned CORE_SIZE_W  = 3;
localparam int unsigned CORE_OCC_W   = $clog2(CORE_ROB_N + 1);
// Readiness observability: the rename state a stuck operand is diagnosed from,
// sized from the same generated geometry rename uses so the widths cannot drift.
localparam int unsigned CORE_ARCH_N  = mosaic_cfg_pkg::MOSAIC_ARCH_INT_REGS;
localparam int unsigned CORE_PRF_N   = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
localparam int unsigned CORE_MAP_W   = CORE_TAG_W + CORE_IGEN_W;
// The memory path's geometry, from the generated profile like everything else.
localparam int unsigned CORE_LQ_N    = mosaic_cfg_pkg::MOSAIC_LQ_ENTRIES;
localparam int unsigned CORE_SQ_N    = mosaic_cfg_pkg::MOSAIC_SQ_ENTRIES;
localparam int unsigned CORE_MEM_CNT_W = $clog2(CORE_LQ_N + 1);   // == clog2(SQ_N+1); p0: 4
// The store queue's exported entry layout, re-expressed here because a wire
// width has to be declared before the instance that drives it. The three fields
// below ARE the layout mosaic_store_queue and mosaic_load_queue both document
// (data_valid, data, size, imm, base, id at those offsets); the case asserts the
// three-way agreement at startup rather than trusting three copies of a formula.
localparam int unsigned CORE_MEM_ID_W  = $bits(mosaic_uop_pkg::uop_id_t);
// Only the pre-commit negative control reads the exported entry view by hand,
// so this offset exists only in that build: an always-present localparam would
// be an unused parameter in the shipping build, which is a lint failure here.
`ifdef MOSAIC_CORE_MUTANT_STORE_PRECOMMIT
localparam int unsigned CORE_SQ_OFF_ID  = 2 + CORE_XLEN + 3 + CORE_XLEN + CORE_XLEN;
`endif
localparam int unsigned CORE_SQ_ENTRY_W =
    2 + CORE_XLEN + 3 + CORE_XLEN + CORE_XLEN + CORE_MEM_ID_W;

module mosaic_core (
    input  logic                        clk,
    input  logic                        rst,

    // ------------------------------------------------- instruction memory port
    output logic                        imem_req_valid,
    output mosaic_uop_pkg::mem_req_t    imem_req,
    input  logic                        imem_req_ready,
    output logic [CORE_REQ_ID_W-1:0]    imem_req_id,
    output logic [CORE_EPOCH_W-1:0]     imem_req_epoch,
    input  logic                        imem_rsp_valid,
    output logic                        imem_rsp_ready,
    input  mosaic_uop_pkg::mem_rsp_t    imem_rsp,
    input  logic [CORE_REQ_ID_W-1:0]    imem_rsp_id,
    input  logic [CORE_EPOCH_W-1:0]     imem_rsp_epoch,
    input  logic [2:0]                  imem_rsp_len,

    // -------------------------------------------------------- data memory port
    output logic                        dmem_req_valid,
    output mosaic_uop_pkg::mem_req_t    dmem_req,
    input  logic                        dmem_req_ready,
    input  logic                        dmem_rsp_valid,
    output logic                        dmem_rsp_ready,
    input  mosaic_uop_pkg::mem_rsp_t    dmem_rsp,

    // ------------------------------------------------------ retire event stream
    output logic [CORE_RET_N-1:0]       ev_valid,
    output logic [CORE_RET_N-1:0]       ev_trap,
    output logic [CORE_RET_N*CORE_SEQ_W-1:0]  ev_seq,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_pc,
    output logic [CORE_RET_N*CORE_RET_ID_W-1:0] ev_id,
    output logic [CORE_RET_N-1:0]       ev_reg_we,
    output logic [CORE_RET_N*CORE_RD_W-1:0]   ev_rd,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_value,
    output logic [CORE_RET_N-1:0]       ev_store,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_store_addr,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_store_data,
    output logic [CORE_RET_N*CORE_SIZE_W-1:0] ev_store_size,

    /* verilator lint_on UNUSEDSIGNAL */

    // ---------------------------------------------- the memory path (I-033..I-038)
    // The integrated LSU's evidence, brought out so CASE=core.mem_program can
    // state what the memory path did rather than infer it from the final memory
    // image: how many accesses were allocated, issued to the endpoint and
    // completed; how many load bytes came from a store and how many from memory;
    // how many stores were authorised, drained and squashed; and how often a
    // memory insert had to wait. They are counters, not control -- nothing in
    // the core reads them.
    output logic [31:0]                 o_mem_lq_alloc,
    output logic [31:0]                 o_mem_lq_issue,
    output logic [31:0]                 o_mem_lq_done,
    output logic [31:0]                 o_mem_lq_replay,
    output logic [31:0]                 o_mem_lq_blocked,
    output logic [31:0]                 o_mem_lq_fwd_bytes,
    output logic [31:0]                 o_mem_lq_mem_bytes,
    output logic [31:0]                 o_mem_lq_fault,
    output logic [31:0]                 o_mem_lq_query_mismatch,
    output logic [31:0]                 o_mem_lq_occupied,
    output logic [31:0]                 o_mem_sq_alloc,
    output logic [31:0]                 o_mem_sq_commit,
    output logic [31:0]                 o_mem_sq_commit2,
    output logic [31:0]                 o_mem_sq_commit_stale,
    output logic [31:0]                 o_mem_sq_drain,
    output logic [31:0]                 o_mem_sq_squash,
    output logic [31:0]                 o_mem_sq_spared,
    output logic [31:0]                 o_mem_sq_occupied,
    output logic [31:0]                 o_mem_sq_auth,
    output logic [31:0]                 o_mem_sq_fault,
    output logic [31:0]                 o_mem_lsu_txn,
    output logic [31:0]                 o_mem_lsu_misaligned,
    output logic [31:0]                 o_mem_lsu_access_fault,
    output logic [31:0]                 o_mem_ins_stall,
    output logic                        o_mem_squash_valid,

    // -------------------------------------------------------------- evidence
    output logic [31:0]                 o_commit_ctr,
    output logic [31:0]                 o_unsupported_ctr,
    output logic [31:0]                 o_illegal_ctr,
    output logic [31:0]                 o_redirect_ctr,
    output logic [31:0]                 o_recovering_ctr,
    output logic [31:0]                 o_stop_ctr,
    output logic                        o_stopped,
    output logic [31:0]                 o_cycle_ctr,
    output logic [31:0]                 o_c0_count,
    output logic [31:0]                 o_c1_count,
    output logic                        o_c0_grant_valid,
    output logic                        o_c1_grant_valid,
    output logic [CORE_UOP_ID_W-1:0]    o_c0_grant_uop,
    output logic [CORE_UOP_ID_W-1:0]    o_c1_grant_uop,
    output logic [31:0]                 o_c0_alu_ctr,
    output logic [31:0]                 o_c1_alu_ctr,
    output logic [31:0]                 o_c0_branch_ctr,
    output logic [31:0]                 o_c1_branch_ctr,
    output logic [31:0]                 o_muldiv_ctr,
    output logic                        o_wb_pub_valid,
    output logic [CORE_IDX_W-1:0]       o_wb_pub_index,
    output logic [CORE_RGEN_W-1:0]      o_wb_pub_gen,
    output logic [CORE_XLEN-1:0]        o_wb_pub_value,
    output logic [31:0]                 o_wb_wr_ctr,
    output logic [31:0]                 o_wb_wake_ctr,
    output logic [31:0]                 o_wb_stale_ctr,
    output logic [31:0]                 o_wb_dup_ctr,
    output logic [31:0]                 o_wb_collision_ctr,
    output logic [31:0]                 o_wb_drop_ctr,
    output logic [CORE_TAG_W:0]         o_free_count,
    output logic [31:0]                 o_squash_not_committed_ctr,
    output logic [31:0]                 o_squash_underflow_ctr,
    output logic [31:0]                 o_journal_overflow_ctr,
    output logic [31:0]                 o_rob_occupied,
    output logic [31:0]                 o_rob_free,
    output logic [31:0]                 o_desc_live,
    output logic                        o_rename_boundary,
    output logic [31:0]                 o_redir_act_ctr,
    output logic [31:0]                 o_redir_wait_ctr,
    output logic [31:0]                 o_redir_dead_ctr,
    // ---------------------------------------------- the redirect, and the squash
    // The redirect pulse and its target, so a case can check that the redirect
    // goes where the resolved branch said instead of inferring it from the
    // retirement stream; and the rename recovery counters, so the checkpoint's
    // precondition and the squash's acceptance are observable rather than
    // asserted in prose.
    output logic                        o_redirect_valid,
    output logic [CORE_XLEN-1:0]        o_redirect_pc,
    // The front end's own PC, so the effect of a redirect on the fetch stream is
    // observable rather than inferred from the retirement stream: the cycle
    // after a redirect this must be the redirect's target.
    output logic [CORE_XLEN-1:0]        o_fetch_pc,
    output logic [31:0]                 o_squash_acc_ctr,
    output logic [31:0]                 o_ckpt_ctr,
    // The redirect arbiter's inputs and decision, so a failing case can say
    // which one of them was wrong instead of only that no redirect came:
    // {61:54 occupied, 53 recovering, 52 barrier, 51 act_taken, 50 act_valid,
    //  49:43 head_gen, 42:37 head_index, 36 head_retire, 35 head_valid,
    //  34:28 c1_gen, 27:22 c1_idx, 21 c1_taken, 20 c1_valid,
    //  19:13 c0_gen, 12:7 c0_idx, 6 c0_taken, 5 c0_valid, 4:0 reserved}
    output logic [63:0]                 o_dbg_redir_bundle,
    // The fetch unit's own response classification and output register, so a
    // failing case can say why an instruction did or did not reach the decoder.
    output logic [127:0]                o_dbg_fetch_state,
    // ------------------------------------------------------- debug observability
    // The instructions and the ROB head, so a failing case can say what the
    // machine was doing instead of only that a count was wrong.
    output logic                        o_dbg_deliver_valid,
    output logic [CORE_XLEN-1:0]        o_dbg_deliver_pc,
    output logic [31:0]                 o_dbg_deliver_bits,
    output logic                        o_dbg_head_valid,
    output logic                        o_dbg_head_complete,
    output logic [CORE_IDX_W-1:0]       o_dbg_head_index,
    output logic [CORE_XLEN-1:0]        o_dbg_head_pc,
    output logic [4:0]                  o_dbg_desc_rd0,
    output logic [4:0]                  o_dbg_desc_rd1,
    output logic [31:0]                 o_dbg_alloc_ctr,
    output logic [31:0]                 o_dbg_ins_ctr,
    // The rename readiness state, so a case that stops making progress can name
    // the mapping that stalled instead of only that nothing retired:
    //   o_dbg_spec_map  one {generation, tag} pair per architectural register,
    //                   entry a at bits a*MAP_W +: MAP_W;
    //   o_dbg_gen_valid one bit per physical tag: "an allocation has taken this
    //                   tag since reset" (clear means the mapping, if any, is the
    //                   architectural initial mapping);
    //   o_dbg_wb_done   one bit per physical tag: "this tag's value has been
    //                   written back".
    // A tag named by a source with gen_valid clear and wb_done clear is exactly
    // the operand no wakeup will ever arrive for.
    output logic [CORE_ARCH_N*CORE_MAP_W-1:0] o_dbg_spec_map,
    output logic [CORE_PRF_N-1:0]       o_dbg_gen_valid,
    output logic [CORE_PRF_N-1:0]       o_dbg_wb_done
);

  // ==========================================================================
  // Interconnect declarations (hoisted: a port connection must see its net)
  // ==========================================================================
  // fetch
  logic [CORE_XLEN-1:0]      fetch_pc_q;
  logic                      fetch_req_fire;
  logic                      want_imem_req;
  logic                      fetch_slot_free;
  logic                      fetch_req_valid_int;
  logic                      fetch_out_valid, fetch_out_ready;
  logic [CORE_XLEN-1:0]      fetch_out_pc;
  logic [31:0]               fetch_out_bits;
  logic                      fetch_out_illegal, fetch_out_fault;
  logic                      fetch_pred_next_valid;
  logic [127:0]              fetch_dbg_state;
  logic [CORE_XLEN-1:0]      fetch_pred_next_pc;

  // control
  logic                      redirect_valid;
  logic [CORE_XLEN-1:0]      redirect_pc;
  logic                      rob_flush_pulse;
  logic                      cluster_flush_pulse;
  logic                      core_stop;
  logic                      recovering;
  logic                      br_inflight;
  logic                      alloc_is_branch_macro;
  logic                      redir_act_valid, redir_act_taken;

  // decode buffer
  mosaic_pkg::decode_ctl_t   dec_ctl_comb;
  mosaic_pkg::decode_ctl_t   dbuf_ctl_new;
  logic                      dbuf_valid [0:1];
  logic [CORE_XLEN-1:0]      dbuf_pc    [0:1];
  mosaic_pkg::decode_ctl_t   dbuf_ctl   [0:1];
  logic [1:0]                dbuf_cnt;
  logic                      dbuf_take, dbuf_push, dbuf_room;
  // The push slot and the buffer's next state. The push lands at the tail
  // *after* this cycle's pop, so it is `dbuf_cnt - dbuf_take`, and the next
  // state is computed per slot rather than by two independent writes to the
  // same one -- see the always_comb in section 2.
  logic                      dbuf_push_at;
  logic [1:0]                dbuf_valid_n;
  logic [CORE_XLEN-1:0]      dbuf_pc_n  [0:1];
  mosaic_pkg::decode_ctl_t   dbuf_ctl_n [0:1];

  // dispatch
  logic [4:0]                alloc_rd_w;
  logic                      disp_take, disp_unsupported;
  logic [1:0]                disp_rq_valid, disp_rq_written;
  logic [1:0][CORE_TAG_W-1:0]  disp_rq_tag;
  logic [1:0][CORE_IGEN_W-1:0] disp_rq_gen;

  // rename
  logic                      ren_alloc_req, ren_alloc_accepted, ren_alloc_exhausted;
  logic                      ren_alloc_squashed, ren_alloc_is_x0, ren_alloc_new_valid;
  logic [CORE_TAG_W-1:0]     ren_new_tag;
  logic [CORE_IGEN_W-1:0]    ren_new_gen;
  logic [4:0]                ren_rs1_addr, ren_rs2_addr;
  logic                      ren_rs1_is_x0, ren_rs2_is_x0;
  // rename's per-tag allocation validity, read out on its `dbg_gen_valid` port.
  // It is the input to dispatch's architectural-initial-mapping fold: a tag whose
  // bit is clear has never been allocated, so a source that names it reads the
  // architectural initial value, zero.
  logic [CORE_PRF_N-1:0]     ren_gen_valid;
  logic [CORE_TAG_W-1:0]     ren_rs1_tag, ren_rs2_tag;
  logic [CORE_IGEN_W-1:0]    ren_rs1_gen, ren_rs2_gen;
  logic                      ren_wb_valid;
  logic [CORE_TAG_W-1:0]     ren_wb_tag;
  logic [CORE_IGEN_W-1:0]    ren_wb_gen;
  logic                      ren_wb_accepted, ren_wb_stale, ren_wb_duplicate;
  logic                      ren_commit_valid;
  logic [4:0]                ren_commit_rd;
  logic [CORE_TAG_W-1:0]     ren_commit_tag;
  logic [CORE_IGEN_W-1:0]    ren_commit_gen;
  logic                      ren_commit2_valid;
  logic [4:0]                ren_commit2_rd;
  logic [CORE_TAG_W-1:0]     ren_commit2_tag;
  logic [CORE_IGEN_W-1:0]    ren_commit2_gen;
  logic                      ren_squash_underflow, ren_journal_overflow;
  logic                      ren_squash_not_committed, ren_ckpt_committed;
  logic                      ren_ckpt_valid, ren_squash, ren_squash_accepted;
  logic                      redirect_delay_q;
  logic [CORE_TAG_W:0]       ren_free_count;

  // descriptor store
  logic                      desc_wr_valid;
  logic [CORE_IDX_W-1:0]     desc_wr_index;
  logic [CORE_TAG_W-1:0]     desc_wr_tag;
  logic [CORE_PGEN_W-1:0]    desc_wr_gen;
  logic [4:0]                desc_wr_rd;
  logic                      desc_wr_reg_we;
  logic [31:0]               desc_live_ctr;
  logic [4:0]                desc_rd0, desc_rd1;
  // The destination's *physical* generation, which the ROB does not carry: the
  // ROB knows the tag and its own entry generation, while rename's maps are
  // keyed on the tag's generation. The descriptor store is where the
  // destination identity lives (that is what "the retire-only fields the ROB
  // does not carry" means), so the commit takes it from there.
  // `rd_gen0`/`rd_gen1` are the 8-bit identity field the PRF carries; rename's
  // maps are keyed on its low `MOSAIC_INT_PRF_TAG_W` bits, and dispatch writes
  // the generation zero-extended into the wider field, so the top bit is
  // structurally zero and is deliberately not read here.
  /* verilator lint_off UNUSEDSIGNAL */
  logic [CORE_PGEN_W-1:0]    desc_gen0, desc_gen1;
  /* verilator lint_on UNUSEDSIGNAL */
  logic                      desc_reg_we0, desc_reg_we1;
  logic [1:0]                retire_clr_valid;
  logic [1:0][CORE_IDX_W-1:0] retire_clr_index;

  // ROB
  logic                      rob_alloc_valid;
  logic [CORE_TAG_W-1:0]     rob_alloc_tag;
  logic [CORE_XLEN-1:0]      rob_alloc_pc;
  logic [3:0]                rob_alloc_num_uops;
  logic                      rob_alloc_exc, rob_alloc_open;
  logic                      rob_alloc_ok, rob_alloc_refused;
  logic [CORE_IDX_W-1:0]     rob_alloc_index;
  logic [CORE_RGEN_W-1:0]    rob_alloc_gen;
  logic                      rob_cmp_valid;
  logic [CORE_IDX_W-1:0]     rob_cmp_index;
  logic [CORE_RGEN_W-1:0]    rob_cmp_gen;
  logic [CORE_UOP_W-1:0]     rob_cmp_uop;
  logic                      rob_cmp_exc;
  logic                      rob_cmp_accepted, rob_cmp_duplicate, rob_cmp_stale;
  logic                      rob_cmp_bad_uop;
  logic                      rob_retire_req_next, rob_retire_ack_next;
  logic                      rob_retire_ack;
  logic                      rob_head_valid, rob_head_ready;
  logic                      rob_head_complete;
  logic                      rob_head_exc;
  logic [CORE_IDX_W-1:0]     rob_head_index;
  logic [CORE_RGEN_W-1:0]    rob_head_gen;
  logic [CORE_TAG_W-1:0]     rob_head_tag;
  logic [CORE_XLEN-1:0]      rob_head_pc;
  logic                      rob_head1_valid, rob_head1_ready;
  logic                      rob_head1_exc;
  logic [CORE_IDX_W-1:0]     rob_head1_index;
  logic [CORE_RGEN_W-1:0]    rob_head1_gen;
  logic [CORE_TAG_W-1:0]     rob_head1_tag;
  logic [CORE_XLEN-1:0]      rob_head1_pc;
  logic [CORE_OCC_W-1:0]     rob_occupied, rob_free_rob;

  // clusters
  logic                      c0_ins_valid, c0_ins_ready;
  logic [CORE_UOP_ID_W-1:0]  c0_ins_uop;
  mosaic_uop_pkg::uop_meta_t c0_ins_meta;
  logic [CORE_XLEN-1:0]      c0_ins_imm;
  logic [CORE_TAG_W-1:0]     c0_s1_tag, c0_s2_tag, c0_dst_tag;
  logic [CORE_IGEN_W-1:0]    c0_s1_gen, c0_s2_gen, c0_dst_gen;
  logic                      c0_s1_rdy, c0_s2_rdy;
  logic [CORE_XLEN-1:0]      c0_s1_val, c0_s2_val;
  logic                      c1_ins_valid, c1_ins_ready;
  logic [CORE_UOP_ID_W-1:0]  c1_ins_uop;
  mosaic_uop_pkg::uop_meta_t c1_ins_meta;
  logic [CORE_XLEN-1:0]      c1_ins_imm;
  logic [CORE_TAG_W-1:0]     c1_s1_tag, c1_s2_tag, c1_dst_tag;
  logic [CORE_IGEN_W-1:0]    c1_s1_gen, c1_s2_gen, c1_dst_gen;
  logic                      c1_s1_rdy, c1_s2_rdy;
  logic [CORE_XLEN-1:0]      c1_s1_val, c1_s2_val;
  mosaic_uop_pkg::wb_event_t c0_wb_ev, c1_wb_ev, md_wb_ev;
  logic                      c0_wb_valid, c0_wb_ready;
  logic                      c1_wb_valid, c1_wb_ready;
  logic                      md_wb_valid, md_wb_ready;
  logic                      c0_redir_valid, c1_redir_valid;
  logic [CORE_XLEN-1:0]      c0_redir_pc, c1_redir_pc;
  logic [CORE_IDX_W-1:0]     c0_redir_idx, c1_redir_idx;
  logic [CORE_RGEN_W-1:0]    c0_redir_gen, c1_redir_gen;
  logic                      c0_redir_taken, c1_redir_taken;
  logic [CORE_RET_N-1:0]     redir_ack_vec;
  logic                      c0_flush_busy, c1_flush_busy;
  logic [31:0]               c0_count, c1_count;
  logic                      c0_grant_valid, c1_grant_valid;
  logic [CORE_UOP_ID_W-1:0]  c0_grant_uop, c1_grant_uop;
  logic [31:0]               c0_alu_ctr, c1_alu_ctr, c0_br_ctr, c1_br_ctr, md_ctr;

  // wakeup / PRF / arbiter
  logic                      wu_valid;
  logic [CORE_TAG_W-1:0]     wu_tag;
  logic [CORE_IGEN_W-1:0]    wu_gen;
  logic [CORE_XLEN-1:0]      wu_val;
  logic [CORE_BANKS-1:0]          prf_wr_en, prf_wr_gen_valid;
  logic [CORE_BANKS*CORE_TAG_W-1:0]  prf_wr_tag;
  logic [CORE_BANKS*CORE_PGEN_W-1:0] prf_wr_gen;
  logic [CORE_BANKS*CORE_XLEN-1:0]   prf_wr_data;
  logic [CORE_BANKS-1:0]          prf_rd_valid;
  logic [CORE_BANKS*CORE_TAG_W-1:0]  prf_rd_tag;
  logic [CORE_BANKS*CORE_PGEN_W-1:0] prf_rd_gen;
  logic [CORE_BANKS-1:0]          prf_rsp_valid, prf_rsp_bad, prf_rsp_never;
  logic [CORE_BANKS*CORE_XLEN-1:0] prf_rsp_data;
  logic [CORE_XLEN-1:0]      stash_value0, stash_value1;
  logic [31:0]               wb_wr_ctr, wb_wake_ctr, wb_stale_ctr, wb_dup_ctr;
  logic [31:0]               wb_collision_ctr, wb_drop_ctr;

  // MUL/DIV
  logic                      md_req_valid, md_req_ready_raw, md_req_ready_gated;
  mosaic_pkg::md_op_e        md_req_op;
  logic                      md_req_w;
  logic [CORE_XLEN-1:0]      md_req_a, md_req_b;
  logic [CORE_IDX_W-1:0]     md_req_rob_index;
  logic [CORE_RGEN_W-1:0]    md_req_rob_gen;
  logic [CORE_UOP_W-1:0]     md_req_uop_index;
  logic [CORE_TAG_W-1:0]     md_req_dst_tag;
  logic [CORE_IGEN_W-1:0]    md_req_dst_gen;
  logic                      md_req_fire;
  logic                      md_res_valid, md_res_ready;
  logic [CORE_XLEN-1:0]      md_res_data;
  logic [CORE_IDX_W-1:0]     md_res_rob_index;
  logic [CORE_RGEN_W-1:0]    md_res_rob_gen;
  logic [CORE_UOP_W-1:0]     md_res_uop_index;
  logic                      md_dst_valid;
  logic [CORE_TAG_W-1:0]     md_dst_tag_q;
  logic [CORE_IGEN_W-1:0]    md_dst_gen_q;

  // retire
  logic [CORE_RET_N-1:0]     ret_req;
  logic [CORE_RET_N-1:0]     ret_commit_valid;
  logic [CORE_RET_N*CORE_RD_W-1:0]  ret_commit_rd;
  logic [CORE_RET_N*CORE_TAG_W-1:0] ret_commit_tag;
  logic [CORE_RET_N*CORE_XLEN-1:0] retire_pay_value;
  logic                      head_pending_taken;

  // evidence
  logic [31:0] commit_ctr, redirect_ctr, recovering_ctr, stop_ctr, cycle_ctr;
  logic [31:0] squash_under_ctr, journal_ovf_ctr;
  logic [31:0] squash_acc_ctr, ckpt_ctr;
  logic [31:0] disp_alloc_ctr, disp_ins_ctr;

  // ------------------------------------------------------- the memory path
  // Everything the integrated LSU needs between dispatch, the two queues and
  // the endpoint. Declared here with the rest: a port connection must see its
  // net, and the whole point of this file's ordering rule is that nothing is
  // declared where it happens to be convenient.
  logic                       disp_mem_valid, disp_mem_ready, disp_mem_is_store;
  logic [CORE_XLEN-1:0]       disp_mem_base, disp_mem_imm, disp_mem_data;
  logic [2:0]                 disp_mem_size;
  logic                       disp_mem_signed;
  logic [CORE_UOP_ID_W-1:0]   disp_mem_id;
  logic [CORE_MEM_ID_W-1:0]   disp_mem_full_id;
  logic [CORE_TAG_W-1:0]      disp_mem_dst_tag;
  logic [CORE_IGEN_W-1:0]     disp_mem_dst_gen;
  logic                       disp_mem_dst_x0;
  logic [31:0]                mem_ins_stall_ctr;

  logic                       lq_alloc_valid, lq_alloc_ready;
  logic                       lq_req_valid, lq_req_ready;
  mosaic_uop_pkg::lsu_req_t   lq_req;
  logic                       lq_rsp_valid, lq_rsp_ready;
  logic                       lq_result_valid, lq_result_ready;
  mosaic_uop_pkg::uop_id_t    lq_result_id;
  logic [CORE_TAG_W-1:0]      lq_result_dst_tag;
  logic [CORE_IGEN_W-1:0]     lq_result_dst_gen;
  logic                       lq_result_dst_x0;
  logic [CORE_XLEN-1:0]       lq_result_data, lq_result_cause, lq_result_tval;
  logic                       lq_result_fault;
  logic                       lq_flush;
  logic                       lq_query_valid, lq_query_blocked;
  logic [CORE_XLEN-1:0]       lq_query_addr, lq_query_data;
  logic [2:0]                 lq_query_size;
  logic [31:0]                lq_alloc_ctr, lq_issue_ctr, lq_done_ctr, lq_replay_ctr;
  logic [31:0]                lq_blocked_ctr, lq_fwd_byte_ctr, lq_mem_byte_ctr;
  logic [31:0]                lq_fault_ctr, lq_mismatch_ctr;
  logic [CORE_MEM_CNT_W-1:0]  lq_count;

  logic                       sq_alloc_valid, sq_alloc_ready;
  logic                       sq_commit_valid;
  logic                       sq_commit2_valid, sq_commit2_ok;
  mosaic_uop_pkg::uop_id_t    sq_commit_id, sq_commit2_id;
  logic [31:0]                sq_commit2_ctr;
  logic                       sq_drain_valid, sq_drain_ready;
  mosaic_uop_pkg::lsu_req_t   sq_drain_req;
  logic                       sq_rsp_valid, sq_rsp_ready;
  logic                       sq_squash_valid, sq_squash_all;
  logic [CORE_IDX_W-1:0]      sq_squash_from, sq_squash_tail;
  logic [CORE_RGEN_W-1:0]     sq_squash_gen;
  logic [CORE_MEM_CNT_W-1:0]  sq_count, sq_auth_cnt;
  logic [CORE_SQ_N*CORE_SQ_ENTRY_W-1:0] sq_entry_pay;
  logic [2:0]                 sq_alloc_size;
  logic [31:0]                sq_alloc_ctr, sq_commit_ctr, sq_commit_stale_ctr;
  logic [31:0]                sq_drain_ctr, sq_squash_ctr, sq_spared_ctr, sq_fault_ctr;

  logic                       ep_req_valid, ep_req_ready;
  mosaic_uop_pkg::lsu_req_t   ep_req;
  logic                       ep_rsp_valid, ep_rsp_ready;
  mosaic_uop_pkg::lsu_rsp_t   ep_rsp;
  logic                       ep_owner_q;
  logic [31:0]                lsu_txn_ctr, lsu_misaligned_ctr, lsu_access_fault_ctr;

  logic                       lsu_wb_valid, lsu_wb_ready;
  mosaic_uop_pkg::wb_event_t  lsu_wb_ev, store_wb_ev, lq_wb_ev;
  logic                       store_cmp_valid;

  logic                       desc_is_store0, desc_is_store1;
  logic                       desc_wr_is_store;
  logic [CORE_IDX_W-1:0]      rob_alloc_ptr;
  logic [31:0] squash_nc_ctr;
  logic        core_stop_prev;

  // Leaf modules bring out observation and status outputs that this package
  // does not consume -- the fetch unit's delivery counters, the retire module's
  // event classification flags, the rename module's debug views, the second
  // allocation lane of rename. Leaving those pins empty is stated here, once,
  // rather than by declaring dozens of nets nobody reads; nothing functional is
  // routed through them, and the units' own cases read them where they matter.
  /* verilator lint_off PINCONNECTEMPTY */

  // ==========================================================================
  // 1. Fetch
  // ==========================================================================
  // The request side: a request is issued whenever the frontend may run.
  // `pred_valid` is tied low, so the predictor offers no next PC and the
  // generator takes its fall-through arm -- the documented
  // `pred_next_valid ? pred_next_pc : last_pc + 4`, with the prediction side
  // disabled because this package cannot classify an instruction before it has
  // been fetched and decoded. Every taken branch therefore redirects; that is a
  // deliberate correctness-first choice, and recovery is what the case exercises.
  // Composition of the two readinesses, and they are different things:
  // `fetch_slot_free` is the fetch unit's credit for an outstanding request, and
  // `imem_req_ready` is the memory endpoint accepting one. The request is
  // offered to the memory whenever fetch has a credit, and is handed to fetch
  // only in the cycle the memory took it, so exactly one request is issued and
  // one slot is spent.
  assign want_imem_req      = !core_stop && !recovering;
  assign imem_req_valid     = want_imem_req && fetch_slot_free;
  assign fetch_req_valid_int= imem_req_valid && imem_req_ready;
  assign fetch_req_fire     = fetch_req_valid_int;
  assign imem_req.we    = 1'b0;
  assign imem_req.addr  = fetch_pc_q;
  assign imem_req.size  = mosaic_pkg::SZ_WORD;
  assign imem_req.wstrb = {(CORE_XLEN/8){1'b0}};
  assign imem_req.wdata = {CORE_XLEN{1'b0}};

  always_ff @(posedge clk) begin
    if (rst) begin
      fetch_pc_q <= mosaic_cfg_pkg::MOSAIC_RESET_VECTOR;
    end else if (redirect_valid) begin
`ifdef MOSAIC_CORE_MUTANT_REDIRECT_NEXT
      // NEGATIVE CONTROL: the front end resumes one instruction past the
      // redirect target instead of at it, so the first instruction of the
      // resolved branch's path is skipped. CASE=core.corpus_branch must fail --
      // the redirect-port comparison names the wrong PC before the retirement
      // stream even diverges.
      fetch_pc_q <= redirect_pc + CORE_XLEN'(4);
`else
      fetch_pc_q <= redirect_pc;
`endif
    end else if (fetch_req_fire) begin
      fetch_pc_q <= fetch_pred_next_valid ? fetch_pred_next_pc
                                          : (fetch_pc_q + CORE_XLEN'(4));
    end
  end

  mosaic_fetch u_fetch (
      .clk                (clk),
      .rst                (rst),
      .req_valid          (fetch_req_valid_int),
      .req_pc             (fetch_pc_q),
      .req_ready          (fetch_slot_free),
      .req_id             (imem_req_id),
      .req_epoch          (imem_req_epoch),
      .rsp_valid          (imem_rsp_valid),
      .rsp_ready          (imem_rsp_ready),
      .rsp_squashed       (),
      .rsp_id             (imem_rsp_id),
      .rsp_epoch          (imem_rsp_epoch),
      .rsp_data           (imem_rsp.rdata[31:0]),
      .rsp_len            (imem_rsp_len),
      .rsp_fault          (imem_rsp.fault),
      .redirect_valid     (redirect_valid),
      .redirect_pc        (redirect_pc),
      .pred_valid         (1'b0),
      .pred_pc            ({CORE_XLEN{1'b0}}),
      .pred_is_branch     (1'b0),
      .pred_is_jump       (1'b0),
      .pred_is_return     (1'b0),
      .upd_valid          (1'b0),
      .upd_pc             ({CORE_XLEN{1'b0}}),
      .upd_is_branch      (1'b0),
      .upd_is_jump        (1'b0),
      .upd_is_call        (1'b0),
      .upd_is_return      (1'b0),
      .upd_is_taken       (1'b0),
      .upd_target         ({CORE_XLEN{1'b0}}),
      .ckpt_valid         (1'b0),
      .flush              (redirect_valid),
      .pred_next_valid    (fetch_pred_next_valid),
      .pred_next_pc       (fetch_pred_next_pc),
      .pred_squashed      (),
      .pred_taken         (),
      .pred_btb_hit       (),
      .pred_btb_miss      (),
      .pred_ras_valid     (),
      .pred_ras_underflow (),
      .ras_overflow       (),
      .ras_underflow      (),
      .out_valid          (fetch_out_valid),
      .out_ready          (fetch_out_ready),
      .out_pc             (fetch_out_pc),
      .out_bits           (fetch_out_bits),
      .out_len            (),
      .out_illegal        (fetch_out_illegal),
      .out_fault          (fetch_out_fault),
      .out_cause          (),
      .o_dbg_state        (fetch_dbg_state),
      .outstanding_count  (),
      .cancel_pending     (),
      .epoch_now          (),
      .issued_count       (),
      .accept_count       (),
      .drop_count         (),
      .stale_drop_count   (),
      .squashed_drop_count(),
      .credit_drop_count  (),
      .delivered_count    (),
      .fault_count        (),
      .illegal_count      (),
      .deny_count         (),
      .cancel_count       (),
      .fetch_pc           ()
  );

  // ==========================================================================
  // 2. Decode buffer (2 entries, program order, lane 0 oldest)
  // ==========================================================================
  mosaic_decoder u_dec (
      .insn (fetch_out_bits),
      .ctl  (dec_ctl_comb)
  );

  // A delivered instruction that fetch reported illegal or faulted does not
  // decode; it is carried as an invalid control word, which dispatch refuses
  // and counts, and the machine stops cleanly at it. Taking the trap is
  // I-019's business, not this package's.
  always_comb begin
    dbuf_ctl_new = dec_ctl_comb;
    if (fetch_out_illegal || fetch_out_fault) begin
      dbuf_ctl_new.valid   = 1'b0;
      dbuf_ctl_new.illegal = 1'b1;
    end
  end

  assign dbuf_take = disp_take;
  assign dbuf_room = (dbuf_cnt < 2'd2) || dbuf_take;
  assign dbuf_push = fetch_out_valid && dbuf_room && !core_stop;
  assign fetch_out_ready = dbuf_room && !core_stop;

  // The buffer's next state, one expression per slot. The valid entries are
  // always the contiguous run `[0 .. dbuf_cnt-1]`, oldest at slot 0:
  //
  //   * a pop shifts every entry down by one and invalidates the slot it
  //     vacated, unless this cycle's push lands there;
  //   * a push lands at the tail *after* the pop, `dbuf_cnt - dbuf_take`.
  //
  // Writing slot 0 whenever nothing was popped -- the form this replaces --
  // overwrites a live entry when the buffer holds exactly one entry in slot 1,
  // which is the state a pop leaves behind: the next push then lands *in front
  // of* an older instruction, and the two swap places in program order. That is
  // not a scheduling freedom: the ROB allocates in the order dispatch presents
  // macros, so the machine would retire two instructions out of program order.
  // CASE=fabric.fixed_two_cluster reads the retirement stream back in program
  // order, and it is what caught this.
  always_comb begin
    // The push slot is the tail after this cycle's pop, `dbuf_cnt - dbuf_take`,
    // and only its low bit is ever needed: a push is offered only when that
    // difference is 0 or 1 (`dbuf_room` refuses a full buffer with no pop), and
    // the low bit of a difference is the xor of the operands' low bits.
`ifdef MOSAIC_CORE_MUTANT_DBUF_PUSH_SLOT
    // NEGATIVE CONTROL for the ordering fix below: the slot is chosen from the
    // pop alone, which is the form that overwrites a live entry when the buffer
    // holds one entry in slot 1. CASE=fabric.fixed_two_cluster must then fail
    // its program-order comparison (the first two macros are allocated in the
    // wrong order); see results/reports/I-023-core.md.
    dbuf_push_at = dbuf_take;
`else
    dbuf_push_at = dbuf_cnt[0] ^ dbuf_take;
`endif
    dbuf_valid_n[0] = dbuf_valid[0];
    dbuf_valid_n[1] = dbuf_valid[1];
    dbuf_pc_n    = dbuf_pc;
    dbuf_ctl_n   = dbuf_ctl;
    if (dbuf_take) begin
      dbuf_valid_n[0] = dbuf_valid[1];
      dbuf_pc_n[0]    = dbuf_pc[1];
      dbuf_ctl_n[0]   = dbuf_ctl[1];
      // The entry's old slot is invalidated. With the push slot chosen above,
      // the live entries are exactly `[0 .. dbuf_cnt-1]`; a pop that is not
      // accompanied by a push would otherwise leave the shifted entry alive in
      // slot 1 and re-dispatch it on the next pop. This case does not reach
      // that state -- it needs the buffer full and then drained with no
      // delivery alongside, i.e. dispatch and fetch stalled together -- and the
      // gap is recorded in results/reports/I-023-core.md rather than claimed.
      dbuf_valid_n[1] = 1'b0;
    end
    if (dbuf_push) begin
      dbuf_valid_n[dbuf_push_at] = 1'b1;
      dbuf_pc_n[dbuf_push_at]    = fetch_out_pc;
      dbuf_ctl_n[dbuf_push_at]   = dbuf_ctl_new;
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      dbuf_cnt      <= 2'd0;
      dbuf_valid[0] <= 1'b0;
      dbuf_valid[1] <= 1'b0;
    end else if (redirect_valid || core_stop) begin
      // A redirect discards everything fetched before it; a stop freezes the
      // buffer where it is (the refused macro must stay refused).
`ifdef MOSAIC_CORE_MUTANT_NO_PURGE
      // NEGATIVE CONTROL: the redirect does not purge the younger work it
      // discards. The taken branch's fall-through instruction is already in the
      // buffer, so it is dispatched after the redirect as though it were on the
      // correct path, retires, and the case's per-instruction comparison against
      // the reference names it. CASE=core.corpus_branch must fail.
      if (1'b0) begin
`else
      if (redirect_valid) begin
`endif
        dbuf_valid[0] <= 1'b0;
        dbuf_valid[1] <= 1'b0;
        dbuf_cnt      <= 2'd0;
      end
    end else begin
      dbuf_valid[0] <= dbuf_valid_n[0];
      dbuf_valid[1] <= dbuf_valid_n[1];
      dbuf_pc       <= dbuf_pc_n;
      dbuf_ctl      <= dbuf_ctl_n;
      dbuf_cnt      <= dbuf_cnt + {1'b0, dbuf_push} - {1'b0, dbuf_take};
    end
  end

  // ==========================================================================
  // 3. Rename
  // ==========================================================================
  mosaic_rename u_rename (
      .clk              (clk),
      .rst              (rst),
      .alloc_req        (ren_alloc_req),
      .alloc_rd         (alloc_rd_w),
      .alloc_accepted   (ren_alloc_accepted),
      .alloc_exhausted  (ren_alloc_exhausted),
      .alloc_squashed   (ren_alloc_squashed),
      .alloc_is_x0      (ren_alloc_is_x0),
      .alloc_new_valid  (ren_alloc_new_valid),
      .alloc_new_tag    (ren_new_tag),
      .alloc_new_gen    (ren_new_gen),
      .alloc_old_valid  (),
      .alloc_old_tag    (),
      .alloc_old_gen    (),
      .alloc2_req       (1'b0),
      .alloc2_rd        (5'd0),
      .alloc2_accepted  (),
      .alloc2_exhausted (),
      .alloc2_squashed  (),
      .alloc2_is_x0     (),
      .alloc2_new_valid (),
      .alloc2_new_tag   (),
      .alloc2_new_gen   (),
      .alloc2_old_valid (),
      .alloc2_old_tag   (),
      .alloc2_old_gen   (),
      .rs1_addr         (ren_rs1_addr),
      .rs2_addr         (ren_rs2_addr),
      .rs1_is_x0        (ren_rs1_is_x0),
      .rs2_is_x0        (ren_rs2_is_x0),
      .rs1_ready        (),
      .rs2_ready        (),
      .rs1_tag          (ren_rs1_tag),
      .rs2_tag          (ren_rs2_tag),
      .rs1_gen          (ren_rs1_gen),
      .rs2_gen          (ren_rs2_gen),
      .rs3_addr         (5'd0),
      .rs4_addr         (5'd0),
      .rs3_is_x0        (),
      .rs4_is_x0        (),
      .rs3_ready        (),
      .rs4_ready        (),
      .rs3_bypass       (),
      .rs4_bypass       (),
      .rs3_tag          (),
      .rs4_tag          (),
      .rs3_gen          (),
      .rs4_gen          (),
      .wb_valid         (ren_wb_valid),
      .wb_tag           (ren_wb_tag),
      .wb_gen           (ren_wb_gen),
      .wb_accepted      (ren_wb_accepted),
      .wb_stale         (ren_wb_stale),
      .wb_duplicate     (ren_wb_duplicate),
      .free_valid       (1'b0),
      .free_tag         ({CORE_TAG_W{1'b0}}),
      .free_gen         ({CORE_IGEN_W{1'b0}}),
      .free_accepted    (),
      .free_stale       (),
      .free_double      (),
      .commit_valid     (ren_commit_valid),
      .commit_rd        (ren_commit_rd),
      .commit_tag       (ren_commit_tag),
      .commit_gen       (ren_commit_gen),
      .commit_accepted  (),
      .commit_x0_dropped(),
      .commit2_valid    (ren_commit2_valid),
      .commit2_rd       (ren_commit2_rd),
      .commit2_tag      (ren_commit2_tag),
      .commit2_gen      (ren_commit2_gen),
      .commit2_accepted (),
      .commit2_x0_dropped(),
      // ------------------------------------------------------- the checkpoint
      // The redirect pulse is also the cycle the branch's own commit has landed:
      // `mosaic_retire`'s commit is combinational with the retire
      // acknowledgement, so at the cycle *after* the branch retires the branch's
      // mapping is in the committed map and nothing younger has allocated (the
      // barrier in section 12 is what makes the second half true). That is the
      // one cycle in which the speculative map equals the committed map, which
      // is exactly the precondition `mosaic_rename` documents for a checkpoint
      // it will later accept a squash to. It is sampled here rather than assumed.
      //
      // The squash follows one cycle after the checkpoint, when the restore can
      // no longer race the commit that funded it. `mosaic_rename` refuses
      // allocation in a squash cycle, and dispatch is held by `recovering` for
      // the same cycle, so the two cannot collide.
      //
      // Under the barrier the squash restores a state that is already correct --
      // that is the point of the barrier. It is wired, and the case asserts
      // `ckpt_committed` held at the checkpoint and that the squash was
      // *accepted*, so the precondition is proved rather than trusted: the day
      // the barrier is lifted (I-018's saved-map controller), a squash that is
      // no longer a no-op lands on the same signal, and a checkpoint taken off
      // the boundary is refused and counted instead of silently corrupting the
      // map.
      .ckpt_valid       (ren_ckpt_valid),
      .squash           (ren_squash),
      .squash_accepted  (ren_squash_accepted),
      .squash_underflow (ren_squash_underflow),
      .squash_not_committed (ren_squash_not_committed),
      .ckpt_committed   (ren_ckpt_committed),
      .journal_overflow (ren_journal_overflow),
      .free_count       (ren_free_count),
      .dbg_free_mask    (),
      .dbg_gen_valid    (ren_gen_valid),
      .dbg_wb_done      (o_dbg_wb_done),
      .dbg_tag_gen      (),
      .dbg_spec_map     (o_dbg_spec_map),
      .dbg_cmt_map      (),
      .dbg_j_len        ()
  );

  // ==========================================================================
  // 4. Descriptor store
  // ==========================================================================
  mosaic_macro_desc u_desc (
      .clk             (clk),
      .rst             (rst),
      .wr_valid        ({1'b0, desc_wr_valid}),
      .wr_index        ({{CORE_IDX_W{1'b0}}, desc_wr_index}),
      .wr_tag          ({{CORE_TAG_W{1'b0}}, desc_wr_tag}),
      .wr_gen          ({{CORE_PGEN_W{1'b0}}, desc_wr_gen}),
      .wr_rd           ({5'd0, desc_wr_rd}),
      .wr_reg_we       ({1'b0, desc_wr_reg_we}),
      .wr_is_store     ({1'b0, desc_wr_is_store}),
      .rd_index0       (rob_head_index),
      .rd_index1       (rob_head1_index),
      .rd_valid0       (),
      .rd_tag0         (),
      .rd_gen0         (desc_gen0),
      .rd_rd0          (desc_rd0),
      .rd_reg_we0      (desc_reg_we0),
      .rd_is_store0    (desc_is_store0),
      .rd_valid1       (),
      .rd_tag1         (),
      .rd_gen1         (desc_gen1),
      .rd_rd1          (desc_rd1),
      .rd_reg_we1      (desc_reg_we1),
      .rd_is_store1    (desc_is_store1),
      .clr_valid       (retire_clr_valid),
      .clr_index       (retire_clr_index),
      .o_write_ctr     (),
      .o_clear_ctr     (),
      .o_live_ctr      (desc_live_ctr)
  );

  // ==========================================================================
  // 5. ROB
  // ==========================================================================
  mosaic_rob u_rob (
      .clk             (clk),
      .rst             (rst),
      .alloc_valid     (rob_alloc_valid),
      .alloc_tag       (rob_alloc_tag),
      .alloc_pc        (rob_alloc_pc),
      .alloc_num_uops  (rob_alloc_num_uops),
      .alloc_exc       (rob_alloc_exc),
      .alloc_open      (rob_alloc_open),
      .alloc_ok        (rob_alloc_ok),
      .alloc_refused   (rob_alloc_refused),
      .alloc_full      (),
      .alloc_bad_uops  (),
      .alloc_index     (rob_alloc_index),
      .alloc_gen       (rob_alloc_gen),
      .close_valid     (1'b0),
      .close_index     ({CORE_IDX_W{1'b0}}),
      .close_gen       ({CORE_RGEN_W{1'b0}}),
      .close_ok        (),
      .close_stale     (),
      .cmp_valid       (rob_cmp_valid),
      .cmp_index       (rob_cmp_index),
      .cmp_gen         (rob_cmp_gen),
      .cmp_uop         (rob_cmp_uop),
      .cmp_exc         (rob_cmp_exc),
      .cmp_accepted    (rob_cmp_accepted),
      .cmp_duplicate   (rob_cmp_duplicate),
      .cmp_stale       (rob_cmp_stale),
      .cmp_bad_uop     (rob_cmp_bad_uop),
      .retire_req      (ret_req[0]),
      .retire_ack      (rob_retire_ack),
      .retire_req_next (rob_retire_req_next),
      .retire_ack_next (rob_retire_ack_next),
      .head_valid      (rob_head_valid),
      .head_ready      (rob_head_ready),
      .head_replay     (),
      .head_complete   (rob_head_complete),
      .head_exc        (rob_head_exc),
      .head_closed     (),
      .head_index      (rob_head_index),
      .head_gen        (rob_head_gen),
      .head_tag        (rob_head_tag),
      .head_pc         (rob_head_pc),
      .head_num_uops   (),
      .head_done_mask  (),
      .head_done_cnt   (),
      .head1_valid     (rob_head1_valid),
      .head1_ready     (rob_head1_ready),
      .head1_replay    (),
      .head1_complete  (),
      .head1_exc       (rob_head1_exc),
      .head1_closed    (),
      .head1_index     (rob_head1_index),
      .head1_gen       (rob_head1_gen),
      .head1_tag       (rob_head1_tag),
      .head1_pc        (rob_head1_pc),
      .head1_num_uops  (),
      .head1_done_mask (),
      .head1_done_cnt  (),
      .flush_valid     (rob_flush_pulse),
      .obs_index       ({CORE_IDX_W{1'b0}}),
      .obs_valid       (),
      .obs_gen         (),
      .obs_tag         (),
      .obs_pc          (),
      .obs_num_uops    (),
      .obs_done_mask   (),
      .obs_done_cnt    (),
      .obs_exc         (),
      .obs_closed      (),
      .o_head_ptr      (),
      .o_alloc_ptr     (rob_alloc_ptr),
      .o_occupied      (rob_occupied),
      .o_free          (rob_free_rob),
      .o_alloc_total   (),
      .o_retired_total (),
      .o_squashed_total(),
      .o_gen_counter   ()
  );

  // ==========================================================================
  // 6. Clusters
  // ==========================================================================
  mosaic_cluster u_c0 (
      .clk             (clk),
      .rst             (rst),
      .ins_valid       (c0_ins_valid),
      .ins_ready       (c0_ins_ready),
      .ins_uop         (c0_ins_uop),
      .ins_meta        (c0_ins_meta),
      .ins_imm         (c0_ins_imm),
      .ins_src1_tag    (c0_s1_tag),
      .ins_src1_gen    (c0_s1_gen),
      .ins_src1_ready  (c0_s1_rdy),
      .ins_src1_val    (c0_s1_val),
      .ins_src2_tag    (c0_s2_tag),
      .ins_src2_gen    (c0_s2_gen),
      .ins_src2_ready  (c0_s2_rdy),
      .ins_src2_val    (c0_s2_val),
      .ins_dst_tag     (c0_dst_tag),
      .ins_dst_gen     (c0_dst_gen),
      .wu_valid        (wu_valid),
      .wu_tag          (wu_tag),
      .wu_gen          (wu_gen),
      .wu_val          (wu_val),
      .flush           (cluster_flush_pulse),
      .flush_busy      (c0_flush_busy),
      .wb_ev           (c0_wb_ev),
      .wb_valid        (c0_wb_valid),
      .wb_ready        (c0_wb_ready),
      .redir_req_valid (c0_redir_valid),
      .redir_req_pc    (c0_redir_pc),
      .redir_req_rob_index (c0_redir_idx),
      .redir_req_rob_gen   (c0_redir_gen),
      .redir_req_taken (c0_redir_taken),
      .redir_req_ack   (redir_ack_vec[0]),
      .md_req_valid    (md_req_valid),
      .md_req_ready    (md_req_ready_gated),
      .md_req_op       (md_req_op),
      .md_req_w        (md_req_w),
      .md_req_a        (md_req_a),
      .md_req_b        (md_req_b),
      .md_req_rob_index(md_req_rob_index),
      .md_req_rob_gen  (md_req_rob_gen),
      .md_req_uop_index(md_req_uop_index),
      .md_req_dst_tag  (md_req_dst_tag),
      .md_req_dst_gen  (md_req_dst_gen),
      .o_occupied      (),
      .o_count         (c0_count),
      .o_full          (),
      .o_dst_conflict  (),
      .o_ins_total     (),
      .o_grant_total   (),
      .o_kill_total    (),
      .o_grant_valid   (c0_grant_valid),
      .o_grant_uop     (c0_grant_uop),
      .o_alu_ctr       (c0_alu_ctr),
      .o_branch_ctr    (c0_br_ctr),
      .o_md_ctr        (md_ctr),
      .o_refuse_ctr    (),
      .o_purge_ctr     (),
      .o_wu_miss_ctr   ()
  );

  mosaic_cluster u_c1 (
      .clk             (clk),
      .rst             (rst),
      .ins_valid       (c1_ins_valid),
      .ins_ready       (c1_ins_ready),
      .ins_uop         (c1_ins_uop),
      .ins_meta        (c1_ins_meta),
      .ins_imm         (c1_ins_imm),
      .ins_src1_tag    (c1_s1_tag),
      .ins_src1_gen    (c1_s1_gen),
      .ins_src1_ready  (c1_s1_rdy),
      .ins_src1_val    (c1_s1_val),
      .ins_src2_tag    (c1_s2_tag),
      .ins_src2_gen    (c1_s2_gen),
      .ins_src2_ready  (c1_s2_rdy),
      .ins_src2_val    (c1_s2_val),
      .ins_dst_tag     (c1_dst_tag),
      .ins_dst_gen     (c1_dst_gen),
      .wu_valid        (wu_valid),
      .wu_tag          (wu_tag),
      .wu_gen          (wu_gen),
      .wu_val          (wu_val),
      .flush           (cluster_flush_pulse),
      .flush_busy      (c1_flush_busy),
      .wb_ev           (c1_wb_ev),
      .wb_valid        (c1_wb_valid),
      .wb_ready        (c1_wb_ready),
      .redir_req_valid (c1_redir_valid),
      .redir_req_pc    (c1_redir_pc),
      .redir_req_rob_index (c1_redir_idx),
      .redir_req_rob_gen   (c1_redir_gen),
      .redir_req_taken (c1_redir_taken),
      .redir_req_ack   (redir_ack_vec[1]),
      .md_req_valid    (),
      .md_req_ready    (1'b1),
      .md_req_op       (),
      .md_req_w        (),
      .md_req_a        (),
      .md_req_b        (),
      .md_req_rob_index(),
      .md_req_rob_gen  (),
      .md_req_uop_index(),
      .md_req_dst_tag  (),
      .md_req_dst_gen  (),
      .o_occupied      (),
      .o_count         (c1_count),
      .o_full          (),
      .o_dst_conflict  (),
      .o_ins_total     (),
      .o_grant_total   (),
      .o_kill_total    (),
      .o_grant_valid   (c1_grant_valid),
      .o_grant_uop     (c1_grant_uop),
      .o_alu_ctr       (c1_alu_ctr),
      .o_branch_ctr    (c1_br_ctr),
      .o_md_ctr        (),
      .o_refuse_ctr    (),
      .o_purge_ctr     (),
      .o_wu_miss_ctr   ()
  );

  // ==========================================================================
  // 7. Shared MUL/DIV and the destination latch for its out-of-band result
  // ==========================================================================
  assign md_req_ready_gated = md_req_ready_raw && !md_dst_valid;
  assign md_req_fire        = md_req_valid && md_req_ready_gated;

  mosaic_muldiv u_muldiv (
      .clk_i           (clk),
      .rst_i           (rst),
      .req_valid_i     (md_req_valid),
      .req_ready_o     (md_req_ready_raw),
      .req_op_i        (md_req_op),
      .req_w_i         (md_req_w),
      .req_a_i         (md_req_a),
      .req_b_i         (md_req_b),
      .req_rob_index_i (md_req_rob_index),
      .req_rob_gen_i   (md_req_rob_gen),
      .req_uop_index_i (md_req_uop_index),
      .flush_i         (redirect_valid),
      .res_valid_o     (md_res_valid),
      .res_ready_i     (md_res_ready),
      .res_data_o      (md_res_data),
      .res_rob_index_o (md_res_rob_index),
      .res_rob_gen_o   (md_res_rob_gen),
      .res_uop_index_o (md_res_uop_index),
      .o_busy          (),
      .o_iter          (),
      .o_accepted_ctr  (),
      .o_completed_ctr (),
      .o_cancelled_ctr (),
      .o_killed_res_ctr()
  );

  // One operation in flight, so one latched destination identity. It is taken
  // when the shared unit accepts a request and released when its result has
  // been handed to the writeback arbiter; a flush cancels the operation and the
  // latch with it, because a cancelled operation never produces a result.
  always_ff @(posedge clk) begin
    if (rst) begin
      md_dst_valid <= 1'b0;
      md_dst_tag_q <= {CORE_TAG_W{1'b0}};
      md_dst_gen_q <= {CORE_IGEN_W{1'b0}};
    end else begin
      if (md_req_fire) begin
        md_dst_valid <= 1'b1;
        md_dst_tag_q <= md_req_dst_tag;
        md_dst_gen_q <= md_req_dst_gen;
      end else if (md_res_valid && md_res_ready) begin
        md_dst_valid <= 1'b0;
      end else if (redirect_valid) begin
        md_dst_valid <= 1'b0;
      end
    end
  end

  // The shared unit's result becomes a completion. Its destination comes from
  // the latch; the identity comes from the unit itself.
  always_comb begin
    md_wb_ev.id.hart      = 1'b0;
    md_wb_ev.id.rob_index = md_res_rob_index;
    md_wb_ev.id.rob_gen   = md_res_rob_gen;
    md_wb_ev.id.uop_index = md_res_uop_index;
    md_wb_ev.dst.tag      = md_dst_tag_q;
    md_wb_ev.dst.gen      = {{(CORE_PGEN_W - CORE_IGEN_W){1'b0}}, md_dst_gen_q};
    md_wb_ev.dst.x0       = (md_dst_tag_q == {CORE_TAG_W{1'b0}});
    md_wb_ev.value_valid  = (md_dst_tag_q != {CORE_TAG_W{1'b0}});
    md_wb_ev.value        = md_res_data;
    md_wb_ev.exc.valid    = 1'b0;
    md_wb_ev.exc.cause    = {CORE_XLEN{1'b0}};
    md_wb_ev.exc.tval     = {CORE_XLEN{1'b0}};
    md_wb_ev.is_store     = 1'b0;
    md_wb_ev.is_load      = 1'b0;
  end

  assign md_wb_valid  = md_res_valid && md_dst_valid;
  assign md_res_ready = md_wb_ready && md_dst_valid;

  // ==========================================================================
  // 8. Writeback arbiter
  // ==========================================================================
  mosaic_wb_arbiter u_wb (
      .clk                 (clk),
      .rst                 (rst),
      .wb_ev0              (c0_wb_ev),
      .wb_valid0           (c0_wb_valid),
      .wb_ready0           (c0_wb_ready),
      .wb_ev1              (c1_wb_ev),
      .wb_valid1           (c1_wb_valid),
      .wb_ready1           (c1_wb_ready),
      .wb_ev2              (md_wb_ev),
      .wb_valid2           (md_wb_valid),
      .wb_ready2           (md_wb_ready),
      // The memory path's producer: a load's merged value or a store's
      // destination-less completion. One port, because the two can never be
      // offered in the same cycle (the core holds the store insert while a load
      // result waits, and vice versa).
      .wb_ev3              (lsu_wb_ev),
      .wb_valid3           (lsu_wb_valid),
      .wb_ready3           (lsu_wb_ready),
      .prf_wr_en           (prf_wr_en),
      .prf_wr_gen_valid    (prf_wr_gen_valid),
      .prf_wr_tag          (prf_wr_tag),
      .prf_wr_gen          (prf_wr_gen),
      .prf_wr_data         (prf_wr_data),
      .ren_wb_valid        (ren_wb_valid),
      .ren_wb_tag          (ren_wb_tag),
      .ren_wb_gen          (ren_wb_gen),
      .ren_wb_accepted     (ren_wb_accepted),
      .ren_wb_stale        (ren_wb_stale),
      .ren_wb_duplicate    (ren_wb_duplicate),
      .rob_cmp_valid       (rob_cmp_valid),
      .rob_cmp_index       (rob_cmp_index),
      .rob_cmp_gen         (rob_cmp_gen),
      .rob_cmp_uop         (rob_cmp_uop),
      .rob_cmp_exc         (rob_cmp_exc),
      .rob_cmp_accepted    (rob_cmp_accepted),
      .rob_cmp_duplicate   (rob_cmp_duplicate),
      .rob_cmp_stale       (rob_cmp_stale),
      .rob_cmp_bad_uop     (rob_cmp_bad_uop),
      .wu_valid            (wu_valid),
      .wu_tag              (wu_tag),
      .wu_gen              (wu_gen),
      .wu_val              (wu_val),
      .q_valid             (disp_rq_valid),
      .q_tag               (disp_rq_tag),
      .q_gen               (disp_rq_gen),
      .q_written           (disp_rq_written),
      .stash_rd0           (rob_head_index),
      .stash_rd1           (rob_head1_index),
      .stash_valid0        (),
      .stash_value0        (stash_value0),
      .stash_valid1        (),
      .stash_value1        (stash_value1),
      .o_wr_ctr            (wb_wr_ctr),
      .o_wake_ctr          (wb_wake_ctr),
      .o_stale_ctr         (wb_stale_ctr),
      .o_dup_ctr           (wb_dup_ctr),
      .o_rob_stale_ctr     (),
      .o_rob_dup_ctr       (),
      .o_rob_ok_ctr        (),
      .o_collision_ctr     (wb_collision_ctr),
      .o_drop_ctr          (wb_drop_ctr),
      .o_pub_ctr           (),
      .o_wide_gen_ctr      (),
      .o_store_ctr         (),
      .o_load_ctr          (),
      .o_rob_bad_ctr       (),
      .o_pub_valid         (o_wb_pub_valid),
      .o_pub_index         (o_wb_pub_index),
      .o_pub_gen           (o_wb_pub_gen),
      .o_pub_value         (o_wb_pub_value)
  );

  // ==========================================================================
  // 9. PRF
  // ==========================================================================
  mosaic_prf u_prf (
      .clk_i               (clk),
      .rst_i               (rst),
      .wr_en_i             (prf_wr_en),
      .wr_gen_valid_i      (prf_wr_gen_valid),
      .wr_tag_i            (prf_wr_tag),
      .wr_gen_i            (prf_wr_gen),
      .wr_data_i           (prf_wr_data),
      .rd_valid_i          (prf_rd_valid),
      .rd_ready_o          (),
      .rd_tag_i            (prf_rd_tag),
      .rd_gen_i            (prf_rd_gen),
      .rsp_valid_o         (prf_rsp_valid),
      .rsp_tag_o           (),
      .rsp_gen_o           (),
      .rsp_data_o          (prf_rsp_data),
      .rsp_gen_mismatch_o  (prf_rsp_bad),
      .rsp_never_written_o (prf_rsp_never),
      .o_wr_ctr            (),
      .o_rd_ctr            (),
      .o_conflict_ctr      (),
      .o_mismatch_ctr      (),
      .o_invalid_ctr       (),
      .o_busy              ()
  );

  // ==========================================================================
  // 10. Dispatch
  // ==========================================================================
  mosaic_dispatch u_disp (
      .clk              (clk),
      .rst              (rst),
      .dec_valid        ({1'b0, dbuf_valid[0]}),
      .dec_ctl0         (dbuf_ctl[0]),
      .dec_ctl1         (dbuf_ctl[1]),
      .dec_pc0          (dbuf_pc[0]),
      .dec_pc1          (dbuf_pc[1]),
      .alloc_req        (ren_alloc_req),
      .alloc_rd         (alloc_rd_w),
      .alloc_accepted   (ren_alloc_accepted),
      .alloc_exhausted  (ren_alloc_exhausted),
      .alloc_squashed   (ren_alloc_squashed),
      .alloc_is_x0      (ren_alloc_is_x0),
      .alloc_new_valid  (ren_alloc_new_valid),
      .alloc_new_tag    (ren_new_tag),
      .alloc_new_gen    (ren_new_gen),
      .rs1_addr         (ren_rs1_addr),
      .rs2_addr         (ren_rs2_addr),
      .rs1_is_x0        (ren_rs1_is_x0),
      .rs2_is_x0        (ren_rs2_is_x0),
      .rs1_tag          (ren_rs1_tag),
      .rs1_gen          (ren_rs1_gen),
      .rs2_tag          (ren_rs2_tag),
      .rs2_gen          (ren_rs2_gen),
      .gen_valid        (ren_gen_valid),
      .rob_free_any     (rob_free_rob != {CORE_OCC_W{1'b0}}),
      .rob_alloc_valid  (rob_alloc_valid),
      .rob_alloc_tag    (rob_alloc_tag),
      .rob_alloc_pc     (rob_alloc_pc),
      .rob_alloc_num_uops(rob_alloc_num_uops),
      .rob_alloc_exc    (rob_alloc_exc),
      .rob_alloc_open   (rob_alloc_open),
      .rob_alloc_ok     (rob_alloc_ok),
      .rob_alloc_refused(rob_alloc_refused),
      .rob_alloc_index  (rob_alloc_index),
      .rob_alloc_gen    (rob_alloc_gen),
      .desc_wr_valid    (desc_wr_valid),
      .desc_wr_index    (desc_wr_index),
      .desc_wr_tag      (desc_wr_tag),
      .desc_wr_gen      (desc_wr_gen),
      .desc_wr_rd       (desc_wr_rd),
      .desc_wr_reg_we   (desc_wr_reg_we),
      .desc_wr_is_store (desc_wr_is_store),
      // ---------------------------------------------------------- memory insert
      .mem_ins_valid    (disp_mem_valid),
      .mem_ins_ready    (disp_mem_ready),
      .mem_ins_is_store (disp_mem_is_store),
      .mem_ins_id       (disp_mem_id),
      .mem_ins_base     (disp_mem_base),
      .mem_ins_imm      (disp_mem_imm),
      .mem_ins_size     (disp_mem_size),
      .mem_ins_signed   (disp_mem_signed),
      .mem_ins_data     (disp_mem_data),
      .mem_ins_dst_tag  (disp_mem_dst_tag),
      .mem_ins_dst_gen  (disp_mem_dst_gen),
      .mem_ins_dst_x0   (disp_mem_dst_x0),
      .rq_valid         (disp_rq_valid),
      .rq_tag           (disp_rq_tag),
      .rq_gen           (disp_rq_gen),
      .rq_written       (disp_rq_written),
      .prf_rd_valid     (prf_rd_valid),
      .prf_rd_tag       (prf_rd_tag),
      .prf_rd_gen       (prf_rd_gen),
      .prf_rsp_valid    (prf_rsp_valid),
      .prf_rsp_gen_mismatch (prf_rsp_bad),
      .prf_rsp_never_written(prf_rsp_never),
      .prf_rsp_data     (prf_rsp_data),
      .c0_ins_valid     (c0_ins_valid),
      .c0_ins_ready     (c0_ins_ready),
      .c0_ins_uop       (c0_ins_uop),
      .c0_ins_meta      (c0_ins_meta),
      .c0_ins_imm       (c0_ins_imm),
      .c0_ins_src1_tag  (c0_s1_tag),
      .c0_ins_src1_gen  (c0_s1_gen),
      .c0_ins_src1_ready(c0_s1_rdy),
      .c0_ins_src1_val  (c0_s1_val),
      .c0_ins_src2_tag  (c0_s2_tag),
      .c0_ins_src2_gen  (c0_s2_gen),
      .c0_ins_src2_ready(c0_s2_rdy),
      .c0_ins_src2_val  (c0_s2_val),
      .c0_ins_dst_tag   (c0_dst_tag),
      .c0_ins_dst_gen   (c0_dst_gen),
      .c1_ins_valid     (c1_ins_valid),
      .c1_ins_ready     (c1_ins_ready),
      .c1_ins_uop       (c1_ins_uop),
      .c1_ins_meta      (c1_ins_meta),
      .c1_ins_imm       (c1_ins_imm),
      .c1_ins_src1_tag  (c1_s1_tag),
      .c1_ins_src1_gen  (c1_s1_gen),
      .c1_ins_src1_ready(c1_s1_rdy),
      .c1_ins_src1_val  (c1_s1_val),
      .c1_ins_src2_tag  (c1_s2_tag),
      .c1_ins_src2_gen  (c1_s2_gen),
      .c1_ins_src2_ready(c1_s2_rdy),
      .c1_ins_src2_val  (c1_s2_val),
      .c1_ins_dst_tag   (c1_dst_tag),
      .c1_ins_dst_gen   (c1_dst_gen),
      .recovering       (recovering),
      .barrier          (br_inflight),
      .stop             (disp_unsupported),
      .o_take           (disp_take),
      .o_alloc_ctr      (disp_alloc_ctr),
      .o_ins_ctr        (disp_ins_ctr),
      .o_unsupported_ctr(o_unsupported_ctr),
      .o_illegal_ctr    (o_illegal_ctr),
      .o_exhausted_ctr  (),
      .o_squashed_ctr   (),
      .o_stall_ctr      (),
      .o_src_read_ctr   (),
      .o_src_conflict_ctr(),
      .o_src_bad_ctr    (),
      .o_rob_full_ctr   (),
      .o_queue_stall_ctr(),
      .o_queue_cnt      (),
      .o_queue_full     ()
  );

  assign core_stop = disp_unsupported;

  // ==========================================================================
  // 11. Redirect arbitration
  // ==========================================================================
  mosaic_redirect_arb u_redir (
      .clk             (clk),
      .rst             (rst),
      .req_valid       ({c1_redir_valid, c0_redir_valid}),
      .req_pc          ({c1_redir_pc, c0_redir_pc}),
      .req_rob_index   ({c1_redir_idx, c0_redir_idx}),
      .req_rob_gen     ({c1_redir_gen, c0_redir_gen}),
      .req_taken       ({c1_redir_taken, c0_redir_taken}),
      .req_ack         (redir_ack_vec),
      .head_valid      (rob_head_valid),
      .head_index      (rob_head_index),
      .head_gen        (rob_head_gen),
      .head_occupied   (rob_occupied),
      .head_retire     (rob_retire_ack),
      // The second retire lane. A branch that reaches the head in the cycle the
      // entry in front of it retires leaves in lane 1, and without this view the
      // request would wait for a lane-0 head that never comes -- the ROB retires
      // both entries in that cycle and is empty afterwards. See
      // mosaic_redirect_arb.sv's head-view note.
      .head1_valid     (rob_head1_valid),
      .head1_index     (rob_head1_index),
      .head1_gen       (rob_head1_gen),
      .head1_retire    (rob_retire_ack_next),
      .redirect_valid  (redirect_valid),
      .redirect_pc     (redirect_pc),
      .o_act_valid     (redir_act_valid),
      .o_act_taken     (redir_act_taken),
      .o_req_ctr       (),
      .o_act_ctr       (o_redir_act_ctr),
      .o_drop_ctr      (),
      .o_dead_ctr      (o_redir_dead_ctr),
      .o_wait_ctr      (o_redir_wait_ctr),
      .o_nothing_ctr   ()
  );

  // ==========================================================================
  // 12. Flush and recovery control
  // ==========================================================================
  assign rob_flush_pulse      = redirect_valid;
  assign cluster_flush_pulse  = redirect_valid;

  always_ff @(posedge clk) begin
    if (rst) begin
      recovering <= 1'b0;
    end else if (redirect_valid) begin
      recovering <= 1'b1;
    end else if (recovering && !c0_flush_busy && !c1_flush_busy) begin
      recovering <= 1'b0;
    end
  end

  // ------------------------------------------------------- the rename recovery
  // A redirect retires the branch, and `mosaic_retire` publishes its commit in
  // that same cycle, so the cycle after a redirect the branch's mapping is in
  // the committed map. Under the barrier nothing younger than the branch ever
  // allocated, so speculative == committed holds there -- and that is the only
  // cycle in which a checkpoint `mosaic_rename` will accept a squash to
  // (`ckpt_at_boundary` is sampled from `ckpt_committed` at the checkpoint, once,
  // and held). So:
  //
  //   * the checkpoint is taken on the redirect pulse;
  //   * the squash follows one cycle later, so the commit that funded the
  //     checkpoint cannot land in the same cycle as the restore that reads the
  //     committed map.
  //
  // `MOSAIC_CORE_MUTANT_EARLY_CKPT` takes the checkpoint one cycle earlier, in
  // the act cycle -- before the branch's own commit has landed -- which is
  // exactly the off-boundary checkpoint rename refuses. See the negative-control
  // note at the site.
`ifdef MOSAIC_CORE_MUTANT_EARLY_CKPT
  // NEGATIVE CONTROL: the checkpoint is taken in the cycle the arbiter acts,
  // before the redirecting branch's commit has been applied to the committed
  // map. `spec == cmt` does not hold there for a link-writing JAL/JALR, so
  // rename refuses the squash with `squash_not_committed` -- the defect is a
  // checkpoint taken off the boundary, and the case's recovery counters catch
  // it. CASE=core.corpus_branch must fail on it.
  assign ren_ckpt_valid = redir_act_valid;
  assign ren_squash     = redirect_valid;
`else
  assign ren_ckpt_valid = redirect_valid;
  assign ren_squash     = redirect_delay_q;
`endif

  always_ff @(posedge clk) begin
    if (rst) begin
      redirect_delay_q <= 1'b0;
    end else begin
      redirect_delay_q <= redirect_valid;
    end
  end

  // ------------------------------------------------------- the branch barrier
  // Set when a branch is allocated, released when the arbiter has accounted for
  // it (not taken: no flush at all) or when its redirect has been applied.
  assign alloc_is_branch_macro = dbuf_ctl[0].is_branch || dbuf_ctl[0].is_jal ||
                                 dbuf_ctl[0].is_jalr;

  always_ff @(posedge clk) begin
    if (rst) begin
      br_inflight <= 1'b0;
    end else if (redirect_valid) begin
      br_inflight <= 1'b0;
    end else if (redir_act_valid && !redir_act_taken) begin
      br_inflight <= 1'b0;
`ifdef MOSAIC_CORE_MUTANT_EARLY_BARRIER_RELEASE
      // NEGATIVE CONTROL: the barrier is released when the branch *resolves*,
      // not when the arbiter has acted on the resolution. Younger work then
      // reaches the clusters while the redirect is still waiting for the branch
      // to become the retiring head, and the recovery's precondition -- the
      // speculative map equals the committed map at the checkpoint -- no longer
      // holds, so rename refuses the squash and the case's recovery counters
      // catch it. It is the control for the barrier being load-bearing rather
      // than decorative; see results/reports/I-023-branches.md.
    end else if (c0_redir_valid || c1_redir_valid) begin
      br_inflight <= 1'b0;
`endif
    end else if (dbuf_valid[0] && alloc_is_branch_macro && !recovering &&
                 !core_stop && (ren_alloc_accepted != 1'b0) &&
                 (rob_free_rob != {CORE_OCC_W{1'b0}})) begin
      br_inflight <= 1'b1;
    end
  end

  // ==========================================================================
  // 13. Retire
  // ==========================================================================
  assign retire_pay_value = {stash_value1, stash_value0};

  // The retire module carries one commit lane per retired instruction. rename
  // has one commit port per lane, applied in program order, which is what makes
  // two commits to one architectural register in one cycle install the younger
  // mapping and release the older tag.
  assign ren_commit_valid  = ret_commit_valid[0];
  assign ren_commit_rd     = ret_commit_rd[CORE_RD_W-1:0];
  assign ren_commit_tag    = ret_commit_tag[CORE_TAG_W-1:0];
  // The committed map is keyed on the *tag generation* rename allocates with
  // (`spec_gen[a] <= alloc_new_gen`), so a commit must install that same
  // generation. The generation mosaic_retire derives from `rob_id` is the ROB's
  // own entry generation -- a different counter that happens to be the same
  // width -- and feeding it here made `speculative map == committed map` false
  // from the first commit on, which is the boundary every redirect and every
  // future checkpoint depends on. The destination identity (tag *and* its
  // generation) is what the descriptor store carries, so that is the source.
  // CASE=fabric.fixed_two_cluster asserts the boundary and is what caught it.
`ifdef MOSAIC_CORE_MUTANT_ROB_GEN_COMMIT
  // NEGATIVE CONTROL for the fix: the committed map is fed the ROB entry
  // generation, which is the wiring this package shipped with. The two fields
  // have the same width, so nothing about the build says they are different
  // counters; the case's rename-boundary comparison is what does.
  assign ren_commit_gen    = rob_head_gen;
  assign ren_commit2_gen   = rob_head1_gen;
`else
  assign ren_commit_gen    = desc_gen0[CORE_IGEN_W-1:0];
  assign ren_commit2_gen   = desc_gen1[CORE_IGEN_W-1:0];
`endif
  assign ren_commit2_valid = ret_commit_valid[1];
  assign ren_commit2_rd    = ret_commit_rd[2*CORE_RD_W-1:CORE_RD_W];
  assign ren_commit2_tag   = ret_commit_tag[2*CORE_TAG_W-1:CORE_TAG_W];

  // Lane 1 must not retire when the head is a taken branch whose redirect is
  // pending: that entry is the first wrong-path instruction and the redirect is
  // about to discard it. A not-taken branch falls through, so it gates nothing.
  always_comb begin
    head_pending_taken =
        (c0_redir_valid && c0_redir_taken &&
         (c0_redir_idx == rob_head_index) && (c0_redir_gen == rob_head_gen)) ||
        (c1_redir_valid && c1_redir_taken &&
         (c1_redir_idx == rob_head_index) && (c1_redir_gen == rob_head_gen));
  end
  assign rob_retire_req_next = ret_req[1] && !head_pending_taken;

  always_comb begin
    retire_clr_valid[0] = rob_retire_ack;
    retire_clr_valid[1] = rob_retire_ack_next;
    retire_clr_index[0] = rob_head_index;
    retire_clr_index[1] = rob_head1_index;
  end

  mosaic_retire u_retire (
      .clk            (clk),
      .rst            (rst),
      .rob_valid      ({rob_head1_valid, rob_head_valid}),
      .rob_ready      ({rob_head1_ready, rob_head_ready}),
      .rob_exc        ({rob_head1_exc, rob_head_exc}),
      .rob_ack        ({rob_retire_ack_next, rob_retire_ack}),
      .rob_id         ({rob_head1_gen, rob_head1_tag, rob_head_gen, rob_head_tag}),
      .rob_pc         ({rob_head1_pc, rob_head_pc}),
      .pay_valid      ({rob_head1_valid, rob_head_valid}),
      .pay_reg_we     ({desc_reg_we1, desc_reg_we0}),
      .pay_rd         ({desc_rd1, desc_rd0}),
      .pay_value      (retire_pay_value),
      .pay_csr_we     ({CORE_RET_N{1'b0}}),
      .pay_csr_addr   ({(CORE_RET_N*CORE_CSR_W){1'b0}}),
      .pay_csr_value  ({(CORE_RET_N*CORE_XLEN){1'b0}}),
      .pay_is_store   ({CORE_RET_N{1'b0}}),
      .pay_store_addr ({(CORE_RET_N*CORE_XLEN){1'b0}}),
      .pay_store_data ({(CORE_RET_N*CORE_XLEN){1'b0}}),
      .pay_store_size ({(CORE_RET_N*CORE_SIZE_W){1'b0}}),
      .pay_exc_cause  ({(CORE_RET_N*CORE_XLEN){1'b0}}),
      .pay_exc_tval   ({(CORE_RET_N*CORE_XLEN){1'b0}}),
      .flush_valid    (rob_flush_pulse),
      .retire_req     (ret_req),
      .trap_flush     (),
      .ev_valid       (ev_valid),
      .ev_trap        (ev_trap),
      .ev_seq         (ev_seq),
      .ev_pc          (ev_pc),
      .ev_id          (ev_id),
      .ev_reg_we      (ev_reg_we),
      .ev_rd          (ev_rd),
      .ev_value       (ev_value),
      .ev_csr_we      (),
      .ev_csr_addr    (),
      .ev_csr_value   (),
      .ev_store       (ev_store),
      .ev_store_addr  (ev_store_addr),
      .ev_store_data  (ev_store_data),
      .ev_store_size  (ev_store_size),
      .ev_trap_cause  (),
      .ev_trap_tval   (),
      .trap_valid     (),
      .trap_pc        (),
      .trap_cause     (),
      .trap_tval      (),
      .commit_valid   (ret_commit_valid),
      .commit_rd      (ret_commit_rd),
      .commit_tag     (ret_commit_tag),
      // The retire module's own commit generation is derived from `rob_id` and
      // is the ROB entry generation; the committed map is keyed on the tag
      // generation, which comes from the descriptor store above. Left
      // unconnected deliberately, with the reason here rather than silently
      // rewired.
      .commit_gen     (),
      .csr_rd_valid   (1'b0),
      .csr_rd_addr    ({CORE_CSR_W{1'b0}}),
      .csr_rd_data    (),
      .csr_rd_unsupported (),
      .o_retire_seq   (),
      .o_minstret     (),
      .o_mcycle       (),
      .o_exc_queued   (),
      .o_mscratch     (),
      .o_event_count  (),
      .o_x0_retired   (),
      .o_pay_missing  (),
      .o_csr_unsupported (),
      .o_order_fault  ()
  );

  // ==========================================================================
  // 14. The memory path (I-033..I-038) at the integration boundary
  // ==========================================================================
  //
  //     dispatch --- allocate ---> load queue ---+
  //                   |                          +--> mosaic_lsu_endpoint --> dmem
  //                   +- allocate ---> store queue+
  //                                        ^
  //     ROB retire -- commit(1,2) ----------+   (authorisation)
  //     redirect ---- squash_all -----------+   (both queues)
  //
  // A load is allocated into the load queue when its base operand is readable; a
  // store is allocated into the store queue when its base *and* payload are, and
  // the store's ROB completion is offered in that same cycle, because a store's
  // work is done the moment its address and data are known -- what remains is an
  // authorisation, not an execution.
  //
  // Both queues share the endpoint's single upstream port. The store drain has
  // priority: it is the side-effecting path and its entries cannot be released
  // any other way (a load that waits costs a cycle, a store that waits costs
  // capacity, and capacity is what the front end stalls on). Neither can starve
  // the other: a store is only ever offered after it has retired, and a load in
  // front of it in the endpoint's queue is served as soon as the drain in
  // progress completes.
  //
  // The endpoint owns the fault boundary and computes `base + imm` once, in one
  // adder, so the strobes, the misalignment test and the reported `tval` cannot
  // disagree about which bytes the access owns.
  assign ep_req_valid = sq_drain_valid | lq_req_valid;
  assign ep_req       = sq_drain_valid ? sq_drain_req : lq_req;
  assign sq_drain_ready = ep_req_ready;
  assign lq_req_ready   = ep_req_ready && !sq_drain_valid;

  // Which queue the endpoint's response belongs to. The endpoint holds one
  // transaction and refuses a second request until it has returned that
  // transaction's response (`accept_c` requires ST_IDLE), so the owner recorded
  // at acceptance is exact and a response can never be delivered to the wrong
  // queue.
  always_ff @(posedge clk) begin
    if (rst) begin
      ep_owner_q <= 1'b0;
    end else if (ep_req_valid && ep_req_ready) begin
      ep_owner_q <= sq_drain_valid;
    end
  end
  assign lq_rsp_valid = ep_rsp_valid && !ep_owner_q;
  assign sq_rsp_valid = ep_rsp_valid &&  ep_owner_q;
  assign ep_rsp_ready = ep_owner_q ? sq_rsp_ready : lq_rsp_ready;

  mosaic_lsu_endpoint u_lsu (
      .clk                  (clk),
      .rst                  (rst),
      .req_valid_i          (ep_req_valid),
      .req_ready_o          (ep_req_ready),
      .req_i                (ep_req),
      .rsp_valid_o          (ep_rsp_valid),
      .rsp_ready_o          (ep_rsp_ready),
      .rsp_o                (ep_rsp),
      .mem_req_valid_o      (dmem_req_valid),
      .mem_req_ready_i      (dmem_req_ready),
      .mem_req_o            (dmem_req),
      .mem_rsp_valid_i      (dmem_rsp_valid),
      .mem_rsp_ready_o      (dmem_rsp_ready),
      .mem_rsp_i            (dmem_rsp),
      .o_busy               (),
      .o_load_ctr           (),
      .o_store_ctr          (),
      .o_txn_ctr            (lsu_txn_ctr),
      .o_misaligned_ctr     (lsu_misaligned_ctr),
      .o_access_fault_ctr   (lsu_access_fault_ctr),
      .o_rsp_ctr            (),
      .o_last_fault_cause   (),
      .o_last_fault_tval    (),
      .o_inflight_addr      (),
      .o_inflight_size      ()
  );

  mosaic_load_queue u_lq (
      .clk                  (clk),
      .rst                  (rst),
      .alloc_valid_i        (lq_alloc_valid),
      .alloc_ready_o        (lq_alloc_ready),
      .alloc_id_i           (disp_mem_full_id),
      .alloc_base_i         (disp_mem_base),
      .alloc_imm_i          (disp_mem_imm),
      .alloc_size_i         (disp_mem_size),
      .alloc_signed_i       (disp_mem_signed),
      .alloc_dst_tag_i      (disp_mem_dst_tag),
      .alloc_dst_gen_i      (disp_mem_dst_gen),
      .alloc_dst_x0_i       (disp_mem_dst_x0),
      .flush_valid_i        (lq_flush),
      .sq_entry_pay_i       (sq_entry_pay),
      .sq_count_i           (sq_count),
      .sq_query_addr_o      (lq_query_addr),
      .sq_query_size_o      (lq_query_size),
      .sq_query_valid_i     (lq_query_valid),
      .sq_query_data_i      (lq_query_data),
      .sq_query_blocked_i   (lq_query_blocked),
      .req_valid_o          (lq_req_valid),
      .req_ready_i          (lq_req_ready),
      .req_o                (lq_req),
      .rsp_valid_i          (lq_rsp_valid),
      .rsp_ready_o          (lq_rsp_ready),
      .rsp_i                (ep_rsp),
      .result_valid_o       (lq_result_valid),
      .result_ready_i       (lq_result_ready),
      .result_id_o          (lq_result_id),
      .result_dst_tag_o     (lq_result_dst_tag),
      .result_dst_gen_o     (lq_result_dst_gen),
      .result_dst_x0_o      (lq_result_dst_x0),
      .result_data_o        (lq_result_data),
      .result_cause_o       (lq_result_cause),
      .result_tval_o        (lq_result_tval),
      .result_fault_o       (lq_result_fault),
      .result_fwd_mask_o    (),
      .o_count              (lq_count),
      .o_occ                (),
      .o_alloc_ctr          (lq_alloc_ctr),
      .o_issue_ctr          (lq_issue_ctr),
      .o_rsp_ctr            (),
      .o_done_ctr           (lq_done_ctr),
      .o_replay_ctr         (lq_replay_ctr),
      .o_blocked_ctr        (lq_blocked_ctr),
      .o_fwd_byte_ctr       (lq_fwd_byte_ctr),
      .o_mem_byte_ctr       (lq_mem_byte_ctr),
      .o_fault_ctr          (lq_fault_ctr),
      .o_query_mismatch_ctr (lq_mismatch_ctr),
      .o_last_fault_cause   (),
      .o_last_fault_tval    (),
      .o_entry_pay          ()
  );

  // One access class gets the wrong size: the store queue is always told the
  // access is a word, whatever the instruction asked for. The strobe mask and
  // the byte count then disagree with the ISA for every byte, half and double
  // store, and the bytes the program publishes differ from the reference's --
  // which is what makes this a control for "the size of one access class is
  // wrong" rather than a control for the drain.
`ifdef MOSAIC_CORE_MUTANT_STORE_SIZE_WORD
  assign sq_alloc_size = mosaic_pkg::SZ_WORD;
`else
  assign sq_alloc_size = disp_mem_size;
`endif

  mosaic_store_queue u_sq (
      .clk                  (clk),
      .rst                  (rst),
      // A store's operands are read for real before it is allocated (dispatch
      // holds the macro until its base and payload are readable), so both
      // readiness bits are set at allocation and the late-fill port is unused.
      // That is a design choice with a price -- the front end waits instead of
      // allocating early -- and it is why the fill path is not exercised here;
      // the store queue's own case covers it.
      .alloc_valid_i        (sq_alloc_valid),
      .alloc_ready_o        (sq_alloc_ready),
      .alloc_id_i           (disp_mem_full_id),
      .alloc_base_i         (disp_mem_base),
      .alloc_imm_i          (disp_mem_imm),
      .alloc_size_i         (sq_alloc_size),
      .alloc_addr_valid_i   (1'b1),
      .alloc_data_i         (disp_mem_data),
      .alloc_data_valid_i   (1'b1),
      .fill_valid_i         (1'b0),
      .fill_id_i            ({CORE_MEM_ID_W{1'b0}}),
      .fill_base_i          ({CORE_XLEN{1'b0}}),
      .fill_imm_i           ({CORE_XLEN{1'b0}}),
      .fill_addr_valid_i    (1'b0),
      .fill_data_i          ({CORE_XLEN{1'b0}}),
      .fill_data_valid_i    (1'b0),
      .fill_hit_o           (),
      .fill_stale_o         (),
      .commit_valid_i       (sq_commit_valid),
      .commit_id_i          (sq_commit_id),
      .commit_ok_o          (),
      .commit_stale_o       (),
      .commit2_valid_i      (sq_commit2_valid),
      .commit2_id_i         (sq_commit2_id),
      .commit2_ok_o         (sq_commit2_ok),
      .commit2_stale_o      (),
      .squash_valid_i       (sq_squash_valid),
      .squash_all_i         (sq_squash_all),
      .squash_from_index_i  (sq_squash_from),
      .squash_tail_index_i  (sq_squash_tail),
      .squash_gen_i         (sq_squash_gen),
      .drain_req_valid_o    (sq_drain_valid),
      .drain_req_ready_i    (sq_drain_ready),
      .drain_req_o          (sq_drain_req),
      .drain_rsp_valid_i    (sq_rsp_valid),
      .drain_rsp_ready_o    (sq_rsp_ready),
      .drain_rsp_i          (ep_rsp),
      .fwd_query_addr_i     (lq_query_addr),
      .fwd_query_size_i     (lq_query_size),
      .fwd_valid_o          (lq_query_valid),
      .fwd_data_o           (lq_query_data),
      .fwd_blocked_o        (lq_query_blocked),
      .o_count              (sq_count),
      .o_auth_cnt           (sq_auth_cnt),
      .o_occ                (),
      .o_entry_authorised   (),
      .o_alloc_ctr          (sq_alloc_ctr),
      .o_fill_ctr           (),
      .o_fill_stale_ctr     (),
      .o_commit_ctr         (sq_commit_ctr),
      .o_commit_stale_ctr   (sq_commit_stale_ctr),
      .o_drain_ctr          (sq_drain_ctr),
      .o_fault_ctr          (sq_fault_ctr),
      .o_squash_ctr         (sq_squash_ctr),
      .o_squash_spared_ctr  (sq_spared_ctr),
      .o_last_fault_tval    (),
      .o_last_fault_cause   (),
      .o_last_fault_id      (),
      .o_entry_pay          (sq_entry_pay)
  );

  // ==========================================================================
  // 14a. The memory path's control: allocation, completion and authorisation
  // ==========================================================================
  // A load needs only its queue to have room. A store needs its queue to have
  // room *and* the completion path to be able to take its completion in the very
  // same cycle: the store's completion is offered as it is allocated and it has
  // no second one. A load result waiting in the writeback port takes precedence
  // over a store insert, because the load has already reached memory and its
  // completion is older work; the store simply waits a cycle, which costs
  // nothing but a stall the call site counts.
  // The insert bus carries the same identity width the cluster insert bus does
  // (the uop id without the hart field); p0 has one hart, so the full identity
  // is that field zero-extended -- the same rule every other producer in this
  // file uses.
  assign disp_mem_full_id = {1'b0, disp_mem_id};

  assign disp_mem_ready = disp_mem_is_store
      ? (sq_alloc_ready && lsu_wb_ready && !lq_result_valid)
      : lq_alloc_ready;

  assign lq_alloc_valid  = disp_mem_valid && !disp_mem_is_store && disp_mem_ready;
  assign sq_alloc_valid  = disp_mem_valid &&  disp_mem_is_store && disp_mem_ready;
  assign store_cmp_valid = disp_mem_valid &&  disp_mem_is_store && disp_mem_ready;

  // A store is complete when both its operands are captured: no register value,
  // no exception, just "this uop is done". Its identity is the ROB identity of
  // the macro, which is what the ROB's completion port matches on.
  always_comb begin
    store_wb_ev.id          = disp_mem_full_id;
    store_wb_ev.dst.tag     = {CORE_TAG_W{1'b0}};
    store_wb_ev.dst.gen     = {CORE_PGEN_W{1'b0}};
    store_wb_ev.dst.x0      = 1'b1;
    store_wb_ev.value_valid = 1'b0;
    store_wb_ev.value       = {CORE_XLEN{1'b0}};
    store_wb_ev.exc.valid   = 1'b0;
    store_wb_ev.exc.cause   = {CORE_XLEN{1'b0}};
    store_wb_ev.exc.tval    = {CORE_XLEN{1'b0}};
    store_wb_ev.is_store    = 1'b1;
    store_wb_ev.is_load     = 1'b0;
  end

  // A load's completion carries the merged, sign-extended value the load queue
  // produced and the destination the entry carried with it.
  always_comb begin
    lq_wb_ev.id          = lq_result_id;
    lq_wb_ev.dst.tag     = lq_result_dst_tag;
    lq_wb_ev.dst.gen     = {{(CORE_PGEN_W - CORE_IGEN_W){1'b0}}, lq_result_dst_gen};
    lq_wb_ev.dst.x0      = lq_result_dst_x0;
    lq_wb_ev.value_valid = !lq_result_dst_x0;
    lq_wb_ev.value       = lq_result_data;
    lq_wb_ev.exc.valid   = lq_result_fault;
    lq_wb_ev.exc.cause   = lq_result_cause;
    lq_wb_ev.exc.tval    = lq_result_tval;
    lq_wb_ev.is_store    = 1'b0;
    lq_wb_ev.is_load     = 1'b1;
  end

  assign lsu_wb_valid      = store_cmp_valid | lq_result_valid;
  assign lsu_wb_ev         = store_cmp_valid ? store_wb_ev : lq_wb_ev;
  assign lq_result_ready   = lsu_wb_ready && !store_cmp_valid;

  // ---------------------------------------------- authorisation and squash
  // The ROB retires at most two instructions per cycle and both lanes can be
  // stores, so both authorisation ports are driven from the lanes that actually
  // retired in this cycle. `uop_index` is zero because this core allocates one
  // uop per macro (mosaic_dispatch).
`ifndef MOSAIC_CORE_MUTANT_STORE_PRECOMMIT
  assign sq_commit_valid  = rob_retire_ack      && desc_is_store0;
  assign sq_commit_id     = {1'b0, rob_head_index,  rob_head_gen,  {CORE_UOP_W{1'b0}}};
  assign sq_commit2_valid = rob_retire_ack_next && desc_is_store1 && rob_retire_ack;
  assign sq_commit2_id    = {1'b0, rob_head1_index, rob_head1_gen, {CORE_UOP_W{1'b0}}};
`else
  // NEGATIVE CONTROL: a store is also authorised the moment it is *allocated*,
  // whenever the queue holds nothing else unauthorised -- the tempting "its
  // address and data are ready, so it can go now". The retirement authorisation
  // is kept as well, so the machine still finishes; what changes is that the
  // store reaches the endpoint before the instruction that owns it has retired.
  // CASE=core.mem_program's "no store reaches memory before its instruction
  // retires" check names it; the final memory image is unchanged, which is
  // exactly why an end-state-only comparison would not catch it.
  // The identity of the store at the authorisation watermark -- the first
  // unauthorised resident entry -- read out of the store queue's exported entry
  // view. Authorising *that* entry is what the retire path does; the mutant
  // drives it every cycle instead of waiting for the ROB.
  logic precommit_c;
  logic [CORE_MEM_ID_W-1:0] sq_first_unauth_id;
  assign sq_first_unauth_id =
      sq_entry_pay[32'(sq_auth_cnt) * CORE_SQ_ENTRY_W + CORE_SQ_OFF_ID +: CORE_MEM_ID_W];
  assign precommit_c      = (sq_count != sq_auth_cnt);
  assign sq_commit_valid  = (rob_retire_ack && desc_is_store0) || precommit_c;
  assign sq_commit_id     = precommit_c
                            ? sq_first_unauth_id
                            : {1'b0, rob_head_index, rob_head_gen, {CORE_UOP_W{1'b0}}};
  assign sq_commit2_valid = rob_retire_ack_next && desc_is_store1 && rob_retire_ack;
  assign sq_commit2_id    = {1'b0, rob_head1_index, rob_head1_gen, {CORE_UOP_W{1'b0}}};
`endif

  // A redirect withdraws everything the recovery decided is dead. The whole
  // queue is the correct region here and is what the store queue is built for:
  // an authorised store that has not drained is *spared* by the watermark rule,
  // and with both retire lanes authorising in their own cycle there is no
  // retired-but-unauthorised store for a whole-queue flush to lose. The region
  // inputs are driven with the redirecting branch's own identity (it is the ROB
  // head in the cycle the redirect is issued) rather than left dangling, so the
  // narrower rule is a one-line change the day the conservative recovery is
  // replaced and younger work really does sit in these queues.
  assign sq_squash_valid = redirect_valid;
  assign sq_squash_all   = 1'b1;
  assign sq_squash_from  = rob_head_index;
  assign sq_squash_tail  = rob_alloc_ptr;
  assign sq_squash_gen   = rob_head_gen;
  assign lq_flush        = redirect_valid;

  // A memory insert that was offered and not taken. Counted, because a stall
  // here is the only thing that can hold a memory macro in dispatch and the
  // number is the evidence that the back-pressure path was exercised.
  always_ff @(posedge clk) begin
    if (rst) begin
      mem_ins_stall_ctr <= 32'd0;
    end else if (disp_mem_valid && !disp_mem_ready) begin
      mem_ins_stall_ctr <= mem_ins_stall_ctr + 32'd1;
    end
  end

  assign o_mem_lq_alloc       = lq_alloc_ctr;
  assign o_mem_lq_issue       = lq_issue_ctr;
  assign o_mem_lq_done        = lq_done_ctr;
  assign o_mem_lq_replay      = lq_replay_ctr;
  assign o_mem_lq_blocked     = lq_blocked_ctr;
  assign o_mem_lq_fwd_bytes   = lq_fwd_byte_ctr;
  assign o_mem_lq_mem_bytes   = lq_mem_byte_ctr;
  assign o_mem_lq_fault       = lq_fault_ctr;
  assign o_mem_lq_query_mismatch = lq_mismatch_ctr;
  assign o_mem_lq_occupied    = 32'(lq_count);
  assign o_mem_sq_alloc       = sq_alloc_ctr;
  assign o_mem_sq_commit      = sq_commit_ctr;
  assign o_mem_sq_commit2     = sq_commit2_ctr;
  assign o_mem_sq_commit_stale= sq_commit_stale_ctr;
  assign o_mem_sq_drain       = sq_drain_ctr;
  assign o_mem_sq_squash      = sq_squash_ctr;
  assign o_mem_sq_spared      = sq_spared_ctr;
  assign o_mem_sq_occupied    = 32'(sq_count);
  assign o_mem_sq_auth        = 32'(sq_auth_cnt);
  assign o_mem_sq_fault       = sq_fault_ctr;
  assign o_mem_lsu_txn        = lsu_txn_ctr;
  assign o_mem_lsu_misaligned = lsu_misaligned_ctr;
  assign o_mem_lsu_access_fault = lsu_access_fault_ctr;
  assign o_mem_ins_stall      = mem_ins_stall_ctr;
  assign o_mem_squash_valid   = sq_squash_valid;

  // How often both retirement lanes were stores in one cycle -- the case that
  // needs the store queue's second authorisation port.
  always_ff @(posedge clk) begin
    if (rst) begin
      sq_commit2_ctr <= 32'd0;
    end else if (sq_commit2_ok) begin
      sq_commit2_ctr <= sq_commit2_ctr + 32'd1;
    end
  end

  // ==========================================================================
  // 15. Core-level evidence
  // ==========================================================================

  assign o_c0_count     = c0_count;
  assign o_c1_count     = c1_count;
  assign o_c0_grant_valid = c0_grant_valid;
  assign o_c1_grant_valid = c1_grant_valid;
  assign o_c0_grant_uop = c0_grant_uop;
  assign o_c1_grant_uop = c1_grant_uop;
  assign o_c0_alu_ctr   = c0_alu_ctr;
  assign o_c1_alu_ctr   = c1_alu_ctr;
  assign o_c0_branch_ctr= c0_br_ctr;
  assign o_c1_branch_ctr= c1_br_ctr;
  assign o_muldiv_ctr   = md_ctr;
  assign o_free_count   = ren_free_count;
  // rename's own boundary report: `speculative map == committed map`. The
  // barrier recovery claims this holds whenever the machine has no unretired
  // register-writer, and in particular at every redirect; the case asserts it
  // there rather than taking the claim on faith.
  assign o_rename_boundary = ren_ckpt_committed;
  assign o_redirect_valid  = redirect_valid;
  assign o_redirect_pc     = redirect_pc;
  assign o_fetch_pc        = fetch_pc_q;
  assign o_squash_acc_ctr  = squash_acc_ctr;
  assign o_ckpt_ctr        = ckpt_ctr;

  always_comb begin
    o_dbg_redir_bundle                 = {CORE_XLEN{1'b0}};
    o_dbg_redir_bundle[0]              = c0_redir_valid;
    o_dbg_redir_bundle[1]              = c1_redir_valid;
    o_dbg_redir_bundle[2]              = c0_redir_taken;
    o_dbg_redir_bundle[3]              = c1_redir_taken;
    o_dbg_redir_bundle[4]              = 1'b0;
    o_dbg_redir_bundle[5]              = 1'b0;
    o_dbg_redir_bundle[6]              = 1'b0;
    o_dbg_redir_bundle[12:7]           = c0_redir_idx;
    o_dbg_redir_bundle[19:13]          = c0_redir_gen;
    o_dbg_redir_bundle[20]             = 1'b0;
    o_dbg_redir_bundle[27:22]          = c1_redir_idx;
    o_dbg_redir_bundle[34:28]          = c1_redir_gen;
    o_dbg_redir_bundle[35]             = rob_head_valid;
    o_dbg_redir_bundle[36]             = rob_retire_ack;
    o_dbg_redir_bundle[42:37]          = rob_head_index;
    o_dbg_redir_bundle[49:43]          = rob_head_gen;
    o_dbg_redir_bundle[50]             = redir_act_valid;
    o_dbg_redir_bundle[51]             = redir_act_taken;
    o_dbg_redir_bundle[52]             = br_inflight;
    o_dbg_redir_bundle[53]             = recovering;
    o_dbg_redir_bundle[60:54]          = rob_occupied;
    o_dbg_redir_bundle[63:61]          = 3'b000;
  end
  assign o_dbg_fetch_state = fetch_dbg_state;

  assign o_squash_underflow_ctr = squash_under_ctr;
  assign o_squash_not_committed_ctr = squash_nc_ctr;
  assign o_journal_overflow_ctr = journal_ovf_ctr;
  assign o_rob_occupied = 32'(rob_occupied);
  assign o_rob_free     = 32'(rob_free_rob);
  assign o_desc_live    = desc_live_ctr;
  assign o_stopped      = core_stop;
  assign o_wb_wr_ctr    = wb_wr_ctr;
  assign o_wb_wake_ctr  = wb_wake_ctr;
  assign o_wb_stale_ctr = wb_stale_ctr;
  assign o_wb_dup_ctr   = wb_dup_ctr;
  assign o_wb_collision_ctr = wb_collision_ctr;
  assign o_wb_drop_ctr  = wb_drop_ctr;

  always_ff @(posedge clk) begin
    if (rst) begin
      commit_ctr     <= 32'd0;
      redirect_ctr   <= 32'd0;
      recovering_ctr <= 32'd0;
      stop_ctr       <= 32'd0;
      cycle_ctr      <= 32'd0;
      squash_under_ctr <= 32'd0;
      squash_nc_ctr    <= 32'd0;
      journal_ovf_ctr  <= 32'd0;
      squash_acc_ctr   <= 32'd0;
      ckpt_ctr         <= 32'd0;
      core_stop_prev   <= 1'b0;
    end else begin
      cycle_ctr      <= cycle_ctr + 32'd1;
      commit_ctr     <= commit_ctr + {31'd0, rob_retire_ack} +
                                   {31'd0, rob_retire_ack_next};
      redirect_ctr   <= redirect_ctr + {31'd0, redirect_valid};
      recovering_ctr <= recovering_ctr + {31'd0, recovering};
      stop_ctr       <= stop_ctr + {31'd0, core_stop && !core_stop_prev};
      squash_under_ctr <= squash_under_ctr + {31'd0, ren_squash_underflow};
      squash_nc_ctr    <= squash_nc_ctr + {31'd0, ren_squash_not_committed};
      journal_ovf_ctr  <= journal_ovf_ctr + {31'd0, ren_journal_overflow};
      squash_acc_ctr   <= squash_acc_ctr + {31'd0, ren_squash_accepted};
      ckpt_ctr         <= ckpt_ctr + {31'd0, ren_ckpt_valid};
      core_stop_prev   <= core_stop;
    end
  end

  assign o_dbg_deliver_valid = fetch_out_valid;
  assign o_dbg_deliver_pc    = fetch_out_pc;
  assign o_dbg_deliver_bits  = fetch_out_bits;
  assign o_dbg_head_valid    = rob_head_valid;
  assign o_dbg_head_complete = rob_head_complete;
  assign o_dbg_head_index    = rob_head_index;
  assign o_dbg_head_pc       = rob_head_pc;
  assign o_dbg_desc_rd0      = desc_rd0;
  assign o_dbg_desc_rd1      = desc_rd1;
  assign o_dbg_alloc_ctr     = disp_alloc_ctr;
  assign o_dbg_ins_ctr       = disp_ins_ctr;
  // The readiness state the stall diagnosis reads: rename's map, its per-tag
  // allocation validity (the same vector dispatch folds from) and its per-tag
  // writeback-done bits.
  assign o_dbg_gen_valid     = ren_gen_valid;

  assign o_commit_ctr    = commit_ctr;
  assign o_redirect_ctr  = redirect_ctr;
  assign o_recovering_ctr= recovering_ctr;
  assign o_stop_ctr      = stop_ctr;
  assign o_cycle_ctr     = cycle_ctr;

  /* verilator lint_on PINCONNECTEMPTY */

endmodule : mosaic_core

`default_nettype wire
