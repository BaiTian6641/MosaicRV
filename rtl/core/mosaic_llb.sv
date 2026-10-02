// ============================================================================
// mosaic_llb -- work package I-060: the strictly restricted clean-copy
// locality buffer (LLB).
//
// The LLB holds *clean copies* of cache lines so that a reuse load can be
// answered without touching the L1. "Clean" means only that this structure
// never owns a dirty byte: it must never write back and never issues a memory
// request of its own. It does **not** mean the copy is fresh. A clean copy is
// stale the moment any writer touches its line, so this module participates in
// the machine's coherence obligation rather than hiding behind a
// "non-architectural" label: it is legal only because its invalidation sources
// are enumerable, and every one of them is a port below and a rule in
// `results/reports/I-060-llb.md`.
//
// ------------------------------------------------------------------- keying
//
// A lookup matches an entry only when *all* of these agree:
//
//   * the physical line identity, `pa[ADDR_WIDTH-1:OFFSET_BITS]` (a 32-byte
//     line for the default geometry). A copy of a physical line is a copy of
//     that physical line: two virtual aliases that translate to the same PA
//     must share it, so the virtual address is deliberately **not** part of
//     the key. (The request's VPN is carried on `req_vpn_i` and is used by the
//     scoped SFENCE.VMA match below; the `VA_KEYED` control is the one build
//     in which it enters the key, and the case shows why that is wrong.)
//   * the permission context: the address-space identifier `asid` and the
//     leaf's permission class `perms`. An entry filled under one permission
//     context is not reused under another. Over-missing is always legal, and
//     this is the conservative direction: a copy taken under a read-only
//     mapping cannot be handed to a write-permitted access.
//
// ------------------------------------------------------------------- bypass
//
// Two access classes never consult or fill the LLB, whatever an entry says:
//
//   * **non-cacheable / device regions.** The decision is the generated
//     platform map's, asked directly (`mosaic_cfg_pkg::mosaic_pa_cacheable`),
//     never a hand-written list, so the LLB and the SoC device decode cannot
//     disagree about where a device lives. A device fill is refused, not just
//     bypassed, so a device line can never be resident for a later hit.
//   * **atomics** (`req_atomic_i`): an AMO is a read-modify-write at a
//     serialization point; serving either half from a private copy would be
//     the same class of defect as caching MMIO. The atomic still pulses
//     `inv_store_i`, so the line it wrote is not left resident.
//
// ------------------------------------------------------------- invalidation
//
// The enumerable sources, and what each invalidates:
//
//   * `inv_store_valid_i`  -- this hart's own store (or AMO, or store-queue
//     commit) to a physical line. Matches the **physical line only**: a store
//     observed under one permission context must remove the copies taken under
//     every other context, because they are copies of the same physical line.
//   * `inv_refill_valid_i` -- an L1 refill/replacement of a line. The L1 is the
//     authority for the data; once it refetches the line, a private copy taken
//     before the refill may be older than what the refill brought in. Matches
//     the physical line only.
//   * `inv_snoop_valid_i`  -- an external writer (another hart, DMA, device,
//     debugger) to a line, or a broadcast (`inv_snoop_all_i`, the single-hart
//     stand-in for a shootdown). Matches the physical line only.
//   * `fence_valid_i`      -- FENCE (memory) and FENCE.I invalidate every
//     entry: a structure that is not itself ordered must not be the way a
//     fence is bypassed. SFENCE.VMA invalidates the context it names (ASID
//     and/or page); it is *not* treated as a memory-data fence -- it only
//     removes translation-backed copies, which is what it "claims".
//   * `ctx_valid_i`        -- a `satp` write or an ASID switch. A clean copy is
//     reachable only through the translation that produced it, so a context
//     change makes every entry unreachable: the whole buffer is flushed. This
//     is deliberately stronger than keying alone, because an ASID number may be
//     reused for a *different* address space.
//
// ------------------------------------------------------ ordering vs a hit
//
// The rule is strict and one-directional: **a cycle that invalidates an entry
// grants no hit on it.** The refusal is combinational and has priority over the
// tag match, so a hit can never be used after the invalidating event is
// visible. The entry is cleared on the same rising edge. There is no
// interleaving in which a stale value escapes: either the hit happened in an
// earlier cycle (the store was not yet visible then) or it is refused here and
// the consumer must refetch. `o_race_refuse_ctr` counts the refusals and the
// case exercises the same-cycle conflict directly.
//
//   MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE   a store does not invalidate the line
//                                           it wrote (the card's central failure)
//   MOSAIC_LLB_MUTANT_MMIO_HIT              the platform map is ignored, so a
//                                           device line is filled and hit
//   MOSAIC_LLB_MUTANT_RACE_STALE_HIT        the combinational refusal is
//                                           removed: a hit is granted in the
//                                           same cycle as the invalidating store
//   MOSAIC_LLB_MUTANT_ASID_SWITCH_NO_INVALIDATE
//                                           a context change leaves entries
//                                           reachable
//   MOSAIC_LLB_MUTANT_VA_KEYED              the key includes the virtual page
//                                           number, so a physical alias misses
// ============================================================================

