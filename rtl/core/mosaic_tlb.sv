// ============================================================================
// mosaic_tlb -- work package I-046: the address-translation cache and
// SFENCE.VMA invalidation.
//
// This module is a *cache in front of* the I-045 walker (`mosaic_ptw`, which it
// instantiates): it presents the walker's own request/response interface and
// its own physical PTE port, so the core that used to drive `mosaic_ptw`
// directly now drives this module and needs no second translation path. On a
// hit it answers the translation itself and **no PTE is read**; on a miss it
// forwards the request to the walker, waits for the walk, installs the result
// under a generation tag, and returns it. That "no PTE read on a hit" is the
// only honest proof that a cache is being used, and it is what the case
// `tlb.sfence_vma` observes on the memory port.
//
// ---------------------------------------------------------------- what is cached
//
// A 2-way-set-associative entry holds exactly:
//
//   * `vpn`  = va[38:12], the 4 KiB-aligned virtual page. The cache is 4 KiB
//     granular. A superpage walk produces a per-4 KiB physical address, and
//     that address is what is cached; a second 4 KiB chunk of the same
//     superpage therefore walks once of its own. That is a deliberate choice:
//     it keeps one page size in the tag, and the report states it.
//   * `asid` = satp.ASID[15:0], compared exactly. Plus `g`: a *global* leaf
//     (PTE.G=1) matches any ASID, which is what makes it survive an ASID
//     change and a per-ASID fence.
//   * `ppn`  = pa[55:12], the translated page number.
//   * `perms`= the leaf's {x, w, r, u}. The translation alone is not enough:
//     a hit must still decide *this* access's permission, and it does so with
//     the shared `mosaic_pkg::leaf_perm_ok`, so a cached permission result
//     cannot drift from a walked one.
//   * `a`, `d` = the leaf's A and D bits as the walk left them. They are not
//     decoration: a *store* must not be served from an entry whose cached D is
//     clear, because the hardware-update scheme this profile uses (I-045's
//     compare-and-set, kept here unchanged) sets D in the page table at walk
//     time. A store that hit such an entry would leave the PTE's D bit unset
//     and the page would never be marked dirty -- so such a store misses,
//     walks, and the walker's update is what the re-installed entry carries.
//
// ---------------------------------------------------------------- invalidation
//
// Two sources, and neither is "a plain memory fence":
//
//   1. `sfence_valid_i` -- one pulse per executed SFENCE.VMA, with the rs1
//      address (`sfence_va_i`/`sfence_has_va_i`) and the rs2 ASID
//      (`sfence_asid_i`/`sfence_has_asid_i`). The four forms and what each
//      invalidates here are the specification's own (priv v1.12 §4.2.1):
//
//        rs1=x0, rs2=x0   all address spaces, all addresses, globals included
//        rs1=x0, rs2!=x0  the address space of rs2, globals *excluded*
//        rs1!=x0, rs2=x0  the VA's page, all address spaces, globals included
//        rs1!=x0, rs2!=x0 the VA's page in rs2's address space, globals excluded
//
//      "globals excluded" is exact, not conservative: the spec says an
//      SFENCE.VMA with rs2!=x0 need not flush global mappings, and this
//      implementation does not. Over-fencing is always legal, so the report
//      states this as *claimed exact* on the ASID dimension.
//
//      A non-canonical `rs1` (rs1!=x0) makes the instruction have no effect,
//      and this implementation takes that literally: nothing is invalidated.
//
//   2. `satp_write_i` -- one pulse per committed `satp` write. Writing `satp`
//      does *not* imply a TLB flush in the ISA (software must SFENCE.VMA), but
//      this implementation flushes conservatively so that no translation can
//      outlive the context that produced it. That is over-invalidation, and
//      therefore permitted.
//
// A privilege or `mstatus` (SUM/MXR) change is deliberately **not** an
// invalidation source: the translation is a property of the page table and the
// ASID, and the privilege-dependent part -- the permission decision -- is
// re-evaluated on every hit against the current privilege, SUM and MXR. The
// spec says those changes take effect immediately *without* an SFENCE.VMA, and
// re-evaluating the decision is what makes that true here.
//
// ------------------------------------------------------- stale in-flight walks
//
// The spec forbids creating a cache entry that a later SFENCE.VMA would have
// invalidated (priv v1.12 §4.2.1: "they must not create address-translation
// cache entries if those entries would have been invalidated by any SFENCE.VMA
// instruction executed by the hart since the speculative execution of the
// algorithm began"). Every invalidation therefore bumps `gen_q`, and a walk is
// tagged with the generation in force when the *walker accepted it*. A walk
// that completes under a different generation is **not installed**; its result
// may still be returned to the requester (the spec permits any translation
// valid since the most recent subsuming fence, and the requester is either
// older than the fence or about to be flushed by it), but no cache entry is
// created. `o_stale_ctr` counts these.
//
// A *cancelled* walk (`xl_cancel_i`) is the same class: the walker aborts, this
// module abandons the request, and nothing is installed.
//
// ---------------------------------------------------------------- hit or miss
//
// A presented request is served from the cache only when all of these hold:
// the tag matches (VPN and, unless the entry is global, ASID); the entry is
// valid with A=1; the shared permission rule passes for *this* kind, privilege,
// SUM and MXR; and, for a store, the cached D is set. Otherwise:
//
//   * tag match but permission fails -> a *page fault* of the access class is
//     returned directly. The PTE's permission bits cannot change without a
//     fence, so the walk would return the same fault; not walking is correct
//     and is not "a PTE error as a cache miss".
//   * tag match but D clear on a store (or A clear) -> a miss: the walk is what
//     performs the A/D update, and its result is what gets installed.
//   * no tag match -> a miss.
//
// Bare mode and M privilege bypass the cache entirely (they are forwarded
// straight to the walker, which answers them as passthroughs): there is no
// translation to cache, and the rest of the machine is unaffected by this
// module's presence.
//
// ---------------------------------------------------------------- geometry
//
// 16 entries, 8 sets x 2 ways, set index va[15:13]. Two ways is what lets two
// address spaces with the same virtual address coexist, which is what makes
// "a per-ASID fence leaves the other ASID's entry alone" observable as a hit.
// Replacement prefers an invalid way, then an in-place match, then a per-set
// round-robin bit.
//
// ------------------------------------------------------- mutant switches
//
//   MOSAIC_TLB_MUTANT_SFENCE_NOOP         an SFENCE.VMA pulse invalidates
//                                         nothing (the card's central failure)
//   MOSAIC_TLB_MUTANT_FENCE_WRONG_ADDR    a fence with an address invalidates
//                                         the *next* page, leaving the named
//                                         one stale
//   MOSAIC_TLB_MUTANT_CANCEL_IGNORED      a cancelled walk still completes and
//                                         installs its entry
//   MOSAIC_TLB_MUTANT_SATP_NO_FLUSH       a satp write does not invalidate
//   MOSAIC_TLB_MUTANT_STORE_IGNORES_DIRTY a store hits an entry whose D is
//                                         clear, so the PTE is never marked
//                                         dirty
// ============================================================================

