// ============================================================================
// mosaic_interrupt -- work package I-020: the M-mode interrupt decision and the
// WFI halt/resume policy, both evaluated only at an architectural boundary.
//
// This module answers two questions and nothing else:
//
//   1. Given the platform's interrupt requests, the mie/mideleg/mstatus.MIE
//      CSR view and the software writes to mip, is there an interrupt that must
//      be taken *now*, and which one?
//   2. Given that a WFI instruction reached the architectural boundary, is the
//      core halted, and has a legal wake event ended the halt?
//
// It does not fetch, decode, retire, or write any CSR. `mip_o` is the pending
// *value*; the CSR file owns the architectural mip state and forwards software
// writes to it here, exactly as the card's interface says.
//
// ------------------------------------------------------------- the boundary
//
// An interrupt is not an instruction: it has no PC and no decode, and taking one
// in the middle of a macro (one instruction that expands into several
// micro-operations) means writing mepc with a half-finished instruction's
// address and losing the rest of it. The decision therefore has to be made at a
// point the core chooses, not at a point the platform chooses -- and the
// platform is asynchronous: a timer fires when it fires.
//
// Two separate mechanisms follow, and both are explicit here.
//
//   * The three sources are synchronised before anything reads them. The card
//     asks for the metastability model to be stated rather than implied, so:
//     this is a discrete-time model, not an analog one. The synchroniser samples
//     each source at every rising clock edge; a source pulse that does not span
//     a sampling edge is never observed, and one that spans exactly one edge is
//     observed as a one-cycle pulse two edges later (first flop, then second).
//     A discrete simulation has no metastability to resolve, so the property the
//     test exercises is the sampling property: a sub-cycle glitch produces no
//     pending bit at all, and the latency of a real level is exactly two edges.
//     Removing the synchroniser is a mutant, and it is caught by cycle, not by a
//     statistical argument.
//
//   * `irq_valid_o` is gated by `core_can_trap_i`, an input this module adds to
//     the interface the card froze. The card names the decision outputs and says
//     the boundary state is the implementer's decision ("the cleanest is an
//     input `core_can_trap_i`"); this is that input. The integrator raises it
//     when the retire boundary is at a legal instruction boundary -- not
//     mid-macro, not while a trap is already being taken -- and the module is
//     then allowed to offer a trap. `irq_valid_o` is *never* high while it is
//     low, which the testbench enforces as a standing invariant, not a comment:
//
//         irq_valid_o ==> core_can_trap_i & mstatus.MIE & (mip & mie & ~mideleg)
//
// -------------------------------------------------- mip and the software latch
//
// mip is not a pure read of the platform wires. config/csr/mode_m.json declares
// mip bits 7 and 3 writable (MTIP and MSIP), and a software-writable pending bit
// has to behave like a real one: the privileged spec says a pending interrupt i
// can be cleared by writing 0 to this bit. The software latch below is therefore
// a first-class pending source, ORed with the synchronised platform request
// rather than replacing it, so that
//
//     * software raising MTIP with the platform timer idle is a pending timer
//       interrupt the core will take, and
//     * a platform request cannot be cleared by writing 0 to mip,
//
// which are the two behaviours a "write mip" path exists for. Bit 11 (MEIP) is
// read-only: only the platform can assert it, so a write to it is ignored.
//
// The writable mask `MIP_WRITABLE_MASK` -- bits 7 and 3 -- is a localparam
// rather than a generated-package constant because the manifest generator emits
// geometry, not CSR field legality; config/csr/mode_m.json is the authority and
// is named where the mask is declared. If a profile ever made another mip bit
// writable, this mask and the CSR file's field table would have to move
// together, and that coupling is the one thing here worth reviewing.
//
// ------------------------------------------------------- priority, delegation
//
// Privileged spec v1.12 (machine.tex, "Machine Interrupt Registers"): when
// several interrupts are pending and enabled, the one with the highest priority
// is taken, and the standard order is
//
//     MEI (code 11)  >  MSI (code 3)  >  MTI (code 7)
//
// which is exactly why the cause codes are not in priority order. This module
// implements that order, highest first, and `irq_cause_o` reports the winner
// with bit 63 set (the interrupt bit of mcause) and the exception code in bits
// 62:0.
//
// mideleg clears the *taken* condition for its bits. p0 is M-only and
// config_check pins mideleg to zero, but the gate is implemented rather than
// assumed: a delegated bit is not this module's trap to take, so it is excluded
// from the decision. It is deliberately *not* excluded from `mip_o`, because
// delegation changes who handles a pending interrupt, not whether one is
// pending.
//
// --------------------------------------------------------------------- WFI
//
// Privileged spec v1.12 (machine.tex, "Wait for Interrupt"): if an enabled
// interrupt is present, or later becomes present while the hart is stalled, the
// interrupt trap is taken -- and this is true even if mstatus.MIE=0. Three
// consequences, all implemented literally:
//
//   * the wake condition is `(mie_i & mip) != 0` -- an enabled pending
//     interrupt, with no mstatus.MIE term, because WFI completes whatever the
//     global enable says and the trap that follows is then subject to MIE like
//     any other;
//   * if such an interrupt is *already* pending when the WFI reaches the
//     boundary, the core does not halt at all: `wfi_valid_i` with an enabled
//     pending interrupt leaves `wfi_halt_o` low;
//   * a pending but *disabled* interrupt must not wake the core. That is the
//     classic WFI bug (test "some pending bit" instead of "pending and
//     enabled"), and `o_spurious_wake_ctr` exists to make it countable rather
//     than invisible: leaving the halt without the architectural wake rule is
//     counted as a spurious wake and must never happen.
//
// Delegation is deliberately not part of the wake condition: a delegated
// interrupt is still an enabled interrupt and still ends the wait; delegation
// only decides which mode takes the resulting trap. In p0 the distinction is
// unobservable because mideleg is zero.
//
// ------------------------------------------------------------------ counters
//
// Four saturating 8-bit counters, because a wrapping byte counter turns a large
// count into a plausible small one. They saturate at 255 and the testbench keeps
// every directed phase's counts below that ceiling, so "saturated" and "wrapped"
// can never be confused with each other.
//
//   o_irq_ctr            cycles in which irq_valid_o was high (a trap offered)
//   o_halt_cycles        completed cycles spent with the halt asserted
//   o_wake_ctr           legal wakes: a halt ended by an enabled pending irq
//   o_spurious_wake_ctr  halts ended by anything else -- must stay 0
//
// The spurious-wake monitor re-derives the architectural wake rule rather than
// reading the halt-clear path's own condition, so a change to the clear path
// that is not the architectural rule is *counted* instead of being silently
// consistent with itself. A wake reported while the halt was never entered is
// impossible in the first place -- the wake counter is gated by the halt being
// asserted -- which is the property that keeps `o_wake_ctr` meaningful.
//
// ------------------------------------------------------------------- mutants
//
// -DMOSAIC_INTERRUPT_MUTANT_<n> injects one defect, used to prove this case can
// fail. The shipping build defines none of them; the table with real output is
// in results/reports/I-020-interrupt.md.
// ============================================================================

