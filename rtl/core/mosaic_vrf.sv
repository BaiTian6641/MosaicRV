// ============================================================================
// mosaic_vrf -- the banked vector register file and its lane mapping (I-053).
//
// The VRF holds the 32 architectural vector registers. Each register is VLEN
// bits; the storage is split into BANKS banks of BANK_W bits so that more than
// one element can be read or written per cycle. The unit owns exactly three
// things, and each of them is a rule the case `vrf.mapping_aliases` names:
//
//   1. a FIXED logical-element-to-bank/row mapping (never a function of the
//      runtime lane quota, only of (register, element, SEW, LMUL));
//   2. conflict arbitration: a bank serves at most RD_PORTS distinct rows per
//      cycle for reads and WR_PORTS for writes, and extra demands are refused
//      rather than silently dropped;
//   3. a read-lifetime guard: a write that would clobber an element a granted
//      read has not yet consumed is refused (the mask/source-overlap rule).
//
// ---------------------------------------------------------------- geometry
//
// The numbers are the p2 geometry's (`config/geometry/p2.json`):
//
//   VLEN = 128, ELEN = 64, VREGS = 32, BANK_W = 32, BANKS = 32.
//
// A BANK_W-bit bank word holds VLEN/BANK_W = 4 words of every register. Eight
// registers share the 32 banks (REGS_PER_ROW = BANKS / WORDS_PER_REG = 8), and
// the file has ROWS = VREGS / REGS_PER_ROW = 4 rows. So register r's word w
// lives at
//
//     row  = r / REGS_PER_ROW                       (0..ROWS-1)
//     bank = (r % REGS_PER_ROW) * WORDS_PER_REG + w (0..BANKS-1)
//
// a real modulo/quotient decode rather than a bit-field split, so a geometry
// whose bank count does not divide its entry count cannot silently alias two
// registers onto one word. Total storage is BANKS * ROWS * BANK_W = 4096 bits =
// 32 registers x 128 bits, and the widest group (VLEN * LMUL=8 = 1024 bits) fits
// four times over.
//
// ------------------------------------------------- element and group address
//
// A demand names (base register, element, SEW, LMUL). The element index is the
// *group* element index, so one rule covers fractional and integer LMUL:
//
//     grp_bits = VLEN << LMUL      (LMUL >= 0, i.e. LMUL = 1,2,4,8)
//                 VLEN >> -LMUL     (LMUL < 0,  i.e. LMUL = 1/2,1/4,1/8)
//     bit_off  = elem * SEW        must satisfy bit_off + SEW <= grp_bits
//     reg_off  = bit_off / VLEN    (always 0 for fractional LMUL)
//     bit      = bit_off % VLEN
//     phys     = (base & ~(grp_regs-1)) + reg_off
//
// where grp_regs = 2^LMUL for LMUL >= 0 and 1 for fractional LMUL, and the base
// register is aligned down to the group size ("vector register groups must be
// aligned to a multiple of the group size"). An element whose bit range leaves
// the group is refused (`bad`), so an out-of-range index cannot alias onto a
// live element.
//
// An element of SEW <= BANK_W lies inside one bank word; an SEW = 64 element
// spans two words of the same row (this is the "wide operand" case). Because SEW
// is a power of two >= 8 and divides BANK_W for SEW <= BANK_W, no element ever
// straddles a word boundary, so a demand touches one or two whole bank words.
// A read returns the element right-justified in the ELEN-bit response with every
// bit above SEW cleared, so a consumer sees the element, not its neighbours.
//
// ------------------------------------------------------------- port model
//
// Read port. An eligible slot (index < lane_count_i) is granted when every bank
// it needs can accept it this cycle. A bank accepts up to RD_PORTS *distinct
// rows*; two demands for the same (bank, row) share one port, because a bank
// word read can be broadcast to every consumer that wants that word. Extra
// distinct rows are refused: `rd_gnt_o[s]` is low, the demand is retried by the
// caller and `o_rd_conflict_ctr` counts it. A demand whose geometry is invalid
// is refused too and counted apart (`o_rd_bad_ctr`), never granted.
//
// Write port. The same rule with WR_PORTS, except a second write to the same
// (bank, row) is *not* shared (two writers of one word would be a race), so a
// write bank accepts a row only if it is not already used this cycle.
//
// Read latency. A granted read's data is presented `RD_LATENCY` cycles later in
// the same slot (`rd_rsp_valid_o[s]` / `rd_rsp_data_o[s]`), carrying the
// request's 16-bit tag back. Latency is a property of the design, exposed on
// `o_rd_latency_o`; it does not depend on `lane_count_i` or `plat_i`.
//
// --------------------------------------------------------- read lifetime
//
// A write is refused when its bit range overlaps a read that has been granted
// but not yet presented:
//
//   * a read granted this cycle (it presents at +RD_LATENCY), and
//   * every pipeline stage that presents *after* this cycle.
//
// The stage that presents this cycle is already visible to its consumer, so it
// does not block a write offered in the same cycle: the write applies at the
// edge and becomes visible next cycle, after the read's value was taken. This is
// what makes `vadd.vv v0, v0, v1` (destination overlapping a source) correct:
// element i's write cannot pass the read of element i, so the overlapping source
// is never clobbered before it is consumed. Refused writes are retried;
// `o_wr_hazard_ctr` counts them.
//
// Writes are visible to reads from the next cycle (no same-cycle write-through),
// so a read and a write to one address presented together resolve as
// read-before-write, and the guard refuses the write for that cycle.
//
// The physical-width function arguments below (`[1:0]` row, `[4:0]` bank) are
// the widths for the p2 geometry (ROWS = 4, BANKS = 32); the geometry read-back
// outputs are what the driver checks them against.
//
// ------------------------------------------------------------------ mutants
//
// Four `-DMOSAIC_VRF_MUTANT_*` defines inject one defect each; the shipping
// build defines none, and the table with real output is in
// results/reports/I-053-vrf.md:
//
//   LANE_PERMUTE       the physical register an element maps to is offset by the
//                      runtime lane quota, so changing lanes permutes (and
//                      aliases) elements -- the "lane resize lost the element"
//                      fail mode.
//   LOSE_ELEMENT       the row decode drops the top row bit, so registers 16-31
//                      alias 0-15 and reading back loses half the file.
//   PLATFORM_LATENCY   the read latency depends on `plat_i`, so the same
//                      request answers at a different cycle on another platform.
//   OVERLAP_CLOBBER    the read-lifetime guard is dropped, so a destination
//                      write overwrites a source element that has not been read.
// ============================================================================