`ifndef MOSAIC_TLB_SV_
`define MOSAIC_TLB_SV_

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

`include "mosaic_pkg.sv"

// `sfence_va_i`'s low 12 bits are the page offset, which an SFENCE.VMA's page
// granularity means nothing to us; the port exists at full width because it is
// an address. The suppression is stated once for the file.
/* verilator lint_off UNUSEDSIGNAL */

module mosaic_tlb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------- translation request
    // The walker's request interface, plus the ASID that belongs in the tag.
    input  logic        xl_req_valid_i,
    output logic        xl_req_ready_o,
    input  logic [63:0] xl_va_i,
    input  logic [1:0]  xl_kind_i,
    input  logic [1:0]  xl_priv_i,
    input  logic [3:0]  xl_satp_mode_i,
    input  logic [43:0] xl_satp_ppn_i,
    input  logic [15:0] xl_satp_asid_i,
    input  logic        xl_sum_i,
    input  logic        xl_mxr_i,
    input  logic        xl_cancel_i,

    output logic        xl_rsp_valid_o,
    input  logic        xl_rsp_ready_i,
    output logic [63:0] xl_pa_o,
    output logic        xl_fault_o,
    output logic [3:0]  xl_cause_o,
    output logic [63:0] xl_tval_o,
    output logic [3:0]  xl_perms_o,
    output logic [2:0]  xl_attr_o,
    output logic        xl_bare_o,

    // --------------------------------------------------------- SFENCE.VMA
    // One pulse per executed SFENCE.VMA. `has_va`/`has_asid` are "rs1 != x0" /
    // "rs2 != x0", which is what distinguishes the four forms; a zero operand
    // value with its `has` bit clear means "all", not "page/ASID zero".
    input  logic        sfence_valid_i,
    input  logic [63:0] sfence_va_i,
    input  logic        sfence_has_va_i,
    input  logic [15:0] sfence_asid_i,
    input  logic        sfence_has_asid_i,

    // A committed `satp` write. One pulse; flushes the whole cache.
    input  logic        satp_write_i,

    // ------------------------------------------------------------ the PTE port
    // Passed through to the walker unchanged: the cache never issues a PTE
    // access of its own.
    output logic        pte_req_valid_o,
    input  logic        pte_req_ready_i,
    output logic        pte_req_we_o,
    output logic [63:0] pte_req_addr_o,
    output logic [63:0] pte_req_wdata_o,
    output logic [7:0]  pte_req_wstrb_o,
    input  logic        pte_rsp_valid_i,
    output logic        pte_rsp_ready_o,
    input  logic [63:0] pte_rsp_rdata_i,
    input  logic        pte_rsp_fault_i,

    // ------------------------------------------------------------- observability
    output logic        o_busy,
    output logic        o_hit,          // the presented request is a cache hit
    output logic [31:0] o_hit_ctr,
    output logic [31:0] o_miss_ctr,
    output logic [31:0] o_perm_fault_ctr,
    output logic [31:0] o_install_ctr,
    output logic [31:0] o_evict_ctr,
    output logic [31:0] o_stale_ctr,    // walk results dropped by a generation
    output logic [31:0] o_sfence_ctr,
    output logic [31:0] o_satp_flush_ctr,
    output logic [31:0] o_cancel_ctr,
    output logic [15:0] o_gen,
    // The walker's own counters, so a case can attribute walks to the cache.
    output logic [31:0] o_walk_ctr,
    output logic [31:0] o_leaf_ctr,
    output logic [31:0] o_fault_ctr,
    output logic [31:0] o_ad_ctr,
    output logic [31:0] o_walk_cancel_ctr
);

  // --------------------------------------------------------------- geometry
  localparam int unsigned TLB_SETS = 8;
  localparam int unsigned TLB_WAYS = 2;
  localparam int unsigned TLB_ENTRIES = TLB_SETS * TLB_WAYS;   // 16

  localparam logic [1:0] KIND_LOAD  = 2'd0;
  localparam logic [1:0] KIND_STORE = 2'd1;
  localparam logic [1:0] KIND_FETCH = 2'd2;

  localparam logic [1:0] PRIV_M = 2'b11;

  localparam logic [3:0] SATP_MODE_SV39 = 4'd8;

`ifdef MOSAIC_TLB_MUTANT_CANCEL_IGNORED
  // NEGATIVE CONTROL: a cancel is not honoured, so the walk completes and
  // installs its entry -- "an entry installed from a cancelled walk".
  localparam logic CANCEL_IGNORED = 1'b1;
