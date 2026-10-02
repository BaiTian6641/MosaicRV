// ============================================================================
// mosaic_lsu_endpoint_tb -- simulation wrapper for
// CASE=lsu.size_fault_boundaries (work package I-033).
//
// Simulation-only glue. It contains no behaviour of its own: every port is
// either driven by sim/unit/tb_lsu.cpp or wired straight out of the DUT. There
// is no clock generation, no reset generation and no `$display` in here, per
// sim/common/sim_common.h -- the C++ side owns all three.
//
// ---------------------------------------------------- why the ports are flat
//
// The DUT's packet ports are packed structs (`mosaic_uop_pkg::lsu_req_t`,
// `lsu_rsp_t`, `mem_req_t`, `mem_rsp_t`). A Verilator C++ driver can only poke
// those as raw bit vectors, so it would have to reproduce their bit layout --
// a second transcription of rtl/core/mosaic_uop_pkg.sv, which is exactly the
// duplication that file exists to prevent, and one that would keep compiling
// after the package changed.
//
// So this wrapper takes flat driver-facing ports, assembles the request packet
// field by field and flattens the response packets the other way. No assignment
// patterns (`'{...}`): Yosys cannot parse them. The C++ driver then contains no
// packet layout at all.
//
// ------------------------------------------------------------ the widths
//
// Narrow fields are narrowed by the packet's *own* width, taken from the same
// generated packages the RTL reads (by scope reference, not by a second
// `include`: the generated header for `mosaic_cfg_pkg` has no include guard, so
// a second include in one compilation is a duplicate package declaration).
// Every width is re-exported as an `o_*_w` constant so the driver can check the
// layout it assumes -- in particular the identity field layout -- against the
// one the packages actually declare, instead of against a formula this wrapper
// invented. The identity is checked as the *sum* of the four identity widths,
// so a driver that placed a generation field at the wrong offset is caught by
// matching widths while a package that dropped one is caught by the sum.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDSIGNAL */
// The driver-facing request ports are fixed 32-bit vectors, so the C++ driver
// carries no packet geometry, while the packet fields they are narrowed to are
// a few bits wide. The unused upper bits are the price of that convention and
// are deliberate, not an omission; the widths they are narrowed to are
// re-exported below so the driver can check them.

