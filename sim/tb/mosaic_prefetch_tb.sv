// ============================================================================
// mosaic_prefetch_tb -- unit-test wrapper for the I-063 reuse/stride prefetcher
// (`mosaic_prefetch`) together with the I-060 clean-copy locality buffer
// (`mosaic_llb`) it is allowed to populate.
//
// The wrapper adds no behaviour of its own. It instantiates the two modules,
// wires the platform map's answers to the prefetcher's gate inputs and exposes
// every remaining port to the driver, which owns the clock, the reset schedule,
// the demand stream and the memory model (sim/common/sim_common.h).
//
// --------------------------------------------------------- the gate wiring
//
// The prefetcher's `gate_mapped_i` and `gate_side_effect_free_i` are computed
// here, straight from `mosaic_cfg_pkg` -- the generated platform map -- so the
// predicate the prefetcher honours and the predicate the SoC device decode uses
// are the same function of the same address. `gate_perm_ok_i` comes from the
// driver, which owns the page table, and is the demand path's own permission
// answer for the candidate address. The prefetcher cannot widen any of the
// three: they are inputs.
//
// ---------------------------------------------------- who drives the fill
//
// The LLB fill port is a top-level port of this wrapper, and the prefetcher's
// fill outputs are top-level outputs too: the driver presents them, and the
// driver decides which wins when a prefetch response and a demand refill want
// the buffer in the same cycle. Wiring them together inside the wrapper would
// hide that arbitration, and the arbitration is exactly what the "a cancelled
// prefetch leaves nothing behind" and "the demand path is unchanged" checks
// have to observe.
// ============================================================================

`default_nettype none
`resetall

// Several driver-facing pins are wider than the field the module reads back
// (64-bit addresses on a 32-bit machine, an 8-bit prefetch id over a 2-bit slot
// index). That is the driver convention; the module declares the unused bits
// away itself, and the wrapper does not fan them out. The lint suppression
// keeps the wrapper's own ports from being reported.
/* verilator lint_off UNUSEDSIGNAL */

