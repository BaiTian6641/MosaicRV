// ============================================================================
// mosaic_ptw -- work package I-045: the serial Sv39 page-table walker.
//
// This module is the *translation engine*: given a virtual address and the
// access it belongs to, it either produces the physical address and the leaf
// PTE's permission metadata, or it produces the `cause`/`tval` pair of the
// exception the ISA names. It owns no architectural state beyond the walk in
// flight; `satp`, `mstatus.SUM` and `mstatus.MXR` are inputs, because they are
// committed CSR state and the walker must not keep a second copy of them.
//
// ------------------------------------------------------------- one at a time
//
// The card asks for a **serial PTW first**: exactly one walk is in flight, and
// no translation is speculated. That is a structural property here rather than
// a policy: `xl_req_ready_o` is high only in `ST_IDLE`, so the requester holds
// its virtual address stable until the walk it asked for is finished, and the
// walker holds the single physical PTE port for the whole of it.
//
// The request handshake is the project-wide one, with one consequence the
// requester must honour: `xl_req_valid_i` may be asserted only until
// `xl_req_ready_o` is seen, because the *result* of the walk is a separate
// response (`xl_rsp_valid_o`) that the requester waits for. A requester that
// left its request asserted while waiting for the result would ask for a second
// walk the moment the walker returned to `ST_IDLE`.
//
// ------------------------------------------------------------- the algorithm
//
// The walk is the privileged specification's own step order (Priv v1.12
// "Virtual Address Translation Process"), and the order is load-bearing because
// it is the fault *priority*:
//
//   2. read the PTE at `pt + 8*vpn[i]`. A PMA/PMP refusal of that *implicit*
//      access is an **access fault** corresponding to the original access type
//      -- not a page fault. (`pt` is the physical address of the current table.)
//   3. V=0, or the reserved encoding W=1&R=0, or a bit reserved for future
//      standard use ([63:54] on Sv39) -> **page fault**.
//   4. R=1 or X=1 -> leaf (step 5). Otherwise the PTE is a pointer: descend to
//      `i-1`; if `i` was already 0 the PTE is a non-leaf at the last level and
//      the walk faults.
//   5. permission check against R/W/X, U, SUM and MXR -> page fault.
//   6. `i>0` and the low `i*9` bits of the leaf PPN nonzero -> misaligned
//      superpage -> page fault.
//   7. A=0, or a store with D=0 -> update the PTE (see below).
//   8. compose the physical address: the page offset is untranslated, a
//      superpage takes its low VPN bits from the virtual address, and the rest
//      of the PPN comes from the leaf.
//
// So a non-canonical virtual address (bits [63:39] not equal to bit 38) faults
// in a step *before* step 2: it is rejected in `ST_IDLE` and **no PTE is ever
// read**, which is the card's first failure mode ("a non-canonical address
// still reaching physical RAM") made unreachable rather than argued away.
//
// ------------------------------------------------------------- the A/D policy
//
// The ISA defines two schemes and forbids leaving the choice undefined. This
// implementation takes the **hardware-update** scheme (not Svade):
//
//   * a leaf with A=0 is *updated*: the walker re-reads the PTE, compares (the
//     atomicity the spec requires of the update), and if the value is unchanged
//     writes it back with A=1;
//   * a leaf written by a store/AMO with D=0 is likewise written back with
//     A=1 and D=1;
//   * if the re-read does not match, the PTE changed under the walk and the
//     walk re-evaluates the fresh value -- it never writes a stale update over
//     a newer PTE;
//   * if the write itself is refused by PMA/PMP, the exception is an **access
//     fault** of the original type (step 7's own rule).
//
// A cancelled walk that has not yet issued its write leaves **no** A/D update
// behind: the update is the last act of a walk and it is issued only from
// `ST_AD_WRITE` after the compare, so a cancel during the walk (the
// wrong-path-load case) cannot have set a bit. This is the same class of
// property as "a wrong-path store writes nothing", and the case checks it.
//
// ------------------------------------------------------------- MXR and SUM
//
// MXR (make executable readable) lets a load use X, and a fetch use R. SUM
// (permit supervisor user memory) lets an S-mode load/store touch a U page; it
// does not apply to fetch -- "the supervisor may not execute code on pages with
// U=1" irrespective of SUM -- so an S-mode fetch of a U page always faults.
// Privilege M bypasses translation entirely (the caller is expected not to ask,
// but a passthrough is the correct answer if it does), and Bare mode does too.
//
// ------------------------------------------------------------- the PTE port
//
// The port is a plain 8-byte read/write beat (`pte_req_*`/`pte_rsp_*`). Reads
// are issued for every level and for the compare of step 7; the only write this
// module ever performs is the A/D update, always a full 8-byte store. Both
// sides use the project-wide valid/ready rule and the walker holds the port for
// one beat at a time, so a PTE read, a PTE write and an ordinary data access
// cannot be confused by the memory system.
//
// ------------------------------------------------------- cancellation
//
// `xl_cancel_i` aborts the walk in flight. It is level-sensitive and is sampled
// in every state: the walker returns to `ST_IDLE`, drops any held response, and
// issues no further PTE beat. If a beat is already on the memory port it is
// allowed to complete (the memory system owns it) and its result is discarded.
// A cancel that arrives while `ST_AD_WRITE`'s write is already on the port
// cannot un-issue that write; the case cancels before the update (which is the
// reachable wrong-path case) and the report says so.
//
// ------------------------------------------------------- mutant switches
//
//   MOSAIC_PTW_MUTANT_ALLOW_NONCANONICAL  a non-canonical address is translated
//                                         anyway (cause 12/13/15 never raised)
//   MOSAIC_PTW_MUTANT_PTE_ERROR_OK        an invalid/reserved PTE is treated as
//                                         a successful mapping to physical 0
//   MOSAIC_PTW_MUTANT_SUPERPAGE_UNALIGNED the low superpage VPN bits are taken
//                                         from the PTE instead of faulting
//   MOSAIC_PTW_MUTANT_SUM_IGNORED         S-mode is allowed to read a U page
//                                         with SUM=0
//   MOSAIC_PTW_MUTANT_AD_INVERTED         the A/D policy is inverted: a leaf
//                                         that needs an update faults and one
//                                         that does not is written anyway
// ============================================================================

