// ============================================================================
// mosaic_csr -- work package I-019: the M-mode CSR file, and the trap / MRET
// boundary at which its side effects become architectural.
//
// ---------------------------------------------------------------- what it is
//
// One register file for exactly the CSR set config/csr/mode_m.json declares for
// this profile, plus the architectural actions a trap entry and an MRET perform
// on it. The module is the *only* place those registers live: no caller keeps a
// shadow copy of mstatus, mepc or the counters, because a second copy is a
// second answer waiting to happen.
//
// ------------------------------------------------------------------ geometry
//
// There is no number in this file: every CSR address, reset value, write mask
// and write-legality flag comes from `build/<profile>/rtl/mosaic_csr_pkg.svh`,
// which tools/gen_manifest.py decodes from the same implementation table
// tools/check_profile.py validates. A profile that changes a reset value or
// makes a field writable changes the generated package and this design together,
// and there is nothing here to edit in step.
//
// The field *positions* this module names (mstatus.MIE = bit 3, mstatus.MPIE =
// bit 7, mstatus.MPP = bits 12:11) are not configuration: they are the RV64
// layout the mstatus clause cites, and which of those bits software may change
// still comes from the generated mask.
//
// ------------------------------------------------------------ WARL decisions
//
// Every WARL field must read back a legal value (Privileged Spec v1.12
// L2857: "No WARL field contains an illegal value"). Three registers need a
// rule beyond "the write mask keeps the field inside its own bits":
//
//   * medeleg / mideleg (0x302 / 0x303). The implementation table declares the
//     whole MXLEN field WARL, but p0 implements no S or U mode, so there is no
//     delegation target and the only legal value of every bit is 0 (medeleg:
//     "p0 is M-only ... every bit is WARL whose only legal value is 0";
//     mideleg: "With no S or U mode there is no delegation target"). A write is
//     therefore *accepted* -- these are not read-only registers and must not
//     raise an illegal-instruction trap -- and the bits canonicalise to 0, which
//     the generated write mask of 0 does by construction. A read returns 0.
//
//   * mtvec (0x305). Table mtvec MODE allocates 0 = Direct, 1 = Vectored and
//     reserves MODE >= 2. A reserved encoding has no defined behaviour, so it is
//     canonicalised to Direct (0): an implementation that does not support a
//     reserved value must not read it back, and 0 is the value the register
//     resets to and the one every caller understands. BASE is aligned by
//     construction, because BASE occupies bits 63:2 and the mode bits 1:0 are
//     written by the same rule rather than folded into the base (L1181-L1183:
//     "The value in the BASE field must always be aligned on a 4-byte boundary").
//
//   * mepc (0x341). The low bits follow the profile's IALIGN: a profile that
//     claims C has IALIGN=16, so only mepc[0] is read-only zero, and one that
//     does not has IALIGN=32 and both low bits are read-only zero (L1935-L1937).
//     The generated write mask encodes that, and the trap path masks the trap PC
//     with the same mask, so a trap cannot install a bit the profile's IALIGN
//     forbids. The mask is derived from the profile's claimed extensions
//     (tools/mosaic/config_check.py `ialign`), not frozen in the CSR table.
//
// ------------------------------------------------------- boundary priority
//
// Three things can ask to change the file in one cycle. They are ordered
// strictly, and a lower-priority request in the same cycle is dropped rather
// than merged:
//
//     1. trap_valid_i   -- trap entry (mstatus, mepc, mcause, mtval)
//     2. mret_valid_i   -- MRET (mstatus only)
//     3. csr_we_i       -- a retiring CSR instruction
//
// A trap and an MRET are mutually exclusive at one architectural boundary; the
// hardware resolves the impossible combination in favour of the trap, and the
// assertion below names the invariant rather than relying on that resolution.
// A trap outranking a retiring CSR instruction is what makes the trap precise:
// the instruction that trapped contributes no retirable CSR write, so mstatus
// and mepc describe the trap and nothing the trapping instruction was about to
// do.
//
// ------------------------------------------------- counters and their writes
//
// mcycle and minstret are free-running: cnt_cycle_i advances mcycle by one on
// every rising edge, by definition ("The mcycle CSR counts the number of clock
// cycles executed by the processor core", L1650-L1655), and cnt_instret_i
// advances minstret by however many instructions retired at that edge -- a
// two-wide machine can retire two in one cycle, and the ISA counter must count
// both, not one (I-076: a minstret wired to a single retire lane under-counts
// every dual-retire cycle). A software write
// (the counters are MRW and the spec says they "can be written with a given
// value", L1657-L1659) therefore supplies the value the counter has *at that
// edge*, and the same edge's tick is added on top:
//
//     next = (write_accepted ? written_value : current) + tick
//
// So a write of V that retires in a cycle where the counter ticks reads back
// V + 1, never V: an instruction cannot delete a cycle from a hardware counter
// that is defined to count every cycle. The alternative (a write suppresses the
// tick) would make the counter's value depend on when the write happened to land
// relative to the clock, which is not a decision software can make. The unit
// test drives both a write and a tick in one cycle and checks V + 1.
//
// cycle, instret and time are read-only shadows (L1805-L1812): cycle and instret
// return the mcycle and minstret registers, time returns mtime_i. A write to any
// of them is an illegal CSR access (`csr_wr_illegal_o`) and changes nothing.
//
// ------------------------------------------------------------------- mip
//
// mip is owned by mosaic_interrupt (I-020): this module holds no mip state. A
// read returns the pending view masked to the implemented bits -- p0 has a CLINT
// driving MSIP and MTIP and no PLIC, so bits 3 and 7 are the implemented ones and
// every other bit, MEIP included, reads zero -- and a software write is forwarded
// on mip_we_o / mip_op_o / mip_wdata_o for the owner to apply.
//
// ------------------------------------------------------- reads and the edge
//
// The read port is combinational over the registers, so a read presented in
// cycle N returns the state at the *start* of cycle N. A write that retires in
// cycle N is visible to a read in cycle N+1 and never combinationally on the
// write port: the write is registered, and csr_rdata_o never depends on
// csr_wdata_i. That is the whole content of "CSR side effects are applied at the
// sequential boundary".
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_CSR_MUTANT_<n> injects exactly one defect to prove the unit test can
// detect it. The shipping build defines none of them; the table with real output
// is in results/reports/I-019-csr.md.
//
//   MIE_KEPT        trap entry sets MPIE but forgets to clear MIE
//   MRET_NO_RESTORE MRET sets MPIE but forgets MIE <- MPIE
//   MPP_CLEARED     trap entry clears MPP instead of writing the current
//                   privilege (which is M=3, the only legal value in p0)
//   READ_AFTER_WRITE csr_rdata_o shows the write operand combinationally
//   RO_WRITE_ACCEPTED a write to a read-only CSR is reported legal
//   CYCLE_WRITABLE  writes to the cycle/instret shadows are accepted and land
//                   in mcycle/minstret
//   NO_FIELD_MASK   the mstatus write is not masked to the generated writable
//                   fields, so bits the profile declares unmodifiable land in
//                   the register (exercised through the core by
//                   CASE=core.trap_csr_program, which reads mstatus back after
//                   writing all ones)
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// mosaic_pkg carries the csr_op_e encoding the write port uses. It is included
// rather than assumed to have been compiled first, for the same reason
// mosaic_fetch.sv includes it: this file has to elaborate on its own, and the
// package carries an include guard so a second inclusion is a no-op.
`include "mosaic_pkg.sv"

// The generated implementation table: one address, reset value, write mask and
// write-legality flag per CSR, decoded from the profile's CSR tables, plus the
// profile's PMP geometry (the CSR numbers of the PMP registers are fixed by the
// ISA; whether they exist at all is the profile's `pmp` block).
`include "mosaic_csr_pkg.svh"
/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_on UNUSEDPARAM */