`else
  localparam logic CANCEL_IGNORED = 1'b0;
`endif

  typedef enum logic [1:0] {
    ST_IDLE = 2'd0,
    ST_REQ  = 2'd1,   // offering the request to the walker
    ST_WAIT = 2'd2,   // taking the walker's result
    ST_DONE = 2'd3    // holding a response for the requester
  } state_e;

  state_e state_q;

  // ------------------------------------------------------------ the entries
  // Parallel arrays rather than an array of structs, so no assignment pattern
  // is needed to reset or install one.
  logic [TLB_ENTRIES-1:0] ent_valid_q;
  logic [TLB_ENTRIES-1:0] ent_g_q;
  logic [15:0]            ent_asid_q  [0:TLB_ENTRIES-1];
  logic [26:0]            ent_vpn_q   [0:TLB_ENTRIES-1];
  logic [43:0]            ent_ppn_q   [0:TLB_ENTRIES-1];
  logic [3:0]             ent_perms_q [0:TLB_ENTRIES-1];
  logic [TLB_ENTRIES-1:0] ent_a_q;
  logic [TLB_ENTRIES-1:0] ent_d_q;

  // Per-set replacement bit: which way to replace when both are valid and
  // neither matches.
  logic [TLB_SETS-1:0] rr_q;

  // ---------------------------------------------------- the generation counter
  logic [15:0] gen_q;

  // --------------------------------------------------------- the request latch
  logic [63:0] va_q;
  logic [1:0]  kind_q;
  logic [1:0]  priv_q;
  logic [3:0]  mode_q;
  logic [43:0] ppn_q;
  logic [15:0] asid_q;
  logic        sum_q;
  logic        mxr_q;
  logic        bypass_q;      // Bare / M: forward and never cache
  logic [15:0] walk_gen_q;

  // ------------------------------------------------------------ the response
  logic        rsp_valid_q;
  logic [63:0] rsp_pa_q;
  logic        rsp_fault_q;
  logic [3:0]  rsp_cause_q;
  logic [63:0] rsp_tval_q;
  logic [3:0]  rsp_perms_q;
  logic [2:0]  rsp_attr_q;
  logic        rsp_bare_q;

  // ---------------------------------------------------------------- counters
  logic [31:0] hit_ctr_q, miss_ctr_q, perm_fault_ctr_q, install_ctr_q;
  logic [31:0] evict_ctr_q, stale_ctr_q, sfence_ctr_q, satp_flush_ctr_q;
  logic [31:0] cancel_ctr_q;

  // ------------------------------------------------------- the walker's ports
  logic        ptw_xl_req_valid;
  logic        ptw_xl_req_ready;
  logic        ptw_xl_rsp_valid;
  logic [63:0] ptw_xl_pa;
  logic        ptw_xl_fault;
  logic [3:0]  ptw_xl_cause;
  logic [63:0] ptw_xl_tval;
  logic [3:0]  ptw_xl_perms;
  logic [2:0]  ptw_xl_attr;
  logic        ptw_xl_bare;
  logic        ptw_xl_cancel;
  logic [31:0] ptw_walk_ctr, ptw_leaf_ctr, ptw_fault_ctr, ptw_ad_ctr;
  logic [31:0] ptw_cancel_ctr;

  // ==========================================================================
  // Invalidation
  // ==========================================================================
  // A non-canonical rs1 makes the SFENCE.VMA have no effect.
  logic        sfence_canonical_c;
  logic        sfence_effective_c;
  logic [26:0] sfence_vpn_c;
  logic [TLB_ENTRIES-1:0] va_match_c;
  logic [TLB_ENTRIES-1:0] asid_match_c;
  logic [TLB_ENTRIES-1:0] sfence_kill_c;
  logic        invalidate_any_c;

  assign sfence_canonical_c = (sfence_va_i[63:39] == {25{sfence_va_i[38]}});
  assign sfence_effective_c = !(sfence_has_va_i && !sfence_canonical_c);

  // The fence is page-granular (`[rs1, rs1+4KiB)`), so the low 12 bits of the
  // address play no part in the comparison.
