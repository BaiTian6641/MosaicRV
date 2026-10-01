// Simulation wrapper for CASE=csr.precise_trap_mret (work package I-019).
//
// The wrapper adds no timing of its own. It presents every `mosaic_csr` port as a
// plain vector so the C++ driver can drive and observe it without knowing
// anything about the DUT's internal types, and it converts the one enum port
// (`csr_op_i`) at this boundary. There is no clock generation, no reset
// generation and no `$display` here: the C++ side owns all three, per
// sim/common/sim_common.h.
//
// `mosaic_pkg` is not re-included: the runner places rtl/core/mosaic_csr.sv,
// which includes it, before this file, and the package carries an include guard.
// The one reference to it is the cast below, fully qualified as the project
// requires (no `import`).
//
// The DUT is registered on its inputs and combinational on its outputs, so the
// driver drives the ports while the clock is low, evaluates, compares every
// output against its shadow, and then applies the edge.

`default_nettype none
`resetall

module mosaic_csr_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------------------ drive
    input  logic [11:0] csr_addr_i,
    input  logic        csr_we_i,
    input  logic [1:0]  csr_op_i,
    input  logic [63:0] csr_wdata_i,

    input  logic        cnt_cycle_i,
    input  logic        cnt_instret_i,

    input  logic        trap_valid_i,
    input  logic [63:0] trap_cause_i,
    input  logic [63:0] trap_tval_i,
    input  logic [63:0] trap_epc_i,

    input  logic        mret_valid_i,

    input  logic [63:0] mip_i,
    input  logic [63:0] mtime_i,

    // ---------------------------------------------------------------- observe
    output logic [63:0] csr_rdata_o,
    output logic        csr_illegal_o,
    output logic        csr_wr_illegal_o,

    output logic        trap_commit_o,
    output logic [63:0] trap_target_o,
    output logic        mret_commit_o,
    output logic [63:0] mret_target_o,

    output logic        mip_we_o,
    output logic [1:0]  mip_op_o,
    output logic [63:0] mip_wdata_o,

    output logic [63:0] o_mstatus_o,
    output logic [63:0] o_mtvec_o,
    output logic [63:0] o_mepc_o,
    output logic [63:0] o_mcause_o,
    output logic [63:0] o_mtval_o,
    output logic [63:0] o_mscratch_o,
    output logic [63:0] o_mie_o,
    output logic [63:0] o_mip_o,
    output logic [63:0] o_misa_o,
    output logic [63:0] o_mcycle_o,
    output logic [63:0] o_minstret_o,
    output logic [31:0] o_wr_ctr,
    output logic [31:0] o_illegal_wr_ctr,
    output logic [31:0] o_trap_ctr,
    output logic [31:0] o_mret_ctr
);

  mosaic_pkg::csr_op_e csr_op;

  assign csr_op = mosaic_pkg::csr_op_e'(csr_op_i);

  mosaic_csr u_csr (
      .clk_i           (clk),
      .rst_i           (rst),

      .csr_addr_i      (csr_addr_i),
      .csr_rdata_o     (csr_rdata_o),
      .csr_illegal_o   (csr_illegal_o),

      .csr_we_i        (csr_we_i),
      .csr_op_i        (csr_op),
      .csr_wdata_i     (csr_wdata_i),
      .csr_wr_illegal_o(csr_wr_illegal_o),

      .cnt_cycle_i     (cnt_cycle_i),
      .cnt_instret_i   (cnt_instret_i),

      .trap_valid_i    (trap_valid_i),
      .trap_cause_i    (trap_cause_i),
      .trap_tval_i     (trap_tval_i),
      .trap_epc_i      (trap_epc_i),
      .trap_commit_o   (trap_commit_o),
      .trap_target_o   (trap_target_o),

      .mret_valid_i    (mret_valid_i),
      .mret_commit_o   (mret_commit_o),
      .mret_target_o   (mret_target_o),

      .mip_i           (mip_i),
      .mip_we_o        (mip_we_o),
      .mip_op_o        (mip_op_o),
      .mip_wdata_o     (mip_wdata_o),

      .mtime_i         (mtime_i),

      .o_mstatus_o     (o_mstatus_o),
      .o_mtvec_o       (o_mtvec_o),
      .o_mepc_o        (o_mepc_o),
      .o_mcause_o      (o_mcause_o),
      .o_mtval_o       (o_mtval_o),
      .o_mscratch_o    (o_mscratch_o),
      .o_mie_o         (o_mie_o),
      .o_mip_o         (o_mip_o),
      .o_misa_o        (o_misa_o),
      .o_mcycle_o      (o_mcycle_o),
      .o_minstret_o    (o_minstret_o),
      .o_wr_ctr        (o_wr_ctr),
      .o_illegal_wr_ctr(o_illegal_wr_ctr),
      .o_trap_ctr      (o_trap_ctr),
      .o_mret_ctr      (o_mret_ctr)
  );

endmodule : mosaic_csr_tb

`resetall
`default_nettype wire
