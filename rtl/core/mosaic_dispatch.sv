// ============================================================================
// mosaic_dispatch -- the front of the machine (work package I-023).
//
// One or two decoded macros per cycle are allocated -- a rename group, a ROB
// entry each -- and then inserted into their cluster's issue queue when that
// queue can take them. The two steps are decoupled, because the allocation is
// irrevocable (rename's free list, the ROB occupancy and the descriptor store
// all move) while the insert may have to wait for issue-queue space. The macros
// wait in a small dispatch queue whose depth is argued below.
//
// ------------------------------------------------------- two-wide allocation
//
// Up to two macros are allocated and renamed per cycle. The group is presented
// to `mosaic_rename` on its two-wide port (I-014) and to `mosaic_rob` on the
// second allocation port this package added, and both take the pair atomically:
// a group rename refuses is not offered to the ROB, and a group the ROB has no
// room for is not offered to rename. The pair is presented only when both
// decoded lanes are **cluster-class** macros (an ALU, branch, MUL/DIV or FP
// macro, all of which enter a cluster's issue queue) and neither is a control
// transfer.
//
// The same-cycle second **insert** is held: only the dispatch queue's head
// leaves per cycle, so the two allocated macros are inserted in program order,
// one per cycle. It is held because it triggers a retirement-order defect in
// `retire.width_and_order` (V-013) that the retire-side lane-1 path exposes;
// the classification experiment and the full evidence are in
// results/reports/held-insert.md. Two-wide allocation and the depth-8 decoded
// queue are kept -- neither breaks V-013, and the insert's own identity is
// sound (the classification says so). The diagnostics that used to bisect the
// insert are kept inert at the point where it lived.
//
// What is deliberately one-wide, and why:
//
//   * a **memory** macro (load/store/AMO/LR/SC) leaves through the single memory
//     insert port into the load or store queue. It captures its operands into
//     the queue at insert, so it needs a value the issue-queue path does not; a
//     second one in the same cycle would need a second operand-capture path and
//     a second queue port, neither of which exists.
//   * a **system or vector** macro is resolved at the architectural boundary and
//     is single-issue by construction (the vector engine is held to one macro in
//     flight for real, I-059).
//   * a **control transfer** allocates alone. The barrier that holds younger
//     work is one bit with one recovery point; two transfers in one group would
//     need two, and a machine that half-widened there would be worse than a
//     narrow one.
//
// A macro the machine has no path for (`l0_unsupported`) is still refused before
// anything is allocated, and still takes exactly the one lane-0 entry it always
// did.
//
// The two-cluster affinity alternates per macro, so successive macros are spread
// across the two clusters (or, under the dynamic strategy, the steering chooses
// instead). Each cluster accepts one insert per cycle; a macro whose unit is
// reached only through cluster 0's queue -- a MUL/DIV or FP macro -- goes there.
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
// The queue's depth cannot deadlock: insertion is strictly in allocation order,
// so a consumer is never in an issue queue before its producer. Every source of
// a not-ready entry therefore names a producer that is either already inserted or
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
// Depth 8: the depth the wider window wanted -- two allocations in flight per
// cycle, plus the insert latencies of two clusters, plus the few cycles a bank
// conflict on the operand read can add, plus the run-ahead the deeper
// instruction queue is there to create. With the same-cycle insert held the
// queue is deeper than it strictly needs, and it is kept: the depth half of the
// package is proven safe and is what it is for. The argument that the depth
// cannot deadlock is in the header.
localparam int unsigned DSP_DEPTH  = 8;
localparam int unsigned DSP_CNT_W  = $clog2(DSP_DEPTH + 1);
localparam int unsigned DSP_QW     = (DSP_DEPTH <= 1) ? 1 : $clog2(DSP_DEPTH);
// The reduced vector decode's own width, so a lane-1 entry can zero it without
// an assignment pattern (this project's RTL style forbids them).
localparam int unsigned DSP_VEC_W  = $bits(mosaic_pkg::vec_payload_t);

