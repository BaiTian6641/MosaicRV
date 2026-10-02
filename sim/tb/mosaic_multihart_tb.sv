// ============================================================================
// mosaic_multihart_tb -- the testbench wrapper for CASE=multihart.isolation
// (work package I-064).
//
// The wrapper adds no timing of its own. It flattens the two-hart design's
// ports to plain vectors so the C++ driver contains no geometry, and it fixes
// the two harts' reset PCs (the two programs live at different physical
// addresses). Everything else -- the two cores, the shared hart-tagged memory
// service and its arbiter -- is `mosaic_multihart`.
//
// Geometry is taken from the generated packages by scope reference, never
// re-derived by a formula this wrapper invented.
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

localparam int unsigned TB_MH_XLEN     = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned TB_MH_RET_N    = mosaic_cfg_pkg::MOSAIC_RETIRE_WIDTH;
localparam int unsigned TB_MH_ROB_N    = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned TB_MH_FETCH_N  = mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING;
localparam int unsigned TB_MH_REQ_ID_W = (TB_MH_FETCH_N <= 1) ? 1 : $clog2(TB_MH_FETCH_N);
localparam int unsigned TB_MH_EPOCH_W  = (TB_MH_ROB_N <= 1) ? 2 : $clog2(TB_MH_ROB_N) + 1;
localparam int unsigned TB_MH_SEQ_W    = $clog2(2 * TB_MH_ROB_N + 1);
localparam int unsigned TB_MH_SIZE_W   = 3;
localparam int unsigned TB_MH_RD_W     = 5;
localparam int unsigned TB_MH_CSR_W    = 12;
localparam int unsigned TB_MH_HART_W   = mosaic_id_pkg::MOSAIC_ID_W_HART;
localparam int unsigned TB_MH_MEM_ID_W = $bits(mosaic_uop_pkg::uop_id_t);
localparam int unsigned TB_MH_HARTS    = 2;

// The two harts' reset PCs. Hart 0's program is assembled at the first and hart
// 1's at the second; the driver has the same two constants.
localparam logic [63:0] TB_MH_H0_RESET_PC = 64'h0000_0000_8000_2000;
localparam logic [63:0] TB_MH_H1_RESET_PC = 64'h0000_0000_8000_4000;

