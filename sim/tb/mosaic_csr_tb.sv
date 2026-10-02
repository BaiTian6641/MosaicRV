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
    output logic [31:0] o_mret_ctr,
    // ------------------------------------- privilege / PMP ports (I-044)
    // The unit test drives these directly: a privilege mode, an SRET strobe, and
    // a stub PMP register file (the entries themselves are CASE=
    // privilege.permission_matrix's subject, not this file's).
    // (the privilege mode is entered through the trap and MRET paths this
    // wrapper already drives; there is no separate mode input)
    input  logic        sret_valid_i,
    output logic        sret_commit_o,
    output logic [63:0] sret_target_o,
    output logic        mret_illegal_o,
    output logic        sret_illegal_o,
    output logic        wfi_illegal_o,
    output logic [1:0]  o_priv_o,
    output logic [63:0] o_medeleg_o,
    output logic [63:0] o_mideleg_o,
    output logic        pmp_sel_o,
    input  logic [63:0] pmp_rdata_i,
    output logic        pmp_we_o,
    output logic [63:0] pmp_wdata_o,
    output logic [63:0] o_sstatus_o,
    output logic [63:0] o_stvec_o,
    output logic [63:0] o_sepc_o,
    output logic [63:0] o_scause_o,
    output logic [63:0] o_stval_o,
    output logic [63:0] o_sscratch_o,
    output logic [63:0] o_sie_o,
    output logic [63:0] o_sip_o,
    output logic [63:0] o_satp_o,
    output logic [63:0] o_senvcfg_o,
    output logic [63:0] o_scounteren_o,
    output logic [63:0] o_mcounteren_o,
    output logic [31:0] o_sret_ctr,
    output logic [31:0] o_trap_s_ctr,
    output logic [31:0] o_priv_illegal_ctr,
    output logic [31:0] o_priv_change_ctr
);

  mosaic_pkg::csr_op_e csr_op;
/* verilator lint_off PINCONNECTEMPTY */

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

      .sret_valid_i    (sret_valid_i),
      .sret_commit_o   (sret_commit_o),
      .sret_target_o   (sret_target_o),
      .mret_illegal_o  (mret_illegal_o),
      .sret_illegal_o  (sret_illegal_o),
      .wfi_illegal_o   (wfi_illegal_o),
      .o_priv_o        (o_priv_o),
      .o_medeleg_o     (o_medeleg_o),
      .o_mideleg_o     (o_mideleg_o),
      .pmp_sel_o       (pmp_sel_o),
      .pmp_rdata_i     (pmp_rdata_i),
      .pmp_we_o        (pmp_we_o),
      .pmp_wdata_o     (pmp_wdata_o),
      .o_sstatus_o     (o_sstatus_o),
      .o_stvec_o       (o_stvec_o),
      .o_sepc_o        (o_sepc_o),
      .o_scause_o      (o_scause_o),
      .o_stval_o       (o_stval_o),
      .o_sscratch_o    (o_sscratch_o),
      .o_sie_o         (o_sie_o),
      .o_sip_o         (o_sip_o),
      .o_satp_o        (o_satp_o),
      .o_senvcfg_o     (o_senvcfg_o),
      .o_scounteren_o  (o_scounteren_o),
      .o_mcounteren_o  (o_mcounteren_o),
      .o_sret_ctr      (o_sret_ctr),
      .o_trap_s_ctr    (o_trap_s_ctr),
      .o_priv_illegal_ctr(o_priv_illegal_ctr),
      .o_priv_change_ctr(o_priv_change_ctr),

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
      .o_fcsr_o        (),
      .o_fflags_o      (),
      .o_frm_o         (),
      .o_wr_ctr        (o_wr_ctr),
      .o_illegal_wr_ctr(o_illegal_wr_ctr),
      .o_trap_ctr      (o_trap_ctr),
      .o_mret_ctr      (o_mret_ctr)
  );

/* verilator lint_on PINCONNECTEMPTY */
endmodule : mosaic_csr_tb

`resetall
`default_nettype wire