`ifndef MOSAIC_LLB_SV_
`define MOSAIC_LLB_SV_

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

`include "mosaic_pkg.sv"

module mosaic_llb #(
  parameter int unsigned LINE_BYTES = 32,   // bytes per line, power of two
  parameter int unsigned ENTRIES    = 8,    // fully associative lines
  parameter int unsigned ADDR_WIDTH = 32,   // physical address width
  localparam int unsigned OFFSET_BITS = $clog2(LINE_BYTES),
  localparam int unsigned LINE_BITS   = LINE_BYTES * 8,
  localparam int unsigned PA_LINE_W   = ADDR_WIDTH - OFFSET_BITS,
  localparam int unsigned IDX_W       = $clog2(ENTRIES),
  localparam logic [7:0]  ENTRIES_B    = 8'(ENTRIES),
  localparam logic [7:0]  LINE_BYTES_B = 8'(LINE_BYTES)
) (
  input  logic                     clk,
  input  logic                     rst,

  // ------------------------------------------------------------- lookup
  // A lookup is combinational and answers in the cycle it is presented. It is
  // the consumer's job to have already checked permissions; the LLB re-checks
  // the context in the key so it cannot widen them.
  input  logic                     req_valid_i,
  input  logic [63:0]              req_pa_i,       // full PA, for the platform map
  input  logic [26:0]              req_vpn_i,      // VA page: scoped-fence match
  input  logic [15:0]              req_asid_i,
  input  logic [3:0]               req_perms_i,    // leaf permission class {x,w,r,u}
  input  logic                     req_atomic_i,   // 1 = AMO/LR/SC: bypass
  output logic                     req_hit_o,
  output logic [LINE_BITS-1:0]     req_data_o,
  output logic                     req_bypass_o,   // non-cacheable or atomic

  // --------------------------------------------------------------- fill
  // A fill installs a clean copy. Refused for a non-cacheable/device line and
  // for a line being invalidated in the same cycle.
  input  logic                     fill_valid_i,
  input  logic [63:0]              fill_pa_i,
  input  logic [26:0]              fill_vpn_i,
  input  logic [15:0]              fill_asid_i,
  input  logic [3:0]               fill_perms_i,
  input  logic [LINE_BITS-1:0]     fill_data_i,
  output logic                     fill_ok_o,

  // ---------------------------------------------- invalidation: data lines
  // A data-line invalidation is line-granular: the sub-line offset bits of the
  // address carry no meaning, and the identity is `pa[ADDR_WIDTH-1:OFFSET_BITS]`.
  // The unused bits are declared away here, once, for all three ports.
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic                     inv_store_valid_i,
  input  logic [63:0]              inv_store_pa_i,
  input  logic                     inv_refill_valid_i,
  input  logic [63:0]              inv_refill_pa_i,
  input  logic                     inv_snoop_valid_i,
  input  logic [63:0]              inv_snoop_pa_i,
  /* verilator lint_on UNUSEDSIGNAL */
  input  logic                     inv_snoop_all_i,

  // --------------------------------------------- invalidation: fences
  // 00 = FENCE (memory), 01 = FENCE.I, 10 = SFENCE.VMA. `has_*` distinguish
  // "operand is x0" from "operand value is zero".
  input  logic                     fence_valid_i,
  input  logic [1:0]               fence_kind_i,
  input  logic [26:0]              fence_vpn_i,
  input  logic                     fence_has_vpn_i,
  input  logic [15:0]              fence_asid_i,
  input  logic                     fence_has_asid_i,

  // -------------------------------------- invalidation: context change
  // A `satp` write or an ASID switch. One pulse; flushes the whole buffer.
  input  logic                     ctx_valid_i,

  // -------------------------------------------------------- observability
  input  logic [IDX_W-1:0]         dbg_index_i,
  output logic                     dbg_valid_o,
  output logic [PA_LINE_W-1:0]     dbg_line_o,
  output logic [15:0]              dbg_asid_o,
  output logic [3:0]               dbg_perms_o,
  output logic [26:0]              dbg_vpn_o,
  output logic [LINE_BITS-1:0]     dbg_data_o,

  output logic [3:0]               o_count_o,       // valid entries
  output logic [7:0]               o_entries_o,     // geometry: ENTRIES
  output logic [7:0]               o_line_bytes_o,  // geometry: LINE_BYTES
  output logic [31:0]              o_hit_ctr,
  output logic [31:0]              o_miss_ctr,
  output logic [31:0]              o_bypass_ctr,
  output logic [31:0]              o_fill_ctr,
  output logic [31:0]              o_fill_refused_ctr,
  output logic [31:0]              o_inv_ctr,
  output logic [31:0]              o_race_refuse_ctr,

  // -------------------------------------------------- replacement observation
  // I-060 integration: the line a fill displaced this cycle, one cycle wide.
  // The prefetcher is told when a line it landed leaves the buffer for any
  // reason, and a displaced line is one of them: a landed prefetch whose copy
  // is gone (or whose in-flight read is now pointless) must not later be
  // reported as having helped.
  output logic                     o_evict_valid_o,
  output logic [PA_LINE_W-1:0]     o_evict_line_o
);

  localparam logic [1:0] FENCE_SFENCE = 2'd2;

`ifdef MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE
  // NEGATIVE CONTROL: a store leaves the line it wrote resident, so a later
  // load is answered with the pre-store copy -- the card's central failure.
  localparam logic STORE_INVALIDATES = 1'b0;
