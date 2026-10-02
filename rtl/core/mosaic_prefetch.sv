// ============================================================================
// mosaic_prefetch -- work package I-063: a bounded, switchable reuse/stride
// prefetcher whose every request passes the *same* permission, alias and
// cacheability checks as a demand access.
//
// A prefetch is a *hint*. This module therefore has no fault output, no
// back-pressure onto the demand path, and no way to make a demand slower or
// different: it observes demand accesses on `dem_*`, keeps a small stride
// table, and offers a candidate line. The candidate becomes a memory read only
// when the environment's gate says the address is mapped, the access context
// permits the read, and the region is not a device (the generated map marks
// every non-idempotent region as a device). That gate is the *same* predicate
// the demand path uses; the platform map itself is never a parameter of this
// module, so a predictor cannot redefine where a device lives or who may read
// what.
//
// ------------------------------------------------------------- the bound
//
// Three bounds, each a property of the state machine rather than of the
// workload:
//
//   * at most `TABLE_ENTRIES` streams are remembered, one entry per PC;
//   * at most `DEPTH` prefetches are outstanding, and a prefetch whose line is
//     already outstanding is not issued again;
//   * **a prefetch never trains the table.** Only a demand access updates an
//     entry. This is what makes the traffic bounded by the demand stream: a
//     wrong prediction cannot manufacture more predictions.
//
// With those, a burst of demand accesses can add at most `DEPTH` outstanding
// reads; the traffic a prefetcher can add is bounded by the table size times
// the demand rate, and it is bounded *before* any prediction is enabled.
//
// ------------------------------------------------------------- the switch
//
// `en_i` is the switch. When it is low no candidate is produced and no memory
// request is issued -- but the table still *observes* demand accesses and the
// observation counters below still move. That is deliberate: the card requires
// the workload's PC/stride/reuse characteristics to be recorded *before*
// prediction is enabled, so the record must not depend on the switch. A
// prediction whose expected benefit was never measured is a guess with a state
// machine attached.
//
// ------------------------------------------- the harm bound, in two halves
//
// (a) A wrong prediction can only add latency or traffic.
//     Every egress read is gated by `gate_mapped_i`, `gate_perm_ok_i` and
//     `gate_side_effect_free_i`; a candidate that fails any of them is counted
//     in `o_gate_refuse_o` and *nothing else happens*. There is no trap path,
//     no architectural state, and no request to a device. A prefetch to an
//     unmapped or device address is therefore unrepresentable as a fault or a
//     side effect: it is a refused hint.
//
// (b) The predictor is never the only source of correctness.
//     A prefetch never writes a demand-visible location and never forwards a
//     value to the fetch/load path. It can only make the locality buffer hold a
//     copy that a later demand *may* hit; the fill takes its data from the
//     memory response and its context from the demand that trained the entry.
//     The consumer's own memory oracle is unchanged.
//
// ---------------------------------------------------------- the alias rule
//
// The stride table is keyed by the PC (the control identity) and the predicted
// address is formed from the *physical* line of the demand
// (`dem_pa_i[ADDR_WIDTH-1:OFFSET_BITS]`): the virtual address only labels the
// context carried to the locality buffer (`pf_vpn_o`). Two virtual aliases of
// one physical line therefore produce one prefetch, and the fill a prefetch
// installs carries the physical line -- it cannot populate something the demand
// path, which keys the same way, would have refused under a different context.
//
// -------------------------------------------------- cancelled leaves nothing
//
// A cancelled prefetch's data must never land. The response path checks the
// slot's `cancelled` flag and, if set, drops the response (`o_dropped_ctr`)
// without presenting a fill. A landed prefetch's slot is only freed after its
// response has been seen, so a freed slot can never be mistaken for a slot with
// a pending response: there is no ABA on the outstanding table.
//
// ---------------------------------------------------------------- mutants
//
//   MOSAIC_PREFETCH_MUTANT_PERM_BYPASS   the translation/permission group is
//                                        dropped, so a prefetch reaches an
//                                        address a demand would have refused
//                                        -- the card's central failure
//   MOSAIC_PREFETCH_MUTANT_DEVICE_READ   the device fact is dropped, so a
//                                        prefetch performs an irreversible
//                                        device read
//   MOSAIC_PREFETCH_MUTANT_CANCEL_LANDS  a cancelled prefetch's response is
//                                        installed anyway
//   MOSAIC_PREFETCH_MUTANT_ARCH_DATA     the fill data is corrupted, so a
//                                        prediction changes a later demand's
//                                        architectural value
// ============================================================================

