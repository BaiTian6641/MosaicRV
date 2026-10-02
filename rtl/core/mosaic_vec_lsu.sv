// ============================================================================
// mosaic_vec_lsu -- the vector memory packetizer (work package I-056,
// CASE=rvv.memory_modes).
//
// The unit turns one vector *memory macro* into per-element memory requests.
// It reads the vector operands a store (or an indexed access) needs from the
// banked VRF (`mosaic_vrf`, I-053) through the same single read slot I-054's
// arithmetic engine uses, writes a load's destination through the write slot,
// and takes its configuration from the snapshot `mosaic_vec_cfg` (I-052) hands
// out: SEW/LMUL/vta/vma are decoded from `cfg_vtype_i` at the ratified
// positions (vsew 5:3, vlmul 2:0, vta 6, vma 7) and from nowhere else. `vl`
// and `vstart` arrive on their own snapshot ports.
//
// --------------------------------------------------------------- the packet
//
// One macro is a sequence of *items*. An item is one (element, field) pair for
// a segmented access, and one element for every other access. Every item
// carries, and the memory port exposes, all four things the plan asks for:
//
//   logical index   the element index (`mem_req_elem_o`) and, for segmented
//                   accesses, the field (`mem_req_field_o`);
//   address         the byte address of the element, presented as the
//                   8-byte-aligned beat plus a byte mask (below);
//   byte mask       `mem_req_wmask_o`, bit k set iff byte `addr+k` is part of
//                   the element. The mask is therefore a function of both the
//                   element *width* (EEW/8 bytes) and its *position* (the lane
//                   inside the beat);
//   fault ownership the same index/field pair travels back on the response, so
//                   a fault is attributed to the element that caused it.
//
// The address rules, one per mode:
//
//   unit stride        addr = base + elem*EEW/8                   (vle/vse)
//   constant stride    addr = base + elem*stride                  (vlse/vsse)
//   indexed            addr = base + offset[elem]                 (vlxei/vsxei)
//                      the offset vector is zero-extended to XLEN, its EEW is
//                      the instruction's `eiN` suffix and its EMUL is
//                      (EEW/SEW)*LMUL
//   segment unit       addr = base + (elem*NFIELDS + field)*EEW/8  (vlseg)
//   segment strided    addr = base + elem*stride + field*EEW/8    (vlsseg)
//   segment indexed    addr = base + offset[elem] + field*EEW/8   (vlxseg)
//   whole register     addr = base + j*EEW/8, j in [0, NFIELDS*VLEN/EEW)
//                      (vl<nf>re<ee>/vs<nf>re<ee>; NFIELDS is 1, 2, 4 or 8)
//   mask               addr = base + b, b in [0, EVL), EVL = ceil(vl/8)
//                      (vlm.v/vsm.v; EEW=8, EMUL=1, vstart is in bytes)
//
// One request is issued per item, in ascending item order. There is **no
// coalescing**: the first correct version asks for exactly the bytes of one
// element. The request count is therefore the item count, which is what makes
// a merge observable.
//
// ------------------------------------------------------ the fault-ownership rule
//
// Faults are **element-granular**. The architecture requires the vector unit to
// report which element faulted: the architectural `vstart` is set to the
// faulting element's logical index, and only the elements *before* it have
// taken effect. The rule this unit implements, stated once:
//
//   The packetizer issues items in ascending logical order. When a request
//   completes with `mem_rsp_fault_i`, the macro stops:
//     * `o_trap_o` asserts with `o_trap_elem_o` = the faulting element index
//       (the value the restart writes to `vstart`);
//     * no item at or after the faulting one is issued;
//     * a store at or after the faulting element is not performed (the response
//       is a fault, so the write is withheld by the memory side);
//     * a load's destination elements at or after the faulting element are
//       left untouched, so a restart re-executes them;
//     * the elements strictly before it have already taken effect -- their
//       stores were performed or their loads written back -- and any of their
//       responses still queued are drained before the unit retires the fault.
//
// A whole-macro trap that reports no element index, or reports `vstart` = 0
// for a fault at element k, is not acceptable for a unit-stride or strided
// load, and the negative control `MOSAIC_VEC_LSU_MUTANT_WHOLE_FAULT` is exactly
// that defect.
//
// The fault class travels with the response (`mem_rsp_fault_code_i`) and out on
// `trap_code_o`, because I-057's restart controller must tell a page fault or
// an access fault (which a fault-only-first load may absorb) from anything else
// (which it may not). The class is opaque to this unit: it carries four bits and
// never decodes them.
//
// ------------------------------------------------------- the boundary stop
//
// A precise interrupt is taken at an element boundary, not mid-item. Asserting
// `stop_i` makes the unit finish the item it is on, offer nothing past the
// boundary, drain every outstanding response (so a store already accepted is
// performed and a load already accepted is written back), and finish with
// `stopped_o` and `stop_elem_o` = the first element *not* performed. That is the
// element a resume writes to `vstart`. `stop_i` therefore behaves like the
// fault path without a fault: it is checked only at the item boundary in
// ST_SCAN, and it sets `stopped_o`, never `trap_o`.
//
// ------------------------------------------------------ ordering and the network
//
// The indexed forms are provided in both an *ordered* form (`vluxei`/`vsuxei`,
// `vluxseg`/`vsuxseg`) and an *unordered* form (`vlxei`/`vsxei`, `vlxseg`/
// `vsxseg`). The ordered forms require element i's access to complete before
// element i+1 is issued; the unordered forms do not. This unit implements:
//
//   * the ordered forms strictly -- at most one request outstanding, the next
//     item is not offered until the previous response has arrived;
//   * the unordered forms with a small request pipeline (OUT_MAX = 2
//     outstanding loads), because a load has no memory side effect and the
//     ordering freedom is real;
//   * **all stores strictly**, ordered or not, because a store that faults
//     must not have a later store already committed -- partial completion
//     needs issue order for stores.
//
// The port carries the element identity on both the request and the response,
// and the request stream on `mem_req_*` is what a case observes, so the order
// is visible on the memory port rather than merely asserted.
//
// ------------------------------------------------------ no merge across regions
//
// With one request per item and no coalescing, a line merge cannot be formed,
// so a merge crossing a device boundary cannot happen -- the property holds by
// construction. The case still proves it: an access whose elements straddle a
// device boundary must show every request wholly inside one region, with the
// device elements addressed in the device and the RAM elements in RAM. The
// negative control `MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION` adds a
// beat-level merge and is caught by that check.
//
// ---------------------------------------------------------------- tail policy
//
// A load's destination elements that are not loaded follow vta/vma: masked-off
// elements (active range, mask clear) use vma and tail elements (index >= vl)
// use vta; an element below vstart is left unchanged, whatever the policy says.
// The policy is applied in a post-pass over the items not loaded, using a
// loaded bitmap, after every response has been drained. Whole-register loads
// fill their group completely; the mask load is tail-agnostic (bytes at or
// above EVL are written all-ones), and neither has a policy pass.
//
// ------------------------------------------------------------------ mutants
//
// Four `-DMOSAIC_VEC_LSU_MUTANT_*` defines inject one defect each; the shipping
// build defines none. Each is proven to fail CASE=rvv.memory_modes in
// results/reports/I-056-vector-memory.md:
//
//   MERGE_CROSS_REGION  the byte mask of each request absorbs the bytes of the
//                       earlier items of the same beat, so a request can name
//                       bytes on both sides of a region boundary
//   WHOLE_FAULT         a fault is reported with vstart reset to zero instead
//                       of the faulting element's index
//   MASK_WRONG          the byte mask drops the element's lane position and
//                       width, so the enabled bytes are not the element's
//   ORDERED_REORDER     the ordered indexed accesses take the unordered
//                       ordering gate, so element i+1 can be issued before
//                       element i has completed
// ============================================================================

