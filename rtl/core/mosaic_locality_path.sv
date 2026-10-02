// ============================================================================
// mosaic_locality_path -- work package I-060: the integration of the
// clean-copy locality buffer (rtl/core/mosaic_llb.sv) and the bounded
// prefetcher (rtl/core/mosaic_prefetch.sv) into the *data* path of the core.
//
// ------------------------------------------------------------------ position
//
// The two structures sit on the L1 data cache's **line port**: between
// `mosaic_cache` and `mosaic_cache_line_bridge` inside
// `mosaic_l1_cache_path.sv`. That is the position the card states ("between the
// L1 data cache and the memory service"), and it is the position in which the
// LLB has a natural line fill and a natural line hit:
//
//   * a **line read** the cache issues because a *load* missed goes to the LLB
//     first. On a hit the line is returned to the cache from the clean copy and
//     the memory service is never asked -- the traffic the LLB is supposed to
//     save is saved here and nowhere else.
//   * on a miss the line read goes to the bridge; the line the memory service
//     returns is installed in the cache *and* in the LLB.
//   * a **line write** (a dirty writeback) never consults the LLB: the cache is
//     the authority for the data and a writeback is not a reuse. It is,
//     however, one of the invalidation sources (the line has left the cache).
//   * a line read the cache issues because a *store* missed (a read for
//     ownership) is neither served nor installed: the line is about to be
//     dirtied, and the store's own invalidation has already been applied. The
//     `MOSAIC_LOC_MUTANT_PERM_BYPASS` control drops that rule.
//
// The prefetcher's only egress is a line read, and it shares the *same* bridge
// as the cache (the cache has absolute priority). That is deliberate: a
// prefetch is a request on the core's one memory service, not a second port, so
// the arbiter the core already has is not given a fourth master.
//
// ------------------------------------------------------------- invalidation
//
// The LLB's eight enumerable sources, and where this module gets each:
//
//   #  source                          driven from
//   -  ------------------------------  ------------------------------------
//   1  this hart's store / AMO commit  `inv_store_*` (the endpoint's port)
//   2  L1 refill / replacement         `inv_refill_*`, derived here from the
//                                      cache's own line transactions
//   3  external snoop (another hart,   `inv_snoop_*` (the coherence
//      DMA, device, debugger)          notification the core already carries)
//   4  broadcast / shootdown          `inv_snoop_all_*`
//   5  FENCE (memory)                 `fence_*` kind 0
//   6  FENCE.I                        `fence_*` kind 1
//   7  SFENCE.VMA (by ASID / page)    `fence_*` kind 2, with the VA and ASID
//                                      the core's own system unit publishes
//   8  context change (satp / ASID)   `ctx_flush_*`
//
// ---------------------------------------------------------------- switching
//
// `llb_en_i` and `pf_en_i` are runtime switches, both default 0, so the machine
// every other case builds is unchanged by construction: with both low the LLB
// is neither asked nor filled and the prefetcher is off. `cache.integrated_path`
// is unaffected because its driver leaves the switches at their default 0.
//
// ---------------------------------------------------------------- mutants
//
//   MOSAIC_LOC_MUTANT_PERM_BYPASS   the access-class rule is dropped: a store's
//                                   read-for-ownership may consult and populate
//                                   the buffer, so a clean copy of a line that is
//                                   about to be dirtied is installed -- a copy
//                                   the store's own invalidation has already
//                                   passed. (The stale copy is then observed by
//                                   a later load that misses the L1.)
//   MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE (mosaic_llb.sv)  a store does not
//                                   invalidate the line it wrote.
//   MOSAIC_PREFETCH_MUTANT_DEVICE_READ    (mosaic_prefetch.sv) a prefetch read
//                                   reaches a device.
//   MOSAIC_PREFETCH_MUTANT_ARCH_DATA      (mosaic_prefetch.sv) the fill data is
//                                   corrupted, so enabling the prefetcher
//                                   changes an architectural result.
// ============================================================================