`ifndef MOSAIC_PTW_SV_
`define MOSAIC_PTW_SV_

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

`include "mosaic_pkg.sv"

module mosaic_ptw (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------- translation request
    // `xl_req_ready_o` is high only when the walker is idle, so an accepted
    // request is the only one in flight and its fields are stable for the whole
    // walk (the requester holds them by the transport rule).
    input  logic        xl_req_valid_i,
    output logic        xl_req_ready_o,
    input  logic [63:0] xl_va_i,
    // 0 load, 1 store (and AMO/SC), 2 fetch. It selects which permission bit the
    // leaf must carry and which page-fault/access-fault cause the original access
    // maps to.
    input  logic [1:0]  xl_kind_i,
    // 0 U, 1 S, 3 M. M short-circuits to a passthrough.
    input  logic [1:0]  xl_priv_i,
    // satp.MODE: 0 Bare, 8 Sv39 (the only value this profile's CSR can hold).
    input  logic [3:0]  xl_satp_mode_i,
    input  logic [43:0] xl_satp_ppn_i,
    input  logic        xl_sum_i,
    input  logic        xl_mxr_i,

    // A redirect or a context change withdraws the request. Level-sensitive.
    input  logic        xl_cancel_i,

    // -------------------------------------------------------- translation result
    // Held until accepted, like the endpoint's response: it names the uop it
    // belongs to only through the requester's identity, which the requester
    // keeps.
    output logic        xl_rsp_valid_o,
    input  logic        xl_rsp_ready_i,
    output logic [63:0] xl_pa_o,
    output logic        xl_fault_o,
    output logic [3:0]  xl_cause_o,     // 1/5/7 access fault, 12/13/15 page fault
    output logic [63:0] xl_tval_o,      // the original virtual address
    output logic [3:0]  xl_perms_o,     // {x, w, r, u} of the leaf (0 when faulting)
    output logic        xl_bare_o,      // the translation was a Bare passthrough

    // ------------------------------------------------------------ the PTE port
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
    output logic [31:0] o_walk_ctr,     // walks accepted
    output logic [31:0] o_bare_ctr,     // passthroughs
    output logic [31:0] o_leaf_ctr,     // leaves reached
    output logic [31:0] o_fault_ctr,    // walks that produced a fault
    output logic [31:0] o_ad_upd_ctr,   // A/D writes performed
    output logic [31:0] o_retry_ctr,    // compare failures that re-evaluated
    output logic [31:0] o_cancel_ctr,   // walks cancelled
    output logic [63:0] o_last_fault_cause,
    output logic [63:0] o_last_fault_tval
);

  // The access class, as the requester labels it.
  localparam logic [1:0] KIND_LOAD  = 2'd0;
  localparam logic [1:0] KIND_STORE = 2'd1;
  localparam logic [1:0] KIND_FETCH = 2'd2;

  // Privilege.
  localparam logic [1:0] PRIV_U = 2'b00;
  localparam logic [1:0] PRIV_S = 2'b01;
  localparam logic [1:0] PRIV_M = 2'b11;

  localparam logic [3:0] SATP_MODE_BARE = 4'd0;
  localparam logic [3:0] SATP_MODE_SV39 = 4'd8;

  // The compare-and-set of step 7 is a loop in the specification. It terminates
  // because the PTE cannot change under a walk that is atomic with respect to
  // concurrent writes; this profile has one hart and no other agent writing the
  // tables, so the re-read is a formality. The bound turns a hypothetical
  // livelock into a bounded number of attempts and then proceeds with the value
  // it read, rather than hanging the hart.
  localparam logic [2:0] RETRY_LIMIT = 3'd4;

  typedef enum logic [2:0] {
    ST_IDLE     = 3'd0,
    ST_READ     = 3'd1,   // offering a PTE read and taking its response
    ST_EVAL     = 3'd2,   // step 3/4/5/6/7 on the PTE just read
    ST_AD_READ  = 3'd3,   // step 7's compare re-read
    ST_AD_WRITE = 3'd4,   // offering the A/D write and taking its ack
    ST_COMPOSE  = 3'd5,   // step 8
    ST_DONE     = 3'd6    // holding the result for the requester
  } state_e;

  state_e state_q;

  // --------------------------------------------------------- the request latch
  logic [63:0] va_q;
  logic [1:0]  kind_q;
  logic [1:0]  priv_q;
  logic        sum_q;
  logic        mxr_q;

  // The walk position: the current level (2..0 for Sv39), the current table's
  // PPN, and the PTE being examined.
  logic [1:0]  level_q;
  logic [43:0] table_ppn_q;
  logic [63:0] pte_q;
  logic [43:0] pte_ppn_q;
  logic [2:0]  retry_q;

  // One PTE beat at a time: `beat_sent_q` is set when the memory accepted the
  // request and clears when the response has been consumed.
  logic        beat_sent_q;

  // A cancel that arrived while a beat was on the port. The beat completes (the
  // memory owns it), its result is discarded, and the walk ends.
  logic        cancel_pending_q;

  // ------------------------------------------------------------ the result
  logic        rsp_valid_q;
  logic [63:0] pa_q;
  logic        fault_q;
  logic [3:0]  cause_q;
  logic [63:0] tval_q;
  logic [3:0]  perms_q;
  logic        bare_q;

  // ---------------------------------------------------------------- counters
  logic [31:0] walk_ctr_q, bare_ctr_q, leaf_ctr_q, fault_ctr_q;
  logic [31:0] ad_upd_ctr_q, retry_ctr_q, cancel_ctr_q;
  logic [63:0] last_fault_cause_q, last_fault_tval_q;

  // ==========================================================================
  // Combinatorial helpers on the current PTE and walk position
  // ==========================================================================
  logic [8:0]  vpn_c;
  logic [43:0] ppn_eff_c;
  logic        align_bad_c;
  logic        perm_fail_c;
  logic        leaf_c;
  logic        reserved_c;
  logic        need_a_c;
  logic        need_d_c;
  logic [63:0] pte_addr_c;
  logic [63:0] pte_new_c;
  logic [3:0]  page_cause_c;
  logic [3:0]  access_cause_c;

  always_comb begin
    case (kind_q)
      KIND_STORE: begin page_cause_c = 4'd15; access_cause_c = 4'd7; end
      KIND_FETCH: begin page_cause_c = 4'd12; access_cause_c = 4'd1; end
      default:    begin page_cause_c = 4'd13; access_cause_c = 4'd5; end
    endcase
  end

  always_comb begin
    case (level_q)
      2'd2:    vpn_c = va_q[38:30];
      2'd1:    vpn_c = va_q[29:21];
      default: vpn_c = va_q[20:12];
    endcase
  end

  assign pte_addr_c = {8'd0, table_ppn_q, 12'd0} | {52'd0, vpn_c, 3'd0};

  // Step 8's composition, and step 6's alignment test in the same units: a
  // superpage at level i takes the low i 9-bit VPN chunks of the PPN from the
  // virtual address, so those bits of the leaf PPN must be zero.
  always_comb begin
    case (level_q)
      2'd2: begin
        ppn_eff_c   = {pte_ppn_q[43:18], va_q[29:12]};
        align_bad_c = (pte_ppn_q[17:0] != 18'd0);
      end
      2'd1: begin
        ppn_eff_c   = {pte_ppn_q[43:9], va_q[20:12]};
        align_bad_c = (pte_ppn_q[8:0] != 9'd0);
      end
      default: begin
        ppn_eff_c   = pte_ppn_q;
        align_bad_c = 1'b0;
      end
    endcase
  end

  // A leaf PTE has R=1 or X=1; W=1 with R=0 is the reserved encoding.
  assign leaf_c     = pte_q[1] | pte_q[3];
  assign reserved_c = (pte_q[63:54] != 10'd0) | (pte_q[2] & ~pte_q[1]);

  // Step 5, with the U-bit rule in front: U-mode needs U=1; S-mode needs SUM=1
  // for a load/store on a U page and can never fetch from one; the class's own
  // permission bit must be set, with MXR substituting X for R on a load and R
  // for X on a fetch.
  always_comb begin
    logic u_ok;
    logic perm_ok;
    begin
      u_ok = 1'b1;
      if (priv_q == PRIV_U) begin
        u_ok = pte_q[4];
      end else if (priv_q == PRIV_S) begin
        if (pte_q[4]) begin
          if (kind_q == KIND_FETCH) u_ok = 1'b0;
          else                      u_ok = sum_q;
        end
      end
      case (kind_q)
        KIND_STORE: perm_ok = pte_q[2];
        // MXR is the specification's "Make eXecutable Readable": it lets a
        // *load* use a page whose X bit is set. It does not let a fetch use an
        // R-only page -- an instruction access needs X, whatever MXR says.
        KIND_FETCH: perm_ok = pte_q[3];
        default:    perm_ok = pte_q[1] | (mxr_q & pte_q[3]);
      endcase
`ifdef MOSAIC_PTW_MUTANT_SUM_IGNORED
      // NEGATIVE CONTROL: the SUM rule is dropped, so S-mode reads a U page
      // with SUM=0. The case's "S-mode load of a U page with SUM=0 faults"
      // check names it.
      if (priv_q == PRIV_S) u_ok = 1'b1;