`default_nettype none
`resetall

// `mosaic_pkg` is pulled in here rather than assumed to be on the command line,
// because two tools build this file and they disagree about the source list:
// tools/run_unit.py compiles exactly the files CASE=interrupt.boundary_replay
// lists in tests/unit/registry.json (only this one), while `make lint-slang`
// compiles every rtl/*/*.sv in a single unit sorted by name, which puts this
// file *before* mosaic_pkg.sv. mosaic_pkg.sv carries its own include guard, so
// when the linter also passes it as a separate source the second inclusion is a
// no-op rather than a duplicate package definition.
`include "mosaic_pkg.sv"

module mosaic_interrupt (
    input  logic                 clk_i,
    input  logic                 rst_i,

    // ------------------------------------------------ platform interrupt sources
    // Level-sensitive and asynchronous in real time; sampled and synchronised
    // inside. In the p0 memory map these are CLINT msip (software), the CLINT
    // timer comparison mtime >= mtimecmp (timer) and the platform interrupt
    // controller output (external).
    input  logic                 irq_soft_i,
    input  logic                 irq_timer_i,
    input  logic                 irq_ext_i,

    // ---------------------------------------------------------------- CSR view
    input  logic [63:0]          mie_i,
    input  logic [63:0]          mideleg_i,
    input  logic                 mstatus_mie_i,

    // Software writes to mip[7]/mip[3], forwarded from mosaic_csr. They take
    // effect on the same clock edge the CSR file's own write does, which is the
    // edge at which the CSR would latch its value.
    input  logic                 mip_we_i,
    input  mosaic_pkg::csr_op_e  mip_op_i,
    input  logic [63:0]          mip_wdata_i,
    output logic [63:0]          mip_o,

    // ------------------------------------------------------- decision boundary
    // `core_can_trap_i` is the architectural boundary: the integrator holds it
    // low in the middle of a macro and while a trap is already being taken, and
    // the module may only offer a trap when it is high. See the header.
    input  logic                 core_can_trap_i,
    output logic                 irq_valid_o,
    output logic [63:0]          irq_cause_o,

    // Pending flags, mirroring the mip bits they are named for. Not gated: a
    // pending interrupt is pending whether or not it is enabled or delegated.
    output logic                 irq_timer_pending_o,
    output logic                 o_irq_soft_pending,
    output logic                 o_irq_ext_pending,

    // -------------------------------------------------------------------- WFI
    input  logic                 wfi_valid_i,
    output logic                 wfi_halt_o,

    // ---------------------------------------------------------------- counters
    output logic [7:0]           o_irq_ctr,
    output logic [7:0]           o_halt_cycles,
    output logic [7:0]           o_wake_ctr,
    output logic [7:0]           o_spurious_wake_ctr
);

  // Interrupt cause codes. These are ISA constants from the privileged spec's
  // mcause table, not geometry, so they are not in the generated package. Bit 63
  // is mcause's Interrupt bit; the code sits in bits 62:0.
  localparam logic [5:0]  IRQ_CODE_MSI = 6'd3;   // machine software interrupt
  localparam logic [5:0]  IRQ_CODE_MTI = 6'd7;   // machine timer interrupt
  localparam logic [5:0]  IRQ_CODE_MEI = 6'd11;  // machine external interrupt

  // Built by concatenation from the codes above, so the code is written down
  // once: bit 63 set, bits 62:6 zero, the code in bits 5:0.
  localparam logic [63:0] CAUSE_MSI = {1'b1, 57'd0, IRQ_CODE_MSI};
  localparam logic [63:0] CAUSE_MTI = {1'b1, 57'd0, IRQ_CODE_MTI};
  localparam logic [63:0] CAUSE_MEI = {1'b1, 57'd0, IRQ_CODE_MEI};

  // mip/mie bit positions, from the mcause table (cause number i is bit i in
  // both registers).
  localparam int unsigned BIT_MSIP = 3;
  localparam int unsigned BIT_MTIP = 7;
  localparam int unsigned BIT_MEIP = 11;

  // The software-writable mip bits: config/csr/mode_m.json, mip
  // "writable_fields": ["7", "3"]. Bits outside this mask are read-only here.
  localparam logic [63:0] MIP_WRITABLE_MASK = 64'h0000_0000_0000_0088;

  // Index of each source inside the synchroniser vector, named so the packing
  // below and the unpacking further down cannot drift apart.
  localparam int unsigned SYNC_SOFT  = 0;
  localparam int unsigned SYNC_TIMER = 1;
  localparam int unsigned SYNC_EXT   = 2;

  localparam logic [7:0] CNT_MAX = 8'hff;

  // --------------------------------------------------------------------------
  // Source synchronisation. Two flops per source, with no logic between them: a
  // single flop samples an asynchronous input into the clock domain but leaves
  // the sampling decision to a metastable race, which is what the second flop
  // exists to settle. The value the rest of the module reads is always the
  // second flop's output, so the decision logic sees one value per clock and
  // never a value that could still change.
  // --------------------------------------------------------------------------
  logic [2:0] irq_src_raw;
  logic [2:0] irq_sync_meta;
  logic [2:0] irq_sync_q;

  assign irq_src_raw = {irq_ext_i, irq_timer_i, irq_soft_i};

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      irq_sync_meta <= 3'd0;
      irq_sync_q    <= 3'd0;
    end else begin
`ifdef MOSAIC_INTERRUPT_MUTANT_NO_SYNC
      // Mutant: the second flop is gone. The raw asynchronous source drives the
      // decision logic directly, so a pending bit appears a cycle early and a
      // sub-cycle glitch is observed as if it had been sampled.
      irq_sync_meta <= irq_src_raw;
      irq_sync_q    <= irq_src_raw;