`ifndef MOSAIC_PREFETCH_SV_
`define MOSAIC_PREFETCH_SV_

module mosaic_prefetch #(
  parameter int unsigned TABLE_ENTRIES = 16,   // remembered PC streams
  parameter int unsigned DEPTH         = 4,    // outstanding prefetches
  parameter int unsigned ADDR_WIDTH    = 32,   // physical address width
  parameter int unsigned LINE_BYTES    = 32    // bytes per line, power of two
) (
  input  logic                      clk,
  input  logic                      rst,

  // ---------------------------------------------------------------- switch
  input  logic                      en_i,           // 1 = issue prefetches
  input  logic [1:0]                conf_thresh_i,  // stride matches before firing

  // ----------------------------------------------------- demand observation
  // The predictor trains on demand accesses only. `dem_hit_i` is the locality
  // buffer's answer for this demand (the reuse observation); `dem_fault_i` says
  // this demand would fault, and a faulting access must not spawn a hint.
  //
  // The high address bits carry no meaning for a 32-bit physical machine, and
  // the low sub-line bits are declared away once here, as the LLB does for its
  // line-granular invalidation ports.
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic                      dem_valid_i,
  input  logic [63:0]               dem_pc_i,
  input  logic [63:0]               dem_pa_i,
  /* verilator lint_on UNUSEDSIGNAL */
  input  logic [26:0]               dem_vpn_i,
  input  logic [15:0]               dem_asid_i,
  input  logic [3:0]                dem_perms_i,
  input  logic                      dem_hit_i,
  input  logic                      dem_fault_i,

  // ------------------------------------------------------- prefetch candidate
  output logic                      pf_valid_o,
  output logic [63:0]               pf_pa_o,
  output logic [26:0]               pf_vpn_o,
  output logic [15:0]               pf_asid_o,
  output logic [3:0]                pf_perms_o,

  // ------------------------------- the shared gate's answer for `pf_pa_o`
  // Computed by the environment (the generated platform map and the page table),
  // never by this module. The module only decides whether to honour it. Two
  // groups, so the two fail modes are separable: the translation/permission
  // group and the device fact. The platform map marks every non-idempotent
  // region as a device, so `mapped && !device` is normal memory here; the
  // driver's own reader checks idempotency independently as a backstop.
  input  logic                      gate_mapped_i,
  input  logic                      gate_perm_ok_i,
  input  logic                      gate_device_i,

  // ------------------------------------------------------------ memory read
  // The one egress a prefetch has. A read is presented only for an admitted
  // candidate; the response is the line's data.
  output logic                      mem_req_valid_o,
  output logic [63:0]               mem_req_pa_o,
  output logic [7:0]                mem_req_id_o,
  input  logic                      mem_req_ready_i,
  input  logic                      mem_resp_valid_i,
  input  logic [LINE_BYTES*8-1:0]   mem_resp_data_i,
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic [7:0]                mem_resp_id_i,   // high bits do not name a slot
  /* verilator lint_on UNUSEDSIGNAL */

  // -------------------------------------------------------------- lifecycle
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic                      pf_cancel_i,        // cancel one, by id
  input  logic [7:0]                pf_cancel_id_i,     // low IDX_W bits name the slot
  input  logic                      pf_flush_i,         // cancel every outstanding
  input  logic                      pf_release_valid_i, // a landed line's window closed
  input  logic [63:0]               pf_release_line_i,  // high and sub-line bits unused
  /* verilator lint_on UNUSEDSIGNAL */

  // ------------------------------------------------------------- LLB fill
  // The prefetch's only architectural-adjacent effect: install a clean copy in
  // the locality buffer. The buffer's own rules decide whether the copy is
  // legal (`fill_ok_i`); the fill's context is the training demand's.
  output logic                      fill_valid_o,
  output logic [63:0]               fill_pa_o,
  output logic [26:0]               fill_vpn_o,
  output logic [15:0]               fill_asid_o,
  output logic [3:0]                fill_perms_o,
  output logic [LINE_BYTES*8-1:0]   fill_data_o,
  input  logic                      fill_ok_i,

  // ---------------------------------------------------- the characteristics
  // Recorded on every demand access, independent of `en_i`.
  output logic [31:0]               o_obs_accesses_o,
  output logic [31:0]               o_obs_new_pc_o,
  output logic [31:0]               o_obs_repeat_pc_o,
  output logic [31:0]               o_obs_stride_match_o,
  output logic [31:0]               o_obs_stride_mismatch_o,
  output logic [31:0]               o_obs_stride_zero_o,
  output logic [31:0]               o_obs_reuse_hit_o,

  // -------------------------------------------------------- usefulness report
  output logic [31:0]               o_issued_o,
  output logic [31:0]               o_useful_o,
  output logic [31:0]               o_useless_o,
  output logic [31:0]               o_late_o,
  output logic [31:0]               o_cancelled_o,
  output logic [31:0]               o_admitted_o,
  output logic [31:0]               o_gate_refuse_o,
  output logic [31:0]               o_full_stall_o,
  output logic [31:0]               o_fill_ctr_o,
  output logic [31:0]               o_fill_refused_ctr_o,
  output logic [31:0]               o_dropped_ctr_o,
  output logic [31:0]               o_inflight_o,

  // --------------------------------------------------------------- geometry
  output logic [31:0]               o_table_entries_o,
  output logic [31:0]               o_depth_o
);

  localparam int unsigned OFFSET_BITS = $clog2(LINE_BYTES);
  localparam int unsigned LINE_BITS   = LINE_BYTES * 8;
  localparam int unsigned LINE_MSB_W  = ADDR_WIDTH - OFFSET_BITS;
  localparam int unsigned TIDX_W      = (TABLE_ENTRIES <= 1) ? 1 : $clog2(TABLE_ENTRIES);
  localparam int unsigned PC_TAG_W    = 32 - TIDX_W - 2;
  localparam int unsigned IDX_W       = (DEPTH <= 1) ? 1 : $clog2(DEPTH);