module mosaic_prefetch_tb (
  input  logic          clk,
  input  logic          rst,

  // ------------------------------------------------------------- the switch
  input  logic          pf_en,
  input  logic [1:0]    pf_conf_thresh,

  // ---------------------------------------------------- demand observation
  input  logic          dem_valid,
  input  logic [63:0]   dem_pc,
  input  logic [63:0]   dem_pa,
  input  logic [26:0]   dem_vpn,
  input  logic [15:0]   dem_asid,
  input  logic [3:0]    dem_perms,
  input  logic          dem_hit,
  input  logic          dem_fault,

  // ------------------------------------------------- prefetch candidate / gate
  output logic          pf_valid,
  output logic [63:0]   pf_pa,
  output logic [26:0]   pf_vpn,
  output logic [15:0]   pf_asid,
  output logic [3:0]    pf_perms,
  input  logic          gate_perm_ok,

  // ---------------------------------------------------------- memory egress
  output logic          mem_req_valid,
  output logic [63:0]   mem_req_pa,
  output logic [7:0]    mem_req_id,
  input  logic          mem_req_ready,
  input  logic          mem_resp_valid,
  input  logic [255:0]  mem_resp_data,
  input  logic [7:0]    mem_resp_id,

  // ------------------------------------------------------------- lifecycle
  input  logic          pf_cancel,
  input  logic [7:0]    pf_cancel_id,
  input  logic          pf_flush,
  input  logic          pf_release_valid,
  input  logic [63:0]   pf_release_line,

  // -------------------------------------------------- prefetch fill outputs
  output logic          pf_fill_valid,
  output logic [63:0]   pf_fill_pa,
  output logic [26:0]   pf_fill_vpn,
  output logic [15:0]   pf_fill_asid,
  output logic [3:0]    pf_fill_perms,
  output logic [255:0]  pf_fill_data,

  // ------------------------------------------------------------ LLB lookup
  input  logic          llb_req_valid,
  input  logic [63:0]   llb_req_pa,
  input  logic [26:0]   llb_req_vpn,
  input  logic [15:0]   llb_req_asid,
  input  logic [3:0]    llb_req_perms,
  input  logic          llb_req_atomic,
  output logic          llb_req_hit,
  output logic [255:0]  llb_req_data,
  output logic          llb_req_bypass,

  // -------------------------------------------------------------- LLB fill
  input  logic          llb_fill_valid,
  input  logic [63:0]   llb_fill_pa,
  input  logic [26:0]   llb_fill_vpn,
  input  logic [15:0]   llb_fill_asid,
  input  logic [3:0]    llb_fill_perms,
  input  logic [255:0]  llb_fill_data,
  output logic          llb_fill_ok,

  // ---------------------------------------------------- LLB invalidation
  input  logic          inv_store_valid,
  input  logic [63:0]   inv_store_pa,
  input  logic          inv_refill_valid,
  input  logic [63:0]   inv_refill_pa,
  input  logic          inv_snoop_valid,
  input  logic [63:0]   inv_snoop_pa,
  input  logic          inv_snoop_all,
  input  logic          fence_valid,
  input  logic [1:0]    fence_kind,
  input  logic [26:0]   fence_vpn,
  input  logic          fence_has_vpn,
  input  logic [15:0]   fence_asid,
  input  logic          fence_has_asid,
  input  logic          ctx_valid,

  // -------------------------------------------------- LLB observability
  input  logic [2:0]    llb_dbg_index,
  output logic          llb_dbg_valid,
  output logic [26:0]   llb_dbg_line,
  output logic [15:0]   llb_dbg_asid,
  output logic [3:0]    llb_dbg_perms,
  output logic [26:0]   llb_dbg_vpn,
  output logic [255:0]  llb_dbg_data,
  output logic [3:0]    llb_count,
  output logic [7:0]    llb_entries,
  output logic [7:0]    llb_line_bytes,
  output logic [31:0]   llb_hit_ctr,
  output logic [31:0]   llb_miss_ctr,
  output logic [31:0]   llb_bypass_ctr,
  output logic [31:0]   llb_fill_ctr,
  output logic [31:0]   llb_fill_refused_ctr,
  output logic [31:0]   llb_inv_ctr,
  output logic [31:0]   llb_race_refuse_ctr,

  // ------------------------------------------------- prefetch usefulness etc.
  output logic [31:0]   o_obs_accesses,
  output logic [31:0]   o_obs_new_pc,
  output logic [31:0]   o_obs_repeat_pc,
  output logic [31:0]   o_obs_stride_match,
  output logic [31:0]   o_obs_stride_mismatch,
  output logic [31:0]   o_obs_stride_zero,
  output logic [31:0]   o_obs_reuse_hit,
  output logic [31:0]   o_issued,
  output logic [31:0]   o_useful,
  output logic [31:0]   o_useless,
  output logic [31:0]   o_late,
  output logic [31:0]   o_cancelled,
  output logic [31:0]   o_admitted,
  output logic [31:0]   o_gate_refuse,
  output logic [31:0]   o_full_stall,
  output logic [31:0]   o_fill_ctr,
  output logic [31:0]   o_fill_refused_ctr,
  output logic [31:0]   o_dropped_ctr,
  output logic [31:0]   o_inflight,
  output logic [31:0]   o_table_entries,
  output logic [31:0]   o_depth
);

  // The platform map's answers for the candidate. The read-safe group is the
  // map's answers for the physical address plus the driver's permission answer;
  // the device-free fact is the map's. Neither is a parameter of the predictor:
  // the locality buffer still refuses to *hold* a non-cacheable line, which is
  // what the case checks.
  logic gate_mapped_w;
  logic gate_device_w;

  assign gate_mapped_w     = mosaic_cfg_pkg::mosaic_pa_cacheable(pf_pa) |
                             mosaic_cfg_pkg::mosaic_pa_device(pf_pa) |
                             mosaic_cfg_pkg::mosaic_pa_idempotent(pf_pa);
  assign gate_device_w     = mosaic_cfg_pkg::mosaic_pa_device(pf_pa);

  mosaic_prefetch u_pf (
    .clk                    (clk),
    .rst                    (rst),
    .en_i                   (pf_en),
    .conf_thresh_i          (pf_conf_thresh),
    .dem_valid_i            (dem_valid),
    .dem_pc_i               (dem_pc),
    .dem_pa_i               (dem_pa),
    .dem_vpn_i              (dem_vpn),
    .dem_asid_i             (dem_asid),
    .dem_perms_i            (dem_perms),
    .dem_hit_i              (dem_hit),
    .dem_fault_i            (dem_fault),
    .pf_valid_o             (pf_valid),
    .pf_pa_o                (pf_pa),
    .pf_vpn_o               (pf_vpn),
    .pf_asid_o              (pf_asid),
    .pf_perms_o             (pf_perms),
    .gate_mapped_i          (gate_mapped_w),
    .gate_perm_ok_i         (gate_perm_ok),
    .gate_device_i          (gate_device_w),
    .mem_req_valid_o        (mem_req_valid),
    .mem_req_pa_o           (mem_req_pa),
    .mem_req_id_o           (mem_req_id),
    .mem_req_ready_i        (mem_req_ready),
    .mem_resp_valid_i       (mem_resp_valid),
    .mem_resp_data_i        (mem_resp_data),
    .mem_resp_id_i          (mem_resp_id),
    .pf_cancel_i            (pf_cancel),
    .pf_cancel_id_i         (pf_cancel_id),
    .pf_flush_i             (pf_flush),
    .pf_release_valid_i     (pf_release_valid),
    .pf_release_line_i      (pf_release_line),
    .fill_valid_o           (pf_fill_valid),
    .fill_pa_o              (pf_fill_pa),
    .fill_vpn_o             (pf_fill_vpn),
    .fill_asid_o            (pf_fill_asid),
    .fill_perms_o           (pf_fill_perms),
    .fill_data_o            (pf_fill_data),
    .fill_ok_i              (llb_fill_ok),
    .o_obs_accesses_o       (o_obs_accesses),
    .o_obs_new_pc_o         (o_obs_new_pc),
    .o_obs_repeat_pc_o      (o_obs_repeat_pc),
    .o_obs_stride_match_o   (o_obs_stride_match),
    .o_obs_stride_mismatch_o(o_obs_stride_mismatch),
    .o_obs_stride_zero_o    (o_obs_stride_zero),
    .o_obs_reuse_hit_o      (o_obs_reuse_hit),
    .o_issued_o             (o_issued),
    .o_useful_o             (o_useful),
    .o_useless_o            (o_useless),
    .o_late_o               (o_late),
    .o_cancelled_o          (o_cancelled),
    .o_admitted_o           (o_admitted),
    .o_gate_refuse_o        (o_gate_refuse),
    .o_full_stall_o         (o_full_stall),
    .o_fill_ctr_o           (o_fill_ctr),
    .o_fill_refused_ctr_o   (o_fill_refused_ctr),
    .o_dropped_ctr_o        (o_dropped_ctr),
    .o_inflight_o           (o_inflight),
    .o_table_entries_o      (o_table_entries),
    .o_depth_o              (o_depth)
  );

  mosaic_llb u_llb (
    .clk                (clk),
    .rst                (rst),
    .req_valid_i        (llb_req_valid),
    .req_pa_i           (llb_req_pa),
    .req_vpn_i          (llb_req_vpn),
    .req_asid_i         (llb_req_asid),
    .req_perms_i        (llb_req_perms),
    .req_atomic_i       (llb_req_atomic),
    .req_hit_o          (llb_req_hit),
    .req_data_o         (llb_req_data),
    .req_bypass_o       (llb_req_bypass),
    .fill_valid_i       (llb_fill_valid),
    .fill_pa_i          (llb_fill_pa),
    .fill_vpn_i         (llb_fill_vpn),
    .fill_asid_i        (llb_fill_asid),
    .fill_perms_i       (llb_fill_perms),
    .fill_data_i        (llb_fill_data),
    .fill_ok_o          (llb_fill_ok),
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
    .dbg_index_i        (llb_dbg_index),
    .dbg_valid_o        (llb_dbg_valid),
    .dbg_line_o         (llb_dbg_line),
    .dbg_asid_o         (llb_dbg_asid),
    .dbg_perms_o        (llb_dbg_perms),
    .dbg_vpn_o          (llb_dbg_vpn),
    .dbg_data_o         (llb_dbg_data),
    .o_count_o          (llb_count),
    .o_entries_o        (llb_entries),
    .o_line_bytes_o     (llb_line_bytes),
    .o_hit_ctr          (llb_hit_ctr),
    .o_miss_ctr         (llb_miss_ctr),
    .o_bypass_ctr       (llb_bypass_ctr),
    .o_fill_ctr         (llb_fill_ctr),
    .o_fill_refused_ctr (llb_fill_refused_ctr),
    .o_inv_ctr          (llb_inv_ctr),
    .o_race_refuse_ctr  (llb_race_refuse_ctr)
  );

endmodule : mosaic_prefetch_tb

/* verilator lint_on UNUSEDSIGNAL */

`default_nettype wire
