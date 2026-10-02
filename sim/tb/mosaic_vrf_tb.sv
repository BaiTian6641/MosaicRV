// Simulation wrapper for CASE=vrf.mapping_aliases (work package I-053).
//
// The wrapper adds no timing of its own: `mosaic_vrf` is driven by the C++
// driver (sim/unit/tb_vrf.cpp), which owns the clock, the reset schedule and
// every comparison, per sim/common/sim_common.h. It exists for two reasons:
//
//   * it fixes the geometry the registry's case is about -- the p2 vector block
//     of config/geometry/p2.json: VLEN=128, ELEN=64, VREGS=32, BANK_W=32,
//     BANKS=32, LANES_MAX=8, RD_PORTS=2, WR_PORTS=1, RD_LATENCY=1 -- and reads
//     the derived widths back as outputs, so the driver checks its own constants
//     against the DUT instead of hardcoding them;
//   * it repacks each slot's demand into one 32-bit word, so the driver holds no
//     per-field geometry: one write per slot, not four packed slices that a
//     width change would silently move.
//
// The control word layout, shared by read, write and query ports, is
//
//     [18:14] base register (5 bits)   [13:7] element (7 bits)
//     [ 6: 4] SEW log2     (3 bits)    [ 3: 0] LMUL log2 (4 bits, signed)
//
// The module is compiled by the case's own source list (tests/unit/registry.json
// lists rtl/core/mosaic_vrf.sv), so the wrapper instantiates it directly.

`default_nettype none
`resetall