`endif
      perm_fail_c = !(u_ok & perm_ok);
    end
  end

  // The A/D need. The mutant inverts it: a leaf that needs no update is the one
  // written, and one that does need it faults, which the case's A/D checks
  // split.
`ifdef MOSAIC_PTW_MUTANT_AD_INVERTED
  assign need_a_c = pte_q[6];
  assign need_d_c = (~(kind_q == KIND_STORE)) & pte_q[7];
`else
  assign need_a_c = ~pte_q[6];
  assign need_d_c = (kind_q == KIND_STORE) & ~pte_q[7];
`endif

  // Step 7's new value: A is always set; D only when the access is a store
  // ("set pte.a to 1 and, if the original memory access is a store, also set
  // pte.d to 1").
  assign pte_new_c = pte_q | (need_d_c ? 64'h0000_0000_0000_00C0
                                      : 64'h0000_0000_0000_0040);

  // Request-time copies of the two decisions ST_IDLE makes *before* the request
  // is latched: the canonicality test and the fault cause both depend on the
  // request being presented, not on the latched copy of the previous walk.
  logic        canonical_req_c;
  logic [3:0]  page_cause_req_c;
  always_comb begin
    canonical_req_c = (xl_va_i[63:39] == {25{xl_va_i[38]}});
    case (xl_kind_i)
      KIND_STORE: page_cause_req_c = 4'd15;
      KIND_FETCH: page_cause_req_c = 4'd12;
      default:    page_cause_req_c = 4'd13;
    endcase
  end

  // ==========================================================================
  // The output registers and the port
  // ==========================================================================
  assign xl_req_ready_o = (state_q == ST_IDLE);
  assign xl_rsp_valid_o = rsp_valid_q;
  assign xl_pa_o        = pa_q;
  assign xl_fault_o     = fault_q;
  assign xl_cause_o     = cause_q;
  assign xl_tval_o      = tval_q;
  assign xl_perms_o     = perms_q;
  assign xl_bare_o      = bare_q;

  // A read is offered while the read states still need their beat; a write while
  // `ST_AD_WRITE` needs its. `beat_sent_q` is the "the memory took it" latch, so
  // the payload is stable from offer to acceptance.
  assign pte_req_valid_o = !cancel_pending_q & !beat_sent_q &
                           ((state_q == ST_READ) | (state_q == ST_AD_READ) |
                            (state_q == ST_AD_WRITE));
  assign pte_req_we_o    = (state_q == ST_AD_WRITE);
  assign pte_req_addr_o  = pte_addr_c;
  assign pte_req_wdata_o = pte_new_c;
  assign pte_req_wstrb_o = 8'hFF;

  // The response is always taken: the walker owns the single outstanding beat
  // and has nowhere else to put it.
  assign pte_rsp_ready_o = 1'b1;

  assign o_busy             = (state_q != ST_IDLE);
  assign o_walk_ctr         = walk_ctr_q;
  assign o_bare_ctr         = bare_ctr_q;
  assign o_leaf_ctr         = leaf_ctr_q;
  assign o_fault_ctr        = fault_ctr_q;
  assign o_ad_upd_ctr       = ad_upd_ctr_q;
  assign o_retry_ctr        = retry_ctr_q;
  assign o_cancel_ctr       = cancel_ctr_q;
  assign o_last_fault_cause = last_fault_cause_q;
  assign o_last_fault_tval  = last_fault_tval_q;

  // ==========================================================================
  // The state machine
  // ==========================================================================
  always_ff @(posedge clk) begin
    if (rst) begin
      state_q          <= ST_IDLE;
      beat_sent_q      <= 1'b0;
      cancel_pending_q <= 1'b0;
      rsp_valid_q      <= 1'b0;
      fault_q          <= 1'b0;
      cause_q          <= 4'd0;
      tval_q           <= 64'd0;
      pa_q             <= 64'd0;
      perms_q          <= 4'd0;
      bare_q           <= 1'b0;
      level_q          <= 2'd2;
      table_ppn_q      <= 44'd0;
      va_q             <= 64'd0;
      kind_q           <= KIND_LOAD;
      priv_q           <= PRIV_S;
      sum_q            <= 1'b0;
      mxr_q            <= 1'b0;
      pte_q            <= 64'd0;
      pte_ppn_q        <= 44'd0;
      retry_q          <= 3'd0;
      walk_ctr_q       <= 32'd0;
      bare_ctr_q       <= 32'd0;
      leaf_ctr_q       <= 32'd0;
      fault_ctr_q      <= 32'd0;
      ad_upd_ctr_q     <= 32'd0;
      retry_ctr_q      <= 32'd0;
      cancel_ctr_q     <= 32'd0;
      last_fault_cause_q <= 64'd0;
      last_fault_tval_q  <= 64'd0;
    end else begin
      // The response leaves only when the requester takes it. Deliberately
      // outside the state case so a new request cannot overwrite a held result.
      if (rsp_valid_q && xl_rsp_ready_i) begin
        rsp_valid_q <= 1'b0;
      end

      case (state_q)
        // ------------------------------------------------------------------
        ST_IDLE: begin
          cancel_pending_q <= 1'b0;
          if (xl_req_valid_i) begin
            va_q       <= xl_va_i;
            kind_q     <= xl_kind_i;
            priv_q     <= xl_priv_i;
            sum_q      <= xl_sum_i;
            mxr_q      <= xl_mxr_i;
            tval_q     <= xl_va_i;
            perms_q    <= 4'd0;
            walk_ctr_q <= walk_ctr_q + 32'd1;
            retry_q    <= 3'd0;

            if ((xl_satp_mode_i == SATP_MODE_BARE) || (xl_priv_i == PRIV_M)) begin
              // Bare (or an M-mode access, which is not translated): the virtual
              // address *is* the physical address and every access is permitted.
              // No PTE is read.
              pa_q        <= xl_va_i;
              fault_q     <= 1'b0;
              cause_q     <= 4'd0;
              perms_q     <= 4'hF;
              bare_q      <= 1'b1;
              rsp_valid_q <= 1'b1;
              bare_ctr_q  <= bare_ctr_q + 32'd1;
              state_q     <= ST_DONE;
            end else if (xl_satp_mode_i != SATP_MODE_SV39) begin
              // Unreachable through the CSR, which canonicalises MODE to {0,8};
              // a defensive fault rather than a silent Bare.
              pa_q        <= 64'd0;
              fault_q     <= 1'b1;
              cause_q     <= page_cause_req_c;
              bare_q      <= 1'b0;
              rsp_valid_q <= 1'b1;
              fault_ctr_q <= fault_ctr_q + 32'd1;
              last_fault_cause_q <= {60'd0, page_cause_req_c};
              last_fault_tval_q  <= xl_va_i;
              state_q     <= ST_DONE;
            end else begin
`ifndef MOSAIC_PTW_MUTANT_ALLOW_NONCANONICAL
              if (!canonical_req_c) begin
                // Step 1's canonicality test, before any physical access.
                pa_q        <= 64'd0;
                fault_q     <= 1'b1;
                cause_q     <= page_cause_req_c;
                bare_q      <= 1'b0;
                rsp_valid_q <= 1'b1;
                fault_ctr_q <= fault_ctr_q + 32'd1;
                last_fault_cause_q <= {60'd0, page_cause_req_c};
                last_fault_tval_q  <= xl_va_i;
                state_q     <= ST_DONE;
              end else begin
                level_q     <= 2'd2;
                table_ppn_q <= xl_satp_ppn_i;
                beat_sent_q <= 1'b0;
                state_q     <= ST_READ;
              end
`else
              // NEGATIVE CONTROL: the canonicality test is dropped, so a
              // non-canonical address enters the walk and reaches the page
              // table. The case's non-canonical checks name it.
              level_q     <= 2'd2;
              table_ppn_q <= xl_satp_ppn_i;
              beat_sent_q <= 1'b0;
              state_q     <= ST_READ;
`endif
            end
          end
        end

        // ------------------------------------------------------------------
        // Step 2: offer the PTE read and latch its result.
        ST_READ: begin
          if (xl_cancel_i) begin
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            state_q      <= ST_IDLE;
          end else if (!beat_sent_q) begin
            if (pte_req_ready_i) beat_sent_q <= 1'b1;
          end else if (pte_rsp_valid_i) begin
            beat_sent_q <= 1'b0;
            pte_q       <= pte_rsp_rdata_i;
            pte_ppn_q   <= pte_rsp_rdata_i[53:10];
            if (pte_rsp_fault_i) begin
              // Step 2's refusal: an access fault, not a page fault.
              pa_q        <= 64'd0;
              fault_q     <= 1'b1;
              cause_q     <= access_cause_c;
              bare_q      <= 1'b0;
              rsp_valid_q <= 1'b1;
              fault_ctr_q <= fault_ctr_q + 32'd1;
              last_fault_cause_q <= {60'd0, access_cause_c};
              last_fault_tval_q  <= va_q;
              state_q     <= ST_DONE;
            end else begin
              state_q <= ST_EVAL;
            end
          end
        end

        // ------------------------------------------------------------------
        // Steps 3-7 on the PTE just read.
        ST_EVAL: begin
          if (xl_cancel_i) begin
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            state_q      <= ST_IDLE;
          end else if (!pte_q[0] | reserved_c) begin
`ifdef MOSAIC_PTW_MUTANT_PTE_ERROR_OK
            // NEGATIVE CONTROL: an invalid or reserved PTE is accepted as a
            // mapping to physical zero. The case's V=0 and reserved-encoding
            // checks name it.
            state_q <= ST_COMPOSE;
`else
            // Step 3.
            pa_q        <= 64'd0;
            fault_q     <= 1'b1;
            cause_q     <= page_cause_c;
            bare_q      <= 1'b0;
            rsp_valid_q <= 1'b1;
            fault_ctr_q <= fault_ctr_q + 32'd1;
            last_fault_cause_q <= {60'd0, page_cause_c};
            last_fault_tval_q  <= va_q;
            state_q     <= ST_DONE;
`endif
          end else if (leaf_c) begin
            leaf_ctr_q <= leaf_ctr_q + 32'd1;
            if (perm_fail_c) begin
              // Step 5.
              pa_q        <= 64'd0;
              fault_q     <= 1'b1;
              cause_q     <= page_cause_c;
              bare_q      <= 1'b0;
              rsp_valid_q <= 1'b1;
              fault_ctr_q <= fault_ctr_q + 32'd1;
              last_fault_cause_q <= {60'd0, page_cause_c};
              last_fault_tval_q  <= va_q;
              state_q     <= ST_DONE;
            end else if (align_bad_c) begin
`ifdef MOSAIC_PTW_MUTANT_SUPERPAGE_UNALIGNED
              // NEGATIVE CONTROL: the misaligned-superpage test is dropped, so
              // the low VPN bits are taken from the virtual address and the
              // access is served from the wrong physical page. The case's
              // misaligned-superpage check names it.
              state_q <= ST_COMPOSE;
`else
              // Step 6.
              pa_q        <= 64'd0;
              fault_q     <= 1'b1;
              cause_q     <= page_cause_c;
              bare_q      <= 1'b0;
              rsp_valid_q <= 1'b1;
              fault_ctr_q <= fault_ctr_q + 32'd1;
              last_fault_cause_q <= {60'd0, page_cause_c};
              last_fault_tval_q  <= va_q;
              state_q     <= ST_DONE;
`endif
            end else if (need_a_c | need_d_c) begin
              // Step 7: the compare-and-set. Re-read the PTE; the write is
              // issued only from ST_AD_WRITE, after the compare.
              state_q <= ST_AD_READ;
            end else begin
              state_q <= ST_COMPOSE;
            end
          end else if (level_q == 2'd0) begin
            // Step 4's end: a non-leaf at the last level.
            pa_q        <= 64'd0;
            fault_q     <= 1'b1;
            cause_q     <= page_cause_c;
            bare_q      <= 1'b0;
            rsp_valid_q <= 1'b1;
            fault_ctr_q <= fault_ctr_q + 32'd1;
            last_fault_cause_q <= {60'd0, page_cause_c};
            last_fault_tval_q  <= va_q;
            state_q     <= ST_DONE;
          end else begin
            level_q     <= level_q - 2'd1;
            table_ppn_q <= pte_ppn_q;
            state_q     <= ST_READ;
          end
        end

        // ------------------------------------------------------------------
        // Step 7's compare: read the PTE again and require the same value.
        ST_AD_READ: begin
          if (xl_cancel_i) begin
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            state_q      <= ST_IDLE;
          end else if (!beat_sent_q) begin
            if (pte_req_ready_i) beat_sent_q <= 1'b1;
          end else if (pte_rsp_valid_i) begin
            beat_sent_q <= 1'b0;
            if (pte_rsp_fault_i) begin
              pa_q        <= 64'd0;
              fault_q     <= 1'b1;
              cause_q     <= access_cause_c;
              bare_q      <= 1'b0;
              rsp_valid_q <= 1'b1;
              fault_ctr_q <= fault_ctr_q + 32'd1;
              last_fault_cause_q <= {60'd0, access_cause_c};
              last_fault_tval_q  <= va_q;
              state_q     <= ST_DONE;
            end else if (pte_rsp_rdata_i != pte_q) begin
              // The PTE changed under the walk. Re-evaluate the fresh value
              // rather than writing a stale update over it; the bound keeps the
              // loop finite.
              retry_ctr_q <= retry_ctr_q + 32'd1;
              pte_q       <= pte_rsp_rdata_i;
              pte_ppn_q   <= pte_rsp_rdata_i[53:10];
              if (retry_q == RETRY_LIMIT) begin
                // Bounded: accept what was just read without further update.
                retry_q <= 3'd0;
                state_q <= ST_COMPOSE;
              end else begin
                retry_q <= retry_q + 3'd1;
                state_q <= ST_EVAL;
              end
            end else begin
              state_q <= ST_AD_WRITE;
            end
          end
        end

        // ------------------------------------------------------------------
        // Step 7's update: write the PTE back with A (and D) set.
        ST_AD_WRITE: begin
          if (xl_cancel_i) begin
            // A cancel before the write is offered leaves no update behind. If
            // the write is already on the port it completes (the memory owns
            // it); the report states this explicitly.
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            if (beat_sent_q) cancel_pending_q <= 1'b1;
            else             state_q <= ST_IDLE;
          end else if (!beat_sent_q) begin
            if (pte_req_ready_i) begin
              beat_sent_q  <= 1'b1;
              ad_upd_ctr_q <= ad_upd_ctr_q + 32'd1;
            end
          end else if (pte_rsp_valid_i) begin
            beat_sent_q <= 1'b0;
            if (pte_rsp_fault_i) begin
              // Step 7's own rule: a refused PTE update is an access fault.
              pa_q        <= 64'd0;
              fault_q     <= 1'b1;
              cause_q     <= access_cause_c;
              bare_q      <= 1'b0;
              rsp_valid_q <= 1'b1;
              fault_ctr_q <= fault_ctr_q + 32'd1;
              last_fault_cause_q <= {60'd0, access_cause_c};
              last_fault_tval_q  <= va_q;
              state_q     <= ST_DONE;
            end else begin
              pte_q     <= pte_new_c;
              pte_ppn_q <= pte_new_c[53:10];
              state_q   <= ST_COMPOSE;
            end
          end
        end

        // ------------------------------------------------------------------
        // Step 8: compose the physical address and present the result.
        ST_COMPOSE: begin
          pa_q        <= {8'd0, ppn_eff_c, va_q[11:0]};
          fault_q     <= 1'b0;
          cause_q     <= 4'd0;
          perms_q     <= {pte_q[3], pte_q[2], pte_q[1], pte_q[4]};
          bare_q      <= 1'b0;
          rsp_valid_q <= 1'b1;
          state_q     <= ST_DONE;
        end

        // ------------------------------------------------------------------
        ST_DONE: begin
          // Hold the result. A cancel here drops it: the requester that
          // cancelled is not waiting for an answer.
          if (xl_cancel_i) begin
            cancel_ctr_q <= cancel_ctr_q + 32'd1;
            rsp_valid_q  <= 1'b0;
            state_q      <= ST_IDLE;
          end else if (rsp_valid_q && xl_rsp_ready_i) begin
            state_q <= ST_IDLE;
          end
        end

        default: state_q <= ST_IDLE;
      endcase

      // A cancel that arrived while a beat was outstanding ends the walk the
      // moment the beat completes: the result is discarded and no further beat
      // is issued.
      if (cancel_pending_q && beat_sent_q && pte_rsp_valid_i) begin
        beat_sent_q      <= 1'b0;
        cancel_pending_q <= 1'b0;
        rsp_valid_q      <= 1'b0;
        state_q          <= ST_IDLE;
      end
    end
  end

endmodule

`endif  // MOSAIC_PTW_SV_