`else
  localparam logic STORE_INVALIDATES = 1'b1;
`endif

`ifdef MOSAIC_LLB_MUTANT_MMIO_HIT
  // NEGATIVE CONTROL: the platform map is not consulted, so a device line is
  // filled and can be hit.
  localparam logic USE_PLATFORM_MAP = 1'b0;
`else
  localparam logic USE_PLATFORM_MAP = 1'b1;
`endif

`ifdef MOSAIC_LLB_MUTANT_VA_KEYED
  // NEGATIVE CONTROL: the virtual page number joins the key, so the same
  // physical line reached through two aliases gets two entries and a hit
  // through the second alias misses.
  localparam logic KEY_ON_VPN = 1'b1;
`else
  localparam logic KEY_ON_VPN = 1'b0;
`endif

`ifdef MOSAIC_LLB_MUTANT_RACE_STALE_HIT
  // NEGATIVE CONTROL: the combinational refusal is removed, so a hit is
  // granted in the same cycle as the invalidating store and returns the
  // pre-store value after the store is visible.
  localparam logic REFUSE_CONFLICT = 1'b0;
`else
  localparam logic REFUSE_CONFLICT = 1'b1;
`endif

`ifdef MOSAIC_LLB_MUTANT_ASID_SWITCH_NO_INVALIDATE
  // NEGATIVE CONTROL: a context change leaves entries reachable.
  localparam logic CTX_INVALIDATES = 1'b0;