`else
      irq_sync_meta <= irq_src_raw;
      irq_sync_q    <= irq_sync_meta;
`endif
    end
  end

  logic platform_msip;
  logic platform_mtip;
  logic platform_meip;

  assign platform_msip = irq_sync_q[SYNC_SOFT];
  assign platform_mtip = irq_sync_q[SYNC_TIMER];
  assign platform_meip = irq_sync_q[SYNC_EXT];

  // --------------------------------------------------------------------------
  // The software latch. CSR_RW replaces the bit, CSR_RS sets it, CSR_RC clears
  // it -- the standard CSR write semantics, applied only to the writable bits.
  // It is a plain set/reset latch, not a one-cycle pulse: software that raises
  // MTIP and never clears it keeps seeing a pending timer interrupt, which is
  // what makes it a testable path rather than a nudge.
  // --------------------------------------------------------------------------
  logic sw_msip_q;
  logic sw_mtip_q;
  logic sw_msip_d;
  logic sw_mtip_d;
  logic [63:0] mip_write_data;

  // A write to a read-only bit is not a write: the mask is the one place the
  // writable set is written down, and the two latch bits below are its two
  // members. MEIP, for example, cannot be moved by software.
  assign mip_write_data = mip_wdata_i & MIP_WRITABLE_MASK;

  always_comb begin
    sw_msip_d = sw_msip_q;
    sw_mtip_d = sw_mtip_q;
    if (mip_we_i) begin
      case (mip_op_i)
        mosaic_pkg::CSR_RW: begin
          sw_msip_d = mip_write_data[BIT_MSIP];
          sw_mtip_d = mip_write_data[BIT_MTIP];
        end
        mosaic_pkg::CSR_RS: begin
          sw_msip_d = sw_msip_q | mip_write_data[BIT_MSIP];
          sw_mtip_d = sw_mtip_q | mip_write_data[BIT_MTIP];
        end
        mosaic_pkg::CSR_RC: begin
          sw_msip_d = sw_msip_q & ~mip_write_data[BIT_MSIP];
          sw_mtip_d = sw_mtip_q & ~mip_write_data[BIT_MTIP];
        end
        default: begin
          // CSR_NONE is not a write; the latch holds.
        end
      endcase
    end
  end

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      sw_msip_q <= 1'b0;
      sw_mtip_q <= 1'b0;
    end else begin