`ifdef MOSAIC_TLB_MUTANT_FENCE_WRONG_ADDR
  // NEGATIVE CONTROL: an address fence invalidates the *next* page, so the page
  // it names keeps its stale translation. CASE=tlb.sfence_vma's by-address
  // check names it.
  assign sfence_vpn_c = sfence_va_i[38:12] + 27'd1;
`else
  assign sfence_vpn_c = sfence_va_i[38:12];
`endif

  always_comb begin : fence_match
    va_match_c   = {TLB_ENTRIES{1'b0}};
    asid_match_c = {TLB_ENTRIES{1'b0}};
    for (int unsigned i = 0; i < TLB_ENTRIES; i++) begin
      va_match_c[i]   = !sfence_has_va_i || (ent_vpn_q[i] == sfence_vpn_c);
      asid_match_c[i] = !sfence_has_asid_i || (ent_asid_q[i] == sfence_asid_i);
    end
  end

  always_comb begin : fence_kill
    sfence_kill_c = {TLB_ENTRIES{1'b0}};
`ifndef MOSAIC_TLB_MUTANT_SFENCE_NOOP
    if (sfence_valid_i && sfence_effective_c) begin
      for (int unsigned i = 0; i < TLB_ENTRIES; i++) begin
        if (ent_valid_q[i]) begin
          if (sfence_has_asid_i) begin
            // A per-ASID fence does not invalidate global mappings.
            if (va_match_c[i] && asid_match_c[i] && !ent_g_q[i])
              sfence_kill_c[i] = 1'b1;
          end else begin
            if (va_match_c[i]) sfence_kill_c[i] = 1'b1;
          end
        end
      end
    end
`endif
  end

`ifdef MOSAIC_TLB_MUTANT_SATP_NO_FLUSH
  // NEGATIVE CONTROL: a satp write does not invalidate, so an entry from the
  // old context survives it. CASE=tlb.sfence_vma's satp-write check names it.
  assign invalidate_any_c = (sfence_valid_i && sfence_effective_c);