`ifdef MOSAIC_PREFETCH_MUTANT_PERM_BYPASS
  // NEGATIVE CONTROL: the read-safe group (mapped, permitted, idempotent) is not
  // applied, so a candidate reaches an address a demand would have refused.
  localparam logic CHECK_READ_SAFE = 1'b0;
`else
  localparam logic CHECK_READ_SAFE = 1'b1;
`endif

`ifdef MOSAIC_PREFETCH_MUTANT_DEVICE_READ
  // NEGATIVE CONTROL: the device-free fact is not applied, so a prefetch
  // performs an irreversible device read.
  localparam logic CHECK_DEVICE_FREE = 1'b0;
`else
  localparam logic CHECK_DEVICE_FREE = 1'b1;
`endif

`ifdef MOSAIC_PREFETCH_MUTANT_CANCEL_LANDS
  // NEGATIVE CONTROL: a cancelled prefetch's response is installed anyway.
  localparam logic DROP_CANCELLED = 1'b0;
`else
  localparam logic DROP_CANCELLED = 1'b1;
`endif

`ifdef MOSAIC_PREFETCH_MUTANT_ARCH_DATA
  // NEGATIVE CONTROL: the fill data is corrupted, so a prediction changes a
  // later demand's architectural value.
  localparam logic CORRUPT_FILL_DATA = 1'b1;
