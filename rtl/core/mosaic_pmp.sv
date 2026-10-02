// ============================================================================
// mosaic_pmp -- work package I-044: Physical Memory Protection.
//
// ------------------------------------------------------------------ what it is
//
// One PMP unit: the entry state (pmpcfg/pmpaddr), the CSR read/write path with
// its WARL and lock rules, and one combinational permission query. It owns no
// privilege state and takes no part in the trap: it answers "may this access
// proceed" and the caller turns a refusal into the access fault the ISA names.
//
// ------------------------------------------------------------------- geometry
//
// The entry count and the grain are a platform decision, so neither is a number
// in this file: `MOSAIC_PMP_ENTRIES` and `MOSAIC_PMP_GRAIN_BYTES` come from the
// profile's geometry file through the generated config package (Priv v1.12
// 2.7.1: "Implementations may implement zero, 16, or 64 PMP entries"; 2.7.1.1:
// "the PMP grain is 2^(G+2) bytes"). A profile with no `pmp` block implements
// zero entries, and then every query is allowed and no CSR address selects this
// unit -- which is the correct description of an M-only hart with nothing below
// it to protect. That case is a generate branch, not a degenerate engine: the
// entries, the counters and the matching logic do not exist in that build.
//
// ------------------------------------------------------------- the rules, and
// ------------------------------------------------------------- where they are
//
// Every rule below is quoted from the ratified privileged specification at the
// tag the CSR tables cite (Priv v1.12, commit 98964261...), section 2.7.1:
//
//   * A field, Table 10: 0 OFF, 1 TOR, 2 NA4, 3 NAPOT. "When A=0, this PMP
//     entry is disabled and matches no addresses."
//   * NAPOT size, Table 7: the number of trailing ones n in pmpaddr encodes a
//     2^(n+3)-byte naturally aligned region, so the match compares the address
//     with the entry above bit n. The all-ones entry therefore covers the whole
//     address space, which the mask arithmetic below produces on its own.
//   * TOR: "the entry matches any address y such that pmpaddr_(i-1) <= y <
//     pmpaddr_i (irrespective of the value of pmpcfg_(i-1))". Entry 0's lower
//     bound is zero. "If pmpaddr_(i-1) >= pmpaddr_i and pmpcfg_i.A=TOR, then
//     PMP entry i matches no addresses", which falls out of the same comparison.
//   * Priority: "The lowest-numbered PMP entry that matches any byte of an
//     access determines whether that access succeeds or fails. The matching PMP
//     entry must match all bytes of an access, or the access fails, irrespective
//     of the L, R, W, and X bits."
//   * Privilege: "If the L bit is clear and the privilege mode of the access is
//     M, the access succeeds. Otherwise, if the L bit is set or the privilege
//     mode of the access is S or U, then the access succeeds only if the R, W,
//     or X bit corresponding to the access type is set." And the default: "If no
//     PMP entry matches an M-mode access, the access succeeds. If no PMP entry
//     matches an S-mode or U-mode access, but at least one PMP entry is
//     implemented, the access fails."
//   * Locking: "If PMP entry i is locked, writes to pmp_i cfg and pmpaddr_i are
//     ignored. Additionally, if PMP entry i is locked and pmp_i cfg.A is set to
//     TOR, writes to pmpaddr_(i-1) are ignored." And "Setting the L bit locks the
//     PMP entry even when the A field is set to OFF."
//   * WARL: "All PMP CSR fields are WARL and may be read-only zero"; bits 6:5 of
//     each entry byte are reserved and read zero; "The R, W, and X fields form a
//     collective WARL field for which the combinations with R=0 and W=1 are
//     reserved", so a write of W=1,R=0 is canonicalised to W=1,R=1 -- a legal
//     value that keeps the write the software asked for.
//   * Grain: "When G >= 1, the NA4 mode is not selectable." With the delivered
//     grain of 4 bytes G is 0 and NA4 is selectable; the G >= 1 rules are
//     implemented as well, because the grain is a knob and a knob that is not
//     implemented is not a knob.
//
// --------------------------------------------------- why first byte and last
//
// "The matching PMP entry must match all bytes" is checked on the first and the
// last byte of the access. That is exact for the shapes this unit can meet: an
// access is at most eight bytes and every region is an interval of at least four
// bytes, so a region that intersects the access contains one of its ends -- there
// is no region that hides strictly inside one. The module therefore needs no
// per-byte loop, and this comment says so rather than leaving the shorter check
// to look like an oversight.
//
// ------------------------------------------------------------------- mutants
//
// -DMOSAIC_PMP_MUTANT_<n> injects exactly one defect; the shipping build defines
// none of them. The table with real output is in results/reports/I-044-privilege.md.
//
//   LOCK_IGNORED      a locked entry's configuration byte is overwritten anyway
//   OVERLAP_INVERTED  the *highest*-numbered matching entry decides, instead of
//                     the lowest, so an overlapping pair resolves the other way
//   M_MODE_ENFORCED   an unlocked entry's permissions are enforced against an
//                     M-mode access, which the specification permits only for a
//                     locked entry
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_pkg.sv"
/* verilator lint_on UNUSEDPARAM */