`else
  localparam logic CTX_INVALIDATES = 1'b1;
`endif

  // ------------------------------------------------------------ the entries
  // Parallel arrays, so no assignment pattern is needed to reset or install.
  logic [ENTRIES-1:0]   ent_valid_q;
  logic [PA_LINE_W-1:0] ent_line_q  [0:ENTRIES-1];
  logic [26:0]          ent_vpn_q   [0:ENTRIES-1];
  logic [15:0]          ent_asid_q  [0:ENTRIES-1];
  logic [3:0]           ent_perms_q [0:ENTRIES-1];
  logic [LINE_BITS-1:0] ent_data_q  [0:ENTRIES-1];

  logic [IDX_W-1:0] rr_q;

  // ---------------------------------------------------------------- counters
  logic [31:0] hit_ctr_q, miss_ctr_q, bypass_ctr_q, fill_ctr_q;
  logic [31:0] fill_refused_ctr_q, inv_ctr_q, race_refuse_ctr_q;

  // --------------------------------------------------------- platform map
  logic req_cacheable_c;
  logic fill_cacheable_c;

  always_comb begin : platform_map
    if (USE_PLATFORM_MAP) begin
      req_cacheable_c  = mosaic_cfg_pkg::mosaic_pa_cacheable(req_pa_i);
      fill_cacheable_c = mosaic_cfg_pkg::mosaic_pa_cacheable(fill_pa_i);
    end else begin
      req_cacheable_c  = 1'b1;
      fill_cacheable_c = 1'b1;
    end
  end

  logic req_bypass_c;
  assign req_bypass_c = req_atomic_i || !req_cacheable_c;
  assign req_bypass_o = req_valid_i && req_bypass_c;

  // ------------------------------------------------------ line identities
  logic [PA_LINE_W-1:0] req_line_c;
  logic [PA_LINE_W-1:0] fill_line_c;
  logic [PA_LINE_W-1:0] store_line_c;
  logic [PA_LINE_W-1:0] refill_line_c;
  logic [PA_LINE_W-1:0] snoop_line_c;

  assign req_line_c    = req_pa_i[ADDR_WIDTH-1:OFFSET_BITS];
  assign fill_line_c   = fill_pa_i[ADDR_WIDTH-1:OFFSET_BITS];
  assign store_line_c  = inv_store_pa_i[ADDR_WIDTH-1:OFFSET_BITS];
  assign refill_line_c = inv_refill_pa_i[ADDR_WIDTH-1:OFFSET_BITS];
  assign snoop_line_c  = inv_snoop_pa_i[ADDR_WIDTH-1:OFFSET_BITS];

  // ------------------------------------------------------------ keyed match
  logic [ENTRIES-1:0] key_match_c;   // the request's key, used for the hit
  logic [ENTRIES-1:0] fill_key_c;    // the fill's own key, used for replacement

  always_comb begin : key_match
    key_match_c = {ENTRIES{1'b0}};
    fill_key_c  = {ENTRIES{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      key_match_c[i] = ent_valid_q[i] &&
                       (ent_line_q[i] == req_line_c) &&
                       (ent_asid_q[i] == req_asid_i) &&
                       (ent_perms_q[i] == req_perms_i);
      if (KEY_ON_VPN) begin
        key_match_c[i] = key_match_c[i] && (ent_vpn_q[i] == req_vpn_i);
      end
      fill_key_c[i] = ent_valid_q[i] &&
                      (ent_line_q[i] == fill_line_c) &&
                      (ent_asid_q[i] == fill_asid_i) &&
                      (ent_perms_q[i] == fill_perms_i);
    end
  end

  // The newest match wins; with distinct keys at most one can match.
  logic                 hit_match_c;
  logic [LINE_BITS-1:0] hit_data_c;

  always_comb begin : hit_select
    hit_match_c = 1'b0;
    hit_data_c  = {LINE_BITS{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (key_match_c[i]) begin
        hit_match_c = 1'b1;
        hit_data_c  = ent_data_q[i];
      end
    end
  end

  // ==========================================================================
  // Invalidation
  // ==========================================================================
  logic               kill_all_c;
  logic [ENTRIES-1:0] kill_one_c;
  logic               fence_scoped_c;

  always_comb begin : kill_decide
    kill_all_c    = 1'b0;
    kill_one_c    = {ENTRIES{1'b0}};
    fence_scoped_c = 1'b0;

    // A context change removes every translation-backed copy.
    if (ctx_valid_i && CTX_INVALIDATES) kill_all_c = 1'b1;

    // FENCE and FENCE.I remove everything; SFENCE.VMA removes the context it
    // names. SFENCE.VMA is not a memory-data fence and is never relied on for
    // one.
    if (fence_valid_i) begin
      if (fence_kind_i == FENCE_SFENCE) begin
        fence_scoped_c = 1'b1;
        for (int unsigned i = 0; i < ENTRIES; i++) begin
          if (ent_valid_q[i] &&
              (!fence_has_asid_i || (ent_asid_q[i] == fence_asid_i)) &&
              (!fence_has_vpn_i  || (ent_vpn_q[i]  == fence_vpn_i))) begin
            kill_one_c[i] = 1'b1;
          end
        end
      end else begin
        // FENCE (memory, kind 0) and FENCE.I (kind 1): remove everything.
        kill_all_c = 1'b1;
      end
    end

    // A broadcast snoop (shootdown / debugger / DMA flush) removes everything.
    if (inv_snoop_valid_i && inv_snoop_all_i) kill_all_c = 1'b1;

    // Data-line sources match the *physical line*, never the context: the copy
    // under every permission context is a copy of the same physical line.
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (ent_valid_q[i]) begin
        if (inv_store_valid_i && STORE_INVALIDATES && (ent_line_q[i] == store_line_c))
          kill_one_c[i] = 1'b1;
        if (inv_refill_valid_i && (ent_line_q[i] == refill_line_c))
          kill_one_c[i] = 1'b1;
        if (inv_snoop_valid_i && !inv_snoop_all_i && (ent_line_q[i] == snoop_line_c))
          kill_one_c[i] = 1'b1;
      end
    end
  end

  // Does this cycle remove the entry a presented lookup would use?
  logic conflict_c;
  logic scoped_req_kill_c;

  always_comb begin : hit_conflict
    conflict_c = 1'b0;
    if (kill_all_c) conflict_c = 1'b1;
    if (inv_store_valid_i && STORE_INVALIDATES && (store_line_c == req_line_c))
      conflict_c = 1'b1;
    if (inv_refill_valid_i && (refill_line_c == req_line_c))
      conflict_c = 1'b1;
    if (inv_snoop_valid_i && (inv_snoop_all_i || (snoop_line_c == req_line_c)))
      conflict_c = 1'b1;

    scoped_req_kill_c = fence_scoped_c &&
                        (!fence_has_asid_i || (req_asid_i == fence_asid_i)) &&
                        (!fence_has_vpn_i  || (req_vpn_i  == fence_vpn_i));
    if (scoped_req_kill_c) conflict_c = 1'b1;
  end

  // The ordering rule, made explicit: an invalidating cycle grants no hit.
  logic refused_c;
  assign refused_c = conflict_c && REFUSE_CONFLICT;

  assign req_hit_o  = req_valid_i && !req_bypass_c && !refused_c && hit_match_c;
  assign req_data_o = hit_match_c ? hit_data_c : {LINE_BITS{1'b0}};

  // ==========================================================================
  // Fill
  // ==========================================================================
  logic fill_conflict_c;
  logic fill_ok_c;

  always_comb begin : fill_decide
    fill_conflict_c = kill_all_c;
    if (inv_store_valid_i && STORE_INVALIDATES && (store_line_c == fill_line_c))
      fill_conflict_c = 1'b1;
    if (inv_refill_valid_i && (refill_line_c == fill_line_c))
      fill_conflict_c = 1'b1;
    if (inv_snoop_valid_i && (inv_snoop_all_i || (snoop_line_c == fill_line_c)))
      fill_conflict_c = 1'b1;
    if (fence_scoped_c &&
        (!fence_has_asid_i || (fill_asid_i == fence_asid_i)) &&
        (!fence_has_vpn_i  || (fill_vpn_i  == fence_vpn_i)))
      fill_conflict_c = 1'b1;
    fill_ok_c = fill_valid_i && fill_cacheable_c && !fill_conflict_c;
  end

  assign fill_ok_o = fill_ok_c;

  // Replacement: an invalid entry, then the matching entry, then round-robin.
  logic [IDX_W-1:0] fill_idx_c;
  logic             fill_have_free_c;

  always_comb begin : fill_select
    fill_have_free_c = 1'b0;
    fill_idx_c       = rr_q;
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (!ent_valid_q[i] && !fill_have_free_c) begin
        fill_have_free_c = 1'b1;
        fill_idx_c       = i[IDX_W-1:0];
      end
    end
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (fill_key_c[i]) fill_idx_c = i[IDX_W-1:0];
    end
  end

  // A fill that displaces a valid entry with a different key is a replacement:
  // the displaced line leaves the buffer. (A fill of the same key replaces the
  // same line and is not a departure.) The observation is one cycle wide and
  // names the line that left, so a consumer tracking a line can drop it.
  logic fill_displaces_c;
  assign fill_displaces_c = fill_ok_c && !fill_have_free_c && !fill_key_c[fill_idx_c];
  assign o_evict_valid_o  = fill_displaces_c;
  assign o_evict_line_o   = ent_line_q[fill_idx_c];

  // ==========================================================================
  // The edge
  // ==========================================================================
  always_ff @(posedge clk) begin
    if (rst) begin
      ent_valid_q      <= {ENTRIES{1'b0}};
      rr_q             <= {IDX_W{1'b0}};
      hit_ctr_q        <= 32'd0;
      miss_ctr_q       <= 32'd0;
      bypass_ctr_q     <= 32'd0;
      fill_ctr_q       <= 32'd0;
      fill_refused_ctr_q <= 32'd0;
      inv_ctr_q        <= 32'd0;
      race_refuse_ctr_q <= 32'd0;
      for (int unsigned i = 0; i < ENTRIES; i++) begin
        ent_line_q[i]  <= {PA_LINE_W{1'b0}};
        ent_vpn_q[i]   <= 27'd0;
        ent_asid_q[i]  <= 16'd0;
        ent_perms_q[i] <= 4'd0;
        ent_data_q[i]  <= {LINE_BITS{1'b0}};
      end
    end else begin
      if (kill_all_c) begin
        ent_valid_q <= {ENTRIES{1'b0}};
      end else begin
        for (int unsigned i = 0; i < ENTRIES; i++) begin
          if (kill_one_c[i]) ent_valid_q[i] <= 1'b0;
        end
      end

      if (fill_ok_c) begin
        ent_valid_q[fill_idx_c] <= 1'b1;
        ent_line_q[fill_idx_c]  <= fill_line_c;
        ent_vpn_q[fill_idx_c]   <= fill_vpn_i;
        ent_asid_q[fill_idx_c]  <= fill_asid_i;
        ent_perms_q[fill_idx_c] <= fill_perms_i;
        ent_data_q[fill_idx_c]  <= fill_data_i;
        if (!fill_have_free_c && !fill_key_c[fill_idx_c]) begin
          rr_q <= rr_q + 1'b1;
        end
      end

      if (req_valid_i) begin
        if (req_bypass_c) begin
          bypass_ctr_q <= bypass_ctr_q + 32'd1;
        end else if (req_hit_o) begin
          hit_ctr_q <= hit_ctr_q + 32'd1;
        end else begin
          miss_ctr_q <= miss_ctr_q + 32'd1;
        end
      end
      if (req_valid_i && !req_bypass_c && hit_match_c && conflict_c && REFUSE_CONFLICT)
        race_refuse_ctr_q <= race_refuse_ctr_q + 32'd1;

      if (fill_valid_i && !fill_ok_c) fill_refused_ctr_q <= fill_refused_ctr_q + 32'd1;
      if (fill_ok_c) fill_ctr_q <= fill_ctr_q + 32'd1;

      if (kill_all_c || (|kill_one_c)) inv_ctr_q <= inv_ctr_q + 32'd1;
    end
  end

  // -------------------------------------------------------- observability
  logic [3:0] count_c;
  always_comb begin : popcount
    count_c = 4'd0;
    for (int unsigned i = 0; i < ENTRIES; i++) count_c = count_c + {3'b000, ent_valid_q[i]};
  end

  assign dbg_valid_o    = ent_valid_q[dbg_index_i];
  assign dbg_line_o     = ent_line_q[dbg_index_i];
  assign dbg_asid_o     = ent_asid_q[dbg_index_i];
  assign dbg_perms_o    = ent_perms_q[dbg_index_i];
  assign dbg_vpn_o      = ent_vpn_q[dbg_index_i];
  assign dbg_data_o     = ent_data_q[dbg_index_i];
  assign o_count_o      = count_c;
  assign o_entries_o    = ENTRIES_B;
  assign o_line_bytes_o = LINE_BYTES_B;
  assign o_hit_ctr          = hit_ctr_q;
  assign o_miss_ctr         = miss_ctr_q;
  assign o_bypass_ctr       = bypass_ctr_q;
  assign o_fill_ctr         = fill_ctr_q;
  assign o_fill_refused_ctr = fill_refused_ctr_q;
  assign o_inv_ctr          = inv_ctr_q;
  assign o_race_refuse_ctr  = race_refuse_ctr_q;

endmodule

`endif  // MOSAIC_LLB_SV_