module mosaic_vrf_tb #(
    parameter int unsigned VLEN       = 128,
    parameter int unsigned ELEN       = 64,
    parameter int unsigned VREGS      = 32,
    parameter int unsigned BANK_W     = 32,
    parameter int unsigned BANKS      = 32,
    parameter int unsigned LANES_MAX  = 8,
    parameter int unsigned RD_PORTS   = 2,
    parameter int unsigned WR_PORTS   = 1,
    parameter int unsigned RD_LATENCY = 1
) (
    input  logic                        clk,
    input  logic                        rst,

    input  logic [3:0]                  lane_count_i,
    input  logic [1:0]                  plat_i,

    // ------------------------------------------------------------- read slots
    input  logic [LANES_MAX-1:0]        rd_valid_i,
    input  logic [31:0]                 rd_ctrl_i  [LANES_MAX],
    input  logic [15:0]                 rd_tag_i   [LANES_MAX],
    output logic [LANES_MAX-1:0]        rd_gnt_o,
    output logic [LANES_MAX-1:0]        rd_rsp_valid_o,
    output logic [15:0]                 rd_rsp_tag_o [LANES_MAX],
    output logic [63:0]                 rd_rsp_data_o[LANES_MAX],

    // ------------------------------------------------------------ write slots
    input  logic [LANES_MAX-1:0]        wr_valid_i,
    input  logic [31:0]                 wr_ctrl_i  [LANES_MAX],
    input  logic [63:0]                 wr_data_i  [LANES_MAX],
    output logic [LANES_MAX-1:0]        wr_gnt_o,

    // ---------------------------------------------------------- mapping query
    input  logic                        q_valid_i,
    input  logic [31:0]                 q_ctrl_i,
    output logic                        q_valid_o,
    output logic [4:0]                  q_phys_reg_o,
    output logic [1:0]                  q_row_o,
    output logic [6:0]                  q_lo_bit_o,
    output logic [7:0]                  q_hi_bit_o,
    output logic [1:0]                  q_nbanks_o,
    output logic [4:0]                  q_bank0_o,
    output logic [4:0]                  q_bank1_o,

    // ---------------------------------------------------------------- status
    output logic [31:0]                 rd_gnt_ctr_o,
    output logic [31:0]                 rd_conflict_ctr_o,
    output logic [31:0]                 rd_bad_ctr_o,
    output logic [31:0]                 wr_gnt_ctr_o,
    output logic [31:0]                 wr_conflict_ctr_o,
    output logic [31:0]                 wr_hazard_ctr_o,
    output logic [31:0]                 wr_bad_ctr_o,
    output logic                        busy_o,

    // -------------------------------------------------------------- geometry
    output logic [31:0]                 o_vlen_o,
    output logic [31:0]                 o_elen_o,
    output logic [31:0]                 o_vregs_o,
    output logic [31:0]                 o_banks_o,
    output logic [31:0]                 o_bank_w_o,
    output logic [31:0]                 o_rows_o,
    output logic [31:0]                 o_regs_per_row_o,
    output logic [31:0]                 o_lane_max_o,
    output logic [31:0]                 o_rd_latency_o,
    output logic [31:0]                 o_rd_ports_o,
    output logic [31:0]                 o_wr_ports_o,
    output logic [31:0]                 o_plat_o
);

  // --------------------------------------------------- flattened module buses
  logic [LANES_MAX*5-1:0]  rd_base_f;
  logic [LANES_MAX*7-1:0]  rd_elem_f;
  logic [LANES_MAX*3-1:0]  rd_sew_f;
  logic [LANES_MAX*4-1:0]  rd_lmul_f;
  logic [LANES_MAX*16-1:0] rd_tag_f;

  logic [LANES_MAX*5-1:0]  wr_base_f;
  logic [LANES_MAX*7-1:0]  wr_elem_f;
  logic [LANES_MAX*3-1:0]  wr_sew_f;
  logic [LANES_MAX*4-1:0]  wr_lmul_f;
  logic [LANES_MAX*ELEN-1:0] wr_data_f;

  logic [LANES_MAX*16-1:0] rd_rsp_tag_f;
  logic [LANES_MAX*ELEN-1:0] rd_rsp_data_f;

  always_comb begin
    rd_base_f = '0;
    rd_elem_f = '0;
    rd_sew_f  = '0;
    rd_lmul_f = '0;
    for (int s = 0; s < LANES_MAX; s++) begin
      rd_base_f[s*5 +: 5] = rd_ctrl_i[s][18:14];
      rd_elem_f[s*7 +: 7] = rd_ctrl_i[s][13:7];
      rd_sew_f[s*3 +: 3]  = rd_ctrl_i[s][6:4];
      rd_lmul_f[s*4 +: 4] = rd_ctrl_i[s][3:0];
    end
  end

  always_comb begin
    wr_base_f = '0;
    wr_elem_f = '0;
    wr_sew_f  = '0;
    wr_lmul_f = '0;
    wr_data_f = '0;
    for (int s = 0; s < LANES_MAX; s++) begin
      wr_base_f[s*5 +: 5] = wr_ctrl_i[s][18:14];
      wr_elem_f[s*7 +: 7] = wr_ctrl_i[s][13:7];
      wr_sew_f[s*3 +: 3]  = wr_ctrl_i[s][6:4];
      wr_lmul_f[s*4 +: 4] = wr_ctrl_i[s][3:0];
      wr_data_f[s*ELEN +: ELEN] = wr_data_i[s];
    end
  end

  always_comb begin
    for (int s = 0; s < LANES_MAX; s++) begin
      rd_tag_f[s*16 +: 16] = rd_tag_i[s];
      rd_rsp_tag_o[s]  = rd_rsp_tag_f[s*16 +: 16];
      rd_rsp_data_o[s] = rd_rsp_data_f[s*ELEN +: ELEN];
    end
  end

  mosaic_vrf #(
      .VLEN      (VLEN),
      .ELEN      (ELEN),
      .VREGS     (VREGS),
      .BANK_W    (BANK_W),
      .BANKS     (BANKS),
      .LANES_MAX (LANES_MAX),
      .RD_PORTS  (RD_PORTS),
      .WR_PORTS  (WR_PORTS),
      .RD_LATENCY(RD_LATENCY)
  ) u_vrf (
      .clk_i            (clk),
      .rst_i            (rst),
      .lane_count_i     (lane_count_i),
      .plat_i           (plat_i),

      .rd_valid_i       (rd_valid_i),
      .rd_base_i        (rd_base_f),
      .rd_elem_i        (rd_elem_f),
      .rd_sew_i         (rd_sew_f),
      .rd_lmul_i        (rd_lmul_f),
      .rd_tag_i         (rd_tag_f),
      .rd_gnt_o         (rd_gnt_o),
      .rd_rsp_valid_o   (rd_rsp_valid_o),
      .rd_rsp_tag_o     (rd_rsp_tag_f),
      .rd_rsp_data_o    (rd_rsp_data_f),

      .wr_valid_i       (wr_valid_i),
      .wr_base_i        (wr_base_f),
      .wr_elem_i        (wr_elem_f),
      .wr_sew_i         (wr_sew_f),
      .wr_lmul_i        (wr_lmul_f),
      .wr_data_i        (wr_data_f),
      .wr_gnt_o         (wr_gnt_o),

      .q_valid_i        (q_valid_i),
      .q_base_i         (q_ctrl_i[18:14]),
      .q_elem_i         (q_ctrl_i[13:7]),
      .q_sew_i          (q_ctrl_i[6:4]),
      .q_lmul_i         (q_ctrl_i[3:0]),
      .o_q_valid_o      (q_valid_o),
      .o_q_phys_reg_o   (q_phys_reg_o),
      .o_q_row_o        (q_row_o),
      .o_q_lo_bit_o     (q_lo_bit_o),
      .o_q_hi_bit_o     (q_hi_bit_o),
      .o_q_nbanks_o     (q_nbanks_o),
      .o_q_bank0_o      (q_bank0_o),
      .o_q_bank1_o      (q_bank1_o),

      .o_rd_gnt_ctr     (rd_gnt_ctr_o),
      .o_rd_conflict_ctr(rd_conflict_ctr_o),
      .o_rd_bad_ctr     (rd_bad_ctr_o),
      .o_wr_gnt_ctr     (wr_gnt_ctr_o),
      .o_wr_conflict_ctr(wr_conflict_ctr_o),
      .o_wr_hazard_ctr  (wr_hazard_ctr_o),
      .o_wr_bad_ctr     (wr_bad_ctr_o),
      .o_busy           (busy_o),

      .o_vlen_o         (o_vlen_o),
      .o_elen_o         (o_elen_o),
      .o_vregs_o        (o_vregs_o),
      .o_banks_o        (o_banks_o),
      .o_bank_w_o       (o_bank_w_o),
      .o_rows_o         (o_rows_o),
      .o_regs_per_row_o (o_regs_per_row_o),
      .o_lane_max_o     (o_lane_max_o),
      .o_rd_latency_o   (o_rd_latency_o),
      .o_rd_ports_o     (o_rd_ports_o),
      .o_wr_ports_o     (o_wr_ports_o),
      .o_plat_o         (o_plat_o)
  );

endmodule : mosaic_vrf_tb

`resetall
`default_nettype wire