// The generated implementation table: the CSR numbers this unit answers for.
// The addresses are fixed by the ISA and the entry count is the profile's, so
// both arrive from the generator rather than from a literal here.
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_csr_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

// A profile with no PMP entries elaborates only the tie-off branch below, in
// which the query and CSR ports have nothing to reach. That warning is scoped to
// the port list and that one branch and re-enabled before the real engine, so a
// build that does implement entries is still held to the full check.
/* verilator lint_off UNUSEDSIGNAL */

module mosaic_pmp (
    input  logic                     clk_i,
    input  logic                     rst_i,

    // ---------------------------------------------------- CSR access (M-mode)
    // The CSR file owns the decision "this address is a PMP register and this
    // access is legal"; it forwards the address and the operation's already
    // applied write operand. The read port answers combinationally.
    input  logic [11:0]              csr_addr_i,
    output logic [63:0]              csr_rdata_o,
    input  logic                     csr_we_i,
    input  logic [63:0]              csr_wdata_i,

    // ------------------------------------------------------- permission query
    // One access: the lowest byte address, how many bytes it covers, which of
    // R/W/X it is, and the *effective* privilege mode (the caller has already
    // applied mstatus.MPRV, which this unit knows nothing about).
    input  logic [63:0]              req_addr_i,
    input  logic [3:0]               req_bytes_i,
    input  logic                     req_r_i,
    input  logic                     req_w_i,
    input  logic                     req_x_i,
    input  logic [1:0]               req_priv_i,
    output logic                     allow_o,
    output logic                     matched_o,
    output logic                     locked_o,
    // The instruction side asks the same question with X permission. It is a
    // second port rather than a shared one because a fetch and a data access can
    // be offered in the same cycle, and a permission unit that could only answer
    // one of them would stall the other for no architectural reason.
    input  logic [63:0]              f_req_addr_i,
    input  logic [3:0]               f_req_bytes_i,
    input  logic [1:0]               f_req_priv_i,
    output logic                     f_allow_o,
    output logic                     f_matched_o,
    output logic                     f_locked_o,
    // ------------------------------------------------- store-commit query (D5)
    // A store must be checked against the PMP at the point it is *authorised to
    // retire*, not when it drains to the endpoint: the endpoint's refusal
    // arrives after retirement and can no longer be taken as the store's own
    // exception. The check is therefore non-speculative -- it is asked in the
    // cycle the ROB is about to commit the store, when every older instruction
    // has retired and the CSR state (including the PMP entries and mstatus.MPRV)
    // is committed. The ROB commits at most two instructions per cycle and both
    // can be stores, so the question is asked for both lanes together; a single
    // engine answers with the lowest-numbered matching entry for each, exactly
    // as it does for the data and fetch ports. Both accesses are store-class, so
    // they are checked for W only (R = X = 0), matching the module's own class
    // rule above.
    input  logic [63:0]              sc_req_addr0_i,
    input  logic [3:0]               sc_req_bytes0_i,
    input  logic [1:0]               sc_req_priv0_i,
    input  logic [63:0]              sc_req_addr1_i,
    input  logic [3:0]               sc_req_bytes1_i,
    input  logic [1:0]               sc_req_priv1_i,
    output logic                     sc_allow0_o,
    output logic                     sc_allow1_o,

    // ------------------------------------------------------------ observability
    output logic [31:0]              o_query_ctr,
    output logic [31:0]              o_deny_ctr,
    output logic [31:0]              o_locked_ctr,
    output logic [31:0]              o_fetch_deny_ctr,
    output logic [mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_CFG_W-1:0]  o_entry_cfg_o,
    output logic [mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_ADDR_W-1:0] o_entry_addr_o
);

  localparam int unsigned NENT  = mosaic_cfg_pkg::MOSAIC_PMP_ENTRIES;
  localparam int unsigned NCFG  = mosaic_cfg_pkg::MOSAIC_PMP_CFG_COUNT;
  localparam int unsigned G     = mosaic_cfg_pkg::MOSAIC_PMP_G;
  localparam int unsigned OCW   = mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_CFG_W;
  localparam int unsigned OAW   = mosaic_cfg_pkg::MOSAIC_PMP_ENTRY_ADDR_W;

  generate
    if (NENT == 0) begin : g_no_pmp
      // No entries are implemented. "If no PMP entry matches an S-mode or U-mode
      // access, but at least one PMP entry is implemented, the access fails" --
      // with none implemented, nothing fails, and no CSR number selects this
      // unit, so every PMP CSR is an illegal instruction (which the CSR file's
      // own implementation table decides, not this module).
      //
      // A query and a CSR access still arrive, because the core and the CSR file
      // are the same in every profile; there is simply nothing here for them to
      // reach, which is why the inputs are unused in this branch and the
      // warning is silenced here rather than at the ports.
      /* verilator lint_off UNUSEDSIGNAL */
      assign csr_rdata_o    = 64'd0;
      assign allow_o        = 1'b1;
      assign matched_o      = 1'b0;
      assign locked_o       = 1'b0;
      assign f_allow_o      = 1'b1;
      assign f_matched_o    = 1'b0;
      assign f_locked_o     = 1'b0;
      assign sc_allow0_o    = 1'b1;
      assign sc_allow1_o    = 1'b1;
      assign o_query_ctr    = 32'd0;
      assign o_deny_ctr     = 32'd0;
      assign o_locked_ctr   = 32'd0;
      assign o_fetch_deny_ctr = 32'd0;
      assign o_entry_cfg_o  = 1'b0;
      assign o_entry_addr_o = 1'b0;
    end else begin : g_pmp
      /* verilator lint_on UNUSEDSIGNAL */
      localparam int unsigned NSLOT = NENT;

      logic [7:0]  cfg_q  [0:NSLOT-1];
      logic [63:0] addr_q [0:NSLOT-1];
      logic [7:0]  cfg_d  [0:NSLOT-1];
      logic [63:0] addr_d [0:NSLOT-1];

      // A byte address expressed in the units pmpaddr uses: bits [55:2] of the
      // physical address, zero-extended to the register width. Bits [1:0] are
      // absent deliberately: the grain is never finer than four bytes, so all
      // four bytes of one aligned word lie in the same PMP region, and a
      // byte-level test inside the word could not distinguish anything.
      /* verilator lint_off UNUSEDSIGNAL */
      function automatic logic [63:0] addr_field(input logic [63:0] byte_addr);
        begin
          addr_field = {2'b00, byte_addr[63:2]};
        end
      endfunction
      /* verilator lint_on UNUSEDSIGNAL */

      // The trailing-ones count that encodes a NAPOT region's size (Table 7).
      function automatic logic [6:0] trailing_ones(input logic [63:0] value);
        logic done;
        begin
          trailing_ones = 7'd0;
          done          = 1'b0;
          for (int k = 0; k < 64; k++) begin
            if (!done) begin
              if (value[k]) trailing_ones = k[6:0] + 7'd1;
              else          done          = 1'b1;
            end
          end
        end
      endfunction

      // Does entry i match one byte at `byte_addr`?
      function automatic logic byte_match(input int unsigned i, input logic [63:0] byte_addr);
        logic [63:0] y;
        logic [63:0] mask;
        logic [6:0]  ones;
        begin
          y = addr_field(byte_addr);
          case (cfg_q[i][4:3])
            2'b00: byte_match = 1'b0;                                    // OFF
            2'b10: byte_match = (y == addr_q[i]);                        // NA4
            2'b11: begin                                                 // NAPOT
              ones = trailing_ones(addr_q[i]);
              // n trailing ones -> a 2^(n+3)-byte region: the address bits at
              // and below bit n are free, everything above must agree. `mask`
              // holds exactly the bits that must agree, so the comparison is
              // against `mask` and not against its complement: comparing the
              // *free* bits would match only addresses whose low bits happened
              // to equal the entry's trailing-ones pattern -- one word of the
              // region -- and CASE=privilege.permission_matrix found exactly
              // that, as an S-mode fetch inside the code region being refused.
              //
              // The all-ones entry shifts a 64-bit vector by 64, which
              // SystemVerilog defines as zero, so `mask` is zero, every address
              // agrees on no bits at all, and the entry matches the whole space
              // with no special case.
              mask = (~64'd0) << (ones + 7'd1);
              byte_match = ((y & mask) == (addr_q[i] & mask));
            end
            default: begin                                               // TOR
              byte_match = (y >= ((i == 0) ? 64'd0 : addr_q[i-1])) && (y < addr_q[i]);
            end
          endcase
        end
      endfunction

      // ------------------------------------------------------------ the query
      //
      // One question, asked twice per cycle at most: given an access and the
      // effective privilege, does the lowest-numbered matching entry permit it?
      // The answer is a packed record rather than three separate signals so the
      // search runs once per call and the two callers cannot disagree about
      // which entry matched.
      //
      // "The matching PMP entry must match all bytes of an access, or the access
      // fails" is checked on the first and the last byte. That is exact for the
      // shapes this unit can meet: an access is at most eight bytes and every
      // region is an interval of at least four bytes, so a region that
      // intersects the access contains one of its ends -- there is no region
      // that hides strictly inside one.
      typedef struct packed {
        logic allow;
        logic matched;
        logic locked;
      } pmp_query_t;

      function automatic pmp_query_t pmp_query(input logic [63:0] addr,
                                               input logic [3:0]  bytes,
                                               input logic        r,
                                               input logic        w,
                                               input logic        x,
                                               input logic [1:0]  priv);
        logic [63:0]      last;
        logic [NSLOT-1:0] first_onehot;
        logic [NSLOT-1:0] last_onehot;
        logic             have_first;
        logic             locked_sel;
        logic             allows_sel;
        pmp_query_t       result;
        begin
          last        = addr + {{60{1'b0}}, bytes} - 64'd1;
          first_onehot = {NSLOT{1'b0}};
          last_onehot  = {NSLOT{1'b0}};
          have_first   = 1'b0;
          for (int unsigned i = 0; i < NSLOT; i++) begin
            // The first set slot in index order is the lowest-numbered match;
            // later iterations must not overwrite it, which is what the
            // `!have_first` guard does.
            if (!have_first && byte_match(i, addr)) begin
              first_onehot[i] = 1'b1;
              have_first      = 1'b1;
            end
            if (byte_match(i, last)) begin
              last_onehot[i] = 1'b1;
            end
          end

          locked_sel = 1'b0;
          allows_sel = 1'b0;
          for (int unsigned i = 0; i < NSLOT; i++) begin
            if (first_onehot[i]) begin
              locked_sel = cfg_q[i][7];
              allows_sel = (r && cfg_q[i][0]) || (w && cfg_q[i][1]) || (x && cfg_q[i][2]);
            end
          end

          result.allow   = 1'b0;
          result.matched = 1'b0;
          result.locked  = locked_sel;
          if (!have_first) begin
            // "If no PMP entry matches an M-mode access, the access succeeds. If
            // no PMP entry matches an S-mode or U-mode access, but at least one
            // PMP entry is implemented, the access fails."
            result.allow = (priv == mosaic_csr_pkg::MOSAIC_PRIV_M);
          end else begin
            result.matched = 1'b1;
            // The governing entry is the lowest-numbered one that matched any
            // byte, and it must also be the one that matched the last byte,
            // which means the same entry matched both ends and -- every region
            // being an interval -- everything between them.
            if (last_onehot != first_onehot) begin
              result.allow = 1'b0;
            end else if (!locked_sel && (priv == mosaic_csr_pkg::MOSAIC_PRIV_M)) begin
`ifdef MOSAIC_PMP_MUTANT_M_MODE_ENFORCED
              // MUTANT: an M-mode access is checked against the entry's R/W/X
              // even when the entry is not locked. The specification allows that
              // only for a locked entry ("When the L bit is clear, any M-mode
              // access matching the PMP entry will succeed"), so a scenario that
              // runs an M-mode access through an unlocked deny entry faults
              // where it must not.
              result.allow = allows_sel;
`else
              result.allow = 1'b1;
`endif
            end else begin
              result.allow = allows_sel;
            end
          end

`ifdef MOSAIC_PMP_MUTANT_OVERLAP_INVERTED
          // MUTANT: the *highest*-numbered matching entry decides. The
          // lowest-numbered-wins rule is what makes an overlapping pair of
          // regions well defined, so an entry placed behind a broader one no
          // longer has the effect it was written for.
          begin
            logic [NSLOT-1:0] hi_onehot;
            logic             hi_locked;
            logic             hi_allows;
            hi_onehot = {NSLOT{1'b0}};
            hi_locked = 1'b0;
            hi_allows = 1'b0;
            for (int unsigned i = 0; i < NSLOT; i++) begin
              if (byte_match(i, addr)) begin
                hi_onehot = {NSLOT{1'b0}};
                hi_onehot[i] = 1'b1;
              end
            end
            for (int unsigned i = 0; i < NSLOT; i++) begin
              if (hi_onehot[i]) begin
                hi_locked = cfg_q[i][7];
                hi_allows = (r && cfg_q[i][0]) || (w && cfg_q[i][1]) || (x && cfg_q[i][2]);
              end
            end
            if (|hi_onehot) begin
              result.matched = 1'b1;
              result.locked  = hi_locked;
              if (last_onehot != hi_onehot) result.allow = 1'b0;
              else if (!hi_locked && (priv == mosaic_csr_pkg::MOSAIC_PRIV_M)) result.allow = 1'b1;
              else result.allow = hi_allows;
            end
          end
`endif
          pmp_query = result;
        end
      endfunction

      pmp_query_t data_q;
      pmp_query_t fetch_q;
      pmp_query_t store_q0, store_q1;

      always_comb begin
        // The data side: a load (and a load-reserved) reads, a store, a
        // store-conditional and an AMO write. The specification names exactly
        // those classes -- "Attempting to execute a load or load-reserved
        // instruction which accesses a physical address within a PMP region
        // without read permissions raises a load access-fault exception.
        // Attempting to execute a store, store-conditional, or AMO instruction
        // which accesses a physical address within a PMP region without write
        // permissions raises a store access-fault exception" -- so an AMO is
        // checked for W and not additionally for R.
        data_q  = pmp_query(req_addr_i, req_bytes_i, req_r_i, req_w_i, req_x_i, req_priv_i);
        fetch_q = pmp_query(f_req_addr_i, f_req_bytes_i, 1'b0, 1'b0, 1'b1, f_req_priv_i);
        // The store-commit lanes: a store is checked for W alone. See the port
        // comment for why this question is asked at authorisation and not at
        // drain.
        store_q0 = pmp_query(sc_req_addr0_i, sc_req_bytes0_i, 1'b0, 1'b1, 1'b0, sc_req_priv0_i);
        store_q1 = pmp_query(sc_req_addr1_i, sc_req_bytes1_i, 1'b0, 1'b1, 1'b0, sc_req_priv1_i);
        allow_o    = data_q.allow;
        matched_o  = data_q.matched;
        locked_o   = data_q.locked;
        f_allow_o  = fetch_q.allow;
        f_matched_o = fetch_q.matched;
        f_locked_o = fetch_q.locked;
        sc_allow0_o    = store_q0.allow;
        sc_allow1_o    = store_q1.allow;
      end

      // ----------------------------------------------------------------- reads
      //
      // The read of pmpaddr_i depends on pmpcfg_i.A: with a grain above four
      // bytes, "When G >= 2 and pmpcfg_i.A[1] is set, i.e. the mode is NAPOT,
      // then bits pmpaddr_i[G-2:0] read as all ones. When G >= 1 and
      // pmpcfg_i.A[1] is clear, i.e. the mode is OFF or TOR, then bits
      // pmpaddr_i[G-1:0] read as all zeros." The *stored* value is not changed
      // by that: it is the read that is masked, and bit [G-1] therefore still
      // reads its stored value in NAPOT mode.
      logic        cfg_sel_c;
      logic        addr_sel_c;
      logic [63:0] cfg_rdata_c;
      logic [63:0] addr_rdata_c;
      logic [11:0] cfg_off_c;
      logic [11:0] addr_off_c;
      logic [63:0] grain_low_mask_c;
      logic [63:0] napot_low_mask_c;
      localparam logic [11:0] CFG_SPAN  = 12'(2 * NCFG);
      localparam logic [11:0] ADDR_SPAN = 12'(NENT);

      // The bits of pmpaddr_i the grain hides on a read. With G == 0 both masks
      // are zero and the register reads back exactly what was stored.
      always_comb begin
        grain_low_mask_c = (G == 0) ? 64'd0 : ((64'd1 << 6'(G)) - 64'd1);
        napot_low_mask_c = (G < 2)  ? 64'd0 : ((64'd1 << 6'(G - 1)) - 64'd1);
      end

      // Is `addr` inside `span` CSR numbers starting at `base`? Expressed as a
      // subtraction so both sides of the comparison are 12-bit CSR numbers and
      // no width is silently extended.
      function automatic logic off_in_range(input logic [11:0] addr,
                                            input logic [11:0] base,
                                            input logic [11:0] span);
        begin
          off_in_range = (addr >= base) && ((addr - base) < span);
        end
      endfunction

      always_comb begin
        // A 12-bit offset from each base, so every comparison below is between
        // two 12-bit values and the width is the CSR number's own.
        cfg_off_c  = csr_addr_i - mosaic_cfg_pkg::MOSAIC_PMPCFG_ADDR_BASE;
        addr_off_c = csr_addr_i - mosaic_cfg_pkg::MOSAIC_PMPADDR_ADDR_BASE;
        cfg_sel_c  = (csr_addr_i[0] == 1'b0) &&
                     off_in_range(csr_addr_i, mosaic_cfg_pkg::MOSAIC_PMPCFG_ADDR_BASE, CFG_SPAN);
        addr_sel_c = off_in_range(csr_addr_i, mosaic_cfg_pkg::MOSAIC_PMPADDR_ADDR_BASE, ADDR_SPAN);

        cfg_rdata_c = 64'd0;
        for (int unsigned k = 0; k < NCFG; k++) begin
          if (cfg_off_c == 12'(2 * k)) begin
            for (int unsigned j = 0; j < 8; j++) begin
              if ((8 * k + j) < NENT) begin
                cfg_rdata_c[8*j +: 8] = cfg_q[8*k + j];
              end
            end
          end
        end

        addr_rdata_c = 64'd0;
        for (int unsigned i = 0; i < NENT; i++) begin
          if (addr_off_c == 12'(i)) begin
            // Bits [63:54] read zero by construction: they are never stored.
            addr_rdata_c = cfg_q[i][4]
                           ? (addr_q[i] | napot_low_mask_c)    // NAPOT: [G-2:0] read ones
                           : (addr_q[i] & ~grain_low_mask_c);  // OFF/TOR: [G-1:0] read zeros
          end
        end
      end

      assign csr_rdata_o = cfg_sel_c ? cfg_rdata_c : (addr_sel_c ? addr_rdata_c : 64'd0);

      // ---------------------------------------------------------------- writes
      //
      // The write is offered only when the CSR file has accepted it, so nothing
      // here decides legality; what it decides is the WARL canonical value and
      // whether the lock rule lets the write land at all.
      logic [7:0] next_byte_c;
      logic       cfg_byte_locked_c;
      logic       addr_wr_blocked_c;

      always_comb begin
        // Default: hold. Every entry is copied, so an entry no write addresses
        // keeps its value and the register only ever changes where a rule lets
        // it.
        next_byte_c       = 8'd0;
        cfg_byte_locked_c = 1'b0;
        addr_wr_blocked_c = 1'b0;
        for (int unsigned i = 0; i < NSLOT; i++) begin
          cfg_d[i]  = cfg_q[i];
          addr_d[i] = addr_q[i];
        end

        // A write to pmpcfg_k is per entry byte: a byte whose entry is locked
        // keeps its value while its unlocked neighbours take the new one.
        if (cfg_sel_c && csr_we_i) begin
          for (int unsigned k = 0; k < NCFG; k++) begin
            if (cfg_off_c == 12'(2 * k)) begin
              for (int unsigned j = 0; j < 8; j++) begin
                if ((8 * k + j) < NENT) begin
                  cfg_byte_locked_c = cfg_q[8*k + j][7];
`ifdef MOSAIC_PMP_MUTANT_LOCK_IGNORED
                  // MUTANT: the L bit does not protect the byte, so a locked
                  // entry can be reconfigured. A scenario that locks an entry
                  // and then writes it again changes the machine's permission,
                  // and the later access the entry was meant to forbid succeeds.
                  cfg_byte_locked_c = 1'b0;
`endif
                  if (!cfg_byte_locked_c) begin
                    next_byte_c      = csr_wdata_i[8*j +: 8];
                    next_byte_c[6:5] = 2'b00;
                    if (G >= 1) begin
                      // "When G >= 1, the NA4 mode is not selectable": a write of
                      // NA4 lands on the nearest supported mode, NAPOT, rather
                      // than reading back as a value the implementation does not
                      // support.
                      if (next_byte_c[4:3] == 2'b10) next_byte_c[4:3] = 2'b11;
                    end
                    if (next_byte_c[1] && !next_byte_c[0]) begin
                      // "The combinations with R=0 and W=1 are reserved": keep
                      // the write's intent by granting the read it implies.
                      next_byte_c[0] = 1'b1;
                    end
                    cfg_d[8*k + j] = next_byte_c;
                  end
                end
              end
            end
          end
        end

        // A write to pmpaddr_i is ignored when pmpcfg_i is locked, and also when
        // the *next* entry is locked with A=TOR, because that entry's lower
        // bound is this register.
        if (addr_sel_c && csr_we_i) begin
          for (int unsigned i = 0; i < NENT; i++) begin
            if (addr_off_c == 12'(i)) begin
              addr_wr_blocked_c = cfg_q[i][7];
              if ((i + 1) < NENT) begin
                if (cfg_q[i+1][7] && (cfg_q[i+1][4:3] == 2'b01)) addr_wr_blocked_c = 1'b1;
              end
              if (!addr_wr_blocked_c) begin
                // Bits 53:0 are the 54 encoded address bits; 63:54 read zero.
                addr_d[i] = {10'b0, csr_wdata_i[53:0]};
              end
            end
          end
        end
      end

      always_ff @(posedge clk_i) begin
        if (rst_i) begin
          for (int unsigned i = 0; i < NSLOT; i++) begin
            cfg_q[i]  <= 8'd0;
            addr_q[i] <= 64'd0;
          end
          o_query_ctr  <= 32'd0;
          o_deny_ctr   <= 32'd0;
          o_locked_ctr <= 32'd0;
          o_fetch_deny_ctr <= 32'd0;
        end else begin
          for (int unsigned i = 0; i < NSLOT; i++) begin
            cfg_q[i]  <= cfg_d[i];
            addr_q[i] <= addr_d[i];
          end
          // A query is a combinational question; the counters count the cycles
          // one was asked, which is what lets a test state "the permission unit
          // was consulted" rather than infer it from the outcome.
          if (data_q.matched) begin
            o_query_ctr <= o_query_ctr + 32'd1;
            if (!data_q.allow) o_deny_ctr <= o_deny_ctr + 32'd1;
            if (data_q.locked) o_locked_ctr <= o_locked_ctr + 32'd1;
          end
          if (fetch_q.matched && !fetch_q.allow)
            o_fetch_deny_ctr <= o_fetch_deny_ctr + 32'd1;
        end
      end

      // -------------------------------------------------------- observability
      always_comb begin
        o_entry_cfg_o  = {OCW{1'b0}};
        o_entry_addr_o = {OAW{1'b0}};
        for (int unsigned i = 0; i < NENT; i++) begin
          o_entry_cfg_o[8*i +: 8]    = cfg_q[i];
          o_entry_addr_o[64*i +: 64] = addr_q[i];
        end
      end

    end
  endgenerate

endmodule : mosaic_pmp

`resetall
`default_nettype wire