`default_nettype none
`resetall

// The resolved physical address of one element. Returned by `resolve` so the
// read path, the write path and the mapping query all use one rule.
typedef struct packed {
  logic        ok;
  int unsigned phys;     // physical register 0..VREGS-1
  int unsigned row;      // bank row
  int unsigned lo;       // low bit inside the physical register
  int unsigned hi;       // one past the high bit
  int unsigned bank0;    // first bank the element touches
  int unsigned bank1;    // second bank (== bank0 when nbanks == 1)
  int unsigned nbanks;   // 1 or 2
} mosaic_vrf_addr_t;

module mosaic_vrf #(
    parameter int unsigned VLEN       = 128,
    parameter int unsigned ELEN       = 64,
    parameter int unsigned VREGS      = 32,
    parameter int unsigned BANK_W     = 32,
    parameter int unsigned BANKS      = 32,
    parameter int unsigned LANES_MAX  = 8,
    parameter int unsigned RD_PORTS   = 2,
    parameter int unsigned WR_PORTS   = 1,
    parameter int unsigned RD_LATENCY = 1
) (
    input  logic                        clk_i,
    input  logic                        rst_i,

    // The runtime lane quota: an eligible slot has index < lane_count_i. It is
    // a throughput knob and never enters the address rule.
    input  logic [3:0]                  lane_count_i,
    // The platform selector. It exists so a control can make one property (the
    // read latency) depend on the target; in the shipping build it changes
    // nothing and is echoed on `o_plat_o`.
    input  logic [1:0]                  plat_i,

    // ------------------------------------------------------------- read slots
    input  logic [LANES_MAX-1:0]        rd_valid_i,
    input  logic [LANES_MAX*5-1:0]      rd_base_i,
    input  logic [LANES_MAX*7-1:0]      rd_elem_i,
    input  logic [LANES_MAX*3-1:0]      rd_sew_i,
    input  logic [LANES_MAX*4-1:0]      rd_lmul_i,
    input  logic [LANES_MAX*16-1:0]     rd_tag_i,

    output logic [LANES_MAX-1:0]        rd_gnt_o,
    output logic [LANES_MAX-1:0]        rd_rsp_valid_o,
    output logic [LANES_MAX*16-1:0]     rd_rsp_tag_o,
    output logic [LANES_MAX*ELEN-1:0]   rd_rsp_data_o,

    // ------------------------------------------------------------ write slots
    input  logic [LANES_MAX-1:0]        wr_valid_i,
    input  logic [LANES_MAX*5-1:0]      wr_base_i,
    input  logic [LANES_MAX*7-1:0]      wr_elem_i,
    input  logic [LANES_MAX*3-1:0]      wr_sew_i,
    input  logic [LANES_MAX*4-1:0]      wr_lmul_i,
    input  logic [LANES_MAX*ELEN-1:0]   wr_data_i,
    output logic [LANES_MAX-1:0]        wr_gnt_o,

    // ---------------------------------------------------------- mapping query
    input  logic                        q_valid_i,
    input  logic [4:0]                  q_base_i,
    input  logic [6:0]                  q_elem_i,
    input  logic [2:0]                  q_sew_i,
    input  logic [3:0]                  q_lmul_i,
    output logic                        o_q_valid_o,
    output logic [4:0]                  o_q_phys_reg_o,
    output logic [1:0]                  o_q_row_o,
    output logic [6:0]                  o_q_lo_bit_o,
    output logic [7:0]                  o_q_hi_bit_o,
    output logic [1:0]                  o_q_nbanks_o,
    output logic [4:0]                  o_q_bank0_o,
    output logic [4:0]                  o_q_bank1_o,

    // ---------------------------------------------------------------- status
    output logic [31:0]                 o_rd_gnt_ctr,
    output logic [31:0]                 o_rd_conflict_ctr,
    output logic [31:0]                 o_rd_bad_ctr,
    output logic [31:0]                 o_wr_gnt_ctr,
    output logic [31:0]                 o_wr_conflict_ctr,
    output logic [31:0]                 o_wr_hazard_ctr,
    output logic [31:0]                 o_wr_bad_ctr,
    output logic                        o_busy,

    // -------------------------------------------------------------- geometry
    output logic [31:0]                 o_vlen_o,
    output logic [31:0]                 o_elen_o,
    output logic [31:0]                 o_vregs_o,
    output logic [31:0]                 o_banks_o,
    output logic [31:0]                 o_bank_w_o,
    output logic [31:0]                 o_rows_o,
    output logic [31:0]                 o_regs_per_row_o,
    output logic [31:0]                 o_lane_max_o,
    output logic [31:0]                 o_rd_latency_o,
    output logic [31:0]                 o_rd_ports_o,
    output logic [31:0]                 o_wr_ports_o,
    output logic [31:0]                 o_plat_o
);

  // ------------------------------------------------------------- localparams
  localparam int unsigned WORDS_PER_REG = VLEN / BANK_W;              // 4
  localparam int unsigned REGS_PER_ROW  = BANKS / WORDS_PER_REG;      // 8
  localparam int unsigned ROWS          = VREGS / REGS_PER_ROW;       // 4
  localparam int unsigned LAT_MAX       = RD_LATENCY + 1;             // one spare stage for the control
  localparam int unsigned HZ_DEPTH      = LANES_MAX * LAT_MAX;

  localparam logic [ROWS-1:0] ROW_ONE = ROWS'(1);

  // ---------------------------------------------------------------- storage
  // Not reset: reset cost is control state, not ROWS x BANKS x BANK_W of
  // storage, the rule rtl/common/mosaic_ram.sv and mosaic_prf.sv follow. A
  // caller reads only locations it has written (or accepts the undefined value).
  logic [BANK_W-1:0] mem_q [ROWS][BANKS];

  // ------------------------------------------------------- resolved addresses
  mosaic_vrf_addr_t rd_ad [LANES_MAX];
  mosaic_vrf_addr_t wr_ad [LANES_MAX];
  mosaic_vrf_addr_t q_ad;

  // ---------------------------------------------------------- arbitration bus
  logic [ROWS-1:0] rd_used_c [BANKS];
  logic [ROWS-1:0] wr_used_c [BANKS];
  logic            rd_gnt_c [LANES_MAX];
  logic            wr_gnt_c [LANES_MAX];
  logic            wr_hz_c  [LANES_MAX];

  // The reads a write must not pass: granted-this-cycle reads plus the pipeline
  // stages that present after this cycle.
  logic        hz_val  [HZ_DEPTH];
  int unsigned hz_phys [HZ_DEPTH];
  int unsigned hz_lo   [HZ_DEPTH];
  int unsigned hz_hi   [HZ_DEPTH];

  // --------------------------------------------------------- pipeline stages
  logic [LANES_MAX-1:0]      st_valid [LAT_MAX];
  logic [LANES_MAX*16-1:0]   st_tag   [LAT_MAX];
  logic [LANES_MAX*ELEN-1:0] st_data  [LAT_MAX];
  logic [LANES_MAX*5-1:0]    st_phys  [LAT_MAX];
  logic [LANES_MAX*7-1:0]    st_lo    [LAT_MAX];
  logic [LANES_MAX*8-1:0]    st_hi    [LAT_MAX];

  // Stage 0's next value, computed combinationally so the sequential process is
  // free of blocking assignments.
  logic [LANES_MAX-1:0]      st0_valid;
  logic [LANES_MAX*16-1:0]   st0_tag;
  logic [LANES_MAX*ELEN-1:0] st0_data;
  logic [LANES_MAX*5-1:0]    st0_phys;
  logic [LANES_MAX*7-1:0]    st0_lo;
  logic [LANES_MAX*8-1:0]    st0_hi;

  // ------------------------------------------------------------ counter deltas
  logic [31:0] rd_gnt_d, rd_conf_d, rd_bad_d, wr_gnt_d, wr_conf_d, wr_hz_d, wr_bad_d;
  logic        busy_c;

  // --------------------------------------------------------------- counters
  logic [31:0] rd_gnt_q, rd_conf_q, rd_bad_q, wr_gnt_q, wr_conf_q, wr_hz_q, wr_bad_q;

  // ------------------------------------------------------------- lane / latency
  logic [3:0] lane_eff;
  logic [3:0] lat_eff;

  always_comb begin
    if (lane_count_i > 4'(LANES_MAX)) begin
      lane_eff = 4'(LANES_MAX);
    end else begin
      lane_eff = lane_count_i;
    end
  end

  always_comb begin
`ifdef MOSAIC_VRF_MUTANT_PLATFORM_LATENCY
    // NEGATIVE CONTROL: the read latency is a property of the platform, so the
    // same request answers at a different cycle when `plat_i` differs.
    if ((int'(RD_LATENCY) + int'(plat_i)) > int'(LAT_MAX)) begin
      lat_eff = 4'(LAT_MAX);
    end else begin
      lat_eff = 4'(int'(RD_LATENCY) + int'(plat_i));
    end
`else
    lat_eff = 4'(RD_LATENCY);
`endif
  end

  // ------------------------------------------------------------- helpers
  // The physical bank a bit of a register lives in: a modulo/quotient decode,
  // not a field split (see the header).
  function automatic logic [4:0] bank_of(input logic [4:0] regs, input logic [6:0] bit_idx);
    int unsigned r, b;
    begin
      r = int'(regs);
      b = int'(bit_idx);
      bank_of = 5'(((r % REGS_PER_ROW) * WORDS_PER_REG) + (b / BANK_W));
    end
  endfunction

  function automatic logic [1:0] row_of(input logic [4:0] regs);
    int unsigned r;
    begin
      r = int'(regs);
`ifdef MOSAIC_VRF_MUTANT_LOSE_ELEMENT
      // NEGATIVE CONTROL: the top row bit is dropped, so the four rows collapse
      // to two and registers 16-31 alias 0-15.
      row_of = 2'((r / REGS_PER_ROW) % (ROWS / 2));
`else
      row_of = 2'(r / REGS_PER_ROW);
`endif
    end
  endfunction

  // Resolve one (register, element, SEW, LMUL) demand. Pure: the lane quota does
  // not appear, which is the property the case measures.
  function automatic mosaic_vrf_addr_t resolve(input logic [4:0] base, input logic [6:0] elem,
                                               input logic [2:0] sew_l,
                                               input logic signed [3:0] lmul);
    mosaic_vrf_addr_t a;
    int sew, grp_bits, grp_regs, grp_base, bit_off, reg_off, bit_in, phys;
    begin
      a        = '0;
      sew      = 1 << int'(sew_l);
      if (lmul >= 0) begin
        grp_bits = int'(VLEN) << int'(lmul);
        grp_regs = 1 << int'(lmul);
      end else begin
        grp_bits = int'(VLEN) >> (-int'(lmul));
        grp_regs = 1;
      end
      grp_base = int'(base) & ~(grp_regs - 1);
      bit_off  = int'(elem) * sew;
      a.ok     = (sew >= 8) && (sew <= int'(ELEN)) && (lmul >= -3) && (lmul <= 3) &&
                 ((bit_off + sew) <= grp_bits) && ((grp_base + grp_regs) <= int'(VREGS));
      reg_off  = bit_off / int'(VLEN);
      bit_in   = bit_off % int'(VLEN);
      phys     = grp_base + reg_off;
      a.phys   = phys;
      a.row    = int'(row_of(5'(phys)));
      a.lo     = bit_in;
      a.hi     = bit_in + sew;
      a.bank0  = int'(bank_of(5'(phys), 7'(bit_in)));
      if (sew > int'(BANK_W)) begin
        a.nbanks = 2;
        a.bank1  = int'(bank_of(5'(phys), 7'(bit_in + int'(BANK_W))));
      end else begin
        a.nbanks = 1;
        a.bank1  = a.bank0;
      end
      if (!a.ok) begin
        a.nbanks = 0;
        a.bank1  = a.bank0;
      end
      resolve = a;
    end
  endfunction

  // The element's SEW bits out of the bank word(s) it occupies, right-justified
  // with the bits above SEW cleared.
  function automatic logic [ELEN-1:0] extract(input logic [1:0] row, input logic [6:0] lo,
                                              input logic [6:0] sew, input logic [1:0] nbanks,
                                              input logic [4:0] bank0, input logic [4:0] bank1);
    logic [ELEN-1:0]   data;
    logic [BANK_W-1:0] w0, w1;
    int off;
    begin
      w0   = mem_q[row][bank0];
      off  = int'(lo) % BANK_W;
      data = '0;
      if (nbanks == 2'd1) begin
        data = ELEN'(w0 >> off);
        if (sew < 7'(ELEN)) begin
          data = data & ((ELEN'(1) << sew) - ELEN'(1));
        end
      end else begin
        w1   = mem_q[row][bank1];
        data = {w1, w0};   // lo is word-aligned when the element spans two words
      end
      extract = data;
    end
  endfunction

  // ---------------------------------------------------- address + arbitration
  always_comb begin
    // ---- defaults -------------------------------------------------------
    for (int b = 0; b < BANKS; b++) begin
      rd_used_c[b] = '0;
      wr_used_c[b] = '0;
    end
    for (int s = 0; s < LANES_MAX; s++) begin
      rd_gnt_c[s] = 1'b0;
      wr_gnt_c[s] = 1'b0;
      wr_hz_c[s]  = 1'b0;
      rd_ad[s]    = '0;
      wr_ad[s]    = '0;
    end
    rd_gnt_d = 32'd0;
    rd_conf_d = 32'd0;
    rd_bad_d = 32'd0;
    wr_gnt_d = 32'd0;
    wr_conf_d = 32'd0;
    wr_hz_d = 32'd0;
    wr_bad_d = 32'd0;
    busy_c = 1'b0;

    // ---- resolve read demands ------------------------------------------
    for (int s = 0; s < LANES_MAX; s++) begin
      rd_ad[s] = resolve(rd_base_i[s*5 +: 5], rd_elem_i[s*7 +: 7], rd_sew_i[s*3 +: 3],
                         rd_lmul_i[s*4 +: 4]);
`ifdef MOSAIC_VRF_MUTANT_LANE_PERMUTE
      // NEGATIVE CONTROL: the physical register is offset by the runtime lane
      // quota, so a change of lane count permutes and aliases elements.
      rd_ad[s].phys  = (rd_ad[s].phys + int'(lane_eff)) % int'(VREGS);
      rd_ad[s].row   = int'(row_of(5'(rd_ad[s].phys)));
      rd_ad[s].bank0 = int'(bank_of(5'(rd_ad[s].phys), 7'(rd_ad[s].lo)));
      if (rd_ad[s].nbanks == 2) begin
        rd_ad[s].bank1 = int'(bank_of(5'(rd_ad[s].phys), 7'(rd_ad[s].lo + int'(BANK_W))));
      end
`endif
    end

    // ---- read arbitration, lowest slot first ---------------------------
    for (int s = 0; s < LANES_MAX; s++) begin
      if ((s < int'(lane_eff)) && rd_valid_i[s]) begin
        if (!rd_ad[s].ok) begin
          rd_bad_d = rd_bad_d + 32'd1;
        end else if (((rd_used_c[rd_ad[s].bank0] & (ROW_ONE << rd_ad[s].row)) == '0) &&
                     ($countones(rd_used_c[rd_ad[s].bank0]) >= int'(RD_PORTS))) begin
          rd_conf_d = rd_conf_d + 32'd1;
          busy_c    = 1'b1;
        end else if ((rd_ad[s].nbanks == 2) &&
                     ((rd_used_c[rd_ad[s].bank1] & (ROW_ONE << rd_ad[s].row)) == '0) &&
                     ($countones(rd_used_c[rd_ad[s].bank1]) >= int'(RD_PORTS))) begin
          rd_conf_d = rd_conf_d + 32'd1;
          busy_c    = 1'b1;
        end else begin
          rd_gnt_c[s] = 1'b1;
          rd_used_c[rd_ad[s].bank0] = rd_used_c[rd_ad[s].bank0] |
                                      (ROW_ONE << rd_ad[s].row);
          if (rd_ad[s].nbanks == 2) begin
            rd_used_c[rd_ad[s].bank1] = rd_used_c[rd_ad[s].bank1] |
                                        (ROW_ONE << rd_ad[s].row);
          end
        end
      end
    end

    // ---- the read-lifetime set -----------------------------------------
    // Every entry names a (physical register, bit range) whose value has been
    // taken by a granted read but not yet presented. A write overlapping any of
    // them is refused.
    for (int e = 0; e < HZ_DEPTH; e++) begin
      hz_val[e]  = 1'b0;
      hz_phys[e] = '0;
      hz_lo[e]   = '0;
      hz_hi[e]   = '0;
    end
    for (int s = 0; s < LANES_MAX; s++) begin
      if (rd_gnt_c[s]) begin
        hz_val[s]  = 1'b1;
        hz_phys[s] = rd_ad[s].phys;
        hz_lo[s]   = rd_ad[s].lo;
        hz_hi[s]   = rd_ad[s].hi;
      end
    end
    for (int k = 0; k < LAT_MAX - 1; k++) begin
      // Stage k presents at +k+1 from now; stages 0..lat_eff-2 present after
      // this cycle, so they are part of the lifetime set.
      if (k < (int'(lat_eff) - 1)) begin
        for (int s = 0; s < LANES_MAX; s++) begin
          if (st_valid[k][s]) begin
            hz_val[LANES_MAX + k*LANES_MAX + s]  = 1'b1;
            hz_phys[LANES_MAX + k*LANES_MAX + s] = int'(st_phys[k][s*5 +: 5]);
            hz_lo[LANES_MAX + k*LANES_MAX + s]   = int'(st_lo[k][s*7 +: 7]);
            hz_hi[LANES_MAX + k*LANES_MAX + s]   = int'(st_hi[k][s*8 +: 8]);
          end
        end
      end
    end

    // ---- resolve write demands -----------------------------------------
    for (int s = 0; s < LANES_MAX; s++) begin
      wr_ad[s] = resolve(wr_base_i[s*5 +: 5], wr_elem_i[s*7 +: 7], wr_sew_i[s*3 +: 3],
                         wr_lmul_i[s*4 +: 4]);
`ifdef MOSAIC_VRF_MUTANT_LANE_PERMUTE
      wr_ad[s].phys  = (wr_ad[s].phys + int'(lane_eff)) % int'(VREGS);
      wr_ad[s].row   = int'(row_of(5'(wr_ad[s].phys)));
      wr_ad[s].bank0 = int'(bank_of(5'(wr_ad[s].phys), 7'(wr_ad[s].lo)));
      if (wr_ad[s].nbanks == 2) begin
        wr_ad[s].bank1 = int'(bank_of(5'(wr_ad[s].phys), 7'(wr_ad[s].lo + int'(BANK_W))));
      end
`endif
    end

    // ---- the lifetime guard --------------------------------------------
    for (int s = 0; s < LANES_MAX; s++) begin
      wr_hz_c[s] = 1'b0;
`ifdef MOSAIC_VRF_MUTANT_OVERLAP_CLOBBER
      // NEGATIVE CONTROL: the guard is dropped, so a destination write clobbers
      // a source element a granted read has not consumed yet. The lifetime set is
      // still read (with the result forced to "no hazard") so this control lints
      // exactly like the shipping build.
      for (int e = 0; e < HZ_DEPTH; e++) begin
        if (hz_val[e] && (hz_phys[e] == wr_ad[s].phys) &&
            (wr_ad[s].lo < hz_hi[e]) && (hz_lo[e] < wr_ad[s].hi)) begin
          wr_hz_c[s] = 1'b0;
        end
      end
`else
      for (int e = 0; e < HZ_DEPTH; e++) begin
        if (hz_val[e] && (hz_phys[e] == wr_ad[s].phys) &&
            (wr_ad[s].lo < hz_hi[e]) && (hz_lo[e] < wr_ad[s].hi)) begin
          wr_hz_c[s] = 1'b1;
        end
      end
`endif
    end

    // ---- write arbitration ---------------------------------------------
    for (int s = 0; s < LANES_MAX; s++) begin
      if ((s < int'(lane_eff)) && wr_valid_i[s]) begin
        if (!wr_ad[s].ok) begin
          wr_bad_d = wr_bad_d + 32'd1;
        end else if (wr_hz_c[s]) begin
          wr_hz_d = wr_hz_d + 32'd1;
        end else if (((wr_used_c[wr_ad[s].bank0] & (ROW_ONE << wr_ad[s].row)) != '0) ||
                     ($countones(wr_used_c[wr_ad[s].bank0]) >= int'(WR_PORTS))) begin
          wr_conf_d = wr_conf_d + 32'd1;
        end else if ((wr_ad[s].nbanks == 2) &&
                     (((wr_used_c[wr_ad[s].bank1] & (ROW_ONE << wr_ad[s].row)) != '0) ||
                      ($countones(wr_used_c[wr_ad[s].bank1]) >= int'(WR_PORTS)))) begin
          wr_conf_d = wr_conf_d + 32'd1;
        end else begin
          wr_gnt_c[s] = 1'b1;
          wr_used_c[wr_ad[s].bank0] = wr_used_c[wr_ad[s].bank0] |
                                      (ROW_ONE << wr_ad[s].row);
          if (wr_ad[s].nbanks == 2) begin
            wr_used_c[wr_ad[s].bank1] = wr_used_c[wr_ad[s].bank1] |
                                        (ROW_ONE << wr_ad[s].row);
          end
        end
      end
    end
  end

  // ------------------------------------------------------------- the query
  always_comb begin
    q_ad = resolve(q_base_i, q_elem_i, q_sew_i, q_lmul_i);
`ifdef MOSAIC_VRF_MUTANT_LANE_PERMUTE
    q_ad.phys  = (q_ad.phys + int'(lane_eff)) % int'(VREGS);
    q_ad.row   = int'(row_of(5'(q_ad.phys)));
    q_ad.bank0 = int'(bank_of(5'(q_ad.phys), 7'(q_ad.lo)));
    if (q_ad.nbanks == 2) begin
      q_ad.bank1 = int'(bank_of(5'(q_ad.phys), 7'(q_ad.lo + int'(BANK_W))));
    end
`endif
    o_q_valid_o    = q_valid_i && q_ad.ok;
    o_q_phys_reg_o = 5'(q_ad.phys);
    o_q_row_o      = 2'(q_ad.row);
    o_q_lo_bit_o   = 7'(q_ad.lo);
    o_q_hi_bit_o   = 8'(q_ad.hi);
    o_q_nbanks_o   = 2'(q_ad.nbanks);
    o_q_bank0_o    = 5'(q_ad.bank0);
    o_q_bank1_o    = 5'(q_ad.bank1);
  end

  // ------------------------------------------------- stage 0 of the pipeline
  always_comb begin
    st0_valid = '0;
    st0_tag   = '0;
    st0_data  = '0;
    st0_phys  = '0;
    st0_lo    = '0;
    st0_hi    = '0;
    for (int s = 0; s < LANES_MAX; s++) begin
      if (rd_gnt_c[s]) begin
        st0_valid[s]             = 1'b1;
        st0_tag[s*16 +: 16]      = rd_tag_i[s*16 +: 16];
        st0_data[s*ELEN +: ELEN] = extract(2'(rd_ad[s].row), 7'(rd_ad[s].lo),
                                           7'(rd_ad[s].hi - rd_ad[s].lo), 2'(rd_ad[s].nbanks),
                                           5'(rd_ad[s].bank0), 5'(rd_ad[s].bank1));
        st0_phys[s*5 +: 5]       = 5'(rd_ad[s].phys);
        st0_lo[s*7 +: 7]         = 7'(rd_ad[s].lo);
        st0_hi[s*8 +: 8]         = 8'(rd_ad[s].hi);
      end
    end
  end

  // -------------------------------------------------------- response outputs
  // The response of slot s comes from the pipeline stage `lat_eff-1`.
  always_comb begin
    for (int s = 0; s < LANES_MAX; s++) begin
      rd_rsp_valid_o[s]             = st_valid[int'(lat_eff) - 1][s];
      rd_rsp_tag_o[s*16 +: 16]      = st_tag[int'(lat_eff) - 1][s*16 +: 16];
      rd_rsp_data_o[s*ELEN +: ELEN] = st_data[int'(lat_eff) - 1][s*ELEN +: ELEN];
    end
  end

  // ------------------------------------------------------- sequential state
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      for (int k = 0; k < LAT_MAX; k++) begin
        st_valid[k] <= '0;
        st_tag[k]   <= '0;
        st_data[k]  <= '0;
        st_phys[k]  <= '0;
        st_lo[k]    <= '0;
        st_hi[k]    <= '0;
      end
      rd_gnt_q  <= 32'd0;
      rd_conf_q <= 32'd0;
      rd_bad_q  <= 32'd0;
      wr_gnt_q  <= 32'd0;
      wr_conf_q <= 32'd0;
      wr_hz_q   <= 32'd0;
      wr_bad_q  <= 32'd0;
    end else begin
      // Pipeline shift: stage 0 takes this cycle's granted reads.
      for (int k = LAT_MAX - 1; k > 0; k--) begin
        st_valid[k] <= st_valid[k-1];
        st_tag[k]   <= st_tag[k-1];
        st_data[k]  <= st_data[k-1];
        st_phys[k]  <= st_phys[k-1];
        st_lo[k]    <= st_lo[k-1];
        st_hi[k]    <= st_hi[k-1];
      end
      st_valid[0] <= st0_valid;
      st_tag[0]   <= st0_tag;
      st_data[0]  <= st0_data;
      st_phys[0]  <= st0_phys;
      st_lo[0]    <= st0_lo;
      st_hi[0]    <= st0_hi;
      for (int s = 0; s < LANES_MAX; s++) begin
        if (wr_gnt_c[s]) begin
          if (wr_ad[s].nbanks == 1) begin
            int unsigned off;
            logic [BANK_W-1:0] mask_w;
            off    = wr_ad[s].lo % BANK_W;
            mask_w = BANK_W'((64'(1) << (wr_ad[s].hi - wr_ad[s].lo)) - 64'd1) << off;
            mem_q[wr_ad[s].row][wr_ad[s].bank0] <=
                ((mem_q[wr_ad[s].row][wr_ad[s].bank0] & ~mask_w) |
                 ((BANK_W'(wr_data_i[s*ELEN +: ELEN]) << off) & mask_w));
          end else begin
            mem_q[wr_ad[s].row][wr_ad[s].bank0] <= wr_data_i[s*ELEN +: BANK_W];
            mem_q[wr_ad[s].row][wr_ad[s].bank1] <=
                wr_data_i[s*ELEN + BANK_W +: BANK_W];
          end
        end
      end
      rd_gnt_q  <= rd_gnt_q  + rd_gnt_d;
      rd_conf_q <= rd_conf_q + rd_conf_d;
      rd_bad_q  <= rd_bad_q  + rd_bad_d;
      wr_gnt_q  <= wr_gnt_q  + wr_gnt_d;
      wr_conf_q <= wr_conf_q + wr_conf_d;
      wr_hz_q   <= wr_hz_q   + wr_hz_d;
      wr_bad_q  <= wr_bad_q  + wr_bad_d;
    end
  end

  // ---------------------------------------------------------- output assigns
  always_comb begin
    for (int s = 0; s < LANES_MAX; s++) begin
      rd_gnt_o[s] = rd_gnt_c[s];
      wr_gnt_o[s] = wr_gnt_c[s];
    end
  end

  assign o_rd_gnt_ctr      = rd_gnt_q;
  assign o_rd_conflict_ctr = rd_conf_q;
  assign o_rd_bad_ctr      = rd_bad_q;
  assign o_wr_gnt_ctr      = wr_gnt_q;
  assign o_wr_conflict_ctr = wr_conf_q;
  assign o_wr_hazard_ctr   = wr_hz_q;
  assign o_wr_bad_ctr      = wr_bad_q;
  assign o_busy            = busy_c;

  assign o_vlen_o          = 32'(VLEN);
  assign o_elen_o          = 32'(ELEN);
  assign o_vregs_o         = 32'(VREGS);
  assign o_banks_o         = 32'(BANKS);
  assign o_bank_w_o        = 32'(BANK_W);
  assign o_rows_o          = 32'(ROWS);
  assign o_regs_per_row_o  = 32'(REGS_PER_ROW);
  assign o_lane_max_o      = 32'(LANES_MAX);
  assign o_rd_latency_o    = 32'(lat_eff);
  assign o_rd_ports_o      = 32'(RD_PORTS);
  assign o_wr_ports_o      = 32'(WR_PORTS);
  assign o_plat_o          = 32'(plat_i);

endmodule : mosaic_vrf

`resetall
`default_nettype wire