// The CSR/memory control bits of `decode_ctl_t` are not read here: this package
// services ALU, branch and MUL/DIV macros only, and a memory or system macro
// leaves through its own insert port (see the header). The two-wide decoded
// input is read in full: lane 1 is allocated and renamed on the second
// allocation port this package added to mosaic_rob.
/* verilator lint_off UNUSEDSIGNAL */
module mosaic_dispatch (
    input  logic                        clk,
    input  logic                        rst,

    // ------------------------------------------------- the fabric strategy
    // I-090. Low is the fixed baseline this core has always used: the first
    // macro of a fetched pair goes to cluster 0, the second to cluster 1, and
    // MUL/DIV and FP macros go to cluster 0 because that queue's grant is the
    // one routed to the shared units. High replaces that alternating toggle
    // with the I-029 locality/least-loaded/age policy (`mosaic_steering`),
    // which sees the same four routable targets (the two clusters and the two
    // shared units) and the same capacity matrix the core states below.
    //
    // The two strategies are the *same machine with one decision changed*,
    // which is what makes their comparison an experiment rather than two
    // designs. The strategy is a runtime input and not a parameter so a case
    // can run the same program, with the same seed, through both.
    input  logic                        fab_dyn,

    // ------------------------------------------------------ decoded macros in
    // Lane 0 is the older macro. Lane 1 is offered in the same cycle and is
    // allocated on the second allocation port in that same cycle when the pair
    // is cluster-class (see the header).
    input  logic [1:0]                  dec_valid,
    input  mosaic_pkg::decode_ctl_t     dec_ctl0,
    input  mosaic_pkg::decode_ctl_t     dec_ctl1,
    input  logic [DSP_XLEN-1:0]         dec_pc0,
    input  logic [DSP_XLEN-1:0]         dec_pc1,
    // I-041: the decoded instruction's own length in bytes (2 or 4) and its own
    // bits. The length is needed for the link value a compressed jump writes and
    // for the event record; the bits are needed for the event record. Both come
    // from the fetch unit through the decode buffer and neither is re-derived.
    input  logic [2:0]                  dec_len0,
    input  logic [31:0]                 dec_bits0,
    // Lane 1's own length and bits, for the same reason lane 0's are here: a
    // second macro allocated in the same cycle must carry its own instruction
    // identity into the descriptor store, or the retire event for it would
    // report lane 0's instruction.
    input  logic [2:0]                  dec_len1,
    input  logic [31:0]                 dec_bits1,

    // ----------------------------------------------------- rename allocation
    output logic                        alloc_req,
    output logic [4:0]                  alloc_rd,
    // F/D (I-050): the namespace each renamed operand lives in. `alloc_is_fp`
    // selects the FP destination map; `rs1_is_fp`/`rs2_is_fp` select the FP
    // source map. An instruction that does not use a source presents it in the
    // integer namespace, exactly as it presents an unused source as x0.
    output logic                        alloc_is_fp,
    output logic                        rs1_is_fp,
    output logic                        rs2_is_fp,
    input  logic                        alloc_accepted,
    input  logic                        alloc_exhausted,
    input  logic                        alloc_squashed,
    input  logic                        alloc_is_x0,
    input  logic                        alloc_new_valid,
    input  logic [DSP_TAG_W-1:0]        alloc_new_tag,
    input  logic [DSP_IGEN_W-1:0]       alloc_new_gen,

    // ------------------------------------------------- lane 1 rename (I-014)
    // The second allocation port. It is driven only when *both* decoded lanes
    // are cluster-class macros and the queue and the ROB each have room for two,
    // so the machine never presents a group rename must refuse half of. See the
    // "two-wide allocation" section below for what is deliberately one-wide.
    output logic                        alloc2_req,
    output logic [4:0]                  alloc2_rd,
    output logic                        alloc2_is_fp,
    input  logic                        alloc2_accepted,
    input  logic                        alloc2_exhausted,
    input  logic                        alloc2_squashed,
    input  logic                        alloc2_is_x0,
    input  logic                        alloc2_new_valid,
    input  logic [DSP_TAG_W-1:0]        alloc2_new_tag,
    input  logic [DSP_IGEN_W-1:0]       alloc2_new_gen,

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

    // ------------------------------------------------- lane 1 source reads
    // rename's second pair of source ports (rs3/rs4), for the second macro of a
    // two-wide group. `rs3_bypass`/`rs4_bypass` say the source names lane 0's
    // destination and rename has resolved it to lane 0's new (tag, generation);
    // the entry records that identity either way, so the flag is observation
    // only and the insert never needs to wait for lane 0's value.
    output logic [4:0]                  rs3_addr,
    output logic [4:0]                  rs4_addr,
    output logic                        rs3_is_fp,
    output logic                        rs4_is_fp,
    input  logic                        rs3_is_x0,
    input  logic                        rs4_is_x0,
    input  logic                        rs3_bypass,
    input  logic                        rs4_bypass,
    input  logic [DSP_TAG_W-1:0]        rs3_tag,
    input  logic [DSP_IGEN_W-1:0]       rs3_gen,
    input  logic [DSP_TAG_W-1:0]        rs4_tag,
    input  logic [DSP_IGEN_W-1:0]       rs4_gen,

    // ---------------------------------------------------------- ROB allocate
    // "the ROB has room for one more entry". A single bit, not the occupancy
    // count: the request is gated on it, and the ROB's own `alloc_ok` is the
    // answer that matters.
    input  logic                        rob_free_any,
    // "the ROB has room for two more entries". A two-wide group is presented to
    // the ROB only when this is high, so the ROB's own second-lane refusal is a
    // resource statement the front end acts on *before* rename allocates: a pair
    // whose second entry the ROB could not take would be a renamed instruction
    // with no ROB slot. The second lane is refused, not dropped -- it stays in
    // the queue and is the next cycle's lane 0.
    input  logic                        rob_free_two,
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

    // ------------------------------------------------- lane 1 ROB allocate
    // The ROB's second allocation port (added by this package). It is offered
    // only in a cycle `rob_free_two` says there is room, so `rob_alloc2_ok` is
    // the ROB's acknowledgement and never a surprise refusal.
    output logic                        rob_alloc2_valid,
    output logic [DSP_TAG_W-1:0]        rob_alloc2_tag,
    output logic [DSP_XLEN-1:0]         rob_alloc2_pc,
    output logic [3:0]                  rob_alloc2_num_uops,
    output logic                        rob_alloc2_exc,
    output logic                        rob_alloc2_open,
    input  logic                        rob_alloc2_ok,
    input  logic                        rob_alloc2_refused,
    input  logic [DSP_IDX_W-1:0]        rob_alloc2_index,
    input  logic [DSP_RGEN_W-1:0]       rob_alloc2_gen,

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
    // I-041: the retire record's instruction identity -- the instruction's own
    // length and its own bits -- written with the rest of the descriptor so the
    // event stream can report what retired rather than only where.
    output logic [2:0]                  desc_wr_len,
    output logic [31:0]                 desc_wr_insn,
    // Lane 1's descriptor write. `mosaic_macro_desc` already carries two write
    // lanes (they were added with the two-wide retire lane); this package is what
    // first drives the second one.
    output logic                        desc_wr2_valid,
    output logic [DSP_IDX_W-1:0]        desc_wr2_index,
    output logic [DSP_TAG_W-1:0]        desc_wr2_tag,
    output logic [DSP_PGEN_W-1:0]       desc_wr2_gen,
    output logic [4:0]                  desc_wr2_rd,
    output logic                        desc_wr2_reg_we,
    output logic                        desc_wr2_is_store,
    output logic [2:0]                  desc_wr2_len,
    output logic [31:0]                 desc_wr2_insn,

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
    // F/D (I-050): this memory macro's register operand is an f-register (an FP
    // load), so the core NaN-boxes a 32-bit result on the way back.
    output logic                        mem_ins_is_fp,
    output logic [DSP_UOP_ID_W-1:0]     mem_ins_id,
    output logic [DSP_XLEN-1:0]         mem_ins_base,
    output logic [DSP_XLEN-1:0]         mem_ins_imm,
    output logic [2:0]                  mem_ins_size,
    output logic                        mem_ins_signed,
    // A extension (I-039): the macro is an atomic read-modify-write. The fields
    // travel with the memory insert bus exactly as the rest of the memory
    // metadata does, so the integration does not re-decode the instruction.
    output logic                        mem_ins_is_amo,
    output mosaic_pkg::amo_op_e         mem_ins_amo_op,
    output logic                        mem_ins_amo_aq,
    output logic                        mem_ins_amo_rl,
    // LR/SC (I-040). The class travels with the memory insert bus for the same
    // reason the AMO's does: the integration recognises the macro from what
    // dispatch says, not by re-decoding the instruction.
    output logic                        mem_ins_is_lr,
    output logic                        mem_ins_is_sc,
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
    output logic                        sys_ins_is_sret,
    output logic                        sys_ins_is_wfi,
    output logic                        sys_ins_is_fetch_fault,
    // FENCE and FENCE.I are the other two macros the system unit resolves; they
    // carry no operand the core uses (the ISA defines fence's fields as
    // ordering hints this profile does not interpret) so the two class bits are
    // the whole payload beyond the identity.
    output logic                        sys_ins_is_fence,
    output logic                        sys_ins_is_fence_i,
    // SFENCE.VMA (I-046): a system macro with two source operands -- rs1 is the
    // address, rs2 the ASID -- and the two `has` bits say which operand names a
    // dimension rather than "all". The system unit resolves it at the ROB head.
    output logic                        sys_ins_is_sfence_vma,
    output logic                        sys_ins_sfence_has_va,
    output logic                        sys_ins_sfence_has_asid,
    // V (I-059): a vector macro leaves through this same port, because it is
    // resolved at the same boundary -- the ROB head -- for the same reason a CSR
    // instruction is: its configuration (vtype/vl) must be the one left by every
    // older instruction, and its element progress must not be visible until it
    // retires. `sys_ins_is_vec` distinguishes it from the scalar system macros;
    // `sys_ins_vec` is the reduced decode the core's vector engine consumes.
    output logic                        sys_ins_is_vec,
    output mosaic_pkg::vec_payload_t    sys_ins_vec,
    output logic [DSP_XLEN-1:0]         sys_ins_src1_val,
    output logic [DSP_XLEN-1:0]         sys_ins_src2_val,
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

    // ------------------------------------------------ the fabric's own state
    // I-090: the two clusters' issue-queue occupancy, which the dynamic policy
    // reads as the least-loaded key. They come straight from the clusters'
    // `o_count` ports; dispatch does not compute a second occupancy of its
    // own, because a policy that guesses the machine's load is not measuring
    // anything.
    input  logic [31:0]                 c0_count_i,
    input  logic [31:0]                 c1_count_i,
    // The cluster the allocating macro was pinned to, exposed so the core can
    // drive rename's I-032 bank preference from the same decision (producer
    // locality: the destination is biased to the producing cluster's home
    // bank). It is the *allocation-time* affinity; the insert-time dynamic
    // choice is the steering's and is not visible here.
    output logic                        o_target_cluster,

    // ------------------------------------------------------------ control
    input  logic                        recovering,
    input  logic                        barrier,
    // V (I-059): a vector macro is in flight (allocated but not yet retired).
    // Dispatch allocates nothing younger while this is high, so the macro
    // reaches the ROB head with the machine behind it empty. That is what makes
    // the vector engine's ownership of the data port safe (no younger load or
    // store can be outstanding) and makes a precise trap on the macro precise
    // with respect to everything younger, not merely everything older.
    input  logic                        vec_block_i,
    // An unresolved control transfer is in flight. Everything younger than it
    // may still be squashed by its redirect, so a macro this stage cannot
    // decode is *held* rather than refused while this is high; see the refusal
    // rule below. (This is narrower than `barrier`, which also covers a WFI
    // halt: a halt is not a transfer and its fall-through is architectural.)
    input  logic                        branch_in_flight,
    // "Software has installed a trap vector" (mtvec is no longer at its reset
    // value). Until it has, an instruction whose whole architectural effect is
    // "take a trap" is refused rather than taken: the p0 reset value of mtvec is
    // 0, and a machine that took an ECALL before any handler was installed would
    // vector into unprogrammed memory instead of stopping where the operator can
    // see it. Once a vector is installed -- which every real program does before
    // it can trap -- ECALL and EBREAK are dispatched and trapped normally. See
    // the refusal rule below.
    input  logic                        trap_vector_armed_i,
    // "Nothing older is still in flight": the ROB is empty, so the macro at the
    // decode input is the architectural next instruction and every architectural
    // state it is judged against -- mtvec, in particular -- is committed. This
    // is what makes the unarmed-trap refusal below a statement about the
    // architectural stream and not about how far the front end happens to have
    // run ahead. A deeper queue or a wider allocator must not be able to change
    // whether an instruction traps.
    input  logic                        rob_empty_i,
    output logic                        stop,           // unsupported macro seen
    // How many decoded macros left the input this cycle (0, 1 or 2). The
    // instruction queue pops on this, so a macro that was refused as unsupported
    // is consumed exactly once and a pair that both allocated is consumed in the
    // one cycle they did. It is never 2 with a refusal, and never 2 unless both
    // lanes were allocated: the two facts are the same decision.
    output logic [1:0]                  o_pop_count,

    // ------------------------------------------------------------ counters
    output logic [31:0]                 o_alloc_ctr,
    // Cycles in which the pair decision allocated two macros, and cycles in
    // which the second macro of a pair was inserted in the same cycle as the
    // first. These are what make "two-wide" a measurement rather than a claim:
    // a front end that never fires them is single-wide whatever its ports say.
    output logic [31:0]                 o_alloc2_ctr,
    output logic [31:0]                 o_pair_ctr,
    // The dispatch queue's occupancy summed over cycles, so the case can report
    // a mean depth rather than only a rate.
    output logic [31:0]                 o_occ_sum,
    output logic [31:0]                 o_pair_offer_ctr,
    output logic [31:0]                 o_l1_elig_ctr,
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
    output logic                        o_queue_full,

    // --------------------------------------------------- fabric observation
    // I-090: what the steering decided, so the case reports the decisions
    // rather than inferring them. `o_fab_unit_issues` is one 32-bit counter per
    // routable target (0/1 the two clusters, 2 the shared MUL/DIV+FP route, 3
    // the LSU), `o_fab_reason_ctr` is one per `grant_reason` value (0 none,
    // 1 fixed affinity, 2 capability alone, 3 locality, 4 load, 5 age), and the
    // three counters are the offers granted, stalled and rejected. They are
    // zero and static while `fab_dyn` is low.
    output logic [4*32-1:0]             o_fab_unit_issues,
    output logic [6*32-1:0]             o_fab_reason_ctr,
    output logic [31:0]                 o_fab_grant_ctr,
    output logic [31:0]                 o_fab_stall_ctr,
    output logic [31:0]                 o_fab_reject_ctr,
    output logic [31:0]                 o_fab_units,
    output logic [31:0]                 o_fab_classes,
    output logic [31:0]                 o_fab_age_w,
    output logic [31:0]                 o_fab_occ_w,
    output logic [31:0]                 o_fab_unit_w
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
    // The macro's program-order age, taken from the allocation counter when the
    // entry is created. It is the only age the dynamic steering's tie-break
    // compares (I-029), and it is meaningful only while the stream between two
    // live ages is below half its modulus (2^16), which every program this
    // machine runs is. A ring index would not do: it is not monotonic.
    logic [15:0]             age;
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
    logic                    sys_sret;
    logic                    sys_wfi;
    // A fetch the PMP unit refused (I-044): a system instruction whose whole
    // effect is an instruction access fault at its own PC.
    logic                    sys_fetch_fault;
    // FENCE / FENCE.I are resolved by the system unit too (I-037); the class
    // bits are all the payload the core reads, because this profile treats the
    // fm/pred/succ fields conservatively and ignores them.
    logic                    sys_fence;
    logic                    sys_fence_i;
    logic                    sys_sfence_vma;
    logic                    sys_sfence_has_va;
    logic                    sys_sfence_has_asid;
    // V (I-059): the macro is a vector macro, and the reduced decode the core's
    // vector engine consumes.
    logic                    vec;
    mosaic_pkg::vec_payload_t vec_pay;
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
  logic                 l0_refused;
  logic                 l0_illegal;
  logic                 alloc_now;
  logic                 alloc_ok;
  logic                 alloc2_ok;
  logic                 queue_has_room2;
  logic [DSP_CNT_W-1:0] queue_room_after_pop;
  // The lane class facts and the pair decision.
  logic                 l0_is_mem, l0_is_sys, l0_is_ctrl;
  logic                 l1_is_mem, l1_is_sys, l1_is_ctrl;
  logic                 l1_eligible, wide_pair;
  logic                 target_cluster, target_cluster1;
  // Lane 1's source identities, latched from rename's second read port pair.
  logic [DSP_TAG_W-1:0] rs3_tag_v, rs4_tag_v;
  logic [DSP_IGEN_W-1:0] rs3_gen_v, rs4_gen_v;
  logic                 s3_init_fold, s4_init_fold;
  mosaic_uop_pkg::uop_meta_t new_meta1;
  // The quantum of work dispatched this cycle.
  logic [DSP_CNT_W-1:0] pop_n;
  logic [DSP_QW-1:0]    push_at1;
  disp_ent_t            e0_new, e1_new;
  logic                 stop_q;
  logic                 aff_toggle;
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
  logic [DSP_XLEN-1:0]  sys_src2_val;
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

  logic [31:0] alloc_ctr, alloc2_ctr, pair_ctr, pair_offer_ctr, l1_elig_ctr, ins_ctr, occ_sum, unsup_ctr, illegal_ctr, exhausted_ctr, squashed_ctr;
  logic [31:0] stall_ctr, src_read_ctr, src_conflict_ctr, src_bad_ctr;
  logic [31:0] rob_full_ctr, queue_stall_ctr;

  // --------------------------------------------------------------------------
  // The fabric: the dynamic steering policy (I-090 wiring of I-029)
  // --------------------------------------------------------------------------
  // The router is one macro wide, exactly like the dispatch queue it sits in:
  // the head is the macro about to leave for a cluster or the memory path, and
  // the offer is made only in a cycle the macro could actually go (its operands
  // are resolved), so a grant is one macro routed and not one cycle the head
  // waited. The four units and the capability matrix are the machine's own: the
  // two clusters execute the integer ALU and branch classes, the shared MUL/DIV
  // and FP unit is reached through cluster 0's queue (so its "room" is that
  // queue's), and the LSU is reached through the memory insert port.
  logic             fab_steer_grant;
  /* verilator lint_off UNUSEDSIGNAL */
  // The module's per-cycle stall/reject flags. Their *counts* are the evidence
  // (`o_fab_stall_ctr`/`o_fab_reject_ctr` below); these two are the same facts
  // one cycle wide and nothing in the router needs them again, but the port
  // must be driven.
  logic             fab_steer_stall;
  logic             fab_steer_reject;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [1:0]       fab_steer_unit;
  logic [2:0]       fab_steer_reason;
  logic [4*32-1:0]  fab_unit_issues_steer;
  logic [31:0]      fab_steer_grant_ctr;
  logic [31:0]      fab_steer_stall_ctr;
  logic [31:0]      fab_steer_reject_ctr;
  logic [31:0]      fab_units, fab_classes, fab_age_w, fab_occ_w, fab_unit_w;

  logic [3:0]       fab_unit_room;
  logic [3:0][3:0]  fab_unit_occ;
  logic [3:0][6:0]  fab_unit_cap;      // one 7-bit class mask per unit
  logic [3:0]       fab_unit_locality;
  logic [3:0]       fab_unit_shared;
  logic [6:0][1:0]  fab_fixed_unit;    // one unit per class

  logic             head_steerable;
  logic [15:0]      head_age;
  logic             fab_cluster_eff;   // the cluster the head goes to this cycle
  logic             fab_route_lsu;     // the router granted the LSU this cycle
  logic             fab_route_clu;     // the router granted a cluster this cycle
  logic             head_goes_lsu;     // the head takes the memory insert path
  logic             head_goes_clu;     // the head takes a cluster insert path
  logic [5:0][31:0] fab_reason_hist;


  // --------------------------------------------------------------------------
  // Classification and the unsupported refusal
  // --------------------------------------------------------------------------
  assign stop = stop_q;
  // "This macro is refused and the machine stops at it." A macro the machine has
  // no path for is refused **only when it is the architectural next
  // instruction**. While a control transfer is still in flight, its redirect may
  // squash everything younger than it -- and it does: the decode buffer is
  // purged with the front end -- so a macro behind that transfer is held here
  // (neither taken, nor allocated, nor stopped) until the transfer has been
  // accounted for. Both of the other choices are wrong. Taking it would be the
  // hole this refusal exists to close: an undecodable instruction leaving the
  // machine silently. Stopping on it would halt the machine on an instruction
  // that never executes architecturally, which is exactly what a speculatively
  // fetched reserved encoding is -- the fetch of the padding behind an
  // unconditional jump delivers one, and the jump's own redirect is what has to
  // discard it (CASE=core.mem_program, and the fetch.redirect_late_response
  // contract for late responses behind a transfer).
  // The invalid-decode refusal is a property of the instruction alone, so it is
  // taken as soon as the macro is the oldest thing the front end can see (a
  // transfer in flight may still squash it). The unarmed-trap refusal is *not*:
  // it asks whether mtvec is zero, and mtvec is written at retirement, so the
  // question is only meaningful once every older macro has retired. Refusing it
  // earlier stops the machine on an instruction whose trap vector a still-
  // in-flight `csrw mtvec` was about to install -- which is exactly what a
  // front end that runs further ahead than the old two-entry buffer made
  // possible. `rob_empty_i` restores the rule the header states: refuse only
  // when it is the architectural next instruction.
  assign l0_refused = l0_unsupported && !branch_in_flight &&
                      (!l0_trap_unarmed || rob_empty_i);
  // The decode input is consumed exactly once per instruction. A pair that both
  // allocated is consumed in one cycle; a refusal consumes the one macro the
  // machine is stopping at; nothing else consumes anything.
`ifdef MOSAIC_DISPATCH_MUTANT_DROP_PAIR_TAIL
  // NEGATIVE CONTROL (two-wide front end): when lane 1 was eligible but was not
  // allocated -- the ROB or the dispatch queue was nearly full and *refused* the
  // group, which is the correct behaviour -- the decode input is popped by two
  // anyway. The second macro is consumed and never allocated: an instruction the
  // machine silently drops. The absolute checks on the `alu_burst` workload of
  // perf.equal_resource_compare (all 96 adds executed; retire PCs strictly
  // increasing) must fail.
  assign o_pop_count = {1'b0, alloc_ok} + {1'b0, alloc2_ok} + {1'b0, l0_refused} +
                       {1'b0, (alloc_now && l1_eligible && !alloc2_ok && !l0_refused)};
`else
  assign o_pop_count = {1'b0, alloc_ok} + {1'b0, alloc2_ok} + {1'b0, l0_refused};
`endif

  logic l0_trap_unarmed;

  assign l0_trap_unarmed = dec_valid[0] && dec_ctl0.is_system &&
                           (dec_ctl0.is_ecall || dec_ctl0.is_ebreak) &&
                           !trap_vector_armed_i;

  // ----------------------------------------------------------- lane classes
  // Which insert path each decoded lane needs. Only a cluster-class pair is
  // allocated two wide: a memory macro captures operands into a queue, a system
  // or vector macro is resolved at the architectural boundary, and a control
  // transfer allocates alone so the one in-flight-branch barrier stays one bit
  // (see the header).
  assign l0_is_mem  = dec_ctl0.mem_kind != mosaic_pkg::MEM_NONE;
  assign l0_is_sys  = dec_ctl0.is_system || dec_ctl0.is_miscmem || dec_ctl0.is_vec;
  assign l0_is_ctrl = dec_ctl0.is_branch || dec_ctl0.is_jal || dec_ctl0.is_jalr;
  assign l1_is_mem  = dec_ctl1.mem_kind != mosaic_pkg::MEM_NONE;
  assign l1_is_sys  = dec_ctl1.is_system || dec_ctl1.is_miscmem || dec_ctl1.is_vec;
  assign l1_is_ctrl = dec_ctl1.is_branch || dec_ctl1.is_jal || dec_ctl1.is_jalr;
  assign l1_eligible = dec_valid[1] && dec_ctl1.valid &&
                       !l1_is_mem && !l1_is_sys && !l1_is_ctrl;
  // The pair is offered to rename only when lane 0 is cluster-class too and is
  // not a control transfer, and only when both the dispatch queue and the ROB
  // can take two. `rob_free_two` is what makes the second lane a refusal rather
  // than a drop: the ROB's own answer arrives in the same cycle, before rename
  // has allocated anything.
`ifdef MOSAIC_DISPATCH_MUTANT_NO_PAIR
  // DIAGNOSTIC/CONTROL: two-wide *allocation* is disabled (the deeper
  // instruction queue stays). Used to attribute a behaviour change between the
  // two halves of this package -- the width and the depth. (The same-cycle
  // second insert is held unconditionally; see the header, so this control now
  // isolates the allocation half alone.)
  assign wide_pair = 1'b0;
`else
  assign wide_pair = alloc_now && l1_eligible && !l0_is_ctrl && !l0_is_mem &&
                     !l0_is_sys && queue_has_room2 && rob_free_two;
`endif

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
  // Two pushes need two free positions *after* this cycle's pops: the pair lands
  // in the slots the head (and, when it fires, the second entry) are leaving.
  // Counting the pop is what lets a pair allocate on the cycles a backed-up
  // queue is actually draining -- which, on the workload that exercises the
  // width, is most of the cycles the width could fire.
  assign queue_room_after_pop = q_cnt - pop_n;
  assign queue_has_room2 = (queue_room_after_pop <= DSP_CNT_W'(DSP_DEPTH - 2));
  assign queue_has_room  = (queue_room_after_pop < DSP_CNT_W'(DSP_DEPTH));
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
                          !barrier && !vec_block_i && queue_has_room && rob_free_any;
`endif
  assign alloc_ok       = alloc_now && alloc_accepted && rob_alloc_ok;
  // Lane 1 is allocated only when the pair decision said two, rename accepted
  // the group (which it does as a group: `alloc_accepted` and `alloc2_accepted`
  // are the same decision) and the ROB's second port took it.
  assign alloc2_ok      = wide_pair && alloc2_accepted && rob_alloc2_ok;

  assign alloc_req = alloc_now;
  assign alloc_rd  = dec_ctl0.rd;

  // ------------------------------------------------------ lane 1 allocation
  assign alloc2_req   = wide_pair;
  assign alloc2_rd    = dec_ctl1.rd;
  assign alloc2_is_fp = dec_ctl1.fp_dst_fp && dec_ctl1.reg_write;

  // Lane 1's four source addresses. A source the instruction does not use is
  // addressed as x0, exactly as lane 0's are, so rename reports it ready with
  // value zero and the queue entry needs no second rule.
  assign rs3_addr  = dec_ctl1.uses_rs1 ? dec_ctl1.rs1 : 5'd0;
  assign rs4_addr  = dec_ctl1.uses_rs2 ? dec_ctl1.rs2 : 5'd0;
  assign rs3_is_fp = dec_ctl1.uses_rs1 && dec_ctl1.fp_src1_fp;
  assign rs4_is_fp = dec_ctl1.uses_rs2 && dec_ctl1.fp_src2_fp;

  assign rob_alloc2_valid    = wide_pair && alloc2_accepted;
  assign rob_alloc2_tag      = alloc2_is_x0 ? {DSP_TAG_W{1'b0}} : alloc2_new_tag;
  assign rob_alloc2_pc       = dec_pc1;
  assign rob_alloc2_num_uops = 4'd1;
  assign rob_alloc2_exc      = 1'b0;
  assign rob_alloc2_open     = 1'b0;

  // F/D (I-050): which architectural map each renamed operand selects. A
  // destination is an f-register only for the FP instructions that write one
  // (arithmetic, fsgnj/minmax, fmv.w.x, fcvt int-to-fp, an FP load); an
  // FP-to-integer move, comparison, fclass or fcvt fp-to-int writes an integer
  // register and must allocate from the integer map. A source the instruction
  // does not use is presented in the integer namespace, so its address is x0 and
  // rename reports it ready with value zero.
  assign alloc_is_fp = dec_ctl0.fp_dst_fp && dec_ctl0.reg_write;
  assign rs1_is_fp   = dec_ctl0.uses_rs1 && dec_ctl0.fp_src1_fp;
  assign rs2_is_fp   = dec_ctl0.uses_rs2 && dec_ctl0.fp_src2_fp;

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
  // The instruction's own length and bits, from the front end, recorded with the
  // descriptor the retire event reads them back from.
  assign desc_wr_len  = dec_len0;
  assign desc_wr_insn = dec_bits0;

  // Lane 1's descriptor, written in the cycle it allocated. It is not gated on
  // `alloc2_ok` for the store/rd fields -- a stale slot must not keep an old
  // "is a store" bit -- but the store bit is a constant 0 for lane 1 by
  // construction, because a memory macro is never lane 1.
  assign desc_wr2_valid   = alloc2_ok;
  assign desc_wr2_index   = rob_alloc2_index;
  assign desc_wr2_tag     = alloc2_is_x0 ? {DSP_TAG_W{1'b0}} : alloc2_new_tag;
  assign desc_wr2_gen     = {{(DSP_PGEN_W - DSP_IGEN_W){1'b0}}, alloc2_new_gen};
  assign desc_wr2_rd      = dec_ctl1.rd;
  assign desc_wr2_reg_we  = alloc2_ok && alloc2_new_valid && dec_ctl1.reg_write &&
                            !alloc2_is_x0;
  assign desc_wr2_is_store = (dec_ctl1.mem_kind == mosaic_pkg::MEM_STORE);
  assign desc_wr2_len     = dec_len1;
  assign desc_wr2_insn    = dec_bits1;

  // --------------------------------------------------------------------------
  // The meta
  // --------------------------------------------------------------------------
  always_comb begin
    new_meta.class_ = mosaic_uop_pkg::UOP_ALU;
    if (dec_ctl0.is_fp) begin
      // F/D (I-050). An OP-FP macro is consumed by the shared floating-point
      // unit through cluster 0's FP request port, exactly as a UOP_MULDIV is
      // consumed by the shared iterative MUL/DIV unit.
      new_meta.class_ = mosaic_uop_pkg::UOP_FP;
    end else if (dec_ctl0.is_muldiv) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_MULDIV;
    end else if (dec_ctl0.mem_kind == mosaic_pkg::MEM_LOAD) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_LOAD;
    end else if (dec_ctl0.mem_kind == mosaic_pkg::MEM_AMO) begin
      // An AMO is issued through the load queue: it returns a value to a
      // destination register before retirement and, sitting at the queue head,
      // it blocks a younger load to the same address from overtaking it. It
      // must NOT be a store class: a store class completes at allocation and
      // drains after retirement, which would write memory before the atomic
      // read was even performed.
      new_meta.class_ = mosaic_uop_pkg::UOP_LOAD;
    end else if ((dec_ctl0.mem_kind == mosaic_pkg::MEM_LR) ||
                 (dec_ctl0.mem_kind == mosaic_pkg::MEM_SC)) begin
      // LR/SC (I-040) take exactly the AMO's route and for the same reasons: an
      // SC returns a *status* to rd and must never complete at allocation, and
      // an LR/SC must be serialized at the ROB head so a reservation is not
      // established on a path that can be squashed and a conditional write is
      // not performed speculatively.
      new_meta.class_ = mosaic_uop_pkg::UOP_LOAD;
    end else if (dec_ctl0.mem_kind == mosaic_pkg::MEM_STORE) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_STORE;
    end else if (dec_ctl0.is_branch || dec_ctl0.is_jal || dec_ctl0.is_jalr) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_BRANCH;
    end else if (dec_ctl0.is_system || dec_ctl0.is_miscmem) begin
      new_meta.class_ = mosaic_uop_pkg::UOP_SYSTEM;
    end else if (dec_ctl0.is_vec) begin
      // V (I-059). A vector macro is resolved at the architectural boundary like
      // a system macro -- the class is the same because the route is the same
      // (the system insert port below), not because the execution is: the core
      // resolves it with the vector engine.
      new_meta.class_ = mosaic_uop_pkg::UOP_SYSTEM;
    end
    new_meta.pc          = dec_pc0;
    new_meta.insn_len    = dec_len0;
    new_meta.alu_op      = dec_ctl0.alu_op;
    new_meta.md_op       = dec_ctl0.md_op;
    new_meta.md_w        = dec_ctl0.md_w;
    new_meta.br_funct    = dec_ctl0.branch_funct;
    new_meta.is_jal      = dec_ctl0.is_jal;
    new_meta.is_jalr     = dec_ctl0.is_jalr;
    new_meta.writes_link = dec_ctl0.writes_link;
    new_meta.mem_size    = dec_ctl0.mem_size;
    new_meta.mem_signed  = dec_ctl0.mem_signed;
    new_meta.is_amo      = (dec_ctl0.mem_kind == mosaic_pkg::MEM_AMO);
    new_meta.amo_op      = dec_ctl0.amo_op;
    new_meta.amo_aq      = dec_ctl0.amo_aq;
    new_meta.amo_rl      = dec_ctl0.amo_rl;
    new_meta.is_lr       = (dec_ctl0.mem_kind == mosaic_pkg::MEM_LR);
    new_meta.is_sc       = (dec_ctl0.mem_kind == mosaic_pkg::MEM_SC);
    new_meta.is_fence    = dec_ctl0.is_miscmem && !dec_ctl0.is_fence_i;
    new_meta.is_fence_i  = dec_ctl0.is_fence_i;
    // F/D (I-050). The FP unit needs the operation, the format, the rm field and
    // the namespace of each operand (for NaN-boxing); all four ride in the meta
    // so the issue queue carries them to the grant without a second decode.
    new_meta.fp_op       = dec_ctl0.fp_op;
    new_meta.fp_fmt      = dec_ctl0.fp_fmt;
    new_meta.fp_rm       = dec_ctl0.fp_rm;
    new_meta.fp_dst_fp   = dec_ctl0.fp_dst_fp;
    new_meta.fp_src1_fp  = dec_ctl0.fp_src1_fp;
    new_meta.fp_src2_fp  = dec_ctl0.fp_src2_fp;
    new_meta.fp_iw       = dec_ctl0.fp_iw;
    new_meta.fp_is       = dec_ctl0.fp_is;
  end

  // Lane 1's meta. It is built with the same rules from lane 1's own control
  // word: the two lanes are different instructions and must not be allowed to
  // share a decode. A memory or system class cannot reach here (the pair is only
  // offered for cluster-class lanes), but the mem/vec tests are kept so the
  // builder does not quietly assume it.
  always_comb begin
    new_meta1.class_ = mosaic_uop_pkg::UOP_ALU;
    if (dec_ctl1.is_fp) begin
      new_meta1.class_ = mosaic_uop_pkg::UOP_FP;
    end else if (dec_ctl1.is_muldiv) begin
      new_meta1.class_ = mosaic_uop_pkg::UOP_MULDIV;
    end else if (dec_ctl1.mem_kind != mosaic_pkg::MEM_NONE) begin
      new_meta1.class_ = (dec_ctl1.mem_kind == mosaic_pkg::MEM_STORE)
                         ? mosaic_uop_pkg::UOP_STORE : mosaic_uop_pkg::UOP_LOAD;
    end else if (dec_ctl1.is_branch || dec_ctl1.is_jal || dec_ctl1.is_jalr) begin
      new_meta1.class_ = mosaic_uop_pkg::UOP_BRANCH;
    end else if (dec_ctl1.is_system || dec_ctl1.is_miscmem || dec_ctl1.is_vec) begin
      new_meta1.class_ = mosaic_uop_pkg::UOP_SYSTEM;
    end
    new_meta1.pc          = dec_pc1;
    new_meta1.insn_len    = dec_len1;
    new_meta1.alu_op      = dec_ctl1.alu_op;
    new_meta1.md_op       = dec_ctl1.md_op;
    new_meta1.md_w        = dec_ctl1.md_w;
    new_meta1.br_funct    = dec_ctl1.branch_funct;
    new_meta1.is_jal      = dec_ctl1.is_jal;
    new_meta1.is_jalr     = dec_ctl1.is_jalr;
    new_meta1.writes_link = dec_ctl1.writes_link;
    new_meta1.mem_size    = dec_ctl1.mem_size;
    new_meta1.mem_signed  = dec_ctl1.mem_signed;
    new_meta1.is_amo      = (dec_ctl1.mem_kind == mosaic_pkg::MEM_AMO);
    new_meta1.amo_op      = dec_ctl1.amo_op;
    new_meta1.amo_aq      = dec_ctl1.amo_aq;
    new_meta1.amo_rl      = dec_ctl1.amo_rl;
    new_meta1.is_lr       = (dec_ctl1.mem_kind == mosaic_pkg::MEM_LR);
    new_meta1.is_sc       = (dec_ctl1.mem_kind == mosaic_pkg::MEM_SC);
    new_meta1.is_fence    = dec_ctl1.is_miscmem && !dec_ctl1.is_fence_i;
    new_meta1.is_fence_i  = dec_ctl1.is_fence_i;
    new_meta1.fp_op       = dec_ctl1.fp_op;
    new_meta1.fp_fmt      = dec_ctl1.fp_fmt;
    new_meta1.fp_rm       = dec_ctl1.fp_rm;
    new_meta1.fp_dst_fp   = dec_ctl1.fp_dst_fp;
    new_meta1.fp_src1_fp  = dec_ctl1.fp_src1_fp;
    new_meta1.fp_src2_fp  = dec_ctl1.fp_src2_fp;
    new_meta1.fp_iw       = dec_ctl1.fp_iw;
    new_meta1.fp_is       = dec_ctl1.fp_is;
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

  // Lane 1's source identity, taken from rename's second pair of ports. The
  // `*_bypass` flags are observation only: a bypassed source is reported by
  // rename as lane 0's new (tag, generation), which the entry records like any
  // other identity, and the issue queue's own wakeup fills the value when lane 0
  // writes back. `rs3_tag_v`/`rs4_tag_v` therefore already carry the bypass
  // result and the insert path does not need to know which it was.
  assign rs3_tag_v = rs3_tag;
  assign rs4_tag_v = rs4_tag;
  assign rs3_gen_v = rs3_gen;
  assign rs4_gen_v = rs4_gen;

  // Lane 1's initial-mapping fold: the same rule as lane 0's, applied to lane
  // 1's own mapping. It has to exist -- a lane-1 source that names a register
  // the program has not written is a source with no producer, and inserting it
  // not-ready would wait for a wakeup that can never arrive.
  // `rs3_bypass`/`rs4_bypass` suppresses the fold: a bypassed source is lane 0's
  // destination, whose tag was allocated *this cycle*, so `gen_valid` for it is
  // still clear at this edge and the fold would wrongly turn a real
  // same-cycle-dependent operand into a constant zero. The bypass is a real
  // producer; only a source with no producer at all is folded.
  assign s3_init_fold = !rs3_is_x0 && !rs3_bypass && !gen_valid[rs3_tag];
  assign s4_init_fold = !rs4_is_x0 && !rs4_bypass && !gen_valid[rs4_tag];

  // ------------------------------------------------------- cluster affinity
  // Fixed, deterministic, and stated rather than emergent: the first macro of a
  // fetched pair goes to cluster 0 and the second to cluster 1, and a MUL/DIV
  // macro always goes to cluster 0's queue because that queue's grant is the
  // one routed to the shared unit. An FP macro goes to cluster 0 for the same
  // reason: cluster 0's queue is the one whose grant is routed to the shared
  // floating-point unit (I-050).
  //
  // Lane 1 takes the *other* alternation slot, so a pair that both allocate
  // lands one macro in each cluster's queue. The toggle advances once per macro,
  // which is why the pair's two allocations cancel: lane 0 keeps this cycle's
  // value and lane 1 takes its complement, so the next pair starts where this
  // one did.
  always_comb begin
    target_cluster  = (dec_ctl0.is_muldiv || dec_ctl0.is_fp) ? 1'b0 : aff_toggle;
    target_cluster1 = (dec_ctl1.is_muldiv || dec_ctl1.is_fp) ? 1'b0 : ~aff_toggle;
  end

  assign o_target_cluster = target_cluster;

  // --------------------------------------------------------------------------
  // The dynamic route (I-029, wired by I-090)
  // --------------------------------------------------------------------------
  // The steering answers "which of the machine's routable targets takes this
  // macro". Its eligibility, capability and capacity inputs are the core's real
  // ones, and its answer is used only while `fab_dyn` is high: with it low the
  // policy is idle (`req_valid` is low) and the fixed affinity above is what
  // routes, so the baseline is bit-for-bit the pre-I-090 machine.
  // A memory macro is not offered to the router. The router replaces the
  // *cluster* affinity, and the LSU is not a cluster a macro can be routed to:
  // it is the single path every load and store takes. Offering it would also
  // close a combinational loop, because the core's memory-insert readiness
  // (`disp_mem_ready`) is a function of the offer it gates -- a store that
  // faults is ready by a different arm. The unit is still in the capability
  // matrix below, so the router's model of the machine is complete; it is just
  // never asked.
  assign head_steerable = head_valid && !head.sys && !head_is_mem;
  assign head_age       = head.age;

  always_comb begin
    // Room: "this unit can take one more macro this cycle". The shared units
    // are reached through cluster 0's queue, so their room is that queue's.
    fab_unit_room[0] = c0_ins_ready;
    fab_unit_room[1] = c1_ins_ready;
    fab_unit_room[2] = c0_ins_ready;
    // Constant: the LSU is never a candidate (see `head_steerable`), and a
    // constant here is what keeps the memory path out of the router's
    // combinational cone.
    fab_unit_room[3] = 1'b1;
  end

  always_comb begin
    // Occupancy: the least-loaded key. Slots 0 and 1 are the real issue queues;
    // 2 mirrors cluster 0 (its queue is what holds a MUL/DIV/FP macro) and 3 is
    // the LSU, which has no queue occupancy dispatch can see and is the only
    // unit eligible for its classes, so the key cannot discriminate there.
    fab_unit_occ[0] = c0_count_i[3:0];
    fab_unit_occ[1] = c1_count_i[3:0];
    fab_unit_occ[2] = c0_count_i[3:0];
    fab_unit_occ[3] = 4'd0;
  end

  always_comb begin
    // The capability matrix, stated once: bit c is set iff the unit may execute
    // class c. Class order is mosaic_uop_pkg::uop_class_e (ALU 0, BRANCH 1,
    // MULDIV 2, LOAD 3, STORE 4, SYSTEM 5, FP 6). No unit implements SYSTEM --
    // it is resolved at the architectural boundary, not in a functional unit --
    // and dispatch never offers one to the steering, so the reject path is
    // unreachable from this integration by construction.
    fab_unit_cap[0] = 7'b0000011;   // cluster 0: ALU, BRANCH
    fab_unit_cap[1] = 7'b0000011;   // cluster 1: ALU, BRANCH
    fab_unit_cap[2] = 7'b1000100;   // shared: MULDIV, FP
    fab_unit_cap[3] = 7'b0011000;   // LSU: LOAD, STORE
    fab_unit_locality = 4'b0010;    // unit 1 belongs to cluster 1
    fab_unit_shared   = 4'b1100;    // units 2 and 3 serve both localities
    // The fixed table is the module's own baseline; it is not used by this
    // integration (the fixed baseline here is the affinity toggle above), but
    // the port must be driven, and pinning each class to the unit that
    // implements it is the only meaningful table.
    fab_fixed_unit[0] = 2'd0;       // ALU
    fab_fixed_unit[1] = 2'd0;       // BRANCH
    fab_fixed_unit[2] = 2'd2;       // MULDIV
    fab_fixed_unit[3] = 2'd3;       // LOAD
    fab_fixed_unit[4] = 2'd3;       // STORE
    fab_fixed_unit[5] = 2'd0;       // SYSTEM (never offered)
    fab_fixed_unit[6] = 2'd2;       // FP
  end

  mosaic_steering #(
      .ST_N_UNITS   (4),
      // The class space is uop_class_e's full width (7); the module's default
      // of 6 predates the FP class (I-050) and would alias FP onto SYSTEM.
      .ST_N_CLASSES (7),
      .ST_AGE_W     (16),
      .ST_OCC_W     (4)
  ) u_steer (
      .clk            (clk),
      .rst            (rst),
      // The strategy is the module's: it is only consulted when this core's
      // strategy is dynamic, and then it is dynamic.
      .mode_dyn       (fab_dyn),
      .req_valid      (fab_dyn && head_steerable && !recovering),
      .req_class      (head.meta.class_),
      // Dispatch does not carry the producing cluster of a macro's operands --
      // rename hands back tags and generations, not provenance -- so no
      // locality preference is expressed here and the locality key
      // discriminates nothing in this integration. That is reported, not
      // hidden: it is the difference between the policy as written and the
      // policy as wired.
      .req_locality   (1'b0),
      .req_locality_en(1'b0),
      .req_age        (head_age),
      .unit_room      (fab_unit_room),
      .unit_occ       (fab_unit_occ),
      .unit_cap       (fab_unit_cap),
      .unit_locality  (fab_unit_locality),
      .unit_shared    (fab_unit_shared),
      .fixed_unit     (fab_fixed_unit),
      .grant_valid    (fab_steer_grant),
      .grant_unit     (fab_steer_unit),
      .grant_reason   (fab_steer_reason),
      .stall          (fab_steer_stall),
      .reject         (fab_steer_reject),
      .o_unit_issues  (fab_unit_issues_steer),
      .o_grant_ctr    (fab_steer_grant_ctr),
      .o_stall_ctr    (fab_steer_stall_ctr),
      .o_reject_ctr   (fab_steer_reject_ctr),
      .o_units        (fab_units),
      .o_classes      (fab_classes),
      .o_age_w        (fab_age_w),
      .o_unit_w       (fab_unit_w),
      .o_occ_w        (fab_occ_w)
  );

  assign o_fab_unit_issues = fab_unit_issues_steer;
  assign o_fab_reason_ctr  = fab_reason_hist;
  assign o_fab_grant_ctr   = fab_steer_grant_ctr;
  assign o_fab_stall_ctr   = fab_steer_stall_ctr;
  assign o_fab_reject_ctr  = fab_steer_reject_ctr;
  assign o_fab_units       = fab_units;
  assign o_fab_classes     = fab_classes;
  assign o_fab_age_w       = fab_age_w;
  assign o_fab_occ_w       = fab_occ_w;
  assign o_fab_unit_w      = fab_unit_w;

  // The insert decision. The router's answer, when it is consulted, *is* the
  // path decision: a grant to a cluster unit is a cluster insert, and a grant to
  // the LSU is the memory insert. With the capability matrix correct these agree
  // with the macro's class by construction -- dispatch never offers a memory
  // macro to the router, and the matrix never lets a cluster class reach the
  // LSU -- so the shipping machine is unchanged. With the matrix broken (the
  // `MOSAIC_STEER_MUTANT_IGNORE_CAPABILITY` control) an ALU macro is granted the
  // LSU and goes there, which is exactly "an incompatible unit receives work".
  // The alternative -- re-checking the class in dispatch -- would mask the
  // router's matrix, and a masked policy is not the one being measured.
  always_comb begin
    fab_cluster_eff = fab_dyn ? (fab_steer_unit == 2'd1) : head.cluster;
    fab_route_lsu   = fab_dyn && fab_steer_grant && (fab_steer_unit == 2'd3);
    fab_route_clu   = fab_dyn && fab_steer_grant && (fab_steer_unit != 2'd3);
  end

  assign head_goes_lsu = head_is_mem || fab_route_lsu;
  assign head_goes_clu = !head_is_mem && !head_is_sys &&
                         (!fab_dyn || fab_route_clu);

  // The reason histogram, so the case reports which key decided rather than
  // only where the macro went. Observational only.
  always_ff @(posedge clk) begin
    if (rst) begin
      fab_reason_hist <= {6{32'd0}};
    end else if (fab_dyn && fab_steer_grant && !head_is_mem && !head_is_sys) begin
      fab_reason_hist[fab_steer_reason[2:0]] <=
          fab_reason_hist[fab_steer_reason[2:0]] + 32'd1;
    end
  end

  // --------------------------------------------------------------------------
  // Queue next state
  // --------------------------------------------------------------------------
  // The head leaves, and the two newly allocated macros land at the tail the pop
  // left. `pop_n` counts the *insert* pop, which is what the shift is: pushing
  // is not a pop. (The same-cycle second insert is held; see the header.)
  assign pop_n     = DSP_CNT_W'(head_fire);
  assign push_at   = DSP_QW'(q_cnt - pop_n);
  assign push_at1  = DSP_QW'(q_cnt - pop_n + DSP_CNT_W'(1));

  always_comb begin
    q_cnt_next = q_cnt - pop_n;
    if (alloc_ok)  q_cnt_next = q_cnt_next + DSP_CNT_W'(1);
    if (alloc2_ok) q_cnt_next = q_cnt_next + DSP_CNT_W'(1);
  end

  // ---------------------------------------------------- the lane-0 entry build
  always_comb begin
    e0_new.id       = {rob_alloc_index, rob_alloc_gen, {DSP_UOP_W{1'b0}}};
    e0_new.age      = alloc_ctr[15:0];
    e0_new.meta     = new_meta;
    e0_new.imm      = dec_ctl0.imm;
    e0_new.cluster  = target_cluster;
    e0_new.dst_tag  = alloc_is_x0 ? {DSP_TAG_W{1'b0}} : alloc_new_tag;
    e0_new.dst_gen  = alloc_new_gen;
    e0_new.dst_x0   = alloc_is_x0;
    e0_new.s1_tag   = rs1_is_x0 ? {DSP_TAG_W{1'b0}} : rs1_tag_v;
    e0_new.s1_gen   = rs1_is_x0 ? {DSP_IGEN_W{1'b0}} : rs1_gen_v;
    e0_new.s1_x0    = rs1_is_x0;
    // The initial mapping is folded into the same ready-constant slot the AUIPC
    // PC uses. `s1_init_fold` and `is_auipc` are mutually exclusive (AUIPC reads
    // no rs1, so its rs1_addr is x0 and rename reports `rs1_is_x0`, which clears
    // the fold), so the constant is unambiguous.
    e0_new.s1_const = dec_ctl0.is_auipc || s1_init_fold || sys_imm_form;
    e0_new.s1_cval  = sys_imm_form ? {59'd0, dec_ctl0.rs1}
                      : (s1_init_fold ? {DSP_XLEN{1'b0}} : dec_pc0);
    e0_new.s2_tag   = rs2_is_x0 ? {DSP_TAG_W{1'b0}} : rs2_tag_v;
    e0_new.s2_gen   = rs2_is_x0 ? {DSP_IGEN_W{1'b0}} : rs2_gen_v;
    e0_new.s2_x0    = rs2_is_x0;
    // `alu_b = uses_rs2 ? rs2_val : imm`, as mosaic_bringup_core.sv states it for
    // the ISA reference.
    e0_new.s2_const = !dec_ctl0.uses_rs2 || s2_init_fold;
    e0_new.s2_cval  = s2_init_fold ? {DSP_XLEN{1'b0}} : dec_ctl0.imm;
    e0_new.sys          = dec_ctl0.is_system || dec_ctl0.is_miscmem;
    e0_new.sys_csr_addr = dec_ctl0.csr_addr;
    e0_new.sys_csr_op   = dec_ctl0.csr_op;
    e0_new.sys_csr_reads = dec_ctl0.csr_reads;
    e0_new.sys_csr_writes = dec_ctl0.csr_writes;
    e0_new.sys_ecall    = dec_ctl0.is_ecall;
    e0_new.sys_ebreak   = dec_ctl0.is_ebreak;
    e0_new.sys_mret     = dec_ctl0.is_mret;
    e0_new.sys_sret     = dec_ctl0.is_sret;
    e0_new.sys_wfi      = dec_ctl0.is_wfi;
    e0_new.sys_fetch_fault = dec_ctl0.is_fetch_fault;
    e0_new.sys_fence    = dec_ctl0.is_miscmem && !dec_ctl0.is_fence_i;
    e0_new.sys_fence_i  = dec_ctl0.is_fence_i;
    e0_new.sys_sfence_vma     = dec_ctl0.is_sfence_vma;
    e0_new.sys_sfence_has_va  = dec_ctl0.sfence_has_va;
    e0_new.sys_sfence_has_asid = dec_ctl0.sfence_has_asid;
    // V (I-059). The reduced decode, assembled once here from the control word
    // the front end produced, so the engine does not re-decode.
    e0_new.vec     = dec_ctl0.is_vec;
    e0_new.vec_pay.op_class      = dec_ctl0.vec_class;
    e0_new.vec_pay.kind          = dec_ctl0.vec_kind;
    e0_new.vec_pay.vset_kind     = dec_ctl0.vec_vset_kind;
    e0_new.vec_pay.vset_uimm     = dec_ctl0.vec_vset_uimm;
    e0_new.vec_pay.vtypei        = dec_ctl0.vec_vtypei;
    e0_new.vec_pay.vd            = dec_ctl0.vec_vd;
    e0_new.vec_pay.vs1           = dec_ctl0.vec_vs1;
    e0_new.vec_pay.vs2           = dec_ctl0.vec_vs2;
    e0_new.vec_pay.data          = dec_ctl0.vec_vd;
    e0_new.vec_pay.index         = dec_ctl0.vec_vs2;
    e0_new.vec_pay.family        = dec_ctl0.vec_family;
    e0_new.vec_pay.op            = dec_ctl0.vec_op;
    e0_new.vec_pay.form          = dec_ctl0.vec_form;
    e0_new.vec_pay.lsu_mode      = dec_ctl0.vec_lsu_mode;
    e0_new.vec_pay.idx_sew       = dec_ctl0.vec_idx_sew;
    e0_new.vec_pay.eew_sew       = dec_ctl0.vec_eew_sew;
    e0_new.vec_pay.nf            = dec_ctl0.vec_nf;
    e0_new.vec_pay.mask_en       = dec_ctl0.vec_mask_en;
    e0_new.vec_pay.lsu_we        = dec_ctl0.vec_lsu_we;
    e0_new.vec_pay.lsu_ordered   = dec_ctl0.vec_lsu_ordered;
    e0_new.vec_pay.lsu_fof       = dec_ctl0.vec_lsu_fof;
    e0_new.vec_pay.scalar_from_imm =
        (dec_ctl0.vec_form == 2'd2) || (dec_ctl0.vec_kind == 3'd0);
    e0_new.vec_pay.imm           = dec_ctl0.vec_imm;
  end

  // ---------------------------------------------------- the lane-1 entry build
  // Lane 1 is a cluster-class macro by construction, so its system and vector
  // payload fields are zero: no consumer can read them for a lane-1 entry, and
  // driving them from lane 0's decode would be exactly the kind of cross-lane
  // leak this package exists to avoid.
  always_comb begin
    e1_new.id       = {rob_alloc2_index, rob_alloc2_gen, {DSP_UOP_W{1'b0}}};
    // Strictly greater than lane 0's age in the same cycle, and strictly less
    // than the next cycle's: the age is the allocation counter before this
    // allocation's increment, plus one for the second macro of the pair.
    e1_new.age      = alloc_ctr[15:0] + 16'd1;
    e1_new.meta     = new_meta1;
    e1_new.imm      = dec_ctl1.imm;
    e1_new.cluster  = target_cluster1;
    e1_new.dst_tag  = alloc2_is_x0 ? {DSP_TAG_W{1'b0}} : alloc2_new_tag;
    e1_new.dst_gen  = alloc2_new_gen;
    e1_new.dst_x0   = alloc2_is_x0;
    e1_new.s1_tag   = rs3_is_x0 ? {DSP_TAG_W{1'b0}} : rs3_tag_v;
    e1_new.s1_gen   = rs3_is_x0 ? {DSP_IGEN_W{1'b0}} : rs3_gen_v;
    e1_new.s1_x0    = rs3_is_x0;
    e1_new.s1_const = dec_ctl1.is_auipc || s3_init_fold;
    e1_new.s1_cval  = s3_init_fold ? {DSP_XLEN{1'b0}} : dec_pc1;
    e1_new.s2_tag   = rs4_is_x0 ? {DSP_TAG_W{1'b0}} : rs4_tag_v;
    e1_new.s2_gen   = rs4_is_x0 ? {DSP_IGEN_W{1'b0}} : rs4_gen_v;
    e1_new.s2_x0    = rs4_is_x0;
    e1_new.s2_const = !dec_ctl1.uses_rs2 || s4_init_fold;
    e1_new.s2_cval  = s4_init_fold ? {DSP_XLEN{1'b0}} : dec_ctl1.imm;
    e1_new.sys      = 1'b0;
    e1_new.sys_csr_addr   = 12'd0;
    e1_new.sys_csr_op     = mosaic_pkg::CSR_NONE;
    e1_new.sys_csr_reads  = 1'b0;
    e1_new.sys_csr_writes = 1'b0;
    e1_new.sys_ecall      = 1'b0;
    e1_new.sys_ebreak     = 1'b0;
    e1_new.sys_mret       = 1'b0;
    e1_new.sys_sret       = 1'b0;
    e1_new.sys_wfi        = 1'b0;
    e1_new.sys_fetch_fault = 1'b0;
    e1_new.sys_fence      = 1'b0;
    e1_new.sys_fence_i    = 1'b0;
    e1_new.sys_sfence_vma     = 1'b0;
    e1_new.sys_sfence_has_va  = 1'b0;
    e1_new.sys_sfence_has_asid = 1'b0;
    e1_new.vec            = 1'b0;
    e1_new.vec_pay        = {DSP_VEC_W{1'b0}};
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
        q_mem[i].vec     <= 1'b0;
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
      // Shift down on a pop, then place the new entries at the tail.
      if (head_fire) begin
        for (int unsigned i = 0; i < DSP_DEPTH-1; i++) begin
          q_mem[i] <= q_mem[i+1];
        end
      end
      if (alloc_ok) begin
        q_mem[push_at] <= e0_new;
      end
      if (alloc2_ok) begin
        q_mem[push_at1] <= e1_new;
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

  // --------------------------------------------------------------------------
  // The same-cycle second insert is held
  // --------------------------------------------------------------------------
  // This package shipped a same-cycle second insert: the second entry of the
  // dispatch queue left for the other cluster in the same cycle as the head,
  // through a per-lane operand mux and a lane-1 best-effort PRF read. It is
  // held -- the code is deleted, not ifdef'd out -- because it triggers a
  // retirement-order defect in `retire.width_and_order` (V-013) that the
  // retire-side lane-1 path exposes. The classification experiment and the
  // full evidence are in results/reports/held-insert.md. Two-wide *allocation*
  // and the depth-8 decoded queue stay: neither breaks V-013.
  //
  // The diagnostics that used to bisect the insert are kept, inert, below, so
  // the isolation builds recorded in held-insert.md are still nameable. They
  // have no subject left: a build that names one is byte-identical to shipping.
`ifdef MOSAIC_DISPATCH_MUTANT_NO_INSERT
  // Inert: the same-cycle second insert is held unconditionally in this build.
`endif
`ifdef MOSAIC_DISPATCH_MUTANT_H1_NO_CTRL
  // Inert: the lane-1 insert this control restricted no longer exists.
`endif
`ifdef MOSAIC_DISPATCH_MUTANT_H1_ALWAYS_NOTREADY
  // Inert: the lane-1 operand read this control forced no longer exists.
`endif

  always_comb begin
    ins_ready_sel = fab_cluster_eff ? c1_ins_ready : c0_ins_ready;
  end

  // A memory macro does not enter a cluster: it is allocated into the load or
  // store queue. So the two insert paths have their own readiness and their own
  // target, and `head_fire` -- the one event that pops the head, and therefore
  // the one event that keeps allocation order equal to program order -- is
  // either of them.
  assign head_is_mem = (head.meta.class_ == mosaic_uop_pkg::UOP_LOAD) ||
                       (head.meta.class_ == mosaic_uop_pkg::UOP_STORE);
  assign head_is_store = (head.meta.class_ == mosaic_uop_pkg::UOP_STORE);
  assign head_is_sys = head.sys || head.vec;

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

  assign ins_ok_cluster = head_valid && !recovering && head_goes_clu &&
                          s1_value_ok && s2_value_ok && ins_ready_sel;
  assign mem_ins_offer  = head_valid && !recovering && head_goes_lsu &&
                          s1_val_ready && s2_val_ready;
  assign mem_ins_valid  = mem_ins_offer;
  // F/D (I-050): an FP load is a memory macro whose destination is an
  // f-register; `head.meta.fp_dst_fp` is set only for those (an FP arithmetic
  // macro is never presented on the memory insert bus). A store's register
  // operand is a source, so `mem_ins_is_fp` is about loads.
  assign mem_ins_is_fp  = head_is_mem && !head_is_store && head.meta.fp_dst_fp;
  // The system macro captures its one operand for the same reason: it has no
  // issue queue behind it, and its CSR read happens at the architectural
  // boundary, where a source that is still in flight would already be too late.
  // The folded constant comes first, exactly as the cluster insert orders it:
  // for the CSR *immediate* forms the value is the zimm in the constant slot
  // while the address is x0, so testing `s1_x0` first would substitute zero for
  // every `csrrwi`/`csrrsi`/`csrrci` operand.
  assign sys_src1_val   = head.s1_const ? head.s1_cval
                          : (head.s1_x0 ? {DSP_XLEN{1'b0}} : s1_val_sel);
  // SFENCE.VMA's second operand (the ASID). Read through the same path the
  // first is, so both operands are the architectural values at the head.
  assign sys_src2_val   = head.s2_const ? head.s2_cval
                          : (head.s2_x0 ? {DSP_XLEN{1'b0}} : s2_val_sel);
  assign sys_ins_offer  = head_valid && !recovering && head_is_sys &&
                          s1_val_ready && (s2_val_ready || !head.sys_sfence_vma);
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

  // The operand values the *cluster* insert presents (I-090). They are the
  // source reads above, except under `MOSAIC_FAB_MUTANT_DYN_SWAP_SRC`, which
  // crosses the two lanes while the fabric is on: the negative control for "a
  // dynamic route must never change what the program computes", since a two
  // register-operand ALU op then computes a different result in the dynamic
  // configuration only.
  logic [DSP_XLEN-1:0]  ins_s1_sel, ins_s2_sel;
`ifdef MOSAIC_FAB_MUTANT_DYN_SWAP_SRC
  assign ins_s1_sel = fab_dyn ? s2_val_sel : s1_val_sel;
  assign ins_s2_sel = fab_dyn ? s1_val_sel : s2_val_sel;
`else
  assign ins_s1_sel = s1_val_sel;
  assign ins_s2_sel = s2_val_sel;
`endif

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

  // The two cluster insert buses carry the head this cycle. Only one of them is
  // driven, by the head's affinity; a macro leaves for exactly one cluster.
  always_comb begin
    c0_ins_valid     = ins_ok_cluster && !fab_cluster_eff;
    c0_ins_uop       = ins_uop_v;
    c0_ins_meta      = ins_meta_v;
    c0_ins_imm       = ins_imm_v;
    c0_ins_src1_tag  = head.s1_tag;
    c0_ins_src1_gen  = head.s1_gen;
    c0_ins_src1_ready = head.s1_x0 || head.s1_const || rq_written[0];
    c0_ins_src1_val  = head.s1_const ? head.s1_cval
                                     : (head.s1_x0 ? {DSP_XLEN{1'b0}} : ins_s1_sel);
    c0_ins_src2_tag  = head.s2_tag;
    c0_ins_src2_gen  = head.s2_gen;
    c0_ins_src2_ready = head.s2_x0 || head.s2_const || rq_written[1];
    c0_ins_src2_val  = head.s2_const ? head.s2_cval
                                     : (head.s2_x0 ? {DSP_XLEN{1'b0}} : ins_s2_sel);
    c0_ins_dst_tag   = ins_dst_tag_v;
    c0_ins_dst_gen   = ins_dst_gen_v;

    c1_ins_valid     = ins_ok_cluster && fab_cluster_eff;
    c1_ins_uop       = ins_uop_v;
    c1_ins_meta      = ins_meta_v;
    c1_ins_imm       = ins_imm_v;
    c1_ins_src1_tag  = head.s1_tag;
    c1_ins_src1_gen  = head.s1_gen;
    c1_ins_src1_ready = head.s1_x0 || head.s1_const || rq_written[0];
    c1_ins_src1_val  = head.s1_const ? head.s1_cval
                                     : (head.s1_x0 ? {DSP_XLEN{1'b0}} : ins_s1_sel);
    c1_ins_src2_tag  = head.s2_tag;
    c1_ins_src2_gen  = head.s2_gen;
    c1_ins_src2_ready = head.s2_x0 || head.s2_const || rq_written[1];
    c1_ins_src2_val  = head.s2_const ? head.s2_cval
                                     : (head.s2_x0 ? {DSP_XLEN{1'b0}} : ins_s2_sel);
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
    mem_ins_is_amo   = head.meta.is_amo;
    mem_ins_amo_op   = head.meta.amo_op;
    mem_ins_amo_aq   = head.meta.amo_aq;
    mem_ins_amo_rl   = head.meta.amo_rl;
    mem_ins_is_lr    = head.meta.is_lr;
    mem_ins_is_sc    = head.meta.is_sc;
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
    sys_ins_is_sret    = head.sys_sret;
    sys_ins_is_wfi     = head.sys_wfi;
    sys_ins_is_fetch_fault = head.sys_fetch_fault;
    sys_ins_is_fence   = head.sys_fence;
    sys_ins_is_fence_i = head.sys_fence_i;
    sys_ins_is_sfence_vma    = head.sys_sfence_vma;
    sys_ins_sfence_has_va    = head.sys_sfence_has_va;
    sys_ins_sfence_has_asid  = head.sys_sfence_has_asid;
    sys_ins_src1_val   = sys_src1_val;
    sys_ins_src2_val   = sys_src2_val;
    sys_ins_is_vec     = head.vec;
    sys_ins_vec        = head.vec_pay;
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
    end else if (alloc_ok || alloc2_ok) begin
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
      // One flip per *macro*, so a pair's two allocations cancel and the next
      // pair starts on the same cluster as this one did. XOR-ing the two
      // allocation bits is that statement: 0 macros -> no flip, 1 -> flip,
      // 2 -> no flip.
      aff_toggle <= aff_toggle ^ (alloc_ok ^ alloc2_ok);
`endif
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      alloc_ctr        <= 32'd0;
      alloc2_ctr       <= 32'd0;
      // `pair_ctr` is reset and read out but never incremented: its increment
      // counted the same-cycle second insert, which is held (see the header and
      // results/reports/held-insert.md). It is structurally zero, not a
      // measurement of a working pair path.
      pair_ctr         <= 32'd0;
      occ_sum          <= 32'd0;
      pair_offer_ctr   <= 32'd0;
      l1_elig_ctr      <= 32'd0;
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
      // The allocation counter counts two macros in one cycle as two
      // allocations. The insert counter counts one: the same-cycle second
      // insert is held (see the header), so `pair_ctr` -- the cycles in which
      // two macros left the dispatch queue together -- stays zero.
      alloc_ctr <= alloc_ctr + {31'd0, alloc_ok} + {31'd0, alloc2_ok};
      ins_ctr   <= ins_ctr   + {31'd0, head_fire};
      if (alloc2_ok) alloc2_ctr <= alloc2_ctr + 32'd1;
      if (wide_pair) pair_offer_ctr <= pair_offer_ctr + 32'd1;
      if (l1_eligible) l1_elig_ctr <= l1_elig_ctr + 32'd1;
      occ_sum <= occ_sum + {{(32-DSP_CNT_W){1'b0}}, q_cnt};
      if (l0_refused) begin
        unsup_ctr <= unsup_ctr + 32'd1;
        stop_q    <= 1'b1;
      end
      // Counted where the refusal happens, not every cycle the macro waits at
      // the head: a held macro is not yet a refusal.
      if (l0_illegal && !branch_in_flight) illegal_ctr <= illegal_ctr + 32'd1;
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
  assign o_alloc2_ctr      = alloc2_ctr;
  // Read out for the performance harness, but structurally zero while the
  // same-cycle insert is held (see the reset block's note).
  assign o_pair_ctr        = pair_ctr;
  assign o_occ_sum         = occ_sum;
  assign o_pair_offer_ctr  = pair_offer_ctr;
  assign o_l1_elig_ctr     = l1_elig_ctr;
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