`else
  localparam logic CORRUPT_FILL_DATA = 1'b0;
`endif

  // ------------------------------------------------------------- the table
  logic [TABLE_ENTRIES-1:0] tbl_valid_q;
  logic [PC_TAG_W-1:0]      tbl_tag_q    [0:TABLE_ENTRIES-1];
  logic [31:0]              tbl_stride_q [0:TABLE_ENTRIES-1];   // line stride, two's complement
  logic [31:0]              tbl_line_q   [0:TABLE_ENTRIES-1];   // last physical line seen
  logic [1:0]               tbl_conf_q   [0:TABLE_ENTRIES-1];

  // ------------------------------------------------------- outstanding table
  // A slot is freed only when no response is pending for it: a landed slot has
  // already seen its response, and a cancelled in-flight slot is kept until the
  // response arrives (and dropped then). No ABA.
  logic [DEPTH-1:0] os_valid_q;
  logic [DEPTH-1:0] os_landed_q;
  logic [DEPTH-1:0] os_cancelled_q;
  logic [31:0]      os_line_q   [0:DEPTH-1];
  logic [63:0]      os_pa_q     [0:DEPTH-1];
  logic [26:0]      os_vpn_q    [0:DEPTH-1];
  logic [15:0]      os_asid_q   [0:DEPTH-1];
  logic [3:0]       os_perms_q  [0:DEPTH-1];

  // ---------------------------------------------------------------- counters
  logic [31:0] obs_accesses_q, obs_new_pc_q, obs_repeat_pc_q;
  logic [31:0] obs_stride_match_q, obs_stride_mismatch_q, obs_stride_zero_q, obs_reuse_hit_q;
  logic [31:0] issued_q, useful_q, useless_q, late_q, cancelled_q;
  logic [31:0] admitted_q, gate_refuse_q, full_stall_q;
  logic [31:0] fill_ctr_q, fill_refused_q, dropped_q;

  // ==========================================================================
  // Demand side: index, tag, observed stride, predicted line
  // ==========================================================================
  logic [TIDX_W-1:0]   dem_idx_c;
  logic [PC_TAG_W-1:0] dem_tag_c;
  logic [31:0]         dem_line_c;
  logic [31:0]         rel_line_c;
  logic                dem_tbl_hit_c;
  logic [31:0]         tbl_stride_c;
  logic [31:0]         tbl_line_c;
  logic [1:0]          tbl_conf_c;
  logic [31:0]         obs_stride_c;
  logic [31:0]         cand_line_c;
  logic                train_c;
  logic                cand_ok_c;
  logic                line_out_c;
  logic                os_full_c;

  assign dem_idx_c  = dem_pc_i[TIDX_W+1:2];
  assign dem_tag_c  = dem_pc_i[31:TIDX_W+2];
  // The line number is a 32-bit window: ADDR_WIDTH here is the prefetcher's
  // own 32-bit PA window (the platform's RAM lives in it), and the pad makes
  // the slice up to it for any line size.
  assign dem_line_c = {{(32-ADDR_WIDTH+OFFSET_BITS){1'b0}}, dem_pa_i[ADDR_WIDTH-1:OFFSET_BITS]};
  assign rel_line_c = {{(32-ADDR_WIDTH+OFFSET_BITS){1'b0}}, pf_release_line_i[ADDR_WIDTH-1:OFFSET_BITS]};

  assign dem_tbl_hit_c = tbl_valid_q[dem_idx_c] && (tbl_tag_q[dem_idx_c] == dem_tag_c);
  assign tbl_stride_c  = tbl_stride_q[dem_idx_c];
  assign tbl_line_c    = tbl_line_q[dem_idx_c];
  assign tbl_conf_c    = tbl_conf_q[dem_idx_c];

  // The stride this demand reveals, in line units, as a wrapping subtraction
  // (two's complement). It is zero on the first repeat of a PC.
  assign obs_stride_c = dem_line_c - tbl_line_c;

  // The predicted line: the demand's own line plus the *stored* stride, so the
  // prediction is made before this access updates the entry.
  assign cand_line_c = dem_line_c + tbl_stride_c;

  // A faulting demand never trains and never spawns a hint (the mutant drops
  // this half of the permission check).
  assign train_c = dem_valid_i && (CHECK_READ_SAFE ? !dem_fault_i : 1'b1);

  always_comb begin : line_outstanding
    line_out_c = 1'b0;
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (os_valid_q[i] && (os_line_q[i] == cand_line_c)) line_out_c = 1'b1;
    end
  end

  assign os_full_c = &os_valid_q;

  assign cand_ok_c = en_i && train_c && dem_tbl_hit_c &&
                     (tbl_conf_c >= conf_thresh_i) &&
                     (tbl_stride_c != 32'd0) &&
                     (cand_line_c != dem_line_c);

  assign pf_valid_o = cand_ok_c && !line_out_c;
  assign pf_pa_o    = {{(64-ADDR_WIDTH){1'b0}}, cand_line_c[LINE_MSB_W-1:0],
                       {OFFSET_BITS{1'b0}}};
  assign pf_vpn_o   = dem_vpn_i;
  assign pf_asid_o  = dem_asid_i;
  assign pf_perms_o = dem_perms_i;

  // ==========================================================================
  // The gate: the prediction is honoured only if the same checks a demand gets
  // pass. The two groups are separate mutants so the two fail modes are
  // separable: bypassing the permission checks and performing a device read.
  // ==========================================================================
  logic read_safe_c;
  logic device_free_c;
  logic admit_c;

  assign read_safe_c  = CHECK_READ_SAFE ? (gate_mapped_i && gate_perm_ok_i) : 1'b1;
  assign device_free_c = CHECK_DEVICE_FREE ? !gate_device_i : 1'b1;

  assign admit_c = pf_valid_o && read_safe_c && device_free_c && !os_full_c;

  assign mem_req_valid_o = admit_c;
  assign mem_req_pa_o    = pf_pa_o;

  // Allocation: the lowest numbered free slot. A full table admits nothing, so
  // an allocation never targets a slot that is about to be freed this cycle.
  logic [IDX_W-1:0] alloc_idx_c;
  always_comb begin : alloc_select
    alloc_idx_c = {IDX_W{1'b0}};
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (!os_valid_q[i]) alloc_idx_c = i[IDX_W-1:0];
    end
  end

  assign mem_req_id_o = {{(8-IDX_W){1'b0}}, alloc_idx_c};

  // ==========================================================================
  // Response side: the fill, and the cancelled drop
  // ==========================================================================
  logic [IDX_W-1:0] resp_idx_c;
  logic             resp_valid_c;
  logic             resp_cancelled_c;

  assign resp_idx_c       = mem_resp_id_i[IDX_W-1:0];
  assign resp_valid_c     = mem_resp_valid_i && os_valid_q[resp_idx_c];
  assign resp_cancelled_c = os_cancelled_q[resp_idx_c];

  assign fill_valid_o = resp_valid_c && (!resp_cancelled_c || !DROP_CANCELLED);
  assign fill_pa_o    = os_pa_q[resp_idx_c];
  assign fill_vpn_o   = os_vpn_q[resp_idx_c];
  assign fill_asid_o  = os_asid_q[resp_idx_c];
  assign fill_perms_o = os_perms_q[resp_idx_c];

  always_comb begin : fill_data
    if (CORRUPT_FILL_DATA) begin
      fill_data_o = mem_resp_data_i ^ {{(LINE_BITS-1){1'b0}}, 1'b1};
    end else begin
      fill_data_o = mem_resp_data_i;
    end
  end

  // ==========================================================================
  // Lifecycle: per-slot terminal outcomes
  //   flush > explicit cancel > demand (useful/late) > release (useless),
  //   and the response is the lowest priority so a slot handled by an outcome
  //   this cycle is not also landed.
  // ==========================================================================
  logic [DEPTH-1:0] out_useful_c;
  logic [DEPTH-1:0] out_useless_c;
  logic [DEPTH-1:0] out_late_c;
  logic [DEPTH-1:0] out_cancel_c;
  logic [DEPTH-1:0] slot_taken_c;
  logic [DEPTH-1:0] out_clear_c;
  logic [DEPTH-1:0] out_mark_cancel_c;

  always_comb begin : outcome_decide
    out_useful_c      = {DEPTH{1'b0}};
    out_useless_c     = {DEPTH{1'b0}};
    out_late_c        = {DEPTH{1'b0}};
    out_cancel_c      = {DEPTH{1'b0}};
    slot_taken_c      = {DEPTH{1'b0}};
    out_clear_c       = {DEPTH{1'b0}};
    out_mark_cancel_c = {DEPTH{1'b0}};

    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (os_valid_q[i] && pf_flush_i) begin
        slot_taken_c[i] = 1'b1;
        out_cancel_c[i] = 1'b1;
        if (os_landed_q[i]) out_clear_c[i] = 1'b1;
        else                out_mark_cancel_c[i] = 1'b1;
      end else if (os_valid_q[i] && pf_cancel_i &&
                   (i[IDX_W-1:0] == pf_cancel_id_i[IDX_W-1:0])) begin
        slot_taken_c[i] = 1'b1;
        out_cancel_c[i] = 1'b1;
        if (os_landed_q[i]) out_clear_c[i] = 1'b1;
        else                out_mark_cancel_c[i] = 1'b1;
      end else if (train_c && os_valid_q[i] && (os_line_q[i] == dem_line_c)) begin
        slot_taken_c[i] = 1'b1;
        if (os_landed_q[i]) begin
          out_useful_c[i] = 1'b1;
          out_clear_c[i]  = 1'b1;
        end else begin
          out_late_c[i]            = 1'b1;
          out_mark_cancel_c[i]     = 1'b1;
        end
      end else if (pf_release_valid_i && os_valid_q[i] && (os_line_q[i] == rel_line_c)) begin
        slot_taken_c[i] = 1'b1;
        out_useless_c[i] = 1'b1;
        if (os_landed_q[i]) out_clear_c[i] = 1'b1;
        else                out_mark_cancel_c[i] = 1'b1;
      end
    end
  end

  // The response installs the fill unless an outcome already owns the slot or
  // the fill was cancelled and cancels are honoured. When the cancel control is
  // off (the mutant) a cancelled response is presented and force-installed.
  logic resp_present_c;
  logic resp_useless_c;
  logic resp_drop_c;

  assign resp_present_c = resp_valid_c && !slot_taken_c[resp_idx_c] && fill_valid_o;
  assign resp_useless_c = resp_present_c && !fill_ok_i;
  assign resp_drop_c    = resp_valid_c && !slot_taken_c[resp_idx_c] &&
                          resp_cancelled_c && DROP_CANCELLED;

  // ------------------------------------------------------------ increments
  logic [31:0] inc_useful_c, inc_useless_c, inc_late_c, inc_cancel_c;
  logic [31:0] inc_fill_c, inc_fill_refused_c, inc_dropped_c;
  logic [31:0] n_useful_c, n_useless_c, n_late_c, n_cancel_c;

  always_comb begin : count_increments
    n_useful_c  = 32'd0;
    n_useless_c = 32'd0;
    n_late_c    = 32'd0;
    n_cancel_c  = 32'd0;
    for (int unsigned i = 0; i < DEPTH; i++) begin
      n_useful_c  = n_useful_c  + {31'b0, out_useful_c[i]};
      n_useless_c = n_useless_c + {31'b0, out_useless_c[i]};
      n_late_c    = n_late_c    + {31'b0, out_late_c[i]};
      n_cancel_c  = n_cancel_c  + {31'b0, out_cancel_c[i]};
    end
    inc_useful_c      = n_useful_c;
    inc_useless_c     = n_useless_c + (resp_useless_c ? 32'd1 : 32'd0);
    inc_late_c        = n_late_c;
    inc_cancel_c      = n_cancel_c;
    inc_fill_c        = (resp_present_c && fill_ok_i) ? 32'd1 : 32'd0;
    inc_fill_refused_c = (fill_valid_o && !fill_ok_i) ? 32'd1 : 32'd0;
    inc_dropped_c     = resp_drop_c ? 32'd1 : 32'd0;
  end

  // ==========================================================================
  // The edge
  // ==========================================================================
  always_ff @(posedge clk) begin
    if (rst) begin
      tbl_valid_q <= {TABLE_ENTRIES{1'b0}};
      for (int unsigned i = 0; i < TABLE_ENTRIES; i++) begin
        tbl_tag_q[i]    <= {PC_TAG_W{1'b0}};
        tbl_stride_q[i] <= 32'd0;
        tbl_line_q[i]   <= 32'd0;
        tbl_conf_q[i]   <= 2'd0;
      end
      os_valid_q     <= {DEPTH{1'b0}};
      os_landed_q    <= {DEPTH{1'b0}};
      os_cancelled_q <= {DEPTH{1'b0}};
      for (int unsigned i = 0; i < DEPTH; i++) begin
        os_line_q[i]  <= 32'd0;
        os_pa_q[i]    <= 64'd0;
        os_vpn_q[i]   <= 27'd0;
        os_asid_q[i]  <= 16'd0;
        os_perms_q[i] <= 4'd0;
      end
      obs_accesses_q        <= 32'd0;
      obs_new_pc_q          <= 32'd0;
      obs_repeat_pc_q       <= 32'd0;
      obs_stride_match_q    <= 32'd0;
      obs_stride_mismatch_q <= 32'd0;
      obs_stride_zero_q     <= 32'd0;
      obs_reuse_hit_q       <= 32'd0;
      issued_q        <= 32'd0;
      useful_q        <= 32'd0;
      useless_q       <= 32'd0;
      late_q          <= 32'd0;
      cancelled_q     <= 32'd0;
      admitted_q      <= 32'd0;
      gate_refuse_q   <= 32'd0;
      full_stall_q    <= 32'd0;
      fill_ctr_q      <= 32'd0;
      fill_refused_q  <= 32'd0;
      dropped_q       <= 32'd0;
    end else begin
      // -------------------------------------------------------- observation
      if (train_c) begin
        obs_accesses_q <= obs_accesses_q + 32'd1;
        if (dem_tbl_hit_c) begin
          obs_repeat_pc_q <= obs_repeat_pc_q + 32'd1;
          if (obs_stride_c == 32'd0) begin
            obs_stride_zero_q <= obs_stride_zero_q + 32'd1;
          end else if (obs_stride_c == tbl_stride_c) begin
            obs_stride_match_q <= obs_stride_match_q + 32'd1;
          end else begin
            obs_stride_mismatch_q <= obs_stride_mismatch_q + 32'd1;
          end
        end else begin
          obs_new_pc_q <= obs_new_pc_q + 32'd1;
        end
        if (dem_hit_i) obs_reuse_hit_q <= obs_reuse_hit_q + 32'd1;
      end

      // ------------------------------------------------------ table update
      if (train_c) begin
        if (dem_tbl_hit_c) begin
          if ((obs_stride_c == tbl_stride_c) && (obs_stride_c != 32'd0)) begin
            if (tbl_conf_c != 2'd3) tbl_conf_q[dem_idx_c] <= tbl_conf_c + 2'd1;
          end else begin
            tbl_stride_q[dem_idx_c] <= obs_stride_c;
            tbl_conf_q[dem_idx_c]   <= (obs_stride_c == 32'd0) ? 2'd0 : 2'd1;
          end
          tbl_line_q[dem_idx_c] <= dem_line_c;
        end else begin
          tbl_valid_q[dem_idx_c]  <= 1'b1;
          tbl_tag_q[dem_idx_c]    <= dem_tag_c;
          tbl_stride_q[dem_idx_c] <= 32'd0;
          tbl_line_q[dem_idx_c]   <= dem_line_c;
          tbl_conf_q[dem_idx_c]   <= 2'd0;
        end
      end

      // ---------------------------------------------------------- allocation
      if (admit_c && mem_req_ready_i) begin
        os_valid_q[alloc_idx_c]     <= 1'b1;
        os_landed_q[alloc_idx_c]    <= 1'b0;
        os_cancelled_q[alloc_idx_c] <= 1'b0;
        os_line_q[alloc_idx_c]      <= cand_line_c;
        os_pa_q[alloc_idx_c]        <= pf_pa_o;
        os_vpn_q[alloc_idx_c]       <= dem_vpn_i;
        os_asid_q[alloc_idx_c]      <= dem_asid_i;
        os_perms_q[alloc_idx_c]     <= dem_perms_i;
      end

      // ----------------------------------------------------------- outcomes
      for (int unsigned i = 0; i < DEPTH; i++) begin
        if (out_clear_c[i])       os_valid_q[i]     <= 1'b0;
        if (out_mark_cancel_c[i]) os_cancelled_q[i] <= 1'b1;
      end

      // ----------------------------------------------------------- response
      if (resp_valid_c && !slot_taken_c[resp_idx_c]) begin
        if (resp_drop_c || resp_useless_c) begin
          os_valid_q[resp_idx_c]  <= 1'b0;
          os_landed_q[resp_idx_c] <= 1'b0;
        end else begin
          os_landed_q[resp_idx_c] <= 1'b1;
        end
      end

      // ----------------------------------------------------------- counters
      if (admit_c)                    admitted_q    <= admitted_q + 32'd1;
      if (admit_c && mem_req_ready_i) issued_q      <= issued_q + 32'd1;
      if (pf_valid_o && !(read_safe_c && device_free_c)) gate_refuse_q <= gate_refuse_q + 32'd1;
      if (pf_valid_o && read_safe_c && device_free_c && os_full_c) full_stall_q <= full_stall_q + 32'd1;

      useful_q       <= useful_q       + inc_useful_c;
      useless_q      <= useless_q      + inc_useless_c;
      late_q         <= late_q         + inc_late_c;
      cancelled_q    <= cancelled_q    + inc_cancel_c;
      fill_ctr_q     <= fill_ctr_q     + inc_fill_c;
      fill_refused_q <= fill_refused_q + inc_fill_refused_c;
      dropped_q      <= dropped_q      + inc_dropped_c;
    end
  end

  // -------------------------------------------------------- observability
  logic [31:0] inflight_c;
  always_comb begin : inflight_count
    inflight_c = 32'd0;
    for (int unsigned i = 0; i < DEPTH; i++) begin
      if (os_valid_q[i] && !os_cancelled_q[i]) inflight_c = inflight_c + 32'd1;
    end
  end

  assign o_obs_accesses_o        = obs_accesses_q;
  assign o_obs_new_pc_o          = obs_new_pc_q;
  assign o_obs_repeat_pc_o       = obs_repeat_pc_q;
  assign o_obs_stride_match_o    = obs_stride_match_q;
  assign o_obs_stride_mismatch_o = obs_stride_mismatch_q;
  assign o_obs_stride_zero_o     = obs_stride_zero_q;
  assign o_obs_reuse_hit_o       = obs_reuse_hit_q;

  assign o_issued_o           = issued_q;
  assign o_useful_o           = useful_q;
  assign o_useless_o          = useless_q;
  assign o_late_o             = late_q;
  assign o_cancelled_o        = cancelled_q;
  assign o_admitted_o         = admitted_q;
  assign o_gate_refuse_o      = gate_refuse_q;
  assign o_full_stall_o       = full_stall_q;
  assign o_fill_ctr_o         = fill_ctr_q;
  assign o_fill_refused_ctr_o = fill_refused_q;
  assign o_dropped_ctr_o      = dropped_q;
  assign o_inflight_o         = inflight_c;

  assign o_table_entries_o = 32'(TABLE_ENTRIES);
  assign o_depth_o         = 32'(DEPTH);

endmodule

`endif  // MOSAIC_PREFETCH_SV_