`default_nettype none
`resetall

`ifndef MOSAIC_LOCALITY_PATH_SV_
`define MOSAIC_LOCALITY_PATH_SV_

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
`include "mosaic_pkg.sv"

module mosaic_locality_path #(
  parameter int unsigned LINE_BYTES    = 32,
  parameter int unsigned ADDR_WIDTH    = 64,
  parameter int unsigned ENTRIES       = 8,
  parameter int unsigned TABLE_ENTRIES = 16,
  parameter int unsigned DEPTH         = 4,
  localparam int unsigned OFFSET_BITS  = $clog2(LINE_BYTES),
  localparam int unsigned LINE_BITS    = LINE_BYTES * 8,
  localparam int unsigned PA_LINE_W     = 32 - OFFSET_BITS
) (
  input  logic                        clk,
  input  logic                        rst,

  // ------------------------------------------------------------- switches
  input  logic                        llb_en_i,
  input  logic                        pf_en_i,
  input  logic [1:0]                  pf_conf_thresh_i,

  // --------------------------------------------------- the cache's line port
  input  logic                        cache_line_req_valid,
  output logic                        cache_line_req_ready,
  input  logic                        cache_line_req_we,
  input  logic [ADDR_WIDTH-1:0]       cache_line_req_addr,
  input  logic [LINE_BITS-1:0]        cache_line_req_wdata,
  output logic                        cache_line_rsp_valid,
  output logic [LINE_BITS-1:0]        cache_line_rsp_rdata,
  output logic                        cache_line_rsp_fault,

  // ------------------------------------------------ the width adapter's port
  output logic                        bridge_line_req_valid,
  input  logic                        bridge_line_req_ready,
  output logic                        bridge_line_req_we,
  output logic [ADDR_WIDTH-1:0]       bridge_line_req_addr,
  output logic [LINE_BITS-1:0]        bridge_line_req_wdata,
  input  logic                        bridge_line_rsp_valid,
  input  logic [LINE_BITS-1:0]        bridge_line_rsp_rdata,
  input  logic                        bridge_line_rsp_fault,

  // ------------------------------------------- the in-flight cache transaction
  // Latched by the caller when the cache accepted the CPU request that is being
  // served. `ctx_is_load` is what makes the store/miss distinction, and the
  // permission context is the triple the LLB's key is defined over.
  input  logic                        ctx_is_load_i,
  input  logic [26:0]                 ctx_vpn_i,
  input  logic [15:0]                 ctx_asid_i,
  input  logic [3:0]                  ctx_perms_i,
  input  logic [63:0]                 ctx_pc_i,

  // ---------------------------------------------------- invalidation sources
  input  logic                        inv_store_valid_i,
  input  logic [63:0]                 inv_store_pa_i,
  input  logic                        inv_snoop_valid_i,
  input  logic [63:0]                 inv_snoop_pa_i,
  input  logic                        inv_snoop_all_i,
  input  logic                        fence_valid_i,
  input  logic [1:0]                  fence_kind_i,
  input  logic [26:0]                 fence_vpn_i,
  input  logic                        fence_has_vpn_i,
  input  logic [15:0]                 fence_asid_i,
  input  logic                        fence_has_asid_i,
  input  logic                        ctx_flush_valid_i,

  // ---------------------------------------------------------- observability
  output logic [31:0]                 o_llb_hit_o,
  output logic [31:0]                 o_llb_miss_o,
  output logic [31:0]                 o_llb_bypass_o,
  output logic [31:0]                 o_llb_fill_o,
  output logic [31:0]                 o_llb_fill_refused_o,
  output logic [31:0]                 o_llb_inv_o,
  output logic [31:0]                 o_mem_line_o,      // cache line txns to memory
  output logic [31:0]                 o_pf_issued_o,
  output logic [31:0]                 o_pf_useful_o,
  output logic [31:0]                 o_pf_useless_o,
  output logic [31:0]                 o_pf_late_o,
  output logic [31:0]                 o_pf_cancelled_o,
  output logic [31:0]                 o_pf_admitted_o,
  output logic [31:0]                 o_pf_fill_o,
  output logic [31:0]                 o_pf_fill_refused_o,
  output logic [31:0]                 o_pf_mem_o         // prefetch line reads to memory
);

  localparam logic OWN_CACHE = 1'b0;
  localparam logic OWN_PF    = 1'b1;

`ifdef MOSAIC_LOC_MUTANT_PERM_BYPASS
  // NEGATIVE CONTROL: the access-class rule is dropped, so a store's
  // read-for-ownership consults and populates the buffer just as a load's
  // refill does. The buffer then holds a clean copy of a line that is about to
  // be dirtied -- a copy the store's invalidation has already passed -- and a
  // later load that misses the L1 reads the pre-store value.
  localparam logic LOAD_ONLY = 1'b0;
