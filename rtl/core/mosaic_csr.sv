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
//   * mepc (0x341). p0 has fixed IALIGN=32, so mepc[1:0] are read-only zero
//     (L1935-L1937). The generated write mask already excludes them, and the
//     trap path masks the trap PC with the same mask, so a trap cannot install a
//     misaligned PC either.
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
// mcycle and minstret are free-running: cnt_cycle_i / cnt_instret_i advance them
// by one on every rising edge, by definition ("The mcycle CSR counts the number
// of clock cycles executed by the processor core", L1650-L1655). A software write
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
// write-legality flag per CSR, decoded from config/csr/mode_m.json.
`include "mosaic_csr_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_csr (
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
    input  logic                   cnt_instret_i,

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

    // interrupt pending view owned by mosaic_interrupt (I-020)
    input  logic [63:0]            mip_i,
    output logic                   mip_we_o,
    output logic [1:0]             mip_op_o,
    output logic [63:0]            mip_wdata_o,

    // platform time
    input  logic [63:0]            mtime_i,

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
    output logic [31:0]            o_mret_ctr
);

  // --------------------------------------------------------------- constants
  //
  // mstatus field positions from the RV64 layout the clause cites (bit-number
  // rows L448-L495, field-name rows L462-L474): MIE = bit 3, MPIE = bit 7,
  // MPP = bits 12:11. Writability is not decided here -- the generated mask is.
  localparam logic [63:0] MSTATUS_MIE  = 64'h0000_0000_0000_0008;
  localparam logic [63:0] MSTATUS_MPIE = 64'h0000_0000_0000_0080;
  localparam logic [63:0] MSTATUS_MPP  = 64'h0000_0000_0000_1800;

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

  logic [63:0] mstatus_d;
  logic [63:0] mie_d;
  logic [63:0] mtvec_d;
  logic [63:0] mscratch_d;
  logic [63:0] mepc_d;
  logic [63:0] mcause_d;
  logic [63:0] mtval_d;
  logic [63:0] mcycle_d;
  logic [63:0] minstret_d;

  // ------------------------------------------------------------------ decode
  //
  // One decode answers both port questions: is the address implemented at all
  // (read legality) and does its access mode permit a write (write legality).
  // `wr_legal` is the generated access mode, not a list written here, so a CSR
  // the table marks read-only cannot become writable by editing this file.
  logic addr_impl;
  logic wr_legal;

  always_comb begin
    addr_impl = 1'b1;
    wr_legal  = 1'b0;
    case (csr_addr_i)
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

  assign csr_illegal_o    = ~addr_impl;
  assign csr_wr_illegal_o = csr_we_i & (~addr_impl | ~wr_legal);

  // A write that is accepted by the ports and not pre-empted by the boundary.
  // The trap/mret terms are what make a trap outrank a retiring CSR instruction.
  logic wr_accept;
  assign wr_accept = csr_we_i & addr_impl & wr_legal & ~trap_valid_i & ~mret_valid_i;

  // The two boundary events cannot both be true at one architectural boundary.
  // The hardware resolves a simultaneous request in favour of the trap (below);
  // this assertion makes a caller that presents both visible instead of letting
  // the resolution hide it.
  always_comb begin
    assert (!(trap_valid_i & mret_valid_i));
  end

  // -------------------------------------------------------------------- read
  logic [63:0] csr_rdata_stored;

  always_comb begin
    case (csr_addr_i)
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSTATUS:    csr_rdata_stored = mstatus_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MISA:       csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MISA;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MEDELEG:    csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MEDELEG;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIDELEG:    csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MIDELEG;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIE:        csr_rdata_stored = mie_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MTVEC:      csr_rdata_stored = mtvec_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCOUNTEREN: csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MCOUNTEREN;
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
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MHARTID:    csr_rdata_stored = mosaic_csr_pkg::MOSAIC_CSR_RESET_MHARTID;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MCYCLE:     csr_rdata_stored = mcycle_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_MINSTRET:   csr_rdata_stored = minstret_q;
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_CYCLE:      csr_rdata_stored = mcycle_q;   // read-only shadow
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_TIME:       csr_rdata_stored = mtime_i;    // read-only shadow
      mosaic_csr_pkg::MOSAIC_CSR_ADDR_INSTRET:    csr_rdata_stored = minstret_q; // read-only shadow
      default:                    csr_rdata_stored = 64'b0;
    endcase
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
  assign mret_commit_o = mret_valid_i & ~trap_valid_i;

  // MRET targets mepc exactly; the low bits are already zero by construction.
  assign mret_target_o = mepc_q;

  // An interrupt (mcause[63] set) taken in Vectored mode enters at
  // base + 4 * cause code; every other trap enters at the base (Table mtvec MODE
  // and the table's direct/vectored description).
  always_comb begin
    if ((mtvec_q[1:0] == 2'b01) && trap_cause_i[63]) begin
      trap_target_o = {mtvec_q[63:2], 2'b00} + {trap_cause_i[61:0], 2'b00};
    end else begin
      trap_target_o = {mtvec_q[63:2], 2'b00};
    end
  end

  // -------------------------------------------------------------- mip forward
  assign mip_we_o    = wr_accept & (csr_addr_i == mosaic_csr_pkg::MOSAIC_CSR_ADDR_MIP);
  assign mip_op_o    = csr_op_i;
  assign mip_wdata_o = csr_wdata_i;

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

    if (trap_valid_i) begin
      // Trap entry: MPIE <- MIE, MIE <- 0, MPP <- current privilege (M = 3,
      // which is also what the read-only MPP field must hold); mepc <- the
      // interrupted PC with its read-only low bits forced zero; mcause <- the
      // cause; mtval <- the trap value.
      mstatus_d = (mstatus_q & ~(MSTATUS_MIE | MSTATUS_MPIE))
                | (mstatus_q[3] ? MSTATUS_MPIE : 64'd0)
                | MSTATUS_MPP;
      mepc_d    = trap_epc_i & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MEPC;
      mcause_d  = trap_cause_i;
      mtval_d   = trap_tval_i;
    end else if (mret_valid_i) begin
      // MRET: MIE <- MPIE, MPIE <- 1, MPP <- the least-privileged supported mode
      // (M = 3 in p0). L2.1.6.1 of the privileged spec for the field rules.
      mstatus_d = (mstatus_q & ~(MSTATUS_MIE | MSTATUS_MPIE))
                | (mstatus_q[7] ? MSTATUS_MIE : 64'd0)
                | MSTATUS_MPIE
                | MSTATUS_MPP;
    end else if (wr_accept) begin
      case (csr_addr_i)
        mosaic_csr_pkg::MOSAIC_CSR_ADDR_MSTATUS:
          mstatus_d = (mstatus_q & ~mosaic_csr_pkg::MOSAIC_CSR_WMASK_MSTATUS)
                    | (csr_op_result & mosaic_csr_pkg::MOSAIC_CSR_WMASK_MSTATUS);
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
      o_wr_ctr         <= 32'd0;
      o_illegal_wr_ctr <= 32'd0;
      o_trap_ctr       <= 32'd0;
      o_mret_ctr       <= 32'd0;
    end else begin
      mstatus_q  <= mstatus_d;
      mie_q      <= mie_d;
      mtvec_q    <= mtvec_d;
      mscratch_q <= mscratch_d;
      mepc_q     <= mepc_d;
      mcause_q   <= mcause_d;
      mtval_q    <= mtval_d;
      // A write supplies the value at this edge and the edge's own tick is added
      // on top, so a counter never loses a cycle to an instruction.
      mcycle_q   <= mcycle_d + {63'b0, cnt_cycle_i};
      minstret_q <= minstret_d + {63'b0, cnt_instret_i};

      if (wr_accept)        o_wr_ctr         <= o_wr_ctr + 32'd1;
      if (csr_wr_illegal_o) o_illegal_wr_ctr <= o_illegal_wr_ctr + 32'd1;
      if (trap_valid_i)     o_trap_ctr       <= o_trap_ctr + 32'd1;
      if (mret_commit_o)    o_mret_ctr       <= o_mret_ctr + 32'd1;
    end
  end

  // ------------------------------------------------------------ observability
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

endmodule : mosaic_csr

`resetall
`default_nettype wire
