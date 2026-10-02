// ============================================================================
// mosaic_llb_tb -- unit-test wrapper for the I-060 clean-copy locality buffer
// (`mosaic_llb`).
//
// The wrapper adds no behaviour of its own: every port of the one instance is a
// top-level port of this module, so `sim/unit/tb_llb.cpp` drives the LLB
// directly and supplies the fill data, the invalidation pulses and the memory
// history from an independent host model. There is no clock generation, no
// reset generation and no `$display` here -- the C++ side owns the clock, the
// reset schedule and all result reporting (sim/common/sim_common.h).
//
// The geometry is the module's default (8 fully associative 32-byte entries).
// The driver does not trust that: it reads `o_entries`/`o_line_bytes` out of the
// elaborated DUT and asserts the widths it was written for, so a geometry
// change is an explicit failure rather than a test that checks the wrong
// number of entries.
// ============================================================================

`default_nettype none
`resetall

module mosaic_llb_tb (
  input  logic          clk,
  input  logic          rst,

  // ------------------------------------------------------------- lookup
  input  logic          req_valid,
  input  logic [63:0]   req_pa,
  input  logic [26:0]   req_vpn,
  input  logic [15:0]   req_asid,
  input  logic [3:0]    req_perms,
  input  logic          req_atomic,
  output logic          req_hit,
  output logic [255:0]  req_data,
  output logic          req_bypass,

  // --------------------------------------------------------------- fill
  input  logic          fill_valid,
  input  logic [63:0]   fill_pa,
  input  logic [26:0]   fill_vpn,
  input  logic [15:0]   fill_asid,
  input  logic [3:0]    fill_perms,
  input  logic [255:0]  fill_data,
  output logic          fill_ok,

  // ---------------------------------------------- invalidation: data lines
  input  logic          inv_store_valid,
  input  logic [63:0]   inv_store_pa,
  input  logic          inv_refill_valid,
  input  logic [63:0]   inv_refill_pa,
  input  logic          inv_snoop_valid,
  input  logic [63:0]   inv_snoop_pa,
  input  logic          inv_snoop_all,

  // --------------------------------------------- invalidation: fences
  input  logic          fence_valid,
  input  logic [1:0]    fence_kind,
  input  logic [26:0]   fence_vpn,
  input  logic          fence_has_vpn,
  input  logic [15:0]   fence_asid,
  input  logic          fence_has_asid,

  // -------------------------------------- invalidation: context change
  input  logic          ctx_valid,

  // -------------------------------------------------------- observability
  input  logic [2:0]    dbg_index,
  output logic          dbg_valid,
  output logic [26:0]   dbg_line,
  output logic [15:0]   dbg_asid,
  output logic [3:0]    dbg_perms,
  output logic [26:0]   dbg_vpn,
  output logic [255:0]  dbg_data,

  output logic [3:0]    o_count,
  output logic [7:0]    o_entries,
  output logic [7:0]    o_line_bytes,
  output logic [31:0]   o_hit_ctr,
  output logic [31:0]   o_miss_ctr,
  output logic [31:0]   o_bypass_ctr,
  output logic [31:0]   o_fill_ctr,
  output logic [31:0]   o_fill_refused_ctr,
  output logic [31:0]   o_inv_ctr,
  output logic [31:0]   o_race_refuse_ctr,
  // I-060 integration: the line a fill displaced this cycle.
  output logic          o_evict_valid,
  output logic [26:0]   o_evict_line
);

  mosaic_llb u_llb (
    .clk                (clk),
    .rst                (rst),
    .req_valid_i        (req_valid),
    .req_pa_i           (req_pa),
    .req_vpn_i          (req_vpn),
    .req_asid_i         (req_asid),
    .req_perms_i        (req_perms),
    .req_atomic_i       (req_atomic),
    .req_hit_o          (req_hit),
    .req_data_o         (req_data),
    .req_bypass_o       (req_bypass),
    .fill_valid_i       (fill_valid),
    .fill_pa_i          (fill_pa),
    .fill_vpn_i         (fill_vpn),
    .fill_asid_i        (fill_asid),
    .fill_perms_i       (fill_perms),
    .fill_data_i        (fill_data),
    .fill_ok_o          (fill_ok),
    .inv_store_valid_i  (inv_store_valid),
    .inv_store_pa_i     (inv_store_pa),
    .inv_refill_valid_i (inv_refill_valid),
    .inv_refill_pa_i    (inv_refill_pa),
    .inv_snoop_valid_i  (inv_snoop_valid),
    .inv_snoop_pa_i     (inv_snoop_pa),
    .inv_snoop_all_i    (inv_snoop_all),
    .fence_valid_i      (fence_valid),
    .fence_kind_i       (fence_kind),
    .fence_vpn_i        (fence_vpn),
    .fence_has_vpn_i    (fence_has_vpn),
    .fence_asid_i       (fence_asid),
    .fence_has_asid_i   (fence_has_asid),
    .ctx_valid_i        (ctx_valid),
    .dbg_index_i        (dbg_index),
    .dbg_valid_o        (dbg_valid),
    .dbg_line_o         (dbg_line),
    .dbg_asid_o         (dbg_asid),
    .dbg_perms_o        (dbg_perms),
    .dbg_vpn_o          (dbg_vpn),
    .dbg_data_o         (dbg_data),
    .o_count_o          (o_count),
    .o_entries_o        (o_entries),
    .o_line_bytes_o     (o_line_bytes),
    .o_hit_ctr          (o_hit_ctr),
    .o_miss_ctr         (o_miss_ctr),
    .o_bypass_ctr       (o_bypass_ctr),
    .o_fill_ctr         (o_fill_ctr),
    .o_fill_refused_ctr (o_fill_refused_ctr),
    .o_inv_ctr          (o_inv_ctr),
    .o_race_refuse_ctr  (o_race_refuse_ctr)
,
    .o_evict_valid_o    (),
    .o_evict_line_o     ()
  );

endmodule

`default_nettype wire
