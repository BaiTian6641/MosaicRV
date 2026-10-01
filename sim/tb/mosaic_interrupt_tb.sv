// Simulation wrapper for CASE=interrupt.boundary_replay (work package I-020).
//
// `mosaic_interrupt` is a single-clock module whose decision outputs are
// combinational and whose halt state and counters are registered. This wrapper
// passes every port straight through: it adds no clock generation, no reset
// generation, no `$display` and no timing of its own, because the C++ driver
// owns the clock schedule, the stimulus timeline and all result reporting, per
// sim/common/sim_common.h.
//
// The one detail worth naming is `mip_op`: the DUT port is
// `mosaic_pkg::csr_op_e`, and the wrapper declares the same type rather than a
// bare 2-bit vector, so the C++ driver cannot hand the DUT an encoding the DUT's
// own case statement does not know. The package is declared by
// mosaic_interrupt.sv, which is always compiled ahead of this file in the case's
// source list.

`default_nettype none
`resetall

module mosaic_interrupt_tb (
    input  logic                 clk,
    input  logic                 rst,

    // platform interrupt sources
    input  logic                 irq_soft,
    input  logic                 irq_timer,
    input  logic                 irq_ext,

    // CSR view
    input  logic [63:0]          mie,
    input  logic [63:0]          mideleg,
    input  logic                 mstatus_mie,

    // software write to mip
    input  logic                 mip_we,
    input  mosaic_pkg::csr_op_e  mip_op,
    input  logic [63:0]          mip_wdata,

    // architectural boundary
    input  logic                 core_can_trap,

    // WFI
    input  logic                 wfi_valid,

    // decision outputs
    output logic [63:0]          mip,
    output logic                 irq_valid,
    output logic [63:0]          irq_cause,
    output logic                 irq_timer_pending,
    output logic                 irq_soft_pending,
    output logic                 irq_ext_pending,
    output logic                 wfi_halt,

    // status counters
    output logic [7:0]           o_irq_ctr,
    output logic [7:0]           o_halt_cycles,
    output logic [7:0]           o_wake_ctr,
    output logic [7:0]           o_spurious_wake_ctr
);

  mosaic_interrupt u_int (
      .clk_i                (clk),
      .rst_i                (rst),

      .irq_soft_i           (irq_soft),
      .irq_timer_i          (irq_timer),
      .irq_ext_i            (irq_ext),

      .mie_i                (mie),
      .mideleg_i            (mideleg),
      .mstatus_mie_i        (mstatus_mie),

      .mip_we_i             (mip_we),
      .mip_op_i             (mip_op),
      .mip_wdata_i          (mip_wdata),
      .mip_o                (mip),

      .core_can_trap_i      (core_can_trap),
      .irq_valid_o          (irq_valid),
      .irq_cause_o          (irq_cause),

      .irq_timer_pending_o  (irq_timer_pending),
      .o_irq_soft_pending   (irq_soft_pending),
      .o_irq_ext_pending    (irq_ext_pending),

      .wfi_valid_i          (wfi_valid),
      .wfi_halt_o           (wfi_halt),

      .o_irq_ctr            (o_irq_ctr),
      .o_halt_cycles        (o_halt_cycles),
      .o_wake_ctr           (o_wake_ctr),
      .o_spurious_wake_ctr  (o_spurious_wake_ctr)
  );

endmodule : mosaic_interrupt_tb

`resetall
`default_nettype wire
