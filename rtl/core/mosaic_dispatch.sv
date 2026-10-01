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
// What this package still refuses is an invalid decode and, until a trap vector
// is installed, ECALL/EBREAK -- the two system instructions whose whole
// architectural effect is to trap. Dispatching a macro the machine cannot
// complete would leave it in the ROB for ever, so the refusal happens **before
// the group is presented to rename**: nothing is allocated, nothing is leaked,
// and the machine stops cleanly at that instruction instead of executing
// something wrong. That ordering is the whole reason the check sits here rather
// than at the issue queue: rename has already allocated by the time the issue
// queue could have refused.
//
// Loads and stores used to be on that list. They are not any more: the memory
// path (I-033..I-038) is integrated, and a memory macro leaves through the
// dedicated insert port below instead of through a cluster. It cannot hang,
// because the LSU always completes a load and a store is complete the moment its
// operands are -- see mosaic_core.sv.
//
// CSR and system macros were on that list too, and are not any more: I-019's CSR
// file and I-020's interrupt decision are integrated, and a system macro leaves
// through its own insert port, to be resolved by the core's system unit when it
// reaches the ROB head. See the system insert port's note for why that boundary
// and not an execution unit.
//
// FENCE and FENCE.I were the last entry on that list and are not any more
// (I-037). They leave through the same system insert port: the ordering a fence
// enforces is a property of the memory path and the fetch front end, neither of
// which a cluster can see, so the core's system unit resolves them at the ROB
// head exactly as it resolves a CSR access.
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
//     reports ready with value zero;
//   * a source that resolves to the **architectural initial mapping** -- a
//     register the program has not written since reset -- is folded to a ready
//     constant zero by the same mechanism. Waiting for its writeback is the
//     deadlock CASE=core.unwritten_reg_read reproduces: the initial mapping's
//     producer does not exist, so the wakeup the issue queue would wait for is
//     never broadcast. The recognition is `!rsN_is_x0 && !gen_valid[rsN_tag]`,
//     exact and not a guess: rename's `gen_valid[tag]` is set by an allocation
//     and cleared only by the undo of that same allocation, so a tag with it
//     clear is a tag no allocation has ever taken -- which is what an initial
//     mapping is. A `(tag, generation)` test would *not* do: the first
//     allocation of a tag also carries generation 0, and a register a real
//     producer wrote must take the ordinary ready/wakeup path rather than be
//     shadowed by the constant. (Two negative controls break one half each:
//     `MOSAIC_DISPATCH_MUTANT_NO_INIT_CONST` removes the fold, and
//     `MOSAIC_DISPATCH_MUTANT_ALL_INIT_CONST` applies it to every source.)
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
localparam int unsigned DSP_ENTRIES = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
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
    // rename's per-tag allocation validity: `gen_valid[tag]` is high once an
    // allocation has taken that tag since reset. The architectural initial
    // mapping is recognised with `!gen_valid[tag]` -- see the operands section of
    // the header for why this bit and not `(tag, generation)` is the exact test.
    // rename exposes it on its `dbg_gen_valid` read-out.
    input  logic [DSP_ENTRIES-1:0]      gen_valid,

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
    // "This macro is a store", so the ROB's commit path can name the store it
    // is retiring and the store queue can authorise exactly that entry.
    output logic                        desc_wr_is_store,

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

    // ---------------------------------------------- memory insert (LQ / SQ)
    // A load or store macro leaves dispatch here instead of entering a cluster:
    // its address operands are already in this module's own operand pipeline
    // (base in slot 1, the store payload in slot 2, the offset in the decoded
    // immediate), the address adder lives in the LSU, and neither queue has an
    // execution unit to issue into. So the macro is *allocated* into its queue
    // in the same cycle its operands become readable, exactly as an ALU macro is
    // inserted into a cluster, and the memory path's completion is what makes it
    // ready to retire.
    //
    // The handshake is the same transport rule the cluster insert uses: `valid`
    // with the payload held stable until `ready`. `mem_ins_ready_i` is the
    // core's composition of the target queue's room and, for a store, the
    // writeback path's willingness to take its completion in this very cycle --
    // a store owns a cluster-style completion at allocation, and it must be
    // taken there, because the store has no other completion to offer later.
    output logic                        mem_ins_valid,
    input  logic                        mem_ins_ready,
    output logic                        mem_ins_is_store,
    output logic [DSP_UOP_ID_W-1:0]     mem_ins_id,
    output logic [DSP_XLEN-1:0]         mem_ins_base,
    output logic [DSP_XLEN-1:0]         mem_ins_imm,
    output logic [2:0]                  mem_ins_size,
    output logic                        mem_ins_signed,
    output logic [DSP_XLEN-1:0]         mem_ins_data,
    output logic [DSP_TAG_W-1:0]        mem_ins_dst_tag,
    output logic [DSP_IGEN_W-1:0]       mem_ins_dst_gen,
    output logic                        mem_ins_dst_x0,

    // ------------------------------------------------- system insert (I-019)
    // A CSR write/read, ECALL, EBREAK, MRET, WFI or FENCE/FENCE.I leaves
    // dispatch here instead of entering a cluster or a memory queue. It has no
    // execution unit: it is *resolved at the architectural boundary*, because a
    // CSR read must see the CSR state left by every older instruction and a CSR
    // write must not be visible until its own instruction retires, and a fence
    // must not complete until every older memory access has. So the macro is
    // allocated into the ROB like any other and its payload is staged for the
    // core's system unit (mosaic_core.sv section 10a), which acts when the macro
    // reaches the ROB head.
    //
    // A fence is on this port rather than on a cluster because the ordering it
    // enforces is a property of the whole memory path (the queues and the
    // endpoint) and of the fetch front end for FENCE.I, none of which a cluster
    // can see. The core's system unit applies the drain-and-block rule and, for
    // FENCE.I, the front-end invalidation (I-037).
    //
    // The transport rule is the cluster's: `valid` with the payload held stable
    // until `ready`. The operand is the CSR instruction's write operand -- the
    // rs1 register value for the register forms and the zero-extended 5-bit
    // `zimm` for the immediate forms -- and it is captured here for the same
    // reason a store's operands are: there is no issue queue behind this macro
    // to deliver a later wakeup, so a source that is not yet written has no
    // value to capture and the insert waits.
    output logic                        sys_ins_valid,
    input  logic                        sys_ins_ready,
    output logic [DSP_UOP_ID_W-1:0]     sys_ins_id,
    output logic [11:0]                 sys_ins_csr_addr,
    output logic [1:0]                  sys_ins_csr_op,
    output logic                        sys_ins_csr_reads,
    output logic                        sys_ins_csr_writes,
    output logic                        sys_ins_is_ecall,
    output logic                        sys_ins_is_ebreak,
    output logic                        sys_ins_is_mret,
    output logic                        sys_ins_is_wfi,
    // FENCE and FENCE.I are the other two macros the system unit resolves; they
    // carry no operand the core uses (the ISA defines fence's fields as
    // ordering hints this profile does not interpret) so the two class bits are
    // the whole payload beyond the identity.
    output logic                        sys_ins_is_fence,
    output logic                        sys_ins_is_fence_i,
    output logic [DSP_XLEN-1:0]         sys_ins_src1_val,
    output logic [DSP_TAG_W-1:0]        sys_ins_dst_tag,
    output logic [DSP_IGEN_W-1:0]       sys_ins_dst_gen,
    output logic                        sys_ins_dst_x0,

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
    // "Software has installed a trap vector" (mtvec is no longer at its reset
    // value). Until it has, an instruction whose whole architectural effect is
    // "take a trap" is refused rather than taken: the p0 reset value of mtvec is
    // 0, and a machine that took an ECALL before any handler was installed would
    // vector into unprogrammed memory instead of stopping where the operator can
    // see it. Once a vector is installed -- which every real program does before
    // it can trap -- ECALL and EBREAK are dispatched and trapped normally. See
    // the refusal rule below.
    input  logic                        trap_vector_armed_i,
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
    // The system payload, carried exactly as the decode produced it, because the
    // system unit resolves the macro from this packet and not by re-decoding.
    logic                    sys;
    logic [11:0]             sys_csr_addr;
    mosaic_pkg::csr_op_e     sys_csr_op;
    logic                    sys_csr_reads;
    logic                    sys_csr_writes;
    logic                    sys_ecall;
    logic                    sys_ebreak;
    logic                    sys_mret;
    logic                    sys_wfi;
    // FENCE / FENCE.I are resolved by the system unit too (I-037); the class
    // bits are all the payload the core reads, because this profile treats the
    // fm/pred/succ fields conservatively and ignores them.
    logic                    sys_fence;
    logic                    sys_fence_i;
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
  // "This macro's first operand is the CSR immediate form's zimm, not a
  // register": decided from lane 0's decode at the head, and used both when the
  // entry is built and when its operand is selected.
  logic                 sys_imm_form;
  logic                 head_is_sys;
  logic                 sys_ins_offer;
  logic [DSP_XLEN-1:0]  sys_src1_val;
  // The initial-mapping fold stored in a queue entry: the shipping build passes
  // rename's flag through, and the two negative controls replace it with one
  // half of the wrong rule each. See the operands section of the header.
  logic                 s1_init_fold, s2_init_fold;
  logic [1:0][1:0]      bank_of_src;
  logic                 s1_present, s2_present;
  logic                 s1_value_ok, s2_value_ok, s1_conflict, s2_conflict;
  logic                 s1_bad, s2_bad;
  logic                 ins_ready_sel;
  logic                 ins_ok_cluster;
  logic                 s1_val_ready, s2_val_ready;
  logic                 head_is_mem;
  logic                 head_is_store;
  logic                 mem_ins_offer;
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

  logic l0_trap_unarmed;

  assign l0_trap_unarmed = dec_valid[0] && dec_ctl0.is_system &&
                           (dec_ctl0.is_ecall || dec_ctl0.is_ebreak) &&
                           !trap_vector_armed_i;

  always_comb begin
    l0_illegal     = dec_valid[0] && !dec_ctl0.valid;
    // Memory macros are services now: a load or a store is executed by the LSU
    // and allocated into its queue through the memory insert port below. A
    // CSR/system macro is a service too: it is allocated into the ROB and
    // resolved by the core's system unit at the architectural boundary (see the
    // system insert port's note). FENCE and FENCE.I are on that same port: the
    // core's system unit drains the memory path and, for FENCE.I, invalidates
    // this hart's instruction view (I-037). What remains refused is everything
    // the machine still has no path for: an invalid decode, and -- until a trap
    // vector is installed -- the two system instructions whose entire
    // architectural effect is to trap.
    l0_unsupported = dec_valid[0] && !dec_ctl0.valid || l0_trap_unarmed;
  end

  // --------------------------------------------------------------------------
  // Allocation
  // --------------------------------------------------------------------------
  assign queue_has_room = (q_cnt < DSP_CNT_W'(DSP_DEPTH));
`ifdef MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH
  // NEGATIVE CONTROL: the branch barrier does not hold a *memory* macro, so the
  // instructions after an unresolved branch are dispatched and their loads and
  // stores are allocated into the queues and issued to the endpoint before the
  // redirect discards them -- "a squashed access still reaching memory". The
  // store side is still safe (the entry never retires, so it is never
  // authorised and never writes), which is why an end-state comparison alone
  // would not catch this: CASE=core.mem_program's "exactly one data transaction
  // per load and per store reached the data port" is what names it.
  assign alloc_now      = dec_valid[0] && !l0_unsupported && !recovering && !stop_q &&
                          (!barrier ||
                           (dec_ctl0.mem_kind != mosaic_pkg::MEM_NONE)) &&
                          queue_has_room && rob_free_any;
`else
  assign alloc_now      = dec_valid[0] && !l0_unsupported && !recovering && !stop_q &&
                          !barrier && queue_has_room && rob_free_any;
`endif
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
  // Recorded for *every* allocated macro (not gated on `alloc_ok`) so a stale
  // slot cannot keep an old "is a store" bit that the retire path would read.
  assign desc_wr_is_store = (dec_ctl0.mem_kind == mosaic_pkg::MEM_STORE);

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
  //
  // The CSR *immediate* forms are the one exception to "uses_rs1 means read
  // rs1": there the 5-bit field is a zimm, and the decoder says so with
  // `csr_imm_form`. Reading it as a register index would make `csrrwi t0, mscratch,
  // 17` read x17, so the address is forced to x0 and the value is folded as a
  // ready constant below (the same slot the AUIPC PC uses).
  assign rs1_addr = (dec_ctl0.is_system && dec_ctl0.csr_imm_form) ? 5'd0
                    : (dec_ctl0.uses_rs1 ? dec_ctl0.rs1 : 5'd0);
  assign rs2_addr = dec_ctl0.uses_rs2 ? dec_ctl0.rs2 : 5'd0;

  assign sys_imm_form = dec_ctl0.is_system && dec_ctl0.csr_imm_form;

  assign rs1_tag_v = rs1_tag;
  assign rs2_tag_v = rs2_tag;
  assign rs1_gen_v = rs1_gen;
  assign rs2_gen_v = rs2_gen;

  // ------------------------------------------------- the initial-mapping fold
  // A source that resolves to the architectural initial mapping is a ready
  // constant zero: waiting for its wakeup is the deadlock this fold removes.
`ifdef MOSAIC_DISPATCH_MUTANT_NO_INIT_CONST
  // NEGATIVE CONTROL: the initial mapping is not folded, so a never-written
  // register is inserted not-ready and waits for a writeback that cannot
  // arrive. CASE=core.unwritten_reg_read must fail on it.
  assign s1_init_fold = 1'b0;
  assign s2_init_fold = 1'b0;
`elsif MOSAIC_DISPATCH_MUTANT_ALL_INIT_CONST
  // NEGATIVE CONTROL: *every* source is folded, so a register a real producer
  // wrote is shadowed by the constant zero and its consumer reads 0 instead of
  // the produced value. CASE=core.unwritten_reg_read must fail on it.
  assign s1_init_fold = !rs1_is_x0;
  assign s2_init_fold = !rs2_is_x0;
`else
  // The architectural initial mapping: a source that does not address x0 and
  // whose mapping is a tag no allocation has taken since reset. It has no
  // producer, so its architectural value -- zero -- is supplied as a ready
  // constant instead of waiting for a wakeup that cannot arrive. `gen_valid` is
  // set by allocation and cleared only by the undo of that same allocation, which
  // also rolls the mapping back, so this test is exact; see the header.
  assign s1_init_fold = !rs1_is_x0 && !gen_valid[rs1_tag];
  assign s2_init_fold = !rs2_is_x0 && !gen_valid[rs2_tag];
`endif

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
        q_mem[i].sys     <= 1'b0;
        q_mem[i].sys_fence   <= 1'b0;
        q_mem[i].sys_fence_i <= 1'b0;
      end
    end else if (recovering) begin
      // A redirect drops every macro from the ROB at and above the head, and
      // this queue holds only *allocated* macros -- all of them at or above the
      // head, because allocation is in program order and the head is the oldest
      // unretired entry. So a redirect empties this queue as well: a stale entry
      // left here would be offered to a completion path for a macro that no
      // longer exists, and for the system path -- which stages a macro until it
      // retires -- that entry could never leave. The queue is refilled by the
      // fetch of the redirect target, and dispatch allocates nothing while
      // `recovering` is high, so this can only ever discard stale entries.
      q_cnt <= {DSP_CNT_W{1'b0}};
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
        // The initial mapping is folded into the same ready-constant slot the
        // AUIPC PC uses. `s1_init_fold` and `is_auipc` are mutually exclusive
        // (AUIPC reads no rs1, so its rs1_addr is x0 and rename reports
        // `rs1_is_x0`, which clears the fold), so the constant is unambiguous.
        q_mem[push_at].s1_const <= dec_ctl0.is_auipc || s1_init_fold || sys_imm_form;
        q_mem[push_at].s1_cval  <= sys_imm_form ? {59'd0, dec_ctl0.rs1}
                                  : (s1_init_fold ? {DSP_XLEN{1'b0}} : dec_pc0);
        q_mem[push_at].s2_tag   <= rs2_is_x0 ? {DSP_TAG_W{1'b0}} : rs2_tag_v;
        q_mem[push_at].s2_gen   <= rs2_is_x0 ? {DSP_IGEN_W{1'b0}} : rs2_gen_v;
        q_mem[push_at].s2_x0    <= rs2_is_x0;
        // `alu_b = uses_rs2 ? rs2_val : imm`, as mosaic_bringup_core.sv states
        // it for the ISA reference.
        q_mem[push_at].s2_const <= !dec_ctl0.uses_rs2 || s2_init_fold;
        q_mem[push_at].s2_cval  <= s2_init_fold ? {DSP_XLEN{1'b0}} : dec_ctl0.imm;
        q_mem[push_at].sys          <= dec_ctl0.is_system || dec_ctl0.is_miscmem;
        q_mem[push_at].sys_csr_addr <= dec_ctl0.csr_addr;
        q_mem[push_at].sys_csr_op   <= dec_ctl0.csr_op;
        q_mem[push_at].sys_csr_reads <= dec_ctl0.csr_reads;
        q_mem[push_at].sys_csr_writes <= dec_ctl0.csr_writes;
        q_mem[push_at].sys_ecall    <= dec_ctl0.is_ecall;
        q_mem[push_at].sys_ebreak   <= dec_ctl0.is_ebreak;
        q_mem[push_at].sys_mret     <= dec_ctl0.is_mret;
        q_mem[push_at].sys_wfi      <= dec_ctl0.is_wfi;
        q_mem[push_at].sys_fence    <= dec_ctl0.is_miscmem && !dec_ctl0.is_fence_i;
        q_mem[push_at].sys_fence_i  <= dec_ctl0.is_fence_i;
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

  // -------------------------------------------------------- operand fetch
  // The register file is banked, and two different architectural registers can
  // share a bank. When they do, one read port has to serve both, so the pair
  // cannot be fetched in one cycle: the first source is read into `hold_val`
  // and the second is read the cycle after, with the held value standing in for
  // the first. `src_conflict` is what puts the pair into that two-cycle fetch
  // and `hold_valid` says the held value belongs to the head being presented.
  //
  // The form this replaces read the *second* source out of the *first* source's
  // bank data -- the same value for two different registers -- and accepted it,
  // because the value-ok test only asked whether that bank's response was
  // valid. It also reported "the second source is re-offered next cycle", which
  // could never happen: slot 0 wins a conflict unconditionally, so slot 1 was
  // never read at all, and a pair sharing a bank compared equal for ever.
  // CASE=core.corpus_branch found it: `beq a0,a1` with
  // a0 = 0x8000000000000000 and a1 = 0x7fffffffffffffff was resolved taken.
  logic                src_conflict;
  logic                hold_valid;
  logic [DSP_XLEN-1:0] hold_val;

  always_comb begin
    src_conflict = s1_needs_read && s2_needs_read &&
                   (bank_of_src[0] == bank_of_src[1]);
  end

  always_comb begin
    // First cycle: slot 0 owns the bank, slot 1 waits (and is counted as a
    // conflict). Second cycle: slot 1 is read and slot 0 is the held value.
`ifdef MOSAIC_DISPATCH_MUTANT_BANK_CONFLICT_VALUE
    // NEGATIVE CONTROL: the conflict is not serialised and the second source's
    // value is read from the first source's bank data, so a conflicting pair of
    // operands is always equal. CASE=core.corpus_branch must fail on it.
    s1_present = s1_needs_read;
    s2_present = s2_needs_read && (!s1_present || (bank_of_src[1] != bank_of_src[0]));
`else
    // First cycle of a conflict: slot 0 owns the bank, slot 1 waits. Second
    // cycle: slot 1 owns it and slot 0 is served by the held value.
    s1_present = s1_needs_read && (!src_conflict || !hold_valid);
    s2_present = s2_needs_read && (!src_conflict || hold_valid);
`endif
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      hold_valid <= 1'b0;
      hold_val   <= {DSP_XLEN{1'b0}};
    end else if (!src_conflict || head_fire) begin
      hold_valid <= 1'b0;
    end else begin
      hold_valid <= 1'b1;
      if (!hold_valid) begin
        hold_val <= prf_rsp_data[bank_of_src[0]*DSP_XLEN +: DSP_XLEN];
      end
    end
  end

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
    // A source that did not get the bank this cycle has no response of its own
    // to check: its value is either the held one (slot 0 in the second cycle of
    // a conflicting pair) or it is not available yet and the insert waits.
`ifdef MOSAIC_DISPATCH_MUTANT_BANK_CONFLICT_VALUE
    // NEGATIVE CONTROL: the value-ok test ignores whether the source actually
    // owns the bank this cycle, so the second operand silently becomes the
    // first. CASE=core.corpus_branch must fail on it.
    s1_value_ok = head.s1_x0 || head.s1_const || !rq_written[0] ||
                  (prf_rsp_valid[bank_of_src[0]] &&
                   !prf_rsp_gen_mismatch[bank_of_src[0]] &&
                   !prf_rsp_never_written[bank_of_src[0]]);
    s2_value_ok = head.s2_x0 || head.s2_const || !rq_written[1] ||
                  (prf_rsp_valid[bank_of_src[1]] &&
                   !prf_rsp_gen_mismatch[bank_of_src[1]] &&
                   !prf_rsp_never_written[bank_of_src[1]]);
`else
    s1_value_ok = head.s1_x0 || head.s1_const || !rq_written[0] ||
                  (src_conflict && hold_valid) ||
                  (s1_present && prf_rsp_valid[bank_of_src[0]] &&
                   !prf_rsp_gen_mismatch[bank_of_src[0]] &&
                   !prf_rsp_never_written[bank_of_src[0]]);
    s2_value_ok = head.s2_x0 || head.s2_const || !rq_written[1] ||
                  (s2_present && prf_rsp_valid[bank_of_src[1]] &&
                   !prf_rsp_gen_mismatch[bank_of_src[1]] &&
                   !prf_rsp_never_written[bank_of_src[1]]);
`endif
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

  // A memory macro does not enter a cluster: it is allocated into the load or
  // store queue. So the two insert paths have their own readiness and their own
  // target, and `head_fire` -- the one event that pops the head, and therefore
  // the one event that keeps allocation order equal to program order -- is
  // either of them.
  assign head_is_mem = (head.meta.class_ == mosaic_uop_pkg::UOP_LOAD) ||
                       (head.meta.class_ == mosaic_uop_pkg::UOP_STORE);
  assign head_is_store = (head.meta.class_ == mosaic_uop_pkg::UOP_STORE);
  assign head_is_sys = head.sys;

  // A memory macro *captures* its operands into its queue at insert: there is
  // no issue queue behind it to deliver a later wakeup, so an operand that is
  // not written yet has no value to capture and the insert must wait. That is a
  // stronger condition than the cluster path's, which deliberately inserts a
  // not-ready uop and lets the issue queue fill the value in:
  // `s1_value_ok`/`s2_value_ok` are *not* readiness -- they are satisfied by
  // `!rq_written`, precisely because the cluster path does not need the value.
  // `rq_written[k] && sN_value_ok` is the conjunction that means "this value is
  // the final value of that operand".
  //
  // Without this, a store whose base register was written in the previous cycle
  // is inserted with the register file's stale content and stores at the wrong
  // address -- which is exactly what CASE=core.corpus_branch caught when the
  // program's exit store used a TOHOST address materialised one instruction
  // earlier.
  assign s1_val_ready = head.s1_x0 || head.s1_const || (rq_written[0] && s1_value_ok);
  assign s2_val_ready = head.s2_x0 || head.s2_const || (rq_written[1] && s2_value_ok);

  assign ins_ok_cluster = head_valid && !recovering && !head_is_mem && !head_is_sys &&
                          s1_value_ok && s2_value_ok && ins_ready_sel;
  assign mem_ins_offer  = head_valid && !recovering && head_is_mem &&
                          s1_val_ready && s2_val_ready;
  assign mem_ins_valid  = mem_ins_offer;
  // The system macro captures its one operand for the same reason: it has no
  // issue queue behind it, and its CSR read happens at the architectural
  // boundary, where a source that is still in flight would already be too late.
  // The folded constant comes first, exactly as the cluster insert orders it:
  // for the CSR *immediate* forms the value is the zimm in the constant slot
  // while the address is x0, so testing `s1_x0` first would substitute zero for
  // every `csrrwi`/`csrrsi`/`csrrci` operand.
  assign sys_src1_val   = head.s1_const ? head.s1_cval
                          : (head.s1_x0 ? {DSP_XLEN{1'b0}} : s1_val_sel);
  assign sys_ins_offer  = head_valid && !recovering && head_is_sys &&
                          s1_val_ready;
  assign sys_ins_valid  = sys_ins_offer;
  assign head_fire      = ins_ok_cluster || (mem_ins_offer && mem_ins_ready) ||
                          (sys_ins_offer && sys_ins_ready);

  // --------------------------------------------------------------------------
  // Insert bus
  // --------------------------------------------------------------------------
  always_comb begin
    // During the second cycle of a conflicting pair slot 0 is not read from the
    // file; its value is the one latched when it was.
    s1_val_sel = hold_valid ? hold_val
                            : prf_rsp_data[bank_of_src[0]*DSP_XLEN +: DSP_XLEN];
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
    c0_ins_valid     = ins_ok_cluster && !head.cluster;
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

    c1_ins_valid     = ins_ok_cluster && head.cluster;
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
  // Memory insert bus
  // --------------------------------------------------------------------------
  // The address is presented as base and offset, not as a sum: the LSU owns the
  // one adder, so the address the strobes come from, the address the misalign
  // check tests and the address the fault reports cannot disagree. `head.imm` is
  // the decoded immediate for both classes; a load's slot 2 is the ready
  // constant it was folded into, a store's slot 2 is the payload.
  always_comb begin
    mem_ins_is_store = head_is_store;
    mem_ins_id       = head.id;
    mem_ins_base     = head.s1_x0 ? {DSP_XLEN{1'b0}}
                       : (head.s1_const ? head.s1_cval : s1_val_sel);
    mem_ins_imm      = head.imm;
    mem_ins_size     = head.meta.mem_size;
    mem_ins_signed   = head.meta.mem_signed;
    mem_ins_data     = head.s2_x0 ? {DSP_XLEN{1'b0}}
                       : (head.s2_const ? head.s2_cval : s2_val_sel);
    // A store writes no register, so its destination is x0 by construction
    // rather than by the decoder happening to say so.
    mem_ins_dst_x0   = head_is_store || head.dst_x0;
    mem_ins_dst_tag  = head_is_store ? {DSP_TAG_W{1'b0}} : head.dst_tag;
    mem_ins_dst_gen  = head_is_store ? {DSP_IGEN_W{1'b0}} : head.dst_gen;
  end

  // --------------------------------------------------------------------------
  // System insert bus
  // --------------------------------------------------------------------------
  // Everything the system unit needs to resolve this macro at the ROB head,
  // taken from the entry rather than re-decoded: the CSR address and operation,
  // the architectural-intent flags (does it read, does it write -- which are
  // not derivable from the opcode alone, because csrrw with rd == x0 reads
  // nothing and csrrs with rs1 == x0 writes nothing), the system instruction's
  // identity, the captured write operand and the destination identity.
  always_comb begin
    sys_ins_id         = head.id;
    sys_ins_csr_addr   = head.sys_csr_addr;
    sys_ins_csr_op     = head.sys_csr_op;
    sys_ins_csr_reads  = head.sys_csr_reads;
    sys_ins_csr_writes = head.sys_csr_writes;
    sys_ins_is_ecall   = head.sys_ecall;
    sys_ins_is_ebreak  = head.sys_ebreak;
    sys_ins_is_mret    = head.sys_mret;
    sys_ins_is_wfi     = head.sys_wfi;
    sys_ins_is_fence   = head.sys_fence;
    sys_ins_is_fence_i = head.sys_fence_i;
    sys_ins_src1_val   = sys_src1_val;
    sys_ins_dst_x0     = head.dst_x0;
    sys_ins_dst_tag    = head.dst_x0 ? {DSP_TAG_W{1'b0}} : head.dst_tag;
    sys_ins_dst_gen    = head.dst_x0 ? {DSP_IGEN_W{1'b0}} : head.dst_gen;
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
      if (head_valid && !recovering && !head_fire) stall_ctr <= stall_ctr + 32'd1;
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