`ifdef MOSAIC_INTERRUPT_MUTANT_IGNORE_SW_WRITE
      // Mutant: the write is decoded and then dropped, so software can no
      // longer raise a pending interrupt.
      sw_msip_q <= sw_msip_q;
      sw_mtip_q <= sw_mtip_q;
`else
      sw_msip_q <= sw_msip_d;
      sw_mtip_q <= sw_mtip_d;
`endif
    end
  end

  // --------------------------------------------------------------------------
  // mip: platform pending OR software latch, on exactly the bits the platform
  // and the config declare to exist. Everything else reads zero -- bits 15:8
  // other than 11 and bits 6:4, 2:0 are WPRI in config/csr/mode_m.json and are
  // therefore read-only zero here, which is also what an implementation that has
  // no such interrupt must return.
  // --------------------------------------------------------------------------
  logic [63:0] mip_comb;

  always_comb begin
    mip_comb = 64'd0;
    mip_comb[BIT_MSIP] = platform_msip | sw_msip_q;
    mip_comb[BIT_MTIP] = platform_mtip | sw_mtip_q;
    mip_comb[BIT_MEIP] = platform_meip;
  end

  assign mip_o = mip_comb;

  assign irq_timer_pending_o = mip_comb[BIT_MTIP];
  assign o_irq_soft_pending  = mip_comb[BIT_MSIP];
  assign o_irq_ext_pending   = mip_comb[BIT_MEIP];

  // --------------------------------------------------------------------------
  // The decision. `take_pending` is the set of interrupts that both a platform
  // or software has raised and software has enabled and has not delegated; the
  // highest-priority member of that set wins.
  // --------------------------------------------------------------------------
  logic [63:0] enabled_pending;
  logic [63:0] take_pending;
  logic        take_mei;
  logic        take_msi;
  logic        take_mti;
  logic        any_take;

  assign enabled_pending = mip_comb & mie_i;
  assign take_pending    = enabled_pending & ~mideleg_i;

  assign take_mei = take_pending[BIT_MEIP];
  assign take_msi = take_pending[BIT_MSIP];
  assign take_mti = take_pending[BIT_MTIP];
  assign any_take = take_mei | take_msi | take_mti;

  // MEI > MSI > MTI, per the privileged spec's standard priority order. The
  // cause is the winning *candidate* -- zero when no enabled, non-delegated
  // pending bit exists at all -- and it deliberately does not depend on
  // mstatus.MIE or on core_can_trap_i. Those two are the gates that decide
  // whether a trap may be taken now (`irq_valid_o`), while the cause stays
  // stable across a blocked window, so a core that latches the cause when the
  // boundary opens cannot latch a value that was invalidated by the wait.
  always_comb begin
    irq_cause_o = 64'd0;
`ifdef MOSAIC_INTERRUPT_MUTANT_PRIORITY_REVERSED
    // Mutant: the order is inverted.
    if (take_mti) begin
      irq_cause_o = CAUSE_MTI;
    end else if (take_msi) begin
      irq_cause_o = CAUSE_MSI;
    end else if (take_mei) begin
      irq_cause_o = CAUSE_MEI;
    end
