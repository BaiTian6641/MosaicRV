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
// The fetch unit's `outstanding_count` is one bit wider than an index because it
// must be able to name "full" as well as 0..CORE_FETCH_N-1.
localparam int unsigned CORE_FETCH_CNT_W = $clog2(CORE_FETCH_N + 1);
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
// The event payload needs the store's own address, data and size as well as its
// identity, so every field of the view is named here and the two readers -- the
// payload bus below and the pre-commit negative control -- share one definition
// of the layout rather than two formulas that could drift.
localparam int unsigned CORE_SQ_OFF_DATA_VALID = 0;
localparam int unsigned CORE_SQ_OFF_ADDR_VALID = 1;
localparam int unsigned CORE_SQ_OFF_DATA       = 2;
localparam int unsigned CORE_SQ_OFF_SIZE       = CORE_SQ_OFF_DATA + CORE_XLEN;
localparam int unsigned CORE_SQ_OFF_IMM        = CORE_SQ_OFF_SIZE + 3;
localparam int unsigned CORE_SQ_OFF_BASE       = CORE_SQ_OFF_IMM + CORE_XLEN;
localparam int unsigned CORE_SQ_OFF_ID         = CORE_SQ_OFF_BASE + CORE_XLEN;
localparam int unsigned CORE_SQ_ENTRY_W        = CORE_SQ_OFF_ID + CORE_MEM_ID_W;

module mosaic_core (
    input  logic                        clk,
    input  logic                        rst,

    // ------------------------------------------------- the fabric strategy
    // I-090. Low is the fixed machine this core has always been: dispatch's
    // alternating cluster affinity, rename's allocator without the I-032 bank
    // preference, and no local bypass (I-027). High enables the fabric --
    // dynamic steering (I-029), the bank preference fed by the routing
    // decision, and the cluster bypass -- so a case can run one program
    // through both strategies and compare. It is a *runtime* input, not a
    // parameter, and it is one toggle because the comparison must be on equal
    // resources: the same core, the same program, the same seed, one bit.
    input  logic                        fab_dyn_i,

    // ------------------------------------------------- the L1 cache path (I-042)
    // A *runtime* enable for the integrated L1 instruction and data caches,
    // exactly as `fab_dyn_i` is a runtime strategy bit. Low is the machine this
    // core has always been -- the caches are a wire and every access goes to the
    // memory service unchanged, with no added cycle. High puts the caches in the
    // path: a request goes to the cache only when the *platform map* says its
    // address is cacheable, and every other access (MMIO, the boot ROM, an
    // atomic) bypasses with its own size and strobes. `cache.integrated_path`
    // runs one program with this low and again with it high and requires the
    // architectural result to be identical.
    input  logic                        cache_en_i,

    // ----------------------------------------- the runtime lane quota (I-059)
    // A *runtime* control port, like `fab_dyn_i`: the vector engine's lane
    // quota is a resource share, not architectural state, so it is not a CSR
    // and `vl`/`vlenb` must not move when it changes. The broker changes the
    // committed quota only at a vector instruction boundary after a drain.
    // Every driver that predates the vector work leaves `req_valid` low, so the
    // quota stays at its reset value of 8 and behaviour is unchanged.
    input  logic                        lane_quota_req_i,
    input  logic [3:0]                  lane_quota_val_i,

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

    // ------------------------------------- the coherence notification (I-040)
    // A write another agent performed on the shared memory. It reaches the
    // reservation manager inside mosaic_lsu_endpoint and has no effect on any
    // access: with one hart and no cache this port *is* the invalidation
    // interface a coherent fabric would otherwise supply, and a single-hart
    // machine that ties it low behaves exactly as before. See I-040's report for
    // what stands in for the second agent in the p0 profile.
    input  logic                        ext_write_valid,
    input  logic [63:0]                 ext_write_addr,
    input  logic [3:0]                  ext_write_bytes,

    // ---------------------------------------------------- LR/SC observation
    // The reservation and what the LR/SC path did, exported so the case can
    // require the *state* an LR establishes and each invalidation source
    // destroys, rather than only the SC status that state produces.
    output logic                        o_mem_res_valid,
    output logic [63:0]                 o_mem_res_granule,
    output logic [31:0]                 o_mem_lr_ctr,
    output logic [31:0]                 o_mem_sc_ok_ctr,
    output logic [31:0]                 o_mem_sc_fail_ctr,
    output logic [31:0]                 o_mem_res_ext_inval_ctr,
    // The class of the transaction the data port is carrying: 0 ordinary,
    // 1 AMO, 2 LR, 3 SC (valid while a transaction is in flight).
    output logic [1:0]                  o_mem_dmem_kind,

    // ------------------------------------------------------ retire event stream
    output logic [CORE_RET_N-1:0]       ev_valid,
    output logic [CORE_RET_N-1:0]       ev_trap,
    output logic [CORE_RET_N*CORE_SEQ_W-1:0]  ev_seq,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_pc,
    // I-041: the instruction's own length (2 or 4 bytes) and its own bits, so a
    // consumer can tell a compressed instruction from a 32-bit one without
    // inferring the length from the PC, which alignment does not decide.
    output logic [CORE_RET_N*CORE_SIZE_W-1:0] ev_len,
    output logic [CORE_RET_N*32-1:0]          ev_insn,
    output logic [CORE_RET_N*CORE_RET_ID_W-1:0] ev_id,
    output logic [CORE_RET_N-1:0]       ev_reg_we,
    output logic [CORE_RET_N*CORE_RD_W-1:0]   ev_rd,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_value,
    output logic [CORE_RET_N-1:0]       ev_store,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_store_addr,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_store_data,
    output logic [CORE_RET_N*CORE_SIZE_W-1:0] ev_store_size,
    // The CSR and trap halves of the same frozen record. `ev_csr_*` are
    // presented for a retiring CSR instruction that writes (`csr_write_valid`),
    // and `ev_trap_cause`/`ev_trap_tval` for a lane that trapped -- from the
    // buffer's exception bit or from a trap the system unit resolved.
    output logic [CORE_RET_N-1:0]       ev_csr_we,
    output logic [CORE_RET_N*CORE_CSR_W-1:0]  ev_csr_addr,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_csr_value,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_trap_cause,
    output logic [CORE_RET_N*CORE_XLEN-1:0]   ev_trap_tval,

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
    // The endpoint's own "a transaction is outstanding" flag. It is the third
    // term of the FENCE/FENCE.I drain rule (I-037): a store leaves the queue when
    // the endpoint accepts it, so "the store queue is empty" does not by itself
    // mean the memory path has drained.
    output logic                        o_mem_lsu_busy,
    output logic [31:0]                 o_mem_ins_stall,
    output logic                        o_mem_squash_valid,
    // ------------------------------------------------- the device path (I-038)
    // What the serializer classified and how it behaved. `o_mem_dev_txn` counts
    // the accesses it presented to the memory system as device accesses (the
    // attribute below), `o_mem_ram_txn` the ordinary ones, and `o_mem_dev_wait`
    // the cycles a device access was offered but refused because it was not yet
    // irreversible-safe -- the evidence that the non-speculation gate was
    // exercised rather than merely declared.
    output logic [31:0]                 o_mem_dev_txn,
    output logic [31:0]                 o_mem_ram_txn,
    output logic [31:0]                 o_mem_dev_wait,
    // The cycles device transactions spent held in the serializer's register
    // waiting for the endpoint. With `o_mem_dev_txn` it is the device path's own
    // cost, measured: the register boundary the serializer adds to an access.
    output logic [31:0]                 o_mem_dev_hold,
    // The device attribute and the identity of the access the endpoint is
    // serving, aligned with the data port: a testbench reads them in the cycle
    // it accepts a transaction and can require the attribute to match the
    // address's region and every device identity to appear exactly once.
    output logic                        o_mem_dmem_dev,
    output logic [CORE_MEM_ID_W-1:0]    o_mem_dmem_id,
    output logic [31:0]                 o_dbg_mmio,

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
    // ------------------------------------------------- CSR / trap / interrupt
    // The platform's interrupt sources and the time the CSR file reads for
    // `time`. Level-sensitive and asynchronous in real time; mosaic_interrupt
    // synchronises them. The p0 map wires these to the CLINT's msip and its
    // timer comparison, and to the (absent) PLIC output.
    input  logic                        irq_soft_i,
    input  logic                        irq_timer_i,
    input  logic                        irq_ext_i,
    input  logic [CORE_XLEN-1:0]        mtime_i,

    // The CSR file's architectural state, so a case can state what a trap left
    // behind instead of inferring it from the retirement stream.
    output logic [CORE_XLEN-1:0]        o_csr_mstatus,
    output logic [CORE_XLEN-1:0]        o_csr_mtvec,
    output logic [CORE_XLEN-1:0]        o_csr_mepc,
    output logic [CORE_XLEN-1:0]        o_csr_mcause,
    output logic [CORE_XLEN-1:0]        o_csr_mtval,
    output logic [CORE_XLEN-1:0]        o_csr_mscratch,
    output logic [CORE_XLEN-1:0]        o_csr_mie,
    output logic [CORE_XLEN-1:0]        o_csr_mip,
    // F/D (I-050): the FP control/status state, so a case observes the
    // architectural fflags/frm/fcsr a program reads rather than inferring them.
    output logic [CORE_XLEN-1:0]        o_csr_fcsr,
    output logic [4:0]                  o_csr_fflags,
    output logic [2:0]                  o_csr_frm,
    // FP evidence: issue/commit/flags counters and the merge count, plus the
    // live FP architectural map pointer for diagnosis.
    output logic [31:0]                 o_fp_issue_ctr,
    output logic [31:0]                 o_fp_commit_ctr,
    output logic [31:0]                 o_fp_flags_ctr,
    output logic [31:0]                 o_fp_merge_ctr,
    // ---------------------------------------------- the vector engine (I-059)
    // The architectural vector CSRs, the engine's own progress counters, and
    // three packed observation words. Nothing in the core reads any of them;
    // they exist so CASE=vec.integrated can state what the integrated machine
    // did rather than infer it.
    output logic [63:0]                 o_vec_vtype,
    output logic [63:0]                 o_vec_vl,
    output logic [63:0]                 o_vec_vstart,
    output logic [63:0]                 o_vec_vcsr,
    output logic [63:0]                 o_vec_vlenb,
    output logic [63:0]                 o_vec_vlmax,
    output logic                        o_vec_vill,
    output logic [31:0]                 o_vec_macro_ctr,
    output logic [31:0]                 o_vec_elem_ctr,
    output logic [31:0]                 o_vec_trap_ctr,
    output logic [31:0]                 o_vec_retire_ctr,
    output logic [31:0]                 o_vec_fault_ctr,
    output logic [31:0]                 o_vec_lsu_req_ctr,
    output logic [31:0]                 o_vec_chain_accept_ctr,
    output logic [31:0]                 o_vec_chain_refuse_ctr,
    output logic [31:0]                 o_vec_desc_alloc_ctr,
    output logic [31:0]                 o_vec_desc_release_ctr,
    output logic [31:0]                 o_vec_alu_elems,
    output logic [31:0]                 o_vec_alu_src_rd_ctr,
    output logic [63:0]                 o_vec_alu_acc,
    output logic [31:0]                 o_vec_vrf_rd_ctr,
    output logic [31:0]                 o_vec_vrf_wr_ctr,
    output logic [31:0]                 o_vec_vrf_bad_ctr,
    output logic [31:0]                 o_vec_vrf_rows,
    output logic [31:0]                 o_vec_vrf_banks,
    output logic [63:0]                 o_vec_dbg0,
    output logic [63:0]                 o_vec_dbg1,
    output logic [63:0]                 o_vec_dbg2,
    // ---------------------------------------------- the lane broker (I-059)
    // The committed quota, the request it is holding, the generation, and the
    // boundary/acknowledgement evidence. `o_lane_elem_ctr` is eight 32-bit
    // per-lane element counts packed lane 0 in the least significant slice:
    // the direct statement that the quota changed *how many lanes worked on the
    // elements* and never *which elements exist*.
    output logic [3:0]                  o_lane_quota,
    output logic [3:0]                  o_lane_req_quota,
    output logic [7:0]                  o_lane_gen,
    output logic                        o_lane_busy,
    output logic                        o_lane_stop_admit,
    output logic                        o_lane_macro_live,
    output logic [31:0]                 o_lane_publish_ctr,
    output logic [31:0]                 o_lane_ack_req_ctr,
    output logic [31:0]                 o_lane_ack_ctr,
    output logic [31:0]                 o_lane_req_mid_macro_ctr,
    output logic [31:0]                 o_lane_pub_mid_macro_ctr,
    output logic [31:0]                 o_lane_abort_ctr,
    output logic [255:0]                o_lane_elem_ctr,
    output logic [31:0]                 o_csr_wr_ctr,
    output logic [31:0]                 o_csr_illegal_wr_ctr,
    output logic [31:0]                 o_csr_trap_ctr,
    output logic [31:0]                 o_csr_mret_ctr,
    // The trap entry as it was presented to the CSR file: the pulse, its class,
    // its cause/tval, the interrupted PC and the vector the front end resumes
    // at. A case checks `mepc`/`mcause`/`mtval` against these rather than
    // against a value read back a cycle later.
    output logic                        o_trap_valid,
    output logic                        o_trap_is_irq,
    output logic [CORE_XLEN-1:0]        o_trap_cause,
    output logic [CORE_XLEN-1:0]        o_trap_tval,
    output logic [CORE_XLEN-1:0]        o_trap_epc,
    output logic [CORE_XLEN-1:0]        o_trap_target,
    // MRET: the pulse the CSR file saw and the PC it returns to.
    output logic                        o_mret_valid,
    output logic [CORE_XLEN-1:0]        o_mret_target,
    // ------------------------------------------------- privilege evidence (I-044)
    output logic [1:0]                  o_priv,
    output logic [63:0]                 o_medeleg,
    output logic [63:0]                 o_mideleg,
    output logic                        o_sret_valid,
    output logic [CORE_XLEN-1:0]        o_sret_target,
    output logic [63:0]                 o_sstatus,
    output logic [63:0]                 o_stvec,
    output logic [63:0]                 o_sepc,
    output logic [63:0]                 o_scause,
    output logic [63:0]                 o_stval,
    output logic [63:0]                 o_sscratch,
    output logic [63:0]                 o_satp,
    output logic [31:0]                 o_csr_sret_ctr,
    output logic [31:0]                 o_csr_trap_s_ctr,
    output logic [31:0]                 o_csr_priv_illegal_ctr,
    output logic [31:0]                 o_csr_priv_change_ctr,
    output logic [31:0]                 o_pmp_query_ctr,
    output logic [31:0]                 o_pmp_deny_ctr,
    output logic [31:0]                 o_pmp_fetch_deny_ctr,
    output logic [31:0]                 o_pmp_locked_ctr,
    // D5: how many times a store was refused by the PMP at authorisation, i.e.
    // how often the commit path took the store access fault. Zero in every
    // profile that implements no PMP entries.
    output logic [31:0]                 o_pmp_store_deny_ctr,
    // The shape of each decision, so a case can require *which* answer it got
    // rather than only whether the machine trapped: a denial that came from a
    // locked entry is a different fact from one that came from a low-numbered
    // unlocked entry above a broader region.
    output logic                        o_pmp_data_matched,
    output logic                        o_pmp_data_locked,
    output logic                        o_pmp_fetch_matched,
    output logic                        o_pmp_fetch_locked,
    output logic                        o_csr_pmp_sel,
    output logic [mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_CFG_W-1:0]  o_pmp_cfg,
    output logic [mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_ADDR_W-1:0] o_pmp_addr,
    // The interrupt decision and the WFI halt, straight from mosaic_interrupt.
    output logic                        o_irq_valid,
    output logic [CORE_XLEN-1:0]        o_irq_cause,
    output logic [7:0]                  o_irq_ctr,
    output logic                        o_wfi_halt,
    output logic [7:0]                  o_spurious_wake_ctr,
    output logic [7:0]                  o_halt_cycles,
    // Counters for the integration itself: system macros resolved at the head,
    // exception payloads captured from the writeback path, traps taken from
    // each source, and the two ways the capture could have been wrong.
    output logic [31:0]                 o_sys_exec_ctr,
    output logic [31:0]                 o_exc_capture_ctr,
    output logic [31:0]                 o_exc_gen_mismatch_ctr,
    output logic [31:0]                 o_trap_irq_ctr,
    output logic [31:0]                 o_sys_redirect_ctr,
    // The I-046 translation cache's own view: how many requests it served from
    // an entry, how many walked, and how many walk results a fence or a satp
    // write discarded before they could install.
    output logic [31:0]                 o_tlb_hit_ctr,
    output logic [31:0]                 o_tlb_miss_ctr,
    output logic [31:0]                 o_tlb_install_ctr,
    output logic [31:0]                 o_tlb_walk_ctr,
    output logic [31:0]                 o_tlb_stale_ctr,
    output logic [31:0]                 o_tlb_sfence_ctr,
    output logic [31:0]                 o_tlb_satp_flush_ctr,
    output logic [15:0]                 o_tlb_gen,

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
    output logic [CORE_PRF_N-1:0]       o_dbg_wb_done,

    // ------------------------------------------------------- fabric evidence
    // I-090. What the dynamic steering decided and where macros went, straight
    // from mosaic_dispatch's `o_fab_*` ports, plus the bank the I-032
    // preference was driven with. All zero and static while `fab_dyn_i` is
    // low. Nothing in the core reads them.
    output logic [4*32-1:0]             o_fab_unit_issues,
    output logic [6*32-1:0]             o_fab_reason_ctr,
    output logic [31:0]                 o_fab_grant_ctr,
    output logic [31:0]                 o_fab_stall_ctr,
    output logic [31:0]                 o_fab_reject_ctr,
    output logic [31:0]                 o_fab_units,
    output logic [31:0]                 o_fab_classes,
    output logic [31:0]                 o_fab_age_w,
    output logic [31:0]                 o_fab_occ_w,
    output logic [31:0]                 o_fab_unit_w,
    output logic [1:0]                  o_fab_alloc_bank,
    // The cluster bypass (I-027), wired by I-090: what each cluster's slot did.
    // `o_fab_bp_hit_ctr` counts operands the slot resolved before the durable
    // wakeup, `o_fab_bp_captured_ctr` the producers it tapped, and
    // `o_fab_bp_flush_ctr` the redirects that cleared it.
    output logic [31:0]                 o_fab_bp_captured_ctr,
    output logic [31:0]                 o_fab_bp_hit_ctr,
    output logic [31:0]                 o_fab_bp_unauth_ctr,
    output logic [31:0]                 o_fab_bp_flush_ctr
);

  // ==========================================================================
  // Interconnect declarations (hoisted: a port connection must see its net)
  // ==========================================================================
  // fetch
  logic [CORE_XLEN-1:0]      fetch_pc_q;
  logic                      want_imem_req;
  logic                      fetch_slot_free;
  logic                      fetch_req_valid_int;
  logic                      fetch_out_valid, fetch_out_ready;
  logic [CORE_XLEN-1:0]      fetch_out_pc;
  logic [31:0]               fetch_out_bits;
  logic [2:0]                fetch_out_len;
  logic                      fetch_out_illegal, fetch_out_fault;
  // I-041: an instruction the fetch unit has delivered but whose consumption
  // has not yet advanced the program counter, and the fetch unit's own count of
  // occupied request slots. Together they hold the front end to one instruction
  // in flight, which is what lets the PC advance by the instruction's own length.
  logic                      fetch_insn16;
  logic [CORE_FETCH_CNT_W-1:0] fetch_outstanding;
  // The accepted response's liveness and its instruction's length, published by
  // the fetch unit; and the sequential PC computed from them.
  logic                      fetch_rsp_live;
  logic [2:0]                fetch_rsp_len;
  logic [CORE_XLEN-1:0]      fetch_next_pc;
  // The predictor offers no next PC in this integration (`pred_valid` is tied
  // low), so the fetch unit's advisory next-PC pair is left unconnected rather
  // than carried as a dead net.
  logic [127:0]              fetch_dbg_state;

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
  // I-041: the delivered instruction's own length and bits, stored beside the
  // control word so the retire event can carry them. They are the *fetch*
  // unit's values, never derived here from the decoded control.
  logic [2:0]                dbuf_len   [0:1];
  logic [31:0]               dbuf_bits  [0:1];
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
  logic [2:0]                dbuf_len_n  [0:1];
  logic [31:0]               dbuf_bits_n [0:1];

  // dispatch
  logic [4:0]                alloc_rd_w;
  logic                      disp_take, disp_unsupported;
  logic [1:0]                disp_rq_valid, disp_rq_written;
  logic [1:0][CORE_TAG_W-1:0]  disp_rq_tag;
  logic [1:0][CORE_IGEN_W-1:0] disp_rq_gen;

  // rename
  logic                      ren_alloc_req, ren_alloc_accepted, ren_alloc_exhausted;
  logic                      ren_alloc_squashed, ren_alloc_is_x0, ren_alloc_new_valid;
  // F/D (I-050): the namespace of each renamed operand, driven by dispatch.
  logic                      ren_alloc_is_fp, ren_rs1_is_fp, ren_rs2_is_fp;
  logic                      ren_commit_is_fp, ren_commit2_is_fp;
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
  logic                      ren_flush_restore;
  logic                      fetch_redir_valid;
  logic                      dbuf_purge;
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
  // I-041: the retiring instruction's own length and bits, read back from the
  // descriptor store on the same two retire lanes the destination comes from.
  logic [2:0]                desc_len0, desc_len1;
  logic [31:0]               desc_insn0, desc_insn1;
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
  // I-090: the two clusters' bypass evidence, summed into the core's own
  // fabric outputs below.
  logic [31:0]               c0_wu2_matched, c1_wu2_matched;
  logic [31:0]               c0_bp_captured, c1_bp_captured;
  logic [31:0]               c0_bp_unauth, c1_bp_unauth;
  logic [31:0]               c0_bp_flush, c1_bp_flush;
  logic [31:0]               c0_count, c1_count;
  // I-090: the strategy the fabric actually sees. `MOSAIC_FAB_MUTANT_NO_DELTA`
  // is the measurement control: it holds this low whatever the input says, so
  // the "dynamic" run of the case is the fixed machine and the case's
  // measurable-difference check has nothing to measure and must fail. It is the
  // control for the comparison itself, not for the policy.
  logic                      fab_dyn_use;
`ifdef MOSAIC_FAB_MUTANT_NO_DELTA
  assign fab_dyn_use = 1'b0;
`else
  assign fab_dyn_use = fab_dyn_i;
`endif

  // I-090 measurement instrumentation: the step table in
  // results/reports/I-090-fabric.md is measured by building the case with
  // exactly one of these defined, which disables one integration step while
  // leaving the others on. They are *not* controls -- a control must fail, a
  // step build is a partial fabric -- and no registered run defines one.
  logic fab_dyn_steer, fab_dyn_bank, fab_dyn_bp;
`ifdef MOSAIC_FAB_STEP_NO_STEERING
  assign fab_dyn_steer = 1'b0;
`else
  assign fab_dyn_steer = fab_dyn_use;
`endif
`ifdef MOSAIC_FAB_STEP_NO_BANK
  assign fab_dyn_bank = 1'b0;
`else
  assign fab_dyn_bank = fab_dyn_use;
`endif
`ifdef MOSAIC_FAB_STEP_NO_BYPASS
  assign fab_dyn_bp = 1'b0;
`else
  assign fab_dyn_bp = fab_dyn_use;
`endif
  // I-090: the cluster dispatch's allocation-time affinity pinned the oldest
  // buffered macro to; the I-032 bank preference is driven from it while the
  // fabric is on.
  logic                      fab_alloc_cluster;
  logic [1:0]                fab_alloc_bank;
  assign fab_alloc_bank   = {1'b0, fab_alloc_cluster};
  assign o_fab_alloc_bank = fab_alloc_bank;
  // The bypass evidence, summed over the two clusters (I-090).
  assign o_fab_bp_captured_ctr = c0_bp_captured + c1_bp_captured;
  assign o_fab_bp_hit_ctr      = c0_wu2_matched + c1_wu2_matched;
  assign o_fab_bp_unauth_ctr   = c0_bp_unauth + c1_bp_unauth;
  assign o_fab_bp_flush_ctr    = c0_bp_flush + c1_bp_flush;
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

  // F/D (I-050): the shared floating-point unit's bridge to cluster 0, the
  // completion it produces, and the retire-side flag sideband.
  logic                      fp_req_valid, fp_req_ready;
  mosaic_pkg::fp_op_e        fp_req_op;
  logic                      fp_req_fmt;
  logic [2:0]                fp_req_rm;
  logic                      fp_req_dst_fp, fp_req_src1_fp, fp_req_src2_fp;
  logic                      fp_req_iw, fp_req_is;
  logic [CORE_XLEN-1:0]      fp_req_a, fp_req_b;
  logic [CORE_IDX_W-1:0]     fp_req_rob_index;
  logic [CORE_RGEN_W-1:0]    fp_req_rob_gen;
  logic [CORE_UOP_W-1:0]     fp_req_uop_index;
  logic [CORE_TAG_W-1:0]     fp_req_dst_tag;
  logic [CORE_IGEN_W-1:0]    fp_req_dst_gen;
  mosaic_uop_pkg::wb_event_t fp_wb_ev;
  logic                      fp_wb_valid, fp_wb_ready;
  logic [4:0]                fp_wb_fflags;
  // The port-2 merge with the shared MUL/DIV unit: one arbiter port, two
  // producers, one at a time (the same sharing rule port 3 uses for the memory
  // path and the CSR/system unit).
  mosaic_uop_pkg::wb_event_t port2_ev;
  logic                      port2_valid, port2_ready;

  // The FP flag sideband, indexed by ROB slot (I-050). `fp_flag_v` is set when
  // an FP completion is taken by the writeback path and cleared on every
  // redirect and on the slot's own retirement; the generation is stored beside
  // the flags so a late completion from a discarded instruction cannot be
  // mistaken for the new owner of a recycled slot. `fp_state_wr` records, per
  // slot, whether the instruction modifies FP state -- the mstatus.FS dirty
  // predicate.
  logic [4:0]                fp_flag_mem [0:CORE_ROB_N-1];
  logic [CORE_RGEN_W-1:0]    fp_flag_gen_mem [0:CORE_ROB_N-1];
  logic                      fp_flag_v_mem [0:CORE_ROB_N-1];
  logic                      fp_state_wr_mem [0:CORE_ROB_N-1];
  logic                      fp_merge0_v, fp_merge1_v;
  logic [4:0]                fp_merge0, fp_merge1;
  logic [4:0]                fp_fflags_or;
  logic                      fp_fs_dirty;
  logic [31:0]               fp_merge_ctr;
  // The FP-load NaN-box predicate, per ROB slot: an flw word load has its upper
  // 32 bits set on the way back from memory.
  logic                      fp_load_box_mem [0:CORE_ROB_N-1];
  // Whether the instruction in a ROB slot writes an f-register (I-050's commit
  // namespace), recorded at allocation and read at retire.
  logic                      fp_dst_mem [0:CORE_ROB_N-1];
  logic                      disp_mem_is_fp;

  // retire
  logic [CORE_RET_N-1:0]     ret_req;
  logic [CORE_RET_N-1:0]     ret_commit_valid;
  logic [CORE_RET_N*CORE_RD_W-1:0]  ret_commit_rd;
  logic [CORE_RET_N*CORE_TAG_W-1:0] ret_commit_tag;
  logic [CORE_RET_N*CORE_XLEN-1:0] retire_pay_value;
  logic                      head_pending_taken;
  logic                      head_fence_i_pending;

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
  logic                       disp_store_faults;
  logic [CORE_XLEN-1:0]       disp_mem_base, disp_mem_imm, disp_mem_data;
  logic [2:0]                 disp_mem_size;
  logic                       disp_mem_signed;
  // The A extension (I-039). The memory insert bus already carries the operand
  // in `disp_mem_data`; these are the operation and the ordering bits.
  logic                       disp_mem_is_amo;
  mosaic_pkg::amo_op_e        disp_mem_amo_op;
  logic                       disp_mem_amo_aq, disp_mem_amo_rl;
  // LR/SC (I-040). `disp_mem_is_atomic` is the union the memory path keys on:
  // allocation is refused while a second atomic is resident, and the serializer
  // treats all three as one non-speculative class.
  logic                       disp_mem_is_lr, disp_mem_is_sc;
  logic                       disp_mem_is_atomic;
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
  // The store payload's address and data, declared here (I-045) rather than with
  // the rest of the retirement payload presentation below, because the Sv39
  // translation section (13b) reads the head stores' `base + imm` to translate
  // them and the trap controller reads the same value as the page fault's tval.
  logic [CORE_XLEN-1:0]             sq_pay0_addr, sq_pay0_data;
  logic [CORE_XLEN-1:0]             sq_pay1_addr, sq_pay1_data;

  // ==========================================================================
  // I-045 Sv39 translation declarations. They live here, with the other
  // memory-path declarations, because the trap controller (section 10) reads
  // the store page-fault decision and the memory path (section 14) drives it.
  // ==========================================================================
  localparam int unsigned ST_FIFO_PTR_W = (CORE_SQ_N <= 1) ? 1 : $clog2(CORE_SQ_N);

  // The one global condition that turns translation on for a data access: the
  // effective privilege is below M *and* satp selects a paging mode. Both are
  // committed CSR state (`eff_priv_c` already folds MPRV), and in a profile with
  // no satp the CSR file drives `csr_satp` to zero, so p0 is unchanged.
  logic        xlate_active_c;

  // The PTW's request, result and PTE-port wires.
  logic        ptw_xl_req_valid, ptw_xl_req_ready;
  logic [63:0] ptw_xl_va;
  logic [1:0]  ptw_xl_kind, ptw_xl_priv;
  logic [3:0]  ptw_xl_mode;
  logic [43:0] ptw_xl_ppn;
  logic        ptw_xl_sum, ptw_xl_mxr, ptw_xl_cancel;
  logic        ptw_xl_rsp_valid, ptw_xl_rsp_ready;
  logic [63:0] ptw_xl_pa;
  logic        ptw_xl_fault;
  logic [3:0]  ptw_xl_cause;
  logic        ptw_busy_q, ptw_owner_q;

  // The PTE port and the endpoint's port, merged onto the core's `dmem` port by
  // a two-master arbiter (one transaction in flight, the walker first).
  logic        ptw_mem_req_valid, ptw_mem_req_ready;
  logic        ptw_mem_we;
  logic [63:0] ptw_mem_addr, ptw_mem_wdata;
  logic [7:0]  ptw_mem_wstrb;
  logic        ptw_mem_rsp_valid, ptw_mem_rsp_ready, ptw_mem_fault;
  logic [63:0] ptw_mem_rdata;
  logic        ep_mem_req_valid, ep_mem_req_ready;
  mosaic_uop_pkg::mem_req_t ep_mem_req;
  logic        ep_mem_rsp_valid, ep_mem_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t ep_mem_rsp;
  // V (I-059) adds a fourth owner: the vector engine's packetizer, which drives
  // the same one data port while a vector macro is at the ROB head. It is given
  // priority over the endpoint because the memory path is idle when it starts
  // (launch is gated on `mem_path_idle`) and no younger macro exists to compete
  // with it.
  logic [2:0]  mem_owner_q;
  localparam logic [2:0] MEM_OWN_NONE = 3'd0, MEM_OWN_EP = 3'd1, MEM_OWN_PTW = 3'd2,
                         MEM_OWN_VEC = 3'd3;

  // The load translation stage. In Bare it is a wire (`lq_bypass_c`); with
  // paging it holds the offered load until its translation is known, then either
  // presents the physical request to the endpoint or hands the fault back to the
  // load queue itself.
  localparam logic [1:0] LS_EMPTY = 2'd0, LS_XL = 2'd1, LS_PRESENT = 2'd2,
                         LS_FAULT = 2'd3;
  logic [1:0]  lq_stg_state_q;
  logic        lq_bypass_c, lq_tx_valid, lq_tx_ready;
  mosaic_uop_pkg::lsu_req_t lq_tx_req;
  logic [63:0] lq_tx_tval;
  logic        lq_xl_req_valid, lq_xl_req_accepted, lq_xl_wait_q;
  mosaic_uop_pkg::lsu_req_t lq_hold_q;
  logic [63:0] lq_hold_tval_q, lq_hold_pa_q;
  logic [1:0]  lq_priv_q;
  logic [3:0]  lq_mode_q;
  logic [43:0] lq_ppn_q;
  logic        lq_sum_q, lq_mxr_q;
  logic [3:0]  lq_xl_cause_q;
  logic        lq_xl_fault_valid;
  mosaic_uop_pkg::lsu_rsp_t lq_xl_fault_rsp;

  // The store translation: a per-lane result for the commit decision and a
  // FIFO of translated physical addresses for the drains that follow.
  logic        st_hit0_c, st_hit1_c, st_fault0_c, st_fault1_c;
  logic [63:0] st_pa0_c, st_pa1_c;
  logic [3:0]  st_cause0_c;
  logic        st_lane0_blocks_c, st_lane1_blocks_c;
  logic        st_req_valid, st_req_lane1_c, st_req_lane1_q;
  logic        st_want0_c, st_want1_c;
  logic [63:0] st_req_va;
  logic [CORE_IDX_W-1:0]  st_req_idx, st_req_idx_q;
  logic [CORE_RGEN_W-1:0] st_req_gen, st_req_gen_q;
  logic        st_r0_valid_q, st_r1_valid_q;
  logic [CORE_IDX_W-1:0]  st_r0_idx_q, st_r1_idx_q;
  logic [CORE_RGEN_W-1:0] st_r0_gen_q, st_r1_gen_q;
  logic [63:0] st_r0_pa_q, st_r1_pa_q;
  logic        st_r0_fault_q, st_r1_fault_q;
  logic [3:0]  st_r0_cause_q;
  logic        st_push0_c, st_push1_c, st_pop_c;
  logic [63:0] sq_drain_pa_c;
  logic [63:0] st_fifo_pa [0:CORE_SQ_N-1];
  logic [CORE_MEM_CNT_W-1:0] st_fifo_cnt_q;
  logic [ST_FIFO_PTR_W-1:0]  st_fifo_head_q, st_fifo_tail_q;
  logic [31:0] st_fifo_ovf_ctr_q, st_xl_alloc_ctr_q, st_xl_pop_ctr_q;

  // The store page-fault decision the trap controller and the retire gate read.
  logic        store0_xl_fault_c, store0_xl_fault_now;
  // The architectural tval presented to the endpoint with the transaction it is
  // offered, and the serializer's latched copy for a held (device or atomic)
  // transaction -- the same lifetime `ser_hold_dev_q` has.
  logic [63:0] ep_tval_c, sq_drain_tval_c;
  logic [63:0] ser_hold_tval_q;
  logic        ptw_take_c;
  // The PMP commit-path address for the two lanes: the translated physical
  // address when there is one, the effective address otherwise.
  logic [CORE_XLEN-1:0] pmp_store_addr0_c, pmp_store_addr1_c;
  logic [2:0]                 sq_alloc_size;
  logic [31:0]                sq_alloc_ctr, sq_commit_ctr, sq_commit_stale_ctr;
  logic [31:0]                sq_drain_ctr, sq_squash_ctr, sq_spared_ctr, sq_fault_ctr;

  logic                       ep_req_valid, ep_req_ready;
  mosaic_uop_pkg::lsu_req_t   ep_req;
  logic                       ep_rsp_valid, ep_rsp_ready;
  mosaic_uop_pkg::lsu_rsp_t   ep_rsp;
  logic                       ep_owner_q;
  logic [31:0]                lsu_txn_ctr, lsu_misaligned_ctr, lsu_access_fault_ctr;
  logic                       ep_busy;
  // The device serializer (I-038): the endpoint's upstream port is driven by the
  // serializer, not by the queue mux, so these are the wires between them, plus
  // the identity/attribute probe the endpoint exports alongside the transaction.
  // The serializer's registers are declared here rather than next to its logic
  // because the FENCE/FENCE.I drain rule (section 10b) reads its busy state.
  logic                       ser_req_valid, ser_ep_req_ready;
  mosaic_uop_pkg::lsu_req_t   ser_req;
  logic [CORE_MEM_ID_W-1:0]   ep_txn_id;
  logic                       ep_txn_dev;
  // I-040: the endpoint's LR/SC observations, and the transaction class.
  logic [1:0]                 ep_txn_kind;
  logic                       ep_res_valid;
  logic [CORE_XLEN-1:0]       ep_res_granule;
  logic [31:0]                ep_lr_ctr, ep_sc_ok_ctr, ep_sc_fail_ctr;
  logic [31:0]                ep_res_ext_inval_ctr;
  logic                       ser_take_c;
  logic                       ser_nonspec_c;
  logic                       ser_is_device_c;
  logic [CORE_XLEN-1:0]       ser_addr_c;
  logic                       ser_hold_valid_q;
  logic                       ser_dev_out_q;
  mosaic_uop_pkg::lsu_req_t   ser_hold_q;
  logic                       ser_out_dev_c;
  logic                       ser_accept_c;
  logic                       ser_owner_q;
  logic                       ser_owner_c;
  // I-039. `ser_serialize_c` is "this access must go through the serializer's
  // non-speculative, exactly-once path": a PMA device (I-038) or an atomic
  // read-modify-write. The device attribute that travels to the memory system is
  // the PMA one alone and is latched separately (`ser_hold_dev_q`), because an
  // AMO to RAM is serialized but is not a device.
  logic                       ser_is_amo_c;
  logic                       ser_serialize_c;
  logic                       ser_hold_dev_q;
  // The AMO issue record (mosaic_amo_unit): the one atomic read-modify-write the
  // load queue is currently carrying. The operation/operand cannot live in the
  // load queue's frozen entry, so they are held here and re-presented when the
  // queue offers the macro by identity.
  logic                       amo_alloc_c;
  logic                       amo_taken_c;
  logic                       amo_busy;
  logic                       amo_hit;
  mosaic_pkg::amo_op_e        amo_hit_op;
  logic                       amo_hit_aq, amo_hit_rl;
  // LR/SC (I-040): the class of the held atomic macro, and the identity/validity
  // the load queue uses to recognise its atomic head.
  logic                       amo_hit_lr, amo_hit_sc;
  logic [CORE_MEM_ID_W-1:0]   amo_atomic_id;
  logic                       amo_atomic_valid;
  logic [CORE_XLEN-1:0]       amo_hit_operand;
  logic [CORE_MEM_ID_W-1:0]   rob_head_id;
  logic                       rob_boundary_ok;
  logic [31:0]                dev_txn_ctr_q, ram_txn_ctr_q, dev_wait_ctr_q, dev_hold_ctr_q;

  logic                       lsu_wb_valid, lsu_wb_ready;
  mosaic_uop_pkg::wb_event_t  lsu_wb_ev, store_wb_ev, lq_wb_ev;
  logic                       store_cmp_valid;

  logic                       desc_is_store0, desc_is_store1;
  logic                       desc_wr_is_store;
  logic [2:0]                 desc_wr_len;
  logic [31:0]                desc_wr_insn;
  logic [CORE_IDX_W-1:0]      rob_alloc_ptr;
  logic [31:0] squash_nc_ctr;
  logic        core_stop_prev;

  // ---------------------------------------------------- the CSR/trap path
  // The CSR file (I-019), the interrupt decision (I-020), the single staging
  // entry the system macros wait in, and the trap controller that turns an
  // exception or an interrupt into a redirect.
  logic [11:0]                csr_addr;
  logic [63:0]                csr_rdata;
  logic                       csr_illegal;
  logic                       csr_wr_illegal;
  logic                       csr_we;
  logic                       csr_trap_valid;
  logic [CORE_XLEN-1:0]       csr_trap_target;
  logic                       csr_mret_valid;
  logic [CORE_XLEN-1:0]       csr_mret_target;
  logic                       csr_mip_we;
  logic [1:0]                 csr_mip_op;
  logic [63:0]                csr_mip_wdata;
  // I-044: privilege, the supervisor frame, SRET and the PMP hand-off.
  logic [1:0]                 csr_priv;
  logic [63:0]                csr_medeleg, csr_mideleg;
  logic                       csr_sret_valid;
  logic                       csr_sret_commit;
  logic [CORE_XLEN-1:0]       csr_sret_target;
  logic                       csr_mret_illegal, csr_sret_illegal, csr_wfi_illegal;
  logic                       csr_pmp_sel, csr_pmp_we;
  logic [63:0]                csr_pmp_rdata, csr_pmp_wdata;
  logic [63:0]                csr_sstatus, csr_stvec, csr_sepc, csr_scause, csr_stval;
  logic [63:0]                csr_sscratch, csr_satp;
  logic [31:0]                csr_sret_ctr, csr_trap_s_ctr;
  logic [31:0]                csr_priv_illegal_ctr, csr_priv_change_ctr;
  // The PMP unit's answer to the two questions asked of it each cycle: may the
  // data path make this access, and may the front end read this address.
  logic                       pmp_data_allow, pmp_data_matched, pmp_data_locked;
  logic                       pmp_fetch_allow, pmp_fetch_matched, pmp_fetch_locked;
  logic [63:0]                lsu_req_addr_c;
  logic                       lsu_req_r_c, lsu_req_w_c;
  logic [3:0]                 lsu_req_bytes_c;
  logic [1:0]                 eff_priv_c;
  logic                       ep_pmp_deny_c;
  logic                       fetch_pmp_deny_c;
  logic [31:0]                pmp_deny_ctr, pmp_fetch_deny_ctr, pmp_query_ctr;
  logic [31:0]                pmp_locked_ctr;
  // The store-commit PMP check (D5). A store's PMP permission is decidable in
  // the cycle the ROB authorises it -- every older instruction has retired, so
  // the PMP entries and mstatus are committed and the check is not speculative
  // -- and a refusal must be taken as the store's own exception rather than
  // left to the endpoint's post-retirement drain, which can only count it.
  logic                       pmp_store_allow0_c, pmp_store_allow1_c;
  logic                       store0_pmp_deny_c, store1_pmp_deny_c;
  logic                       store_pmp_trap_now;
  logic [CORE_XLEN-1:0]       store_pmp_trap_addr_c;
  logic [31:0]                pmp_store_deny_ctr;
  logic [63:0]                irq_mip;
  logic                       irq_valid;
  logic [63:0]                irq_cause;
  logic [7:0]                 irq_ctr, halt_cycles, spurious_wake_ctr;
  logic                       wfi_halt;
  logic                       core_can_trap;

  // the staging entry: one macro at a time, because a system macro is resolved
  // at the ROB head and only one entry can be the head
  logic                       sys_valid_q;
  logic [CORE_IDX_W-1:0]      sys_index_q;
  logic [CORE_RGEN_W-1:0]     sys_gen_q;
  logic [CORE_UOP_W-1:0]      sys_uop_q;
  logic [11:0]                sys_csr_addr_q;
  mosaic_pkg::csr_op_e        sys_csr_op_q;
  logic                       sys_csr_reads_q, sys_csr_writes_q;
  logic                       sys_ecall_q, sys_ebreak_q, sys_mret_q, sys_wfi_q;
  logic                       sys_sret_q, sys_fetch_fault_q;
  // The two fence class bits, staged exactly like the rest of the system
  // payload. They are the whole of what the fence rule reads: this profile
  // treats fence's fm/pred/succ fields conservatively and does not distinguish
  // fence.tso (see results/reports/I-037-fence.md).
  logic                       sys_fence_q, sys_fence_i_q;
  // SFENCE.VMA's staged payload (I-046): the class bit, which operand names a
  // dimension, and both operand values.
  logic                       sys_sfence_vma_q;
  logic                       sys_sfence_has_va_q, sys_sfence_has_asid_q;
  logic [15:0]                sys_src2_q;       // SFENCE.VMA's rs2 (ASID)
  logic                       fence_like_q;      // a fence or fence.i is staged
  logic                       fence_pending;     // ... and its rule still binds
  logic                       fence_mem_ok;      // "the memory path has drained"
  logic                       mem_path_idle;
  logic [CORE_XLEN-1:0]       sys_src1_q;
  logic [CORE_TAG_W-1:0]      sys_dst_tag_q;
  logic [CORE_IGEN_W-1:0]     sys_dst_gen_q;
  logic                       sys_dst_x0_q;
  logic                       sys_head;
  logic                       sys_exec;
  logic                       sys_wb_want;
  logic                       sys_wb_valid;
  logic                       sys_wb_pending_q;
  mosaic_uop_pkg::wb_event_t  sys_wb_ev;
  logic                       sys_exc;
  logic [63:0]                sys_exc_cause;
  logic                       sys_trap_q;
  logic [63:0]                sys_trap_cause_q;
  logic [63:0]                sys_trap_tval_q;
  // A staged system macro that writes mstatus. It is the only state change that
  // can alter the *effective* privilege of a later memory access without
  // redirecting, so it is named once here and used both to hold younger memory
  // macros off and to make the write wait for the memory path to drain.
  logic                       sys_priv_wr_q;
  logic                       port3_taken_sys;
  logic                       port3_taken_vec;
  logic                       lsu_wb_ready_int;
  mosaic_uop_pkg::wb_event_t  wb3_ev;
  logic                       wb3_valid;

  // the trap controller
  logic                       trap_decision;
  logic                       trap_is_irq;
  logic                       sys_trap_now;
  logic                       sys_trap_take;
  logic [63:0]                trap_cause, trap_tval, trap_epc;
  logic [63:0]                exc_cause_head, exc_tval_head;
  logic                       exc_capture;
  logic [63:0]                exc_cause_win, exc_tval_win;
  logic [CORE_RGEN_W-1:0]     exc_gen_match;
  logic                       arb_sys_redirect;
  logic                       sys_redirect_delay_q;
  logic                       ret_req_gated;
  logic                       trap_irq_prev;
  logic [CORE_RET_N*CORE_XLEN-1:0] pay_exc_cause_vec;
  logic [CORE_RET_N*CORE_XLEN-1:0] pay_exc_tval_vec;
  logic                       head_exc_trap;
  logic [63:0]                trap_epc_sync, trap_epc_irq;
  logic                       sys_redir_req_valid, sys_redir_act_now;
  logic [CORE_XLEN-1:0]       sys_redir_pc;
  logic                       trap_vector_armed;

  // ------------------------------------------------------- dispatch's sys bus
  logic                       disp_sys_valid, sys_ins_ready_int;
  logic [CORE_UOP_ID_W-1:0]   disp_sys_id;
  logic [11:0]                disp_sys_csr_addr;
  logic [1:0]                 disp_sys_csr_op;
  logic                       disp_sys_csr_reads, disp_sys_csr_writes;
  logic                       disp_sys_is_ecall, disp_sys_is_ebreak;
  logic                       disp_sys_is_mret, disp_sys_is_sret, disp_sys_is_wfi;
  logic                       disp_sys_is_fetch_fault;
  logic                       disp_sys_is_fence, disp_sys_is_fence_i;
  logic                       disp_sys_is_sfence_vma;
  logic                       disp_sys_sfence_has_va, disp_sys_sfence_has_asid;
  logic [CORE_XLEN-1:0]       disp_sys_src1_val;
  // SFENCE.VMA's rs2 is an XLEN-wide register, but the ASID it names is
  // ASIDLEN=16 bits (the spec's ASIDMAX for Sv39); bits above ASIDMAX are
  // reserved and ignored. The operands are carried at XLEN because that is what
  // the pipeline renames, so the high bits are genuinely unused here.
  /* verilator lint_off UNUSEDSIGNAL */
  logic [CORE_XLEN-1:0]       disp_sys_src2_val;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [CORE_TAG_W-1:0]      disp_sys_dst_tag;
  logic [CORE_IGEN_W-1:0]     disp_sys_dst_gen;
  logic                       disp_sys_dst_x0;

  // ==========================================================================
  // V (I-059): the integrated vector engine's declarations.
  // ==========================================================================
  // One vector macro is staged at a time and resolved at the ROB head, exactly
  // as a system macro is (section 10b). The stage carries the macro's ROB
  // identity, the reduced decode dispatch handed over, and the two captured
  // integer source values (a base address, a stride, an AVL or a scalar).
  logic                       disp_sys_is_vec;
  mosaic_pkg::vec_payload_t   disp_sys_vec;
  logic                       vec_valid_q;
  logic [CORE_IDX_W-1:0]      vec_index_q;
  logic [CORE_RGEN_W-1:0]     vec_gen_q;
  logic [CORE_UOP_W-1:0]      vec_uop_q;
  mosaic_pkg::vec_payload_t   vec_pay_q;
  logic [CORE_XLEN-1:0]       vec_src1_q, vec_src2_q;
  logic [CORE_TAG_W-1:0]      vec_dst_tag_q;
  logic [CORE_IGEN_W-1:0]     vec_dst_gen_q;
  logic                       vec_dst_x0_q;
  logic                       vec_at_head;
  logic                       vec_block;
  logic                       vec_is_mem;
  logic [4:0]                 vec_pay_q_vs1;
  logic                       vec_pay_q_vtype_dep;
  // The engine's own small sequencer: idle (waiting for the head to be the
  // staged macro), running (a unit is executing), done (the completion is owed
  // to the writeback path), trap (a fault or illegality is owed to the trap
  // controller).
  localparam logic [1:0] VEC_IDLE = 2'd0;
  localparam logic [1:0] VEC_RUN  = 2'd1;
  localparam logic [1:0] VEC_DONE = 2'd2;
  localparam logic [1:0] VEC_TRAP = 2'd3;
  logic [1:0]                 vec_state_q;
  logic                       vec_launch;
  logic                       vec_launch_vset;
  logic                       vec_launch_unit;
  logic                       vec_unit_done;
  logic                       vec_unit_illegal;
  logic [63:0]                vec_trap_cause_q, vec_trap_tval_q;
  logic [6:0]                 vec_vstart_q;
  logic                       vec_trap_take;
  logic                       vec_wb_want;
  logic                       vec_wb_valid;
  logic                       vec_wb_pending_q;
  mosaic_uop_pkg::wb_event_t  vec_wb_ev;
  logic                       vec_done_q;
  logic                       vec_macro_leave;

  // mosaic_vec_cfg: the architectural vector configuration and CSR state.
  logic                       vec_vset_valid, vec_vset_vs_off;
  logic [1:0]                 vec_vset_kind;
  logic [4:0]                 vec_vset_rd, vec_vset_uimm;
  logic [10:0]                vec_vtypei;
  logic [63:0]                vec_vset_rd_val;
  logic [63:0]                vec_vtype, vec_vl, vec_vstart;
  logic [63:0]                vec_vcsr, vec_vlenb, vec_vlmax;
  logic                       vec_vill;
  logic [15:0]                vec_cfg_gen;
  logic                       vec_csr_valid, vec_csr_write, vec_csr_ready;
  logic                       vec_csr_illegal;
  logic [11:0]                vec_csr_addr;
  logic [63:0]                vec_csr_wdata, vec_csr_rdata;
  logic                       vec_csr_commit;
  logic                       vec_csr_internal;
  logic [11:0]                vec_csr_internal_addr;
  logic [63:0]                vec_csr_internal_wdata;

  // mosaic_vec_desc: the legality authority and the per-macro progress record.
  logic                       vec_desc_alloc_valid, vec_desc_alloc_ready;
  logic                       vec_desc_release;
  logic [4:0]                 vec_desc_class, vec_desc_vd, vec_desc_vs1, vec_desc_vs2;
  logic                       vec_desc_mask_en;
  logic                       vec_desc_illegal, vec_desc_vtype_legal, vec_desc_cfg_legal;
  logic [3:0]                 vec_desc_reason;
  logic [2:0]                 vec_desc_sew_log2;
  logic signed [3:0]          vec_desc_lmul_exp;
  logic [7:0]                 vec_desc_elem_count;
  logic                       vec_desc_elem_done_valid;
  logic [6:0]                 vec_desc_elem_done_index;
  logic                       vec_desc_fault_valid;
  logic [6:0]                 vec_desc_fault_elem;
  logic [3:0]                 vec_desc_fault_code;
  logic                       vec_desc_valid;
  logic [CORE_IDX_W-1:0]      vec_desc_rob_index;
  logic [CORE_RGEN_W-1:0]     vec_desc_rob_gen;
  logic [CORE_UOP_W-1:0]      vec_desc_uop_index;
  logic [7:0]                 vec_desc_vl;
  logic [6:0]                 vec_desc_vstart;
  logic [4:0]                 vec_desc_vd_out;
  logic [127:0]               vec_desc_bitmap;
  logic [7:0]                 vec_desc_prefix;
  logic [7:0]                 vec_desc_elems_done;
  logic                       vec_desc_fault_valid_out;
  logic [6:0]                 vec_desc_fault_elem_out;
  logic [3:0]                 vec_desc_fault_code_out;
  logic                       vec_desc_accepting;
  logic [7:0]                 vec_desc_rob_entries_used;
  logic [15:0]                vec_desc_alloc_ctr, vec_desc_release_ctr;

  // mosaic_vec_alu: the integer/mask/permute lane.
  localparam logic [16:0]     VEC_ALU_CAPS = 17'b0000000_0100_0001;  // ADDSUB + LOGIC
  logic                       vec_alu_exec_valid;
  logic [4:0]                 vec_alu_family;
  logic [3:0]                 vec_alu_op;
  logic [1:0]                 vec_alu_form;
  logic [4:0]                 vec_alu_vd, vec_alu_vs1, vec_alu_vs2;
  logic [63:0]                vec_alu_scalar;
  logic                       vec_alu_mask_en;
  logic                       vec_alu_done, vec_alu_illegal;
  logic                       vec_alu_sat;
  logic [7:0]                 vec_alu_elems, vec_alu_cur;
  logic [63:0]                vec_alu_acc;
  logic                       vec_alu_trace_valid;
  logic [7:0]                 vec_alu_trace_elem;
  logic [31:0]                vec_alu_src_rd_ctr;

  // mosaic_vec_lsu: the memory packetizer.
  localparam logic [7:0]      VEC_LSU_CAPS = 8'b0000_0001;  // unit-stride only
  logic                       vec_lsu_exec_valid;
  logic [3:0]                 vec_lsu_mode;
  logic                       vec_lsu_we, vec_lsu_ordered;
  logic [3:0]                 vec_lsu_nf;
  logic [4:0]                 vec_lsu_vd, vec_lsu_data, vec_lsu_index;
  logic [2:0]                 vec_lsu_idx_sew;
  logic [63:0]                vec_lsu_base, vec_lsu_stride;
  logic                       vec_lsu_mask_en;
  logic                       vec_lsu_busy, vec_lsu_done, vec_lsu_illegal, vec_lsu_trap;
  logic [6:0]                 vec_lsu_trap_elem;
  logic [3:0]                 vec_lsu_trap_code;
  logic                       vec_lsu_stopped;
  logic [6:0]                 vec_lsu_stop_elem;
  logic [7:0]                 vec_lsu_elems;
  logic [31:0]                vec_lsu_req_ctr;

  // mosaic_vec_restart: the partial-trap / vstart controller.
  logic                       vec_rst_exec_valid;
  logic [3:0]                 vec_rst_exec_mode;
  logic                       vec_rst_exec_we, vec_rst_exec_fof;
  logic [3:0]                 vec_rst_exec_nf;
  logic                       vec_rst_stop;
  logic                       vec_rst_busy, vec_rst_resolved, vec_rst_illegal;
  logic                       vec_rst_trap;
  logic [6:0]                 vec_rst_vstart;
  logic [3:0]                 vec_rst_trap_code;
  logic                       vec_rst_vl_write;
  logic [7:0]                 vec_rst_vl_new;
  logic                       vec_rst_fof_trim, vec_rst_complete, vec_rst_retire_ok;
  logic                       vec_rst_restart_ready;
  logic [6:0]                 vec_rst_restart_vstart;
  logic [7:0]                 vec_rst_elems_committed;
  logic                       vec_rst_prefix_agree;
  logic                       vec_rst_elem_done_valid;
  logic [6:0]                 vec_rst_elem_done_index;
  logic                       vec_rst_fault_valid;
  logic [6:0]                 vec_rst_fault_elem;
  logic [3:0]                 vec_rst_fault_code;

  // mosaic_vec_chain: the element-packet identity discipline (I-058).
  logic                       vec_chain_p_alloc_valid, vec_chain_p_alloc_ready;
  logic [CORE_RGEN_W-1:0]     vec_chain_p_gen;
  logic [4:0]                 vec_chain_p_vd;
  logic [7:0]                 vec_chain_p_vl;
  logic                       vec_chain_p_wr_valid, vec_chain_p_wr_accept;
  logic [6:0]                 vec_chain_p_wr_index;
  logic [63:0]                vec_chain_p_wr_data;
  logic [CORE_RGEN_W-1:0]     vec_chain_p_wr_gen;
  logic                       vec_chain_p_fault_valid;
  logic [6:0]                 vec_chain_p_fault_elem;
  logic                       vec_chain_p_cancel, vec_chain_p_done;
  logic                       vec_chain_accept_valid;
  logic [6:0]                 vec_chain_accept_index;
  logic                       vec_chain_valid;
  logic [CORE_RGEN_W-1:0]     vec_chain_gen;
  logic [4:0]                 vec_chain_vd;
  logic                       vec_chain_done, vec_chain_fault;
  logic [6:0]                 vec_chain_fault_elem;
  logic [15:0]                vec_chain_pkt_accept_ctr, vec_chain_pkt_refuse_ctr;

  // mosaic_vrf: one read slot and one write port, arbitrated between the ALU and
  // the LSU. Only one of them runs at a time (one macro in flight).
  // One lane slot: the ALU and the LSU each own a single read slot and write
  // port, and only one of them runs at a time, so LANES_MAX is 1 and the
  // arbiter below is a mux rather than an eight-way fan-out.
  logic                       vec_vrf_rd_valid;
  logic [4:0]                 vec_vrf_rd_base;
  logic [6:0]                 vec_vrf_rd_elem;
  logic [2:0]                 vec_vrf_rd_sew;
  logic [3:0]                 vec_vrf_rd_lmul;
  logic [15:0]                vec_vrf_rd_tag;
  logic                       vec_vrf_rd_gnt;
  logic                       vec_vrf_rd_rsp_valid;
  logic [15:0]                vec_vrf_rd_rsp_tag;
  logic [63:0]                vec_vrf_rd_rsp_data;
  logic                       vec_vrf_wr_valid;
  logic [4:0]                 vec_vrf_wr_base;
  logic [6:0]                 vec_vrf_wr_elem;
  logic [2:0]                 vec_vrf_wr_sew;
  logic [3:0]                 vec_vrf_wr_lmul;
  logic [63:0]                vec_vrf_wr_data;
  logic                       vec_vrf_wr_gnt;
  logic                       vec_lsu_owns_vrf;
  logic [31:0]                vec_vrf_rd_gnt_ctr, vec_vrf_wr_gnt_ctr, vec_vrf_rd_bad_ctr;
  logic [31:0]                vec_vrf_rows, vec_vrf_banks;
  // The unit's write data before the lane-quota gate. In the shipping build the
  // gate is the identity; the DISCARD_REST control makes it drop the writes of a
  // macro-in-flight whose plan-lane the newly requested (smaller) quota no
  // longer covers.
  logic [63:0]                vec_vrf_wr_data_raw;
  /* verilator lint_off UNUSEDSIGNAL */
  // Read only by the DISCARD_REST control's gate below; in the shipping build the
  // gate is the identity and this is written and never read.
  logic [6:0]                 vec_vrf_wr_elem_now;
  /* verilator lint_on UNUSEDSIGNAL */

  // ------------------------------------------------------------------ the
  // lane broker (I-059). The committed quota and its evidence; the per-macro
  // lane plan; and the eight per-lane element counters.
  logic [3:0]                 lane_quota;
  logic [3:0]                 lane_req_quota;
  logic [7:0]                 lane_gen;
  logic                       lane_busy, lane_stop_admit, lane_macro_live;
  logic [31:0]                lane_publish_ctr, lane_ack_req_ctr, lane_ack_ctr;
  logic [31:0]                lane_req_mid_macro_ctr, lane_pub_mid_macro_ctr;
  logic [31:0]                lane_abort_ctr;
  logic                       lane_ack;
  logic                       lane_macro_insert;
  logic                       lane_wb_new;
  /* verilator lint_off UNUSEDSIGNAL */
  // Bit 3 (the value 8) is read only by the DISCARD_REST gate; the lane mask
  // below uses the low three bits, for which 8 wraps to 0 and subtracts to 7.
  logic [3:0]                 lane_plan_quota_q;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [2:0]                 lane_accept_lane;

  // the vector engine's memory port, merged onto the core's one data port
  logic                       vec_mem_req_valid, vec_mem_req_ready;
  logic [63:0]                vec_mem_req_addr, vec_mem_req_wdata;
  logic [7:0]                 vec_mem_req_wmask;
  logic                       vec_mem_req_we;
  logic [3:0]                 vec_mem_req_size;
  logic [6:0]                 vec_mem_req_elem;
  logic [3:0]                 vec_mem_req_field;
  logic                       vec_mem_rsp_valid;
  logic [63:0]                vec_mem_rsp_rdata;
  logic                       vec_mem_rsp_fault;
  logic                       vec_mem_rsp_fault_raw;
  logic [3:0]                 vec_mem_rsp_code;
  logic [6:0]                 vec_mem_rsp_elem_q;
  logic [3:0]                 vec_mem_rsp_field_q;

  // vector CSR address decode and the CSR read mux
  logic                       csr_is_vec;
  logic [63:0]                csr_rdata_final;
  logic                       vec_vstart_clear;

  // evidence
  logic [31:0]                vec_macro_ctr, vec_elem_ctr, vec_trap_ctr, vec_retire_ctr;
  logic [31:0]                vec_fault_ctr;

  // ---------------------------------------------------- the CSR/trap evidence
  logic [31:0] sys_exec_ctr, exc_capture_ctr, exc_gen_mismatch_ctr;
  logic [31:0] trap_irq_ctr, sys_redirect_ctr;

  // The exception payload of each ROB entry, indexed by slot. The ROB carries
  // the *bit* "this entry excepted"; the cause and tval are what the trap entry
  // needs and the ROB's descriptor does not hold, so they are recorded here when
  // the completion that raised the exception is accepted, tagged with the slot's
  // generation so a stale record cannot be read for a recycled slot.
  logic [63:0]                exc_cause_q [0:CORE_ROB_N-1];
  logic [63:0]                exc_tval_q  [0:CORE_ROB_N-1];
  logic [CORE_RGEN_W-1:0]     exc_rec_gen_q [0:CORE_ROB_N-1];

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
  // The WFI halt stops the front end the same way a recovery does: there is no
  // instruction to fetch until the wake event, and fetching ahead of it would
  // execute past the halt.
  // The front end is also held off while the cache fence (below) is writing the
  // data cache back and invalidating the instruction cache: a fetch issued in
  // that window would race the two steps it is ordering. The declaration sits
  // here rather than with the rest of the cache-path signals below, because
  // `want_imem_req` reads it and slang rejects a use that precedes its
  // declaration (Verilator accepts either order, so only the second tool sees it).
  logic                     cache_fence_busy;
  assign want_imem_req      = !core_stop && !recovering && !wfi_halt && !cache_fence_busy;
  // The sequential program counter. RISC-V instructions are 2 or 4 bytes, so the
  // byte after the instruction being answered is `its PC + its own length` --
  // and the length only exists in the answer. `fetch_rsp_live` is high in the
  // cycle a live response is accepted and `fetch_rsp_len` is that instruction's
  // length, both published by the fetch unit from the same rule it builds the
  // instruction record with. Using them *combinationally* is what keeps the
  // front end at one instruction per cycle: the next request is issued in the
  // very cycle the current answer is taken, instead of a cycle later.
  //
  // The request is only offered when no other request is in flight, or when the
  // response being accepted is freeing the one slot there is -- so the fetch
  // unit never has two unanswered requests whose PCs it would have to guess.
  assign fetch_next_pc      = fetch_rsp_live
                              ? (fetch_pc_q + CORE_XLEN'(fetch_rsp_len))
                              : fetch_pc_q;
  // ---------------------------------------------------- the fetch permission
  //
  // The physical memory protection check on an instruction access is made
  // *before* the request leaves, so a denied address is never presented to the
  // memory system at all -- the same structural statement the data path makes by
  // refusing in ST_IDLE. It is gated to the cycle in which no request is in
  // flight and none is being answered, because a denied fetch is delivered to
  // the front end as a fault macro rather than as a response, and one delivery
  // per cycle is all the decode buffer has room for. The cost is that a deny is
  // noticed one cycle later than it could be; the alternative is a second
  // delivery port.
  //
  // Instruction address translation does not exist in this profile (I-045), and
  // neither does MPRV for fetch ("Instruction address-translation and protection
  // are unaffected by the setting of MPRV"), so the privilege the check uses is
  // the current one.
  assign fetch_pmp_deny_c = want_imem_req && fetch_slot_free &&
                            (fetch_outstanding == {CORE_FETCH_CNT_W{1'b0}}) &&
                            !fetch_rsp_live && !pmp_fetch_allow;

  // ------------------------------------------------------- the L1 cache path
  // The instruction side: fetch's request port is the wrapper's CPU side and the
  // core's `imem` port is its memory side. The data side (below, at the memory
  // path) is structured the same way. Both wrappers are a wire when
  // `cache_en_i` is low.
  logic                     ic_cpu_req_valid;
  logic                     ic_cpu_req_ready;
  mosaic_uop_pkg::mem_req_t ic_cpu_req;
  logic [CORE_REQ_ID_W-1:0] ic_cpu_req_id;
  logic [CORE_EPOCH_W-1:0]  ic_cpu_req_epoch;
  logic                     ic_cpu_rsp_valid;
  logic                     ic_cpu_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t ic_cpu_rsp;
  logic [CORE_REQ_ID_W-1:0] ic_cpu_rsp_id;
  logic [CORE_EPOCH_W-1:0]  ic_cpu_rsp_epoch;
  logic [2:0]               ic_cpu_rsp_len;
  logic                     icache_flush;
  logic                     icache_flush_done;
  logic                     dcache_flush;
  logic                     dcache_flush_done;

  // The data side: the LSU endpoint's memory port is the wrapper's CPU side and
  // the endpoint's slot on the PTE/data arbiter is its memory side.
  logic                     dc_cpu_req_valid;
  logic                     dc_cpu_req_ready;
  mosaic_uop_pkg::mem_req_t dc_cpu_req;
  logic                     dc_cpu_rsp_valid;
  logic                     dc_cpu_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t dc_cpu_rsp;
  logic                     dc_mem_req_valid;
  logic                     dc_mem_req_ready;
  mosaic_uop_pkg::mem_req_t dc_mem_req;
  logic                     dc_mem_rsp_valid;
  logic                     dc_mem_rsp_ready;
  mosaic_uop_pkg::mem_rsp_t dc_mem_rsp;

  // The fetch request now goes to the L1 instruction cache path, which is a wire
  // when `cache_en_i` is low. `ic_*` is the fetch side of that wrapper and the
  // `imem_*` ports are its memory side (see the instance below).
  assign ic_cpu_req_valid   = want_imem_req && fetch_slot_free && !fetch_pmp_deny_c &&
                              ((fetch_outstanding == {CORE_FETCH_CNT_W{1'b0}}) ||
                               fetch_rsp_live);
  assign fetch_req_valid_int= ic_cpu_req_valid && ic_cpu_req_ready;
  assign ic_cpu_req.we    = 1'b0;
  assign ic_cpu_req.addr  = fetch_next_pc;
  assign ic_cpu_req.size  = mosaic_pkg::SZ_WORD;
  assign ic_cpu_req.wstrb = {(CORE_XLEN/8){1'b0}};
  assign ic_cpu_req.wdata = {CORE_XLEN{1'b0}};
  // The instruction port is never atomic (I-039); the fields are driven so the
  // packet is never left half-assigned.
  assign ic_cpu_req.amo    = 1'b0;
  assign ic_cpu_req.amo_op = mosaic_pkg::AMO_ADD;
  assign ic_cpu_req.aq     = 1'b0;
  assign ic_cpu_req.rl     = 1'b0;

  always_ff @(posedge clk) begin
    if (rst) begin
      fetch_pc_q <= mosaic_cfg_pkg::MOSAIC_RESET_VECTOR;
    end else if (fetch_redir_valid) begin
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
    end else if (fetch_pmp_deny_c) begin
      // A denied fetch never becomes an instruction. The PC advances by four --
      // the length of the instruction the machine never saw -- and the value is
      // never architecturally visible: the macro this delivery creates is an
      // instruction access fault, and the trap it raises redirects the front end
      // before anything fetched behind it can execute. Guessing 4 for an
      // instruction whose length is unreadable is therefore safe *because* the
      // instruction traps, and that is the reason rather than an approximation.
      fetch_pc_q <= fetch_pc_q + CORE_XLEN'(4);
    end else if (fetch_rsp_live) begin
      // The PC advances by the instruction's *own* length -- 4 for a 32-bit
      // instruction, 2 for a compressed one -- in the cycle the answer is taken,
      // so a compressed instruction's PC is never "the previous PC plus four".
`ifdef MOSAIC_CORE_MUTANT_FETCH_PC_PLUS4
      // NEGATIVE CONTROL for I-041 (the card's named failure mode): the counter
      // is advanced by four whatever the instruction's length was. The byte
      // after a compressed instruction is skipped and the machine fetches from
      // the middle of the next instruction. CASE=compressed.cross_boundary names
      // the first programme counter that is not the next instruction's.
      fetch_pc_q <= fetch_pc_q + CORE_XLEN'(4);
`else
      fetch_pc_q <= fetch_pc_q + CORE_XLEN'(fetch_rsp_len);
`endif
    end
  end

  mosaic_fetch u_fetch (
      .clk                (clk),
      .rst                (rst),
      .req_valid          (fetch_req_valid_int),
      .req_pc             (fetch_next_pc),
      .req_ready          (fetch_slot_free),
      .req_id             (ic_cpu_req_id),
      .req_epoch          (ic_cpu_req_epoch),
      .rsp_valid          (ic_cpu_rsp_valid),
      .rsp_ready          (ic_cpu_rsp_ready),
      .rsp_squashed       (),
      .rsp_id             (ic_cpu_rsp_id),
      .rsp_epoch          (ic_cpu_rsp_epoch),
      .rsp_data           (ic_cpu_rsp.rdata[31:0]),
      .rsp_len            (ic_cpu_rsp_len),
      .rsp_fault          (ic_cpu_rsp.fault),
      .redirect_valid     (fetch_redir_valid),
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
      .flush              (fetch_redir_valid),
      // The predictor offers no next PC here (`pred_valid` is tied low), so the
      // advisory pair is left unconnected rather than carried as a dead net.
      .pred_next_valid    (),
      .pred_next_pc       (),
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
      .out_len            (fetch_out_len),
      .out_illegal        (fetch_out_illegal),
      .out_fault          (fetch_out_fault),
      .out_cause          (),
      .o_dbg_state        (fetch_dbg_state),
      .outstanding_count  (fetch_outstanding),
      .o_rsp_live         (fetch_rsp_live),
      .o_rsp_len          (fetch_rsp_len),
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
  // 1b. The L1 cache path (I-042) -- instruction side
  // ==========================================================================
  // The instruction cache sits between fetch's request port and the core's
  // instruction memory port. It is a wire until `cache_en_i` is high. The
  // topology and the rules are in rtl/core/mosaic_l1_cache_path.sv's header;
  // this is the wiring.
  mosaic_l1_cache_path #(
      .IS_FETCH      (1'b1),
      .LINE_BYTES    (32),
      .SETS          (8),
      .ADDR_WIDTH    (64),
      .CPU_DATA_WIDTH(64),
      .ID_W          (CORE_REQ_ID_W),
      .EPOCH_W       (CORE_EPOCH_W)
  ) u_icache_path (
      .clk             (clk),
      .rst             (rst),
      .en_i            (cache_en_i),
      .flush_i         (icache_flush),
      .flush_done      (icache_flush_done),
      .cpu_req_valid_i (ic_cpu_req_valid),
      .cpu_req_ready_o (ic_cpu_req_ready),
      .cpu_req_i       (ic_cpu_req),
      .cpu_req_id_i    (ic_cpu_req_id),
      .cpu_req_epoch_i (ic_cpu_req_epoch),
      .cpu_rsp_valid_o (ic_cpu_rsp_valid),
      .cpu_rsp_ready_i (ic_cpu_rsp_ready),
      .cpu_rsp_o       (ic_cpu_rsp),
      .cpu_rsp_id_o    (ic_cpu_rsp_id),
      .cpu_rsp_epoch_o (ic_cpu_rsp_epoch),
      .cpu_rsp_len_o   (ic_cpu_rsp_len),
      .mem_req_valid_o (imem_req_valid),
      .mem_req_ready_i (imem_req_ready),
      .mem_req_o       (imem_req),
      .mem_req_id_o    (imem_req_id),
      .mem_req_epoch_o (imem_req_epoch),
      .mem_rsp_valid_i (imem_rsp_valid),
      .mem_rsp_ready_o (imem_rsp_ready),
      .mem_rsp_i       (imem_rsp),
      .mem_rsp_id_i    (imem_rsp_id),
      .mem_rsp_epoch_i (imem_rsp_epoch),
      .mem_rsp_len_i   (imem_rsp_len),
      .o_hit           (),
      .o_miss          (),
      .o_refill        (),
      .o_writeback     (),
      .o_fault         (),
      .o_cpu_txn       (),
      .o_mem_beat      (),
      .o_line_txn      (),
      .o_bypass_txn    (),
      .dbg_index_i     (3'b0),
      .dbg_valid_o     (),
      .dbg_dirty_o     ()
  );

  // ==========================================================================
  // 2. Decode buffer (2 entries, program order, lane 0 oldest)
  // ==========================================================================
  // I-041: the instruction's length is part of what the front end delivered, so
  // it is what tells the decoder which of the two encodings it is looking at,
  // and it is carried with the control word through the buffer so the retire
  // event can report it. `stopped`/`faulted` deliveries are pushed too, as a
  // fully illegal control word, so the machine stops at the instruction that
  // could not be decoded instead of dropping it and continuing.
  assign fetch_insn16    = (fetch_out_len == 3'd2);

  mosaic_decoder u_dec (
      .insn_raw (fetch_out_bits),
      .insn16   (fetch_insn16),
      .ctl      (dec_ctl_comb)
  );

  // A delivered instruction that fetch reported illegal or faulted does not
  // decode; it is carried as an invalid control word, which dispatch refuses
  // and counts, and the machine stops cleanly at it. Taking the trap is
  // I-019's business, not this package's.
  //
  // WFI is recognised here rather than in mosaic_decoder, and the reason is a
  // division of ownership rather than taste: CASE=decode.rv64im_reserved
  // enumerates every funct3-000 imm12 other than 000/001/302 as reserved, and
  // that enumeration belongs to the decoder's own case. Adding WFI to the
  // decoder would falsify that case's reference model, so the integration
  // recognises the one encoding it needs -- exact, from the raw word, with no
  // other field of WFI's to decode -- and leaves the decoder's illegal set as
  // its owner asserts it. `is_wfi` in the decode control is set here and
  // nowhere else; the decoder never produces it.
  localparam logic [31:0] WFI_WORD = 32'h1050_0073;
  // SRET (I-044) is recognised here for exactly the same reason WFI is: the
  // decoder's case owns the reserved-encoding enumeration and this profile would
  // otherwise have to tell it that 0x102 is legal. Funct12 0x102 with rd = rs1 =
  // 0 and funct3 000 is one word, so the recognition is a comparison against the
  // word rather than a second decode of fields that are all zero.
  localparam logic [31:0] SRET_WORD = 32'h1020_0073;

  // SFENCE.VMA (I-046) is recognised here for the same reason WFI and SRET are:
  // its funct7 (0001001) is outside the decoder's legal set, and the enumeration
  // of that set belongs to the decoder's case. The encoding is matched field by
  // field rather than as one word, because its two operands vary: rs1 is the
  // address, rs2 is the ASID, and either being x0 is the "all" form of that
  // dimension.
  logic        sfence_vma_c;
  logic        sfence_has_va_c;
  logic        sfence_has_asid_c;

  assign sfence_vma_c = (fetch_out_bits[31:25] == 7'b0001001) &&
                        (fetch_out_bits[14:12] == 3'b000) &&
                        (fetch_out_bits[11:7] == 5'd0) &&
                        (fetch_out_bits[6:0] == mosaic_pkg::OP_SYSTEM);
  assign sfence_has_va_c   = (fetch_out_bits[19:15] != 5'd0);
  assign sfence_has_asid_c = (fetch_out_bits[24:20] != 5'd0);

  // A-extension decode (I-039), also done here rather than in mosaic_decoder and
  // for the same ownership reason as WFI: the decoder's case asserts every
  // opcode outside RV64IM is illegal, and opcode 0101111 is outside RV64IM. The
  // integration recognises the encoding it builds from the raw word.
  logic [4:0]          amo_f5_c;
  logic [2:0]          amo_f3_c;
  logic                amo_size_ok_c;
  logic                amo_op_ok_c;
  mosaic_pkg::amo_op_e amo_op_c;
  // LR/SC (I-040) share opcode 0101111 and are recognised by funct5 here, for
  // the same ownership reason the AMO operations are: they are outside RV64IM,
  // so CASE=decode.rv64im_reserved pins them illegal *in the decoder*, and the
  // integration recognises the encodings it builds from the raw word.
  logic                lr_c;
  logic                sc_c;

  assign amo_f5_c = fetch_out_bits[31:27];
  assign amo_f3_c = fetch_out_bits[14:12];
  // funct3 010 is AMO*.W and 011 is AMO*.D; every other funct3 on this opcode
  // (including the W/D-less reserved bytes) is illegal. LR/SC use the same two
  // widths, so the same width check applies to them.
  assign amo_size_ok_c = (amo_f3_c == 3'b010) || (amo_f3_c == 3'b011);
  assign lr_c = (amo_f5_c == 5'b00010);
  assign sc_c = (amo_f5_c == 5'b00011);

  always_comb begin
    amo_op_ok_c = 1'b1;
    amo_op_c    = mosaic_pkg::AMO_ADD;
    case (amo_f5_c)
      5'b00000: amo_op_c = mosaic_pkg::AMO_ADD;
      5'b00001: amo_op_c = mosaic_pkg::AMO_SWAP;
      5'b00100: amo_op_c = mosaic_pkg::AMO_XOR;
      5'b01100: amo_op_c = mosaic_pkg::AMO_AND;
      5'b01000: amo_op_c = mosaic_pkg::AMO_OR;
      5'b10000: amo_op_c = mosaic_pkg::AMO_MIN;
      5'b10100: amo_op_c = mosaic_pkg::AMO_MAX;
      5'b11000: amo_op_c = mosaic_pkg::AMO_MINU;
      5'b11100: amo_op_c = mosaic_pkg::AMO_MAXU;
      default:  amo_op_ok_c = 1'b0;
    endcase
  end

  // ---------------------------------------------------- F/D decode (I-050)
  // Recognised here rather than in mosaic_decoder for exactly the reason WFI,
  // SRET, SFENCE.VMA and AMO are: CASE=decode.rv64im_reserved enumerates every
  // opcode outside RV64IM as illegal, and that enumeration belongs to the
  // decoder's own case. The integration recognises the encodings it needs from
  // the raw word and leaves the decoder's illegal set untouched.
  //
  // Two opcode families are decoded: OP-FP (1010011), which dispatch routes to
  // the shared floating-point unit, and OP-FP-LOAD/OP-FP-STORE (0000111/0100111),
  // which are ordinary memory macros whose register operand lives in the FP
  // namespace. A reserved funct7/funct3/rs2 combination leaves `fp_legal_c` low,
  // so the control word stays fully illegal and the machine stops at it, exactly
  // as for a reserved AMO.
  logic        fp_legal_c;
  logic [6:0]  fp_f7_c;
  logic [2:0]  fp_f3_c;
  logic [4:0]  fp_rs2_c;
  // The encoding fields are continuous assignments, not reads inside the block
  // below: an always_comb that reads a variable before it writes it is a
  // combinational loop, which Verilator reports as UNOPTFLAT.
  assign fp_f7_c  = fetch_out_bits[31:25];
  assign fp_f3_c  = fetch_out_bits[14:12];
  assign fp_rs2_c = fetch_out_bits[24:20];
  mosaic_pkg::fp_op_e fp_op_c;
  logic        fp_fmt_c;
  logic        fp_dst_is_fp_c;
  logic        fp_s1_is_fp_c;
  logic        fp_s2_is_fp_c;
  logic        fp_mod_c;
  logic        fp_uses_rs1_c;
  logic        fp_uses_rs2_c;
  logic        fp_reg_write_c;
  logic [2:0]  fp_rm_c;
  logic        fp_iw_c;
  logic        fp_is_c;
  logic        fp_load_c;
  logic        fp_store_c;
  logic [2:0]  fp_mem_size_c;

  always_comb begin
    fp_legal_c     = 1'b0;
    fp_op_c        = mosaic_pkg::FP_ADD;
    fp_fmt_c       = 1'b0;
    fp_dst_is_fp_c = 1'b0;
    fp_s1_is_fp_c  = 1'b0;
    fp_s2_is_fp_c  = 1'b0;
    fp_mod_c       = 1'b0;
    fp_uses_rs1_c  = 1'b0;
    fp_uses_rs2_c  = 1'b0;
    fp_reg_write_c = 1'b0;
    fp_rm_c        = 3'b000;
    fp_iw_c        = 1'b0;
    fp_is_c        = 1'b0;
    fp_load_c      = 1'b0;
    fp_store_c     = 1'b0;
    fp_mem_size_c  = mosaic_pkg::SZ_WORD;
    if (!fetch_insn16) begin
      fp_rm_c  = fp_f3_c;
      fp_iw_c  = fp_rs2_c[1];
      fp_is_c  = ~fp_rs2_c[0];
      case (fetch_out_bits[6:0])
        mosaic_pkg::OP_FP: begin
          // Arithmetic, min/max, compare, classify, move and convert all read
          // an FP register as rs1; the arithmetic and min/max forms also read
          // rs2. The destination namespace is per operation: an FP-to-integer
          // form (feq/flt/fle, fclass, fcvt.*.x, fmv.x.*) writes an integer
          // register and obeys the x0 discard rule; every other form writes an
          // f-register, where index 0 is the real register f0.
          fp_s1_is_fp_c  = 1'b1;
          fp_uses_rs1_c  = 1'b1;
          fp_dst_is_fp_c = 1'b1;
          fp_reg_write_c = 1'b1;
          fp_mod_c       = 1'b1;
          case (fp_f7_c)
            7'b0000000: begin fp_op_c = mosaic_pkg::FP_ADD;   fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            7'b0000001: begin fp_op_c = mosaic_pkg::FP_ADD;   fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            7'b0000100: begin fp_op_c = mosaic_pkg::FP_SUB;   fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            7'b0000101: begin fp_op_c = mosaic_pkg::FP_SUB;   fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            7'b0001000: begin fp_op_c = mosaic_pkg::FP_MUL;   fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            7'b0001001: begin fp_op_c = mosaic_pkg::FP_MUL;   fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            7'b0001100: begin fp_op_c = mosaic_pkg::FP_DIV;   fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            7'b0001101: begin fp_op_c = mosaic_pkg::FP_DIV;   fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b1; fp_legal_c = 1'b1; end
            // fsqrt is a legal encoding whose datapath I-049 declared absent: it
            // is decoded and routed, and the unit returns the canonical quiet
            // NaN with NV rather than a plausible number.
            7'b0101100: begin fp_op_c = mosaic_pkg::FP_SQRT;  fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b0; fp_legal_c = 1'b1; end
            7'b0101101: begin fp_op_c = mosaic_pkg::FP_SQRT;  fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b0; fp_legal_c = 1'b1; end
            7'b0010000: begin
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b1;
              case (fp_f3_c)
                3'b000: begin fp_op_c = mosaic_pkg::FP_SGNJ;  fp_legal_c = 1'b1; end
                3'b001: begin fp_op_c = mosaic_pkg::FP_SGNJN; fp_legal_c = 1'b1; end
                3'b010: begin fp_op_c = mosaic_pkg::FP_SGNJX; fp_legal_c = 1'b1; end
                default: ;
              endcase
            end
            7'b0010001: begin
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b1;
              case (fp_f3_c)
                3'b000: begin fp_op_c = mosaic_pkg::FP_SGNJ;  fp_legal_c = 1'b1; end
                3'b001: begin fp_op_c = mosaic_pkg::FP_SGNJN; fp_legal_c = 1'b1; end
                3'b010: begin fp_op_c = mosaic_pkg::FP_SGNJX; fp_legal_c = 1'b1; end
                default: ;
              endcase
            end
            7'b0010100: begin
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b1;
              case (fp_f3_c)
                3'b000: begin fp_op_c = mosaic_pkg::FP_MIN; fp_legal_c = 1'b1; end
                3'b001: begin fp_op_c = mosaic_pkg::FP_MAX; fp_legal_c = 1'b1; end
                default: ;
              endcase
            end
            7'b0010101: begin
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b1;
              case (fp_f3_c)
                3'b000: begin fp_op_c = mosaic_pkg::FP_MIN; fp_legal_c = 1'b1; end
                3'b001: begin fp_op_c = mosaic_pkg::FP_MAX; fp_legal_c = 1'b1; end
                default: ;
              endcase
            end
            7'b1010000: begin
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b1;
              fp_dst_is_fp_c = 1'b0;   // the comparison result is an integer
              case (fp_f3_c)
                3'b010: begin fp_op_c = mosaic_pkg::FP_CMP_EQ; fp_legal_c = 1'b1; end
                3'b001: begin fp_op_c = mosaic_pkg::FP_CMP_LT; fp_legal_c = 1'b1; end
                3'b000: begin fp_op_c = mosaic_pkg::FP_CMP_LE; fp_legal_c = 1'b1; end
                default: ;
              endcase
            end
            7'b1010001: begin
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b1;
              fp_dst_is_fp_c = 1'b0;
              case (fp_f3_c)
                3'b010: begin fp_op_c = mosaic_pkg::FP_CMP_EQ; fp_legal_c = 1'b1; end
                3'b001: begin fp_op_c = mosaic_pkg::FP_CMP_LT; fp_legal_c = 1'b1; end
                3'b000: begin fp_op_c = mosaic_pkg::FP_CMP_LE; fp_legal_c = 1'b1; end
                default: ;
              endcase
            end
            7'b1110000: begin
              // fmv.x.w: a pure read of FP state; it writes no FP state, so it
              // does not dirty FS.
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b0;
              fp_dst_is_fp_c = 1'b0; fp_mod_c = 1'b0;
              if (fp_f3_c == 3'b000) begin fp_op_c = mosaic_pkg::FP_MV_X; fp_legal_c = 1'b1; end
            end
            7'b1110001: begin
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b0;
              fp_dst_is_fp_c = 1'b0; fp_mod_c = 1'b0;
              if (fp_f3_c == 3'b000) begin fp_op_c = mosaic_pkg::FP_MV_X; fp_legal_c = 1'b1; end
            end
            7'b1111000: begin
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b0;
              fp_s1_is_fp_c = 1'b0;   // the source is an integer register
              if (fp_f3_c == 3'b000) begin fp_op_c = mosaic_pkg::FP_MV_W; fp_legal_c = 1'b1; end
            end
            7'b1111001: begin
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b0;
              fp_s1_is_fp_c = 1'b0;
              if (fp_f3_c == 3'b000) begin fp_op_c = mosaic_pkg::FP_MV_W; fp_legal_c = 1'b1; end
            end
            7'b1100000: begin
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b0;
              fp_dst_is_fp_c = 1'b0;   // fp -> integer
              if (fp_rs2_c == 5'b00000 || fp_rs2_c == 5'b00001 ||
                  fp_rs2_c == 5'b00010 || fp_rs2_c == 5'b00011) begin
                fp_op_c = mosaic_pkg::FP_CVT_FI; fp_legal_c = 1'b1;
              end
            end
            7'b1100001: begin
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b0;
              fp_dst_is_fp_c = 1'b0;
              if (fp_rs2_c == 5'b00000 || fp_rs2_c == 5'b00001 ||
                  fp_rs2_c == 5'b00010 || fp_rs2_c == 5'b00011) begin
                fp_op_c = mosaic_pkg::FP_CVT_FI; fp_legal_c = 1'b1;
              end
            end
            7'b1101000: begin
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b0;
              fp_s1_is_fp_c = 1'b0;   // integer -> fp
              if (fp_rs2_c == 5'b00000 || fp_rs2_c == 5'b00001 ||
                  fp_rs2_c == 5'b00010 || fp_rs2_c == 5'b00011) begin
                fp_op_c = mosaic_pkg::FP_CVT_IF; fp_legal_c = 1'b1;
              end
            end
            7'b1101001: begin
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b0;
              fp_s1_is_fp_c = 1'b0;
              if (fp_rs2_c == 5'b00000 || fp_rs2_c == 5'b00001 ||
                  fp_rs2_c == 5'b00010 || fp_rs2_c == 5'b00011) begin
                fp_op_c = mosaic_pkg::FP_CVT_IF; fp_legal_c = 1'b1;
              end
            end
            7'b0100000: begin
              // fcvt.s.d: double -> single, rounding in the single format.
              fp_fmt_c = 1'b1; fp_uses_rs2_c = 1'b0;
              if (fp_rs2_c == 5'b00001) begin fp_op_c = mosaic_pkg::FP_CVT_FS; fp_legal_c = 1'b1; end
            end
            7'b0100001: begin
              // fcvt.d.s: single -> double, exact.
              fp_fmt_c = 1'b0; fp_uses_rs2_c = 1'b0;
              if (fp_rs2_c == 5'b00000) begin fp_op_c = mosaic_pkg::FP_CVT_SF; fp_legal_c = 1'b1; end
            end
            default: ;
          endcase
          // The second source of every OP-FP form that *has* one is an
          // f-register: the arithmetic, min/max, sign-injection and compare
          // families all name fs2. `fp_uses_rs2_c` is set low exactly for the
          // forms whose rs2 is a field rather than a register (the conversions,
          // whose rs2 encodes the destination type, and the moves and fclass) --
          // and for those the namespace does not matter, because dispatch
          // presents an unused source as address x0.
          //
          // Leaving this at its default (0) read fs2 from the INTEGER map, so
          // `fadd.s f3, f1, f2` added f1 to x2 -- zero in a program that never
          // touches x2, and whatever happened to be in x2 otherwise. The
          // single-source forms hid it, and so did any pair whose integer
          // registers happened to hold the right bits; it is exactly the class of
          // defect this case exists to find. CASE=fp.precise_flags_and_boxing
          // found it: two load-produced operands read as one plus zero.
          fp_s2_is_fp_c = fp_uses_rs2_c;
        end
        mosaic_pkg::OP_LOAD_FP: begin
          // flw/fld. The load's address base is an integer register; its result
          // is written to an f-register. flw's 32-bit result is NaN-boxed on the
          // way in (the core ORs the upper half), so the register always holds a
          // boxed single.
          if (fp_f3_c == 3'b010) begin
            fp_load_c = 1'b1; fp_mem_size_c = mosaic_pkg::SZ_WORD; fp_legal_c = 1'b1;
          end else if (fp_f3_c == 3'b011) begin
            fp_load_c = 1'b1; fp_mem_size_c = mosaic_pkg::SZ_DBL; fp_legal_c = 1'b1;
          end
          fp_dst_is_fp_c = 1'b1;
          fp_uses_rs1_c  = 1'b1;
          fp_reg_write_c = 1'b1;
          fp_mod_c       = 1'b1;
        end
        mosaic_pkg::OP_STORE_FP: begin
          // fsw/fsd. The stored datum is an f-register; the address base is an
          // integer register. A store reads FP state without modifying it, so it
          // does not dirty FS.
          if (fp_f3_c == 3'b010) begin
            fp_store_c = 1'b1; fp_mem_size_c = mosaic_pkg::SZ_WORD; fp_legal_c = 1'b1;
          end else if (fp_f3_c == 3'b011) begin
            fp_store_c = 1'b1; fp_mem_size_c = mosaic_pkg::SZ_DBL; fp_legal_c = 1'b1;
          end
          fp_s2_is_fp_c  = 1'b1;
          fp_uses_rs1_c  = 1'b1;
          fp_uses_rs2_c  = 1'b1;
          fp_reg_write_c = 1'b0;
          fp_mod_c       = 1'b0;
        end
        default: ;
      endcase
    end
  end

  // ==========================================================================
  // V decode (I-059)
  // ==========================================================================
  // Recognised here rather than in mosaic_decoder for exactly the reason WFI,
  // SRET, SFENCE.VMA, AMO and OP-FP are: CASE=decode.rv64im_reserved enumerates
  // every opcode outside RV64IM as illegal, and that enumeration belongs to the
  // decoder's own case. The integration recognises the encodings it needs from
  // the raw word and leaves the decoder's illegal set untouched.
  //
  // What is decoded here is the *structure* of the encoding -- which family it
  // names and which registers and immediate it carries. Its *legality* is not
  // decided here, because vtype is architectural state that vset{i}vl{i}
  // establishes, and this front end is speculative: the descriptor's legality
  // matrix is queried at the ROB head (section 7b), where the configuration in
  // effect is by construction the one every older instruction left.
  //
  // Supported: vsetvli/vsetivli/vsetvl; unit-stride vector loads and stores;
  // the integer arithmetic and logical forms vadd/vsub/vand/vor/vxor in their
  // vv/vx/vi shapes. Every other OP-V encoding (multiply, divide, widening,
  // narrowing, extension, reduction, mask, slide, FP, the segmented/indexed/
  // whole/mask memory modes, vector AMO) is refused -- the machine stops at it
  // exactly as it stops at any undecoded instruction -- and the report names
  // each as unreachable.
  logic        vec_legal_c;
  logic [5:0]  vec_f6_c;
  logic [2:0]  vec_f3_c;
  logic [2:0]  vec_kind_c;
  logic [4:0]  vec_class_c;
  logic [1:0]  vec_vset_kind_c;
  logic [4:0]  vec_vset_uimm_c;
  logic [10:0] vec_vtypei_c;
  logic [4:0]  vec_vd_c, vec_vs1_c, vec_vs2_c;
  logic        vec_mask_en_c;
  logic [4:0]  vec_family_c;
  logic [3:0]  vec_op_c;
  logic [1:0]  vec_form_c;
  logic [3:0]  vec_lsu_mode_c;
  logic        vec_lsu_we_c;
  logic [2:0]  vec_eew_sew_c;
  logic [63:0] vec_imm_c;

  always_comb begin
    vec_legal_c     = 1'b0;
    vec_f6_c        = fetch_out_bits[31:26];
    vec_f3_c        = fetch_out_bits[14:12];
    vec_kind_c      = 3'd0;
    vec_class_c     = 5'd16;   // VOP_VSET
    vec_vset_kind_c = 2'd0;
    vec_vset_uimm_c = 5'd0;
    vec_vtypei_c    = 11'd0;
    vec_vd_c        = fetch_out_bits[11:7];
    vec_vs1_c       = fetch_out_bits[19:15];
    vec_vs2_c       = fetch_out_bits[24:20];
    vec_mask_en_c   = 1'b0;
    vec_family_c    = 5'd0;
    vec_op_c        = 4'd0;
    vec_form_c      = 2'd0;
    vec_lsu_mode_c  = 4'd0;
    vec_lsu_we_c    = 1'b0;
    vec_eew_sew_c   = 3'd3;
    vec_imm_c       = 64'd0;
    if (!fetch_insn16) begin
      case (fetch_out_bits[6:0])
        mosaic_pkg::OP_V: begin
          if (vec_f3_c == 3'b111) begin
            // ---------------------------------------------------- vset{i}vl{i}
            vec_kind_c  = 3'd0;
            vec_class_c = 5'd16;   // VOP_VSET
            if (fetch_out_bits[31] == 1'b0) begin
              // vsetvli rd, rs1, vtypei: vtypei = insn[30:20].
              vec_vset_kind_c = 2'd0;
              vec_vtypei_c    = fetch_out_bits[30:20];
              vec_legal_c     = 1'b1;
            end else if (fetch_out_bits[31:30] == 2'b11) begin
              // vsetivli rd, uimm, zimm: AVL = uimm, vtypei = zimm[9:0].
              vec_vset_kind_c = 2'd1;
              vec_vset_uimm_c = fetch_out_bits[19:15];
              vec_vtypei_c    = {1'b0, fetch_out_bits[29:20]};
              vec_legal_c     = 1'b1;
            end else if (fetch_out_bits[31:30] == 2'b10) begin
              // vsetvl rd, rs1, rs2: the vtype argument is x[rs2].
              vec_vset_kind_c = 2'd2;
              vec_legal_c     = 1'b1;
            end
          end else if ((vec_f3_c == 3'b000) || (vec_f3_c == 3'b100) ||
                       (vec_f3_c == 3'b011)) begin
            // ------------------------------------------------- integer arith
            // OPIVV (000), OPIVX (100), OPIVI (011). The OPFVV/OPMVV/OPFVF/
            // OPMVX forms are vector FP and mask-to-mask, which this
            // integration does not wire.
            vec_kind_c   = 3'd1;
            vec_class_c  = 5'd0;   // VOP_IVV
            vec_mask_en_c = !fetch_out_bits[25];
            vec_form_c   = (vec_f3_c == 3'b000) ? 2'd0
                         : (vec_f3_c == 3'b100) ? 2'd1 : 2'd2;
            case (vec_f6_c)
              6'b000000: begin vec_family_c = 5'd0; vec_op_c = 4'd0; vec_legal_c = 1'b1; end  // vadd
              6'b000010: begin                                                                 // vsub
                if (vec_form_c != 2'd2) begin
                  vec_family_c = 5'd0; vec_op_c = 4'd1; vec_legal_c = 1'b1;
                end
              end
              6'b001001: begin vec_family_c = 5'd6; vec_op_c = 4'd0; vec_legal_c = 1'b1; end  // vand
              6'b001010: begin vec_family_c = 5'd6; vec_op_c = 4'd1; vec_legal_c = 1'b1; end  // vor
              6'b001011: begin vec_family_c = 5'd6; vec_op_c = 4'd2; vec_legal_c = 1'b1; end  // vxor
              default: ;
            endcase
            if (vec_form_c == 2'd2) begin
              // The immediate form's scalar operand is the sign-extended 5-bit
              // `imm` field.
              vec_imm_c = {{59{fetch_out_bits[19]}}, fetch_out_bits[19:15]};
            end
          end
        end
        mosaic_pkg::OP_LOAD_FP: begin
          // A vector load reuses the scalar FP load's opcode; funct3 carries the
          // element width and bits[31:26] are nf/mew/mop. Only unit-stride with
          // no fields (nf=0, mew=0, mop=00) is wired; funct3 010/011 are the
          // scalar flw/fld and are left to the FP arm below.
          if ((vec_f6_c == 6'd0) &&
              ((vec_f3_c == 3'b000) || (vec_f3_c == 3'b101) ||
               (vec_f3_c == 3'b110) || (vec_f3_c == 3'b111))) begin
            vec_kind_c     = 3'd2;
            vec_class_c    = 5'd13;  // VOP_VLOAD
            vec_lsu_mode_c = 4'd0;
            vec_lsu_we_c   = 1'b0;
            vec_mask_en_c  = !fetch_out_bits[25];
            vec_eew_sew_c  = (vec_f3_c == 3'b000) ? 3'd3
                           : (vec_f3_c == 3'b101) ? 3'd4
                           : (vec_f3_c == 3'b110) ? 3'd5 : 3'd6;
            vec_legal_c    = 1'b1;
          end
        end
        mosaic_pkg::OP_STORE_FP: begin
          if ((vec_f6_c == 6'd0) &&
              ((vec_f3_c == 3'b000) || (vec_f3_c == 3'b101) ||
               (vec_f3_c == 3'b110) || (vec_f3_c == 3'b111))) begin
            vec_kind_c     = 3'd3;
            vec_class_c    = 5'd14;  // VOP_VSTORE
            vec_lsu_mode_c = 4'd0;
            vec_lsu_we_c   = 1'b1;
            vec_mask_en_c  = !fetch_out_bits[25];
            vec_eew_sew_c  = (vec_f3_c == 3'b000) ? 3'd3
                           : (vec_f3_c == 3'b101) ? 3'd4
                           : (vec_f3_c == 3'b110) ? 3'd5 : 3'd6;
            vec_legal_c    = 1'b1;
          end
        end
        default: ;
      endcase
    end
  end

  always_comb begin
    dbuf_ctl_new = dec_ctl_comb;
    if (fetch_pmp_deny_c) begin
      // The front end was not allowed to read this address, so there is no
      // instruction to decode and no encoding to refuse: the macro is a system
      // instruction whose whole effect is an instruction access fault at its own
      // PC. It is *not* an illegal instruction (the machine has not even seen
      // the bits), which is why it does not take the refusal path above; the
      // system unit raises cause 1 with the PC as the trap value.
      dbuf_ctl_new.valid          = 1'b1;
      dbuf_ctl_new.illegal        = 1'b0;
      dbuf_ctl_new.is_system      = 1'b1;
      dbuf_ctl_new.is_fetch_fault = 1'b1;
    end else if (fetch_out_illegal || fetch_out_fault) begin
      dbuf_ctl_new.valid   = 1'b0;
      dbuf_ctl_new.illegal = 1'b1;
    end else if (fetch_out_bits == WFI_WORD) begin
      // WFI has no operands, writes nothing, and reads no CSR: `ctl` starts as
      // the decoder's fully-illegal constant, and only the three fields that say
      // "a system instruction, do not stop" change.
      dbuf_ctl_new.valid     = 1'b1;
      dbuf_ctl_new.illegal   = 1'b0;
      dbuf_ctl_new.is_system = 1'b1;
      dbuf_ctl_new.is_wfi    = 1'b1;
    end else if (fetch_out_bits == SRET_WORD) begin
      // SRET is a system instruction with no operands and no destination, so it
      // is stated here the same way WFI is and for the same ownership reason.
      dbuf_ctl_new.valid     = 1'b1;
      dbuf_ctl_new.illegal   = 1'b0;
      dbuf_ctl_new.is_system = 1'b1;
      dbuf_ctl_new.is_sret   = 1'b1;
    end else if (sfence_vma_c) begin
      // SFENCE.VMA (I-046). A system instruction with two source registers and
      // no destination; the operands are captured through the ordinary rename
      // path, so a value still in flight cannot be sampled stale.
      dbuf_ctl_new.valid           = 1'b1;
      dbuf_ctl_new.illegal         = 1'b0;
      dbuf_ctl_new.is_system       = 1'b1;
      dbuf_ctl_new.is_sfence_vma   = 1'b1;
      dbuf_ctl_new.sfence_has_va   = sfence_has_va_c;
      dbuf_ctl_new.sfence_has_asid = sfence_has_asid_c;
      dbuf_ctl_new.uses_rs1        = sfence_has_va_c;
      dbuf_ctl_new.uses_rs2        = sfence_has_asid_c;
      dbuf_ctl_new.rs1             = fetch_out_bits[19:15];
      dbuf_ctl_new.rs2             = fetch_out_bits[24:20];
      dbuf_ctl_new.rd              = 5'd0;
      dbuf_ctl_new.reg_write       = 1'b0;
    end else if ((fetch_out_bits[6:0] == mosaic_pkg::OP_AMO) &&
                 (amo_op_ok_c || lr_c || sc_c)) begin
      // A legal AMO/lr/sc always overwrites the illegal constant's fields; the
      // reserved cases (bad funct3, or a funct5 that is none of the nine AMO
      // operations, lr or sc) are refused by not entering this arm, so the
      // control word stays fully illegal rather than half-decoded.
      if (amo_size_ok_c) begin
        dbuf_ctl_new.valid      = 1'b1;
        dbuf_ctl_new.illegal    = 1'b0;
        dbuf_ctl_new.uses_rs1   = 1'b1;
        // `lr` has no second source: its rs2 field is reserved. Leaving it
        // unused is what makes rename present it as a ready x0 rather than
        // waiting on a register the instruction never reads.
        dbuf_ctl_new.uses_rs2   = !lr_c;
        dbuf_ctl_new.uses_imm   = 1'b0;   // the address is rs1, there is no offset
        dbuf_ctl_new.rs1        = fetch_out_bits[19:15];
        dbuf_ctl_new.rs2        = fetch_out_bits[24:20];
        dbuf_ctl_new.rd         = fetch_out_bits[11:7];
        dbuf_ctl_new.reg_write  = (fetch_out_bits[11:7] != 5'd0);
        dbuf_ctl_new.mem_kind   = lr_c ? mosaic_pkg::MEM_LR
                                : sc_c ? mosaic_pkg::MEM_SC
                                : mosaic_pkg::MEM_AMO;
        dbuf_ctl_new.amo_op     = amo_op_c;
        dbuf_ctl_new.mem_size   = (amo_f3_c == 3'b010) ? mosaic_pkg::SZ_WORD
                                                       : mosaic_pkg::SZ_DBL;
        // The old value is returned sign-extended for AMO*.W and lr.w, exactly
        // as `lw` returns it. The *operation*'s signedness is `amo_op`
        // (MIN/MAX vs MINU/MAXU), not this bit; folding the two is the named
        // signed/unsigned-boundary defect. An SC's result is a status in
        // {0,1}, so its extension is immaterial -- it is zero-extended.
        dbuf_ctl_new.mem_signed = !sc_c;
        dbuf_ctl_new.amo_aq     = fetch_out_bits[26];
        dbuf_ctl_new.amo_rl     = fetch_out_bits[25];
        dbuf_ctl_new.is_lr      = lr_c;
        dbuf_ctl_new.is_sc      = sc_c;
        dbuf_ctl_new.imm        = 64'd0;
      end
    end else if (vec_legal_c) begin
      // V (I-059). The structure is decoded; the legality of the combination
      // against vtype is decided at the ROB head by the descriptor's matrix.
      dbuf_ctl_new.valid             = 1'b1;
      dbuf_ctl_new.illegal           = 1'b0;
      dbuf_ctl_new.is_vec            = 1'b1;
      dbuf_ctl_new.vec_class         = vec_class_c;
      dbuf_ctl_new.vec_kind          = vec_kind_c;
      dbuf_ctl_new.vec_vset_kind     = vec_vset_kind_c;
      dbuf_ctl_new.vec_vset_uimm     = vec_vset_uimm_c;
      dbuf_ctl_new.vec_vtypei        = vec_vtypei_c;
      dbuf_ctl_new.vec_vd            = vec_vd_c;
      dbuf_ctl_new.vec_vs1           = vec_vs1_c;
      dbuf_ctl_new.vec_vs2           = vec_vs2_c;
      dbuf_ctl_new.vec_mask_en       = vec_mask_en_c;
      dbuf_ctl_new.vec_family        = vec_family_c;
      dbuf_ctl_new.vec_op            = vec_op_c;
      dbuf_ctl_new.vec_form          = vec_form_c;
      dbuf_ctl_new.vec_lsu_mode      = vec_lsu_mode_c;
      dbuf_ctl_new.vec_lsu_we        = vec_lsu_we_c;
      dbuf_ctl_new.vec_lsu_ordered   = 1'b0;
      dbuf_ctl_new.vec_lsu_fof       = 1'b0;
      dbuf_ctl_new.vec_nf            = 4'd1;
      dbuf_ctl_new.vec_idx_sew       = 3'd0;
      dbuf_ctl_new.vec_eew_sew       = vec_eew_sew_c;
      dbuf_ctl_new.vec_imm           = vec_imm_c;
      dbuf_ctl_new.mem_kind          = mosaic_pkg::MEM_NONE;
      dbuf_ctl_new.alu_op            = mosaic_pkg::ALU_PASSB;
      dbuf_ctl_new.is_system         = 1'b0;
      dbuf_ctl_new.is_miscmem        = 1'b0;
      dbuf_ctl_new.uses_imm          = 1'b0;
      dbuf_ctl_new.imm               = 64'd0;
      if (vec_kind_c == 3'd0) begin
        // vset{i}vl{i}: the architectural destination is the integer rd, and the
        // only integer source is the AVL in x[rs1] (vsetvli/vsetvl).
        dbuf_ctl_new.rd        = vec_vd_c;
        dbuf_ctl_new.reg_write = (vec_vd_c != 5'd0);
        dbuf_ctl_new.uses_rs1  = (vec_vset_kind_c != 2'd1);
        dbuf_ctl_new.uses_rs2  = (vec_vset_kind_c == 2'd2);
        dbuf_ctl_new.rs1       = (vec_vset_kind_c == 2'd1) ? 5'd0 : vec_vs1_c;
        dbuf_ctl_new.rs2       = vec_vs2_c;
      end else if (vec_kind_c == 3'd1) begin
        // Integer arithmetic: no integer destination (the result is a vector
        // register, which the VRF owns). Only the .vx form names an integer
        // source; .vv reads two vector registers and .vi an immediate.
        dbuf_ctl_new.rd        = 5'd0;
        dbuf_ctl_new.reg_write = 1'b0;
        dbuf_ctl_new.uses_rs1  = (vec_form_c == 2'd1);
        dbuf_ctl_new.uses_rs2  = 1'b0;
        dbuf_ctl_new.rs1       = (vec_form_c == 2'd1) ? vec_vs1_c : 5'd0;
        dbuf_ctl_new.rs2       = 5'd0;
      end else begin
        // Vector load or store: the base address is x[rs1]; neither writes an
        // integer register. The element register is named by the instruction's
        // `vd` field and is owned by the VRF.
        dbuf_ctl_new.rd        = 5'd0;
        dbuf_ctl_new.reg_write = 1'b0;
        dbuf_ctl_new.uses_rs1  = 1'b1;
        dbuf_ctl_new.uses_rs2  = 1'b0;
        dbuf_ctl_new.rs1       = vec_vs1_c;
        dbuf_ctl_new.rs2       = 5'd0;
      end
    end else if (fp_load_c) begin
      // flw / fld (I-050). An ordinary memory load whose register operand lives
      // in the FP namespace; the address base is an integer register. The result
      // is zero-extended and the core NaN-boxes a word load on the way back.
      dbuf_ctl_new.valid             = 1'b1;
      dbuf_ctl_new.illegal           = 1'b0;
      dbuf_ctl_new.uses_rs1          = 1'b1;
      dbuf_ctl_new.uses_rs2          = 1'b0;
      dbuf_ctl_new.uses_imm          = 1'b1;
      dbuf_ctl_new.rs1               = fetch_out_bits[19:15];
      dbuf_ctl_new.rs2               = 5'd0;
      dbuf_ctl_new.rd                = fetch_out_bits[11:7];
      dbuf_ctl_new.reg_write         = 1'b1;
      dbuf_ctl_new.mem_kind          = mosaic_pkg::MEM_LOAD;
      dbuf_ctl_new.mem_size          = fp_mem_size_c;
      dbuf_ctl_new.mem_signed        = 1'b0;
      dbuf_ctl_new.imm               = {{52{fetch_out_bits[31]}}, fetch_out_bits[31:20]};
      dbuf_ctl_new.fp_dst_fp         = 1'b1;
      dbuf_ctl_new.fp_src1_fp        = 1'b0;
      dbuf_ctl_new.fp_src2_fp        = 1'b0;
      dbuf_ctl_new.fp_modifies_state = 1'b1;
    end else if (fp_store_c) begin
      // fsw / fsd. The stored datum is an f-register; a store reads FP state
      // without modifying it.
      dbuf_ctl_new.valid             = 1'b1;
      dbuf_ctl_new.illegal           = 1'b0;
      dbuf_ctl_new.uses_rs1          = 1'b1;
      dbuf_ctl_new.uses_rs2          = 1'b1;
      dbuf_ctl_new.uses_imm          = 1'b1;
      dbuf_ctl_new.rs1               = fetch_out_bits[19:15];
      dbuf_ctl_new.rs2               = fetch_out_bits[24:20];
      dbuf_ctl_new.rd                = 5'd0;
      dbuf_ctl_new.reg_write         = 1'b0;
      dbuf_ctl_new.mem_kind          = mosaic_pkg::MEM_STORE;
      dbuf_ctl_new.mem_size          = fp_mem_size_c;
      dbuf_ctl_new.mem_signed        = 1'b0;
      dbuf_ctl_new.imm               = {{52{fetch_out_bits[31]}}, fetch_out_bits[31:25],
                                         fetch_out_bits[11:7]};
      dbuf_ctl_new.fp_dst_fp         = 1'b0;
      dbuf_ctl_new.fp_src1_fp        = 1'b0;
      dbuf_ctl_new.fp_src2_fp        = 1'b1;
      dbuf_ctl_new.fp_modifies_state = 1'b0;
    end else if (fp_legal_c) begin
      // OP-FP. The reserved combinations (a bad funct7, a bad funct3 for the
      // sign-injection/min/max/compare forms, a bad rs2 for a conversion) leave
      // `fp_legal_c` low and fall out of this chain, so the control word stays
      // fully illegal rather than half-decoded.
      dbuf_ctl_new.valid             = 1'b1;
      dbuf_ctl_new.illegal           = 1'b0;
      dbuf_ctl_new.is_fp             = 1'b1;
      dbuf_ctl_new.fp_op             = fp_op_c;
      dbuf_ctl_new.fp_fmt            = fp_fmt_c;
      dbuf_ctl_new.fp_rm             = fp_rm_c;
      dbuf_ctl_new.fp_dst_fp         = fp_dst_is_fp_c;
      dbuf_ctl_new.fp_src1_fp        = fp_s1_is_fp_c;
      dbuf_ctl_new.fp_src2_fp        = fp_s2_is_fp_c;
      dbuf_ctl_new.fp_iw             = fp_iw_c;
      dbuf_ctl_new.fp_is             = fp_is_c;
      dbuf_ctl_new.fp_modifies_state = fp_mod_c;
      dbuf_ctl_new.uses_rs1          = fp_uses_rs1_c;
      dbuf_ctl_new.uses_rs2          = fp_uses_rs2_c;
      dbuf_ctl_new.uses_imm          = 1'b0;
      dbuf_ctl_new.rs1               = fetch_out_bits[19:15];
      dbuf_ctl_new.rs2               = fetch_out_bits[24:20];
      dbuf_ctl_new.rd                = fetch_out_bits[11:7];
      // An integer destination for a comparison, classify, fp-to-integer
      // conversion or fmv.x.* obeys the x0 discard rule; an FP destination does
      // not, because f0 is a real register.
      dbuf_ctl_new.reg_write         = fp_reg_write_c &&
                                        (fp_dst_is_fp_c ? 1'b1
                                         : (fetch_out_bits[11:7] != 5'd0));
      dbuf_ctl_new.imm               = 64'd0;
    end
  end

  assign dbuf_take = disp_take;
  assign dbuf_room = (dbuf_cnt < 2'd2) || dbuf_take;
  // Every kind of delivery advances the buffer: an instruction (out_valid), and
  // an undecodable response (out_illegal / out_fault) as a control word that is
  // fully illegal, so dispatch refuses it and the machine stops *at that
  // instruction* with its PC rather than skipping over it. A delivery that is
  // never pushed is an instruction the machine silently executed past.
  assign dbuf_push = (fetch_out_valid || fetch_out_illegal || fetch_out_fault ||
                      fetch_pmp_deny_c) &&
                     dbuf_room && !core_stop && !wfi_halt;
  assign fetch_out_ready = dbuf_room && !core_stop && !wfi_halt;

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
    dbuf_len_n   = dbuf_len;
    dbuf_bits_n  = dbuf_bits;
    if (dbuf_take) begin
      dbuf_valid_n[0] = dbuf_valid[1];
      dbuf_pc_n[0]    = dbuf_pc[1];
      dbuf_ctl_n[0]   = dbuf_ctl[1];
      dbuf_len_n[0]   = dbuf_len[1];
      dbuf_bits_n[0]  = dbuf_bits[1];
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
      // A denied fetch carries no response, so its PC is the one the request was
      // about to be issued for.
      dbuf_pc_n[dbuf_push_at]    = fetch_pmp_deny_c ? fetch_next_pc : fetch_out_pc;
      dbuf_ctl_n[dbuf_push_at]   = dbuf_ctl_new;
      // The instruction's own length and its own bits travel with it, so the
      // retire event can report what the instruction *was*, not what the
      // decoder made of it.
      dbuf_len_n[dbuf_push_at]   = fetch_out_len;
      dbuf_bits_n[dbuf_push_at]  = fetch_out_bits;
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      dbuf_cnt      <= 2'd0;
      dbuf_valid[0] <= 1'b0;
      dbuf_valid[1] <= 1'b0;
    end else if (dbuf_purge || core_stop) begin
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
      if (dbuf_purge) begin
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
      dbuf_len      <= dbuf_len_n;
      dbuf_bits     <= dbuf_bits_n;
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
      .alloc_is_fp      (ren_alloc_is_fp),
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
      // I-032, wired by I-090: while the fabric is on, the destination is
      // biased to the home bank of the cluster the macro was routed to
      // (producer locality -- cluster c's home bank is bank c). It is a
      // *preference with a fallback*, proven by CASE=rename.bank_bias_exhaustion
      // to change only which legal free tag is chosen and never whether an
      // allocation succeeds. With the fabric off it is the pre-I-032 allocator,
      // bit for bit.
      .alloc_bias_en    (fab_dyn_bank),
      .alloc_bias_bank  ({1'b0, fab_alloc_cluster}),
      .rs1_addr         (ren_rs1_addr),
      .rs2_addr         (ren_rs2_addr),
      .rs1_is_fp        (ren_rs1_is_fp),
      .rs2_is_fp        (ren_rs2_is_fp),
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
      .commit_is_fp     (ren_commit_is_fp),
      .commit_tag       (ren_commit_tag),
      .commit_gen       (ren_commit_gen),
      .commit_accepted  (),
      .commit_x0_dropped(),
      .commit2_valid    (ren_commit2_valid),
      .commit2_rd       (ren_commit2_rd),
      .commit2_is_fp    (ren_commit2_is_fp),
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
      .flush_restore    (ren_flush_restore),
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
      .dbg_spec_map_fp  (),
      .dbg_cmt_map_fp   (),
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
      .wr_len          ({3'd0, desc_wr_len}),
      .wr_insn         ({32'd0, desc_wr_insn}),
      .rd_index0       (rob_head_index),
      .rd_index1       (rob_head1_index),
      .rd_valid0       (),
      .rd_tag0         (),
      .rd_gen0         (desc_gen0),
      .rd_rd0          (desc_rd0),
      .rd_reg_we0      (desc_reg_we0),
      .rd_is_store0    (desc_is_store0),
      .rd_len0         (desc_len0),
      .rd_insn0        (desc_insn0),
      .rd_valid1       (),
      .rd_tag1         (),
      .rd_gen1         (desc_gen1),
      .rd_rd1          (desc_rd1),
      .rd_reg_we1      (desc_reg_we1),
      .rd_is_store1    (desc_is_store1),
      .rd_len1         (desc_len1),
      .rd_insn1        (desc_insn1),
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
      .retire_req      (ret_req_gated),
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
      // F/D (I-050): cluster 0's grant is the one routed to the shared FP unit.
      .fp_req_valid    (fp_req_valid),
      .fp_req_ready    (fp_req_ready),
      .fp_req_op       (fp_req_op),
      .fp_req_fmt      (fp_req_fmt),
      .fp_req_rm       (fp_req_rm),
      .fp_req_dst_fp   (fp_req_dst_fp),
      .fp_req_src1_fp  (fp_req_src1_fp),
      .fp_req_src2_fp  (fp_req_src2_fp),
      .fp_req_iw       (fp_req_iw),
      .fp_req_is       (fp_req_is),
      .fp_req_a        (fp_req_a),
      .fp_req_b        (fp_req_b),
      .fp_req_rob_index(fp_req_rob_index),
      .fp_req_rob_gen  (fp_req_rob_gen),
      .fp_req_uop_index(fp_req_uop_index),
      .fp_req_dst_tag  (fp_req_dst_tag),
      .fp_req_dst_gen  (fp_req_dst_gen),
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
      .o_wu_miss_ctr   (),
      .fab_dyn         (fab_dyn_bp),
      .o_wu2_matched   (c0_wu2_matched),
      .o_bp_captured_ctr(c0_bp_captured),
      .o_bp_unauth_ctr (c0_bp_unauth),
      .o_bp_flush_ctr  (c0_bp_flush)
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
      // F/D (I-050): cluster 1's queue never receives an FP macro (dispatch
      // routes them all to cluster 0), so the port is quiescent and the unit
      // always "ready".
      .fp_req_valid    (),
      .fp_req_ready    (1'b1),
      .fp_req_op       (),
      .fp_req_fmt      (),
      .fp_req_rm       (),
      .fp_req_dst_fp   (),
      .fp_req_src1_fp  (),
      .fp_req_src2_fp  (),
      .fp_req_iw       (),
      .fp_req_is       (),
      .fp_req_a        (),
      .fp_req_b        (),
      .fp_req_rob_index(),
      .fp_req_rob_gen  (),
      .fp_req_uop_index(),
      .fp_req_dst_tag  (),
      .fp_req_dst_gen  (),
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
      .o_wu_miss_ctr   (),
      .fab_dyn         (fab_dyn_bp),
      .o_wu2_matched   (c1_wu2_matched),
      .o_bp_captured_ctr(c1_bp_captured),
      .o_bp_unauth_ctr (c1_bp_unauth),
      .o_bp_flush_ctr  (c1_bp_flush)
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
  // 7a. The shared floating-point unit and the FP flag sideband (I-050)
  // ==========================================================================
  // The FP unit is reached exactly as the shared MUL/DIV unit is: cluster 0
  // grants an FP macro to its request port, the unit executes it with the
  // operands the issue queue captured, and the completion comes back as an
  // ordinary writeback event. It is speculative -- the operation runs as soon as
  // its sources are ready -- which is what makes "an executed but squashed FP
  // operation" a real state for the precise-fflags rule to handle.
  mosaic_fp_unit u_fp (
      .clk             (clk),
      .rst             (rst),
      .req_valid       (fp_req_valid),
      .req_ready       (fp_req_ready),
      .req_op          (fp_req_op),
      .req_fmt         (fp_req_fmt),
      .req_rm          (fp_req_rm),
      .req_dst_fp      (fp_req_dst_fp),
      .req_src1_fp     (fp_req_src1_fp),
      .req_src2_fp     (fp_req_src2_fp),
      .req_iw          (fp_req_iw),
      .req_is          (fp_req_is),
      .req_a           (fp_req_a),
      .req_b           (fp_req_b),
      .req_rob_index   (fp_req_rob_index),
      .req_rob_gen     (fp_req_rob_gen),
      .req_uop_index   (fp_req_uop_index),
      .req_dst_tag     (fp_req_dst_tag),
      .req_dst_gen     (fp_req_dst_gen),
      .frm_i           (o_csr_frm),
      .flush_i         (redirect_valid),
      .wb_ev           (fp_wb_ev),
      .wb_valid        (fp_wb_valid),
      .wb_ready        (fp_wb_ready),
      .wb_fflags       (fp_wb_fflags),
      .wb_modifies_fs  (),
      .o_busy          (),
      .o_issue_ctr     (o_fp_issue_ctr),
      .o_commit_ctr    (o_fp_commit_ctr),
      .o_flags_ctr     (o_fp_flags_ctr)
  );

  assign o_fp_merge_ctr = fp_merge_ctr;

  // ---------------------------------------------------- the vector evidence
  assign o_vec_vtype    = vec_vtype;
  assign o_vec_vl       = vec_vl;
  assign o_vec_vstart   = vec_vstart;
  assign o_vec_vcsr     = vec_vcsr;
`ifdef MOSAIC_LANE_MUTANT_VLEN_LEAK
  // NEGATIVE CONTROL: the runtime lane quota leaks into the architectural VLEN
  // read-back. The lane count must only change throughput, never VLEN; with
  // this define `vlenb` reads 16 + quota and the resize case's invariance check
  // fails with the value the DUT reported.
  assign o_vec_vlenb    = vec_vlenb + {60'd0, lane_quota};
`else
  assign o_vec_vlenb    = vec_vlenb;
`endif
  assign o_vec_vlmax    = vec_vlmax;
  assign o_vec_vill     = vec_vill;
  assign o_vec_macro_ctr = vec_macro_ctr;
  assign o_vec_elem_ctr  = vec_elem_ctr;
  assign o_vec_trap_ctr  = vec_trap_ctr;
  assign o_vec_retire_ctr = vec_retire_ctr;
  assign o_vec_fault_ctr  = vec_fault_ctr;
  assign o_vec_lsu_req_ctr = vec_lsu_req_ctr;
  assign o_vec_chain_accept_ctr = {16'd0, vec_chain_pkt_accept_ctr};
  assign o_vec_chain_refuse_ctr = {16'd0, vec_chain_pkt_refuse_ctr};
  assign o_vec_desc_alloc_ctr   = {16'd0, vec_desc_alloc_ctr};
  assign o_vec_desc_release_ctr = {16'd0, vec_desc_release_ctr};
  assign o_vec_alu_elems        = {24'd0, vec_alu_elems};
  assign o_vec_alu_src_rd_ctr   = vec_alu_src_rd_ctr;
  assign o_vec_alu_acc          = vec_alu_acc;
  assign o_vec_vrf_rd_ctr       = vec_vrf_rd_gnt_ctr;
  assign o_vec_vrf_wr_ctr       = vec_vrf_wr_gnt_ctr;
  assign o_vec_vrf_bad_ctr      = vec_vrf_rd_bad_ctr;
  assign o_vec_vrf_rows         = vec_vrf_rows;
  assign o_vec_vrf_banks        = vec_vrf_banks;
  // dbg0: the descriptor's legality decision and the restart controller's
  // outcome. dbg1: the descriptor's own record and the ALU's element walk.
  // dbg2: the chain's identity discipline and the vector CSR port's handshake.
  assign o_vec_dbg0 = { 8'd0, vec_desc_reason[3:0], vec_desc_sew_log2[2:0],
                        vec_desc_lmul_exp[3:0], vec_desc_elem_count[7:0],
                        vec_desc_vtype_legal, vec_desc_cfg_legal,
                        vec_desc_accepting, vec_desc_rob_entries_used[7:0],
                        vec_desc_fault_valid_out, vec_desc_fault_elem_out[6:0],
                        vec_desc_fault_code_out[3:0],
                        vec_rst_complete, vec_rst_retire_ok, vec_rst_fof_trim,
                        vec_rst_vl_write, vec_rst_restart_ready,
                        vec_rst_prefix_agree, vec_rst_vl_new[7:0] };
  assign o_vec_dbg1 = { 3'd0, vec_rst_restart_vstart[6:0], vec_rst_elems_committed[7:0],
                        vec_desc_vd_out[4:0], vec_desc_vl[7:0],
                        vec_desc_vstart[6:0], vec_desc_elems_done[7:0],
                        vec_alu_sat, vec_alu_cur[7:0],
                        vec_alu_trace_elem[7:0], vec_mem_req_size[3] };
  assign o_vec_dbg2 = { 6'd0, vec_desc_rob_index[CORE_IDX_W-1:0],
                        vec_desc_rob_gen[CORE_RGEN_W-1:0],
                        vec_desc_uop_index[CORE_UOP_W-1:0],
                        vec_chain_valid, vec_chain_done, vec_chain_fault,
                        vec_chain_fault_elem[6:0], vec_chain_gen[CORE_RGEN_W-1:0],
                        vec_chain_vd[4:0], vec_chain_p_alloc_ready,
                        vec_chain_p_wr_accept, vec_csr_ready, vec_csr_commit,
                        vec_cfg_gen };

  // Port 2 is shared by the shared MUL/DIV unit and the FP unit. They are never
  // offered to the arbiter in the same cycle: FP wins, and the MUL/DIV result is
  // held by its own ready/valid handshake until the next cycle -- the same
  // sharing rule port 3 uses for the memory path and the CSR/system unit.
  assign port2_valid = fp_wb_valid | md_wb_valid;
  assign port2_ev    = fp_wb_valid ? fp_wb_ev : md_wb_ev;
  assign fp_wb_ready = port2_ready;
  assign md_wb_ready = port2_ready && !fp_wb_valid;

  // ---------------------------------------------------- the FP flag sideband
  // The flags of a speculative FP completion are recorded against its ROB slot
  // when the writeback path takes the completion, and merged into fcsr only when
  // that slot retires. An allocation clears its slot's record (so a recycled
  // slot cannot inherit stale flags), a completion sets it, and every redirect
  // or trap clears all records -- which is exactly why a squashed FP operation
  // contributes nothing to the architectural flags.
  integer fp_i;
  always_ff @(posedge clk) begin
    if (rst) begin
      for (fp_i = 0; fp_i < CORE_ROB_N; fp_i++) begin
        fp_flag_v_mem[fp_i]       <= 1'b0;
        fp_flag_gen_mem[fp_i]     <= {CORE_RGEN_W{1'b0}};
        fp_flag_mem[fp_i]         <= 5'd0;
        fp_state_wr_mem[fp_i]     <= 1'b0;
        fp_load_box_mem[fp_i]     <= 1'b0;
        fp_dst_mem[fp_i]          <= 1'b0;
      end
      fp_merge_ctr <= 32'd0;
    end else begin
      if (desc_wr_valid) begin
        fp_flag_v_mem[desc_wr_index]   <= 1'b0;
        fp_state_wr_mem[desc_wr_index] <= dbuf_ctl[0].fp_modifies_state;
        fp_dst_mem[desc_wr_index]      <= dbuf_ctl[0].fp_dst_fp && dbuf_ctl[0].reg_write;
      end
      if (fp_wb_valid && fp_wb_ready) begin
        fp_flag_v_mem[fp_wb_ev.id.rob_index]   <= 1'b1;
        fp_flag_gen_mem[fp_wb_ev.id.rob_index] <= fp_wb_ev.id.rob_gen;
        fp_flag_mem[fp_wb_ev.id.rob_index]     <= fp_wb_fflags;
      end
      // The FP-load NaN-box predicate, recorded at the load's memory insert.
      if (disp_mem_valid && disp_mem_ready && !disp_mem_is_store) begin
        fp_load_box_mem[disp_mem_id[CORE_UOP_W + CORE_RGEN_W +: CORE_IDX_W]] <=
            disp_mem_is_fp && (disp_mem_size == mosaic_pkg::SZ_WORD);
      end
      if (rob_flush_pulse) begin
        for (fp_i = 0; fp_i < CORE_ROB_N; fp_i++) begin
          fp_flag_v_mem[fp_i] <= 1'b0;
        end
      end
      if (fp_merge0_v || fp_merge1_v) begin
        fp_merge_ctr <= fp_merge_ctr + 32'd1;
      end
    end
  end

  // ==========================================================================
  // 7b. The integrated vector engine (I-059)
  // ==========================================================================
  // One vector *macro* is resolved here, at the ROB head, exactly as a system
  // macro is in section 10b -- and for the same reasons. A vector instruction's
  // configuration (vtype/vl/vstart) must be the one every older instruction
  // left, and its element progress and memory side effects must not be visible
  // until it retires. Resolving it at the head gives both: the head is the oldest
  // unretired instruction, so "at the head" *is* "after everything older".
  //
  // Dispatch holds allocation while the macro is live (`vec_block` below), so
  // the machine behind it is empty when it executes. That is what makes the
  // engine's ownership of the data port and of the VRF safe, and it is the price
  // this integration pays for not renaming vector registers: the VRF holds the
  // 32 architectural vector registers, and vector instructions are serialized.
  //
  // The engine is a sequencer over the seven I-051..I-058 units:
  //
  //   vec_cfg      the vset{i}vl{i} commit point, the seven vector CSRs, and the
  //                vtype the descriptor and the execution units read
  //   vec_desc     the legality authority (queried with the live configuration
  //                when the macro reaches the head) and the element bitmap
  //   mosaic_vrf   the 32-register vector file, one read slot and one write port
  //   vec_alu      the integer/mask/permute lane for vadd/vsub/vand/vor/vxor
  //   vec_lsu      the memory packetizer for unit-stride loads and stores
  //   vec_restart  the partial-trap/vstart/FOF controller for a memory macro
  //   vec_chain    the element-packet identity discipline (generation + vl
  //                bounds), so a packet from a cancelled macro cannot advance
  //                the descriptor
  //
  // Vector FP (`mosaic_vec_fp`) and the families neither the packetizer nor the
  // ALU implement are deliberately not wired; the report names each.

  // ---------------------------------------------------------- the stage
  assign vec_at_head = vec_valid_q && rob_head_valid &&
                       (rob_head_index == vec_index_q) &&
                       (rob_head_gen == vec_gen_q);
  // Dispatch allocates nothing while a vector macro is live. `vec_block` is the
  // level dispatch holds on; it is asserted for the whole life of the staged
  // macro, from the insert cycle to its retirement (or the redirect that kills
  // it).
  assign vec_block   = vec_valid_q;

  // --------------------------------------------------- the CSR address decode
  // The seven vector CSRs are owned by mosaic_vec_cfg, not by mosaic_csr: their
  // state is written by vset{i}vl{i} as well as by software, and the two writers
  // must see one copy. An address in this set is routed to the unit's CSR port;
  // every other address is the scalar CSR file's.
  always_comb begin
    csr_is_vec = 1'b0;
    case (csr_addr)
      12'h008, 12'h009, 12'h00A, 12'h00F,
      12'hC20, 12'hC21, 12'hC22: csr_is_vec = 1'b1;
      default: csr_is_vec = 1'b0;
    endcase
  end

  // The staged vector macro's configuration, as the engine and the units read
  // it. A live, committed value is correct here precisely because the macro is
  // resolved at the head: no younger vset can be in flight (dispatch holds
  // allocation), so this *is* the configuration the macro was issued under.
`ifdef MOSAIC_CORE_MUTANT_VEC_IGNORE_VS_OFF
  // NEGATIVE CONTROL: `mstatus.VS` == Off stops forbidding vector state, so a
  // vector instruction executes from a configuration the ISA says must trap
  // illegal. CASE=vec.integrated's vs-off phase requires the first `vsetvli` to
  // trap with mcause=2 and the engine to count one trap; with this define it
  // commits instead and both checks fail.
  assign vec_vset_vs_off = 1'b0;
`else
  assign vec_vset_vs_off = (o_csr_mstatus[10:9] == 2'b00);
`endif

  mosaic_vec_cfg u_vec_cfg (
      .clk_i            (clk),
      .rst_i            (rst),
      .vset_valid_i     (vec_vset_valid),
      .vset_kind_i      (vec_vset_kind),
      .vset_rd_i        (vec_vset_rd),
      .vset_rs1_i       (vec_pay_q_vs1),
      .vset_rs1_val_i   (vec_src1_q),
      .vset_rs2_val_i   (vec_src2_q),
      .vset_uimm_i      (vec_vset_uimm),
      .vset_vtypei_i    (vec_vtypei),
      .vset_vs_off_i    (vec_vset_vs_off),
      .vset_illegal_o   (),
      .vset_commit_o    (),
      .vset_rd_we_o     (),
      .vset_rd_val_o    (vec_vset_rd_val),
      .o_vtype_o        (vec_vtype),
      .o_vl_o           (vec_vl),
      .o_vstart_o       (vec_vstart),
      .o_vxrm_o         (),
      .o_vxsat_o        (),
      .o_vcsr_o         (vec_vcsr),
      .o_vlenb_o        (vec_vlenb),
      .o_vlmax_o        (vec_vlmax),
      .o_vill_o         (vec_vill),
      .o_cfg_gen_o      (vec_cfg_gen),
      .snap_capture_i   (1'b0),
      .snap_valid_o     (),
      .snap_vtype_o     (),
      .snap_vl_o        (),
      .snap_vstart_o    (),
      .snap_gen_o       (),
      .replay_valid_i   (1'b0),
      .replay_gen_i     (16'd0),
      .replay_ok_o      (),
      .replay_vtype_o   (),
      .replay_vl_o      (),
      .replay_vstart_o  (),
      .exec_valid_i     (vec_desc_alloc_valid),
      .exec_vtype_dep_i (vec_pay_q_vtype_dep),
      .exec_illegal_o   (),
      .csr_valid_i      (vec_csr_valid),
      .csr_addr_i       (vec_csr_addr),
      .csr_write_i      (vec_csr_write),
      .csr_wdata_i      (vec_csr_wdata),
      .csr_priv_i       (csr_priv),
      .csr_vs_off_i     (vec_vset_vs_off),
      .csr_ready_o      (vec_csr_ready),
      .csr_illegal_o    (vec_csr_illegal),
      .csr_rdata_o      (vec_csr_rdata),
      .csr_commit_o     (vec_csr_commit)
  );

  // The descriptor's query is combinational and stateful only in its allocate
  // port: it is queried with the *live* configuration when the macro reaches the
  // head, and allocated in the cycle the engine launches it.
  assign vec_desc_class   = vec_pay_q.op_class;
  // A store has no destination group, and a load has no source groups; naming
  // the unused groups x0 keeps the overlap rules from firing on fields the
  // instruction does not use.
  assign vec_desc_vd      = (vec_pay_q.kind == 3'd3) ? 5'd0 : vec_pay_q.vd;
  assign vec_desc_vs1     = (vec_pay_q.kind == 3'd1) ? vec_pay_q.vs1 : 5'd0;
  assign vec_desc_vs2     = (vec_pay_q.kind == 3'd1) ? vec_pay_q.vs2 : 5'd0;
  assign vec_desc_mask_en = vec_pay_q.mask_en;

  mosaic_vec_desc #(
      .VLEN        (128),
      .ELEN        (64),
      .ROB_INDEX_W (CORE_IDX_W),
      .ROB_GEN_W   (CORE_RGEN_W),
      .UOP_INDEX_W (CORE_UOP_W)
  ) u_vec_desc (
      .clk_i                 (clk),
      .rst_i                 (rst),
      .vtype_i               (vec_vtype),
      .op_class_i            (vec_desc_class),
      .vd_i                  (vec_desc_vd),
      .vs1_i                 (vec_desc_vs1),
      .vs2_i                 (vec_desc_vs2),
      .vs3_i                 (5'd0),
      .mask_en_i             (vec_desc_mask_en),
      .lane_count_i          (lane_quota),
      .o_vtype_legal_o       (vec_desc_vtype_legal),
      .o_cfg_legal_o         (vec_desc_cfg_legal),
      .o_illegal_o           (vec_desc_illegal),
      .o_reason_o            (vec_desc_reason),
      .o_sew_log2_o          (vec_desc_sew_log2),
      .o_lmul_exp_o          (vec_desc_lmul_exp),
      .o_emul_src_exp_o      (),
      .o_emul_dst_exp_o      (),
      .o_elem_count_o        (vec_desc_elem_count),
      .o_vlen_o              (),
      .o_vlenb_o             (),
      .o_lane_count_o        (),
      .o_class_count_o       (),
      .alloc_valid_i         (vec_desc_alloc_valid),
      .alloc_ready_o         (vec_desc_alloc_ready),
      .alloc_vtype_i         (vec_vtype),
      .alloc_vl_i            (vec_vl[7:0]),
      .alloc_vstart_i        (vec_vstart[6:0]),
      .alloc_vd_i            (vec_pay_q.vd),
      .alloc_mask_ver_i      (4'd0),
      .alloc_rob_index_i     (vec_index_q),
      .alloc_rob_gen_i       (vec_gen_q),
      .alloc_uop_index_i     (vec_uop_q),
      .elem_done_valid_i     (vec_desc_elem_done_valid),
      .elem_done_index_i     (vec_desc_elem_done_index),
      .fault_valid_i         (vec_desc_fault_valid),
      .fault_elem_i          (vec_desc_fault_elem),
      .fault_code_i          (vec_desc_fault_code),
      .fault_clear_i         (1'b0),
      .release_i             (vec_desc_release),
      .o_valid_o             (vec_desc_valid),
      .o_macro_rob_index_o   (vec_desc_rob_index),
      .o_macro_rob_gen_o     (vec_desc_rob_gen),
      .o_macro_uop_index_o   (vec_desc_uop_index),
      .o_vtype_o             (),
      .o_vl_o                (vec_desc_vl),
      .o_vstart_o            (vec_desc_vstart),
      .o_vd_o                (vec_desc_vd_out),
      .o_mask_ver_o          (),
      .o_elem_bitmap_o       (vec_desc_bitmap),
      .o_prefix_o            (vec_desc_prefix),
      .o_elems_done_ctr_o    (vec_desc_elems_done),
      .o_fault_valid_o       (vec_desc_fault_valid_out),
      .o_fault_elem_o        (vec_desc_fault_elem_out),
      .o_fault_code_o        (vec_desc_fault_code_out),
      .o_accepting_elems_o   (vec_desc_accepting),
      .o_rob_entries_used_o  (vec_desc_rob_entries_used),
      .o_alloc_ctr_o         (vec_desc_alloc_ctr),
      .o_release_ctr_o       (vec_desc_release_ctr)
  );

  // ------------------------------------------------------- the VRF and its two
  // masters. The ALU and the LSU share one read slot and one write port, exactly
  // as the standalone binding does; only one of them runs at a time because only
  // one macro is in flight.
  logic        vec_alu_rd_valid;
  logic [4:0]  vec_alu_rd_base;
  logic [6:0]  vec_alu_rd_elem;
  logic [2:0]  vec_alu_rd_sew;
  logic [3:0]  vec_alu_rd_lmul;
  logic [15:0] vec_alu_rd_tag;
  logic        vec_alu_rd_gnt;
  logic        vec_alu_rd_rsp_valid;
  logic [15:0] vec_alu_rd_rsp_tag;
  logic [63:0] vec_alu_rd_rsp_data;
  logic        vec_alu_wr_valid;
  logic [4:0]  vec_alu_wr_base;
  logic [6:0]  vec_alu_wr_elem;
  logic [2:0]  vec_alu_wr_sew;
  logic [3:0]  vec_alu_wr_lmul;
  logic [63:0] vec_alu_wr_data;
  logic        vec_alu_wr_gnt;
  logic        vec_lsu_rd_valid;
  logic [4:0]  vec_lsu_rd_base;
  logic [6:0]  vec_lsu_rd_elem;
  logic [2:0]  vec_lsu_rd_sew;
  logic [3:0]  vec_lsu_rd_lmul;
  logic [15:0] vec_lsu_rd_tag;
  logic        vec_lsu_rd_gnt;
  logic        vec_lsu_rd_rsp_valid;
  logic [15:0] vec_lsu_rd_rsp_tag;
  logic [63:0] vec_lsu_rd_rsp_data;
  logic        vec_lsu_wr_valid;
  logic [4:0]  vec_lsu_wr_base;
  logic [6:0]  vec_lsu_wr_elem;
  logic [2:0]  vec_lsu_wr_sew;
  logic [3:0]  vec_lsu_wr_lmul;
  logic [63:0] vec_lsu_wr_data;
  logic        vec_lsu_wr_gnt;

  always_comb begin
    if (vec_lsu_owns_vrf) begin
      vec_vrf_rd_valid = vec_lsu_rd_valid;
      vec_vrf_rd_base  = vec_lsu_rd_base;
      vec_vrf_rd_elem  = vec_lsu_rd_elem;
      vec_vrf_rd_sew   = vec_lsu_rd_sew;
      vec_vrf_rd_lmul  = vec_lsu_rd_lmul;
      vec_vrf_rd_tag   = vec_lsu_rd_tag;
      vec_vrf_wr_valid = vec_lsu_wr_valid;
      vec_vrf_wr_base  = vec_lsu_wr_base;
      vec_vrf_wr_elem  = vec_lsu_wr_elem;
      vec_vrf_wr_sew   = vec_lsu_wr_sew;
      vec_vrf_wr_lmul  = vec_lsu_wr_lmul;
      vec_vrf_wr_data_raw = vec_lsu_wr_data;
      vec_vrf_wr_elem_now = vec_lsu_wr_elem;
    end else begin
      vec_vrf_rd_valid = vec_alu_rd_valid;
      vec_vrf_rd_base  = vec_alu_rd_base;
      vec_vrf_rd_elem  = vec_alu_rd_elem;
      vec_vrf_rd_sew   = vec_alu_rd_sew;
      vec_vrf_rd_lmul  = vec_alu_rd_lmul;
      vec_vrf_rd_tag   = vec_alu_rd_tag;
      vec_vrf_wr_valid = vec_alu_wr_valid;
      vec_vrf_wr_base  = vec_alu_wr_base;
      vec_vrf_wr_elem  = vec_alu_wr_elem;
      vec_vrf_wr_sew   = vec_alu_wr_sew;
      vec_vrf_wr_lmul  = vec_alu_wr_lmul;
      vec_vrf_wr_data_raw = vec_alu_wr_data;
      vec_vrf_wr_elem_now = vec_alu_wr_elem;
    end
  end

`ifdef MOSAIC_LANE_MUTANT_DISCARD_REST
  // NEGATIVE CONTROL: a shrinking resize is read as "the elements the closed
  // lanes still owed are discarded". The macro in flight keeps its launch-time
  // lane plan, so its elements 0..plan-1 are named; the instant a *smaller*
  // quota is requested, every element whose plan lane the new quota no longer
  // covers has its destination write dropped (written as zero), and the
  // architectural result loses exactly those elements. The shipping build's
  // gate is the identity, because the quota never changes inside a macro.
  assign vec_vrf_wr_data =
      (((vec_vrf_wr_elem_now & (7'(lane_plan_quota_q) - 7'd1)) >= 7'(lane_req_quota))
       ? 64'd0 : vec_vrf_wr_data_raw);
`else
  assign vec_vrf_wr_data = vec_vrf_wr_data_raw;
`endif

  assign vec_alu_rd_gnt       = vec_vrf_rd_gnt;
  assign vec_alu_rd_rsp_valid = vec_vrf_rd_rsp_valid;
  assign vec_alu_rd_rsp_tag   = vec_vrf_rd_rsp_tag;
  assign vec_alu_rd_rsp_data  = vec_vrf_rd_rsp_data;
  assign vec_alu_wr_gnt       = vec_vrf_wr_gnt;
  assign vec_lsu_rd_gnt       = vec_vrf_rd_gnt;
  assign vec_lsu_rd_rsp_valid = vec_vrf_rd_rsp_valid;
  assign vec_lsu_rd_rsp_tag   = vec_vrf_rd_rsp_tag;
  assign vec_lsu_rd_rsp_data  = vec_vrf_rd_rsp_data;
  assign vec_lsu_wr_gnt       = vec_vrf_wr_gnt;

  mosaic_vrf #(
      .VLEN       (128),
      .ELEN       (64),
      .VREGS      (32),
      .BANK_W     (32),
      .BANKS      (32),
      .LANES_MAX  (1),
      .RD_PORTS   (2),
      .WR_PORTS   (1),
      .RD_LATENCY (1)
  ) u_vec_vrf (
      .clk_i             (clk),
      .rst_i             (rst),
      .lane_count_i      (lane_quota),
      .plat_i            (2'd0),
      .rd_valid_i        (vec_vrf_rd_valid),
      .rd_base_i         (vec_vrf_rd_base),
      .rd_elem_i         (vec_vrf_rd_elem),
      .rd_sew_i          (vec_vrf_rd_sew),
      .rd_lmul_i         (vec_vrf_rd_lmul),
      .rd_tag_i          (vec_vrf_rd_tag),
      .rd_gnt_o          (vec_vrf_rd_gnt),
      .rd_rsp_valid_o    (vec_vrf_rd_rsp_valid),
      .rd_rsp_tag_o      (vec_vrf_rd_rsp_tag),
      .rd_rsp_data_o     (vec_vrf_rd_rsp_data),
      .wr_valid_i        (vec_vrf_wr_valid),
      .wr_base_i         (vec_vrf_wr_base),
      .wr_elem_i         (vec_vrf_wr_elem),
      .wr_sew_i          (vec_vrf_wr_sew),
      .wr_lmul_i         (vec_vrf_wr_lmul),
      .wr_data_i         (vec_vrf_wr_data),
      .wr_gnt_o          (vec_vrf_wr_gnt),
      .q_valid_i         (1'b0),
      .q_base_i          (5'd0),
      .q_elem_i          (7'd0),
      .q_sew_i           (3'd0),
      .q_lmul_i          (4'd0),
      .o_q_valid_o       (),
      .o_q_phys_reg_o    (),
      .o_q_row_o         (),
      .o_q_lo_bit_o      (),
      .o_q_hi_bit_o      (),
      .o_q_nbanks_o      (),
      .o_q_bank0_o       (),
      .o_q_bank1_o       (),
      .o_rd_gnt_ctr      (vec_vrf_rd_gnt_ctr),
      .o_rd_conflict_ctr (),
      .o_rd_bad_ctr      (vec_vrf_rd_bad_ctr),
      .o_wr_gnt_ctr      (vec_vrf_wr_gnt_ctr),
      .o_wr_conflict_ctr (),
      .o_wr_hazard_ctr   (),
      .o_wr_bad_ctr      (),
      .o_busy            (),
      .o_vlen_o          (),
      .o_elen_o          (),
      .o_vregs_o         (),
      .o_banks_o         (vec_vrf_banks),
      .o_bank_w_o        (),
      .o_rows_o          (vec_vrf_rows),
      .o_regs_per_row_o  (),
      .o_lane_max_o      (),
      .o_rd_latency_o    (),
      .o_rd_ports_o      (),
      .o_wr_ports_o      (),
      .o_plat_o          ()
  );

  // ------------------------------------------------------------- the ALU lane
  assign vec_alu_family  = vec_pay_q.family;
  assign vec_alu_op      = vec_pay_q.op;
  assign vec_alu_form    = vec_pay_q.form;
  assign vec_alu_vd      = vec_pay_q.vd;
  assign vec_alu_vs1     = vec_pay_q.vs1;
  assign vec_alu_vs2     = vec_pay_q.vs2;
  assign vec_alu_mask_en = vec_pay_q.mask_en;

  mosaic_vec_alu #(
      .VLEN (128),
      .ELEN (64),
      .NFAM (17)
  ) u_vec_alu (
      .clk_i              (clk),
      .rst_i              (rst),
      .caps_i             (VEC_ALU_CAPS),
      .cfg_vtype_i        (vec_vtype),
      .cfg_vl_i           (vec_vl[7:0]),
      .cfg_vstart_i       (vec_vstart[6:0]),
      .cfg_vxrm_i         (vec_vcsr[2:1]),
      .e_valid_i          (1'b0),
      .e_family_i         (5'd0),
      .e_op_i             (4'd0),
      .e_form_i           (2'd0),
      .e_vs2_i            (64'd0),
      .e_vs1_i            (64'd0),
      .e_acc_i            (64'd0),
      .e_mask_i           (1'b0),
      .e_scalar_i         (64'd0),
      .e_index_i          (8'd0),
      .e_pfx_i            (1'b0),
      .e_result_o         (),
      .e_mres_o           (),
      .e_sat_o            (),
      .e_illegal_o        (),
      .e_trap_o           (),
      .e_access_o         (),
      .e_write_o          (),
      .e_rd2_o            (),
      .e_pfx_o            (),
      .exec_valid_i       (vec_alu_exec_valid),
      .exec_family_i      (vec_alu_family),
      .exec_op_i          (vec_alu_op),
      .exec_form_i        (vec_alu_form),
      .exec_vd_i          (vec_alu_vd),
      .exec_vs1_i         (vec_alu_vs1),
      .exec_vs2_i         (vec_alu_vs2),
      .exec_scalar_i      (vec_alu_scalar),
      .exec_mask_en_i     (vec_alu_mask_en),
      .exec_busy_o        (),
      .exec_done_o        (vec_alu_done),
      .exec_illegal_o     (vec_alu_illegal),
      .exec_trap_o        (),
      .exec_trap_elem_o   (),
      .exec_sat_o         (vec_alu_sat),
      .exec_elems_o       (vec_alu_elems),
      .exec_cur_o         (vec_alu_cur),
      .exec_acc_o         (vec_alu_acc),
      .exec_trace_valid_o (vec_alu_trace_valid),
      .exec_trace_elem_o  (vec_alu_trace_elem),
      .exec_src_rd_ctr_o  (vec_alu_src_rd_ctr),
      .vrf_rd_valid_o     (vec_alu_rd_valid),
      .vrf_rd_base_o      (vec_alu_rd_base),
      .vrf_rd_elem_o      (vec_alu_rd_elem),
      .vrf_rd_sew_o       (vec_alu_rd_sew),
      .vrf_rd_lmul_o      (vec_alu_rd_lmul),
      .vrf_rd_tag_o       (vec_alu_rd_tag),
      .vrf_rd_gnt_i       (vec_alu_rd_gnt),
      .vrf_rd_rsp_valid_i (vec_alu_rd_rsp_valid),
      .vrf_rd_rsp_tag_i   (vec_alu_rd_rsp_tag),
      .vrf_rd_rsp_data_i  (vec_alu_rd_rsp_data),
      .vrf_wr_valid_o     (vec_alu_wr_valid),
      .vrf_wr_base_o      (vec_alu_wr_base),
      .vrf_wr_elem_o      (vec_alu_wr_elem),
      .vrf_wr_sew_o       (vec_alu_wr_sew),
      .vrf_wr_lmul_o      (vec_alu_wr_lmul),
      .vrf_wr_data_o      (vec_alu_wr_data),
      .vrf_wr_gnt_i       (vec_alu_wr_gnt)
  );

  // ------------------------------------------------------- the LSU packetizer
  assign vec_lsu_mode      = vec_pay_q.lsu_mode;
  assign vec_lsu_we        = vec_pay_q.lsu_we;
  assign vec_lsu_ordered   = vec_pay_q.lsu_ordered;
  assign vec_lsu_nf        = vec_pay_q.nf;
  assign vec_lsu_vd        = vec_pay_q.vd;
  assign vec_lsu_data      = vec_pay_q.data;
  assign vec_lsu_index     = vec_pay_q.index;
  assign vec_lsu_idx_sew   = vec_pay_q.idx_sew;
  assign vec_lsu_base      = vec_src1_q;
  assign vec_lsu_stride    = vec_src2_q;
  assign vec_lsu_mask_en   = vec_pay_q.mask_en;

  mosaic_vec_lsu #(
      .VLEN (128),
      .ELEN (64),
      .NLSM (8)
  ) u_vec_lsu (
      .clk_i              (clk),
      .rst_i              (rst),
      .caps_i             (VEC_LSU_CAPS),
      .cfg_vtype_i        (vec_vtype),
      .cfg_vl_i           (vec_vl[7:0]),
      .cfg_vstart_i       (vec_vstart[6:0]),
      .exec_valid_i       (vec_lsu_exec_valid),
      .exec_mode_i        (vec_lsu_mode),
      .exec_we_i          (vec_lsu_we),
      .exec_ordered_i     (vec_lsu_ordered),
      .exec_nf_i          (vec_lsu_nf),
      .exec_vd_i          (vec_lsu_vd),
      .exec_data_i        (vec_lsu_data),
      .exec_index_i       (vec_lsu_index),
      .exec_idx_sew_i     (vec_lsu_idx_sew),
      .exec_base_i        (vec_lsu_base),
      .exec_stride_i      (vec_lsu_stride),
      .exec_mask_en_i     (vec_lsu_mask_en),
      .busy_o             (vec_lsu_busy),
      .done_o             (vec_lsu_done),
      .illegal_o          (vec_lsu_illegal),
      .trap_o             (vec_lsu_trap),
      .trap_elem_o        (vec_lsu_trap_elem),
      .trap_code_o        (vec_lsu_trap_code),
      .stop_i             (vec_rst_stop),
      .stopped_o          (vec_lsu_stopped),
      .stop_elem_o        (vec_lsu_stop_elem),
      .elems_o            (vec_lsu_elems),
      .req_ctr_o          (vec_lsu_req_ctr),
      .vrf_rd_valid_o     (vec_lsu_rd_valid),
      .vrf_rd_base_o      (vec_lsu_rd_base),
      .vrf_rd_elem_o      (vec_lsu_rd_elem),
      .vrf_rd_sew_o       (vec_lsu_rd_sew),
      .vrf_rd_lmul_o      (vec_lsu_rd_lmul),
      .vrf_rd_tag_o       (vec_lsu_rd_tag),
      .vrf_rd_gnt_i       (vec_lsu_rd_gnt),
      .vrf_rd_rsp_valid_i (vec_lsu_rd_rsp_valid),
      .vrf_rd_rsp_tag_i   (vec_lsu_rd_rsp_tag),
      .vrf_rd_rsp_data_i  (vec_lsu_rd_rsp_data),
      .vrf_wr_valid_o     (vec_lsu_wr_valid),
      .vrf_wr_base_o      (vec_lsu_wr_base),
      .vrf_wr_elem_o      (vec_lsu_wr_elem),
      .vrf_wr_sew_o       (vec_lsu_wr_sew),
      .vrf_wr_lmul_o      (vec_lsu_wr_lmul),
      .vrf_wr_data_o      (vec_lsu_wr_data),
      .vrf_wr_gnt_i       (vec_lsu_wr_gnt),
      .mem_req_valid_o    (vec_mem_req_valid),
      .mem_req_ready_i    (vec_mem_req_ready),
      .mem_req_elem_o     (vec_mem_req_elem),
      .mem_req_field_o    (vec_mem_req_field),
      .mem_req_addr_o     (vec_mem_req_addr),
      .mem_req_wmask_o    (vec_mem_req_wmask),
      .mem_req_wdata_o    (vec_mem_req_wdata),
      .mem_req_we_o       (vec_mem_req_we),
      .mem_req_size_o     (vec_mem_req_size),
      .mem_req_ordered_o  (),
      .mem_rsp_valid_i    (vec_mem_rsp_valid),
      .mem_rsp_elem_i     (vec_mem_rsp_elem_q),
      .mem_rsp_field_i    (vec_mem_rsp_field_q),
      .mem_rsp_fault_i    (vec_mem_rsp_fault),
      .mem_rsp_fault_code_i (vec_mem_rsp_code),
      .mem_rsp_rdata_i    (vec_mem_rsp_rdata)
  );

  // ---------------------------------------------------- the restart controller
  assign vec_rst_exec_mode = vec_pay_q.lsu_mode;
  assign vec_rst_exec_we   = vec_pay_q.lsu_we;
  assign vec_rst_exec_fof  = vec_pay_q.lsu_fof;
  assign vec_rst_exec_nf   = vec_pay_q.nf;

  mosaic_vec_restart #(
      .VLEN (128),
      .ELEN (64)
  ) u_vec_restart (
      .clk_i              (clk),
      .rst_i              (rst),
      .exec_valid_i       (vec_rst_exec_valid),
      .exec_mode_i        (vec_rst_exec_mode),
      .exec_we_i          (vec_rst_exec_we),
      .exec_fof_i         (vec_rst_exec_fof),
      .exec_nf_i          (vec_rst_exec_nf),
      .cfg_vtype_i        (vec_vtype),
      .cfg_vl_i           (vec_vl[7:0]),
      .cfg_vstart_i       (vec_vstart[6:0]),
      .lsu_busy_i         (vec_lsu_busy),
      .lsu_done_i         (vec_lsu_done),
      .lsu_illegal_i      (vec_lsu_illegal),
      .lsu_trap_i         (vec_lsu_trap),
      .lsu_trap_elem_i    (vec_lsu_trap_elem),
      .lsu_trap_code_i    (vec_lsu_trap_code),
      .lsu_stopped_i      (vec_lsu_stopped),
      .lsu_stop_elem_i    (vec_lsu_stop_elem),
      .lsu_elems_i        (vec_lsu_elems),
      .intr_i             (1'b0),
      .stop_o             (vec_rst_stop),
      .elem_done_valid_o  (vec_rst_elem_done_valid),
      .elem_done_index_o  (vec_rst_elem_done_index),
      .fault_valid_o      (vec_rst_fault_valid),
      .fault_elem_o       (vec_rst_fault_elem),
      .fault_code_o       (vec_rst_fault_code),
      .desc_valid_i       (vec_desc_valid),
      .desc_bitmap_i      (vec_desc_bitmap),
      .desc_prefix_i      (vec_desc_prefix),
      .o_busy_o           (vec_rst_busy),
      .o_resolved_o       (vec_rst_resolved),
      .o_illegal_o        (vec_rst_illegal),
      .o_trap_o           (vec_rst_trap),
      .o_vstart_o         (vec_rst_vstart),
      .o_trap_code_o      (vec_rst_trap_code),
      .o_vl_write_o       (vec_rst_vl_write),
      .o_vl_new_o         (vec_rst_vl_new),
      .o_fof_trim_o       (vec_rst_fof_trim),
      .o_complete_o       (vec_rst_complete),
      .o_retire_ok_o      (vec_rst_retire_ok),
      .o_restart_ready_o  (vec_rst_restart_ready),
      .o_restart_vstart_o (vec_rst_restart_vstart),
      .o_elems_committed_o(vec_rst_elems_committed),
      .o_prefix_agree_o   (vec_rst_prefix_agree)
  );

  // ---------------------------------------------------- the chaining network
  // The element-completion packets the descriptor accepts are the ones the
  // chaining network validated: right generation, index inside vl, not the same
  // element twice, and not after a fault or a cancel. A packet from a macro a
  // redirect discarded therefore cannot advance the descriptor -- the identity
  // discipline I-058 built, applied at the one place this integration has it.
  assign vec_chain_p_alloc_valid = vec_launch_unit;
  assign vec_chain_p_gen         = vec_gen_q;
  assign vec_chain_p_vd          = vec_pay_q.vd;
  assign vec_chain_p_vl          = vec_vl[7:0];
`ifdef MOSAIC_CORE_MUTANT_VEC_STALE_PACKET
  // NEGATIVE CONTROL: the one-macro barrier makes a packet from a *genuinely*
  // cancelled macro unreachable here -- the restart controller's walker is
  // drained before the trap redirects, so no element completion is still in
  // flight when the successor allocates (this is the integration's "not
  // covered" item, controlled structurally by CASE=rvv.chaining_hazards). To
  // prove the integration's own progress accounting is sensitive to the defect
  // anyway, the injection synthesises the offer: one cycle after a *restarted*
  // memory macro (vstart != 0) allocates its descriptor, the element the
  // cancelled attempt had already committed is offered to the chain again and
  // accepted, so the successor's progress counts a packet that belongs to the
  // macro the redirect discarded.
  logic vec_stale_pkt_q;
  always_ff @(posedge clk) begin
    if (rst) vec_stale_pkt_q <= 1'b0;
    else vec_stale_pkt_q <= vec_launch_unit && vec_is_mem && (vec_vstart != 64'd0);
  end
  assign vec_chain_p_wr_valid    = (vec_pay_q.kind == 3'd1) ? vec_alu_trace_valid
                                                           : (vec_rst_elem_done_valid ||
                                                              vec_stale_pkt_q);
  assign vec_chain_p_wr_index    = vec_stale_pkt_q ? 7'd0
                                 : ((vec_pay_q.kind == 3'd1) ? vec_alu_trace_elem[6:0]
                                                            : vec_rst_elem_done_index);
`else
  assign vec_chain_p_wr_valid    = (vec_pay_q.kind == 3'd1) ? vec_alu_trace_valid
                                                           : vec_rst_elem_done_valid;
  assign vec_chain_p_wr_index    = (vec_pay_q.kind == 3'd1) ? vec_alu_trace_elem[6:0]
                                                           : vec_rst_elem_done_index;
`endif
  assign vec_chain_p_wr_data     = 64'd0;
  assign vec_chain_p_wr_gen      = vec_gen_q;
  assign vec_chain_p_fault_valid = vec_rst_fault_valid;
  assign vec_chain_p_fault_elem  = vec_rst_fault_elem;
  assign vec_chain_p_cancel      = vec_macro_leave;
  assign vec_chain_p_done        = (vec_pay_q.kind == 3'd1) ? vec_alu_done : vec_rst_resolved;

  mosaic_vec_chain #(
      .VLEN    (128),
      .ELEN    (64),
      .GEN_W   (CORE_RGEN_W),
      .GROUP_W (5)
  ) u_vec_chain (
      .clk_i              (clk),
      .rst_i              (rst),
      // The shipping build uses element-granular chaining. The no-chaining
      // control is a `-D` mutant that ties it low (the architectural result
      // must be identical either way, which is what makes it a control).
      .chain_en_i         (1'b1),
      .p_alloc_valid_i    (vec_chain_p_alloc_valid),
      .p_alloc_ready_o    (vec_chain_p_alloc_ready),
      .p_gen_i            (vec_chain_p_gen),
      .p_vd_i             (vec_chain_p_vd),
      .p_vl_i             (vec_chain_p_vl),
      .p_wr_valid_i       (vec_chain_p_wr_valid),
      .p_wr_index_i       (vec_chain_p_wr_index),
      .p_wr_data_i        (vec_chain_p_wr_data),
      .p_wr_gen_i         (vec_chain_p_wr_gen),
      .p_wr_accept_o      (vec_chain_p_wr_accept),
      .p_fault_valid_i    (vec_chain_p_fault_valid),
      .p_fault_elem_i     (vec_chain_p_fault_elem),
      .p_cancel_i         (vec_chain_p_cancel),
      .p_done_i           (vec_chain_p_done),
      .o_accept_valid_o   (vec_chain_accept_valid),
      .o_accept_index_o   (vec_chain_accept_index),
      .c0_alloc_valid_i   (1'b0),
      .c0_alloc_ready_o   (),
      .c0_gen_i           ({CORE_RGEN_W{1'b0}}),
      .c0_vs_i            (5'd0),
      .c0_vl_i            (8'd0),
      .c0_req_valid_i     (1'b0),
      .c0_req_index_i     (7'd0),
      .c0_req_accept_o    (),
      .c0_rdy_o           (),
      .c0_data_o          (),
      .c0_finish_i        (1'b0),
      .c1_alloc_valid_i   (1'b0),
      .c1_alloc_ready_o   (),
      .c1_gen_i           ({CORE_RGEN_W{1'b0}}),
      .c1_vs_i            (5'd0),
      .c1_vl_i            (8'd0),
      .c1_req_valid_i     (1'b0),
      .c1_req_index_i     (7'd0),
      .c1_req_accept_o    (),
      .c1_rdy_o           (),
      .c1_data_o          (),
      .c1_finish_i        (1'b0),
      .war_valid_i        (1'b0),
      .war_vd_i           (5'd0),
      .war_elem_i         (7'd0),
      .war_data_i         (64'd0),
      .war_grant_o        (),
      .src_release_ok_o   (),
      .o_valid_o          (vec_chain_valid),
      .o_gen_o            (vec_chain_gen),
      .o_vd_o             (vec_chain_vd),
      .o_done_o           (vec_chain_done),
      .o_fault_o          (vec_chain_fault),
      .o_fault_elem_o     (vec_chain_fault_elem),
      .o_ready_o          (),
      .o_pkt_accept_ctr_o (vec_chain_pkt_accept_ctr),
      .o_pkt_refuse_ctr_o (vec_chain_pkt_refuse_ctr),
      .o_fwd_ctr_o        (),
      .o_stall_ctr_o      ()
  );

  // The descriptor's element progress is exactly the packets the chain accepted.
  assign vec_desc_elem_done_valid = vec_chain_accept_valid;
  assign vec_desc_elem_done_index = vec_chain_accept_index;
  // A fault is recorded by the restart controller and forwarded into the
  // descriptor and the chain (above); the chain freezes on it.
  assign vec_desc_fault_valid = vec_rst_fault_valid;
  assign vec_desc_fault_elem  = vec_rst_fault_elem;
  assign vec_desc_fault_code  = vec_rst_fault_code;

  // --------------------------------------------------------------------------
  // The engine sequencer
  // --------------------------------------------------------------------------
  // A macro is "at the head and ready to start" when its ROB entry is the head
  // and nothing else is in flight. Everything that could redirect is excluded
  // (`recovering`, `redirect_valid`), as is the interrupt offer, so the trap the
  // macro may take is its own.
  logic vec_illegal_launch;
  logic vec_eew_mismatch;

  // The packetizer derives an element's width from vtype.SEW, not from the
  // instruction's width suffix. A suffix that disagrees is therefore refused
  // rather than mis-addressed; the report names the widths that remain
  // unreachable because of it.
  assign vec_eew_mismatch = ((vec_pay_q.kind == 3'd2) || (vec_pay_q.kind == 3'd3)) &&
                            (vec_pay_q.eew_sew != vec_vtype[5:3]);
  assign vec_illegal_launch = vec_vset_vs_off || vec_eew_mismatch ||
                              ((vec_pay_q.kind != 3'd0) && vec_desc_illegal);

  // Launch waits for the memory path to drain: the vector LSU drives the core's
  // one data port and must not overtake a store the queue is still draining.
  assign vec_launch = vec_at_head && (vec_state_q == VEC_IDLE) &&
                      !vec_wb_pending_q && !vec_done_q &&
                      !sys_head && !head_exc_trap && !irq_valid && !recovering &&
                      !redirect_valid && !core_stop &&
                      (!vec_is_mem || mem_path_idle) &&
                      vec_desc_alloc_ready;

  assign vec_is_mem      = (vec_pay_q.kind == 3'd2) || (vec_pay_q.kind == 3'd3);
  assign vec_launch_vset = vec_launch && (vec_pay_q.kind == 3'd0) &&
                           !vec_illegal_launch;
  assign vec_launch_unit = vec_launch && (vec_pay_q.kind != 3'd0) && !vec_illegal_launch;
  assign vec_vset_valid  = vec_launch_vset;
  assign vec_vset_kind   = vec_pay_q.vset_kind;
  assign vec_vset_rd     = vec_pay_q.vd;
  assign vec_vset_uimm   = vec_pay_q.vset_uimm;
  assign vec_vtypei      = vec_pay_q.vtypei;

  // A unit launch is a one-cycle strobe; dispatch holds the macro at the head
  // until it retires, so the strobe cannot be re-issued.
  assign vec_alu_exec_valid = vec_launch_unit && (vec_pay_q.kind == 3'd1);
  assign vec_lsu_exec_valid = vec_launch_unit && vec_is_mem;
  assign vec_rst_exec_valid = vec_launch_unit && vec_is_mem;
  assign vec_desc_alloc_valid = vec_launch_unit;
  assign vec_desc_release     = vec_macro_leave && vec_desc_valid;

  // The scalar operand of an arithmetic macro: x[rs1] for the .vx form, the
  // immediate for .vi, unused for .vv.
  assign vec_alu_scalar = vec_pay_q.scalar_from_imm ? vec_pay_q.imm : vec_src1_q;

  // The staged vset reads x[rs1]; the payload's vs1 is the same field.
  assign vec_pay_q_vs1 = vec_pay_q.vs1;

  // `vec_pay_q_vtype_dep` is "the descriptor must be legal for this macro to
  // execute": true for everything except vset (which defines the configuration
  // rather than depending on it).
  assign vec_pay_q_vtype_dep = (vec_pay_q.kind != 3'd0);

  assign vec_lsu_owns_vrf = vec_lsu_busy;

  // The completion the arbiter publishes on port 3.
  always_comb begin
    vec_wb_ev.id.hart      = 1'b0;
    vec_wb_ev.id.rob_index = vec_index_q;
    vec_wb_ev.id.rob_gen   = vec_gen_q;
    vec_wb_ev.id.uop_index = vec_uop_q;
    vec_wb_ev.dst.tag      = vec_dst_x0_q ? {CORE_TAG_W{1'b0}} : vec_dst_tag_q;
    vec_wb_ev.dst.gen      = {{(CORE_PGEN_W - CORE_IGEN_W){1'b0}}, vec_dst_gen_q};
    vec_wb_ev.dst.x0       = vec_dst_x0_q;
    // Only vset{i}vl{i} carries a value: the new vl in the integer rd. Every
    // other vector macro's architectural result is a vector register, which the
    // engine wrote through the VRF.
    vec_wb_ev.value_valid  = (vec_pay_q.kind == 3'd0) && !vec_dst_x0_q;
    vec_wb_ev.value        = (vec_pay_q.kind == 3'd0) ? vec_vset_rd_val : {CORE_XLEN{1'b0}};
    // A vector macro's exception is taken by the trap controller, not by the
    // writeback path: the instruction does not retire, and the trap carries a
    // `vstart` the writeback event has no field for. So the payload is empty
    // here, exactly as the system unit's is.
    vec_wb_ev.exc.valid    = 1'b0;
    vec_wb_ev.exc.cause    = {CORE_XLEN{1'b0}};
    vec_wb_ev.exc.tval     = {CORE_XLEN{1'b0}};
    vec_wb_ev.is_store     = 1'b0;
    vec_wb_ev.is_load      = 1'b0;
  end

  assign vec_wb_want  = vec_at_head && vec_done_q && !vec_wb_pending_q;

  // -------------------------------------------------------- trap resolution
  // A memory fault's cause is the access class the instruction performed; its
  // tval is the faulting element's own address, which the packetizer's element
  // index and the instruction's EEW name exactly (the packetizer issues one
  // request per element, so element k is at base + k*EEW/8 for the stride-0
  // form this integration wires).
  logic [63:0] vec_trap_cause_mem;
  logic [63:0] vec_trap_tval_mem;
  always_comb begin
    vec_trap_cause_mem = vec_lsu_we ? mosaic_pkg::EXC_STORE_ACCESS
                                    : mosaic_pkg::EXC_LOAD_ACCESS;
    case (vec_rst_trap_code)
      4'd1:    vec_trap_cause_mem = vec_lsu_we ? mosaic_pkg::EXC_STORE_PAGE
                                               : mosaic_pkg::EXC_LOAD_PAGE;
      4'd2:    vec_trap_cause_mem = vec_lsu_we ? mosaic_pkg::EXC_STORE_ACCESS
                                               : mosaic_pkg::EXC_LOAD_ACCESS;
      default: vec_trap_cause_mem = vec_lsu_we ? mosaic_pkg::EXC_STORE_ACCESS
                                               : mosaic_pkg::EXC_LOAD_ACCESS;
    endcase
    // `eew_sew` is log2(EEW in *bits*) (3..6, the packetizer's own encoding, see
    // mosaic_vec_lsu's `be64 = 1 << (eew_log2_q - 3)`), so the element's byte
    // offset is `vstart << (eew_sew - 3)`. Shifting by `eew_sew` itself made the
    // address eight times the real one for e32/lmul=1: the case drove a fault at
    // element 2 of a 32-bit unit-stride load at 0x801FFFF8 and the DUT named
    // 0x80200038 where the faulting element is at 0x80200000.
    vec_trap_tval_mem = vec_lsu_base +
        (64'(vec_rst_vstart) << (vec_pay_q.eew_sew - 3'd3));
  end

  // ---------------------------------------------------------- the FSM outputs
  logic vec_lsu_seen_busy_q;
  logic vec_lsu_finished;
  assign vec_lsu_finished = vec_lsu_seen_busy_q && !vec_rst_busy;
`ifdef MOSAIC_CORE_MUTANT_VEC_RETIRE_INCOMPLETE
  // NEGATIVE CONTROL: the engine calls every macro complete the cycle after it
  // is launched, so a vector instruction retires with its element progress
  // unfinished -- the units are still writing the VRF and the chain has accepted
  // no packets. CASE=vec.integrated's arith phase requires sixteen accepted
  // element completions and the architectural result; both fail.
  assign vec_unit_done = 1'b1;
`else
  assign vec_unit_done = (vec_pay_q.kind == 3'd1) ? vec_alu_done
                                                  : vec_lsu_finished;
`endif

  assign vec_macro_leave = (vec_valid_q && rob_retire_ack && vec_at_head) ||
                           redirect_valid;

  // internal CSR writes: clear vstart when a non-vset macro completes, and set
  // it to the faulting element when a memory macro traps.
  assign vec_vstart_clear = vec_wb_valid && (vec_pay_q.kind != 3'd0);
  assign vec_csr_internal = vec_vstart_clear || (vec_trap_take && vec_is_mem);
  assign vec_csr_internal_addr  = 12'h008;
  assign vec_csr_internal_wdata = vec_trap_take ? {57'd0, vec_vstart_q}
                                                : 64'd0;

  assign vec_csr_valid = vec_csr_internal ? 1'b1
                       : (csr_is_vec && (sys_exec ||
                                         (sys_wb_valid && sys_csr_writes_q)));
  assign vec_csr_addr  = vec_csr_internal ? vec_csr_internal_addr : csr_addr;
  assign vec_csr_write = vec_csr_internal ? 1'b1
                                          : (sys_wb_valid && sys_csr_writes_q);
  assign vec_csr_wdata = vec_csr_internal ? vec_csr_internal_wdata : sys_src1_q;

  assign vec_trap_take = (vec_state_q == VEC_TRAP) && vec_at_head &&
                         !redirect_valid && !recovering;

  // ------------------------------------------------------- the FSM registers
  always_ff @(posedge clk) begin
    if (rst) begin
      vec_valid_q        <= 1'b0;
      vec_index_q        <= {CORE_IDX_W{1'b0}};
      vec_gen_q          <= {CORE_RGEN_W{1'b0}};
      vec_uop_q          <= {CORE_UOP_W{1'b0}};
      vec_pay_q          <= '0;
      vec_src1_q         <= {CORE_XLEN{1'b0}};
      vec_src2_q         <= {CORE_XLEN{1'b0}};
      vec_dst_tag_q      <= {CORE_TAG_W{1'b0}};
      vec_dst_gen_q      <= {CORE_IGEN_W{1'b0}};
      vec_dst_x0_q       <= 1'b1;
      vec_state_q        <= VEC_IDLE;
      vec_done_q         <= 1'b0;
      vec_wb_pending_q   <= 1'b0;
      vec_trap_cause_q   <= {CORE_XLEN{1'b0}};
      vec_trap_tval_q    <= {CORE_XLEN{1'b0}};
      vec_vstart_q       <= 7'd0;
      vec_lsu_seen_busy_q<= 1'b0;
      vec_macro_ctr      <= 32'd0;
      vec_trap_ctr       <= 32'd0;
      vec_retire_ctr     <= 32'd0;
      vec_fault_ctr      <= 32'd0;
    end else begin
      if (vec_valid_q && (vec_state_q == VEC_RUN)) begin
        if (vec_lsu_owns_vrf) vec_lsu_seen_busy_q <= 1'b1;
      end
      if (vec_launch_unit) vec_lsu_seen_busy_q <= 1'b0;

      if (redirect_valid) begin
        vec_valid_q        <= 1'b0;
        vec_state_q        <= VEC_IDLE;
        vec_done_q         <= 1'b0;
        vec_wb_pending_q   <= 1'b0;
        vec_lsu_seen_busy_q<= 1'b0;
      end else begin
        if (vec_valid_q && rob_retire_ack && vec_at_head) begin
          vec_valid_q      <= 1'b0;
          vec_state_q      <= VEC_IDLE;
          vec_done_q       <= 1'b0;
          vec_wb_pending_q <= 1'b0;
          vec_retire_ctr   <= vec_retire_ctr + 32'd1;
        end
        if (disp_sys_valid && sys_ins_ready_int && disp_sys_is_vec) begin
          vec_valid_q      <= 1'b1;
          vec_index_q      <= disp_sys_id[CORE_UOP_W + CORE_RGEN_W +: CORE_IDX_W];
          vec_gen_q        <= disp_sys_id[CORE_UOP_W +: CORE_RGEN_W];
          vec_uop_q        <= disp_sys_id[CORE_UOP_W-1:0];
          vec_pay_q        <= disp_sys_vec;
          vec_src1_q       <= disp_sys_src1_val;
          vec_src2_q       <= disp_sys_src2_val;
          vec_dst_tag_q    <= disp_sys_dst_tag;
          vec_dst_gen_q    <= disp_sys_dst_gen;
          vec_dst_x0_q     <= disp_sys_dst_x0;
          vec_state_q      <= VEC_IDLE;
          vec_done_q       <= 1'b0;
          vec_wb_pending_q <= 1'b0;
          vec_lsu_seen_busy_q <= 1'b0;
          vec_macro_ctr    <= vec_macro_ctr + 32'd1;
        end

        case (vec_state_q)
          VEC_IDLE: begin
            if (vec_launch) begin
              if (vec_launch_vset) begin
                // vset commits this cycle (its registers update at the edge);
                // its completion is offered next cycle.
                vec_done_q  <= 1'b1;
                vec_state_q <= VEC_DONE;
              end else if (vec_illegal_launch) begin
                vec_trap_cause_q  <= mosaic_pkg::EXC_ILLEGAL_INSN;
                vec_trap_tval_q   <= {CORE_XLEN{1'b0}};
                vec_state_q       <= VEC_TRAP;
              end else begin
                vec_state_q <= VEC_RUN;
              end
            end
          end
          VEC_RUN: begin
            if (vec_unit_done) begin
              if (vec_rst_trap && vec_is_mem) begin
                vec_trap_cause_q <= vec_trap_cause_mem;
                vec_trap_tval_q  <= vec_trap_tval_mem;
                vec_vstart_q     <= vec_rst_vstart;
                vec_fault_ctr    <= vec_fault_ctr + 32'd1;
                vec_state_q      <= VEC_TRAP;
              end else if (vec_unit_illegal) begin
                vec_trap_cause_q <= mosaic_pkg::EXC_ILLEGAL_INSN;
                vec_trap_tval_q  <= {CORE_XLEN{1'b0}};
                vec_state_q      <= VEC_TRAP;
              end else begin
                vec_done_q  <= 1'b1;
                vec_state_q <= VEC_DONE;
              end
            end
          end
          VEC_DONE: begin
            if (vec_wb_valid) vec_wb_pending_q <= 1'b1;
          end
          VEC_TRAP: begin
            if (vec_trap_take) vec_trap_ctr <= vec_trap_ctr + 32'd1;
          end
          default: vec_state_q <= VEC_IDLE;
        endcase
      end
    end
  end

  assign vec_unit_illegal = (vec_pay_q.kind == 3'd1) ? vec_alu_illegal
                                                     : vec_rst_illegal;

  // The engine's element counter: every element completion the chain validated.
  always_ff @(posedge clk) begin
    if (rst) begin
      vec_elem_ctr <= 32'd0;
    end else if (vec_chain_accept_valid) begin
      vec_elem_ctr <= vec_elem_ctr + 32'd1;
    end
  end

  // ==========================================================================
  // 7c. The lane broker (I-059)
  // ==========================================================================
  // The vector engine's lane quota is a throughput knob, so a change to it is
  // safe exactly at a vector instruction boundary after the macro in flight has
  // drained. `mosaic_lane_broker` owns that transition by instantiating the
  // I-031 ownership FSM and binding its three settle classes to the engine's
  // three obligations: a staged macro (`uop`), a launched macro's element work
  // (`res`), and an uncollected macro completion (`crd`).
  //
  // The broker's `o_stop_admit` gates new macro *admission* in
  // `sys_ins_ready_int` below; a macro already staged is never stopped, because
  // it is the macro the drain is waiting for.
  assign lane_macro_insert = disp_sys_valid && sys_ins_ready_int && disp_sys_is_vec;

  // The completion that a writeback is owed for: the vset path sets `vec_done_q`
  // on its launch cycle, the unit path when the unit reports done without a trap
  // or an illegal.
  assign lane_wb_new = ((vec_state_q == VEC_IDLE) && vec_launch_vset) ||
                       ((vec_state_q == VEC_RUN) && vec_unit_done &&
                        !vec_rst_trap && !vec_unit_illegal);

  // The engine's own statement that its lane state for the old quota is
  // quiesced: no staged macro, no pending writeback, nothing running. The FSM
  // will not publish without it, so a quota is never committed over live state.
  assign lane_ack = (vec_state_q == VEC_IDLE) && !vec_valid_q && !vec_wb_pending_q;

  mosaic_lane_broker u_lane_broker (
      .clk             (clk),
      .rst             (rst),
      .req_valid_i     (lane_quota_req_i),
      .req_quota_i     (lane_quota_val_i),
      .macro_live_i    (lane_macro_live),
      .macro_new_i     (lane_macro_insert),
      .macro_done_i    (vec_macro_leave),
      .elem_new_i      (vec_launch_unit),
      .elem_done_i     (vec_unit_done),
      .wb_new_i        (lane_wb_new),
      .wb_done_i       (vec_wb_valid),
      .ack_i           (lane_ack),
      .o_quota         (lane_quota),
      .o_req_quota     (lane_req_quota),
      .o_stop_admit    (lane_stop_admit),
      .o_busy          (lane_busy),
      .o_gen           (lane_gen),
      .o_publish_ctr   (lane_publish_ctr),
      .o_ack_req_ctr   (lane_ack_req_ctr),
      .o_ack_ctr       (lane_ack_ctr),
      .o_req_mid_macro_ctr (lane_req_mid_macro_ctr),
      .o_pub_mid_macro_ctr (lane_pub_mid_macro_ctr),
      .o_abort_ctr     (lane_abort_ctr)
  );

  // The per-macro lane plan: the quota in force when the macro *launched*. It is
  // a snapshot, not a live value, and that is the whole mechanism -- a lane that
  // goes away after the launch cannot change which elements the running macro
  // owns, so no element is discarded. The plan also names the lane each element
  // completion is attributed to.
  always_ff @(posedge clk) begin
    if (rst) lane_plan_quota_q <= 4'd8;
    else if (vec_launch_unit) lane_plan_quota_q <= lane_quota;
  end

  // The lane an accepted element completion belongs to: element index modulo the
  // launch-time quota. Every quota is a power of two, so the modulo is a mask.
  assign lane_accept_lane = vec_chain_accept_index[2:0] & (3'(lane_plan_quota_q) - 3'd1);

  // Eight 32-bit counters packed lane 0 in the least significant slice. A for
  // loop rather than a generate block: this file uses no generate elsewhere,
  // and a loop keeps the per-lane logic and its read-back in one place.
  logic [255:0] lane_elem_ctr_q;
  always_ff @(posedge clk) begin
    if (rst) begin
      lane_elem_ctr_q <= 256'd0;
    end else begin
      for (int lane = 0; lane < 8; lane++) begin
        if (vec_desc_elem_done_valid && (lane_accept_lane == 3'(lane)))
          lane_elem_ctr_q[32*lane +: 32] <= lane_elem_ctr_q[32*lane +: 32] + 32'd1;
      end
    end
  end
  assign o_lane_elem_ctr = lane_elem_ctr_q;

  assign o_lane_quota        = lane_quota;
  assign o_lane_req_quota    = lane_req_quota;
  assign o_lane_gen          = lane_gen;
  assign o_lane_busy         = lane_busy;
  assign o_lane_stop_admit   = lane_stop_admit;
  assign o_lane_macro_live   = lane_macro_live;
  assign lane_macro_live     = vec_valid_q;
  assign o_lane_publish_ctr  = lane_publish_ctr;
  assign o_lane_ack_req_ctr  = lane_ack_req_ctr;
  assign o_lane_ack_ctr      = lane_ack_ctr;
  assign o_lane_req_mid_macro_ctr = lane_req_mid_macro_ctr;
  assign o_lane_pub_mid_macro_ctr = lane_pub_mid_macro_ctr;
  assign o_lane_abort_ctr    = lane_abort_ctr;

  // The vector-state write predicate for mstatus.VS. It is recorded per ROB slot
  // at allocation (like the FP FS predicate) and read when that slot retires, so
  // a vector macro a redirect discards never dirties VS.
  logic vec_state_wr_mem [0:CORE_ROB_N-1];
  logic vec_vs_dirty;
  integer vec_i;
  always_ff @(posedge clk) begin
    if (rst) begin
      for (vec_i = 0; vec_i < CORE_ROB_N; vec_i++) vec_state_wr_mem[vec_i] <= 1'b0;
    end else begin
      if (desc_wr_valid) begin
        vec_state_wr_mem[desc_wr_index] <= dbuf_ctl[0].is_vec &&
                                            (dbuf_ctl[0].vec_kind != 3'd3);
      end
      if (rob_flush_pulse) begin
        for (vec_i = 0; vec_i < CORE_ROB_N; vec_i++) vec_state_wr_mem[vec_i] <= 1'b0;
      end
    end
  end
  assign vec_vs_dirty = rob_retire_ack && vec_state_wr_mem[rob_head_index];

  // ---------------------------------------------------- the core's CSR read
`ifdef MOSAIC_LANE_MUTANT_VLEN_LEAK
  // NEGATIVE CONTROL (same defect as the export above, on the path software
  // actually reads): a `csrr vlenb` returns 16 + quota.
  assign csr_rdata_final = (vec_csr_addr == 12'hC22)
                         ? (vec_csr_rdata + {60'd0, lane_quota})
                         : (csr_is_vec ? vec_csr_rdata : csr_rdata);
`else
  assign csr_rdata_final = csr_is_vec ? vec_csr_rdata : csr_rdata;
`endif

  // -------- the vector engine's memory response return path (elem/field echo)
  // The packetizer identifies a response by the element and field of the
  // request it answers. The core's data port serves one accepted request at a
  // time, so the request currently outstanding is exactly the one the latch
  // holds; the fault flag and its class come from the reply itself.
  always_ff @(posedge clk) begin
    if (rst) begin
      vec_mem_rsp_elem_q  <= 7'd0;
      vec_mem_rsp_field_q <= 4'd0;
    end else if (vec_mem_req_valid && vec_mem_req_ready) begin
      vec_mem_rsp_elem_q  <= vec_mem_req_elem;
      vec_mem_rsp_field_q <= vec_mem_req_field;
    end
  end

  assign vec_mem_rsp_fault = vec_mem_rsp_valid && vec_mem_rsp_fault_raw;
  // The packetizer's fault classes: 1 page fault, 2 access fault. This
  // integration has no translation on the vector path (the profile is bare at
  // reset and the report says so), so a refused access is an access fault.
  assign vec_mem_rsp_code  = vec_mem_rsp_fault ? 4'd2 : 4'd0;

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
      .wb_ev2              (port2_ev),
      .wb_valid2           (port2_valid),
      .wb_ready2           (port2_ready),
      // The memory path's producer: a load's merged value or a store's
      // destination-less completion. One port, because the two can never be
      // offered in the same cycle (the core holds the store insert while a load
      // result waits, and vice versa). The CSR/system unit shares this port
      // rather than adding a fifth one: its completion is offered in the same
      // register-transfer shape, and the core keeps the two apart by
      // construction -- see section 10b.
      .wb_ev3              (wb3_ev),
      .wb_valid3           (wb3_valid),
      .wb_ready3           (lsu_wb_ready_int),
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
      .fab_dyn          (fab_dyn_steer),
      .dec_valid        ({1'b0, dbuf_valid[0]}),
      .dec_ctl0         (dbuf_ctl[0]),
      .dec_ctl1         (dbuf_ctl[1]),
      .dec_pc0          (dbuf_pc[0]),
      .dec_pc1          (dbuf_pc[1]),
      // I-041: the oldest buffered instruction's own length and bits.
      .dec_len0         (dbuf_len[0]),
      .dec_bits0        (dbuf_bits[0]),
      .alloc_req        (ren_alloc_req),
      .alloc_rd         (alloc_rd_w),
      .alloc_is_fp      (ren_alloc_is_fp),
      .alloc_accepted   (ren_alloc_accepted),
      .alloc_exhausted  (ren_alloc_exhausted),
      .alloc_squashed   (ren_alloc_squashed),
      .alloc_is_x0      (ren_alloc_is_x0),
      .alloc_new_valid  (ren_alloc_new_valid),
      .alloc_new_tag    (ren_new_tag),
      .alloc_new_gen    (ren_new_gen),
      .rs1_addr         (ren_rs1_addr),
      .rs2_addr         (ren_rs2_addr),
      .rs1_is_fp        (ren_rs1_is_fp),
      .rs2_is_fp        (ren_rs2_is_fp),
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
      .desc_wr_len      (desc_wr_len),
      .desc_wr_insn     (desc_wr_insn),
      // ---------------------------------------------------------- memory insert
      .mem_ins_valid    (disp_mem_valid),
      .mem_ins_ready    (disp_mem_ready),
      .mem_ins_is_store (disp_mem_is_store),
      .mem_ins_is_fp    (disp_mem_is_fp),
      .mem_ins_id       (disp_mem_id),
      .mem_ins_base     (disp_mem_base),
      .mem_ins_imm      (disp_mem_imm),
      .mem_ins_size     (disp_mem_size),
      .mem_ins_signed   (disp_mem_signed),
      .mem_ins_is_amo   (disp_mem_is_amo),
      .mem_ins_amo_op   (disp_mem_amo_op),
      .mem_ins_amo_aq   (disp_mem_amo_aq),
      .mem_ins_amo_rl   (disp_mem_amo_rl),
      .mem_ins_is_lr    (disp_mem_is_lr),
      .mem_ins_is_sc    (disp_mem_is_sc),
      .mem_ins_data     (disp_mem_data),
      .mem_ins_dst_tag  (disp_mem_dst_tag),
      .mem_ins_dst_gen  (disp_mem_dst_gen),
      .mem_ins_dst_x0   (disp_mem_dst_x0),
      // ---------------------------------------------------------- system insert
      .sys_ins_valid    (disp_sys_valid),
      .sys_ins_ready    (sys_ins_ready_int),
      .sys_ins_id       (disp_sys_id),
      .sys_ins_csr_addr (disp_sys_csr_addr),
      .sys_ins_csr_op   (disp_sys_csr_op),
      .sys_ins_csr_reads(disp_sys_csr_reads),
      .sys_ins_csr_writes(disp_sys_csr_writes),
      .sys_ins_is_ecall (disp_sys_is_ecall),
      .sys_ins_is_ebreak(disp_sys_is_ebreak),
      .sys_ins_is_mret  (disp_sys_is_mret),
      .sys_ins_is_sret  (disp_sys_is_sret),
      .sys_ins_is_wfi   (disp_sys_is_wfi),
      .sys_ins_is_fetch_fault (disp_sys_is_fetch_fault),
      .sys_ins_is_fence (disp_sys_is_fence),
      .sys_ins_is_fence_i(disp_sys_is_fence_i),
      .sys_ins_is_sfence_vma   (disp_sys_is_sfence_vma),
      .sys_ins_sfence_has_va   (disp_sys_sfence_has_va),
      .sys_ins_sfence_has_asid (disp_sys_sfence_has_asid),
      .sys_ins_is_vec   (disp_sys_is_vec),
      .sys_ins_vec      (disp_sys_vec),
      .sys_ins_src1_val (disp_sys_src1_val),
      .sys_ins_src2_val (disp_sys_src2_val),
      .sys_ins_dst_tag  (disp_sys_dst_tag),
      .sys_ins_dst_gen  (disp_sys_dst_gen),
      .sys_ins_dst_x0   (disp_sys_dst_x0),
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
      .c0_count_i       (c0_count),
      .c1_count_i       (c1_count),
      .o_target_cluster (fab_alloc_cluster),
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
      // A WFI halt holds allocation exactly as an unresolved branch does: the
      // front end has nothing to run until the wake event.
      .barrier          (br_inflight | wfi_halt),
      // The barrier above also holds for a WFI halt, which is not a transfer
      // (its fall-through is architectural). The refusal rule needs the
      // narrower fact: "an unresolved branch may still squash younger work".
      .branch_in_flight (br_inflight),
      // V (I-059): a vector macro is live, so nothing younger allocates. The
      // macro then reaches the head with the machine behind it empty.
      .vec_block_i      (vec_block),
      .trap_vector_armed_i(trap_vector_armed),
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
      .o_queue_full     (),

      // -------------------------------------------------- fabric observation
      .o_fab_unit_issues(o_fab_unit_issues),
      .o_fab_reason_ctr (o_fab_reason_ctr),
      .o_fab_grant_ctr  (o_fab_grant_ctr),
      .o_fab_stall_ctr  (o_fab_stall_ctr),
      .o_fab_reject_ctr (o_fab_reject_ctr),
      .o_fab_units      (o_fab_units),
      .o_fab_classes    (o_fab_classes),
      .o_fab_age_w      (o_fab_age_w),
      .o_fab_occ_w      (o_fab_occ_w),
      .o_fab_unit_w     (o_fab_unit_w)
  );

  assign core_stop = disp_unsupported;

  // ==========================================================================
  // 10a. The CSR file and the interrupt decision (I-019, I-020)
  // ==========================================================================
  // Both modules are instantiated here and nowhere deeper, because both are
  // *architectural boundary* devices: the CSR file's write port is strobed by
  // the retire boundary, and the interrupt unit's decision is gated by
  // `core_can_trap`, which only the integrator can define (a boundary is a
  // property of the whole machine, not of the interrupt unit).
  //
  // The CSR read address is the staged macro's, always: the only consumer of the
  // read port is the system unit, and it presents one macro at a time.

  // "Software has installed a trap vector": see mosaic_dispatch's refusal rule.
  // p0's mtvec resets to 0, and a machine with no handler installed stops at a
  // trap-raising system instruction instead of vectoring into address 0.
  assign trap_vector_armed = (o_csr_mtvec != {CORE_XLEN{1'b0}});

  assign csr_addr = sys_csr_addr_q;
  // The write is strobed exactly when a system macro's completion is accepted by
  // the writeback path, so a CSR write is applied by the retirement of its
  // instruction and by nothing else -- a squashed or trapping macro never
  // strobes it. An *illegal* write is strobed too and refused by the CSR file's
  // generated legality rule; that is what makes the illegal-access trap
  // detectable without a second copy of the table here.
  assign csr_we = sys_wb_valid && sys_csr_writes_q && !csr_is_vec;

  mosaic_csr u_csr (
      .clk_i              (clk),
      .rst_i              (rst),
      .csr_addr_i         (csr_addr),
      .csr_rdata_o        (csr_rdata),
      .csr_illegal_o      (csr_illegal),
      .csr_we_i           (csr_we),
      .csr_op_i           (sys_csr_op_q),
      .csr_wdata_i        (sys_src1_q),
      .csr_wr_illegal_o   (csr_wr_illegal),
      // mcycle counts every core cycle; minstret counts retired instructions,
      // which is what the retire acknowledgement says.
      .cnt_cycle_i        (1'b1),
      .cnt_instret_i      (rob_retire_ack | rob_retire_ack_next),
      .trap_valid_i       (csr_trap_valid),
      .trap_cause_i       (trap_cause),
      .trap_tval_i        (trap_tval),
      .trap_epc_i         (trap_epc),
      .trap_commit_o      (),
      .trap_target_o      (csr_trap_target),
      .mret_valid_i       (csr_mret_valid),
      .mret_commit_o      (o_mret_valid),
      .mret_target_o      (csr_mret_target),
      .sret_valid_i       (csr_sret_valid),
      .sret_commit_o      (csr_sret_commit),
      .sret_target_o      (csr_sret_target),
      .mret_illegal_o     (csr_mret_illegal),
      .sret_illegal_o     (csr_sret_illegal),
      .wfi_illegal_o      (csr_wfi_illegal),
      .o_priv_o           (csr_priv),
      .o_medeleg_o        (csr_medeleg),
      .o_mideleg_o        (csr_mideleg),
      .pmp_sel_o          (csr_pmp_sel),
      .pmp_rdata_i        (csr_pmp_rdata),
      .pmp_we_o           (csr_pmp_we),
      .pmp_wdata_o        (csr_pmp_wdata),
      .o_sstatus_o        (csr_sstatus),
      .o_stvec_o          (csr_stvec),
      .o_sepc_o           (csr_sepc),
      .o_scause_o         (csr_scause),
      .o_stval_o          (csr_stval),
      .o_sscratch_o       (csr_sscratch),
      .o_sie_o            (),
      .o_sip_o            (),
      .o_satp_o           (csr_satp),
      .o_senvcfg_o        (),
      .o_scounteren_o     (),
      .o_mcounteren_o     (),
      .o_sret_ctr         (csr_sret_ctr),
      .o_trap_s_ctr       (csr_trap_s_ctr),
      .o_priv_illegal_ctr (csr_priv_illegal_ctr),
      .o_priv_change_ctr  (csr_priv_change_ctr),
      .mip_i              (irq_mip),
      .mip_we_o           (csr_mip_we),
      .mip_op_o           (csr_mip_op),
      .mip_wdata_o        (csr_mip_wdata),
      .mtime_i            (mtime_i),
      .o_mstatus_o        (o_csr_mstatus),
      .o_mtvec_o          (o_csr_mtvec),
      .o_mepc_o           (o_csr_mepc),
      .o_mcause_o         (o_csr_mcause),
      .o_mtval_o          (o_csr_mtval),
      .o_mscratch_o       (o_csr_mscratch),
      .o_mie_o            (o_csr_mie),
      .o_mip_o            (o_csr_mip),
      // F/D (I-050): the FP control state and the commit-time interfaces.
      .o_fcsr_o           (o_csr_fcsr),
      .o_fflags_o         (o_csr_fflags),
      .o_frm_o            (o_csr_frm),
      .fp_fflags_or_i     (fp_fflags_or),
      .fp_fs_dirty_i      (fp_fs_dirty),
      .vec_vs_dirty_i     (vec_vs_dirty),
      .o_misa_o           (),
      .o_mcycle_o         (),
      .o_minstret_o       (),
      .o_wr_ctr           (o_csr_wr_ctr),
      .o_illegal_wr_ctr   (o_csr_illegal_wr_ctr),
      .o_trap_ctr         (o_csr_trap_ctr),
      .o_mret_ctr         (o_csr_mret_ctr)
  );

  // `mideleg` is tied to zero rather than read out of the CSR file, and the tie
  // is a statement the CSR file's own rule makes: p0 is M-only, so every bit of
  // mideleg is WARL whose only legal value is 0, a write is accepted and
  // canonicalises to zero, and the generated write mask is 0. There is no value
  // this input could ever carry. A profile with an S mode would need the CSR
  // file to bring the register out, and this is where that would be wired.
  mosaic_interrupt u_irq (
      .clk_i              (clk),
      .rst_i              (rst),
      .irq_soft_i         (irq_soft_i),
      .irq_timer_i        (irq_timer_i),
      .irq_ext_i          (irq_ext_i),
      .mie_i              (o_csr_mie),
      .mideleg_i          (csr_mideleg),
      .mstatus_mie_i      (o_csr_mstatus[3]),
      .priv_i             (csr_priv),
      .mstatus_sie_i      (o_csr_mstatus[1]),
      .mip_we_i           (csr_mip_we),
      // mosaic_csr publishes the mip write operation as the 2-bit encoding;
      // the interrupt unit takes the csr_op_e the rest of the core uses.
      .mip_op_i           (mosaic_pkg::csr_op_e'(csr_mip_op)),
      .mip_wdata_i        (csr_mip_wdata),
      .mip_o              (irq_mip),
      .core_can_trap_i    (core_can_trap),
      .irq_valid_o        (irq_valid),
      .irq_cause_o        (irq_cause),
      .irq_timer_pending_o(),
      .o_irq_soft_pending (),
      .o_irq_ext_pending  (),
      .wfi_valid_i        (sys_wb_valid && sys_wfi_q && !sys_exc),
      .wfi_halt_o         (wfi_halt),
      .o_irq_ctr          (irq_ctr),
      .o_halt_cycles      (halt_cycles),
      .o_wake_ctr         (),
      .o_spurious_wake_ctr(spurious_wake_ctr)
  );

  // ==========================================================================
  // 10b. The system unit and the trap controller
  // ==========================================================================
  //
  // A CSR instruction cannot be executed by an execution unit and cannot produce
  // its result speculatively:
  //
  //   * its *read* must return the CSR state left by every older instruction, so
  //     it cannot run before those have retired;
  //   * its *write* must not be visible until the instruction itself retires, so
  //     it cannot run before that either.
  //
  // The machine therefore resolves a system macro at the ROB head, in the same
  // cycle the entry would be retiring, and the completion it produces is the
  // ordinary writeback completion: the CSR read value goes to the destination
  // through the register file and the wakeup network, the ROB entry is marked
  // done, and the write port is strobed on the same edge. Because the head is the
  // oldest unretired instruction, "at the head" *is* "after every older
  // instruction has committed", which is exactly the ordering both rules need.
  // Nothing younger has to be held: a younger consumer simply waits for the
  // wakeup of the destination tag it already named.
  //
  // The same unit resolves the three system instructions that are not CSR
  // accesses: ECALL, EBREAK and MRET. ECALL and EBREAK (and an illegal CSR
  // access) are delivered as an *exception completion* -- the writeback event
  // carries `exc.valid`, so the ROB marks the entry exceptional through the same
  // port every other fault uses, and the trap controller below then has exactly
  // one synchronous source to look at.
  //
  // One macro at a time is staged, because only one entry can be the head. The
  // staging entry is filled by dispatch in the same cycle the macro is allocated
  // (dispatch holds the macro until the entry is free, so allocation order stays
  // program order) and cleared by a redirect, whose flush discards everything at
  // and above the head.

  assign sys_head = sys_valid_q && rob_head_valid &&
                    (rob_head_index == sys_index_q) && (rob_head_gen == sys_gen_q);
  // The staging entry is free unless a macro is staged in it; that is what
  // keeps one entry enough, because dispatch holds the macro until it can be
  // taken.
  // A vector macro is admitted here; the lane broker's stop-admit holds new
  // admission while a quota change is draining (I-059). A macro already staged
  // is not affected -- it is the work the drain is waiting for -- so this gate
  // can never deadlock against the drain.
  assign sys_ins_ready_int = !sys_valid_q && !vec_valid_q && !lane_stop_admit;

  // --------------------------------------------------------------------------
  // The fence rule (I-037)
  // --------------------------------------------------------------------------
  // A FENCE or FENCE.I is resolved here, at the ROB head, exactly as a CSR
  // access is -- but its completion is not "the head is reached". The rule this
  // profile ships is the conservative one the work package asks for:
  //
  //   A fence is complete only when the memory path it orders is *idle*: the
  //   store queue holds nothing, the load queue holds nothing, and the memory
  //   endpoint has no transaction outstanding (no accepted request in flight
  //   and no response still held for a consumer).
  //
  //   While a fence is staged, no *younger* memory macro may be handed to a
  //   queue: the memory insert port is refused, so a load or store younger than
  //   the fence cannot allocate until the fence has retired. Younger non-memory
  //   work still allocates and executes; it simply cannot retire past the fence,
  //   because the fence owns the ROB head.
  //
  // Why each conjunction is there, and what is deliberately *not* drained, is
  // argued in results/reports/I-037-fence.md. In one line: the store queue and
  // the endpoint are the two structures through which a younger access could
  // otherwise overtake a store this fence must order; the load queue is drained
  // with them because the load queue is where a *prior* access could still be
  // outstanding, and this profile chose the strong reading ("every older access
  // has completed") over the minimum RVWMO obligation.
  //
  // `fence_like_q` is the staged class; `fence_pending` is the same thing named
  // for the block, and both clear when the macro retires or a redirect discards
  // it (the staging entry's own lifetime).
  assign fence_like_q = sys_valid_q && (sys_fence_q || sys_fence_i_q ||
                                        sys_sfence_vma_q);
  assign fence_pending = fence_like_q;

  // The device serializer is part of the memory path for this rule (I-038): a
  // device transaction that has been classified and taken into the serializer's
  // register is an access in flight even before the endpoint has accepted it, so
  // a fence that ignored it could retire with an MMIO access still owed. The
  // conjunction below therefore uses the serializer's busy too, and
  // `o_mem_lsu_busy` reports the whole path.
  logic dev_ser_busy;
  assign dev_ser_busy = ser_hold_valid_q || ser_dev_out_q;
  assign mem_path_idle = (sq_count == {CORE_MEM_CNT_W{1'b0}}) &&
                         (lq_count == {CORE_MEM_CNT_W{1'b0}}) &&
                         !ep_busy && !dev_ser_busy;

`ifdef MOSAIC_CORE_MUTANT_FENCE_EARLY
  // NEGATIVE CONTROL: the fence does not wait for the memory path. It is
  // "complete" as soon as it reaches the ROB head, so it can retire with an
  // older store still queued -- CASE=fence.code_and_data_order's
  // "the store queue is empty at every fence retirement" check names it, and
  // the data-ordering program reads a device register one access too early.
  assign fence_mem_ok = 1'b1;
`else
  // A write to mstatus is in the same class as a fence for the memory path, and
  // for a reason that is about *this* profile: mstatus.MPRV and MPP select the
  // effective privilege of loads and stores, and a CSR write does not redirect.
  // The write is resolved at the ROB head, so an access younger than it would
  // otherwise be checked against a privilege the instruction that made it did
  // not execute under. Draining the memory path before the write executes, and
  // refusing younger memory macros while it is staged (below), is what makes the
  // sampled value the architectural one.
  assign fence_mem_ok = (!fence_like_q && !sys_priv_wr_q) || mem_path_idle;
`endif

`ifdef MOSAIC_CORE_MUTANT_FENCE_ACCESS_PAST
  // NEGATIVE CONTROL: younger *loads* are not blocked while a fence is staged
  // (younger stores still are: a younger store in the queue would make the
  // fence's own "store queue empty" drain rule unsatisfiable, so the mutation
  // would deadlock rather than let an access past). The younger load allocates
  // and issues while the older store is still queued, reads the device before
  // the publish, and the data-ordering program publishes a wrong difference.
  logic fence_block_younger_c;
  assign fence_block_younger_c = fence_pending && disp_mem_is_store;
`else
  logic fence_block_younger_c;
  assign fence_block_younger_c = fence_pending || sys_priv_wr_q;
`endif

  // The one thing that makes a system macro trap rather than complete. `csr_illegal`
  // is "the address is not implemented"; `csr_wr_illegal` is "this write is not
  // legal for that address" (a read-only CSR, or one the profile does not
  // implement), and it is evaluated on the *intended* write, so it does not
  // depend on the completion being accepted.
  logic csr_access_illegal;

  assign csr_access_illegal = (sys_csr_op_q != mosaic_pkg::CSR_NONE) &&
                              (csr_is_vec ? vec_csr_illegal
                               : (csr_illegal ||
                                  (sys_csr_writes_q && csr_wr_illegal)));

  // SFENCE.VMA's own legality (I-046). The spec makes it an illegal instruction
  // in U-mode, and in S-mode when mstatus.TVM is set ("attempts to ... execute
  // SFENCE.VMA ... while executing in S-mode will raise an illegal instruction
  // exception"). It is available in M-mode. The TVM bit is mstatus[20], the same
  // bit the CSR file reads for the satp-access rule.
  logic csr_sfence_illegal;
  assign csr_sfence_illegal = (csr_priv == mosaic_csr_pkg::MOSAIC_PRIV_U) ||
                              ((csr_priv == mosaic_csr_pkg::MOSAIC_PRIV_S) &&
                               (o_csr_mstatus[20] == 1'b1));

  // The satp-write pulse is derived from the committed register itself rather
  // than from the write's address, because `satp` and its address constant
  // exist only in a profile with S-mode: a change of the committed value is
  // exactly "an instruction wrote satp and the write was not canonicalised
  // away", and it needs no profile-specific name here.
  logic [63:0] csr_satp_prev_q;
  always_ff @(posedge clk) begin
    if (rst) csr_satp_prev_q <= 64'd0;
    else     csr_satp_prev_q <= csr_satp;
  end

  logic tlb_sfence_valid;
  logic tlb_satp_write;
  logic [63:0] tlb_sfence_va;
  logic [15:0] tlb_sfence_asid;

  assign tlb_sfence_valid = sys_wb_valid && sys_sfence_vma_q && !sys_exc;
  assign tlb_satp_write   = (csr_satp != csr_satp_prev_q);
  assign tlb_sfence_va    = sys_src1_q;
  assign tlb_sfence_asid  = sys_src2_q[15:0];

  always_comb begin
    sys_exc       = 1'b0;
    sys_exc_cause = {CORE_XLEN{1'b0}};
    if (sys_head && !rob_head_complete) begin
      if (sys_fetch_fault_q) begin
        // The front end was not allowed to read this address. The cause is the
        // instruction *access* fault (1), not the illegal-instruction exception
        // (2): the machine never saw an encoding to call illegal, and the value
        // the memory system would have refused is the PC, which is what
        // mtval/stval receive for this cause.
        sys_exc       = 1'b1;
        sys_exc_cause = mosaic_pkg::EXC_INSN_ACCESS;
      end else if (sys_ecall_q) begin
        // ECALL has one encoding and three causes: the code identifies the mode
        // the call was made *from*, which is the architectural privilege at the
        // boundary -- the system unit resolves the macro at the ROB head, so
        // that is the instruction's own mode and not a speculative guess.
        sys_exc       = 1'b1;
        sys_exc_cause = (csr_priv == mosaic_csr_pkg::MOSAIC_PRIV_M) ? mosaic_pkg::EXC_ECALL_M
                      : (csr_priv == mosaic_csr_pkg::MOSAIC_PRIV_S) ? mosaic_pkg::EXC_ECALL_S
                                                                   : mosaic_pkg::EXC_ECALL_U;
      end else if (sys_ebreak_q) begin
        sys_exc       = 1'b1;
        sys_exc_cause = mosaic_pkg::EXC_BREAKPOINT;
      end else if (csr_access_illegal) begin
        sys_exc       = 1'b1;
        sys_exc_cause = mosaic_pkg::EXC_ILLEGAL_INSN;
      end else if (sys_mret_q && csr_mret_illegal) begin
        // "An xRET instruction can be executed in privilege mode x or higher":
        // MRET below M-mode is an illegal instruction.
        sys_exc       = 1'b1;
        sys_exc_cause = mosaic_pkg::EXC_ILLEGAL_INSN;
      end else if (sys_sret_q && csr_sret_illegal) begin
        // SRET in U-mode, or in S-mode with mstatus.TSR set.
        sys_exc       = 1'b1;
        sys_exc_cause = mosaic_pkg::EXC_ILLEGAL_INSN;
      end else if (sys_wfi_q && csr_wfi_illegal) begin
        // WFI in U-mode, or in S-mode with mstatus.TW set.
        sys_exc       = 1'b1;
        sys_exc_cause = mosaic_pkg::EXC_ILLEGAL_INSN;
      end else if (sys_sfence_vma_q && csr_sfence_illegal) begin
        // SFENCE.VMA in U-mode, or in S-mode with mstatus.TVM set: the
        // specification makes both an illegal instruction.
        sys_exc       = 1'b1;
        sys_exc_cause = mosaic_pkg::EXC_ILLEGAL_INSN;
      end
    end
  end

  // Execution. A trap being taken in this cycle wins: the interrupt is blocked
  // outright while a system macro is at the head (see `core_can_trap` below), and
  // a synchronous exception on the head is not a system macro's to make.
  // `sys_wb_pending_q` is the "one completion outstanding" rule the writeback
  // arbiter's producers keep: the system macro is executed once and its
  // completion is offered once. Without it the macro would be re-executed in the
  // cycle the arbiter *publishes* that completion, because the ROB's done bit
  // only lands at the end of that cycle -- which would commit a CSR write and an
  // MRET twice.
  assign sys_exec = sys_head && !rob_head_complete && !sys_wb_pending_q &&
                    !head_exc_trap && !irq_valid &&
                    !sys_trap_q && !recovering && !redirect_valid && !core_stop &&
                    fence_mem_ok;
  // The trap this macro resolves to, latched in the cycle its completion is
  // accepted so that the trap controller sees it at the boundary one cycle later.
  assign sys_trap_now = sys_head && !rob_head_complete && sys_exc;

  // The completion, on the writeback arbiter's memory port (port 3). That port is
  // shared rather than duplicated because the two producers can be kept apart by
  // construction: while the system unit wants it, the memory path is held --
  // a load result waits in the load queue, and a store insert is refused (which
  // is what keeps `store_cmp_valid` from being offered and lost). The system unit
  // in turn waits for the port to be free (`wb_ready3`), so a retry never
  // re-applies a CSR write that the first attempt already made.
  always_comb begin
    sys_wb_ev.id.hart      = 1'b0;
    sys_wb_ev.id.rob_index = sys_index_q;
    sys_wb_ev.id.rob_gen   = sys_gen_q;
    sys_wb_ev.id.uop_index = sys_uop_q;
    sys_wb_ev.dst.tag      = sys_dst_x0_q ? {CORE_TAG_W{1'b0}} : sys_dst_tag_q;
    sys_wb_ev.dst.gen      = {{(CORE_PGEN_W - CORE_IGEN_W){1'b0}}, sys_dst_gen_q};
    sys_wb_ev.dst.x0       = sys_dst_x0_q;
    sys_wb_ev.value_valid  = !sys_dst_x0_q;
    sys_wb_ev.value        = sys_csr_reads_q ? csr_rdata_final : {CORE_XLEN{1'b0}};
    // A system macro never raises its exception through the writeback path.
    // The retire stream publishes one event per retired macro, and an
    // exception completion would publish a second lane-0 event for a macro
    // that never retired. ECALL/EBREAK/illegal-CSR are therefore *latched*
    // here and taken by the trap controller below, at the boundary, one cycle
    // later; a memory fault keeps the writeback route, because that is where
    // the LSU reports it and where the ROB's exception bit comes from.
    sys_wb_ev.exc.valid    = 1'b0;
    sys_wb_ev.exc.cause    = {CORE_XLEN{1'b0}};
    sys_wb_ev.exc.tval     = {CORE_XLEN{1'b0}};
    sys_wb_ev.is_store     = 1'b0;
    sys_wb_ev.is_load      = 1'b0;
  end

  assign sys_wb_want = sys_exec;
  assign port3_taken_sys = sys_wb_want;
  assign port3_taken_vec = vec_wb_want && !port3_taken_sys;
  assign vec_wb_valid    = vec_wb_want && lsu_wb_ready_int;
  assign sys_wb_valid    = sys_wb_want && lsu_wb_ready_int;

  // ==========================================================================
  // The cache fence (I-042): FENCE.I writes the D-cache back, then invalidates
  // the I-cache
  // ==========================================================================
  // The caches are write-back and non-coherent, so the store a program patches
  // its own code with can still be *dirty in the data cache* when the FENCE.I
  // retires -- and the instruction cache, refilling from memory, would read the
  // old bytes. The fence therefore does two things, in this order:
  //
  //   1. it writes the data cache back to memory (and invalidates it), so the
  //      patched bytes are in memory;
  //   2. it invalidates the instruction cache, so the next fetch refills.
  //
  // It holds the front end off for the whole window: `icache_flush` is a *level*
  // asserted from the first step to the last, and the instruction cache path
  // refuses requests while it is high. The two steps are ordered because step 2's
  // refill must not read memory before step 1's bytes have been written there.
  //
  // The sequence starts in the cycle FENCE.I executes (`sys_exec`, which the
  // existing drain rule has already gated on the memory path being idle) and only
  // when the caches are enabled: with `cache_en_i` low there is nothing to flush
  // and the machine's cycle timing is exactly what it was.
  typedef enum logic [1:0] { CF_IDLE = 2'd0, CF_D = 2'd1, CF_I = 2'd2 } cf_state_e;
  cf_state_e cf_state;

  always_ff @(posedge clk) begin
    if (rst) begin
      cf_state <= CF_IDLE;
    end else begin
      case (cf_state)
        CF_IDLE: if (cache_en_i && sys_exec && sys_fence_i_q) cf_state <= CF_D;
`ifdef MOSAIC_CACHE_MUTANT_FENCEI_NO_FLUSH
        // NEGATIVE CONTROL: the fence writes the data cache back and then stops,
        // never invalidating the instruction cache. The patched bytes do reach
        // memory (so the store itself is not the defect) and the front end keeps
        // executing the line it already holds, which is exactly the
        // self-modifying-code hazard FENCE.I exists to close. The case's
        // "FENCE.I made the patched instruction visible" comparison names it.
        CF_D:    if (dcache_flush_done) cf_state <= CF_IDLE;
`else
        CF_D:    if (dcache_flush_done) cf_state <= CF_I;
`endif
        CF_I:    if (icache_flush_done) cf_state <= CF_IDLE;
        default: cf_state <= CF_IDLE;
      endcase
    end
  end

  assign dcache_flush     = (cf_state == CF_D);
`ifdef MOSAIC_CACHE_MUTANT_FENCEI_NO_FLUSH
  // NEGATIVE CONTROL (continued): the instruction cache is never asked to
  // invalidate, so no flush acknowledgement is waited for either.
  assign icache_flush     = 1'b0;
  assign cache_fence_busy = (cf_state == CF_D);
`else
  assign icache_flush     = (cf_state != CF_IDLE);
  assign cache_fence_busy = (cf_state != CF_IDLE);
`endif
  assign wb3_valid    = port3_taken_sys ? sys_wb_valid
                      : (port3_taken_vec ? vec_wb_valid : lsu_wb_valid);
  assign wb3_ev       = port3_taken_sys ? sys_wb_ev
                      : (port3_taken_vec ? vec_wb_ev : lsu_wb_ev);
  assign lsu_wb_ready = lsu_wb_ready_int && !port3_taken_sys && !port3_taken_vec;

  // MRET updates mstatus in its own execution cycle, exactly as a CSR write
  // would, and its redirect is requested from the cycle its entry is complete --
  // the arbiter's head-retire gate is what makes it act in the cycle the MRET
  // instruction actually retires.
  // A staged system macro that writes mstatus. See the fence rule above for what
  // this buys: the write cannot execute until the memory path is idle, and no
  // younger memory macro may allocate while it is staged, so no access can be
  // checked against a privilege its instruction did not execute under.
  assign sys_priv_wr_q = sys_valid_q && sys_csr_writes_q &&
                         (sys_csr_addr_q == mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSTATUS);

  assign csr_mret_valid = sys_wb_valid && sys_mret_q && !sys_exc;
  // SRET is strobed exactly like MRET: the CSR file performs the supervisor
  // return in the cycle the instruction's completion is accepted by the
  // writeback arbiter, and the redirect that follows restarts the front end at
  // sepc.
  assign csr_sret_valid = sys_wb_valid && sys_sret_q && !sys_exc;
  // `!sys_exc` is what makes an *illegal* xRET an exception rather than an
  // effect. The macro's completion is still offered -- that is what latches
  // `sys_trap_q` and takes the trap -- but the return itself must not happen:
  // without this term an MRET executed below M-mode changed the privilege and
  // redirected to mepc in the same cycle its exception was recorded, so the trap
  // was then taken from the mode the illegal return had just entered.
  // CASE=privilege.permission_matrix found exactly that, as `mret` executed in
  // S-mode trapping with mstatus.MPP = U.

  // ------------------------------------------------------- the trap controller
  // Two things can trap from the head, and they are the same event as far as
  // everything downstream is concerned:
  //
  //   * a *synchronous* exception. The ROB carries the bit and this file carries
  //     the payload (cause and tval), recorded from the completion that raised it
  //     -- so a load/store fault from the LSU and an ECALL/EBREAK/illegal-CSR
  //     from the system unit arrive here by one path.
  //   * an *interrupt*, offered by mosaic_interrupt only at a boundary this file
  //     declares legal.
  //
  // The PC written to mepc differs between the two and only by what the ISA says:
  // for a synchronous exception it is the faulting instruction (the head, which
  // does not retire), and for an interrupt it is the instruction that will
  // execute after the return -- which is the same head, because the interrupt is
  // taken *before* it retires.
  assign exc_gen_match  = exc_rec_gen_q[rob_head_index];
  assign exc_cause_head = (exc_gen_match == rob_head_gen)
                          ? exc_cause_q[rob_head_index] : {CORE_XLEN{1'b0}};
  assign exc_tval_head  = (exc_gen_match == rob_head_gen)
                          ? exc_tval_q[rob_head_index] : {CORE_XLEN{1'b0}};

  // The retire event's own payload: the same records, for both lanes. The trap
  // event a memory fault emits on the retire stream therefore carries the cause
  // and tval the fault reported, instead of the zero a second copy of the
  // exception path would have supplied.
  assign pay_exc_cause_vec = {
      ((exc_rec_gen_q[rob_head1_index] == rob_head1_gen)
       ? exc_cause_q[rob_head1_index] : {CORE_XLEN{1'b0}}),
      exc_cause_head};
  assign pay_exc_tval_vec = {
      ((exc_rec_gen_q[rob_head1_index] == rob_head1_gen)
       ? exc_tval_q[rob_head1_index] : {CORE_XLEN{1'b0}}),
      exc_tval_head};

  assign head_exc_trap = rob_head_valid && rob_head_exc &&
                         !redirect_valid && !recovering;
  // Every source of a trap is gated the same way, and the gate has to be here
  // rather than only on the arrival of the redirect: a resolved system trap is
  // held until the redirect that takes it, and without the gate it would be
  // taken a second time in the very cycle that redirect is in flight -- as a
  // second trap with a stale head. `irq_valid` is already gated this way by
  // `core_can_trap`.
  assign sys_trap_take = sys_trap_q && !redirect_valid && !recovering;
  assign trap_is_irq   = !head_exc_trap && !sys_trap_take && !vec_trap_take && irq_valid;
  assign trap_decision = head_exc_trap || sys_trap_take || vec_trap_take || irq_valid;

`ifdef MOSAIC_CORE_MUTANT_TRAP_EPC_NEXT
  // NEGATIVE CONTROL: the exception Program Counter is the instruction *after*
  // the faulting one. `mret` then resumes at the wrong instruction, and the
  // failing program's own trap handler -- which compares mepc against the
  // address it armed -- reports it.
  assign trap_epc_sync = rob_head_pc + CORE_XLEN'(4);
`else
  assign trap_epc_sync = rob_head_pc;
`endif
`ifdef MOSAIC_CORE_MUTANT_IRQ_EPC_NEXT
  // NEGATIVE CONTROL: the interrupted instruction is skipped by skipping its
  // Program Counter, the mirror of the case above. mepc must name the instruction
  // that will execute after `mret` -- the one the interrupt was taken before --
  // not the one after it.
  assign trap_epc_irq = rob_head_pc + CORE_XLEN'(4);
`else
  assign trap_epc_irq = rob_head_pc;
`endif

  assign trap_epc   = trap_is_irq ? trap_epc_irq : trap_epc_sync;
  assign trap_cause = head_exc_trap ? exc_cause_head
                      : (vec_trap_take ? vec_trap_cause_q
                      : (sys_trap_q ? sys_trap_cause_q : irq_cause));
  // A memory fault carries its own tval through the ROB's exception record; a
  // synchronous system trap carries one only when the ISA defines it -- the
  // instruction access fault of a denied fetch names the address that could not
  // be read, and every other system trap (ECALL, EBREAK, the illegal returns)
  // has no informative value, which this profile writes as zero rather than
  // guessing an encoding. A vector memory fault names the faulting element's
  // address, exactly as the scalar access it resembles would.
  assign trap_tval  = head_exc_trap ? exc_tval_head
                      : (vec_trap_take ? vec_trap_tval_q
                      : (sys_trap_q ? sys_trap_tval_q : {CORE_XLEN{1'b0}}));

  assign csr_trap_valid = trap_decision;

  // The legal boundary the interrupt unit is allowed to offer a trap at. It is
  // *not* a trap already being taken, not a redirect in flight, not a system
  // macro waiting to be resolved (the CSR access owns the boundary), not a WFI
  // halt, and the head must exist. A synchronous exception at the head is
  // excluded too: the instruction's own fault is taken first, and the interrupt
  // is offered again at the handler's first boundary.
  //
  // I-038 adds one more conjunct: no device transaction may be in flight. A
  // device load is issued from the head, so an interrupt taken at the head while
  // its access is in the memory system would retire *nothing* for that access --
  // the device's side effect would have happened for an instruction that never
  // commits. Blocking the interrupt until the access completes makes the
  // side effect and the commit the same instruction again. The window is the
  // access latency, and the load's own fault path is unaffected: an error
  // response arrives as the head's exception, which `head_exc_trap` takes.
  assign core_can_trap = rob_head_valid && !rob_head_exc && !sys_head &&
                         !vec_valid_q &&
                         !redirect_valid && !recovering && !core_stop && !wfi_halt &&
                         !dev_ser_busy;

  // Retirement is suppressed in the cycle a trap is decided, for the interrupt's
  // sake: a completed instruction at the head would otherwise retire in the same
  // cycle the interrupt was taken, and mepc would name an instruction that had
  // already committed. (`rob_head_ready` already excludes the exceptional case.)
  assign ret_req_gated = ret_req[0] && !trap_decision && !store0_pmp_deny_c &&
                         !st_lane0_blocks_c && !store0_xl_fault_c;

  // The redirect request. A trap acts immediately (the trapping entry does not
  // retire); an MRET or a FENCE.I acts through the ordinary head-retire gate, in
  // the cycle the instruction retires.
  //
  // FENCE.I re-enters the pipeline at the instruction after itself (pc + 4). It
  // is a *system* redirect -- `sys_redirect_kill` -- so the front end, the decode
  // buffer, the clusters, both memory queues and the rename state are all torn
  // down and rebuilt from the committed boundary, exactly as they are for a trap.
  // That teardown is the invalidation: every instruction the fetch unit had
  // already delivered (in the decode buffer, in dispatch, in the pipeline) is
  // discarded, and the fetch that follows is a fresh one with the redirect's
  // epoch. See results/reports/I-037-fence.md.
`ifdef MOSAIC_CORE_MUTANT_FENCEI_SKIP
  // NEGATIVE CONTROL: FENCE.I resumes one instruction too far, so the first
  // instruction after it is skipped. A legitimate instruction is lost -- a wrong
  // retirement stream, not a hang.
  assign sys_redir_pc = trap_decision ? csr_trap_target
                                      : (sys_fence_i_q ? (rob_head_pc + CORE_XLEN'(8))
                                                       : (sys_sfence_vma_q ? (rob_head_pc + CORE_XLEN'(4))
                                                       : (sys_sret_q ? csr_sret_target
                                                                     : csr_mret_target)));
`elsif MOSAIC_CORE_MUTANT_MRET_PC_WRONG
  // NEGATIVE CONTROL: MRET returns to the instruction after mepc. The failing
  // program's interrupt round trip then resumes one instruction late.
  assign sys_redir_pc = trap_decision ? csr_trap_target
                                      : (sys_fence_i_q ? (rob_head_pc + CORE_XLEN'(4))
                                                       : (sys_sfence_vma_q ? (rob_head_pc + CORE_XLEN'(4))
                                                       : (sys_sret_q ? (csr_sret_target + CORE_XLEN'(4))
                                                                     : (csr_mret_target + CORE_XLEN'(4)))));
`else
  assign sys_redir_pc = trap_decision ? csr_trap_target
                                      : (sys_fence_i_q ? (rob_head_pc + CORE_XLEN'(4))
                                                       : (sys_sfence_vma_q ? (rob_head_pc + CORE_XLEN'(4))
                                                       : (sys_sret_q ? csr_sret_target
                                                                     : csr_mret_target)));
`endif
`ifdef MOSAIC_CORE_MUTANT_FENCEI_NO_INVALIDATE
  // NEGATIVE CONTROL: FENCE.I completes like a plain fence and does *not*
  // redirect, so the front end keeps the instruction view it delivered before
  // the publishing store. The stale bytes the program patched execute, which is
  // the card's first failure mode.
  assign sys_redir_req_valid = trap_decision || (sys_head && sys_mret_q) ||
                               (sys_head && sys_sret_q);
`else
  assign sys_redir_req_valid = trap_decision || (sys_head && sys_mret_q) ||
                               (sys_head && sys_sret_q) ||
                               (sys_head && sys_fence_i_q) ||
                               (sys_head && sys_sfence_vma_q);
`endif
  assign sys_redir_act_now   = trap_decision;

  // -------------------------------------------------- the exception payload
  // Recorded when the completion that raised the exception is accepted by the
  // ROB, indexed by the slot it names and tagged with that slot's generation.
  // The system unit's own exception completion takes the port it is offered on,
  // so the payload is selected by the same "who owns port 3" rule the writeback
  // path uses -- there is no second arbitration to disagree with.
  // The LSU is the only producer of an exception completion (the system unit's
  // is empty by construction, above), so the payload is the LSU's -- and it is
  // taken in the cycle the writeback arbiter *accepts* it, not in the cycle the
  // arbiter publishes it. Those are different cycles: the arbiter holds one
  // completion per producer and publishes the lowest pending one, so the bus a
  // later cycle carries a *different* producer's event. Sampling the bus at
  // publish time captured the cause and tval of whatever happened to be offered
  // then, which is exactly the defect this ordering removes.
  logic exc_offer;
  assign exc_offer   = lsu_wb_valid && lsu_wb_ready_int && !port3_taken_sys &&
                       lsu_wb_ev.exc.valid;
  assign exc_cause_win = lsu_wb_ev.exc.cause;
  assign exc_tval_win  = lsu_wb_ev.exc.tval;
  assign exc_capture   = exc_offer;

  always_ff @(posedge clk) begin
    if (rst) begin
      for (int unsigned i = 0; i < CORE_ROB_N; i++) begin
        exc_cause_q[i]   <= {CORE_XLEN{1'b0}};
        exc_tval_q[i]    <= {CORE_XLEN{1'b0}};
        exc_rec_gen_q[i] <= {CORE_RGEN_W{1'b0}};
      end
      sys_valid_q        <= 1'b0;
      sys_index_q        <= {CORE_IDX_W{1'b0}};
      sys_gen_q          <= {CORE_RGEN_W{1'b0}};
      sys_uop_q          <= {CORE_UOP_W{1'b0}};
      sys_csr_addr_q     <= 12'd0;
      sys_csr_op_q       <= mosaic_pkg::CSR_NONE;
      sys_csr_reads_q    <= 1'b0;
      sys_csr_writes_q   <= 1'b0;
      sys_ecall_q        <= 1'b0;
      sys_ebreak_q       <= 1'b0;
      sys_mret_q         <= 1'b0;
      sys_wfi_q          <= 1'b0;
      sys_fence_q        <= 1'b0;
      sys_fence_i_q      <= 1'b0;
      sys_sfence_vma_q      <= 1'b0;
      sys_sfence_has_va_q   <= 1'b0;
      sys_sfence_has_asid_q <= 1'b0;
      sys_src1_q         <= {CORE_XLEN{1'b0}};
      sys_src2_q         <= 16'd0;
      sys_dst_tag_q      <= {CORE_TAG_W{1'b0}};
      sys_dst_gen_q      <= {CORE_IGEN_W{1'b0}};
      sys_dst_x0_q       <= 1'b1;
      sys_redirect_delay_q <= 1'b0;
      sys_wb_pending_q   <= 1'b0;
      trap_irq_prev      <= 1'b0;
      sys_trap_q         <= 1'b0;
      sys_trap_cause_q   <= {CORE_XLEN{1'b0}};
      sys_exec_ctr       <= 32'd0;
      exc_capture_ctr    <= 32'd0;
      exc_gen_mismatch_ctr <= 32'd0;
      trap_irq_ctr       <= 32'd0;
      sys_redirect_ctr   <= 32'd0;
      pmp_store_deny_ctr <= 32'd0;
    end else begin
      // One completion with an exception payload per cycle, and its slot's
      // generation with it.
      if (exc_capture) begin
        exc_cause_q[lsu_wb_ev.id.rob_index]   <= exc_cause_win;
        exc_tval_q[lsu_wb_ev.id.rob_index]    <= exc_tval_win;
        exc_rec_gen_q[lsu_wb_ev.id.rob_index] <= lsu_wb_ev.id.rob_gen;
      end

      // The staging entry: filled with the inserted macro, freed when that macro
      // retires, and cleared outright by a redirect (whose flush discards
      // everything at and above the head -- which includes the staged macro
      // whenever a redirect is issued).
      if (redirect_valid) begin
        sys_valid_q <= 1'b0;
      end else if (sys_valid_q && rob_retire_ack && sys_head) begin
        sys_valid_q <= 1'b0;
      end else if (disp_sys_valid && sys_ins_ready_int && !disp_sys_is_vec) begin
        sys_valid_q      <= 1'b1;
        // The identity fields are the truncated uop id's own layout --
        // {rob_index, rob_gen, uop_index}, most significant first, exactly as
        // mosaic_uop_pkg defines it and as dispatch packs it.
        sys_index_q      <= disp_sys_id[CORE_UOP_W + CORE_RGEN_W +: CORE_IDX_W];
        sys_gen_q        <= disp_sys_id[CORE_UOP_W +: CORE_RGEN_W];
        sys_uop_q        <= disp_sys_id[CORE_UOP_W-1:0];
        sys_csr_addr_q   <= disp_sys_csr_addr;
        sys_csr_op_q     <= mosaic_pkg::csr_op_e'(disp_sys_csr_op);
        sys_csr_reads_q  <= disp_sys_csr_reads;
        sys_csr_writes_q <= disp_sys_csr_writes;
        sys_ecall_q      <= disp_sys_is_ecall;
        sys_ebreak_q     <= disp_sys_is_ebreak;
        sys_mret_q       <= disp_sys_is_mret;
        sys_sret_q       <= disp_sys_is_sret;
        sys_wfi_q        <= disp_sys_is_wfi;
        sys_fetch_fault_q <= disp_sys_is_fetch_fault;
        sys_fence_q      <= disp_sys_is_fence;
        sys_fence_i_q    <= disp_sys_is_fence_i;
        sys_sfence_vma_q      <= disp_sys_is_sfence_vma;
        sys_sfence_has_va_q   <= disp_sys_sfence_has_va;
        sys_sfence_has_asid_q <= disp_sys_sfence_has_asid;
        sys_src1_q       <= disp_sys_src1_val;
        sys_src2_q       <= disp_sys_src2_val[15:0];
        sys_dst_tag_q    <= disp_sys_dst_tag;
        sys_dst_gen_q    <= disp_sys_dst_gen;
        sys_dst_x0_q     <= disp_sys_dst_x0;
      end

      sys_redirect_delay_q <= arb_sys_redirect;
      trap_irq_prev        <= trap_is_irq;
      // The completion in flight: offered once, released when the macro leaves.
      if (redirect_valid || (sys_head && rob_retire_ack)) begin
        sys_wb_pending_q <= 1'b0;
      end else if (sys_wb_valid) begin
        sys_wb_pending_q <= 1'b1;
      end
      // The resolved trap is held until the redirect that takes it, and cleared
      // by that redirect like everything else at the head.
      if (redirect_valid) begin
        sys_trap_q <= 1'b0;
      end else if (sys_wb_valid && sys_trap_now) begin
        sys_trap_q       <= 1'b1;
        sys_trap_cause_q <= sys_exc_cause;
        // The staged macro is the ROB head here, so its PC is the faulting
        // address the trap value must carry.
        sys_trap_tval_q  <= sys_fetch_fault_q ? rob_head_pc : {CORE_XLEN{1'b0}};
      end else if (store_pmp_trap_now) begin
        // D5: the ROB was about to authorise a store whose PMP check refuses
        // it. The authorisation is suppressed this cycle (`ret_req_gated`) and
        // the refusal is latched here so the trap controller takes it next
        // cycle, at the store's own PC, with the access address as tval --
        // exactly the route an excepting system macro takes. The store never
        // retires and the redirect that follows the trap flushes it from the
        // queue before it can reach memory.
        sys_trap_q       <= 1'b1;
        sys_trap_cause_q <= store0_xl_fault_c ? {60'd0, st_cause0_c}
                                              : mosaic_pkg::EXC_STORE_ACCESS;
        sys_trap_tval_q  <= store_pmp_trap_addr_c;
      end

      if (sys_wb_valid)     sys_exec_ctr <= sys_exec_ctr + 32'd1;
      if (exc_capture)      exc_capture_ctr <= exc_capture_ctr + 32'd1;
      if (store0_pmp_deny_c) pmp_store_deny_ctr <= pmp_store_deny_ctr + 32'd1;
      if (head_exc_trap && (exc_gen_match != rob_head_gen))
        exc_gen_mismatch_ctr <= exc_gen_mismatch_ctr + 32'd1;
      if (trap_is_irq && !trap_irq_prev) trap_irq_ctr <= trap_irq_ctr + 32'd1;
      if (arb_sys_redirect) sys_redirect_ctr <= sys_redirect_ctr + 32'd1;
    end
  end

  assign o_sys_exec_ctr        = sys_exec_ctr;
  assign o_exc_capture_ctr     = exc_capture_ctr;
  assign o_exc_gen_mismatch_ctr = exc_gen_mismatch_ctr;
  assign o_trap_irq_ctr        = trap_irq_ctr;
  assign o_sys_redirect_ctr    = sys_redirect_ctr;
  assign o_trap_valid          = trap_decision;
  assign o_trap_is_irq         = trap_is_irq;
  assign o_trap_cause          = trap_cause;
  assign o_trap_tval           = trap_tval;
  assign o_trap_epc            = trap_epc;
  assign o_trap_target         = csr_trap_target;
  assign o_mret_target         = csr_mret_target;
  assign o_priv                = csr_priv;
  assign o_medeleg             = csr_medeleg;
  assign o_mideleg             = csr_mideleg;
  assign o_sret_valid          = csr_sret_commit;
  assign o_sret_target         = csr_sret_target;
  assign o_sstatus             = csr_sstatus;
  assign o_stvec               = csr_stvec;
  assign o_sepc                = csr_sepc;
  assign o_scause              = csr_scause;
  assign o_stval               = csr_stval;
  assign o_sscratch            = csr_sscratch;
  assign o_satp                = csr_satp;
  assign o_csr_sret_ctr        = csr_sret_ctr;
  assign o_csr_trap_s_ctr      = csr_trap_s_ctr;
  assign o_csr_priv_illegal_ctr = csr_priv_illegal_ctr;
  assign o_csr_priv_change_ctr = csr_priv_change_ctr;
  assign o_pmp_query_ctr       = pmp_query_ctr;
  assign o_pmp_deny_ctr        = pmp_deny_ctr;
  assign o_pmp_fetch_deny_ctr  = pmp_fetch_deny_ctr;
  assign o_pmp_locked_ctr      = pmp_locked_ctr;
  assign o_pmp_store_deny_ctr  = pmp_store_deny_ctr;
  assign o_pmp_data_matched    = pmp_data_matched;
  assign o_pmp_data_locked     = pmp_data_locked;
  assign o_pmp_fetch_matched   = pmp_fetch_matched;
  assign o_pmp_fetch_locked    = pmp_fetch_locked;
  assign o_csr_pmp_sel         = csr_pmp_sel;
  assign o_irq_valid           = irq_valid;
  assign o_irq_cause           = irq_cause;
  assign o_irq_ctr             = irq_ctr;
  assign o_wfi_halt            = wfi_halt;
  assign o_spurious_wake_ctr   = spurious_wake_ctr;
  assign o_halt_cycles         = halt_cycles;

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
      // The trap/MRET request. It is at the ROB head by construction and always
      // redirects; `act_now` is the trap's licence to act on a head that is not
      // retiring (an exception is architecturally final and never retires).
      .sys_req_valid   (sys_redir_req_valid),
      .sys_req_pc      (sys_redir_pc),
      .sys_req_rob_index(rob_head_index),
      .sys_req_rob_gen (rob_head_gen),
      .sys_req_act_now (sys_redir_act_now),
      .o_sys_act       (),
      .o_sys_redirect  (arb_sys_redirect),
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
  // The kill a system redirect performs. The negative control removes it at one
  // point, so "the trap killed the younger work" is one named decision and not a
  // property that four separate wires happen to agree about.
`ifdef MOSAIC_CORE_MUTANT_TRAP_NO_FLUSH
  logic sys_redirect_kill;
  assign sys_redirect_kill = 1'b0;
`else
  logic sys_redirect_kill;
  assign sys_redirect_kill = arb_sys_redirect;
`endif

  // The trap's own flush is *not* the registered redirect: a trap must drop the
  // entry at the head in the cycle it is taken. An exceptional entry is
  // architecturally final and must not sit in the buffer for a cycle (the
  // retire event stream would publish it twice), and an interrupted entry must
  // not be able to retire in the cycle after the interrupt was taken. The
  // *redirect* still comes from the arbiter one cycle later, exactly as a
  // branch's does, and drives the front end, the clusters and the queues.
  assign rob_flush_pulse      = redirect_valid | trap_decision;
  assign cluster_flush_pulse  = redirect_valid;

  // The front end's own purge. It is the same redirect, with one distinction
  // given to it by the negative control below: the fetch redirect and the
  // buffer purge are what stop the *front end* from continuing down the
  // interrupted path, while the ROB flush is what stops the young work already
  // in the buffer. A trap that does one and not the other is still a defect, so
  // the two are named separately.
`ifdef MOSAIC_CORE_MUTANT_TRAP_NO_FETCH_REDIRECT
  assign fetch_redir_valid = redirect_valid && !arb_sys_redirect;
`else
  assign fetch_redir_valid = redirect_valid;
`endif
  assign dbuf_purge = fetch_redir_valid;

  always_ff @(posedge clk) begin
    if (rst) begin
      recovering <= 1'b0;
    end else if (redirect_valid || trap_decision) begin
      // A trap holds the front end from the cycle after it is taken: the ROB
      // flush at the trap cycle has already emptied the buffer, and nothing may
      // allocate into it before the redirect has moved the fetch PC.
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
  // A *trap* or MRET redirect is not a branch redirect and must not take a
  // branch checkpoint: its boundary is not a committed one (the trapping
  // instruction never committed, and younger work is in the machine), so the
  // checkpoint would be refused and the speculative map left corrupt. It uses
  // the CSR/trap path's full restore instead -- spec := cmt and free := ~cmt --
  // in the same cycle as the flush, which is exactly "everything at and above
  // the head is gone".
  assign ren_ckpt_valid  = redirect_valid && !arb_sys_redirect;
  assign ren_squash      = redirect_delay_q && !sys_redirect_delay_q;
  assign ren_flush_restore = sys_redirect_kill | trap_decision;
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

  // ==========================================================================
  // 13a. The retire event's payload bus
  // ==========================================================================
  // The frozen architectural event (config/contracts/event_v1.json) is one
  // record per instruction, and two of its groups are owned by state the retire
  // unit does not hold: a store's address, data and size (the store queue holds
  // them until it authorises the store, which is exactly the cycle the
  // instruction retires) and a CSR write's address and value (the CSR file
  // holds them, and applies the write at the completion edge -- one cycle
  // before the retirement that makes it architectural).
  //
  // Both are built here as *per-lane presentations*, and every field of a
  // lane's group is zero unless that lane is the instruction that owns it. That
  // is what makes the schema's validity rules hold by construction: a load
  // retirement carries no store payload, an ordinary retirement carries no CSR
  // payload, and a trap carries neither -- the retire unit gates every payload
  // field on the lane having retired, and a trap is not a retirement
  // (mosaic_retire.sv).
  //
  // Neither source is a second copy of a rule. The address is `base + imm`, the
  // sum the memory endpoint's single adder forms, read from the same entry the
  // store queue authorises, so the event cannot describe a different store from
  // the one that reached memory. The CSR value is the CSR file's own read-back
  // in the retirement cycle, so it is the value the register actually holds --
  // WARL canonicalisation included -- and not a re-derivation of the write
  // operation here.
  logic                             sq_commit_ok;
  logic [CORE_MEM_CNT_W-1:0]        sq_pay0_slot, sq_pay1_slot;
  logic                             sq_pay0_valid, sq_pay1_valid;
  logic                             sq_pay0_ready, sq_pay1_ready;
  logic [CORE_SIZE_W-1:0]           sq_pay0_size, sq_pay1_size;
  // The size the payload presents. NEGATIVE CONTROL: every store is reported as
  // a word whatever the instruction asked for -- the valid bit, the address and
  // the data are all right, so only a check that reads `mem_size` field by
  // field can see it.
  logic [CORE_SIZE_W-1:0]           sq_pay0_size_ev, sq_pay1_size_ev;
  logic [CORE_RET_N-1:0]            pay_store_we_vec;
  logic [CORE_RET_N*CORE_XLEN-1:0]  pay_store_addr_vec, pay_store_data_vec;
  logic [CORE_RET_N*CORE_SIZE_W-1:0] pay_store_size_vec;

  // The authorisation names the first unauthorised entry, and the queue is
  // compacted, so that entry sits at the authorisation watermark; the second
  // lane's is the one after it -- the same rule the queue's own commit path
  // applies, expressed in the count domain so it cannot wrap onto entry 0.
  assign sq_pay0_slot = sq_auth_cnt;
  assign sq_pay1_slot = sq_auth_cnt + CORE_MEM_CNT_W'(sq_commit_ok);

`ifdef MOSAIC_CORE_MUTANT_NO_STORE_PAYLOAD
  // NEGATIVE CONTROL: the store payload is tied off again -- the wiring this
  // package shipped with. A retiring store still retires and still reaches
  // memory; what is lost is the event's ability to say *which* store it was,
  // which is the record the frozen schema declares and CASE=core.event_payload
  // checks field by field.
  assign sq_pay0_valid = 1'b0;
  assign sq_pay1_valid = 1'b0;
`else
  // The entry must actually hold the store's address and value: an entry the
  // queue has not filled is not a store that can describe itself, and the two
  // readiness bits are the queue's own statement of that.
  assign sq_pay0_valid = sq_commit_valid && sq_commit_ok && sq_pay0_ready;
  assign sq_pay1_valid = sq_commit2_valid && sq_commit2_ok && sq_pay1_ready;
`endif

  always_comb begin
    sq_pay0_ready = 1'b0;
    sq_pay1_ready = 1'b0;
    sq_pay0_addr = {CORE_XLEN{1'b0}};
    sq_pay0_data = {CORE_XLEN{1'b0}};
    sq_pay0_size = {CORE_SIZE_W{1'b0}};
    sq_pay1_addr = {CORE_XLEN{1'b0}};
    sq_pay1_data = {CORE_XLEN{1'b0}};
    sq_pay1_size = {CORE_SIZE_W{1'b0}};
    for (int unsigned i = 0; i < CORE_SQ_N; i++) begin
      if (CORE_MEM_CNT_W'(i) == sq_pay0_slot) begin
        sq_pay0_ready = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_ADDR_VALID]
                     && sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_DATA_VALID];
        sq_pay0_addr = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_BASE +: CORE_XLEN]
                     + sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_IMM  +: CORE_XLEN];
        sq_pay0_data = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_DATA +: CORE_XLEN];
        sq_pay0_size = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_SIZE +: CORE_SIZE_W];
      end
      if (CORE_MEM_CNT_W'(i) == sq_pay1_slot) begin
        sq_pay1_ready = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_ADDR_VALID]
                     && sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_DATA_VALID];
        sq_pay1_addr = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_BASE +: CORE_XLEN]
                     + sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_IMM  +: CORE_XLEN];
        sq_pay1_data = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_DATA +: CORE_XLEN];
        sq_pay1_size = sq_entry_pay[i*CORE_SQ_ENTRY_W + CORE_SQ_OFF_SIZE +: CORE_SIZE_W];
      end
    end
  end

  always_comb begin
    pay_store_we_vec   = '0;
    pay_store_addr_vec = '0;
    pay_store_data_vec = '0;
    pay_store_size_vec = '0;
    // Each lane presents its payload only when that lane really is a store the
    // queue is authorising, so a non-store lane carries zero in every field of
    // the group rather than a neighbour's bytes with the valid bit clear: the
    // schema's validity rule ("zero for a load retirement, for a non-memory
    // instruction") is then a property of the bus, not a rule a consumer has to
    // remember to apply.
    if (sq_pay0_valid) begin
      pay_store_we_vec[0] = 1'b1;
      pay_store_addr_vec[0*CORE_XLEN +: CORE_XLEN]  = sq_pay0_addr;
      pay_store_data_vec[0*CORE_XLEN +: CORE_XLEN]  = sq_pay0_data;
      pay_store_size_vec[0*CORE_SIZE_W +: CORE_SIZE_W] = sq_pay0_size_ev;
    end
    if (CORE_RET_N > 1 && sq_pay1_valid) begin
      pay_store_we_vec[1] = 1'b1;
      pay_store_addr_vec[1*CORE_XLEN +: CORE_XLEN]  = sq_pay1_addr;
      pay_store_data_vec[1*CORE_XLEN +: CORE_XLEN]  = sq_pay1_data;
      pay_store_size_vec[1*CORE_SIZE_W +: CORE_SIZE_W] = sq_pay1_size_ev;
    end
  end

`ifdef MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG
  assign sq_pay0_size_ev = CORE_SIZE_W'(mosaic_pkg::SZ_WORD);
  assign sq_pay1_size_ev = CORE_SIZE_W'(mosaic_pkg::SZ_WORD);
`else
  assign sq_pay0_size_ev = sq_pay0_size;
  assign sq_pay1_size_ev = sq_pay1_size;
`endif

  // ------------------------------------------------------- the CSR payload
  // A CSR macro is staged at the head and its staging entry is freed at the end
  // of the very cycle it retires, so in that cycle the entry still names it and
  // the CSR file still answers for its address. The value presented is the
  // file's read-back one cycle after the write strobe: the write landed at the
  // completion edge, so the register already holds what the instruction wrote.
  logic                            retire_csr_write;
  logic [CORE_RET_N-1:0]           pay_csr_we_vec;
  logic [CORE_RET_N*CORE_CSR_W-1:0] pay_csr_addr_vec;
  logic [CORE_RET_N*CORE_XLEN-1:0]  pay_csr_value_vec;

  assign retire_csr_write = sys_valid_q && sys_head && rob_retire_ack &&
                            sys_csr_writes_q;

`ifdef MOSAIC_CORE_MUTANT_NO_CSR_PAYLOAD
  // NEGATIVE CONTROL: the CSR payload is tied off again. The CSR write still
  // happens and still shows in the architectural state; the event stream just
  // stops naming the address and the value it wrote.
  assign pay_csr_we_vec = {CORE_RET_N{1'b0}};
`else
  always_comb begin
    pay_csr_we_vec    = '0;
    pay_csr_addr_vec  = '0;
    pay_csr_value_vec = '0;
    if (retire_csr_write) begin
      pay_csr_we_vec[0] = 1'b1;
      pay_csr_addr_vec[CORE_CSR_W-1:0] = sys_csr_addr_q;
      pay_csr_value_vec[CORE_XLEN-1:0] = csr_rdata;
    end
  end
`endif

  // --------------------------------------------------- the system-trap event
  // The trap the *system unit* resolves at the head never appears on the
  // buffer's exception bit: the macro completes normally, the core latches the
  // trap, and the redirect arbiter takes it. It is a trap of a real instruction
  // all the same, so the event stream is told about it, in the cycle it is
  // taken, with the cause and tval the trap entry publishes. The PC the record
  // carries is the head's own -- which is the trap's epc for a synchronous
  // trap, exactly as the schema's trap_epc rule states.
  logic                            retire_sys_trap;
  assign retire_sys_trap = sys_trap_take && !head_exc_trap && !irq_valid;


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

  // F/D (I-050): the destination namespace of each commit. It is recorded per
  // ROB slot at allocation, because the retiring instruction's own word cannot
  // say whether the destination is an f-register (fcvt.w.s writes an integer
  // register, fcvt.s.w writes an f-register, and both are OP-FP).
  assign ren_commit_is_fp  = ret_commit_valid[0] && fp_dst_mem[rob_head_index];
  assign ren_commit2_is_fp = ret_commit_valid[1] && fp_dst_mem[rob_head1_index];

  // The precise-fflags merge and the FS dirty predicate, both evaluated at the
  // ROB head in program order. A slot's flags are merged only when that slot
  // retires with the generation its record was written for; a discarded
  // instruction's record was cleared by the flush and its slot never becomes the
  // head, so its flags never reach fcsr.
  assign fp_merge0_v  = rob_retire_ack && fp_flag_v_mem[rob_head_index] &&
                        (fp_flag_gen_mem[rob_head_index] == rob_head_gen);
  assign fp_merge0    = fp_merge0_v ? fp_flag_mem[rob_head_index] : 5'd0;
  assign fp_merge1_v  = rob_retire_ack_next && rob_retire_ack &&
                        fp_flag_v_mem[rob_head1_index] &&
                        (fp_flag_gen_mem[rob_head1_index] == rob_head1_gen);
  assign fp_merge1    = fp_merge1_v ? fp_flag_mem[rob_head1_index] : 5'd0;
`ifdef MOSAIC_CORE_MUTANT_FFLAGS_EARLY
  // MUTANT (control for CASE=fp.precise_flags_and_boxing): the operation's flags
  // are written into the architectural fcsr when the FP unit signals done,
  // instead of when the operation retires. This is the card's fail mode: an
  // operation that is executed and then squashed has already leaked its flags,
  // so a trap or a redirect no longer makes `fflags` precise.
  assign fp_fflags_or = (fp_wb_valid && fp_wb_ready) ? fp_wb_fflags : 5'd0;
`else
  assign fp_fflags_or = fp_merge0 | fp_merge1;
`endif
  assign fp_fs_dirty  = (rob_retire_ack && fp_state_wr_mem[rob_head_index]) ||
                        (rob_retire_ack_next && rob_retire_ack &&
                         fp_state_wr_mem[rob_head1_index]);

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
  // FENCE.I is the second such barrier, and for a stronger reason: the
  // instruction immediately after it may well be the *stale* one it exists to
  // invalidate. The arbiter acts on the fence's retirement edge and the redirect
  // the machine applies one cycle later; if lane 1 retired in that same cycle the
  // younger instruction would commit before the flush that discards it, which is
  // exactly "a stale byte executed". So while a FENCE.I is the head -- waiting to
  // drain, or retiring -- lane 1 does not leave the ROB. (I-037.)
  assign head_fence_i_pending = sys_head && sys_fence_i_q;
  assign rob_retire_req_next = ret_req[1] && !head_pending_taken && !head_fence_i_pending &&
                               !store1_pmp_deny_c && !st_lane1_blocks_c && !st_fault1_c;

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
      .rob_len        ({desc_len1, desc_len0}),
      .rob_insn       ({desc_insn1, desc_insn0}),
      .pay_valid      ({rob_head1_valid, rob_head_valid}),
      .pay_reg_we     ({desc_reg_we1, desc_reg_we0}),
      .pay_rd         ({desc_rd1, desc_rd0}),
      .pay_value      (retire_pay_value),
      .pay_csr_we     (pay_csr_we_vec),
      .pay_csr_addr   (pay_csr_addr_vec),
      .pay_csr_value  (pay_csr_value_vec),
      .pay_is_store   (pay_store_we_vec),
      .pay_store_addr (pay_store_addr_vec),
      .pay_store_data (pay_store_data_vec),
      .pay_store_size (pay_store_size_vec),
      .pay_exc_cause  (pay_exc_cause_vec),
      .pay_exc_tval   (pay_exc_tval_vec),
      .sys_trap_valid (retire_sys_trap),
      .sys_trap_cause (trap_cause),
      .sys_trap_tval  (trap_tval),
      .flush_valid    (rob_flush_pulse),
      .retire_req     (ret_req),
      .trap_flush     (),
      .ev_valid       (ev_valid),
      .ev_trap        (ev_trap),
      .ev_seq         (ev_seq),
      .ev_pc          (ev_pc),
      .ev_len         (ev_len),
      .ev_insn        (ev_insn),
      .ev_id          (ev_id),
      .ev_reg_we      (ev_reg_we),
      .ev_rd          (ev_rd),
      .ev_value       (ev_value),
      .ev_csr_we      (ev_csr_we),
      .ev_csr_addr    (ev_csr_addr),
      .ev_csr_value   (ev_csr_value),
      .ev_store       (ev_store),
      .ev_store_addr  (ev_store_addr),
      .ev_store_data  (ev_store_data),
      .ev_store_size  (ev_store_size),
      .ev_trap_cause  (ev_trap_cause),
      .ev_trap_tval   (ev_trap_tval),
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
  //
  // The two queues' offers are muxed here -- the store drain has priority, as
  // above -- and the mux feeds the device serializer below, which is the last
  // point before the endpoint.
  // ===========================================================================
  // 14-pre. Sv39 address translation (I-045)
  // ===========================================================================
  // The translation engine (`mosaic_ptw`) sits here, between the queues and the
  // endpoint, and it is owned in two places because the ISA gives a load and a
  // store different points at which their translation must be resolved:
  //
  //   * a **load** (and an LR/AMO/SC, which the load queue issues) is translated
  //     in the stage below, as it is offered. A translation fault is handed back
  //     to the load queue as an ordinary fault response and the access never
  //     reaches the endpoint -- "a PTE error is not an ordinary cache miss" made
  //     structural.
  //   * a **store** is translated at the commit boundary. A store may reach
  //     memory only after it retires, so a page fault discovered at its drain
  //     would be reported after the instruction had already committed; instead
  //     the store's translation is resolved *before* its retirement, its
  //     physical address is recorded, and a fault suppresses the retirement and
  //     takes the trap at the store's own PC, exactly as the PMP commit check
  //     does. The recorded address is what the drain then writes to.
  //
  // Both share the one serial walker and the one physical `dmem` port. The
  // walker is a plain master of that port: an arbiter gives it the port ahead of
  // the endpoint, and only one of the two can have a transaction outstanding at
  // a time (the endpoint is in a translation stage while the walker reads PTEs,
  // and the walker is idle for the endpoint's data beat).
  //
  // The `satp`-visibility rule (the brief's question, answered without
  // SFENCE.VMA, which is I-046): a translation is performed once, at the access's
  // own point in program order, and the resulting physical address is what the
  // access uses. A later `satp` write therefore cannot retroactively change an
  // access that already executed -- the store address FIFO below is the record
  // that makes this true even for a store whose drain follows the write. A walk
  // in flight when a redirect arrives is cancelled (`lq_flush`), so a stale walk
  // cannot produce a translation for an instruction that was squashed.

  assign xlate_active_c = (eff_priv_c != mosaic_csr_pkg::MOSAIC_PRIV_M) &&
                          (csr_satp[63:60] != 4'd0);

  // -------------------------------------------------- the translation cache
  // I-046 replaces the bare walker with the TLB that wraps it. The request and
  // response interface is the walker's own, plus the ASID for the tag; the PTE
  // port is the walker's unchanged. The fence inputs come from the staged
  // SFENCE.VMA macro, the satp pulse from the committed CSR write, and the
  // counters let a case see a hit without inferring it from a cycle count.
  mosaic_tlb u_tlb (
      .clk             (clk),
      .rst             (rst),
      .xl_req_valid_i  (ptw_xl_req_valid),
      .xl_req_ready_o  (ptw_xl_req_ready),
      .xl_va_i         (ptw_xl_va),
      .xl_kind_i       (ptw_xl_kind),
      .xl_priv_i       (ptw_xl_priv),
      .xl_satp_mode_i  (ptw_xl_mode),
      .xl_satp_ppn_i   (ptw_xl_ppn),
      .xl_satp_asid_i  (csr_satp[59:44]),
      .xl_sum_i        (ptw_xl_sum),
      .xl_mxr_i        (ptw_xl_mxr),
      .xl_cancel_i     (ptw_xl_cancel),
      .xl_rsp_valid_o  (ptw_xl_rsp_valid),
      .xl_rsp_ready_i  (ptw_xl_rsp_ready),
      .xl_pa_o         (ptw_xl_pa),
      .xl_fault_o      (ptw_xl_fault),
      .xl_cause_o      (ptw_xl_cause),
      .xl_tval_o       (),
      .xl_perms_o      (),
      .xl_attr_o       (),
      .xl_bare_o       (),
      .sfence_valid_i  (tlb_sfence_valid),
      .sfence_va_i     (tlb_sfence_va),
      .sfence_has_va_i (sys_sfence_has_va_q),
      .sfence_asid_i   (tlb_sfence_asid),
      .sfence_has_asid_i(sys_sfence_has_asid_q),
      .satp_write_i    (tlb_satp_write),
      .pte_req_valid_o (ptw_mem_req_valid),
      .pte_req_ready_i (ptw_mem_req_ready),
      .pte_req_we_o    (ptw_mem_we),
      .pte_req_addr_o  (ptw_mem_addr),
      .pte_req_wdata_o (ptw_mem_wdata),
      .pte_req_wstrb_o (ptw_mem_wstrb),
      .pte_rsp_valid_i (ptw_mem_rsp_valid),
      .pte_rsp_ready_o (ptw_mem_rsp_ready),
      .pte_rsp_rdata_i (ptw_mem_rdata),
      .pte_rsp_fault_i (ptw_mem_fault),
      .o_busy          (),
      .o_hit           (),
      .o_hit_ctr       (o_tlb_hit_ctr),
      .o_miss_ctr      (o_tlb_miss_ctr),
      .o_perm_fault_ctr(),
      .o_install_ctr   (o_tlb_install_ctr),
      .o_evict_ctr     (),
      .o_stale_ctr     (o_tlb_stale_ctr),
      .o_sfence_ctr    (o_tlb_sfence_ctr),
      .o_satp_flush_ctr(o_tlb_satp_flush_ctr),
      .o_cancel_ctr    (),
      .o_gen           (o_tlb_gen),
      .o_walk_ctr      (o_tlb_walk_ctr),
      .o_leaf_ctr      (),
      .o_fault_ctr     (),
      .o_ad_ctr        (),
      .o_walk_cancel_ctr()
  );

  // --------------------------------------------------------- the PTE/dmem merge
  // One transaction outstanding on `dmem` at a time, owned by a register. The
  // walker has priority: its beat is short and it is on the critical path of
  // whatever the endpoint is waiting to translate, while the endpoint's data
  // beat has no translation left to do.
  // The walker's response is always taken (its `pte_rsp_ready_o` is tied high):
  // it owns the single outstanding beat and has nowhere else to put it.
  assign ptw_mem_rdata     = dmem_rsp.rdata;
  assign ptw_mem_fault     = dmem_rsp.fault;
  assign ptw_mem_rsp_valid = dmem_rsp_valid && (mem_owner_q == MEM_OWN_PTW);
  // The endpoint's slot on the arbiter is now the L1 data cache path's memory
  // side (`dc_mem_*`); the endpoint itself talks to that path's CPU side.
  assign dc_mem_rsp_valid  = dmem_rsp_valid && (mem_owner_q == MEM_OWN_EP);
  assign dc_mem_rsp        = dmem_rsp;
  // The vector engine's slot on the same arbiter (I-059).
  assign vec_mem_rsp_valid = dmem_rsp_valid && (mem_owner_q == MEM_OWN_VEC);
  assign vec_mem_rsp_rdata = dmem_rsp.rdata;
  assign vec_mem_rsp_fault_raw = dmem_rsp.fault;

  assign ptw_mem_req_ready = dmem_req_ready && (mem_owner_q == MEM_OWN_NONE);
  assign vec_mem_req_ready = dmem_req_ready && (mem_owner_q == MEM_OWN_NONE) &&
                             !ptw_mem_req_valid;
  assign dc_mem_req_ready  = dmem_req_ready && (mem_owner_q == MEM_OWN_NONE) &&
                             !ptw_mem_req_valid && !vec_mem_req_valid;
  assign dmem_req_valid    = (mem_owner_q == MEM_OWN_NONE) &&
                             (ptw_mem_req_valid || vec_mem_req_valid ||
                              dc_mem_req_valid);
  always_comb begin
    if (ptw_mem_req_valid) begin
      // A PTE access is always an 8-byte access to the PTE's physical address.
      dmem_req.we     = ptw_mem_we;
      dmem_req.addr   = ptw_mem_addr;
      dmem_req.size   = mosaic_pkg::SZ_DBL;
      dmem_req.wstrb  = ptw_mem_wstrb;
      dmem_req.wdata  = ptw_mem_wdata;
      dmem_req.amo    = 1'b0;
      dmem_req.amo_op = mosaic_pkg::AMO_ADD;
      dmem_req.aq     = 1'b0;
      dmem_req.rl     = 1'b0;
    end else if (vec_mem_req_valid) begin
      // The packetizer's one request per element: the 8-byte-aligned beat, the
      // byte enables relative to it and the lane-positioned data. Never an
      // atomic, never ordered by aq/rl -- the packetizer orders its own stream.
      dmem_req.we     = vec_mem_req_we;
      dmem_req.addr   = vec_mem_req_addr;
      dmem_req.size   = vec_mem_req_size[2:0];
      dmem_req.wstrb  = vec_mem_req_wmask;
      dmem_req.wdata  = vec_mem_req_wdata;
      dmem_req.amo    = 1'b0;
      dmem_req.amo_op = mosaic_pkg::AMO_ADD;
      dmem_req.aq     = 1'b0;
      dmem_req.rl     = 1'b0;
    end else begin
      dmem_req = dc_mem_req;
    end
  end
  assign dmem_rsp_ready = (mem_owner_q == MEM_OWN_PTW) ? ptw_mem_rsp_ready
                        : (mem_owner_q == MEM_OWN_EP)  ? dc_mem_rsp_ready
                        : (mem_owner_q == MEM_OWN_VEC) ? 1'b1
                        : 1'b1;

  always_ff @(posedge clk) begin
    if (rst) begin
      mem_owner_q <= MEM_OWN_NONE;
    end else begin
      if (dmem_req_valid && dmem_req_ready) begin
        mem_owner_q <= ptw_mem_req_valid ? MEM_OWN_PTW
                    : (vec_mem_req_valid ? MEM_OWN_VEC : MEM_OWN_EP);
      end
      if (dmem_rsp_valid && dmem_rsp_ready) begin
        mem_owner_q <= MEM_OWN_NONE;
      end
    end
  end

  // ===========================================================================
  // The L1 cache path (I-042) -- data side
  // ===========================================================================
  // The data cache sits between the LSU endpoint's memory port and the
  // endpoint's slot on the PTE/data arbiter. A cacheable access is served by the
  // cache (a miss refills a line, a store allocates and dirties a line, a dirty
  // eviction writes it back); a non-cacheable access -- MMIO, the boot ROM, any
  // atomic -- bypasses with its own size and strobes. The PTE walker is *not*
  // behind the cache: page tables are read directly, so a translation is never
  // served from a stale line (see the report's "not covered").
  mosaic_l1_cache_path #(
      .IS_FETCH      (1'b0),
      .LINE_BYTES    (32),
      .SETS          (8),
      .ADDR_WIDTH    (64),
      .CPU_DATA_WIDTH(64),
      .ID_W          (1),
      .EPOCH_W       (1)
  ) u_dcache_path (
      .clk             (clk),
      .rst             (rst),
      .en_i            (cache_en_i),
      .flush_i         (dcache_flush),
      .flush_done      (dcache_flush_done),
      .cpu_req_valid_i (dc_cpu_req_valid),
      .cpu_req_ready_o (dc_cpu_req_ready),
      .cpu_req_i       (dc_cpu_req),
      .cpu_req_id_i    (1'b0),
      .cpu_req_epoch_i (1'b0),
      .cpu_rsp_valid_o (dc_cpu_rsp_valid),
      .cpu_rsp_ready_i (dc_cpu_rsp_ready),
      .cpu_rsp_o       (dc_cpu_rsp),
      .cpu_rsp_id_o    (),
      .cpu_rsp_epoch_o (),
      .cpu_rsp_len_o   (),
      .mem_req_valid_o (dc_mem_req_valid),
      .mem_req_ready_i (dc_mem_req_ready),
      .mem_req_o       (dc_mem_req),
      .mem_req_id_o    (),
      .mem_req_epoch_o (),
      .mem_rsp_valid_i (dc_mem_rsp_valid),
      .mem_rsp_ready_o (dc_mem_rsp_ready),
      .mem_rsp_i       (dc_mem_rsp),
      .mem_rsp_id_i    (1'b0),
      .mem_rsp_epoch_i (1'b0),
      .mem_rsp_len_i   (3'b0),
      .o_hit           (),
      .o_miss          (),
      .o_refill        (),
      .o_writeback     (),
      .o_fault         (),
      .o_cpu_txn       (),
      .o_mem_beat      (),
      .o_line_txn      (),
      .o_bypass_txn    (),
      .dbg_index_i     (3'b0),
      .dbg_valid_o     (),
      .dbg_dirty_o     ()
  );

  // The endpoint talks to the cache path's CPU side; the cache path's memory side
  // takes the endpoint's slot on the arbiter.
  assign dc_cpu_req_valid = ep_mem_req_valid;
  assign dc_cpu_req       = ep_mem_req;
  assign ep_mem_req_ready = dc_cpu_req_ready;
  assign ep_mem_rsp_valid = dc_cpu_rsp_valid;
  assign ep_mem_rsp       = dc_cpu_rsp;
  assign dc_cpu_rsp_ready = ep_mem_rsp_ready;

  // ===========================================================================
  // The load translation stage
  // ===========================================================================
  // In Bare it is transparent (`lq_bypass_c`), so a machine with no paging sees
  // exactly the wire it had. With paging it latches the offered load, walks, and
  // then either presents the physical request or returns the fault.
  assign lq_bypass_c = !xlate_active_c && (lq_stg_state_q == LS_EMPTY);

  assign lq_tx_valid = lq_bypass_c ? lq_req_valid
                     : (lq_stg_state_q == LS_PRESENT);
  assign lq_tx_ready = ep_req_ready && !sq_drain_valid;

  always_comb begin
    if (lq_bypass_c) begin
      lq_tx_req  = lq_req;
      lq_tx_tval = lq_req.base + lq_req.imm;
    end else begin
      lq_tx_req           = lq_hold_q;
      lq_tx_req.base      = lq_hold_pa_q;
      lq_tx_req.imm       = {CORE_XLEN{1'b0}};
      lq_tx_tval          = lq_hold_tval_q;
    end
  end

  // The walker request the stage drives. The class is the access's own: an SC
  // and an AMO are store-class (their fault is a store page fault), an LR is a
  // load.
  logic lq_is_store_c;
  assign lq_is_store_c = lq_hold_q.we | lq_hold_q.is_amo | lq_hold_q.is_sc;
  assign lq_xl_req_valid = (lq_stg_state_q == LS_XL) && !lq_xl_wait_q &&
                           !st_req_valid;

  // The load queue's ready is the stage's, not the endpoint's: the queue is told
  // "taken" when the stage accepts it, and the stage owns it until its response.
  assign lq_req_ready = lq_bypass_c ? lq_tx_ready
                      : (lq_stg_state_q == LS_EMPTY);

  // The fault response the stage hands to the load queue. It is offered only
  // when the endpoint is not delivering a load-queue response in the same cycle,
  // so the queue never sees two in one cycle.
  assign lq_xl_fault_valid = (lq_stg_state_q == LS_FAULT) &&
                             !(ep_rsp_valid && !ep_owner_q);
  always_comb begin
    lq_xl_fault_rsp.id    = lq_hold_q.id;
    lq_xl_fault_rsp.fault = 1'b1;
    lq_xl_fault_rsp.cause = {60'd0, lq_xl_cause_q};
    lq_xl_fault_rsp.tval  = lq_hold_tval_q;
    lq_xl_fault_rsp.data  = {CORE_XLEN{1'b0}};
  end

  // ------------------------------------------------------------- the PTW arbiter
  // The store request has priority (it is holding up retirement), and the load
  // request excludes itself while the store's is presented so only one is ever
  // offered. One walk in flight at a time.
  assign ptw_xl_req_valid = !ptw_busy_q && (st_req_valid || lq_xl_req_valid);
  assign ptw_xl_va        = st_req_valid ? st_req_va : (lq_hold_q.base + lq_hold_q.imm);
  assign ptw_xl_kind      = st_req_valid ? 2'd1 : (lq_is_store_c ? 2'd1 : 2'd0);
  assign ptw_xl_priv      = st_req_valid ? eff_priv_c : lq_priv_q;
  assign ptw_xl_mode      = st_req_valid ? csr_satp[63:60] : lq_mode_q;
  assign ptw_xl_ppn       = st_req_valid ? csr_satp[43:0] : lq_ppn_q;
  assign ptw_xl_sum       = st_req_valid ? o_csr_mstatus[18] : lq_sum_q;
  assign ptw_xl_mxr       = st_req_valid ? o_csr_mstatus[19] : lq_mxr_q;
  assign ptw_xl_rsp_ready = 1'b1;
  assign ptw_take_c       = ptw_xl_req_valid && ptw_xl_req_ready;
  assign lq_xl_req_accepted = ptw_take_c && !st_req_valid;
  // A redirect withdraws a load's walk. A store's walk is never cancelled: the
  // store at the head survives a younger redirect, and if a trap does discard it
  // the result is keyed by a generation that can no longer match.
  assign ptw_xl_cancel    = ptw_busy_q && (ptw_owner_q == 1'b0) && lq_flush;

  // ------------------------------------------------------ the store translation
  assign st_hit0_c = st_r0_valid_q && rob_head_valid &&
                     (st_r0_idx_q == rob_head_index) && (st_r0_gen_q == rob_head_gen);
  assign st_hit1_c = st_r1_valid_q && rob_head1_valid &&
                     (st_r1_idx_q == rob_head1_index) && (st_r1_gen_q == rob_head1_gen);
  assign st_fault0_c = st_hit0_c && st_r0_fault_q;
  assign st_fault1_c = st_hit1_c && st_r1_fault_q;
  assign st_cause0_c = st_r0_cause_q;
  assign st_pa0_c    = st_hit0_c ? st_r0_pa_q : sq_pay0_addr;
  assign st_pa1_c    = st_hit1_c ? st_r1_pa_q : sq_pay1_addr;

  assign st_want0_c = xlate_active_c && rob_head_valid && desc_is_store0 &&
                      ret_req[0] && !st_hit0_c;
  assign st_lane0_blocks_c = st_want0_c;
  assign st_want1_c = xlate_active_c && rob_head1_valid && desc_is_store1 &&
                      ret_req[1] && !st_hit1_c;
  assign st_lane1_blocks_c = st_want1_c;
  assign st_req_valid    = st_want0_c || (st_want1_c && !st_lane0_blocks_c);
  assign st_req_lane1_c  = !st_want0_c && st_want1_c;
  assign st_req_va       = st_req_lane1_c ? sq_pay1_addr : sq_pay0_addr;
  assign st_req_idx      = st_req_lane1_c ? rob_head1_index : rob_head_index;
  assign st_req_gen      = st_req_lane1_c ? rob_head1_gen : rob_head_gen;

  // The commit-boundary fault, and the PMP address the commit check must use:
  // the translated physical address when the store has one, its effective
  // address otherwise (Bare, where they are the same value).
  assign store0_xl_fault_c = xlate_active_c && rob_head_valid && desc_is_store0 &&
                             ret_req[0] && st_fault0_c;
  assign store0_xl_fault_now = store0_xl_fault_c && !trap_decision;
  assign pmp_store_addr0_c = (st_hit0_c && desc_is_store0) ? st_r0_pa_q : sq_pay0_addr;
  assign pmp_store_addr1_c = (st_hit1_c && desc_is_store1) ? st_r1_pa_q : sq_pay1_addr;

  // ------------------------------------------------- the drain address FIFO
  // Every retiring store pushes the address its drain must use: the translated
  // physical address when it has one, its effective address otherwise. Pushes
  // are in retire order and pops in drain order -- both are program order -- so a
  // pop always names the store that was pushed first. The recorded address is
  // what makes a `satp` write after the store retire irrelevant to it.
  // The push conditions are the store queue's *authorisations*, not the ROB
  // retirements: `commit_ok` is the queue saying it accepted the authorisation
  // for the entry at its watermark, and the queue's watermark advances by
  // exactly one per `commit_ok`/`commit2_ok`. Pushing on anything else would let
  // the FIFO and the queue's authorisation watermark drift apart.
  assign st_push0_c = sq_commit_ok;
  assign st_push1_c = sq_commit2_ok;
  assign st_pop_c   = sq_drain_valid && sq_drain_ready;
  assign sq_drain_tval_c = sq_drain_req.base + sq_drain_req.imm;
  assign sq_drain_pa_c = (st_fifo_cnt_q != {CORE_MEM_CNT_W{1'b0}})
                       ? st_fifo_pa[st_fifo_head_q]
                       : sq_drain_tval_c;

  assign ep_tval_c = sq_drain_valid ? sq_drain_tval_c : lq_tx_tval;

  // --------------------------------------------------------------------------
  // The stage and store-translation state
  // --------------------------------------------------------------------------
  always_ff @(posedge clk) begin
    if (rst) begin
      lq_stg_state_q   <= LS_EMPTY;
      lq_xl_wait_q     <= 1'b0;
      ptw_busy_q       <= 1'b0;
      ptw_owner_q      <= 1'b0;
      st_r0_valid_q    <= 1'b0;
      st_r1_valid_q    <= 1'b0;
      st_req_lane1_q   <= 1'b0;
      st_fifo_cnt_q    <= {CORE_MEM_CNT_W{1'b0}};
      st_fifo_head_q   <= {ST_FIFO_PTR_W{1'b0}};
      st_fifo_tail_q   <= {ST_FIFO_PTR_W{1'b0}};
      st_fifo_ovf_ctr_q <= 32'd0;
      st_xl_alloc_ctr_q <= 32'd0;
      st_xl_pop_ctr_q   <= 32'd0;
    end else begin
      // ------------------------------------------------------- walker ownership
      if (ptw_take_c) begin
        ptw_busy_q  <= 1'b1;
        ptw_owner_q <= st_req_valid;
        st_req_lane1_q <= st_req_lane1_c;
        st_req_idx_q   <= st_req_idx;
        st_req_gen_q   <= st_req_gen;
      end
      if (ptw_xl_rsp_valid) begin
        ptw_busy_q <= 1'b0;
      end
      // A cancellation has no response: clear the ownership directly.
      if (ptw_xl_cancel) begin
        ptw_busy_q <= 1'b0;
      end

      // ------------------------------------------------------ the load stage
      case (lq_stg_state_q)
        LS_EMPTY: begin
          lq_xl_wait_q <= 1'b0;
          if (!lq_bypass_c && lq_req_valid) begin
            lq_hold_q      <= lq_req;
            lq_hold_tval_q <= lq_req.base + lq_req.imm;
            lq_priv_q      <= eff_priv_c;
            lq_mode_q      <= csr_satp[63:60];
            lq_ppn_q       <= csr_satp[43:0];
            lq_sum_q       <= o_csr_mstatus[18];
            lq_mxr_q       <= o_csr_mstatus[19];
            if (xlate_active_c) lq_stg_state_q <= LS_XL;
            else begin
              lq_hold_pa_q   <= lq_req.base + lq_req.imm;
              lq_stg_state_q <= LS_PRESENT;
            end
          end
        end
        LS_XL: begin
          if (lq_flush) begin
            lq_xl_wait_q   <= 1'b0;
            lq_stg_state_q <= LS_EMPTY;
          end else if (!lq_xl_wait_q) begin
            if (lq_xl_req_accepted) lq_xl_wait_q <= 1'b1;
          end else if (ptw_xl_rsp_valid && (ptw_owner_q == 1'b0)) begin
            lq_xl_wait_q <= 1'b0;
            if (ptw_xl_fault) begin
              lq_xl_cause_q  <= ptw_xl_cause;
              lq_stg_state_q <= LS_FAULT;
            end else begin
              lq_hold_pa_q   <= ptw_xl_pa;
              lq_stg_state_q <= LS_PRESENT;
            end
          end
        end
        LS_PRESENT: begin
          if (lq_flush) begin
            lq_stg_state_q <= LS_EMPTY;
          end else if (lq_tx_valid && lq_tx_ready) begin
            lq_stg_state_q <= LS_EMPTY;
          end
        end
        LS_FAULT: begin
          if (lq_flush) begin
            lq_stg_state_q <= LS_EMPTY;
          end else if (lq_xl_fault_valid && lq_rsp_ready) begin
            lq_stg_state_q <= LS_EMPTY;
          end
        end
        default: lq_stg_state_q <= LS_EMPTY;
      endcase

      // ------------------------------------------------ the store result latch
      if (ptw_xl_rsp_valid && (ptw_owner_q == 1'b1)) begin
        if (st_req_lane1_q) begin
          st_r1_valid_q <= 1'b1;
          st_r1_idx_q   <= st_req_idx_q;
          st_r1_gen_q   <= st_req_gen_q;
          st_r1_pa_q    <= ptw_xl_pa;
          st_r1_fault_q <= ptw_xl_fault;
        end else begin
          st_r0_valid_q <= 1'b1;
          st_r0_idx_q   <= st_req_idx_q;
          st_r0_gen_q   <= st_req_gen_q;
          st_r0_pa_q    <= ptw_xl_pa;
          st_r0_fault_q <= ptw_xl_fault;
          st_r0_cause_q <= ptw_xl_cause;
        end
      end
      // A result is consumed by the retirement it authorised, and a redirect
      // discards anything it might still be used for.
      if (rob_flush_pulse) begin
        st_r0_valid_q <= 1'b0;
        st_r1_valid_q <= 1'b0;
      end
      if (rob_retire_ack && desc_is_store0)      st_r0_valid_q <= 1'b0;
      if (rob_retire_ack_next && desc_is_store1) st_r1_valid_q <= 1'b0;

      // ------------------------------------------------------- the drain FIFO
      if (st_push0_c) begin
        st_fifo_pa[st_fifo_tail_q] <= st_pa0_c;
        st_xl_alloc_ctr_q <= st_xl_alloc_ctr_q + 32'd1;
      end
      if (st_push1_c) begin
        // The second lane's entry goes one past the first *only when the first
        // lane pushed too*; a lane-1-only commit appends at the tail itself.
        st_fifo_pa[st_fifo_tail_q + ST_FIFO_PTR_W'(st_push0_c)] <= st_pa1_c;
        st_xl_alloc_ctr_q <= st_xl_alloc_ctr_q + 32'd1;
      end
      if (st_pop_c) st_xl_pop_ctr_q <= st_xl_pop_ctr_q + 32'd1;
      if ((st_push0_c || st_push1_c) &&
          (st_fifo_cnt_q == CORE_MEM_CNT_W'(CORE_SQ_N)) && !st_pop_c) begin
        st_fifo_ovf_ctr_q <= st_fifo_ovf_ctr_q + 32'd1;
      end
      st_fifo_head_q <= st_fifo_head_q + ST_FIFO_PTR_W'(st_pop_c);
      st_fifo_tail_q <= st_fifo_tail_q + ST_FIFO_PTR_W'(st_push0_c)
                                       + ST_FIFO_PTR_W'(st_push1_c);
      st_fifo_cnt_q  <= st_fifo_cnt_q
                      + CORE_MEM_CNT_W'(st_push0_c) + CORE_MEM_CNT_W'(st_push1_c)
                      - CORE_MEM_CNT_W'(st_pop_c);
    end
  end

  assign ep_req_valid = sq_drain_valid | lq_tx_valid;

  // The AMO overlay (I-039). The operation and operand of the one atomic
  // read-modify-write the load queue is carrying live in mosaic_amo_unit; when
  // the load queue offers that macro (matched by its whole identity) they are
  // merged into the request here. A store drain, and any ordinary load, is
  // unchanged: `amo_active_c` is high only for the load-queue offer of the held
  // AMO.
  logic amo_active_c;
  assign amo_active_c = amo_hit && !sq_drain_valid && lq_tx_valid;

  // I-040: the class of the held atomic macro. The load queue issues all three
  // as load class and carries none of these fields (see its atomic-head port),
  // so the integration states the class here from the record it matched.
  logic atomic_lr_c, atomic_sc_c;
  assign atomic_lr_c = amo_active_c && amo_hit_lr;
  assign atomic_sc_c = amo_active_c && amo_hit_sc;

  always_comb begin
    if (sq_drain_valid) begin
      // A store drains to the physical address its translation recorded; in
      // Bare that address is its effective address, so the expression is
      // uniform. `imm` is zero because the whole address is in `base`.
      ep_req      = sq_drain_req;
      ep_req.base = sq_drain_pa_c;
      ep_req.imm  = {CORE_XLEN{1'b0}};
    end else begin
      // A load arrives already translated (the stage above), with the physical
      // address in `base` and `imm` zero.
      ep_req = lq_tx_req;
    end
    ep_req.store_data = amo_active_c ? amo_hit_operand : ep_req.store_data;
    ep_req.is_amo     = amo_active_c && !atomic_lr_c && !atomic_sc_c;
    ep_req.amo_op     = amo_active_c ? amo_hit_op : mosaic_pkg::AMO_ADD;
    ep_req.aq         = amo_active_c && amo_hit_aq;
    ep_req.rl         = amo_active_c && amo_hit_rl;
    ep_req.is_lr      = atomic_lr_c;
    ep_req.is_sc      = atomic_sc_c;
  end

  assign sq_drain_ready = ep_req_ready;

  // --------------------------------------------------------------------------
  // The device serializer (I-038): non-speculative MMIO
  // --------------------------------------------------------------------------
  // Everything the queues offer goes through this point before the endpoint.
  // Its rule, in one paragraph:
  //
  //   **A device access is presented to the memory system exactly once, only
  //   when it is irreversible-safe.** For a *load* that means the access is the
  //   ROB head at a legal boundary: every older instruction has already
  //   committed, so nothing can squash it and no older access can be reordered
  //   after it. For a *store* it is the store queue's own contract -- a store is
  //   offered for drain only after the instruction that owns it has retired (the
  //   authorisation watermark), which is the same statement one step later.
  //
  //   An ordinary (idempotent) access is not serialized: it passes straight
  //   through, so speculative RAM loads keep their existing timing and the fast
  //   path this profile is built around is unchanged.
  //
  // Why a register rather than a combinational gate: the device transaction is
  // *owned* here from the cycle its requester is told "accepted" until its
  // response is consumed. That has two consequences the card asks for by name.
  // First, the requester cannot re-offer the transaction (it has been accepted),
  // so the access is presented once and a replay is impossible -- the store
  // queue's and the load queue's own exactly-once rules are not weakened, they
  // are extended. Second, the classification and the identity travel *with* the
  // held transaction, so the attribute the memory system sees cannot change
  // between the decision and the access, which is what makes "a device access is
  // not coalesced with RAM" a property of the access rather than of a cycle.
  //
  // A device access is also a barrier for the ordinary path: while one is held,
  // the endpoint port is not offered to anything else, so no younger RAM access
  // can overtake it. It is never the other way round -- the queue mux gives the
  // store drain priority, and every store older than the head has already
  // drained -- so the order the memory system sees is program order.

  assign ser_addr_c      = ep_req.base + ep_req.imm;
  assign ser_is_device_c = mosaic_uop_pkg::is_device_addr(ser_addr_c);
  // An atomic read-modify-write is non-idempotent for exactly the same reason a
  // device is: it has a side effect that a squashed or repeated instruction must
  // not perform. So it takes the *same* serialization path -- presented once,
  // only when it is the ROB head and cannot be squashed, and a barrier for
  // everything else while it is held -- and the only thing the memory system is
  // told differently is the atomic attribute itself. Keeping the device
  // attribute separate (`ser_hold_dev_q`) is what stops an AMO to RAM being
  // reported to the memory system as an MMIO access.
  assign ser_is_amo_c    = ep_req.is_amo || ep_req.is_lr || ep_req.is_sc;
  assign ser_serialize_c = ser_is_device_c || ser_is_amo_c;

  // The identity of the ROB head, built exactly as every other identity in this
  // file is (one uop per macro, one hart), so the comparison below is a whole
  // identity and not a wrapping index.
  assign rob_head_id = {1'b0, rob_head_index, rob_head_gen, {CORE_UOP_W{1'b0}}};

  // A device is non-speculative when it cannot be squashed. `rob_boundary_ok` is
  // the same legal-boundary conjunction the system unit uses: not a redirect in
  // flight, not recovering, not stopped.
  assign rob_boundary_ok = !redirect_valid && !recovering && !core_stop;

`ifdef MOSAIC_CORE_MUTANT_DEV_SPECULATIVE
  // NEGATIVE CONTROL: the non-speculation gate is removed, so a device load is
  // issued as soon as the load queue offers it -- while it is still speculative.
  // A load on a wrong path reaches the device and its side effect is performed
  // for an instruction that never retires; CASE=mmio.exactly_once's
  // "the device performed exactly the retired accesses" check names it.
  assign ser_nonspec_c = 1'b1;
`else
  assign ser_nonspec_c = ep_req.we ? 1'b1
                                   : (rob_head_valid && rob_boundary_ok &&
                                      mosaic_uop_pkg::uop_id_eq(ep_req.id, rob_head_id));
`endif

  // The upstream acceptance. A device offer is taken into the hold regardless of
  // whether the endpoint can take it now; anything else is passed through, and
  // the upstream is told "accepted" only when the endpoint actually takes it, so
  // the payload stays stable in between by the project-wide transport rule.
  assign ep_req_ready  = ser_hold_valid_q ? 1'b0
                       : (ser_serialize_c ? (ser_nonspec_c && !ser_dev_out_q)
                                          : ser_ep_req_ready);
  // A device offer never passes through combinationally: it is always taken into
  // the hold first, so the access is presented to the endpoint in exactly one
  // cycle-window owned by this register -- the property the RETRY control below
  // removes.
  assign ser_req_valid = ser_hold_valid_q ? 1'b1 : (ep_req_valid && !ser_serialize_c);
  assign ser_req       = ser_hold_valid_q ? ser_hold_q : ep_req;

`ifdef MOSAIC_CORE_MUTANT_DEV_AS_RAM
  // NEGATIVE CONTROL: the classification the memory system is told is cleared.
  // The access is still serialized correctly -- the gate below uses the real
  // predicate -- but it is presented as an ordinary access, which is exactly the
  // attribute a coalescer or a cache keys on when it refuses to merge MMIO with
  // RAM. CASE=mmio.exactly_once's "every access carries the attribute its
  // address's region demands" check names it, and the device/RAM transaction
  // counters disagree with the memory system's address-based classification.
  assign ser_out_dev_c = 1'b0;
`else
  // The PMA attribute of the transaction the endpoint is being offered. It is
  // the *held* transaction's attribute and nothing else: an AMO to RAM is held
  // too and must not be reported as a device, while an access that is not held
  // at all (only an ordinary, non-serialized one can be) is by construction not
  // a device. Reading the latch alone would report a stale attribute for that
  // direct access -- the previous held transaction's -- which is exactly the
  // misclassification CASE=mmio.exactly_once's "every access carries the device
  // attribute its region demands" check names.
  assign ser_out_dev_c = ser_hold_valid_q && ser_hold_dev_q;
`endif

  assign ser_take_c = ep_req_valid && ep_req_ready && ser_serialize_c;
  assign ser_accept_c = ser_req_valid && ser_ep_req_ready;
  assign ser_owner_c  = ser_hold_valid_q ? ser_owner_q : sq_drain_valid;

  // The response being consumed is what ends the device transaction's life.
  logic ser_rsp_consumed;
  assign ser_rsp_consumed = ep_rsp_valid && ep_rsp_ready;

  always_ff @(posedge clk) begin
    if (rst) begin
      ser_hold_valid_q <= 1'b0;
      ser_dev_out_q    <= 1'b0;
      ser_hold_dev_q   <= 1'b0;
      ser_hold_tval_q  <= 64'd0;
      dev_txn_ctr_q    <= 32'd0;
      ram_txn_ctr_q    <= 32'd0;
      dev_wait_ctr_q   <= 32'd0;
      dev_hold_ctr_q   <= 32'd0;
    end else begin
      if (ser_take_c) begin
        ser_hold_q       <= ep_req;
        ser_hold_valid_q <= 1'b1;
        ser_dev_out_q    <= 1'b1;
        // I-045: the architectural tval travels with the held transaction for
        // the same reason its identity and device attribute do -- the queue mux
        // may be presenting a different request by the time the endpoint takes
        // this one.
        ser_hold_tval_q  <= ep_tval_c;
        // The PMA attribute of *this* transaction, not of whatever the mux
        // happens to offer after it. An AMO (also serialized) latches 0.
        ser_hold_dev_q   <= ser_is_device_c;
        // The owner travels with the held transaction: the queue that offered
        // it may have moved on (its request was accepted), and by the time the
        // endpoint takes the transaction the mux may be presenting a different
        // request entirely. Recording the owner here and publishing it when the
        // *endpoint* accepts is what keeps a response with the queue it belongs
        // to.
        ser_owner_q      <= sq_drain_valid;
      end
      if (ser_hold_valid_q && ser_req_valid && ser_ep_req_ready) begin
`ifdef MOSAIC_CORE_MUTANT_DEV_RETRY
        // NEGATIVE CONTROL: the held device transaction is not released when the
        // endpoint takes it, so the same access is offered again as soon as the
        // endpoint is free -- a transport retry that repeats the side effect.
        // The identity probe then shows one identity twice, which is the
        // card's "a replayed transaction duplicates a side effect".
        ser_hold_valid_q <= 1'b1;
`else
        ser_hold_valid_q <= 1'b0;
`endif
      end
      if (ser_dev_out_q && ser_rsp_consumed) begin
        ser_dev_out_q <= 1'b0;
      end
      if (ser_accept_c && ser_out_dev_c) dev_txn_ctr_q <= dev_txn_ctr_q + 32'd1;
      if (ser_accept_c && !ser_out_dev_c) ram_txn_ctr_q <= ram_txn_ctr_q + 32'd1;
      if (ep_req_valid && ser_is_device_c && !ser_nonspec_c && !ser_hold_valid_q &&
          !ser_dev_out_q) begin
        dev_wait_ctr_q <= dev_wait_ctr_q + 32'd1;
      end
      if (ser_hold_valid_q) begin
        dev_hold_ctr_q <= dev_hold_ctr_q + 32'd1;
      end
    end
  end

  assign o_mem_dev_txn  = dev_txn_ctr_q;
  assign o_mem_ram_txn  = ram_txn_ctr_q;
  assign o_mem_dev_wait = dev_wait_ctr_q;
  assign o_mem_dev_hold = dev_hold_ctr_q;
  assign o_mem_dmem_dev = ep_txn_dev;
  assign o_mem_dmem_id  = ep_txn_id;
  // I-040: the reservation and the LR/SC path, straight from the endpoint.
  assign o_mem_dmem_kind           = ep_txn_kind;
  assign o_mem_res_valid           = ep_res_valid;
  assign o_mem_res_granule         = ep_res_granule;
  assign o_mem_lr_ctr              = ep_lr_ctr;
  assign o_mem_sc_ok_ctr           = ep_sc_ok_ctr;
  assign o_mem_sc_fail_ctr         = ep_sc_fail_ctr;
  assign o_mem_res_ext_inval_ctr   = ep_res_ext_inval_ctr;
`ifndef SYNTHESIS
  // Debug bundle for CASE=mmio.exactly_once while the device path is brought up:
  //   {31 lq_rsp_valid, 30 sq_rsp_valid, 29 ser_dev_out_q, 28 ser_hold_valid_q,
  //    27 ep_busy, 26 ep_rsp_valid, 25 ep_owner_q, 24 sq_drain_valid,
  //    23 lq_req_valid, 22:0 lq_done_ctr}
  assign o_dbg_mmio = {lq_rsp_valid, sq_rsp_valid, ser_dev_out_q, ser_hold_valid_q,
                       ep_busy, ep_rsp_valid, ep_owner_q, sq_drain_valid, lq_req_valid,
                       lq_done_ctr[22:0]};
`else
  assign o_dbg_mmio = 32'd0;
`endif

  // --------------------------------------------------------------------------
  // The atomic read-modify-write's issue record (I-039)
  // --------------------------------------------------------------------------
  // The one AMO the load queue is carrying. It is written when the load queue
  // accepts the macro and read back, by identity, when the queue offers it; the
  // operation and operand are merged into the endpoint request in the queue mux
  // above. `amo_taken_c` is the cycle the serializer takes the transaction: from
  // then on the fields are latched in `ser_hold_q` and the endpoint, and a second
  // copy here would be a second thing to keep in step.
  assign amo_taken_c = (ser_take_c &&
                        (ep_req.is_amo || ep_req.is_lr || ep_req.is_sc)) ||
                       (lq_xl_fault_valid &&
                        (lq_hold_q.is_amo || lq_hold_q.is_lr || lq_hold_q.is_sc));

  mosaic_amo_unit u_amo (
      .clk            (clk),
      .rst            (rst),
      .alloc_valid_i  (amo_alloc_c),
      .alloc_id_i     (disp_mem_full_id),
      .alloc_op_i     (disp_mem_amo_op),
      .alloc_aq_i     (disp_mem_amo_aq),
      .alloc_rl_i     (disp_mem_amo_rl),
      .alloc_is_lr_i  (disp_mem_is_lr),
      .alloc_is_sc_i  (disp_mem_is_sc),
      .alloc_operand_i(disp_mem_data),
      .taken_i        (amo_taken_c),
      .flush_i        (lq_flush),
      .probe_id_i     ((!sq_drain_valid && lq_tx_valid) ? lq_tx_req.id : lq_req.id),
      .match_o        (amo_hit),
      .op_o           (amo_hit_op),
      .aq_o           (amo_hit_aq),
      .rl_o           (amo_hit_rl),
      .is_lr_o        (amo_hit_lr),
      .is_sc_o        (amo_hit_sc),
      .operand_o      (amo_hit_operand),
      .atomic_id_o    (amo_atomic_id),
      .atomic_valid_o (amo_atomic_valid),
      .busy_o         (amo_busy)
  );

  // ==========================================================================
  // 14b. Physical memory protection (I-044)
  // ==========================================================================
  // One unit, two questions per cycle. The entries and their WARL/lock rules
  // live in mosaic_pmp; this section states only what an access *is* -- which
  // privilege it executes at and whether it reads or writes -- and what a refusal
  // means on each side.
  //
  // The effective privilege comes from mstatus: "When MPRV=1, load and store
  // memory addresses are translated and protected ... as though the current
  // privilege mode were set to MPP", and it applies to loads and stores only
  // ("Instruction address-translation and protection are unaffected by the
  // setting of MPRV"), which is why the fetch query is given the current mode.
  //
  // The class of the access is the specification's, not a guess: "Attempting to
  // execute a load or load-reserved instruction ... without read permissions
  // raises a load access-fault exception. Attempting to execute a store,
  // store-conditional, or AMO instruction ... without write permissions raises a
  // store access-fault exception". Those five instruction forms are exactly the
  // five this endpoint carries, and the same predicate the endpoint uses to pick
  // the cause (we || is_amo || is_sc) picks the class here.
  assign lsu_req_bytes_c = 4'(mosaic_uop_pkg::size_bytes(ser_req.size));
  assign lsu_req_w_c     = ser_req.we | ser_req.is_amo | ser_req.is_sc;
  assign lsu_req_r_c     = ~lsu_req_w_c;
  assign eff_priv_c      = ((csr_priv == mosaic_csr_pkg::MOSAIC_PRIV_M) &&
                            (o_csr_mstatus[17] == 1'b1))
                           ? o_csr_mstatus[12:11] : csr_priv;

  assign ep_pmp_deny_c = ser_req_valid && !pmp_data_allow;

  // --------------------------------------------------- store-commit PMP check
  // D5. The endpoint's refusal is a fact about a transaction; the ISA's store
  // access fault is a fact about the *instruction*, and it is precise only if
  // the store has not retired when it is taken. So the permission question is
  // asked here, in the cycle the ROB is about to authorise the store, when the
  // CSR state that answers it is committed: `req_priv` is `eff_priv_c`, the same
  // MPRV-adjusted privilege the endpoint uses, and the entries are the
  // committed ones because every older instruction -- including any PMP CSR
  // write -- has retired.
  //
  // The address and size are the store queue's own view of the entry at the
  // authorisation watermark, the same `base + imm` and size the retire event
  // publishes; the check cannot describe a different access from the one that
  // would reach memory. The questions are asked of the PMP unit's store-commit
  // port, which is separate from the endpoint's data port because a younger
  // load can be using that one in the same cycle.
  //
  // The first store the ROB would commit this cycle is lane 0's when the head
  // is a store, otherwise lane 1's (a non-store lane 0 authorises nothing, so a
  // store behind it is still the first unauthorised entry); either way it sits
  // at the watermark. The second store can only exist when lane 0 is also a
  // store, and it then sits one entry past the watermark.
  logic [3:0] pmp_store_bytes0_c, pmp_store_bytes1_c;

  assign pmp_store_bytes0_c = 4'(mosaic_uop_pkg::size_bytes(sq_pay0_size));
  assign pmp_store_bytes1_c = 4'(mosaic_uop_pkg::size_bytes(sq_pay1_size));

  logic first_store_c, second_store_c;
  assign first_store_c  = ret_req[0] && rob_head_valid && rob_head_ready &&
                          (desc_is_store0 ||
                           (rob_head1_valid && desc_is_store1 && rob_head1_ready)) &&
                          sq_pay0_ready;
  assign second_store_c = ret_req[0] && rob_head_valid && desc_is_store0 &&
                          rob_head1_valid && desc_is_store1 && rob_head1_ready &&
                          sq_pay1_ready;

`ifdef MOSAIC_PMP_MUTANT_STORE_DENY_NOT_TAKEN
  // NEGATIVE CONTROL for D5: the commit-path check is removed, so a store is
  // authorised on its retirement exactly as it was before the fix and the
  // endpoint's post-retirement refusal is counted and dropped. The store then
  // retires and the program's trailing instruction is what traps -- the defect
  // the fix exists to remove. CASE=privilege.permission_matrix's
  // `s-store-deny-w` must fail on it.
  assign store0_pmp_deny_c = 1'b0;
  assign store1_pmp_deny_c = 1'b0;
`else
  // Lane 0's store, refused. With paging on, the check runs on the translated
  // physical address (`pmp_store_addr0_c` on the store-commit port below) and it
  // is suppressed while the translation is unresolved or has itself faulted: a
  // page fault is decided by the translation, not by the PMP.
  assign store0_pmp_deny_c = first_store_c && desc_is_store0 && !pmp_store_allow0_c &&
                             !st_lane0_blocks_c && !store0_xl_fault_c;
  // Lane 1's store, refused: either it is the first store (lane 0 is not a
  // store) and the watermark entry is refused, or lane 0 is a store and lane
  // 1's entry -- one past the watermark -- is the one refused.
  assign store1_pmp_deny_c =
      ((second_store_c && !store0_pmp_deny_c && !pmp_store_allow1_c) ||
       (first_store_c && !desc_is_store0 && !pmp_store_allow0_c)) &&
      !st_lane1_blocks_c && !st_fault1_c;
`endif

  // The trap the commit path takes: the lane-0 store's own exception, at its
  // own PC (the trap controller's synchronous epc is the ROB head) with the
  // access address as tval. It is latched only when no other trap is being
  // decided in the same cycle, so an interrupt at the same boundary wins and the
  // store is simply re-fetched and checked again after the handler returns.
  assign store_pmp_trap_now    = (store0_pmp_deny_c || store0_xl_fault_now) &&
                                 !trap_decision;
  assign store_pmp_trap_addr_c = sq_pay0_addr;

  mosaic_pmp u_pmp (
      .clk_i              (clk),
      .rst_i              (rst),
      .csr_addr_i         (csr_addr),
      .csr_rdata_o        (csr_pmp_rdata),
      .csr_we_i           (csr_pmp_we),
      .csr_wdata_i        (csr_pmp_wdata),
      .req_addr_i         (lsu_req_addr_c),
      .req_bytes_i        (lsu_req_bytes_c),
      .req_r_i            (lsu_req_r_c),
      .req_w_i            (lsu_req_w_c),
      .req_x_i            (1'b0),
      .req_priv_i         (eff_priv_c),
      .allow_o            (pmp_data_allow),
      .matched_o          (pmp_data_matched),
      .locked_o           (pmp_data_locked),
      .f_req_addr_i       (fetch_next_pc),
      .f_req_bytes_i      (4'd4),
      .f_req_priv_i       (csr_priv),
      .f_allow_o          (pmp_fetch_allow),
      .f_matched_o        (pmp_fetch_matched),
      .f_locked_o         (pmp_fetch_locked),
      .sc_req_addr0_i     (pmp_store_addr0_c),
      .sc_req_bytes0_i    (pmp_store_bytes0_c),
      .sc_req_priv0_i     (eff_priv_c),
      .sc_req_addr1_i     (pmp_store_addr1_c),
      .sc_req_bytes1_i    (pmp_store_bytes1_c),
      .sc_req_priv1_i     (eff_priv_c),
      .sc_allow0_o        (pmp_store_allow0_c),
      .sc_allow1_o        (pmp_store_allow1_c),
      .o_query_ctr        (pmp_query_ctr),
      .o_deny_ctr         (pmp_deny_ctr),
      .o_locked_ctr       (pmp_locked_ctr),
      .o_fetch_deny_ctr   (pmp_fetch_deny_ctr),
      .o_entry_cfg_o      (o_pmp_cfg),
      .o_entry_addr_o     (o_pmp_addr)
  );

  mosaic_lsu_endpoint u_lsu (
      .clk                  (clk),
      .rst                  (rst),
      .req_valid_i          (ser_req_valid),
      .req_ready_o          (ser_ep_req_ready),
      .req_i                (ser_req),
      .req_dev_i            (ser_out_dev_c),
      // The permission answer for the request being offered. The refusal is
      // taken inside the endpoint, in the same place the misalignment refusal
      // is, so a denied access never enters ST_REQ and the memory system never
      // sees it.
      .o_req_addr_o         (lsu_req_addr_c),
      .pmp_deny_i           (ep_pmp_deny_c),
      // I-045: the architectural tval of the transaction the endpoint is being
      // offered. With translation in the path `o_req_addr_o` is the *physical*
      // address; the exception's tval is the virtual one, and for a held
      // (device/atomic) transaction it is the copy latched with the hold.
      .req_tval_i           (ser_hold_valid_q ? ser_hold_tval_q : ep_tval_c),
      .rsp_valid_o          (ep_rsp_valid),
      .rsp_ready_o          (ep_rsp_ready),
      .rsp_o                (ep_rsp),
      // The endpoint's memory port is merged with the walker's PTE port onto the
      // core's `dmem` port by the arbiter above; the endpoint never sees `dmem`
      // directly.
      .mem_req_valid_o      (ep_mem_req_valid),
      .mem_req_ready_i      (ep_mem_req_ready),
      .mem_req_o            (ep_mem_req),
      .mem_rsp_valid_i      (ep_mem_rsp_valid),
      .mem_rsp_ready_o      (ep_mem_rsp_ready),
      .mem_rsp_i            (ep_mem_rsp),
      .ext_write_valid_i    (ext_write_valid),
      .ext_write_addr_i     (ext_write_addr),
      .ext_write_bytes_i    (ext_write_bytes),
      .flush_i              (lq_flush),
      .o_busy               (ep_busy),
      .o_load_ctr           (),
      .o_store_ctr          (),
      .o_txn_ctr            (lsu_txn_ctr),
      .o_misaligned_ctr     (lsu_misaligned_ctr),
      .o_access_fault_ctr   (lsu_access_fault_ctr),
      .o_rsp_ctr            (),
      .o_last_fault_cause   (),
      .o_last_fault_tval    (),
      .o_inflight_addr      (),
      .o_inflight_size      (),
      .o_txn_id             (ep_txn_id),
      .o_txn_dev            (ep_txn_dev),
      .o_txn_kind           (ep_txn_kind),
      .o_res_valid          (ep_res_valid),
      .o_res_granule        (ep_res_granule),
      .o_lr_ctr             (ep_lr_ctr),
      .o_sc_ok_ctr          (ep_sc_ok_ctr),
      .o_sc_fail_ctr        (ep_sc_fail_ctr),
      .o_res_set_ctr        (),
      .o_res_clear_ctr      (),
      .o_res_ext_inval_ctr  (ep_res_ext_inval_ctr),
      .o_res_hit_ctr        (),
      .o_res_miss_ctr       ()
  );

  // Which queue the endpoint's response belongs to. The endpoint holds one
  // transaction and refuses a second request until it has returned that
  // transaction's response (`accept_c` requires ST_IDLE), so the owner recorded
  // at acceptance is exact and a response can never be delivered to the wrong
  // queue.
  // The owner is recorded when the *endpoint* accepts the transaction it is
  // presenting -- `ser_accept_c`, which is the same handshake the endpoint
  // uses -- and not when the queues' offer is taken. The difference is
  // load-bearing under backpressure: the endpoint may still be serving an older
  // transaction when a device offer is taken into the serializer's register, and
  // recording the owner at the take would hand that older transaction's response
  // to the new owner. That defect is reachable at any memory latency above one
  // cycle; CASE=mmio.exactly_once's four-cycle latency is what exposed it.
  always_ff @(posedge clk) begin
    if (rst) begin
      ep_owner_q <= 1'b0;
    end else if (ser_accept_c) begin
      ep_owner_q <= ser_owner_c;
    end
  end
  // I-045: the load queue's response is either the endpoint's (for an access that
  // reached it) or the translation stage's own fault (for an access that never
  // did). The two cannot be offered in the same cycle -- the stage withholds its
  // fault while the endpoint is delivering a load-queue response -- so the mux
  // is exact, not a priority that could drop one.
  assign lq_rsp_valid = (ep_rsp_valid && !ep_owner_q) || lq_xl_fault_valid;
  assign sq_rsp_valid = ep_rsp_valid &&  ep_owner_q;
  assign ep_rsp_ready = ep_owner_q ? sq_rsp_ready
                                   : (lq_rsp_ready && !lq_xl_fault_valid);

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
      // I-040: the atomic record, so the queue never forwards into or replays an
      // AMO/LR/SC head.
      .atomic_id_i          (amo_atomic_id),
      .atomic_valid_i       (amo_atomic_valid),
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
      .rsp_i                (lq_xl_fault_valid ? lq_xl_fault_rsp : ep_rsp),
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
      .commit_ok_o          (sq_commit_ok),
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

  // ------------------------------------------------------ the store fault check
  //
  // A store's address is known before it is allocated -- dispatch holds the
  // macro until its base operand and its payload are readable, and the address
  // is `base + imm`, which this file computes once, the same way the endpoint
  // computes it. So the two faults a store can take are decidable *before* the
  // store retires, and a precise trap needs exactly that: the store must not
  // have retired when its trap is taken, or mepc would name an instruction that
  // had already committed.
  //
  // The store queue cannot supply this. Its contract is "a store may reach
  // memory only when it is authorised *and* non-faulting", and the endpoint --
  // which owns the fault boundary for the accesses it performs -- only learns
  // about an access fault when the store *drains*, which is after retirement. So
  // the decision this file needs is not the drain's: it is "may this store be
  // allocated at all", and it is made here, from the same address and the same
  // configuration the platform's own rules come from.
  //
  // The rules are the platform's, from `config/memory/p0.json` through the
  // generated `mosaic_cfg_pkg.svh`: an access must lie inside one region, and
  // the region must be writable. boot_rom is readable and executable but not
  // writable; the test-harness window defines no writable register (the frozen
  // protocol's TOHOST and FROMHOST live in RAM); uart, clint and ram are
  // writable. The precedence is the endpoint's: misalignment is decided from the
  // address alone, before the map is consulted, so a misaligned store to an
  // unmapped page reports a misaligned store and not an access fault.
  //
  // A faulting store is *not* allocated into the store queue: its completion is
  // the fault, delivered on the same writeback port a load's fault uses, so the
  // ROB marks the entry exceptional exactly as it does for a load and the trap
  // controller takes it with the store's PC in mepc.
  localparam logic [1:0] STORE_OK       = 2'd0;
  localparam logic [1:0] STORE_MISALIGN = 2'd1;
  localparam logic [1:0] STORE_ACCESS   = 2'd2;

  function automatic logic [1:0] store_fault_kind(input logic [CORE_XLEN-1:0] addr,
                                                  input logic [2:0] size);
    logic [CORE_XLEN-1:0] bytes;
    logic covered;
    logic writable;
    logic in_region;
    begin
      bytes    = (CORE_XLEN'(1) << size);
      covered  = 1'b0;
      writable = 1'b0;
      in_region = 1'b0;
      // "the access lies inside this region, whole": the subtraction is the
      // containment test and it cannot be fooled by an address below the base,
      // because the wrapped difference is then far larger than any size.
      // boot_rom: readable and executable, not writable.
      if ((addr - mosaic_cfg_pkg::MOSAIC_BOOT_ROM_BASE) <
          mosaic_cfg_pkg::MOSAIC_BOOT_ROM_SIZE) begin
        covered   = 1'b1;
        writable  = 1'b0;
        in_region = (((addr - mosaic_cfg_pkg::MOSAIC_BOOT_ROM_BASE) + bytes) <=
                     mosaic_cfg_pkg::MOSAIC_BOOT_ROM_SIZE);
      end
      if ((addr - mosaic_cfg_pkg::MOSAIC_UART_BASE) <
          mosaic_cfg_pkg::MOSAIC_UART_SIZE) begin
        covered   = 1'b1;
        writable  = 1'b1;
        in_region = (((addr - mosaic_cfg_pkg::MOSAIC_UART_BASE) + bytes) <=
                     mosaic_cfg_pkg::MOSAIC_UART_SIZE);
      end
      // The test-harness window is mapped and defines no writable register; the
      // frozen protocol's TOHOST and FROMHOST live in RAM.
      if ((addr - mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_BASE) <
          mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_SIZE) begin
        covered   = 1'b1;
        writable  = 1'b0;
        in_region = (((addr - mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_BASE) + bytes) <=
                     mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_SIZE);
      end
      if ((addr - mosaic_cfg_pkg::MOSAIC_CLINT_BASE) <
          mosaic_cfg_pkg::MOSAIC_CLINT_SIZE) begin
        covered   = 1'b1;
        writable  = 1'b1;
        in_region = (((addr - mosaic_cfg_pkg::MOSAIC_CLINT_BASE) + bytes) <=
                     mosaic_cfg_pkg::MOSAIC_CLINT_SIZE);
      end
      if ((addr - mosaic_cfg_pkg::MOSAIC_RAM_BASE) <
          mosaic_cfg_pkg::MOSAIC_RAM_SIZE) begin
        covered   = 1'b1;
        writable  = 1'b1;
        in_region = (((addr - mosaic_cfg_pkg::MOSAIC_RAM_BASE) + bytes) <=
                     mosaic_cfg_pkg::MOSAIC_RAM_SIZE);
      end

      if ((addr & (bytes - CORE_XLEN'(1))) != {CORE_XLEN{1'b0}}) begin
        store_fault_kind = STORE_MISALIGN;
      end else if (!covered || !in_region || !writable) begin
        store_fault_kind = STORE_ACCESS;
      end else begin
        store_fault_kind = STORE_OK;
      end
    end
  endfunction

  logic [CORE_XLEN-1:0] disp_store_addr;
  logic [1:0]           disp_store_fault;

  assign disp_store_addr  = disp_mem_base + disp_mem_imm;
  assign disp_store_fault = store_fault_kind(disp_store_addr, disp_mem_size);
  // The region check is a *physical* test, so with paging turned on it cannot be
  // made on the effective address: a valid Sv39 mapping may have a virtual
  // address that lands in no region at all. It is therefore made only when
  // translation is off; with translation on, misalignment is still decided here
  // (it is a property of the low offset bits and is unaffected by translation)
  // and the region/PMP decision moves to the store's commit boundary, where the
  // physical address is known.
  assign disp_store_faults = disp_mem_valid && disp_mem_is_store &&
                             ((disp_store_fault == STORE_MISALIGN) ||
                              ((disp_store_fault == STORE_ACCESS) && !xlate_active_c));

  // A store that faults takes the *completion* path and not the queue's: it is
  // never allocated into the store queue (the queue's contract is that only a
  // non-faulting store may reach memory) and its completion is the fault. A
  // store that does not fault takes the queue exactly as before.
  //
  // `fence_block_younger_c` is the FENCE/FENCE.I rule's younger-access block
  // (I-037): while a fence is staged, no memory macro is handed to a queue, so a
  // load or store younger than the fence cannot allocate until the fence has
  // retired. The refusal is here, at the one port both queues are fed from, so
  // "younger than the fence" is the same predicate for both.
  //
  // `!rob_flush_pulse` is the same rule for a redirect, and it closes a window
  // the queues cannot close themselves: their squash removes the entries
  // resident when it is applied, not one *inserted in that same cycle*. A macro
  // at the dispatch head is always younger than the redirecting instruction, so
  // it is dead work either way -- refusing the insert here means the queue never
  // holds an entry whose ROB slot the flush has already freed. Without it a
  // store inserted on the flush edge survives, is never authorised (its ROB
  // entry is gone) and blocks the queue head for ever; CASE=fence.code_and_data_order's
  // FENCE.I redirect is what exposed it.
  // An AMO, LR or SC is also refused while the atomic issue record is occupied
  // (I-039/I-040): at most one atomic macro is resident in the load queue, so
  // the record that carries its operation, operand and class can be a single
  // entry. An ordinary load is not refused -- it simply queues behind the
  // atomic, which is exactly the ordering the atomic needs.
  assign disp_mem_is_atomic = disp_mem_is_amo || disp_mem_is_lr || disp_mem_is_sc;
  assign amo_alloc_c = disp_mem_valid && disp_mem_is_atomic && disp_mem_ready;
  assign disp_mem_ready = !fence_block_younger_c && !rob_flush_pulse &&
      (disp_mem_is_store
      ? (disp_store_faults ? (lsu_wb_ready && !lq_result_valid)
                           : (sq_alloc_ready && lsu_wb_ready && !lq_result_valid))
      : (lq_alloc_ready && (!disp_mem_is_atomic || !amo_busy)));

  assign lq_alloc_valid  = disp_mem_valid && !disp_mem_is_store && disp_mem_ready;
  assign sq_alloc_valid  = disp_mem_valid &&  disp_mem_is_store && !disp_store_faults &&
                           disp_mem_ready;
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
    store_wb_ev.is_store    = 1'b1;
    store_wb_ev.is_load     = 1'b0;
    // The completion of a faulting store carries the fault and nothing else, so
    // the ROB marks the entry exceptional through the same path a load's fault
    // uses and the store never retires.
    store_wb_ev.exc.valid   = disp_store_faults;
    store_wb_ev.exc.cause   = (disp_store_fault == STORE_MISALIGN)
                              ? mosaic_pkg::EXC_STORE_MISALIGNED
                              : mosaic_pkg::EXC_STORE_ACCESS;
    store_wb_ev.exc.tval    = disp_store_addr;
  end

  // A load's completion carries the merged, sign-extended value the load queue
  // produced and the destination the entry carried with it.
  always_comb begin
    lq_wb_ev.id          = lq_result_id;
    lq_wb_ev.dst.tag     = lq_result_dst_tag;
    lq_wb_ev.dst.gen     = {{(CORE_PGEN_W - CORE_IGEN_W){1'b0}}, lq_result_dst_gen};
    lq_wb_ev.dst.x0      = lq_result_dst_x0;
    lq_wb_ev.value_valid = !lq_result_dst_x0;
    // F/D (I-050): an flw's 32-bit result is NaN-boxed on the way into the
    // f-register -- the load itself zero-extends, and the upper half is set to
    // all ones here. The predicate was recorded at the load's memory insert
    // (word-sized FP load), so a double load and an integer load are untouched.
    lq_wb_ev.value       = fp_load_box_mem[lq_result_id.rob_index]
                           ? {32'hFFFF_FFFF, lq_result_data[31:0]}
                           : lq_result_data;
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
  // "A transaction is outstanding" now means the whole path, not only the
  // endpoint (I-038): a device access taken into the serializer's register is
  // outstanding before the endpoint accepts it, and the FENCE/FENCE.I rule above
  // uses the same conjunction.
  assign o_mem_lsu_busy       = ep_busy || dev_ser_busy;
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
