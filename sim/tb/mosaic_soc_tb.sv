// ============================================================================
// mosaic_soc_tb -- Verilator top for the I-047 SoC unit case.
//
// The wrapper is deliberately inert: every port of the one `mosaic_soc`
// instance is a top-level port of this module, so `sim/unit/tb_soc.cpp` drives
// the fabric directly and observes it without a layer that could hide a
// behaviour the case is trying to test.
//
// The one thing it does decide is the platform map's MSIP word: p0's
// `config/memory/p0.json` declares no CLINT MSIP region while p1..p3 do, and the
// RTL cannot see the JSON. `MOSAIC_PROFILE_INDEX` is 0 for p0 and non-zero for
// the profiles that add the region (checked against every file in
// `config/memory/`), so the wrapper sets `HAS_MSIP` from it. A p0 build
// therefore decodes 0x000C_0000 as unmapped -- `DECERR` -- exactly as p0's map
// says, and the case checks whichever of the two maps the profile declares.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_soc_tb (
  input  logic            clk,
  input  logic            rst,

  input  logic            m0_req_valid,
  output logic            m0_req_ready,
  input  logic            m0_req_we,
  input  logic [63:0]     m0_req_addr,
  input  logic [2:0]      m0_req_size,
  input  logic [7:0]      m0_req_wstrb,
  input  logic [63:0]     m0_req_wdata,
  input  logic            m0_req_amo,
  input  logic [3:0]      m0_req_id,
  output logic            m0_rsp_valid,
  input  logic            m0_rsp_ready,
  output logic [63:0]     m0_rsp_rdata,
  output logic            m0_rsp_fault,
  output logic [1:0]      m0_rsp_err,
  output logic [3:0]      m0_rsp_id,

  input  logic            m1_req_valid,
  output logic            m1_req_ready,
  input  logic            m1_req_we,
  input  logic [63:0]     m1_req_addr,
  input  logic [2:0]      m1_req_size,
  input  logic [7:0]      m1_req_wstrb,
  input  logic [63:0]     m1_req_wdata,
  input  logic            m1_req_amo,
  input  logic [3:0]      m1_req_id,
  output logic            m1_rsp_valid,
  input  logic            m1_rsp_ready,
  output logic [63:0]     m1_rsp_rdata,
  output logic            m1_rsp_fault,
  output logic [1:0]      m1_rsp_err,
  output logic [3:0]      m1_rsp_id,

  input  logic            stall,
  input  logic            rom_load_en,
  input  logic [8:0]      rom_load_index,
  input  logic [63:0]     rom_load_data,
  input  logic            uart_rx_push,
  input  logic [7:0]      uart_rx_data,
  output logic            uart_rx_full,
  output logic            uart_tx_valid,
  output logic [7:0]      uart_tx_data,
  output logic            irq_timer,
  output logic            irq_soft,
  output logic            irq_ext,
  output logic            exit_valid,
  output logic [31:0]     exit_code,

  output logic [31:0]     o_accepted_ctr,
  output logic [31:0]     o_completed_normal_ctr,
  output logic [31:0]     o_completed_error_ctr,
  output logic [31:0]     o_outstanding_ctr,
  output logic            o_conservation_ok,
  output logic            o_abort_pulse,
  output logic            o_id_mismatch,
  output logic [31:0]     o_uart_rx_pop_ctr,
  output logic [31:0]     o_uart_tx_ctr,
  output logic [31:0]     o_clint_exit_ctr,
  output logic [31:0]     o_dec_err_ctr,
  output logic [31:0]     o_slv_err_ctr
);

  localparam bit HAS_MSIP = (mosaic_cfg_pkg::MOSAIC_PROFILE_INDEX != 0);

  mosaic_soc #(
    .ID_W            (4),
    .MAX_OUTSTANDING (4),
    .HAS_MSIP        (HAS_MSIP),
    .MSIP_BASE       (64'h00000000000C0000)
  ) u_soc (
    .clk (clk),
    .rst (rst),
    .m0_req_valid_i (m0_req_valid),
    .m0_req_ready_o (m0_req_ready),
    .m0_req_we_i    (m0_req_we),
    .m0_req_addr_i  (m0_req_addr),
    .m0_req_size_i  (m0_req_size),
    .m0_req_wstrb_i (m0_req_wstrb),
    .m0_req_wdata_i (m0_req_wdata),
    .m0_req_amo_i   (m0_req_amo),
    .m0_req_id_i    (m0_req_id),
    .m0_rsp_valid_o (m0_rsp_valid),
    .m0_rsp_ready_i (m0_rsp_ready),
    .m0_rsp_rdata_o (m0_rsp_rdata),
    .m0_rsp_fault_o (m0_rsp_fault),
    .m0_rsp_err_o   (m0_rsp_err),
    .m0_rsp_id_o    (m0_rsp_id),
    .m1_req_valid_i (m1_req_valid),
    .m1_req_ready_o (m1_req_ready),
    .m1_req_we_i    (m1_req_we),
    .m1_req_addr_i  (m1_req_addr),
    .m1_req_size_i  (m1_req_size),
    .m1_req_wstrb_i (m1_req_wstrb),
    .m1_req_wdata_i (m1_req_wdata),
    .m1_req_amo_i   (m1_req_amo),
    .m1_req_id_i    (m1_req_id),
    .m1_rsp_valid_o (m1_rsp_valid),
    .m1_rsp_ready_i (m1_rsp_ready),
    .m1_rsp_rdata_o (m1_rsp_rdata),
    .m1_rsp_fault_o (m1_rsp_fault),
    .m1_rsp_err_o   (m1_rsp_err),
    .m1_rsp_id_o    (m1_rsp_id),
    .stall_i        (stall),
    .rom_load_en_i  (rom_load_en),
    .rom_load_index_i (rom_load_index),
    .rom_load_data_i  (rom_load_data),
    .uart_rx_push_i (uart_rx_push),
    .uart_rx_data_i (uart_rx_data),
    .uart_rx_full_o (uart_rx_full),
    .uart_tx_valid_o (uart_tx_valid),
    .uart_tx_data_o  (uart_tx_data),
    .irq_timer_o    (irq_timer),
    .irq_soft_o     (irq_soft),
    .irq_ext_o      (irq_ext),
    .exit_valid_o   (exit_valid),
    .exit_code_o    (exit_code),
    .o_accepted_ctr         (o_accepted_ctr),
    .o_completed_normal_ctr (o_completed_normal_ctr),
    .o_completed_error_ctr  (o_completed_error_ctr),
    .o_outstanding_ctr      (o_outstanding_ctr),
    .o_conservation_ok      (o_conservation_ok),
    .o_abort_pulse          (o_abort_pulse),
    .o_id_mismatch          (o_id_mismatch),
    .o_uart_rx_pop_ctr      (o_uart_rx_pop_ctr),
    .o_uart_tx_ctr          (o_uart_tx_ctr),
    .o_clint_exit_ctr       (o_clint_exit_ctr),
    .o_dec_err_ctr          (o_dec_err_ctr),
    .o_slv_err_ctr          (o_slv_err_ctr)
  );

endmodule

`resetall