module mosaic_lsu_endpoint_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------ upstream: the memory uop
    input  logic        up_req_valid,
    output logic        up_req_ready,
    input  logic [31:0] up_req_id,
    input  logic        up_req_we,
    input  logic [63:0] up_req_base,
    input  logic [63:0] up_req_imm,
    input  logic [31:0] up_req_size,
    input  logic        up_req_signed,
    input  logic [63:0] up_req_store_data,
    // The PMA device attribute (I-038). It is a flat port rather than a packet
    // field, and the packet below is assembled without it, for the same reason
    // the rest of this wrapper exists: the C++ driver must not know the packet
    // layout, and `lsu_req_t` is a frozen interface.
    input  logic        up_req_dev,

    // ------------------------------------------------ upstream: the response
    output logic        up_rsp_valid,
    input  logic        up_rsp_ready,
    output logic [31:0] up_rsp_id,
    output logic        up_rsp_fault,
    output logic [63:0] up_rsp_cause,
    output logic [63:0] up_rsp_tval,
    output logic [63:0] up_rsp_data,

    // -------------------------------------------- downstream: memory request
    output logic        dn_req_valid,
    input  logic        dn_req_ready,
    output logic        dn_req_we,
    output logic [63:0] dn_req_addr,
    output logic [31:0] dn_req_size,
    output logic [31:0] dn_req_wstrb,
    output logic [63:0] dn_req_wdata,

    // ------------------------------------------- downstream: memory response
    input  logic        dn_rsp_valid,
    output logic        dn_rsp_ready,
    input  logic [63:0] dn_rsp_rdata,
    input  logic        dn_rsp_fault,

    // ------------------------------------------------------- observability
    output logic        o_busy,
    output logic [31:0] o_load_ctr,
    output logic [31:0] o_store_ctr,
    output logic [31:0] o_txn_ctr,
    output logic [31:0] o_misaligned_ctr,
    output logic [31:0] o_access_fault_ctr,
    output logic [31:0] o_rsp_ctr,
    output logic [63:0] o_last_fault_cause,
    output logic [63:0] o_last_fault_tval,
    output logic [63:0] o_inflight_addr,
    output logic [31:0] o_inflight_size,
    // The identity of the transaction being served and its device attribute
    // (I-038), flattened for the driver. The expected id width is derivable from
    // the `o_*_w` geometry below.
    output logic [31:0] o_txn_id,
    output logic        o_txn_dev,

    // ------------------------------------------------------------ geometry
    output logic [31:0] o_xlen_w,
    output logic [31:0] o_strb_w,
    output logic [31:0] o_id_w,
    output logic [31:0] o_size_w,
    output logic [31:0] o_hart_w,
    output logic [31:0] o_rob_index_w,
    output logic [31:0] o_rob_gen_w,
    output logic [31:0] o_uop_index_w
);
/* verilator lint_on UNUSEDSIGNAL */

  localparam int unsigned XLEN   = mosaic_cfg_pkg::MOSAIC_XLEN;
  localparam int unsigned STRB_W = XLEN / 8;
  // The identity's own width, and the widths of its fields, taken from the
  // packages that define them rather than re-derived here.
  localparam int unsigned ID_W   = $bits(mosaic_uop_pkg::uop_id_t);
  localparam int unsigned SIZE_W = $bits(mosaic_pkg::SZ_BYTE);

  mosaic_uop_pkg::lsu_req_t up_req_s;
  mosaic_uop_pkg::lsu_rsp_t up_rsp_s;
  mosaic_uop_pkg::mem_req_t dn_req_s;
  mosaic_uop_pkg::mem_rsp_t dn_rsp_s;
  logic [SIZE_W-1:0]        o_inflight_size_s;
  mosaic_uop_pkg::uop_id_t  o_txn_id_s;

  // The request packet, assembled field by field.
  always_comb begin
    up_req_s.id         = up_req_id[ID_W-1:0];
    up_req_s.we         = up_req_we;
    up_req_s.base       = up_req_base;
    up_req_s.imm        = up_req_imm;
    up_req_s.size       = up_req_size[SIZE_W-1:0];
    up_req_s.signed_    = up_req_signed;
    up_req_s.store_data = up_req_store_data;
    // The A-extension fields (I-039) exist in `lsu_req_t` and are driven here so
    // the packet is fully assigned. This wrapper is CASE=lsu.size_fault_boundaries'
    // and exercises no atomic access; the AMO path has its own case.
    up_req_s.is_amo     = 1'b0;
    up_req_s.amo_op     = mosaic_pkg::AMO_ADD;
    up_req_s.aq         = 1'b0;
    up_req_s.rl         = 1'b0;
  end

  // The downstream response packet the driver presents.
  always_comb begin
    dn_rsp_s.rdata = dn_rsp_rdata;
    dn_rsp_s.fault = dn_rsp_fault;
  end

  // The upstream response packet, flattened.
  assign up_rsp_id    = {{(32 - ID_W) {1'b0}}, up_rsp_s.id};
  assign up_rsp_fault = up_rsp_s.fault;
  assign up_rsp_cause = up_rsp_s.cause;
  assign up_rsp_tval  = up_rsp_s.tval;
  assign up_rsp_data  = up_rsp_s.data;

  // The downstream request packet, flattened. `wstrb` for a load is not read by
  // the driver (the memory must ignore it when `we` is low); it is exported
  // anyway so a wrapper-level width mistake cannot make it invisible.
  assign dn_req_we    = dn_req_s.we;
  assign dn_req_addr  = dn_req_s.addr;
  assign dn_req_size  = {{(32 - SIZE_W) {1'b0}}, dn_req_s.size};
  assign dn_req_wstrb = {{(32 - STRB_W) {1'b0}}, dn_req_s.wstrb};
  assign dn_req_wdata = dn_req_s.wdata;

  assign o_inflight_size = {{(32 - SIZE_W) {1'b0}}, o_inflight_size_s};

  // Read back from the packages, so the driver can check the layout it assumes
  // against the one that exists rather than trusting either side alone.
  assign o_xlen_w       = 32'(XLEN);
  assign o_strb_w       = 32'(STRB_W);
  assign o_id_w         = 32'(ID_W);
  assign o_size_w       = 32'(SIZE_W);
  assign o_hart_w       = 32'(mosaic_id_pkg::MOSAIC_ID_W_HART);
  assign o_rob_index_w  = 32'(mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX);
  assign o_rob_gen_w    = 32'(mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN);
  assign o_uop_index_w  = 32'(mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX);

  mosaic_lsu_endpoint u_lsu (
      .clk                 (clk),
      .rst                 (rst),

      .req_valid_i         (up_req_valid),
      .req_ready_o         (up_req_ready),
      .req_i               (up_req_s),
      .req_dev_i           (up_req_dev),

      .rsp_valid_o         (up_rsp_valid),
      .rsp_ready_o         (up_rsp_ready),
      .rsp_o               (up_rsp_s),

      .mem_req_valid_o     (dn_req_valid),
      .mem_req_ready_i     (dn_req_ready),
      .mem_req_o           (dn_req_s),

      .mem_rsp_valid_i     (dn_rsp_valid),
      .mem_rsp_ready_o     (dn_rsp_ready),
      .mem_rsp_i           (dn_rsp_s),

      .o_busy              (o_busy),
      .o_load_ctr          (o_load_ctr),
      .o_store_ctr         (o_store_ctr),
      .o_txn_ctr           (o_txn_ctr),
      .o_misaligned_ctr    (o_misaligned_ctr),
      .o_access_fault_ctr  (o_access_fault_ctr),
      .o_rsp_ctr           (o_rsp_ctr),
      .o_last_fault_cause  (o_last_fault_cause),
      .o_last_fault_tval   (o_last_fault_tval),
      .o_inflight_addr     (o_inflight_addr),
      .o_inflight_size     (o_inflight_size_s),
      .o_txn_id            (o_txn_id_s),
      .o_txn_dev           (o_txn_dev)
  );

  assign o_txn_id = {{(32 - ID_W) {1'b0}}, o_txn_id_s};

endmodule : mosaic_lsu_endpoint_tb

`resetall
`default_nettype wire