`else
  localparam logic LOAD_ONLY = 1'b1;
`endif

  // ==========================================================================
  // Signals
  // ==========================================================================
  logic                 llb_hit_c;
  logic                 llb_bypass_c;
  logic                 llb_fill_ok_c;
  logic [LINE_BITS-1:0] llb_req_data_c;
  logic                 llb_evict_valid_c;
  logic [PA_LINE_W-1:0] llb_evict_line_c;

  logic        llb_lookup_c;
  logic        llb_serve_c;
  logic        llb_req_valid_c;
  logic [63:0] llb_req_pa_c;
  logic [3:0]  llb_req_perms_c;

  logic                 llb_hold_valid_q;
  logic [LINE_BITS-1:0] llb_hold_data_q;

  logic                 pf_mem_req_valid;
  logic [63:0]          pf_mem_req_pa;
  logic [7:0]           pf_mem_req_id;
  logic                 pf_rsp_c;
  logic                 pf_fill_valid;
  logic                 pf_fill_gate_c;
  logic                 pf_fill_ok_c;
  logic [63:0]          pf_fill_pa;
  logic [26:0]          pf_fill_vpn;
  logic [15:0]          pf_fill_asid;
  logic [3:0]           pf_fill_perms;
  logic [LINE_BITS-1:0] pf_fill_data;
  logic                 pf_flush_c;
  logic                 pf_release_store_c;
  logic                 pf_release_snoop_c;
  logic                 pf_release_valid_c;
  logic [63:0]          pf_release_line_c;
  logic [63:0]          pf_cand_pa;
  logic                 gate_mapped_c;
  logic                 gate_device_c;

  // The prefetcher offers a candidate for exactly one cycle -- the cycle its
  // training demand's line read is taken -- and in that cycle the demand is
  // usually using the bridge itself. A candidate refused then would be lost, and
  // the prefetcher would never issue anything on a shared port. So the wrapper
  // is a small in-order request queue between the prefetcher and the bridge: the
  // module is told "accepted" as soon as the queue has room (a legal
  // valid/ready backpressure, and the module's own DEPTH bounds the queue), and
  // the read is performed on the bridge as soon as the cache wants nothing.
  localparam int unsigned PFQ_IDX_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH);
  localparam int unsigned PFQ_CNT_W = $clog2(DEPTH + 1);
  logic [PFQ_CNT_W-1:0] pfq_count_q;
  logic [63:0]          pfq_pa_q    [0:DEPTH-1];
  logic [7:0]           pfq_id_q    [0:DEPTH-1];
  logic [PFQ_IDX_W-1:0] pfq_head_q;
  logic [PFQ_IDX_W-1:0] pfq_tail_q;
  logic                 pfq_full_c;
  logic                 pfq_empty_c;
  logic                 pfq_push_c;
  logic                 pfq_pop_c;
  logic [7:0]           pf_inflight_id_q;

  logic cache_to_bridge_valid_c;
  logic cache_bridge_accept_c;
  logic cache_rsp_from_bridge_c;
  logic bridge_owner_q;
  logic dem_valid_c;

  logic        inv_refill_valid_c;
  logic [63:0] inv_refill_pa_c;

  logic                 llb_fill_valid_c;
  logic [63:0]          llb_fill_pa_c;
  logic [26:0]          llb_fill_vpn_c;
  logic [15:0]          llb_fill_asid_c;
  logic [3:0]           llb_fill_perms_c;
  logic [LINE_BITS-1:0] llb_fill_data_c;

  logic                 fill_ctx_is_load_q;
  logic [63:0]          fill_ctx_pa_q;
  logic [26:0]          fill_ctx_vpn_q;
  logic [15:0]          fill_ctx_asid_q;
  logic [3:0]           fill_ctx_perms_q;
  logic                 demand_fill_c;

  logic [31:0] mem_line_ctr_q;
  logic [31:0] pf_mem_ctr_q;

  // ==========================================================================
  // The lookup
  // ==========================================================================
  assign llb_req_pa_c = {{(64-ADDR_WIDTH){1'b0}}, cache_line_req_addr};

`ifdef MOSAIC_LOC_MUTANT_PERM_BYPASS
  // The lookup does not ask which access class it is answering.
  assign llb_req_perms_c = 4'b0000;
`else
  assign llb_req_perms_c = ctx_perms_i;
`endif

  // A line read is offered to the LLB when the buffer is enabled, the read is
  // a *load*'s (a store's read-for-ownership is not a reuse and must not install
  // a copy of a line about to be dirtied), and no earlier hit's data is still
  // waiting to be handed back.
  assign llb_lookup_c     = llb_en_i && cache_line_req_valid && !cache_line_req_we &&
                            (LOAD_ONLY ? ctx_is_load_i : 1'b1) && !llb_hold_valid_q;
  assign llb_req_valid_c  = llb_lookup_c;
  assign llb_serve_c      = llb_lookup_c && llb_hit_c && !llb_bypass_c;

  /* verilator lint_off PINCONNECTEMPTY */
  mosaic_llb #(
    .LINE_BYTES (LINE_BYTES),
    .ENTRIES    (ENTRIES),
    .ADDR_WIDTH (32)
  ) u_llb (
    .clk                (clk),
    .rst                (rst),
    .req_valid_i        (llb_req_valid_c),
    .req_pa_i           (llb_req_pa_c),
    .req_vpn_i          (ctx_vpn_i),
    .req_asid_i         (ctx_asid_i),
    .req_perms_i        (llb_req_perms_c),
    .req_atomic_i       (1'b0),
    .req_hit_o          (llb_hit_c),
    .req_data_o         (llb_req_data_c),
    .req_bypass_o       (llb_bypass_c),
    .fill_valid_i       (llb_fill_valid_c),
    .fill_pa_i          (llb_fill_pa_c),
    .fill_vpn_i         (llb_fill_vpn_c),
    .fill_asid_i        (llb_fill_asid_c),
    .fill_perms_i       (llb_fill_perms_c),
    .fill_data_i        (llb_fill_data_c),
    .fill_ok_o          (llb_fill_ok_c),
    .inv_store_valid_i  (inv_store_valid_i),
    .inv_store_pa_i     (inv_store_pa_i),
    .inv_refill_valid_i (inv_refill_valid_c),
    .inv_refill_pa_i    (inv_refill_pa_c),
    .inv_snoop_valid_i  (inv_snoop_valid_i),
    .inv_snoop_pa_i     (inv_snoop_pa_i),
    .inv_snoop_all_i    (inv_snoop_all_i),
    .fence_valid_i      (fence_valid_i),
    .fence_kind_i       (fence_kind_i),
    .fence_vpn_i        (fence_vpn_i),
    .fence_has_vpn_i    (fence_has_vpn_i),
    .fence_asid_i       (fence_asid_i),
    .fence_has_asid_i   (fence_has_asid_i),
    .ctx_valid_i        (ctx_flush_valid_i),
    .dbg_index_i        (3'b0),
    .dbg_valid_o        (),
    .dbg_line_o         (),
    .dbg_asid_o         (),
    .dbg_perms_o        (),
    .dbg_vpn_o          (),
    .dbg_data_o         (),
    .o_count_o          (),
    .o_entries_o        (),
    .o_line_bytes_o     (),
    .o_hit_ctr          (o_llb_hit_o),
    .o_miss_ctr         (o_llb_miss_o),
    .o_bypass_ctr       (o_llb_bypass_o),
    .o_fill_ctr         (o_llb_fill_o),
    .o_fill_refused_ctr (o_llb_fill_refused_o),
    .o_inv_ctr          (o_llb_inv_o),
    .o_race_refuse_ctr  (),
    .o_evict_valid_o    (llb_evict_valid_c),
    .o_evict_line_o     (llb_evict_line_c)
  );
  /* verilator lint_on PINCONNECTEMPTY */

  // ==========================================================================
  // The prefetcher
  // ==========================================================================
  /* verilator lint_off PINCONNECTEMPTY */
  mosaic_prefetch #(
    .TABLE_ENTRIES (TABLE_ENTRIES),
    .DEPTH         (DEPTH),
    .ADDR_WIDTH    (32),
    .LINE_BYTES    (LINE_BYTES)
  ) u_pf (
    .clk                (clk),
    .rst                (rst),
    .en_i               (pf_en_i),
    .conf_thresh_i      (pf_conf_thresh_i),
    .dem_valid_i        (dem_valid_c),
    .dem_pc_i           (ctx_pc_i),
    .dem_pa_i           ({{(64-32){1'b0}}, cache_line_req_addr[31:0]}),
    .dem_vpn_i          (ctx_vpn_i),
    .dem_asid_i         (ctx_asid_i),
    .dem_perms_i        (ctx_perms_i),
    .dem_hit_i          (llb_hit_c),
    .dem_fault_i        (1'b0),
    .pf_valid_o         (),
    .pf_pa_o            (pf_cand_pa),
    .pf_vpn_o           (),
    .pf_asid_o          (),
    .pf_perms_o         (),
    .gate_mapped_i      (gate_mapped_c),
    .gate_perm_ok_i     (1'b1),
    .gate_device_i      (gate_device_c),
    .mem_req_valid_o    (pf_mem_req_valid),
    .mem_req_pa_o       (pf_mem_req_pa),
    .mem_req_id_o       (pf_mem_req_id),
    .mem_req_ready_i    (!pfq_full_c),
    .mem_resp_valid_i   (pf_rsp_c),
    .mem_resp_data_i    (bridge_line_rsp_rdata),
    .mem_resp_id_i      (pf_inflight_id_q),
    .pf_cancel_i        (1'b0),
    .pf_cancel_id_i     (8'b0),
    .pf_flush_i         (pf_flush_c),
    .pf_release_valid_i (pf_release_valid_c),
    .pf_release_line_i  (pf_release_line_c),
    .fill_valid_o       (pf_fill_valid),
    .fill_pa_o          (pf_fill_pa),
    .fill_vpn_o         (pf_fill_vpn),
    .fill_asid_o        (pf_fill_asid),
    .fill_perms_o       (pf_fill_perms),
    .fill_data_o        (pf_fill_data),
    .fill_ok_i          (pf_fill_ok_c),
    .o_obs_accesses_o   (),
    .o_obs_new_pc_o     (),
    .o_obs_repeat_pc_o  (),
    .o_obs_stride_match_o (),
    .o_obs_stride_mismatch_o (),
    .o_obs_stride_zero_o (),
    .o_obs_reuse_hit_o  (),
    .o_issued_o         (o_pf_issued_o),
    .o_useful_o         (o_pf_useful_o),
    .o_useless_o        (o_pf_useless_o),
    .o_late_o           (o_pf_late_o),
    .o_cancelled_o      (o_pf_cancelled_o),
    .o_admitted_o       (o_pf_admitted_o),
    .o_gate_refuse_o    (),
    .o_full_stall_o     (),
    .o_fill_ctr_o       (o_pf_fill_o),
    .o_fill_refused_ctr_o (o_pf_fill_refused_o),
    .o_dropped_ctr_o    (),
    .o_inflight_o       (),
    .o_table_entries_o  (),
    .o_depth_o          ()
  );
  /* verilator lint_on PINCONNECTEMPTY */

  // The prefetch candidate's own gate: the same platform map the demand path
  // asks. `gate_perm_ok` is the *demand's* answer and is therefore a constant
  // 1 here: the line read this observation comes from is one the demand path
  // already permitted (a device access never produces a cache line request).
  assign gate_mapped_c = mosaic_cfg_pkg::mosaic_pa_idempotent(pf_cand_pa) ||
                         mosaic_cfg_pkg::mosaic_pa_device(pf_cand_pa);
  assign gate_device_c = mosaic_cfg_pkg::mosaic_pa_device(pf_cand_pa);

  // ==========================================================================
  // The line-path mux and the response routing
  // ==========================================================================
  assign cache_to_bridge_valid_c = cache_line_req_valid && !llb_serve_c;
  assign cache_bridge_accept_c   = cache_to_bridge_valid_c && bridge_line_req_ready;
  assign cache_rsp_from_bridge_c = bridge_line_rsp_valid && (bridge_owner_q == OWN_CACHE);
  assign pf_rsp_c                = bridge_line_rsp_valid && (bridge_owner_q == OWN_PF);

  // The demand observation is a one-cycle pulse on the cycle the demand's line
  // read is *taken* -- by the LLB or by the bridge. A multi-cycle presentation
  // (the bridge busy with a prefetch) is therefore one demand, not several.
  assign dem_valid_c = (llb_serve_c || cache_bridge_accept_c) &&
                       !cache_line_req_we && ctx_is_load_i;

  // The bridge's request side: the cache first, so a demand can never be held
  // behind a hint. The prefetch is taken only in a cycle the cache wants
  // nothing.
  assign bridge_line_req_valid = cache_to_bridge_valid_c || !pfq_empty_c;
  assign bridge_line_req_we    = cache_to_bridge_valid_c ? cache_line_req_we : 1'b0;
  assign bridge_line_req_addr  = cache_to_bridge_valid_c ? cache_line_req_addr
                                                         : pfq_pa_q[pfq_head_q];
  assign bridge_line_req_wdata = cache_to_bridge_valid_c ? cache_line_req_wdata
                                                         : {LINE_BITS{1'b0}};
  assign pfq_full_c            = (pfq_count_q == PFQ_CNT_W'(DEPTH));
  assign pfq_empty_c           = (pfq_count_q == PFQ_CNT_W'(0));
  assign pfq_push_c            = pf_mem_req_valid && !pfq_full_c;
  assign pfq_pop_c             = !pfq_empty_c && !cache_to_bridge_valid_c &&
                                 bridge_line_req_ready;

  assign cache_line_req_ready  = llb_hold_valid_q ? 1'b0
                               : (llb_serve_c ? 1'b1 : bridge_line_req_ready);

  assign cache_line_rsp_valid  = llb_hold_valid_q || cache_rsp_from_bridge_c;
  assign cache_line_rsp_rdata  = llb_hold_valid_q ? llb_hold_data_q
                                                  : bridge_line_rsp_rdata;
  assign cache_line_rsp_fault  = llb_hold_valid_q ? 1'b0 : bridge_line_rsp_fault;

  // The invalidation that reaches the LLB from the memory side: every line
  // transaction the cache makes to memory, both a refill and a writeback. The
  // cache is the authority for the data, so a line it (re)fetched from memory,
  // or wrote back, is newer than any private copy taken before.
  //
  // It is *registered*, not combinational, and that is load-bearing: the LLB's
  // own hit output depends on its invalidation inputs (a cycle that invalidates
  // an entry grants no hit), and the bridge acceptance that would drive this
  // port is itself a function of the hit. Driving it combinationally is a real
  // combinational loop -- Verilator's UNOPTFLAT names it -- and a registered
  // pulse is also the more conservative statement: the private copy is gone one
  // cycle after the line left the cache, never before.
  logic        inv_refill_valid_q;
  logic [63:0] inv_refill_pa_q;
  assign inv_refill_valid_c = inv_refill_valid_q;
  assign inv_refill_pa_c    = inv_refill_pa_q;

  // ==========================================================================
  // The fill port (one port, two producers)
  // ==========================================================================
  assign demand_fill_c = cache_rsp_from_bridge_c && !bridge_line_rsp_fault &&
                         (LOAD_ONLY ? fill_ctx_is_load_q : 1'b1);

  assign llb_fill_valid_c = llb_en_i && (demand_fill_c || pf_fill_gate_c);
  assign llb_fill_pa_c    = demand_fill_c ? fill_ctx_pa_q    : pf_fill_pa;
  assign llb_fill_vpn_c   = demand_fill_c ? fill_ctx_vpn_q   : pf_fill_vpn;
  assign llb_fill_asid_c  = demand_fill_c ? fill_ctx_asid_q  : pf_fill_asid;
  assign llb_fill_perms_c = demand_fill_c ? fill_ctx_perms_q : pf_fill_perms;
  assign llb_fill_data_c  = demand_fill_c ? bridge_line_rsp_rdata : pf_fill_data;

  // A prefetch's fill is refused (and reported as such) when the response
  // faulted or when the demand fill owns the port this cycle. A faulted
  // prefetch line must never be installed: the memory service faulted the read,
  // so the bytes are meaningless.
  assign pf_fill_gate_c = pf_fill_valid && !bridge_line_rsp_fault;
  assign pf_fill_ok_c   = pf_fill_gate_c && !demand_fill_c && llb_fill_ok_c;

  // ==========================================================================
  // A prefetch's window
  // ==========================================================================
  // A landed prefetch stops being useful the moment its line leaves the buffer:
  // the next demand for that line will refetch it from memory. It leaves on a
  // store's or a snoop's invalidation, or when a fill replaces it. A global
  // invalidation (FENCE, a context change, a shootdown) cancels every
  // outstanding prefetch instead, because naming one line would be a guess.
  assign pf_flush_c         = ctx_flush_valid_i || fence_valid_i || inv_snoop_all_i;
  assign pf_release_store_c = inv_store_valid_i;
  assign pf_release_snoop_c = inv_snoop_valid_i && !inv_snoop_all_i;
  assign pf_release_valid_c = pf_release_store_c || pf_release_snoop_c || llb_evict_valid_c;
  assign pf_release_line_c  = pf_release_store_c ? inv_store_pa_i
                            : pf_release_snoop_c ? inv_snoop_pa_i
                            : {{(64-32){1'b0}}, llb_evict_line_c, {OFFSET_BITS{1'b0}}};

  // ==========================================================================
  // The edge
  // ==========================================================================
  always_ff @(posedge clk) begin
    if (rst) begin
      llb_hold_valid_q   <= 1'b0;
      llb_hold_data_q    <= {LINE_BITS{1'b0}};
      bridge_owner_q     <= OWN_CACHE;
      pf_resp_id_q       <= 8'd0;
      mem_line_ctr_q     <= 32'd0;
      pf_mem_ctr_q       <= 32'd0;
      fill_ctx_is_load_q <= 1'b0;
      fill_ctx_pa_q      <= 64'd0;
      fill_ctx_vpn_q     <= 27'd0;
      fill_ctx_asid_q    <= 16'd0;
      fill_ctx_perms_q   <= 4'd0;
      inv_refill_valid_q <= 1'b0;
      inv_refill_pa_q    <= 64'd0;
      pfq_count_q        <= {PFQ_CNT_W{1'b0}};
      pfq_head_q         <= {PFQ_IDX_W{1'b0}};
      pfq_tail_q         <= {PFQ_IDX_W{1'b0}};
      pf_inflight_id_q   <= 8'd0;
    end else begin
      // The registered memory-side invalidation: one cycle after the cache
      // handed a line to the bridge.
      inv_refill_valid_q <= cache_bridge_accept_c;
      if (cache_bridge_accept_c) begin
        inv_refill_pa_q <= {{(64-ADDR_WIDTH){1'b0}}, cache_line_req_addr};
      end
      // The LLB's answer is registered for one cycle, because the cache waits
      // for `mem_resp_valid` after `mem_req_ready` and the LLB's own hit is
      // combinational with the request.
      if (llb_serve_c) begin
        llb_hold_valid_q <= 1'b1;
        llb_hold_data_q  <= llb_req_data_c;
      end else begin
        llb_hold_valid_q <= 1'b0;
      end

      // Which producer the bridge is serving. Set on the acceptance and stable
      // for the whole transaction (the bridge's ready is low while it is busy),
      // so the response is routed to the producer that asked.
      if (bridge_line_req_valid && bridge_line_req_ready) begin
        bridge_owner_q <= cache_to_bridge_valid_c ? OWN_CACHE : OWN_PF;
      end
      // (the prefetch's response is routed by the same owner bit; the pop above
      // is the same event as this acceptance when the prefetcher is selected)

      // The prefetch request queue, and the id of the transaction the bridge is
      // carrying for it. The bridge has no id field, so the id travels beside
      // the request in the queue and is latched when the request is popped.
      if (pfq_push_c) begin
        pfq_pa_q[pfq_tail_q] <= pf_mem_req_pa;
        pfq_id_q[pfq_tail_q] <= pf_mem_req_id;
      end
      if (pfq_pop_c) pf_inflight_id_q <= pfq_id_q[pfq_head_q];
      pfq_head_q <= pfq_head_q + PFQ_IDX_W'(pfq_pop_c);
      pfq_tail_q <= pfq_tail_q + PFQ_IDX_W'(pfq_push_c);
      pfq_count_q <= pfq_count_q + PFQ_CNT_W'(pfq_push_c) - PFQ_CNT_W'(pfq_pop_c);

      // The context of the line transaction in flight, captured when the
      // bridge takes it. The fill a read produces is labelled with it, so a
      // copy cannot be installed under a context the access did not have.
      if (cache_bridge_accept_c) begin
        fill_ctx_is_load_q <= ctx_is_load_i;
        fill_ctx_pa_q      <= {{(64-ADDR_WIDTH){1'b0}}, cache_line_req_addr};
        fill_ctx_vpn_q     <= ctx_vpn_i;
        fill_ctx_asid_q    <= ctx_asid_i;
        fill_ctx_perms_q   <= ctx_perms_i;
      end

      if (cache_bridge_accept_c) mem_line_ctr_q <= mem_line_ctr_q + 32'd1;
      if (pfq_pop_c) pf_mem_ctr_q <= pf_mem_ctr_q + 32'd1;
    end
  end

  assign o_mem_line_o = mem_line_ctr_q;
  assign o_pf_mem_o   = pf_mem_ctr_q;

endmodule

`endif  // MOSAIC_LOCALITY_PATH_SV_
`resetall