`else
  assign invalidate_any_c = (sfence_valid_i && sfence_effective_c) || satp_write_i;
`endif

  // ==========================================================================
  // Hit detection on the presented request
  // ==========================================================================
  logic        bypass_c;
  logic [2:0]  set_c;
  logic [3:0]  idx0_c, idx1_c;
  logic        tag0_c, tag1_c;
  logic        perm0_c, perm1_c;
  logic        d_ok0_c, d_ok1_c;
  logic        use0_c, use1_c;
  logic        hit_c, perm_bad_c;
  logic        hit_way_c;
  logic [63:0] hit_pa_c;
  logic [3:0]  hit_perms_c;
  logic [3:0]  page_cause_req_c;

  assign bypass_c = (xl_satp_mode_i != SATP_MODE_SV39) || (xl_priv_i == PRIV_M);
  assign set_c    = xl_va_i[15:13];
  assign idx0_c   = {set_c, 1'b0};
  assign idx1_c   = {set_c, 1'b1};

  assign tag0_c = ent_valid_q[idx0_c] &&
                  (ent_vpn_q[idx0_c] == xl_va_i[38:12]) &&
                  (ent_g_q[idx0_c] || (ent_asid_q[idx0_c] == xl_satp_asid_i));
  assign tag1_c = ent_valid_q[idx1_c] &&
                  (ent_vpn_q[idx1_c] == xl_va_i[38:12]) &&
                  (ent_g_q[idx1_c] || (ent_asid_q[idx1_c] == xl_satp_asid_i));

  assign perm0_c = mosaic_pkg::leaf_perm_ok(ent_perms_q[idx0_c], xl_kind_i,
                                            xl_priv_i, xl_sum_i, xl_mxr_i);
  assign perm1_c = mosaic_pkg::leaf_perm_ok(ent_perms_q[idx1_c], xl_kind_i,
                                            xl_priv_i, xl_sum_i, xl_mxr_i);

  assign d_ok0_c = (xl_kind_i != KIND_STORE) || ent_d_q[idx0_c];
  assign d_ok1_c = (xl_kind_i != KIND_STORE) || ent_d_q[idx1_c];
`ifdef MOSAIC_TLB_MUTANT_STORE_IGNORES_DIRTY
  // NEGATIVE CONTROL: a store is served from an entry whose cached D is clear,
  // so the walker never runs and the PTE's D bit is never set.
  // CASE=tlb.sfence_vma's "a store sets D" check names it.
  assign use0_c = tag0_c && perm0_c && ent_a_q[idx0_c];
  assign use1_c = tag1_c && perm1_c && ent_a_q[idx1_c];
`else
  assign use0_c = tag0_c && perm0_c && ent_a_q[idx0_c] && d_ok0_c;
  assign use1_c = tag1_c && perm1_c && ent_a_q[idx1_c] && d_ok1_c;
`endif

  assign hit_c      = !bypass_c && !invalidate_any_c && (use0_c || use1_c);
  assign perm_bad_c = !bypass_c && !invalidate_any_c && !hit_c &&
                      ((tag0_c && !perm0_c) || (tag1_c && !perm1_c));

  assign hit_way_c   = use0_c ? 1'b0 : 1'b1;
  assign hit_pa_c    = {8'd0, ent_ppn_q[hit_way_c ? idx1_c : idx0_c], xl_va_i[11:0]};
  assign hit_perms_c = ent_perms_q[hit_way_c ? idx1_c : idx0_c];

  always_comb begin : page_cause
    case (xl_kind_i)
      KIND_STORE: page_cause_req_c = 4'd15;
      KIND_FETCH: page_cause_req_c = 4'd12;
      default:    page_cause_req_c = 4'd13;
    endcase
  end

  assign o_hit = hit_c;

  // ==========================================================================
  // The install decision
  // ==========================================================================
  logic        walk_rsp_take_c;
  logic        install_ok_c;
  logic [2:0]  iset_c;
  logic [3:0]  inst0_c, inst1_c;
  logic        inst_match0_c, inst_match1_c;
  logic        inst_way_c;
  logic [3:0]  inst_idx_c;

  assign walk_rsp_take_c = (state_q == ST_WAIT) && ptw_xl_rsp_valid;

  assign iset_c = va_q[15:13];
  assign inst0_c = {iset_c, 1'b0};
  assign inst1_c = {iset_c, 1'b1};
  assign inst_match0_c = ent_valid_q[inst0_c] &&
                         (ent_vpn_q[inst0_c] == va_q[38:12]) &&
                         (ent_g_q[inst0_c] || (ent_asid_q[inst0_c] == asid_q));
  assign inst_match1_c = ent_valid_q[inst1_c] &&
                         (ent_vpn_q[inst1_c] == va_q[38:12]) &&
                         (ent_g_q[inst1_c] || (ent_asid_q[inst1_c] == asid_q));

  always_comb begin : install_way
    if (!ent_valid_q[inst0_c])      inst_way_c = 1'b0;
    else if (!ent_valid_q[inst1_c]) inst_way_c = 1'b1;
    else if (inst_match0_c)         inst_way_c = 1'b0;
    else if (inst_match1_c)         inst_way_c = 1'b1;
    else                            inst_way_c = rr_q[iset_c];
  end
  assign inst_idx_c = inst_way_c ? inst1_c : inst0_c;

  // A walk result is installed only when it is a real Sv39 translation, the
  // walker did not fault, no invalidation is happening in this cycle, and the
  // generation the walk began under is still current. Anything else is dropped
  // -- that is the "a late walk must not re-insert a stale translation" rule.
  assign install_ok_c = walk_rsp_take_c && !ptw_xl_fault && !ptw_xl_bare &&
                        !bypass_q && !invalidate_any_c && (gen_q == walk_gen_q);

  // ==========================================================================
  // The walker
  // ==========================================================================
  assign ptw_xl_req_valid = (state_q == ST_REQ);
`ifdef MOSAIC_TLB_MUTANT_CANCEL_IGNORED
  assign ptw_xl_cancel = 1'b0;
`else
  assign ptw_xl_cancel = xl_cancel_i &&
                         ((state_q == ST_REQ) || (state_q == ST_WAIT));
`endif

  // The walker's unused observation pins are named empty rather than declared
  // as dead nets; nothing functional is routed through them.
  /* verilator lint_off PINCONNECTEMPTY */
  mosaic_ptw u_ptw (
      .clk             (clk),
      .rst             (rst),
      .xl_req_valid_i  (ptw_xl_req_valid),
      .xl_req_ready_o  (ptw_xl_req_ready),
      .xl_va_i         (va_q),
      .xl_kind_i       (kind_q),
      .xl_priv_i       (priv_q),
      .xl_satp_mode_i  (mode_q),
      .xl_satp_ppn_i   (ppn_q),
      .xl_sum_i        (sum_q),
      .xl_mxr_i        (mxr_q),
      .xl_cancel_i     (ptw_xl_cancel),
      .xl_rsp_valid_o  (ptw_xl_rsp_valid),
      .xl_rsp_ready_i  (1'b1),
      .xl_pa_o         (ptw_xl_pa),
      .xl_fault_o      (ptw_xl_fault),
      .xl_cause_o      (ptw_xl_cause),
      .xl_tval_o       (ptw_xl_tval),
      .xl_perms_o      (ptw_xl_perms),
      .xl_attr_o       (ptw_xl_attr),
      .xl_bare_o       (ptw_xl_bare),
      .pte_req_valid_o (pte_req_valid_o),
      .pte_req_ready_i (pte_req_ready_i),
      .pte_req_we_o    (pte_req_we_o),
      .pte_req_addr_o  (pte_req_addr_o),
      .pte_req_wdata_o (pte_req_wdata_o),
      .pte_req_wstrb_o (pte_req_wstrb_o),
      .pte_rsp_valid_i (pte_rsp_valid_i),
      .pte_rsp_ready_o (pte_rsp_ready_o),
      .pte_rsp_rdata_i (pte_rsp_rdata_i),
      .pte_rsp_fault_i (pte_rsp_fault_i),
      .o_busy          (),
      .o_walk_ctr      (ptw_walk_ctr),
      .o_bare_ctr      (),
      .o_leaf_ctr      (ptw_leaf_ctr),
      .o_fault_ctr     (ptw_fault_ctr),
      .o_ad_upd_ctr    (ptw_ad_ctr),
      .o_retry_ctr     (),
      .o_cancel_ctr    (ptw_cancel_ctr),
      .o_last_fault_cause (),
      .o_last_fault_tval  ()
  );
  /* verilator lint_on PINCONNECTEMPTY */

  // ==========================================================================
  // Outputs
  // ==========================================================================
  assign xl_req_ready_o = (state_q == ST_IDLE) && ptw_xl_req_ready;
  assign xl_rsp_valid_o = rsp_valid_q;
  assign xl_pa_o        = rsp_pa_q;
  assign xl_fault_o     = rsp_fault_q;
  assign xl_cause_o     = rsp_cause_q;
  assign xl_tval_o      = rsp_tval_q;
  assign xl_perms_o     = rsp_perms_q;
  assign xl_attr_o      = rsp_attr_q;
  assign xl_bare_o      = rsp_bare_q;

  assign o_busy           = (state_q != ST_IDLE);
  assign o_hit_ctr        = hit_ctr_q;
  assign o_miss_ctr       = miss_ctr_q;
  assign o_perm_fault_ctr = perm_fault_ctr_q;
  assign o_install_ctr    = install_ctr_q;
  assign o_evict_ctr      = evict_ctr_q;
  assign o_stale_ctr      = stale_ctr_q;
  assign o_sfence_ctr     = sfence_ctr_q;
  assign o_satp_flush_ctr = satp_flush_ctr_q;
  assign o_cancel_ctr     = cancel_ctr_q;
  assign o_gen            = gen_q;
  assign o_walk_ctr       = ptw_walk_ctr;
  assign o_leaf_ctr       = ptw_leaf_ctr;
  assign o_fault_ctr      = ptw_fault_ctr;
  assign o_ad_ctr         = ptw_ad_ctr;
  assign o_walk_cancel_ctr = ptw_cancel_ctr;

  // ==========================================================================
  // The state machine and the entries
  // ==========================================================================
  always_ff @(posedge clk) begin
    if (rst) begin
      state_q          <= ST_IDLE;
      ent_valid_q      <= {TLB_ENTRIES{1'b0}};
      ent_g_q          <= {TLB_ENTRIES{1'b0}};
      ent_a_q          <= {TLB_ENTRIES{1'b0}};
      ent_d_q          <= {TLB_ENTRIES{1'b0}};
      rr_q             <= {TLB_SETS{1'b0}};
      gen_q            <= 16'd0;
      for (int unsigned i = 0; i < TLB_ENTRIES; i++) begin
        ent_asid_q[i]  <= 16'd0;
        ent_vpn_q[i]   <= 27'd0;
        ent_ppn_q[i]   <= 44'd0;
        ent_perms_q[i] <= 4'd0;
      end
      va_q        <= 64'd0;
      kind_q      <= KIND_LOAD;
      priv_q      <= 2'b01;
      mode_q      <= 4'd0;
      ppn_q       <= 44'd0;
      asid_q      <= 16'd0;
      sum_q       <= 1'b0;
      mxr_q       <= 1'b0;
      bypass_q    <= 1'b0;
      walk_gen_q  <= 16'd0;
      rsp_valid_q <= 1'b0;
      rsp_pa_q    <= 64'd0;
      rsp_fault_q <= 1'b0;
      rsp_cause_q <= 4'd0;
      rsp_tval_q  <= 64'd0;
      rsp_perms_q <= 4'd0;
      rsp_attr_q  <= 3'd0;
      rsp_bare_q  <= 1'b0;
      hit_ctr_q        <= 32'd0;
      miss_ctr_q       <= 32'd0;
      perm_fault_ctr_q <= 32'd0;
      install_ctr_q    <= 32'd0;
      evict_ctr_q      <= 32'd0;
      stale_ctr_q      <= 32'd0;
      sfence_ctr_q     <= 32'd0;
      satp_flush_ctr_q <= 32'd0;
      cancel_ctr_q     <= 32'd0;
    end else begin
      // The response leaves when the requester takes it.
      if (rsp_valid_q && xl_rsp_ready_i) rsp_valid_q <= 1'b0;

      // ------------------------------------------------------- invalidation
      if (invalidate_any_c) begin
        gen_q <= gen_q + 16'd1;
        for (int unsigned i = 0; i < TLB_ENTRIES; i++) begin
          if (sfence_kill_c[i]) ent_valid_q[i] <= 1'b0;
        end
`ifndef MOSAIC_TLB_MUTANT_SATP_NO_FLUSH
        if (satp_write_i) begin
          ent_valid_q <= {TLB_ENTRIES{1'b0}};
        end
`endif
      end
      if (sfence_valid_i && sfence_effective_c) sfence_ctr_q <= sfence_ctr_q + 32'd1;
      if (satp_write_i) satp_flush_ctr_q <= satp_flush_ctr_q + 32'd1;

      // ------------------------------------------------------------ install
      if (install_ok_c) begin
        if (ent_valid_q[inst_idx_c] && !(inst_match0_c || inst_match1_c)) begin
          evict_ctr_q <= evict_ctr_q + 32'd1;
        end
        if (!inst_match0_c && !inst_match1_c && ent_valid_q[inst0_c] &&
            ent_valid_q[inst1_c]) begin
          rr_q[iset_c] <= ~rr_q[iset_c];
        end
        ent_valid_q[inst_idx_c] <= 1'b1;
        ent_g_q[inst_idx_c]     <= ptw_xl_attr[2];
        ent_asid_q[inst_idx_c]  <= asid_q;
        ent_vpn_q[inst_idx_c]   <= va_q[38:12];
        ent_ppn_q[inst_idx_c]   <= ptw_xl_pa[55:12];
        ent_perms_q[inst_idx_c] <= ptw_xl_perms;
        ent_a_q[inst_idx_c]     <= ptw_xl_attr[1];
        ent_d_q[inst_idx_c]     <= ptw_xl_attr[0];
        install_ctr_q <= install_ctr_q + 32'd1;
      end else if (walk_rsp_take_c && !ptw_xl_fault && !ptw_xl_bare &&
                   !bypass_q) begin
        // A real translation that was not installed: an invalidation landed
        // during the walk, or the generation moved.
        stale_ctr_q <= stale_ctr_q + 32'd1;
      end

      // ------------------------------------------------------- the FSM
      case (state_q)
        // ------------------------------------------------------------------
        ST_IDLE: begin
          if (xl_req_valid_i && ptw_xl_req_ready) begin
            va_q    <= xl_va_i;
            kind_q  <= xl_kind_i;
            priv_q  <= xl_priv_i;
            mode_q  <= xl_satp_mode_i;
            ppn_q   <= xl_satp_ppn_i;
            asid_q  <= xl_satp_asid_i;
            sum_q   <= xl_sum_i;
            mxr_q   <= xl_mxr_i;
            bypass_q <= bypass_c;
            if (!bypass_c && !invalidate_any_c && hit_c) begin
              rsp_pa_q    <= hit_pa_c;
              rsp_fault_q <= 1'b0;
              rsp_cause_q <= 4'd0;
              rsp_tval_q  <= xl_va_i;
              rsp_perms_q <= hit_perms_c;
              rsp_attr_q  <= 3'd0;
              rsp_bare_q  <= 1'b0;
              rsp_valid_q <= 1'b1;
              hit_ctr_q   <= hit_ctr_q + 32'd1;
              state_q     <= ST_DONE;
            end else if (!bypass_c && !invalidate_any_c && perm_bad_c) begin
              // A tag match whose permission fails: the PTE cannot change
              // without a fence, so the fault is returned without a walk.
              rsp_pa_q    <= 64'd0;
              rsp_fault_q <= 1'b1;
              rsp_cause_q <= page_cause_req_c;
              rsp_tval_q  <= xl_va_i;
              rsp_perms_q <= 4'd0;
              rsp_attr_q  <= 3'd0;
              rsp_bare_q  <= 1'b0;
              rsp_valid_q <= 1'b1;
              perm_fault_ctr_q <= perm_fault_ctr_q + 32'd1;
              state_q     <= ST_DONE;
            end else begin
              miss_ctr_q <= miss_ctr_q + 32'd1;
              state_q    <= ST_REQ;
            end
          end
        end

        // ------------------------------------------------------------------
        // Offer the request to the walker. The fields are the latched ones, so
        // the walker's transport rule ("hold the payload until ready") holds.
        ST_REQ: begin
          if (xl_cancel_i && !CANCEL_IGNORED) begin
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            state_q      <= ST_IDLE;
          end else if (ptw_xl_req_ready) begin
            walk_gen_q <= gen_q + (invalidate_any_c ? 16'd1 : 16'd0);
            state_q    <= ST_WAIT;
          end
        end

        // ------------------------------------------------------------------
        ST_WAIT: begin
          if (xl_cancel_i && !CANCEL_IGNORED) begin
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            state_q      <= ST_IDLE;
          end else if (ptw_xl_rsp_valid) begin
            rsp_pa_q    <= ptw_xl_pa;
            rsp_fault_q <= ptw_xl_fault;
            rsp_cause_q <= ptw_xl_cause;
            rsp_tval_q  <= ptw_xl_tval;
            rsp_perms_q <= ptw_xl_perms;
            rsp_attr_q  <= ptw_xl_attr;
            rsp_bare_q  <= ptw_xl_bare;
            rsp_valid_q <= 1'b1;
            state_q     <= ST_DONE;
          end
        end

        // ------------------------------------------------------------------
        ST_DONE: begin
          if (xl_cancel_i && !CANCEL_IGNORED) begin
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            rsp_valid_q  <= 1'b0;
            state_q      <= ST_IDLE;
          end else if (rsp_valid_q && xl_rsp_ready_i) begin
            state_q <= ST_IDLE;
          end
        end

        default: state_q <= ST_IDLE;
      endcase
    end
  end

endmodule

`endif  // MOSAIC_TLB_SV_