module mosaic_multihart_tb (

    input  logic                          clk,
    input  logic                          rst,
    // Per-hart enable. Low holds that hart in reset (its requests are masked off
    // the shared bus), which is how the driver takes each hart's solo reference
    // run. Both high is the concurrent run.
    input  logic [TB_MH_HARTS-1:0]           hart_en_i,

    // ================================================== the shared memory bus
    // One request channel, one response channel, every packet hart-tagged. This
    // is the shared resource contract: the arbiter inside this module is the
    // only thing that decides who gets the port, and the tag is the only thing
    // that decides where the answer goes.
    output logic                          mem_req_valid,
    input  logic                          mem_req_ready,
    output logic [TB_MH_HART_W-1:0]          mem_req_hart,
    output logic                          mem_req_src,       // 0 = data, 1 = instruction
    output logic                          mem_req_we,
    output logic [TB_MH_XLEN-1:0]            mem_req_addr,
    output logic [2:0]                    mem_req_size,
    output logic [TB_MH_XLEN/8-1:0]          mem_req_wstrb,
    output logic [TB_MH_XLEN-1:0]            mem_req_wdata,
    output logic                          mem_req_amo,
    output logic [3:0]                    mem_req_amo_op,
    output logic                          mem_req_aq,
    output logic                          mem_req_rl,
    output logic [TB_MH_REQ_ID_W-1:0]        mem_req_id,
    output logic [TB_MH_EPOCH_W-1:0]         mem_req_epoch,

    input  logic                          mem_rsp_valid,
    output logic                          mem_rsp_ready,
    input  logic [TB_MH_HART_W-1:0]          mem_rsp_hart,
    input  logic                          mem_rsp_src,
    input  logic [TB_MH_XLEN-1:0]            mem_rsp_rdata,
    input  logic                          mem_rsp_fault,
    input  logic [TB_MH_REQ_ID_W-1:0]        mem_rsp_id,
    input  logic [TB_MH_EPOCH_W-1:0]         mem_rsp_epoch,
    input  logic [2:0]                    mem_rsp_len,

    // ================================================== shared-service evidence
    output logic [31:0]                   o_req_ctr,          // accepted requests
    output logic [31:0]                   o_req_h0_ctr,
    output logic [31:0]                   o_req_h1_ctr,
    output logic [31:0]                   o_req_imem_ctr,
    output logic [31:0]                   o_req_dmem_ctr,
    // Cycles on which both harts had a request outstanding at once: the direct
    // statement that they are contending for the one shared service.
    output logic [31:0]                   o_contend_ctr,
    // A response that named a slot which did not request it. Zero in the
    // shipping machine; non-zero is the detected ownership error.
    output logic [31:0]                   o_owner_mismatch_ctr,
    // The identity geometry the case reads rather than re-deriving: the hart
    // field width and the full uop-identity width, so the driver can extract the
    // hart from a core's own packet identity without guessing a width.
    output logic [31:0]                   o_geom_hart_w,
    output logic [31:0]                   o_geom_mem_id_w,
    output logic [31:0]                   o_geom_ret_n,
    output logic [31:0]                   o_geom_seq_w,

    // ================================================== per-hart evidence (h0)
    output logic [TB_MH_RET_N-1:0]                    h0_ev_valid,
    output logic [TB_MH_RET_N-1:0]                    h0_ev_trap,
    output logic [TB_MH_RET_N*TB_MH_SEQ_W-1:0]           h0_ev_seq,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h0_ev_pc,
    output logic [TB_MH_RET_N*TB_MH_SIZE_W-1:0]          h0_ev_len,
    output logic [TB_MH_RET_N*32-1:0]                 h0_ev_insn,
    output logic [TB_MH_RET_N-1:0]                    h0_ev_reg_we,
    output logic [TB_MH_RET_N*TB_MH_RD_W-1:0]            h0_ev_rd,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h0_ev_value,
    output logic [TB_MH_RET_N-1:0]                    h0_ev_store,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h0_ev_store_addr,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h0_ev_store_data,
    output logic [TB_MH_RET_N*TB_MH_SIZE_W-1:0]          h0_ev_store_size,
    output logic [TB_MH_RET_N-1:0]                    h0_ev_csr_we,
    output logic [TB_MH_RET_N*TB_MH_CSR_W-1:0]           h0_ev_csr_addr,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h0_ev_csr_value,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h0_ev_trap_cause,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h0_ev_trap_tval,
    output logic [31:0]                            h0_commit_ctr,
    output logic [31:0]                            h0_redirect_ctr,
    output logic                                   h0_redirect_valid,
    output logic [TB_MH_XLEN-1:0]                     h0_redirect_pc,
    output logic [TB_MH_XLEN-1:0]                     h0_fetch_pc,
    output logic [31:0]                            h0_rob_occupied,
    output logic                                   h0_stopped,
    output logic                                   h0_trap_valid,
    output logic [TB_MH_XLEN-1:0]                     h0_trap_cause,
    output logic [TB_MH_XLEN-1:0]                     h0_trap_tval,
    output logic [TB_MH_XLEN-1:0]                     h0_trap_epc,
    output logic [1:0]                             h0_priv,
    output logic [TB_MH_XLEN-1:0]                     h0_satp,
    output logic [TB_MH_XLEN-1:0]                     h0_mstatus,
    output logic [TB_MH_XLEN-1:0]                     h0_mepc,
    output logic [TB_MH_XLEN-1:0]                     h0_mcause,
    output logic [TB_MH_XLEN-1:0]                     h0_mtval,
    output logic [TB_MH_MEM_ID_W-1:0]                 h0_dmem_id,
    // The cycle hart 0's data request was accepted by the shared service: the
    // cycle the identity below describes a real transaction, so the case can
    // require its hart field to name hart 0 rather than reading it blind.
    output logic                                   h0_dmem_id_valid,

    // ================================================== per-hart evidence (h1)
    output logic [TB_MH_RET_N-1:0]                    h1_ev_valid,
    output logic [TB_MH_RET_N-1:0]                    h1_ev_trap,
    output logic [TB_MH_RET_N*TB_MH_SEQ_W-1:0]           h1_ev_seq,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h1_ev_pc,
    output logic [TB_MH_RET_N*TB_MH_SIZE_W-1:0]          h1_ev_len,
    output logic [TB_MH_RET_N*32-1:0]                 h1_ev_insn,
    output logic [TB_MH_RET_N-1:0]                    h1_ev_reg_we,
    output logic [TB_MH_RET_N*TB_MH_RD_W-1:0]            h1_ev_rd,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h1_ev_value,
    output logic [TB_MH_RET_N-1:0]                    h1_ev_store,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h1_ev_store_addr,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h1_ev_store_data,
    output logic [TB_MH_RET_N*TB_MH_SIZE_W-1:0]          h1_ev_store_size,
    output logic [TB_MH_RET_N-1:0]                    h1_ev_csr_we,
    output logic [TB_MH_RET_N*TB_MH_CSR_W-1:0]           h1_ev_csr_addr,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h1_ev_csr_value,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h1_ev_trap_cause,
    output logic [TB_MH_RET_N*TB_MH_XLEN-1:0]            h1_ev_trap_tval,
    output logic [31:0]                            h1_commit_ctr,
    output logic [31:0]                            h1_redirect_ctr,
    output logic                                   h1_redirect_valid,
    output logic [TB_MH_XLEN-1:0]                     h1_redirect_pc,
    output logic [TB_MH_XLEN-1:0]                     h1_fetch_pc,
    output logic [31:0]                            h1_rob_occupied,
    output logic                                   h1_stopped,
    output logic                                   h1_trap_valid,
    output logic [TB_MH_XLEN-1:0]                     h1_trap_cause,
    output logic [TB_MH_XLEN-1:0]                     h1_trap_tval,
    output logic [TB_MH_XLEN-1:0]                     h1_trap_epc,
    output logic [1:0]                             h1_priv,
    output logic [TB_MH_XLEN-1:0]                     h1_satp,
    output logic [TB_MH_XLEN-1:0]                     h1_mstatus,
    output logic [TB_MH_XLEN-1:0]                     h1_mepc,
    output logic [TB_MH_XLEN-1:0]                     h1_mcause,
    output logic [TB_MH_XLEN-1:0]                     h1_mtval,
    output logic [TB_MH_MEM_ID_W-1:0]                 h1_dmem_id,
    output logic                                   h1_dmem_id_valid
);

  // The shared bus's two packet types, assembled from the flat driver ports.
  mosaic_uop_pkg::mem_req_t mh_req;
  mosaic_uop_pkg::mem_rsp_t mh_rsp;

  // The request side: the design drives the packet, the flat driver ports carry
  // its fields out. The response side is the other direction.
  always_comb begin
    mem_req_we     = mh_req.we;
    mem_req_addr   = mh_req.addr;
    mem_req_size   = mh_req.size;
    mem_req_wstrb  = mh_req.wstrb;
    mem_req_wdata  = mh_req.wdata;
    mem_req_amo    = mh_req.amo;
    mem_req_amo_op = 4'(mh_req.amo_op);
    mem_req_aq     = mh_req.aq;
    mem_req_rl     = mh_req.rl;
  end

  always_comb begin
    mh_rsp.rdata  = mem_rsp_rdata;
    mh_rsp.fault  = mem_rsp_fault;
  end

  mosaic_multihart #(
      .HART0_RESET_PC (TB_MH_H0_RESET_PC),
      .HART1_RESET_PC (TB_MH_H1_RESET_PC)
  ) u_mh (
      .clk (clk),
      .rst (rst),
      .hart_en_i (hart_en_i),
      .mem_req_valid (mem_req_valid),
      .mem_req_ready (mem_req_ready),
      .mem_req_hart (mem_req_hart),
      .mem_req_src (mem_req_src),
      .mem_req            (mh_req),
      .mem_req_id (mem_req_id),
      .mem_req_epoch (mem_req_epoch),
      .mem_rsp_valid (mem_rsp_valid),
      .mem_rsp_ready (mem_rsp_ready),
      .mem_rsp_hart (mem_rsp_hart),
      .mem_rsp_src (mem_rsp_src),
      .mem_rsp            (mh_rsp),
      .mem_rsp_id (mem_rsp_id),
      .mem_rsp_epoch (mem_rsp_epoch),
      .mem_rsp_len (mem_rsp_len),
      .o_req_ctr (o_req_ctr),
      .o_req_h0_ctr (o_req_h0_ctr),
      .o_req_h1_ctr (o_req_h1_ctr),
      .o_req_imem_ctr (o_req_imem_ctr),
      .o_req_dmem_ctr (o_req_dmem_ctr),
      .o_contend_ctr (o_contend_ctr),
      .o_owner_mismatch_ctr (o_owner_mismatch_ctr),
      .o_geom_hart_w (o_geom_hart_w),
      .o_geom_mem_id_w (o_geom_mem_id_w),
      .o_geom_ret_n (o_geom_ret_n),
      .o_geom_seq_w (o_geom_seq_w),
      .h0_ev_valid (h0_ev_valid),
      .h0_ev_trap (h0_ev_trap),
      .h0_ev_seq (h0_ev_seq),
      .h0_ev_pc (h0_ev_pc),
      .h0_ev_len (h0_ev_len),
      .h0_ev_insn (h0_ev_insn),
      .h0_ev_reg_we (h0_ev_reg_we),
      .h0_ev_rd (h0_ev_rd),
      .h0_ev_value (h0_ev_value),
      .h0_ev_store (h0_ev_store),
      .h0_ev_store_addr (h0_ev_store_addr),
      .h0_ev_store_data (h0_ev_store_data),
      .h0_ev_store_size (h0_ev_store_size),
      .h0_ev_csr_we (h0_ev_csr_we),
      .h0_ev_csr_addr (h0_ev_csr_addr),
      .h0_ev_csr_value (h0_ev_csr_value),
      .h0_ev_trap_cause (h0_ev_trap_cause),
      .h0_ev_trap_tval (h0_ev_trap_tval),
      .h0_commit_ctr (h0_commit_ctr),
      .h0_redirect_ctr (h0_redirect_ctr),
      .h0_redirect_valid (h0_redirect_valid),
      .h0_redirect_pc (h0_redirect_pc),
      .h0_fetch_pc (h0_fetch_pc),
      .h0_rob_occupied (h0_rob_occupied),
      .h0_stopped (h0_stopped),
      .h0_trap_valid (h0_trap_valid),
      .h0_trap_cause (h0_trap_cause),
      .h0_trap_tval (h0_trap_tval),
      .h0_trap_epc (h0_trap_epc),
      .h0_priv (h0_priv),
      .h0_satp (h0_satp),
      .h0_mstatus (h0_mstatus),
      .h0_mepc (h0_mepc),
      .h0_mcause (h0_mcause),
      .h0_mtval (h0_mtval),
      .h0_dmem_id (h0_dmem_id),
      .h0_dmem_id_valid (h0_dmem_id_valid),
      .h1_ev_valid (h1_ev_valid),
      .h1_ev_trap (h1_ev_trap),
      .h1_ev_seq (h1_ev_seq),
      .h1_ev_pc (h1_ev_pc),
      .h1_ev_len (h1_ev_len),
      .h1_ev_insn (h1_ev_insn),
      .h1_ev_reg_we (h1_ev_reg_we),
      .h1_ev_rd (h1_ev_rd),
      .h1_ev_value (h1_ev_value),
      .h1_ev_store (h1_ev_store),
      .h1_ev_store_addr (h1_ev_store_addr),
      .h1_ev_store_data (h1_ev_store_data),
      .h1_ev_store_size (h1_ev_store_size),
      .h1_ev_csr_we (h1_ev_csr_we),
      .h1_ev_csr_addr (h1_ev_csr_addr),
      .h1_ev_csr_value (h1_ev_csr_value),
      .h1_ev_trap_cause (h1_ev_trap_cause),
      .h1_ev_trap_tval (h1_ev_trap_tval),
      .h1_commit_ctr (h1_commit_ctr),
      .h1_redirect_ctr (h1_redirect_ctr),
      .h1_redirect_valid (h1_redirect_valid),
      .h1_redirect_pc (h1_redirect_pc),
      .h1_fetch_pc (h1_fetch_pc),
      .h1_rob_occupied (h1_rob_occupied),
      .h1_stopped (h1_stopped),
      .h1_trap_valid (h1_trap_valid),
      .h1_trap_cause (h1_trap_cause),
      .h1_trap_tval (h1_trap_tval),
      .h1_trap_epc (h1_trap_epc),
      .h1_priv (h1_priv),
      .h1_satp (h1_satp),
      .h1_mstatus (h1_mstatus),
      .h1_mepc (h1_mepc),
      .h1_mcause (h1_mcause),
      .h1_mtval (h1_mtval),
      .h1_dmem_id (h1_dmem_id),
      .h1_dmem_id_valid (h1_dmem_id_valid)
  );

endmodule : mosaic_multihart_tb

`default_nettype wire