module mosaic_csr #(
    // The hart's own identifier, read back through `mhartid` (V-064). The
    // default is the generated single-hart value, so every existing single-hart
    // instantiation reads exactly what it read before; a multi-hart build passes
    // the core's HART_ID so each hart reads its own number. A hart identifier is
    // per-hart architectural state, not a machine-wide constant: two harts that
    // both read 0 cannot be told apart by software.
    parameter logic [63:0] MHARTID_VALUE = mosaic_csr_pkg::MOSAIC_CSR_RESET_MHARTID
) (
    input  logic                   clk_i,
    input  logic                   rst_i,

    // Zicsr read port: combinational, valid for the address presented this cycle
    input  logic [11:0]            csr_addr_i,
    output logic [63:0]            csr_rdata_o,       // 0 when illegal
    output logic                   csr_illegal_o,     // unimplemented address

    // Zicsr write port, applied at the clock edge when csr_we_i
    input  logic                   csr_we_i,
    input  mosaic_pkg::csr_op_e    csr_op_i,
    input  logic [63:0]            csr_wdata_i,
    output logic                   csr_wr_illegal_o,  // read-only or unimplemented

    // counters
    input  logic                   cnt_cycle_i,
    // How many instructions retired at this edge: 0, 1 or 2 (retire width 2).
    input  logic [1:0]             cnt_instret_i,

    // trap entry / return at the architectural boundary
    input  logic                   trap_valid_i,
    input  logic [63:0]            trap_cause_i,
    input  logic [63:0]            trap_tval_i,
    input  logic [63:0]            trap_epc_i,
    output logic                   trap_commit_o,
    output logic [63:0]            trap_target_o,
    input  logic                   mret_valid_i,
    output logic                   mret_commit_o,
    output logic [63:0]            mret_target_o,
    // SRET, the supervisor return (I-044). Committed exactly like MRET: the
    // system unit resolves it at the ROB head and strobes it in the cycle the
    // instruction retires.
    input  logic                   sret_valid_i,
    output logic                   sret_commit_o,
    output logic [63:0]            sret_target_o,
    // Whether the *requested* return is legal in the current mode. The core
    // turns a false into the illegal-instruction exception the spec names:
    // "SRET should also raise an illegal instruction exception when TSR=1 in
    // mstatus", "an xRET instruction can be executed in privilege mode x or
    // higher", and "executing WFI in U-mode causes an illegal instruction
    // exception" is the same rule applied to the halt below.
    output logic                   mret_illegal_o,
    output logic                   sret_illegal_o,
    // WFI's own legality. "When S-mode is implemented, then executing WFI in
    // U-mode causes an illegal instruction exception, unless it completes within
    // an implementation-specific, bounded time limit" -- this profile's WFI
    // halts until an interrupt, which is unbounded -- and TW intercepts S-mode's
    // WFI the way TSR intercepts its SRET.
    output logic                   wfi_illegal_o,

    // ------------------------------------------------------ privilege (I-044)
    // The CSR file owns the architectural privilege mode: it is the state that
    // the trap entry, MRET and SRET change, and no other module keeps a copy.
    // The memory path reads it here for the fetch permission check and for the
    // ECALL cause, and it is a *combinational* read of the register, so an
    // instruction younger than a return cannot see the return's new mode: the
    // return redirects, and everything younger is flushed.
    output logic [1:0]             o_priv_o,
    output logic [63:0]            o_medeleg_o,
    output logic [63:0]            o_mideleg_o,

    // ------------------------------------------------------- PMP CSR hand-off
    // The PMP entries and their lock/WARL rules live in mosaic_pmp; this file
    // decides only whether an address selects them and whether the access is
    // legal, and forwards the operation's already-applied write operand.
    output logic                   pmp_sel_o,
    input  logic [63:0]            pmp_rdata_i,
    output logic                   pmp_we_o,
    output logic [63:0]            pmp_wdata_o,

    // --------------------------------------------------------- supervisor view
    output logic [63:0]            o_sstatus_o,
    output logic [63:0]            o_stvec_o,
    output logic [63:0]            o_sepc_o,
    output logic [63:0]            o_scause_o,
    output logic [63:0]            o_stval_o,
    output logic [63:0]            o_sscratch_o,
    output logic [63:0]            o_sie_o,
    output logic [63:0]            o_sip_o,
    output logic [63:0]            o_satp_o,
    output logic [63:0]            o_senvcfg_o,
    output logic [63:0]            o_scounteren_o,
    output logic [63:0]            o_mcounteren_o,

    // interrupt pending view owned by mosaic_interrupt (I-020)
    input  logic [63:0]            mip_i,
    output logic                   mip_we_o,
    output logic [1:0]             mip_op_o,
    output logic [63:0]            mip_wdata_o,

    // platform time
    input  logic [63:0]            mtime_i,

    // --------------------------------------------------------- F/D state (I-050)
    // `o_fcsr_o`/`o_fflags_o`/`o_frm_o` are the FP control state as read by
    // software; `o_frm_o` is what the FP unit resolves a dynamic rounding mode
    // against. `fp_fflags_or_i` is the flags of the FP operations that commit in
    // this cycle (already ordered by the core against a same-cycle fcsr write),
    // and `fp_fs_dirty_i` is "an FP instruction that modifies FP state retires
    // now", which sets mstatus.FS = Dirty.
    output logic [63:0]            o_fcsr_o,
    output logic [4:0]             o_fflags_o,
    output logic [2:0]             o_frm_o,
    input  logic [4:0]             fp_fflags_or_i = 5'd0,
    input  logic                   fp_fs_dirty_i = 1'b0,

    // -------------------------------------------------------- vector state (I-059)
    // `vec_vs_dirty_i` is "a vector instruction that modifies vector state
    // retires now", which sets mstatus.VS = Dirty (11): vset{i}vl{i}, any
    // instruction that writes a vector register, and any vstart/vxrm/vxsat
    // write. Like FS it is a set, never a clear, so a same-cycle software write
    // to mstatus loses to the dirty transition.
    input  logic                   vec_vs_dirty_i = 1'b0,

    // ------------------------------------------------------------ observability
    output logic [63:0]            o_mstatus_o,
    output logic [63:0]            o_mtvec_o,
    output logic [63:0]            o_mepc_o,
    output logic [63:0]            o_mcause_o,
    output logic [63:0]            o_mtval_o,
    output logic [63:0]            o_mscratch_o,
    output logic [63:0]            o_mie_o,
    output logic [63:0]            o_mip_o,
    output logic [63:0]            o_misa_o,
    output logic [63:0]            o_mcycle_o,
    output logic [63:0]            o_minstret_o,
    output logic [31:0]            o_wr_ctr,
    output logic [31:0]            o_illegal_wr_ctr,
    output logic [31:0]            o_trap_ctr,
    output logic [31:0]            o_mret_ctr,
    output logic [31:0]            o_sret_ctr,
    output logic [31:0]            o_trap_s_ctr,
    output logic [31:0]            o_priv_illegal_ctr,
    output logic [31:0]            o_priv_change_ctr
);

  // --------------------------------------------------------------- constants
  //
  // mstatus field positions from the RV64 layout the clause cites (bit-number
  // rows L448-L495, field-name rows L462-L474): MIE = bit 3, MPIE = bit 7,
  // MPP = bits 12:11. Writability is not decided here -- the generated mask is.
  localparam logic [63:0] MSTATUS_MIE  = 64'h0000_0000_0000_0008;
  localparam logic [63:0] MSTATUS_MPIE = 64'h0000_0000_0000_0080;
  localparam logic [63:0] MSTATUS_MPP  = 64'h0000_0000_0000_1800;
  // FS (bits 14:13) is the FP-state field, and 2'b11 is its Dirty encoding
  // (I-050). The field is writable through the generated mask in every profile;
  // this constant is the value an instruction that modifies FP state leaves.
  localparam logic [63:0] MSTATUS_FS   = 64'h0000_0000_0000_6000;

  // VS (bits 10:9) is the vector-state field, and 2'b11 is its Dirty encoding
  // (I-059). The field is writable through the generated mask; this constant is
  // the value an instruction that modifies vector state leaves.
  localparam logic [63:0] MSTATUS_VS   = 64'h0000_0000_0000_0600;

  // The rest of the mstatus fields this profile makes real (I-044). Positions
  // are the RV64 layout the clause cites; writability still comes from the
  // generated mask, so a profile without S or U zeros exactly these bits.
  localparam logic [63:0] MSTATUS_SIE  = 64'h0000_0000_0000_0002;
  localparam logic [63:0] MSTATUS_SPIE = 64'h0000_0000_0000_0020;
  localparam logic [63:0] MSTATUS_SPP  = 64'h0000_0000_0000_0100;
  localparam logic [63:0] MSTATUS_MPRV = 64'h0000_0000_0002_0000;
  // TVM and TSR only have a meaning where S-mode does, so they are named only
  // in the profiles that implement it -- a constant nothing can consult would be
  // a declaration, not a rule.
  `ifdef MOSAIC_CSR_HAS_S
    localparam logic [63:0] MSTATUS_TVM  = 64'h0000_0000_0010_0000;
  `endif
  localparam logic [63:0] MSTATUS_TW   = 64'h0000_0000_0020_0000;
  `ifdef MOSAIC_CSR_HAS_S
    localparam logic [63:0] MSTATUS_TSR  = 64'h0000_0000_0040_0000;
  `endif


  // sstatus is a *view* of mstatus, not a register. The bits it exposes are the
  // fields the supervisor-status figure names: SD (63), UXL (33:32), MXR (19),
  // SUM (18), XS (16:15), FS (14:13), VS (10:9), SPP (8), UBE (6), SPIE (5),
  // SIE (1). This mask is the layout; which of those bits software may change is
  // the generated `MOSAIC_CSR_WMASK_SSTATUS`, and the two are used together:
  //   read  -> mstatus & SSTATUS_FIELDS
  //   write -> mstatus = (mstatus & ~WMASK) | (operand & WMASK)
  // A second register would be a second answer to "what is sstatus.SIE", which
  // is exactly the failure this file's header says it exists to avoid.
  localparam logic [63:0] SSTATUS_FIELDS = 64'h8000_0003_000d_e722;

  // The supervisor interrupt bits inside mie/mip, and the writable parts of the
  // sie/sip views, from the same generated table the registers come from.
  localparam logic [63:0] SIE_VIEW_MASK = 64'h0000_0000_0000_0222;  // SEIE, STIE, SSIE
  localparam logic [63:0] SIP_VIEW_MASK = 64'h0000_0000_0000_0222;  // SEIP, STIP, SSIP

  // The U-level counter addresses this profile implements, and the mcounteren/
  // scounteren bit each one is gated by ("Interrupt cause number i ... bit i"):
  // cycle = 0xC00 -> CY (bit 0), time = 0xC01 -> TM (bit 1), instret = 0xC02 ->
  // IR (bit 2).
  localparam logic [11:0] CSR_CYCLE   = 12'hC00;
  localparam logic [11:0] CSR_TIME    = 12'hC01;
  localparam logic [11:0] CSR_INSTRET = 12'hC02;

  // ------------------------------------------------------------------- state
  //
  // mstatus.MPP is read-only 3 in this profile: bits 12:11 are absent from the
  // generated write mask and the reset value has them set. That is the correct
  // encoding of "the only privilege mode this profile implements is M", and the
  // trap and MRET paths below write M into the field anyway so the rule "MPP
  // becomes the previous/next privilege" is visible in the hardware rather than
  // implied by a constant.
  logic [63:0] mstatus_q;
  logic [63:0] mie_q;
  logic [63:0] mtvec_q;
  logic [63:0] mscratch_q;
  logic [63:0] mepc_q;
  logic [63:0] mcause_q;
  logic [63:0] mtval_q;
  logic [63:0] mcycle_q;
  logic [63:0] minstret_q;

  // F/D (I-050): the FP control state, one register. fcsr = {frm[7:5],
  // fflags[4:0]}; fflags (0x001) and frm (0x002) are views of the same storage,
  // exactly as sstatus is a view of mstatus, so the three addresses cannot
  // disagree. Only the low 8 bits carry state.
  logic [7:0]  fcsr_q;
  logic [7:0]  fcsr_d;

  logic [63:0] mstatus_d;
  logic [63:0] mie_d;
  logic [63:0] mtvec_d;
  logic [63:0] mscratch_d;
  logic [63:0] mepc_d;
  logic [63:0] mcause_d;
  logic [63:0] mtval_d;
  logic [63:0] mcycle_d;
  logic [63:0] minstret_d;

  // --------------------------------------------- the supervisor state (I-044)
  //
  // sstatus, sie and sip are deliberately absent: they are views of mstatus and
  // of mie/mip, and a register of their own would be a second copy of a bit that
  // already has an owner. What does live here is the state the supervisor
  // controller would own if it existed, which is the trap frame and the
  // configuration registers with no M-mode equivalent.
  // The delegation registers exist in every profile: they are M-mode registers
  // whose every bit is WARL, and a profile with no less-privileged mode gives
  // them a write mask of 0 rather than removing them (a write is still accepted
  // and canonicalises to zero, so the register is not a trap).
  logic [63:0] medeleg_q;
  logic [63:0] mideleg_q;
  logic [63:0] mcounteren_q;

  logic [63:0] medeleg_d;
  logic [63:0] mideleg_d;
  logic [63:0] mcounteren_d;

  `ifdef MOSAIC_CSR_HAS_S
    logic [63:0] stvec_q;
    logic [63:0] sscratch_q;
    logic [63:0] sepc_q;
    logic [63:0] scause_q;
    logic [63:0] stval_q;
    logic [63:0] satp_q;
    logic [63:0] senvcfg_q;
    logic [63:0] scounteren_q;

    logic [63:0] stvec_d;
    logic [63:0] sscratch_d;
    logic [63:0] sepc_d;
    logic [63:0] scause_d;
    logic [63:0] stval_d;
    logic [63:0] satp_d;
    logic [63:0] senvcfg_d;
    logic [63:0] scounteren_d;
  `endif

  // The architectural privilege mode. Two bits, with the encoding the
  // specification uses in mstatus.MPP/SPP: 0 = U, 1 = S, 3 = M.
  logic [1:0]  priv_q;
  logic [1:0]  priv_d;

  // ------------------------------------------------------------------ decode
  //
  // One decode answers both port questions: is the address implemented at all
  // (read legality) and does its access mode permit a write (write legality).
  // `wr_legal` is the generated access mode, not a list written here, so a CSR
  // the table marks read-only cannot become writable by editing this file.
  logic addr_impl;
  logic wr_legal;
  // Declared here because the address-decode block below reads it before the
  // counter-gate block that drives it; slang rejects a use before the
  // declaration, and moving the driving assign up would separate it from the
  // gate rule it belongs to.
  logic csr_is_hpm;

  // PMP CSR selection: base + span from the generated config package, so a
  // profile with no PMP entries (MOSAIC_PMP_ENTRIES == 0) selects nothing and
  // every PMP number stays an illegal instruction there.
  logic [11:0] pmp_cfg_off_c;
  logic [11:0] pmp_addr_off_c;
  logic        pmp_cfg_sel_c;
  logic        pmp_addr_sel_c;

  always_comb begin
    pmp_cfg_off_c  = csr_addr_i - mosaic_cfg_pkg::MOSAIC_PMPCFG_ADDR_BASE;
    pmp_addr_off_c = csr_addr_i - mosaic_cfg_pkg::MOSAIC_PMPADDR_ADDR_BASE;
    pmp_cfg_sel_c  = (mosaic_cfg_pkg::MOSAIC_PMP_ENTRIES != 0) && (csr_addr_i[0] == 1'b0) &&
                     (csr_addr_i >= mosaic_cfg_pkg::MOSAIC_PMPCFG_ADDR_BASE) &&
                     (pmp_cfg_off_c < 12'(2 * mosaic_cfg_pkg::MOSAIC_PMP_CFG_COUNT));
    pmp_addr_sel_c = (mosaic_cfg_pkg::MOSAIC_PMP_ENTRIES != 0) &&
                     (csr_addr_i >= mosaic_cfg_pkg::MOSAIC_PMPADDR_ADDR_BASE) &&
                     (pmp_addr_off_c < 12'(mosaic_cfg_pkg::MOSAIC_PMP_ENTRIES));
  end

  assign pmp_sel_o = pmp_cfg_sel_c | pmp_addr_sel_c;

  always_comb begin
    addr_impl = 1'b1;
    wr_legal  = 1'b0;
    case (csr_addr_i)
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_FFLAGS:     wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_FFLAGS;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_FRM:        wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_FRM;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_FCSR:       wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_FCSR;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSTATUS:    wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MSTATUS;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MISA:       wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MISA;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MEDELEG:    wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MEDELEG;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIDELEG:    wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MIDELEG;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIE:        wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MIE;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MTVEC:      wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MTVEC;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCOUNTEREN: wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MCOUNTEREN;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSCRATCH:   wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MSCRATCH;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MEPC:       wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MEPC;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCAUSE:     wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MCAUSE;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MTVAL:      wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MTVAL;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIP:        wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MIP;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MVENDORID:  wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MVENDORID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MARCHID:    wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MARCHID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIMPID:     wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MIMPID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MHARTID:    wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MHARTID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCYCLE:     wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MCYCLE;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MINSTRET:   wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_MINSTRET;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_CYCLE:      wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_CYCLE;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_TIME:       wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_TIME;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_INSTRET:    wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_INSTRET;
      default:                    addr_impl = 1'b0;
    endcase

    // The supervisor registers. They come from a second table that only the
    // profiles with S-mode load, so the addresses exist in every profile that
    // has them and this arm is compiled out of the ones that do not. An address
    // this arm matches is *implemented*, and it has to say so: the first table's
    // `default` above already declared every address it does not name
    // unimplemented, so an arm that raised only the write legality would leave
    // every supervisor CSR looking unimplemented. That is not a hypothetical --
    // CASE=privilege.permission_matrix found it, as a `csrw scounteren` from
    // M-mode raising the illegal-instruction exception at reset.
    `ifdef MOSAIC_CSR_HAS_S
      case (csr_addr_i)
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SSTATUS:    begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SSTATUS; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SIE:        begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SIE; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SIP:        begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SIP; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_STVEC:      begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_STVEC; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SCOUNTEREN: begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SCOUNTEREN; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SENVCFG:    begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SENVCFG; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SSCRATCH:   begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SSCRATCH; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SEPC:       begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SEPC; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SCAUSE:     begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SCAUSE; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_STVAL:      begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_STVAL; end
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SATP:       begin addr_impl = 1'b1; wr_legal = mosaic_csr_pkg::MOSAIC_CSR_WRITE_LEGAL_SATP; end
        default: ;
      endcase
    `endif

    // PMP registers are read/write M-mode registers.
    if (pmp_sel_o) addr_impl = 1'b1;
    if (pmp_sel_o) wr_legal  = 1'b1;

    // The Zihpm counter shadows: implemented, read-only zero. `wr_legal` stays 0
    // because the address itself encodes read-only (csr[11:10] == 2'b11), and the
    // value is the read mux's `default` arm; what has to be said here is only
    // that the address is implemented, i.e. that a read answers 0 instead of
    // raising an illegal instruction.
    if (csr_is_hpm) addr_impl = 1'b1;

    `ifdef MOSAIC_CSR_MUTANT_RO_WRITE_ACCEPTED
      // MUTANT: every implemented CSR is treated as writable, so a write to a
      // read-only register stops raising csr_wr_illegal_o.
      if (addr_impl) wr_legal = 1'b1;
    `endif

    `ifdef MOSAIC_CSR_MUTANT_CYCLE_WRITABLE
      // MUTANT: the read-only cycle/instret shadows are treated as writable
      // aliases of mcycle/minstret.
      if ((csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_CYCLE) || (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_INSTRET)) begin
        wr_legal = 1'b1;
      end
    `endif
  end

  // ------------------------------------------------------ the access check
  //
  // Privileged spec v1.12, "CSR Address Mapping Conventions": "The next two bits
  // (csr[9:8]) encode the lowest privilege level that can access the CSR", and
  // "The top two bits (csr[11:10]) indicate whether the register is read/write
  // (00, 01, or 10) or read-only (11)". So the rule is read straight out of the
  // address rather than from a second table here, and the generated package
  // carries the same decoding as a constant so the testbench model and this file
  // cannot disagree about it.
  //
  // The two gates are separate, and that separation is the whole content of the
  // read-only encoding: `csr[11:10] == 11` says "no write is legal", not "this
  // register needs M-mode to *read*". Folding them into one comparison refused a
  // U-mode read of the `cycle` counter that mcounteren and scounteren both
  // permit -- CASE=privilege.permission_matrix found exactly that, as cause 2
  // where the ISA's rule allows the read.
  logic [1:0] csr_min_priv_r;
  logic [1:0] csr_min_priv_w;
  logic       csr_priv_ok_r;
  logic       csr_priv_ok_w;
  logic       csr_counter_ok;
  logic [5:0] csr_counter_bit;
  logic       csr_is_counter;
  logic [63:0] scounteren_view;

  assign csr_min_priv_r = csr_addr_i[9:8];
  assign csr_min_priv_w = (csr_addr_i[11:10] == 2'b11) ? mosaic_csr_pkg::MOSAIC_PRIV_M
                                                       : csr_addr_i[9:8];

  // The counter gate. "When the CY, TM, IR, or HPMn bit in the mcounteren
  // register is clear, attempts to read the cycle, time, instret, or hpmcounter_n
  // register while executing in U-mode will cause an illegal instruction
  // exception"; S-mode reads are gated by mcounteren alone ("When the ... bit in
  // mcounteren is clear, attempts to read ... while executing in S-mode will
  // cause an illegal instruction exception"). The bit is the counter's own
  // number: 0, 1 and 2 for cycle/time/instret, and HPMn for the Zihpm
  // hpmcounter_n shadow at 0xC00+n -- which is exactly the low six bits of that
  // address. A profile whose tables declare no hpmcounter row compiles the whole
  // block out, because a reference to a constant the package does not declare is
  // a compile error, not a zero.
  assign csr_is_counter  = (csr_addr_i == CSR_CYCLE) || (csr_addr_i == CSR_TIME) ||
                           (csr_addr_i == CSR_INSTRET) || csr_is_hpm;
  assign csr_counter_bit = (csr_addr_i == CSR_CYCLE)   ? 6'd0 :
                           (csr_addr_i == CSR_TIME)    ? 6'd1 :
                           (csr_addr_i == CSR_INSTRET) ? 6'd2 : csr_addr_i[5:0];

  // The Zihpm counter shadows, from the generated range. With no HPM counter
  // implemented the whole block is read-only zero, which is what the `default`
  // arm of the read mux already answers; what this decode decides is that the
  // addresses are *implemented* (a read must not trap) and that the counter gate
  // applies to them. MOSAIC_CSR_MUTANT_NO_HPM removes the decode, putting the
  // reads back to illegal -- the control CASE=core.act_dut's Zihpm ELF names.
  `ifdef MOSAIC_CSR_HAS_HPM
    `ifdef MOSAIC_CSR_MUTANT_NO_HPM
      assign csr_is_hpm = 1'b0;
    `else
      assign csr_is_hpm = (csr_addr_i >= mosaic_csr_pkg::MOSAIC_CSR_HPM_FIRST) &&
                          (csr_addr_i <= mosaic_csr_pkg::MOSAIC_CSR_HPM_LAST);
    `endif
  `else
    assign csr_is_hpm = 1'b0;
  `endif

  // A profile without S-mode has no scounteren, so the U-mode half of the gate
  // has nothing to consult; it reads zero, which is the same answer a register
  // that is entirely read-only zero would give.
  `ifdef MOSAIC_CSR_HAS_S
    assign scounteren_view = scounteren_q;
  `else
    assign scounteren_view = 64'd0;
  `endif

  always_comb begin
    csr_counter_ok = 1'b1;
    if (csr_is_counter) begin
      if (priv_q == mosaic_csr_pkg::MOSAIC_PRIV_M) begin
        csr_counter_ok = 1'b1;
      end else if (priv_q == mosaic_csr_pkg::MOSAIC_PRIV_S) begin
        csr_counter_ok = mcounteren_q[csr_counter_bit];
      end else begin
        csr_counter_ok = mcounteren_q[csr_counter_bit] && scounteren_view[csr_counter_bit];
      end
    end
  end

  // TVM: "When TVM=1, attempts to read or write the satp CSR while executing in
  // S-mode will raise an illegal instruction exception."
  logic satp_tvm_illegal;

  `ifdef MOSAIC_CSR_HAS_S
    assign satp_tvm_illegal = (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_SATP) &&
                              (priv_q == mosaic_csr_pkg::MOSAIC_PRIV_S) &&
                              ((mstatus_q & MSTATUS_TVM) != 64'd0);
  `else
    assign satp_tvm_illegal = 1'b0;
  `endif

  assign csr_priv_ok_r = (priv_q >= csr_min_priv_r);
  assign csr_priv_ok_w = (priv_q >= csr_min_priv_w);

  assign csr_illegal_o    = ~addr_impl | ~csr_priv_ok_r | ~csr_counter_ok |
                            satp_tvm_illegal;
  assign csr_wr_illegal_o = csr_we_i & (~addr_impl | ~wr_legal | ~csr_priv_ok_r |
                                        ~csr_priv_ok_w | ~csr_counter_ok |
                                        satp_tvm_illegal);

  // A write that is accepted by the ports and not pre-empted by the boundary.
  // The trap/mret terms are what make a trap outrank a retiring CSR instruction.
  logic wr_accept;
  assign wr_accept = csr_we_i & addr_impl & wr_legal & ~trap_valid_i & ~mret_valid_i &
                     ~sret_valid_i;

  // The two boundary events cannot both be true at one architectural boundary.
  // The hardware resolves a simultaneous request in favour of the trap (below);
  // this assertion makes a caller that presents both visible instead of letting
  // the resolution hide it.
  always_comb begin
    assert (!(trap_valid_i & mret_valid_i));
    assert (!(trap_valid_i & sret_valid_i));
    assert (!(mret_valid_i & sret_valid_i));
  end

  // -------------------------------------------------------------------- read
  logic [63:0] csr_rdata_stored;

  always_comb begin
    case (csr_addr_i)
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_FFLAGS:     csr_rdata_stored = {59'b0, fcsr_q[4:0]};
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_FRM:        csr_rdata_stored = {61'b0, fcsr_q[7:5]};
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_FCSR:       csr_rdata_stored = {56'b0, fcsr_q};
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSTATUS:    csr_rdata_stored = mstatus_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MISA:       csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MISA;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MEDELEG:    csr_rdata_stored = medeleg_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIDELEG:    csr_rdata_stored = mideleg_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIE:        csr_rdata_stored = mie_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MTVEC:      csr_rdata_stored = mtvec_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCOUNTEREN: csr_rdata_stored = mcounteren_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSCRATCH:   csr_rdata_stored = mscratch_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MEPC:       csr_rdata_stored = mepc_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCAUSE:     csr_rdata_stored = mcause_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MTVAL:      csr_rdata_stored = mtval_q;
      // mip is the interrupt unit's view; only the implemented pending bits are
      // readable, and every other bit reads zero (bits 3 and 7 in p0).
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIP:        csr_rdata_stored = mip_i & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MIP;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MVENDORID:  csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MVENDORID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MARCHID:    csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MARCHID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIMPID:     csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MIMPID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MHARTID:    csr_rdata_stored = MHARTID_VALUE;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCYCLE:     csr_rdata_stored = mcycle_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MINSTRET:   csr_rdata_stored = minstret_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_CYCLE:      csr_rdata_stored = mcycle_q;   // read-only shadow
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_TIME:       csr_rdata_stored = mtime_i;    // read-only shadow
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_INSTRET:    csr_rdata_stored = minstret_q; // read-only shadow
      default:                    csr_rdata_stored = 64'b0;
    endcase

    // The supervisor view. sstatus, sie and sip are views of state that already
    // has an owner -- mstatus, mie and mip -- masked to the bits the supervisor
    // layout exposes, so there is one copy of every one of them.
    `ifdef MOSAIC_CSR_HAS_S
      case (csr_addr_i)
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SSTATUS:    csr_rdata_stored = mstatus_q & SSTATUS_FIELDS;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SIE:        csr_rdata_stored = mie_q & SIE_VIEW_MASK;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SIP:        csr_rdata_stored = mip_i & SIP_VIEW_MASK;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_STVEC:      csr_rdata_stored = stvec_q;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SCOUNTEREN: csr_rdata_stored = scounteren_q;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SENVCFG:    csr_rdata_stored = senvcfg_q;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SSCRATCH:   csr_rdata_stored = sscratch_q;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SEPC:       csr_rdata_stored = sepc_q;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SCAUSE:     csr_rdata_stored = scause_q;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_STVAL:      csr_rdata_stored = stval_q;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_SATP:       csr_rdata_stored = satp_q;
        default: ;
      endcase
    `endif

    // The PMP registers live in mosaic_pmp; this port is their read value.
    if (pmp_sel_o) csr_rdata_stored = pmp_rdata_i;
  end

  `ifdef MOSAIC_CSR_MUTANT_READ_AFTER_WRITE
    // MUTANT: the write operand appears on the read port combinationally, so a
    // write retires in the same cycle it is presented.
    assign csr_rdata_o = csr_we_i ? csr_wdata_i : csr_rdata_stored;
  `else
    // The shipping rule: the read port is a function of the registers only, so a
    // write retiring this cycle is first readable next cycle.
    assign csr_rdata_o = csr_rdata_stored;
  `endif

  // --------------------------------------------------- the write operand view
  //
  // CSR_RW replaces, CSR_RS sets, CSR_RC clears -- the operand has already been
  // selected by the caller, and CSR_NONE never reaches an accepted write.
  logic [63:0] csr_op_result;

  always_comb begin
    case (csr_op_i)
      mosaic_pkg::CSR_RW: csr_op_result = csr_wdata_i;
      mosaic_pkg::CSR_RS: csr_op_result = csr_rdata_stored | csr_wdata_i;
      mosaic_pkg::CSR_RC: csr_op_result = csr_rdata_stored & ~csr_wdata_i;
      default:            csr_op_result = csr_wdata_i;
    endcase
  end

  // -------------------------------------------------------- mtvec canonical form
  function automatic logic [1:0] mtvec_mode_canon(input logic [1:0] mode);
    // Table mtvec MODE: 0 Direct, 1 Vectored, >= 2 Reserved. A reserved encoding
    // has no defined behaviour and must not be read back, so it becomes Direct.
    mtvec_mode_canon = (mode <= 2'b01) ? mode : 2'b00;
  endfunction

  // ------------------------------------------------------- trap / mret targets
  assign trap_commit_o = trap_valid_i;
  assign mret_commit_o = mret_valid_i & ~trap_valid_i & ~sret_valid_i;
  assign sret_commit_o = sret_valid_i & ~trap_valid_i & ~mret_valid_i;

  // MRET targets mepc exactly; the low bits are already zero by construction.
  assign mret_target_o = mepc_q;
  // SRET resumes at sepc exactly, the same rule as MRET and mepc. A profile
  // with no S-mode has no sepc; there is also no SRET it could execute, and the
  // target reads zero rather than a register that does not exist.
  `ifdef MOSAIC_CSR_HAS_S
    assign sret_target_o = sepc_q;
  `else
    assign sret_target_o = 64'd0;
  `endif

  // ---------------------------------------------------------------------------
  // Delegation. The cause's low bits index the delegation register and bit 63
  // says which of the two it is; the delegated bit only applies from S or U.
  // ---------------------------------------------------------------------------
  logic trap_deleg_o;
  logic [5:0] trap_code_c;

  assign trap_code_c  = trap_cause_i[5:0];
`ifdef MOSAIC_CSR_MUTANT_NO_DELEGATION
  // NEGATIVE CONTROL (boot.p1_contract): delegation is dropped, so every trap
  // the profile delegated to S-mode is taken in M-mode instead. The case sees
  // the trap target become mtvec and the supervisor handler never run.
  assign trap_deleg_o = 1'b0;
`else
  assign trap_deleg_o = (trap_cause_i[63] ? mideleg_q[trap_code_c] : medeleg_q[trap_code_c]) &&
                        (priv_q != mosaic_csr_pkg::MOSAIC_PRIV_M);
`endif

  // ---------------------------------------------------------------------------
  // The legality of the two returns, evaluated in the mode the hart is in.
  // "An xRET instruction can be executed in privilege mode x or higher":
  // MRET in M-mode always, SRET in S-mode or higher; and "SRET should also raise
  // an illegal instruction exception when TSR=1 in mstatus" applies to S-mode
  // only ("When TSR=1, attempts to execute SRET while executing in S-mode will
  // raise an illegal instruction exception").
  // ---------------------------------------------------------------------------
  assign mret_illegal_o = (priv_q != mosaic_csr_pkg::MOSAIC_PRIV_M);
  assign wfi_illegal_o  = (priv_q == mosaic_csr_pkg::MOSAIC_PRIV_U) ||
                          ((priv_q == mosaic_csr_pkg::MOSAIC_PRIV_S) &&
                           ((mstatus_q & MSTATUS_TW) != 64'd0));
  // TSR is a field of a profile that has S-mode; a profile that does not has the
  // bit read-only zero, so the "TSR is set" test is a constant there rather than
  // a reference to a constant the package does not declare.
  `ifdef MOSAIC_CSR_HAS_S
    logic tsr_set_c;
    assign tsr_set_c = ((mstatus_q & MSTATUS_TSR) != 64'd0);
  `else
    logic tsr_set_c;
    assign tsr_set_c = 1'b0;
  `endif

  assign sret_illegal_o = (priv_q == mosaic_csr_pkg::MOSAIC_PRIV_U) ||
                          ((priv_q == mosaic_csr_pkg::MOSAIC_PRIV_S) && tsr_set_c);

  // An interrupt (mcause[63] set) taken in Vectored mode enters at
  // base + 4 * cause code; every other trap enters at the base (Table mtvec MODE
  // and the table's direct/vectored description). A trap delegated to S-mode
  // enters through the supervisor vector by the same rule.
  logic [63:0] trap_vec_c;

  always_comb begin
    trap_vec_c = mtvec_q;
    `ifdef MOSAIC_CSR_HAS_S
      if (trap_deleg_o) trap_vec_c = stvec_q;
    `endif
  end

  always_comb begin
    if ((trap_vec_c[1:0] == 2'b01) && trap_cause_i[63]) begin
      trap_target_o = {trap_vec_c[63:2], 2'b00} + {trap_cause_i[61:0], 2'b00};
    end else begin
      trap_target_o = {trap_vec_c[63:2], 2'b00};
    end
  end

  // -------------------------------------------------------------- mip forward
  // mip and sip are the same pending state seen through two addresses, so a
  // write to either reaches the interrupt unit, masked to the bits that
  // register may change. sie is the same relationship with mie and needs no
  // forwarding at all: it is applied to mie below.
  // The interrupt unit is the owner of the mask a mip write is filtered by, and
  // it applies the *mip* one to everything offered on this port. That is right
  // for mip and wrong for sip, whose writable set is a smaller subset, so the
  // difference is taken here: sip's operand is narrowed by sip's own mask before
  // it is forwarded, and mip's is passed exactly as the retired instruction
  // presented it.
  logic mip_alias_c;

  always_comb begin
    mip_alias_c = (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIP);
    `ifdef MOSAIC_CSR_HAS_S
      if (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_SIP) mip_alias_c = 1'b1;
    `endif
  end

  assign mip_we_o    = wr_accept & mip_alias_c;
  assign mip_op_o    = csr_op_i;
  `ifdef MOSAIC_CSR_HAS_S
    assign mip_wdata_o = (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_SIP)
                         ? (csr_wdata_i & mosaic_csr_pkg::MOSAIC_CSR_WMASK_SIP)
                         : csr_wdata_i;
  `else
    assign mip_wdata_o = csr_wdata_i;
  `endif

  // ----------------------------------------------------------- next-state view
  //
  // Strict priority: trap, then MRET, then the retiring CSR write. The counters
  // are part of the same chain so a trap cannot be accompanied by a counter write
  // from the instruction it is pre-empting.
  always_comb begin
    mstatus_d  = mstatus_q;
    mie_d      = mie_q;
    mtvec_d    = mtvec_q;
    mscratch_d = mscratch_q;
    mepc_d     = mepc_q;
    mcause_d   = mcause_q;
    mtval_d    = mtval_q;
    mcycle_d   = mcycle_q;
    minstret_d = minstret_q;
    medeleg_d  = medeleg_q;
    mideleg_d  = mideleg_q;
    mcounteren_d = mcounteren_q;
    fcsr_d     = fcsr_q;
    `ifdef MOSAIC_CSR_HAS_S
      stvec_d    = stvec_q;
      sscratch_d = sscratch_q;
      sepc_d     = sepc_q;
      scause_d   = scause_q;
      stval_d    = stval_q;
      satp_d     = satp_q;
      senvcfg_d  = senvcfg_q;
      scounteren_d = scounteren_q;
    `endif
    priv_d     = priv_q;

    if (trap_valid_i) begin
      // ---------------------------------------------------------- trap entry
      //
      // Where the trap goes is decided here, because the delegation registers
      // live here: "setting a bit in medeleg or mideleg will delegate the
      // corresponding trap, when occurring in S-mode or U-mode, to the S-mode
      // trap handler", and "Traps never transition from a more-privileged mode
      // to a less-privileged mode. For example, if M-mode has delegated
      // illegal-instruction exceptions to S-mode, and M-mode software later
      // executes an illegal instruction, the trap is taken in M-mode". So the
      // delegation bit only applies when the trap is taken from S or U.
      `ifdef MOSAIC_CSR_HAS_S
      if (trap_deleg_o) begin
        // Trap into S-mode: SPP <- the mode the trap was taken from (one bit),
        // SPIE <- SIE, SIE <- 0, and the trap frame is the supervisor's.
        mstatus_d = (mstatus_q & ~(MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP))
                  | (mstatus_q[1] ? MSTATUS_SPIE : 64'd0)
                  | (priv_q[0] ? MSTATUS_SPP : 64'd0);
        sepc_d    = trap_epc_i & mosaic_csr_pkg::MOSAIC_CSR_WMASK_SEPC;
        scause_d  = trap_cause_i;
        stval_d   = trap_tval_i;
        priv_d    = mosaic_csr_pkg::MOSAIC_PRIV_S;
      end else
      `endif
      begin
        // Trap into M-mode: MPIE <- MIE, MIE <- 0, MPP <- the mode the trap was
        // taken from; mepc <- the interrupted PC with its read-only low bits
        // forced zero; mcause <- the cause; mtval <- the trap value.
        mstatus_d = (mstatus_q & ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP))
                  | (mstatus_q[3] ? MSTATUS_MPIE : 64'd0)
                  | ({{62{1'b0}}, priv_q} << 11);
`ifdef MOSAIC_CSR_MUTANT_MEPC_IALIGN32
        // NEGATIVE CONTROL (EX-034): the trap epc is masked with the IALIGN=32
        // rule even for a profile that claims C, so bit 1 of a trap taken on a
        // 2-mod-4 PC is lost and mepc no longer names the faulting instruction.
        // The shipping build uses the generated mask, which follows the profile.
        mepc_d    = trap_epc_i & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MEPC & ~64'h2;
`else
        mepc_d    = trap_epc_i & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MEPC;
`endif
        mcause_d  = trap_cause_i;
        mtval_d   = trap_tval_i;
        priv_d    = mosaic_csr_pkg::MOSAIC_PRIV_M;
      end
    end else if (mret_valid_i) begin
      // ------------------------------------------------------------- MRET
      //
      // "When executing an xRET instruction, supposing xPP holds the value y,
      // xIE is set to xPIE; the privilege mode is changed to y; xPIE is set to
      // 1; and xPP is set to the least-privileged supported mode (U if U-mode is
      // implemented, else M). If y != M, xRET also sets MPRV=0."
      mstatus_d = (mstatus_q & ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP | MSTATUS_MPRV))
                | (mstatus_q[7] ? MSTATUS_MIE : 64'd0)
                | MSTATUS_MPIE
                | ({{62{1'b0}}, mosaic_csr_pkg::MOSAIC_PRIV_LEAST} << 11)
                | ((mstatus_q[12:11] != mosaic_csr_pkg::MOSAIC_PRIV_M) ? 64'd0 : (mstatus_q & MSTATUS_MPRV));
      priv_d    = mstatus_q[12:11];
    end else if (sret_valid_i) begin
      // ------------------------------------------------------------- SRET
      //
      // Same field rule with the supervisor stack: SIE <- SPIE, SPIE <- 1,
      // SPP <- the least-privileged supported mode, and MPRV is cleared because
      // SRET always returns to a mode less privileged than M.
`ifdef MOSAIC_CSR_MUTANT_SRET_NO_RESTORE
      // NEGATIVE CONTROL (boot.p1_contract): SRET returns but restores nothing:
      // SIE is not reloaded from SPIE, SPIE/SPP are not updated and the
      // privilege does not move. The case observes sstatus.SIE == 0 after the
      // first `sret` and fails with that named check.
      mstatus_d = mstatus_q;
      priv_d    = priv_q;
`else
      mstatus_d = (mstatus_q & ~(MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | MSTATUS_MPRV))
                | (mstatus_q[5] ? MSTATUS_SIE : 64'd0)
                | MSTATUS_SPIE
                | (mosaic_csr_pkg::MOSAIC_PRIV_LEAST[0] ? MSTATUS_SPP : 64'd0);
      priv_d    = {1'b0, mstatus_q[8]};   // SPP is one bit: 0 = U, 1 = S
`endif
    end else if (wr_accept) begin
      case (csr_addr_i)
`ifdef MOSAIC_CSR_MUTANT_NO_FIELD_MASK
        // MUTANT: the mstatus write does not apply the generated field mask, so
        // a value whose bits the profile declares unmodifiable lands in the
        // register. The mask is not written here -- it is generated from
        // config/csr/mode_m.json -- which is why this control is about the
        // *use* of the mask and not about a table that could drift.
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSTATUS:
          mstatus_d = csr_op_result;
`else
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSTATUS:
          mstatus_d = (mstatus_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_MSTATUS)
                    | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MSTATUS);
`endif
        // F/D (I-050): fflags, frm and fcsr are three addresses over one 8-bit
        // register. A write to one leaves the other field's bits alone, and the
        // field masks come from the generated table rather than from literals.
        // The fflags bits are ORed with the flags of the FP operations that
        // commit in the same cycle (the core orders them against this write)
        // after the case, so a write here and a retiring FP operation cannot
        // lose either one.
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_FCSR:
          fcsr_d = (fcsr_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_FCSR[7:0])
                 | (csr_op_result[7:0] & mosaic_csr_pkg::MOSAIC_CSR_WMASK_FCSR[7:0]);
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_FFLAGS:
          fcsr_d = (fcsr_q & ~{3'b000, mosaic_csr_pkg::MOSAIC_CSR_WMASK_FFLAGS[4:0]})
                 | (csr_op_result[7:0]
                    & {3'b000, mosaic_csr_pkg::MOSAIC_CSR_WMASK_FFLAGS[4:0]});
        // The frm address is a *read/write register of its own*: its three bits
        // are bits 2:0 of the write operand, and they become fcsr[7:5] because
        // that is where frm lives inside the one 8-bit register. Taking the
        // operand's bits 7:5 here -- which is where frm sits inside *fcsr*, but
        // not inside the 0x002 write -- masked every write to zero and made
        // `csrw frm` silently do nothing.
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_FRM:
          fcsr_d = (fcsr_q & ~{mosaic_csr_pkg::MOSAIC_CSR_WMASK_FRM[2:0], 5'b00000})
                 | {(csr_op_result[2:0] & mosaic_csr_pkg::MOSAIC_CSR_WMASK_FRM[2:0]),
                    5'b00000};
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIE:
          mie_d = (mie_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_MIE)
                | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MIE);
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MTVEC:
          mtvec_d = {csr_op_result[63:2], mtvec_mode_canon(csr_op_result[1:0])};
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSCRATCH: mscratch_d = csr_op_result;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MEPC:
          mepc_d = csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MEPC;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCAUSE:  mcause_d = csr_op_result;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MTVAL:   mtval_d = csr_op_result;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCYCLE:  mcycle_d = csr_op_result;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MINSTRET: minstret_d = csr_op_result;
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MEDELEG:
          medeleg_d = (medeleg_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_MEDELEG)
                    | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MEDELEG);
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIDELEG:
          mideleg_d = (mideleg_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_MIDELEG)
                    | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MIDELEG);
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCOUNTEREN:
`ifdef MOSAIC_CSR_MUTANT_MCOUNTEREN_RO
          // NEGATIVE CONTROL: the counter-enable write mask is dropped, so every
          // mcounteren bit stays read-only zero. An S-mode read of an HPM counter
          // shadow is then gated off and traps, which is the defect the Zihpm ACT4
          // ELF names once the HPM counters exist. Reverts the configuration fix
          // (mcounteren_writable_bits = 0xFFFF_FFFF) at the RTL instead.
          mcounteren_d = mcounteren_q;
`else
          mcounteren_d = (mcounteren_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_MCOUNTEREN)
                       | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MCOUNTEREN);
`endif
        `ifdef MOSAIC_CSR_HAS_S
          // sstatus and sie are views of mstatus and mie: the write lands in the
          // register that owns the bit, masked by the view's own writable set.
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SSTATUS:
            mstatus_d = (mstatus_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_SSTATUS)
                      | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_SSTATUS);
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SIE:
            mie_d = (mie_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_SIE)
                  | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_SIE);
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_STVEC:
            stvec_d = {csr_op_result[63:2], mtvec_mode_canon(csr_op_result[1:0])};
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SCOUNTEREN:
            scounteren_d = (scounteren_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_SCOUNTEREN)
                         | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_SCOUNTEREN);
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SENVCFG:
            senvcfg_d = (senvcfg_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_SENVCFG)
                      | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_SENVCFG);
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SSCRATCH: sscratch_d = csr_op_result;
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SEPC:
            sepc_d = csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_SEPC;
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SCAUSE: scause_d = csr_op_result;
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_STVAL:  stval_d  = csr_op_result;
          // satp is WARL (I-045). The honest value set is {0b0000 Bare,
          // 0b1000 Sv39}: this profile implements Sv39 and claims no other
          // paging mode, so a MODE the machine cannot execute must not read
          // back as if it could. The rule is the specification's own for a
          // WARL field with a restricted set -- "a write of any other MODE
          // leaves the entire register unmodified" -- so an unsupported MODE
          // writes nothing rather than silently selecting Sv39 or Bare.
          //
          // ASID is declared unmodifiable (config/csr/mode_su.json,
          // `unmodifiable_bits 59:44`), so the write zeroes it; the PPN is
          // storage and takes the written value. Selecting MODE=Bare is
          // required by software to zero the remaining fields, and the hardware
          // keeps whatever was written, which is one of the behaviours the
          // specification permits.
          // ASID: I-046 implements ASIDLEN = 16 (the spec's ASIDMAX for Sv39),
          // so the field is storage and takes the written value; the TLB
          // compares it exactly. PPN is storage too. The write ignores nothing
          // but an unsupported MODE.
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_SATP: begin
            if ((csr_op_result[63:60] == 4'd0) || (csr_op_result[63:60] == 4'd8)) begin
              satp_d = csr_op_result;
            end else begin
              satp_d = satp_q;
            end
          end
        `endif
        `ifdef MOSAIC_CSR_MUTANT_CYCLE_WRITABLE
          // MUTANT: the read-only shadows write through to the counters.
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_CYCLE:    mcycle_d = csr_op_result;
          mosaic_csr_pkg::MOSAIC_CSR_ADDR_INSTRET:  minstret_d = csr_op_result;
        `endif
        // medeleg and mideleg need no arm here: the generator gives them a write
        // mask of 0, because p0 has no S or U mode for a delegation target to be,
        // so an accepted write canonicalises to zero by construction. mip has no
        // state in this module either -- its write is forwarded on mip_we_o.
        default: ;
      endcase
    end

    `ifdef MOSAIC_CSR_MUTANT_MIE_KEPT
      if (trap_valid_i) begin
        // MUTANT: MPIE is updated but MIE is left set.
        mstatus_d = (mstatus_q & ~MSTATUS_MPIE)
                  | (mstatus_q[3] ? MSTATUS_MPIE : 64'd0)
                  | MSTATUS_MPP;
      end
    `endif

    `ifdef MOSAIC_CSR_MUTANT_MRET_NO_RESTORE
      if (mret_valid_i) begin
        // MUTANT: MPIE <- 1 but MIE is not restored from MPIE.
        mstatus_d = (mstatus_q & ~MSTATUS_MPIE) | MSTATUS_MPIE | MSTATUS_MPP;
      end
    `endif

    `ifdef MOSAIC_CSR_MUTANT_MPP_CLEARED
      if (trap_valid_i) begin
        // MUTANT: MPP is cleared instead of becoming the current privilege.
        mstatus_d = mstatus_d & ~MSTATUS_MPP;
      end
    `endif

    `ifdef MOSAIC_CSR_MUTANT_CYCLE_WRITE_LANDS
      // MUTANT: a write to a read-only cycle/instret shadow is still refused at
      // the port (csr_wr_illegal_o rises, so that report stays correct) but the
      // value lands in the counter anyway. This is the state half of "the
      // counters are writable when they should be a read-only shadow", separated
      // from the report half so the test has to catch the state change itself.
      if (csr_we_i && !trap_valid_i && !mret_valid_i) begin
        if (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_CYCLE) begin
          mcycle_d = csr_op_result;
        end
        if (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_INSTRET) begin
          minstret_d = csr_op_result;
        end
      end
    `endif

    // F/D (I-050): the flags of the FP operations that commit in this cycle are
    // ORed into fflags after any software write, so a `csrw fcsr` and a retiring
    // `fadd` in the same cycle keep both contributions. The core orders the OR
    // against a same-cycle fcsr write by suppressing the flags of operations
    // older than it, so the OR is always the younger contribution.
    fcsr_d[4:0] = fcsr_d[4:0] | fp_fflags_or_i;

    // F/D (I-050): an instruction that modifies FP state sets mstatus.FS =
    // Dirty (11) when it commits. It is a set, never a clear, so a same-cycle
    // software write to mstatus loses to the dirty transition -- the
    // conservative direction. The trap and MRET paths above rewrite only
    // MIE/MPIE/MPP (and SIE/SPIE/SPP), so FS has exactly this one writer besides
    // software.
    if (fp_fs_dirty_i) begin
`ifdef MOSAIC_CSR_MUTANT_FS_NO_DIRTY
      // MUTANT (control for CASE=fp.precise_flags_and_boxing): an instruction
      // that modifies FP state does NOT set mstatus.FS to Dirty, so the machine
      // claims its FP state is clean after an FP write.
`else
      mstatus_d = mstatus_d | MSTATUS_FS;
`endif
    end

    // V (I-059): an instruction that modifies vector state sets mstatus.VS =
    // Dirty when it commits. Same set-never-clear rule as FS.
    if (vec_vs_dirty_i) begin
      mstatus_d = mstatus_d | MSTATUS_VS;
    end
  end

  // ------------------------------------------------------------------ state
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      mstatus_q  <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MSTATUS;
      mie_q      <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MIE;
      mtvec_q    <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MTVEC;
      mscratch_q <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MSCRATCH;
      mepc_q     <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MEPC;
      mcause_q   <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MCAUSE;
      mtval_q    <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MTVAL;
      mcycle_q   <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MCYCLE;
      minstret_q <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MINSTRET;
      medeleg_q  <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MEDELEG;
      mideleg_q  <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MIDELEG;
      mcounteren_q <= mosaic_csr_pkg::MOSAIC_CSR_RESET_MCOUNTEREN;
      fcsr_q     <= mosaic_csr_pkg::MOSAIC_CSR_RESET_FCSR[7:0];
      // The hart starts in M-mode: reset is M-mode by definition ("M-mode is
      // used for low-level access to a hardware platform and is the first mode
      // entered at reset").
      priv_q     <= mosaic_csr_pkg::MOSAIC_PRIV_M;
      o_wr_ctr         <= 32'd0;
      o_illegal_wr_ctr <= 32'd0;
      o_trap_ctr       <= 32'd0;
      o_mret_ctr       <= 32'd0;
      o_sret_ctr       <= 32'd0;
      o_trap_s_ctr     <= 32'd0;
      o_priv_illegal_ctr <= 32'd0;
      o_priv_change_ctr  <= 32'd0;
      `ifdef MOSAIC_CSR_HAS_S
        stvec_q      <= mosaic_csr_pkg::MOSAIC_CSR_RESET_STVEC;
        sscratch_q   <= mosaic_csr_pkg::MOSAIC_CSR_RESET_SSCRATCH;
        sepc_q       <= mosaic_csr_pkg::MOSAIC_CSR_RESET_SEPC;
        scause_q     <= mosaic_csr_pkg::MOSAIC_CSR_RESET_SCAUSE;
        stval_q      <= mosaic_csr_pkg::MOSAIC_CSR_RESET_STVAL;
        satp_q       <= mosaic_csr_pkg::MOSAIC_CSR_RESET_SATP;
        senvcfg_q    <= mosaic_csr_pkg::MOSAIC_CSR_RESET_SENVCFG;
        scounteren_q <= mosaic_csr_pkg::MOSAIC_CSR_RESET_SCOUNTEREN;
      `endif
    end else begin
      mstatus_q  <= mstatus_d;
      mie_q      <= mie_d;
      mtvec_q    <= mtvec_d;
      mscratch_q <= mscratch_d;
      mepc_q     <= mepc_d;
      mcause_q   <= mcause_d;
      mtval_q    <= mtval_d;
      medeleg_q  <= medeleg_d;
      mideleg_q  <= mideleg_d;
      mcounteren_q <= mcounteren_d;
      fcsr_q     <= fcsr_d;
      priv_q     <= priv_d;
      `ifdef MOSAIC_CSR_HAS_S
        stvec_q      <= stvec_d;
        sscratch_q   <= sscratch_d;
        sepc_q       <= sepc_d;
        scause_q     <= scause_d;
        stval_q      <= stval_d;
        satp_q       <= satp_d;
        senvcfg_q    <= senvcfg_d;
        scounteren_q <= scounteren_d;
      `endif
      // A write supplies the value at this edge and the edge's own tick is added
      // on top, so a counter never loses a cycle to an instruction.
      mcycle_q   <= mcycle_d + {63'b0, cnt_cycle_i};
      minstret_q <= minstret_d + {62'b0, cnt_instret_i};

      if (wr_accept)        o_wr_ctr         <= o_wr_ctr + 32'd1;
      if (csr_wr_illegal_o) o_illegal_wr_ctr <= o_illegal_wr_ctr + 32'd1;
      if (trap_valid_i)     o_trap_ctr       <= o_trap_ctr + 32'd1;
      if (mret_commit_o)    o_mret_ctr       <= o_mret_ctr + 32'd1;
      if (sret_commit_o)    o_sret_ctr       <= o_sret_ctr + 32'd1;
      if (trap_valid_i && trap_deleg_o) o_trap_s_ctr <= o_trap_s_ctr + 32'd1;
      // A CSR access refused because the mode is too low is counted apart from
      // one refused because the address is unimplemented: the two are different
      // findings, and a test that could not tell them apart would not be evidence
      // of a privilege check at all. The port has no "this is a CSR instruction"
      // qualifier, so the count is of *writes*; a read refused for privilege is
      // visible in the trap it raises and in `o_illegal_wr_ctr` is not counted
      // here.
      if (csr_we_i && addr_impl && (!csr_priv_ok_r || !csr_priv_ok_w))
        o_priv_illegal_ctr <= o_priv_illegal_ctr + 32'd1;
      if (priv_d != priv_q) o_priv_change_ctr <= o_priv_change_ctr + 32'd1;
    end
  end

  assign pmp_we_o     = wr_accept & pmp_sel_o;
  assign pmp_wdata_o  = csr_op_result;

  // ------------------------------------------------------------ observability
  assign o_priv_o     = priv_q;
  assign o_medeleg_o  = medeleg_q;
  assign o_mideleg_o  = mideleg_q;
  assign o_sstatus_o  = mstatus_q & SSTATUS_FIELDS;
  assign o_sie_o      = mie_q & SIE_VIEW_MASK;
  assign o_sip_o      = mip_i & SIP_VIEW_MASK;
  assign o_mcounteren_o = mcounteren_q;
  `ifdef MOSAIC_CSR_HAS_S
    assign o_stvec_o    = stvec_q;
    assign o_sepc_o     = sepc_q;
    assign o_scause_o   = scause_q;
    assign o_stval_o    = stval_q;
    assign o_sscratch_o = sscratch_q;
    assign o_satp_o     = satp_q;
    assign o_senvcfg_o  = senvcfg_q;
    assign o_scounteren_o = scounteren_q;
  `else
    // A profile with no S-mode has no supervisor trap frame and no satp. The
    // outputs exist because the port list does, and they read as zero rather
    // than as a stale value from a register that does not exist.
    assign o_stvec_o    = 64'd0;
    assign o_sepc_o     = 64'd0;
    assign o_scause_o   = 64'd0;
    assign o_stval_o    = 64'd0;
    assign o_sscratch_o = 64'd0;
    assign o_satp_o     = 64'd0;
    assign o_senvcfg_o  = 64'd0;
    assign o_scounteren_o = 64'd0;
  `endif
  assign o_mstatus_o  = mstatus_q;
  assign o_mtvec_o    = mtvec_q;
  assign o_mepc_o     = mepc_q;
  assign o_mcause_o   = mcause_q;
  assign o_mtval_o    = mtval_q;
  assign o_mscratch_o = mscratch_q;
  assign o_mie_o      = mie_q;
  assign o_mip_o      = mip_i & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MIP;
  assign o_misa_o     = mosaic_csr_pkg::MOSAIC_CSR_RESET_MISA;
  assign o_mcycle_o   = mcycle_q;
  assign o_minstret_o = minstret_q;
  assign o_fcsr_o     = {56'b0, fcsr_q};
  assign o_fflags_o   = fcsr_q[4:0];
  assign o_frm_o      = fcsr_q[7:5];

endmodule : mosaic_csr

`resetall
`default_nettype wire