`default_nettype none
`resetall

module mosaic_vec_lsu #(
    parameter int unsigned VLEN = 128,
    parameter int unsigned ELEN = 64,
    parameter int unsigned NLSM = 8
) (
    input  logic                    clk_i,
    input  logic                    rst_i,

    // ------------------------------------------------- capability gate
    // One bit per memory mode; a mode whose bit is clear is refused and issues
    // no request, so a machine cannot execute a mode it does not advertise.
    input  logic [NLSM-1:0]         caps_i,

    // --------------------------------- configuration snapshot (from I-052)
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [63:0]             cfg_vtype_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [7:0]              cfg_vl_i,
    input  logic [6:0]              cfg_vstart_i,

    // ---------------------------------------------------- macro launch
    input  logic                    exec_valid_i,
    input  logic [3:0]              exec_mode_i,
    input  logic                    exec_we_i,
    input  logic                    exec_ordered_i,
    input  logic [3:0]              exec_nf_i,
    input  logic [4:0]              exec_vd_i,
    input  logic [4:0]              exec_data_i,
    input  logic [4:0]              exec_index_i,
    input  logic [2:0]              exec_idx_sew_i,
    input  logic [63:0]             exec_base_i,
    input  logic [63:0]             exec_stride_i,
    input  logic                    exec_mask_en_i,

    output logic                    busy_o,
    output logic                    done_o,
    output logic                    illegal_o,
    output logic                    trap_o,
    output logic [6:0]              trap_elem_o,
    output logic [3:0]              trap_code_o,
    // ------------------------------------------------- partial-trap stop
    // A precise interrupt is taken at an element boundary: asserting `stop_i`
    // makes the unit finish the item it is on, issue nothing past the boundary,
    // drain every outstanding response, and finish with `stopped_o` and
    // `stop_elem_o` = the first element not performed.  A fault reports
    // `trap_o`, never both.
    input  logic                    stop_i,
    output logic                    stopped_o,
    output logic [6:0]              stop_elem_o,
    output logic [7:0]              elems_o,
    output logic [31:0]             req_ctr_o,

    // -------------------------------------------------- VRF read slot
    output logic                    vrf_rd_valid_o,
    output logic [4:0]              vrf_rd_base_o,
    output logic [6:0]              vrf_rd_elem_o,
    output logic [2:0]              vrf_rd_sew_o,
    output logic [3:0]              vrf_rd_lmul_o,
    output logic [15:0]             vrf_rd_tag_o,
    input  logic                    vrf_rd_gnt_i,
    input  logic                    vrf_rd_rsp_valid_i,
    input  logic [15:0]             vrf_rd_rsp_tag_i,
    input  logic [63:0]             vrf_rd_rsp_data_i,

    // -------------------------------------------------- VRF write port
    output logic                    vrf_wr_valid_o,
    output logic [4:0]              vrf_wr_base_o,
    output logic [6:0]              vrf_wr_elem_o,
    output logic [2:0]              vrf_wr_sew_o,
    output logic [3:0]              vrf_wr_lmul_o,
    output logic [63:0]             vrf_wr_data_o,
    input  logic                    vrf_wr_gnt_i,

    // ------------------------------------------------------ memory port
    // One request per item. `addr` is the 8-byte-aligned beat and `wmask` the
    // byte enables relative to it; `wdata` is lane-positioned.
    output logic                    mem_req_valid_o,
    input  logic                    mem_req_ready_i,
    output logic [6:0]              mem_req_elem_o,
    output logic [3:0]              mem_req_field_o,
    output logic [63:0]             mem_req_addr_o,
    output logic [7:0]              mem_req_wmask_o,
    output logic [63:0]             mem_req_wdata_o,
    output logic                    mem_req_we_o,
    output logic [3:0]              mem_req_size_o,
    output logic                    mem_req_ordered_o,
    input  logic                    mem_rsp_valid_i,
    input  logic [6:0]              mem_rsp_elem_i,
    input  logic [3:0]              mem_rsp_field_i,
    input  logic                    mem_rsp_fault_i,
    input  logic [3:0]              mem_rsp_fault_code_i,
    input  logic [63:0]             mem_rsp_rdata_i
);

  // ------------------------------------------------------------------ modes
  localparam logic [3:0] LM_UNIT        = 4'd0;
  localparam logic [3:0] LM_STRIDED     = 4'd1;
  localparam logic [3:0] LM_INDEXED     = 4'd2;
  localparam logic [3:0] LM_SEG_UNIT    = 4'd3;
  localparam logic [3:0] LM_SEG_STRIDED = 4'd4;
  localparam logic [3:0] LM_SEG_INDEXED = 4'd5;
  localparam logic [3:0] LM_WHOLE       = 4'd6;
  localparam logic [3:0] LM_MASK        = 4'd7;

  localparam logic [15:0] TAG_MASK = 16'h1000;
  localparam logic [15:0] TAG_IDX  = 16'h2000;
  localparam logic [15:0] TAG_DAT  = 16'h3000;

  localparam int unsigned OUT_MAX = 2;   // outstanding unordered loads
  localparam int unsigned WFIFO_D = 4;   // load write-back queue depth

  localparam logic [3:0] ST_IDLE  = 4'd0;
  localparam logic [3:0] ST_SCAN  = 4'd1;
  localparam logic [3:0] ST_MASK  = 4'd2;
  localparam logic [3:0] ST_INDEX = 4'd3;
  localparam logic [3:0] ST_DATA  = 4'd4;
  localparam logic [3:0] ST_VRD   = 4'd5;
  localparam logic [3:0] ST_REQ   = 4'd6;
  localparam logic [3:0] ST_WAIT  = 4'd7;
  localparam logic [3:0] ST_DRAIN = 4'd8;
  localparam logic [3:0] ST_TAIL  = 4'd9;
  localparam logic [3:0] ST_TWR   = 4'd10;
  localparam logic [3:0] ST_DONE  = 4'd11;

  // ================================================================== helpers
  function automatic int nm_lmul_exp(input logic [2:0] vlmul);
    begin
      case (vlmul)
        3'd0:    nm_lmul_exp = 0;
        3'd1:    nm_lmul_exp = 1;
        3'd2:    nm_lmul_exp = 2;
        3'd3:    nm_lmul_exp = 3;
        3'd5:    nm_lmul_exp = -3;
        3'd6:    nm_lmul_exp = -2;
        3'd7:    nm_lmul_exp = -1;
        default: nm_lmul_exp = 0;   // reserved; refused separately
      endcase
    end
  endfunction

  // A 4-bit two's-complement LMUL exponent as a plain int, so no signed-cast
  // surprises survive the arithmetic.
  function automatic int nm_i4(input logic [3:0] v);
    begin
      if (v[3]) nm_i4 = int'(v) - 16;
      else      nm_i4 = int'(v);
    end
  endfunction

  function automatic logic [3:0] nm_e4(input int e);
    begin
      nm_e4 = 4'(e & 32'hF);
    end
  endfunction

  // NFIELDS is a power of two; its LMUL exponent is log2 of the count.
  function automatic logic [3:0] nm_nf_log2(input logic [3:0] nf);
    begin
      case (nf)
        4'd1:    nm_nf_log2 = 4'd0;
        4'd2:    nm_nf_log2 = 4'd1;
        4'd4:    nm_nf_log2 = 4'd2;
        4'd8:    nm_nf_log2 = 4'd3;
        default: nm_nf_log2 = 4'd0;
      endcase
    end
  endfunction

  function automatic int nm_vlmax(input int eew_l, input int lmul_e);
    int e;
    begin
      e = 7 + lmul_e - eew_l;
      if (e < 0 || e > 7) nm_vlmax = 0;
      else nm_vlmax = (1 << e);
    end
  endfunction

  function automatic logic [63:0] nm_width_mask(input int w);
    begin
      if (w >= 64) nm_width_mask = {64{1'b1}};
      else nm_width_mask = (64'd1 << w) - 64'd1;
    end
  endfunction

  // byte enables relative to the 8-byte beat holding byte `addr`
  function automatic logic [7:0] nm_byte_mask(input logic [3:0] be, input logic [2:0] lane);
    /* verilator lint_off UNUSEDSIGNAL */
    logic [15:0] ones;
    logic [15:0] shifted;
    /* verilator lint_on UNUSEDSIGNAL */
    begin
      if (be >= 4'd8) begin
        nm_byte_mask = 8'hFF;
      end else begin
        ones = (16'h0001 << be) - 16'h0001;   // be width bits, then at the lane
        shifted = ones << {1'b0, lane};
        nm_byte_mask = shifted[7:0];
      end
    end
  endfunction

  // ------------------------------------------------------------ derived config
  logic [2:0]        cfg_vsew;
  logic [2:0]        cfg_vlmul;
  logic              cfg_vma;
  logic              cfg_vta;
  int                cfg_lmul_exp;
  int                cfg_vlmax;

  always_comb begin
    cfg_vsew     = cfg_vtype_i[5:3];
    cfg_vlmul    = cfg_vtype_i[2:0];
    cfg_vma      = cfg_vtype_i[7];
    cfg_vta      = cfg_vtype_i[6];
    cfg_lmul_exp = nm_lmul_exp(cfg_vlmul);
    cfg_vlmax    = nm_vlmax(int'(cfg_vsew), cfg_lmul_exp);
  end

  // ================================================================ state
  logic [3:0]  state_q;
  logic [3:0]  mode_q;
  logic        we_q;
  logic        ordered_q;
  logic        mask_en_q;
  logic [2:0]  eew_log2_q;
  logic [2:0]  idx_log2_q;
  logic [3:0]  lmul_exp_q;      // two's complement in [-3, 3]
  logic [3:0]  nf_q;
  logic [4:0]  vd_q;
  logic [4:0]  data_q;
  logic [4:0]  index_q;
  logic [63:0] base_q;
  logic [63:0] stride_q;
  logic [7:0]  vl_q;
  logic [7:0]  vstart_q;
  logic        vma_q;
  logic        vta_q;

  logic [7:0]  elem_q;          // main scan element
  logic [3:0]  field_q;         // main scan field
  logic [7:0]  t_elem_q;        // post-pass element
  logic [3:0]  t_field_q;       // post-pass field
  logic [127:0] loaded_bm_q;

  logic [63:0] index_val_q;
  logic [63:0] data_val_q;
  logic [7:0]  mask_byte_q;
  logic [4:0]  mask_byte_idx_q;
  logic        mask_valid_q;

  logic        busy_r;
  logic        done_r;
  logic        illegal_r;
  logic        trap_r;
  logic [6:0]  trap_elem_r;
  logic [3:0]  trap_code_r;
  logic        stopped_r;
  logic [6:0]  stop_elem_r;
  logic        stop_pending_q;
  logic [7:0]  elems_r;
  logic [31:0] reqctr_r;
  logic [31:0] vrdctr_r;
  logic        abort_q;
  logic [2:0]  outstanding_q;
  logic [2:0]  vr_kind_q;

  logic                  wf_valid_q [0:WFIFO_D-1];
  logic [6:0]            wf_elem_q  [0:WFIFO_D-1];
  logic [3:0]            wf_field_q [0:WFIFO_D-1];
  logic [63:0]           wf_data_q  [0:WFIFO_D-1];
  logic [2:0]            wf_count_q;

  // the byte lane of each accepted request, in issue order: a response does not
  // carry the address, so the load's element is extracted from the beat with the
  // lane the request was offered with
  logic [2:0]            lane_fifo_q [0:3];
  logic [2:0]            lane_count_q;

  // ------------------------------------------------------- derived signals
  logic        is_seg;
  logic        is_indexed;
  logic        is_whole;
  logic        is_mask;
  logic [3:0]  nf_eff;
  logic [3:0]  emul_regs;
  int          elem_end;
  int          tail_end;
  int          evl;
  logic [63:0] be64;
  logic [63:0] elem64;
  logic [63:0] field64;
  logic [63:0] nf64;
  logic [63:0] off_c;
  logic [63:0] addr_c;
  logic [2:0]  lane_c;
  logic [63:0] beat_c;
  logic [7:0]  wmask_c;
  logic [63:0] wdata_c;
  logic        active_c;
  logic        need_mask_c;
  logic [2:0]  limit_c;
  logic        limit_hit_c;
  logic [4:0]  rdw_base_c;
  logic [6:0]  rdw_elem_c;
  logic [2:0]  rdw_sew_c;
  logic [3:0]  rdw_lmul_c;
  logic [4:0]  rdw_grp_base;

  always_comb begin
    is_seg     = (mode_q == LM_SEG_UNIT) || (mode_q == LM_SEG_STRIDED) ||
                 (mode_q == LM_SEG_INDEXED);
    is_indexed = (mode_q == LM_INDEXED) || (mode_q == LM_SEG_INDEXED);
    is_whole   = (mode_q == LM_WHOLE);
    is_mask    = (mode_q == LM_MASK);
    nf_eff     = is_seg ? nf_q : 4'd1;
    emul_regs  = (!lmul_exp_q[3]) ? (4'd1 << lmul_exp_q[2:0]) : 4'd1;

    evl      = (int'(vl_q) + 7) >> 3;
    elem_end = is_whole ? (int'(nf_q) * (VLEN / (1 << eew_log2_q)))
             : is_mask  ? evl
             : int'(vl_q);
    tail_end = is_mask ? (VLEN / 8) : nm_vlmax(int'(eew_log2_q), nm_i4(lmul_exp_q));

    be64    = 64'd1 << (eew_log2_q - 3'd3);   // EEW in bytes
    elem64  = {56'd0, elem_q};
    field64 = {60'd0, field_q};
    nf64    = {60'd0, nf_q};

    case (mode_q)
      LM_UNIT:        off_c = elem64 * be64;
      LM_STRIDED:     off_c = elem64 * stride_q;
      LM_INDEXED:     off_c = index_val_q;
      LM_SEG_UNIT:    off_c = (elem64 * nf64 + field64) * be64;
      LM_SEG_STRIDED: off_c = elem64 * stride_q + field64 * be64;
      LM_SEG_INDEXED: off_c = index_val_q + field64 * be64;
      LM_WHOLE:       off_c = elem64 * be64;
      default:        off_c = elem64;              // mask: one byte per item
    endcase

    addr_c  = base_q + off_c;
    lane_c  = addr_c[2:0];
    beat_c  = {addr_c[63:3], 3'b000};
    wmask_c = nm_byte_mask(4'(be64[3:0]), lane_c);
    wdata_c = data_val_q << (8 * {61'b0, lane_c});
`ifdef MOSAIC_VEC_LSU_MUTANT_MASK_WRONG
    // NEGATIVE CONTROL: the byte mask names the first byte of the beat and the
    // whole width, not the element's bytes at its own position.
    wmask_c = 8'hFF;
`endif

    active_c    = !mask_en_q || (!is_whole && !is_mask &&
                  ((mask_byte_q >> elem_q[2:0]) & 8'd1) != 8'd0);
    need_mask_c = mask_en_q && (!mask_valid_q ||
                  (mask_byte_idx_q != 5'(elem_q[6:3])));

    limit_c = (we_q || ordered_q) ? 3'd1 : 3'(OUT_MAX);
`ifdef MOSAIC_VEC_LSU_MUTANT_ORDERED_REORDER
    // NEGATIVE CONTROL: an ordered access takes the unordered gate, so the next
    // item is offered before the previous response has arrived.
    limit_c = 3'(OUT_MAX);
`endif
    limit_hit_c = (outstanding_q + 3'd1) >= limit_c;

    // the store-data / load-destination register demand, one rule per mode
    rdw_grp_base = 5'd0;
    if (is_seg) begin
      rdw_grp_base = lmul_exp_q[3] ? data_q
                   : (data_q & ~(5'(emul_regs) - 5'd1));
      rdw_base_c = rdw_grp_base + 5'(field_q) * 5'(emul_regs);
      rdw_elem_c = elem_q[6:0];
      rdw_sew_c  = eew_log2_q;
      rdw_lmul_c = lmul_exp_q;
    end else if (is_whole) begin
      rdw_base_c = data_q;
      rdw_elem_c = elem_q[6:0];
      rdw_sew_c  = eew_log2_q;
      rdw_lmul_c = nm_nf_log2(nf_q);
    end else if (is_mask) begin
      rdw_base_c = data_q;
      rdw_elem_c = elem_q[6:0];
      rdw_sew_c  = 3'd3;
      rdw_lmul_c  = 4'd0;
    end else begin
      rdw_base_c = data_q;
      rdw_elem_c = elem_q[6:0];
      rdw_sew_c  = eew_log2_q;
      rdw_lmul_c = lmul_exp_q;
    end
  end

  // ============================================================== outputs
  assign busy_o      = busy_r;
  assign done_o      = done_r;
  assign illegal_o   = illegal_r;
  assign trap_o      = trap_r;
  assign trap_elem_o = trap_elem_r;
  assign trap_code_o = trap_code_r;
  assign stopped_o   = stopped_r;
  assign stop_elem_o = stop_elem_r;
  assign elems_o     = elems_r;
  assign req_ctr_o   = reqctr_r;

  // ======================================================== VRF read drive
  logic        rd_valid_c;
  logic [4:0]  rd_base_c;
  logic [6:0]  rd_elem_c;
  logic [2:0]  rd_sew_c;
  logic [3:0]  rd_lmul_c;
  logic [15:0] rd_tag_c;

  always_comb begin
    rd_valid_c = 1'b0;
    rd_base_c  = 5'd0;
    rd_elem_c  = 7'd0;
    rd_sew_c   = 3'd3;
    rd_lmul_c  = 4'd0;
    rd_tag_c   = TAG_MASK;

    if (state_q == ST_MASK) begin
      rd_valid_c = 1'b1;
      rd_elem_c  = {3'b000, elem_q[6:3]};
      rd_sew_c   = 3'd3;
      rd_lmul_c  = 4'd0;
      rd_tag_c   = TAG_MASK;
    end else if (state_q == ST_INDEX) begin
      rd_valid_c = 1'b1;
      rd_base_c  = index_q;
      rd_elem_c  = elem_q[6:0];
      rd_sew_c   = idx_log2_q;
      rd_lmul_c  = nm_e4(nm_i4(lmul_exp_q) + int'(idx_log2_q) - int'(eew_log2_q));
      rd_tag_c   = TAG_IDX;
    end else if (state_q == ST_DATA) begin
      rd_valid_c = 1'b1;
      rd_base_c  = rdw_base_c;
      rd_elem_c  = rdw_elem_c;
      rd_sew_c   = rdw_sew_c;
      rd_lmul_c  = rdw_lmul_c;
      rd_tag_c   = TAG_DAT;
    end
  end

  assign vrf_rd_valid_o = rd_valid_c;
  assign vrf_rd_base_o  = rd_base_c;
  assign vrf_rd_elem_o  = rd_elem_c;
  assign vrf_rd_sew_o   = rd_sew_c;
  assign vrf_rd_lmul_o  = rd_lmul_c;
  assign vrf_rd_tag_o   = rd_tag_c;

  // ======================================================= VRF write drive
  logic        wr_valid_c;
  logic [4:0]  wr_base_c;
  logic [6:0]  wr_elem_c;
  logic [2:0]  wr_sew_c;
  logic [3:0]  wr_lmul_c;
  logic [63:0] wr_data_c;

  always_comb begin
    wr_valid_c = 1'b0;
    wr_base_c  = 5'd0;
    wr_elem_c  = 7'd0;
    wr_sew_c   = 3'd3;
    wr_lmul_c  = 4'd0;
    wr_data_c  = 64'd0;

    if (wf_count_q != 3'd0) begin
      // a load's response, queued
      wr_valid_c = 1'b1;
      wr_data_c  = wf_data_q[0];
      if (is_seg) begin
        wr_base_c = (lmul_exp_q[3] ? vd_q : (vd_q & ~(5'(emul_regs) - 5'd1))) +
                    5'(wf_field_q[0]) * 5'(emul_regs);
        wr_elem_c = wf_elem_q[0];
        wr_sew_c  = eew_log2_q;
        wr_lmul_c = lmul_exp_q;
      end else if (is_whole) begin
        wr_base_c = vd_q;
        wr_elem_c = wf_elem_q[0];
        wr_sew_c  = eew_log2_q;
        wr_lmul_c = nm_nf_log2(nf_q);
      end else if (is_mask) begin
        wr_base_c = vd_q;
        wr_elem_c = wf_elem_q[0];
        wr_sew_c  = 3'd3;
        wr_lmul_c = 4'd0;
      end else begin
        wr_base_c = vd_q;
        wr_elem_c = wf_elem_q[0];
        wr_sew_c  = eew_log2_q;
        wr_lmul_c = lmul_exp_q;
      end
    end else if (state_q == ST_TWR) begin
      wr_valid_c = 1'b1;
      if (is_mask) begin
        wr_base_c = vd_q;
        wr_elem_c = t_elem_q[6:0];
        wr_sew_c  = 3'd3;
        wr_lmul_c = 4'd0;
        wr_data_c = 64'h0000_0000_0000_00FF;
      end else begin
        wr_data_c = nm_width_mask(1 << eew_log2_q);
        if (is_seg) begin
          wr_base_c = (lmul_exp_q[3] ? vd_q : (vd_q & ~(5'(emul_regs) - 5'd1))) +
                      5'(t_field_q) * 5'(emul_regs);
          wr_elem_c = t_elem_q[6:0];
          wr_sew_c  = eew_log2_q;
          wr_lmul_c = lmul_exp_q;
        end else begin
          wr_base_c = vd_q;
          wr_elem_c = t_elem_q[6:0];
          wr_sew_c  = eew_log2_q;
          wr_lmul_c = lmul_exp_q;
        end
      end
    end
  end

  assign vrf_wr_valid_o = wr_valid_c;
  assign vrf_wr_base_o  = wr_base_c;
  assign vrf_wr_elem_o  = wr_elem_c;
  assign vrf_wr_sew_o   = wr_sew_c;
  assign vrf_wr_lmul_o  = wr_lmul_c;
  assign vrf_wr_data_o  = wr_data_c;

  // ========================================================= memory port
  logic [63:0] mreq_addr_c;
  logic [7:0]  mreq_mask_c;
  logic [63:0] mreq_wdata_c;
  logic [6:0]  mreq_elem_c;
  logic [3:0]  mreq_field_c;

`ifdef MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION
  logic [63:0] merge_beat_q;
  logic [7:0]  merge_mask_q;
  logic        merge_seen_q;
`endif

  always_comb begin
    mreq_addr_c  = beat_c;
    mreq_mask_c  = wmask_c;
    mreq_wdata_c = wdata_c;
    mreq_elem_c  = elem_q[6:0];
    mreq_field_c = field_q;
    if (is_mask) begin
      mreq_addr_c  = {addr_c[63:3], 3'b000};
      mreq_mask_c  = 8'h01 << {1'b0, lane_c};
      mreq_wdata_c = data_val_q << (8 * {61'b0, lane_c});
    end
`ifdef MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION
    // NEGATIVE CONTROL: a request's byte mask absorbs the bytes of the earlier
    // items that fell in the same beat, so it can name bytes on both sides of a
    // region boundary even though no single element does.
    if (merge_seen_q && (mreq_addr_c == merge_beat_q)) begin
      mreq_mask_c = mreq_mask_c | merge_mask_q;
    end
`endif
  end

  assign mem_req_valid_o  = (state_q == ST_REQ) && !abort_q && active_c;
  assign mem_req_elem_o   = mreq_elem_c;
  assign mem_req_field_o  = mreq_field_c;
  assign mem_req_addr_o   = mreq_addr_c;
  assign mem_req_wmask_o  = mreq_mask_c;
  assign mem_req_wdata_o  = mreq_wdata_c;
  assign mem_req_we_o     = we_q;
  assign mem_req_size_o   = 4'(eew_log2_q - 3'd3);   // log2 bytes
  assign mem_req_ordered_o = ordered_q;

  // ---------------------------------------------------- item stepping
  // The next (element, field) of the main scan and of the post-pass, computed
  // combinationally so the FSM has no separate task process writing its state.
  logic [7:0] main_nelem_c;
  logic [3:0] main_nfield_c;
  logic [7:0] tail_nelem_c;
  logic [3:0] tail_nfield_c;

  always_comb begin
    if (field_q + 4'd1 >= nf_eff) begin
      main_nfield_c = 4'd0;
      main_nelem_c  = elem_q + 8'd1;
    end else begin
      main_nfield_c = field_q + 4'd1;
      main_nelem_c  = elem_q;
    end
    if (t_field_q + 4'd1 >= nf_eff) begin
      tail_nfield_c = 4'd0;
      tail_nelem_c  = t_elem_q + 8'd1;
    end else begin
      tail_nfield_c = t_field_q + 4'd1;
      tail_nelem_c  = t_elem_q;
    end
  end

  // the tail/mask policy for the current post-pass item
  function automatic logic tail_write_needed();
    begin
      if (loaded_bm_q[t_elem_q[6:0]]) tail_write_needed = 1'b0;
      else if (int'(t_elem_q) < int'(vl_q)) tail_write_needed = vma_q;
      else tail_write_needed = vta_q;
    end
  endfunction

  // ---------------------------------------------------------------- retire
  // A response and an acceptance can fall in one cycle, so the outstanding
  // count is updated once from these flags: two blocking-assignment-style
  // writes to one variable would otherwise have the later one win and lose a
  // completion.
  logic accept_c;
  logic rsp_c;
  logic wf_pop_c;
  logic wf_push_c;
  logic [1:0] wf_tail_c;
  logic [1:0] lane_tail_c;

  always_comb begin
    accept_c = (state_q == ST_REQ) && !abort_q && mem_req_ready_i;
    rsp_c    = mem_rsp_valid_i && (outstanding_q != 3'd0);
    wf_pop_c = (wf_count_q != 3'd0) && vrf_wr_gnt_i;
    // the FIFO holds at most WFIFO_D entries; the push index is the count after
    // any pop this cycle, expressed in two bits because the push is refused at
    // the full count
    wf_tail_c = 2'(wf_count_q) - (wf_pop_c ? 2'd1 : 2'd0);
    lane_tail_c = 2'(lane_count_q) - (rsp_c ? 2'd1 : 2'd0);
    wf_push_c = mem_rsp_valid_i && !we_q && !mem_rsp_fault_i && !abort_q &&
                ((wf_count_q != 3'(WFIFO_D)) || wf_pop_c);
  end

  // ================================================================= FSM
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      state_q        <= ST_IDLE;
      mode_q         <= LM_UNIT;
      we_q           <= 1'b0;
      ordered_q      <= 1'b0;
      mask_en_q      <= 1'b0;
      eew_log2_q     <= 3'd3;
      idx_log2_q     <= 3'd3;
      lmul_exp_q     <= 4'd0;
      nf_q           <= 4'd1;
      vd_q           <= 5'd0;
      data_q         <= 5'd0;
      index_q        <= 5'd0;
      base_q         <= 64'd0;
      stride_q       <= 64'd0;
      vl_q           <= 8'd0;
      vstart_q       <= 8'd0;
      vma_q          <= 1'b0;
      vta_q          <= 1'b0;
      elem_q         <= 8'd0;
      field_q        <= 4'd0;
      t_elem_q       <= 8'd0;
      t_field_q      <= 4'd0;
      loaded_bm_q    <= 128'd0;
      index_val_q    <= 64'd0;
      data_val_q     <= 64'd0;
      mask_byte_q    <= 8'd0;
      mask_byte_idx_q <= 5'd0;
      mask_valid_q   <= 1'b0;
      busy_r         <= 1'b0;
      done_r         <= 1'b0;
      illegal_r      <= 1'b0;
      trap_r         <= 1'b0;
      trap_elem_r    <= 7'd0;
      trap_code_r    <= 4'd0;
      stopped_r      <= 1'b0;
      stop_elem_r    <= 7'd0;
      stop_pending_q <= 1'b0;
      elems_r        <= 8'd0;
      reqctr_r       <= 32'd0;
      vrdctr_r       <= 32'd0;
      abort_q        <= 1'b0;
      outstanding_q  <= 3'd0;
      wf_count_q     <= 3'd0;
      vr_kind_q      <= 3'd0;
      lane_count_q   <= 3'd0;
      for (int unsigned k = 0; k < 3; ++k) lane_fifo_q[k] <= 3'd0;
      for (int unsigned k = 0; k < WFIFO_D; ++k) begin
        wf_valid_q[k] <= 1'b0;
        wf_elem_q[k]  <= 7'd0;
        wf_field_q[k] <= 4'd0;
        wf_data_q[k]  <= 64'd0;
      end
`ifdef MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION
      merge_beat_q <= 64'd0;
      merge_mask_q <= 8'd0;
      merge_seen_q <= 1'b0;
`endif
    end else begin
      done_r <= 1'b0;

      // ---- boundary stop request (I-057) -------------------------------
      // Latched here so a one-cycle request is not lost while the unit is
      // reading operands; consumed at the next item boundary in ST_SCAN.
      if (stop_i && busy_r && !abort_q) stop_pending_q <= 1'b1;

      // ---- VRF read response -------------------------------------------
      if (vrf_rd_rsp_valid_i) begin
        case (vrf_rd_rsp_tag_i)
          TAG_MASK: begin
            mask_byte_q     <= vrf_rd_rsp_data_i[7:0];
            mask_byte_idx_q <= 5'(elem_q[6:3]);
            mask_valid_q    <= 1'b1;
          end
          TAG_IDX: index_val_q <= vrf_rd_rsp_data_i;
          TAG_DAT: data_val_q  <= vrf_rd_rsp_data_i;
          default: ;
        endcase
      end

      // ---- retirement: outstanding count -------------------------------
      outstanding_q <= outstanding_q + (accept_c ? 3'd1 : 3'd0) -
                       (rsp_c ? 3'd1 : 3'd0);

      // ---- memory response ---------------------------------------------
      if (mem_rsp_valid_i) begin
        if (mem_rsp_fault_i && !abort_q) begin
          abort_q <= 1'b1;
          trap_r  <= 1'b1;
          trap_code_r <= mem_rsp_fault_code_i;
`ifdef MOSAIC_VEC_LSU_MUTANT_WHOLE_FAULT
          // NEGATIVE CONTROL: the fault is reported with vstart reset to zero.
          trap_elem_r <= 7'd0;
`else
          trap_elem_r <= mem_rsp_elem_i;
`endif
        end
      end

      // ---- lane FIFO (issue order) -------------------------------------
      if (rsp_c) begin
        for (int unsigned k = 0; k < 3; ++k) lane_fifo_q[k] <= lane_fifo_q[k+1];
      end
      if (accept_c) begin
        if (lane_count_q < 3'(WFIFO_D)) lane_fifo_q[lane_tail_c] <= lane_c;
      end
      lane_count_q <= lane_count_q + (accept_c ? 3'd1 : 3'd0) -
                      (rsp_c ? 3'd1 : 3'd0);

      // ---- write-queue push and drain ----------------------------------
      if (wf_pop_c) begin
        for (int unsigned k = 0; k < WFIFO_D - 1; ++k) begin
          wf_valid_q[k] <= wf_valid_q[k+1];
          wf_elem_q[k]  <= wf_elem_q[k+1];
          wf_field_q[k] <= wf_field_q[k+1];
          wf_data_q[k]  <= wf_data_q[k+1];
        end
        wf_valid_q[WFIFO_D-1] <= 1'b0;
      end
      if (wf_push_c) begin
        wf_valid_q[wf_tail_c] <= 1'b1;
        wf_elem_q[wf_tail_c]  <= mem_rsp_elem_i;
        wf_field_q[wf_tail_c] <= mem_rsp_field_i;
        wf_data_q[wf_tail_c]  <=
            (mem_rsp_rdata_i >> (8 * {61'b0, lane_fifo_q[0]})) &
            nm_width_mask(1 << eew_log2_q);
      end
      wf_count_q <= wf_count_q + (wf_push_c ? 3'd1 : 3'd0) -
                    (wf_pop_c ? 3'd1 : 3'd0);

`ifdef MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION
      // ---- merge bookkeeping -------------------------------------------
      if ((state_q == ST_REQ) && mem_req_ready_i && active_c && !abort_q) begin
        if (!merge_seen_q || (merge_beat_q != mreq_addr_c)) begin
          merge_seen_q <= 1'b1;
          merge_beat_q <= mreq_addr_c;
          merge_mask_q <= mreq_mask_c;
        end else begin
          merge_mask_q <= merge_mask_q | mreq_mask_c;
        end
      end
`endif

      case (state_q)
        // ------------------------------------------------------ launch
        ST_IDLE: begin
          busy_r <= 1'b0;
          if (exec_valid_i) begin
            busy_r      <= 1'b1;
            illegal_r   <= 1'b0;
            trap_r      <= 1'b0;
            trap_elem_r <= 7'd0;
            trap_code_r <= 4'd0;
            stopped_r   <= 1'b0;
            stop_elem_r <= 7'd0;
            stop_pending_q <= 1'b0;
            elems_r     <= 8'd0;
            reqctr_r    <= 32'd0;
            vrdctr_r    <= 32'd0;
            abort_q     <= 1'b0;
            outstanding_q <= 3'd0;
            wf_count_q  <= 3'd0;
            loaded_bm_q <= 128'd0;
            mask_valid_q <= 1'b0;
            index_val_q <= 64'd0;
            data_val_q  <= 64'd0;

            mode_q     <= exec_mode_i;
            we_q       <= exec_we_i;
            ordered_q  <= exec_ordered_i;
            nf_q       <= (exec_nf_i == 4'd0) ? 4'd1 : exec_nf_i;
            vd_q       <= exec_vd_i;
            data_q     <= ((exec_we_i || (exec_mode_i == LM_WHOLE) ||
                            (exec_mode_i == LM_MASK)) ? exec_data_i : exec_vd_i);
            index_q    <= exec_index_i;
            idx_log2_q <= exec_idx_sew_i;
            eew_log2_q <= cfg_vsew;
            lmul_exp_q <= nm_e4(cfg_lmul_exp);
            base_q     <= exec_base_i;
            stride_q   <= exec_stride_i;
            vma_q      <= cfg_vma;
            vta_q      <= cfg_vta;
            vl_q       <= cfg_vl_i;
            vstart_q   <= {1'b0, cfg_vstart_i};
            mask_en_q  <= exec_mask_en_i && (exec_mode_i != LM_WHOLE) &&
                          (exec_mode_i != LM_MASK);
            elem_q     <= {1'b0, cfg_vstart_i};
            field_q    <= 4'd0;
            t_elem_q   <= 8'd0;
            t_field_q  <= 4'd0;

            if (exec_mode_i > LM_MASK) begin
              illegal_r <= 1'b1;
              state_q   <= ST_DONE;
            end else if (!caps_i[exec_mode_i[2:0]]) begin
              illegal_r <= 1'b1;
              state_q   <= ST_DONE;
            end else if ((int'(cfg_vsew) < 3) || (int'(cfg_vsew) > 6) ||
                         ((1 << int'(cfg_vsew)) > int'(ELEN))) begin
              illegal_r <= 1'b1;
              state_q   <= ST_DONE;
            end else if ((exec_mode_i == LM_WHOLE) &&
                         !((exec_nf_i == 4'd1) || (exec_nf_i == 4'd2) ||
                           (exec_nf_i == 4'd4) || (exec_nf_i == 4'd8))) begin
              illegal_r <= 1'b1;
              state_q   <= ST_DONE;
            end else if ((exec_mode_i != LM_WHOLE) && (exec_mode_i != LM_MASK) &&
                         (cfg_vlmax < 1)) begin
              illegal_r <= 1'b1;
              state_q   <= ST_DONE;
            end else if ((exec_mode_i == LM_INDEXED) ||
                         (exec_mode_i == LM_SEG_INDEXED)) begin
              if ((int'(exec_idx_sew_i) < 3) || (int'(exec_idx_sew_i) > 6)) begin
                illegal_r <= 1'b1;
                state_q   <= ST_DONE;
              end else if ((int'(exec_idx_sew_i) + cfg_lmul_exp - int'(cfg_vsew) > 3) ||
                           (int'(exec_idx_sew_i) + cfg_lmul_exp - int'(cfg_vsew) < -3)) begin
                illegal_r <= 1'b1;
                state_q   <= ST_DONE;
              end else begin
                state_q <= ST_SCAN;
              end
            end else begin
              state_q <= ST_SCAN;
            end
          end
        end

        // ------------------------------------------------- scan next item
        ST_SCAN: begin
          if (int'(elem_q) >= elem_end) begin
            state_q <= ST_DRAIN;
          end else if (stop_pending_q && !abort_q) begin
            // the boundary stop: nothing at or after elem_q has been offered,
            // so elem_q is the first element not performed
            stopped_r   <= 1'b1;
            stop_elem_r <= elem_q[6:0];
            abort_q     <= 1'b1;
            state_q     <= ST_WAIT;
          end else if (need_mask_c) begin
            vr_kind_q <= 3'd1;
            state_q   <= ST_MASK;
          end else if (!active_c) begin
            elem_q  <= main_nelem_c;
            field_q <= main_nfield_c;
          end else if (is_indexed) begin
            vr_kind_q <= 3'd2;
            state_q   <= ST_INDEX;
          end else if (we_q) begin
            vr_kind_q <= 3'd3;
            state_q   <= ST_DATA;
          end else begin
            state_q <= ST_REQ;
          end
        end

        ST_MASK, ST_INDEX, ST_DATA: begin
          if (vrf_rd_gnt_i) begin
            vrdctr_r <= vrdctr_r + 32'd1;
            state_q  <= ST_VRD;
          end
        end

        ST_VRD: begin
          if (vrf_rd_rsp_valid_i) begin
            if (vr_kind_q == 3'd1) state_q <= ST_SCAN;
            else if ((vr_kind_q == 3'd2) && we_q) begin
              vr_kind_q <= 3'd3;
              state_q   <= ST_DATA;   // an indexed store reads its data too
            end else begin
              state_q <= ST_REQ;
            end
          end
        end

        // ------------------------------------------------- offer request
        ST_REQ: begin
          if (abort_q) begin
            state_q <= ST_WAIT;
          end else if (mem_req_ready_i) begin
            reqctr_r      <= reqctr_r + 32'd1;
            elems_r       <= elems_r + 8'd1;
            if (elem_q <= 8'd127) loaded_bm_q[elem_q[6:0]] <= 1'b1;
            if (limit_hit_c) begin
              state_q <= ST_WAIT;
            end else begin
              elem_q  <= main_nelem_c;
              field_q <= main_nfield_c;
              state_q <= ST_SCAN;
            end
          end else if (stop_pending_q) begin
            // the memory cannot take this item and a boundary stop is pending:
            // stop before it, so nothing at elem_q is performed
            stopped_r   <= 1'b1;
            stop_elem_r <= elem_q[6:0];
            abort_q     <= 1'b1;
            state_q     <= ST_WAIT;
          end
        end

        // ----------------------------------------------------------- wait
        ST_WAIT: begin
          if (abort_q) begin
            if ((outstanding_q == 3'd0) && (wf_count_q == 3'd0)) state_q <= ST_DONE;
          end else if (outstanding_q < limit_c) begin
            elem_q  <= main_nelem_c;
            field_q <= main_nfield_c;
            state_q <= ST_SCAN;
          end
        end

        // -------------------------------------------------- drain queues
        ST_DRAIN: begin
          if ((outstanding_q == 3'd0) && (wf_count_q == 3'd0)) begin
            t_elem_q  <= is_mask ? 8'((int'(vl_q) + 7) >> 3) : vstart_q;
            t_field_q <= 4'd0;
            state_q   <= ST_TAIL;
          end
        end

        // ------------------------------------------------------ tail pass
        ST_TAIL: begin
          if (abort_q || we_q || is_whole) begin
            state_q <= ST_DONE;
          end else if (is_mask) begin
            state_q <= (int'(t_elem_q) >= (VLEN / 8)) ? ST_DONE : ST_TWR;
          end else if (int'(t_elem_q) >= tail_end) begin
            state_q <= ST_DONE;
          end else if (tail_write_needed()) begin
            state_q <= ST_TWR;
          end else begin
            t_elem_q  <= tail_nelem_c;
            t_field_q <= tail_nfield_c;
          end
        end

        ST_TWR: begin
          if (vrf_wr_gnt_i) begin
            t_elem_q  <= tail_nelem_c;
            t_field_q <= tail_nfield_c;
            state_q   <= ST_TAIL;
          end
        end

        // --------------------------------------------------------- done
        ST_DONE: begin
          busy_r  <= 1'b0;
          done_r  <= 1'b1;
          state_q <= ST_IDLE;
        end

        default: state_q <= ST_IDLE;
      endcase
    end
  end

  // unused tie-off
  logic unused_ok;
  assign unused_ok = (rdw_grp_base[4:0] == 5'd0) | (rd_base_c[4]);

endmodule

`resetall
`default_nettype wire