`else
    if (take_mei) begin
      irq_cause_o = CAUSE_MEI;
    end else if (take_msi) begin
      irq_cause_o = CAUSE_MSI;
    end else if (take_mti) begin
      irq_cause_o = CAUSE_MTI;
    end
`endif
  end

`ifdef MOSAIC_INTERRUPT_MUTANT_IGNORE_MIDELEG
  // Mutant: the delegation gate is dropped, so a delegated interrupt is taken
  // as if it were not delegated.
  logic [63:0] take_pending_gated;
  assign take_pending_gated = enabled_pending;
  logic        any_take_gated;
  assign any_take_gated = take_pending_gated[BIT_MEIP]
                        | take_pending_gated[BIT_MSIP]
                        | take_pending_gated[BIT_MTIP];
`endif

`ifdef MOSAIC_INTERRUPT_MUTANT_MIE_IGNORED
  // Mutant: the global enable is dropped, so a masked interrupt is offered.
  assign irq_valid_o = core_can_trap_i & any_take;
`elsif MOSAIC_INTERRUPT_MUTANT_IGNORE_MIDELEG
  assign irq_valid_o = core_can_trap_i & mstatus_mie_i & any_take_gated;
`elsif MOSAIC_INTERRUPT_MUTANT_IRQ_OUTSIDE_BOUNDARY
  // Mutant: the architectural boundary is ignored and a trap is offered as
  // soon as an enabled interrupt is pending.
  assign irq_valid_o = mstatus_mie_i & any_take;
`else
  assign irq_valid_o = core_can_trap_i & mstatus_mie_i & any_take;
`endif

  // --------------------------------------------------------------------------
  // WFI. `wake_legal` is the architectural wake rule. The halt-clear path is a
  // separate signal, `halt_clear_now`, rather than an alias used in both places:
  // `spurious_wake` below is computed from `wake_legal`, never from
  // `halt_clear_now`, so a halt that ends for a reason other than the
  // architectural rule is *counted* instead of being consistent with itself by
  // construction. A "wake" reported while the halt was never entered is
  // impossible for the same reason it is a bug: the wake counter is gated by
  // `halted_q`.
  // --------------------------------------------------------------------------
  logic wake_legal;
  logic halt_clear_now;
  logic halted_q;
  logic halted_d;

  assign wake_legal = |enabled_pending;

`ifdef MOSAIC_INTERRUPT_MUTANT_WFI_WAKE_DISABLED
  // Mutant: any pending bit ends the halt, enabled or not -- the classic WFI
  // bug. The monitor below still uses the architectural rule, so it counts.
  assign halt_clear_now = |mip_comb;
`else
  assign halt_clear_now = wake_legal;
`endif

  // An enabled pending interrupt present when the WFI reaches the boundary
  // leaves the core running: the first branch is not taken (halt_clear_now is
  // high) and the second keeps the halt low.
  always_comb begin
    if (wfi_valid_i && !halt_clear_now) begin
      halted_d = 1'b1;
    end else if (halt_clear_now) begin
      halted_d = 1'b0;
    end else begin
      halted_d = halted_q;
    end
  end

  assign wfi_halt_o = halted_q;

  logic halt_leave;
  logic legal_wake;
  logic spurious_wake;

  assign halt_leave    = halted_q & ~halted_d;
  assign legal_wake    = halt_leave &  wake_legal;
  assign spurious_wake = halt_leave & ~wake_legal;

  // Saturating counters. See the header for why saturation rather than wrap.
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      halted_q            <= 1'b0;
      o_irq_ctr           <= 8'd0;
      o_halt_cycles       <= 8'd0;
      o_wake_ctr          <= 8'd0;
      o_spurious_wake_ctr <= 8'd0;
    end else begin
      halted_q <= halted_d;

      if (irq_valid_o) begin
        o_irq_ctr <= (o_irq_ctr == CNT_MAX) ? CNT_MAX : (o_irq_ctr + 8'd1);
      end

      // Counts *completed* cycles: the halt was already asserted when this
      // cycle began. The cycle in which WFI sets the halt did not run halted.
      if (halted_q) begin
        o_halt_cycles <= (o_halt_cycles == CNT_MAX) ? CNT_MAX
                                                    : (o_halt_cycles + 8'd1);
      end

      if (legal_wake) begin
        o_wake_ctr <= (o_wake_ctr == CNT_MAX) ? CNT_MAX : (o_wake_ctr + 8'd1);
      end

      if (spurious_wake) begin
        o_spurious_wake_ctr <= (o_spurious_wake_ctr == CNT_MAX) ? CNT_MAX
                                                                : (o_spurious_wake_ctr + 8'd1);
      end
    end
  end

endmodule : mosaic_interrupt

`resetall
`default_nettype wire
