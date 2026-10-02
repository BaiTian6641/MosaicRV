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
// One request is issued per item, in ascending item order, unless coalescing is
// enabled (I-061, below). With `exec_coalesce_i` low the first correct version
// asks for exactly the bytes of one element; the request count is therefore the
// item count, which is what makes a merge observable.
//
// ---------------------------------------------------- line coalescing (I-061)
//
// `exec_coalesce_i` enables same-hart coalescing: a run of consecutive items of
// one macro that fall in the same 8-byte memory beat becomes **one** request
// with the union of their byte masks, and the response is distributed back to
// the members. The memory port's transaction is one 8-byte beat, so the beat is
// the largest unit a single request can name; a full 32-byte cache line cannot
// be reached through this port and is not claimed.
//
// The mergeability rule, and each clause is why the merged result is identical
// to the uncoalesced one:
//
//   * the macro is coalescing and is not an atomic class access
//     (`exec_atomic_i`), because an AMO is a serialization point;
//   * it is not one of the ordered indexed forms, whose per-element ordering
//     contract a merged request would break;
//   * it is a unit-stride or constant-stride item (the indexed forms read a
//     per-element operand and are left uncoalesced);
//   * every byte the item enables is **normal memory** as the *generated*
//     platform map defines normal (`mosaic_cfg_pkg::mosaic_pa_normal`:
//     idempotent and not a device). An MMIO or non-idempotent byte can never be
//     merged, and neither can an uncovered address. The predicate is the map's,
//     never a hand-written list, exactly as the cache and the LLB take theirs;
//   * for a store, the item's bytes are **disjoint** from the group's. Two
//     stores that touch the same byte are therefore never merged, so the memory
//     side sees them one at a time in element order.
//
// The group's request carries the first member's logical index and the merged
// byte mask; the group descriptor (its members' index, field and lane) rides an
// in-order FIFO beside the responses. A load's beat is handed to each member in
// turn through the existing write-back queue, so every element keeps its own
// mask and destination slot. `merge_ctr_o` counts the elements a merge removed
// from the request stream (a group counts only once it completes without a
// fault), and `req_ctr_o` counts the requests actually offered.
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
// A coalesced group that faults keeps that granularity: a merged request cannot
// say which of its members faulted, so the group is **replayed element by
// element, in element order**, at most one request outstanding. The elements
// before the faulting one are then performed individually (a store applied, a
// load written back) and the faulting member reports its own index. A group
// that faults therefore removes nothing from the request stream -- its members
// are re-issued -- which is why `merge_ctr_o` counts only groups that complete.
// (A fault that is not a property of the addressed bytes would not reproduce
// element by element; the fallback reports it at the group's first element
// rather than dropping it.)
//
// A whole-macro trap that reports no element index, or reports `vstart` = 0
// for a fault at element k, is not acceptable for a unit-stride or strided
// load, and the negative control `MOSAIC_VEC_LSU_MUTANT_WHOLE_FAULT` is exactly
// that defect.
//
// ------------------------------------------------- repeated-address stores
//
// Two stores to the same address inside one macro apply in the order the
// instruction type defines: **element order**. The ordered forms require it;
// the unordered forms are implemented in element order too, because that is the
// only order under which the coalesced and uncoalesced results are identical.
// The order is a property of the instruction's access class and of the item
// sequence, never of a lane number or of the order requests happen to complete:
// overlapping-byte stores are simply not merged, so the merge never has to pick
// a winner.
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
// While coalescing is enabled one request is kept in flight, because a merged
// load's beat is distributed to its members one per cycle; the response stream
// stays in order, which is what the group-descriptor FIFO relies on.
//
// The port carries the element identity on both the request and the response,
// and the request stream on `mem_req_*` is what a case observes, so the order
// is visible on the memory port rather than merely asserted.
//
// ------------------------------------------------------ no merge across regions
//
// The coalescer never merges a byte the generated platform map does not call
// normal memory, so a request can never name both a device byte and a RAM byte:
// a member joins only when every byte it enables answers
// `mosaic_cfg_pkg::mosaic_pa_normal`, and the group's mask is the union of
// members that each passed. The I-056 case still proves the property directly
// too, by requiring every uncoalesced request's enabled bytes to lie in one
// region; the negative control `MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION` adds a
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
// Eight `-DMOSAIC_VEC_LSU_MUTANT_*` defines inject one defect each; the
// shipping build defines none. The first four are proven to fail
// CASE=rvv.memory_modes in results/reports/I-056-vector-memory.md, the last
// four to fail CASE=coalesce.element_faults in
// results/reports/I-061-coalescing.md:
//
//   MERGE_CROSS_REGION  the byte mask of each request absorbs the bytes of the
//                       earlier items of the same beat, so a request can name
//                       bytes on both sides of a region boundary
//   WHOLE_FAULT         a fault is reported with vstart reset to zero instead
//                       of the faulting element's index
//   MASK_WRONG          the byte mask drops the element's lane position and
//                       width, so the enabled bytes are not the element's
//                       own
//   ORDERED_REORDER     the ordered indexed accesses take the unordered
//                       ordering gate, so element i+1 can be issued before
//                       element i has completed
//   COALESCE_MASK_LOSS    a joining element's byte mask is dropped from the
//                         merged group
//   COALESCE_GROUP_FAULT  a fault on a coalesced group is reported for the
//                         group instead of being replayed element by element
//   COALESCE_DEVICE       the platform map is not consulted, so a device or
//                         MMIO element is merged with its beat-mates
//   COALESCE_STORE_ORDER  overlapping-byte stores are merged, so a
//                         repeated-address pair applies out of element order
// ============================================================================

`default_nettype none
`resetall

// The platform map: the coalescer's device/cacheability predicates are the
// generated map's (mosaic_pa_normal), never a hand-written list, so this module
// and the SoC device decode cannot disagree about where a device lives.
/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

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
    // ------------------------------------------------- coalescing (I-061)
    // `exec_coalesce_i` enables same-hart, same-beat line coalescing for a
    // macro; `exec_atomic_i` marks the macro as an atomic class access, which is
    // never coalesced. Both are per-macro. With `exec_coalesce_i` low the unit
    // issues exactly one request per item, which is the behaviour every I-056
    // case observes.
    input  logic                    exec_coalesce_i,
    input  logic                    exec_atomic_i,

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
    // --------------------------------------------------- coalescer counters
    // `req_ctr_o` counts requests offered on the memory port (a replay request
    // counts too); `merge_ctr_o` counts the *elements* a merge removed from the
    // request stream, so `merge_ctr_o` is the transaction reduction. Both are
    // zero when `exec_coalesce_i` is low.
    output logic [31:0]             merge_ctr_o,

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

  // --------------------------------------------------------- coalescing (I-061)
  // A coalesced group is a contiguous run of active items that share one 8-byte
  // beat. At most eight 1-byte elements fit a beat, so a group is at most eight
  // members; the group descriptors are kept in an in-order FIFO beside the
  // responses (a response is returned in issue order).
  localparam int unsigned COAL_MAX = 8;  // members in one coalesced group
  localparam int unsigned GF_D     = 4;  // in-flight group descriptors

  localparam logic [3:0] ST_CUR   = 4'd12;   // a ready item waits to join/flush
  localparam logic [3:0] ST_DIST  = 4'd13;   // distribute a coalesced load

  // where ST_REQ returns after the request is accepted
  localparam logic [1:0] RET_SCAN  = 2'd0;   // advance the item, then ST_SCAN
  localparam logic [1:0] RET_CUR   = 2'd1;   // re-evaluate the waiting item
  localparam logic [1:0] RET_DRAIN = 2'd2;   // the macro's items are exhausted
  localparam logic [1:0] RET_STOP  = 2'd3;   // issue the pending group, then stop

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

  // ------------------------------------------------- per-element normal check
  // An element may be coalesced only if every byte it enables is normal memory,
  // as the generated platform map defines normal: idempotent and not a device.
  // A byte in a device/MMIO region, or one no region covers, answers no.
  function automatic logic nm_bytes_normal(input logic [63:0] beat, input logic [7:0] mask);
    logic ok;
    begin
      ok = 1'b1;
      for (int unsigned k = 0; k < 8; ++k) begin
        if (mask[k]) ok = ok && mosaic_cfg_pkg::mosaic_pa_normal(beat + 64'(k));
      end
      nm_bytes_normal = ok;
    end
  endfunction

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

  // --------------------------------------------------------- coalescing (I-061)
  logic        coalesce_q;         // coalescing enabled for this macro
  logic        atomic_q;           // atomic class: never coalesced
  logic [1:0]  ret_q;              // where ST_REQ returns on acceptance
  logic [31:0] merge_r;            // elements removed from the request stream

  // the pending coalesced group: one (beat, mask, data) with up to COAL_MAX
  // members. `acc_beat_q` is the shared 8-byte beat; the members are stored
  // packed, `acc_n_q` of them valid.
  logic        acc_valid_q;
  logic [63:0] acc_beat_q;
  logic [7:0]  acc_mask_q;
  logic [63:0] acc_wdata_q;
  logic [6:0]  acc_lead_elem_q;
  logic [3:0]  acc_lead_field_q;
  logic [3:0]  acc_n_q;
  logic [COAL_MAX*7-1:0] acc_me_q;
  logic [COAL_MAX*4-1:0] acc_mf_q;
  logic [COAL_MAX*3-1:0] acc_ml_q;

  // in-order group descriptors, one per outstanding request
  logic        gf_coal_q  [0:GF_D-1];
  logic [3:0]  gf_n_q     [0:GF_D-1];
  logic [COAL_MAX*7-1:0] gf_me_q [0:GF_D-1];
  logic [COAL_MAX*4-1:0] gf_mf_q [0:GF_D-1];
  logic [COAL_MAX*3-1:0] gf_ml_q [0:GF_D-1];
  logic [2:0]  gf_count_q;

  // return distributor: a coalesced load response is handed to its members one
  // per cycle through the existing write-back queue
  logic        dist_valid_q;
  logic [3:0]  dist_idx_q;
  logic [3:0]  dist_n_q;
  logic [63:0] dist_rdata_q;
  logic [COAL_MAX*7-1:0] dist_me_q;
  logic [COAL_MAX*3-1:0] dist_ml_q;

  // de-coalesce on fault: replay the group's items individually so the fault
  // names the element that caused it and only the elements before it take effect
  logic        replay_q;
  logic [6:0]  replay_first_q;
  logic [6:0]  replay_last_q;
  logic [3:0]  replay_code_q;

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
    // A coalesced group is one request covering many items; its return is
    // distributed one member per cycle, so the unit keeps a single request in
    // flight while coalescing (the response then needs no concurrent push).
    if (coalesce_q) limit_c = 3'd1;
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

  // ======================================================= coalescing rules
  // The mergeability rule, stated once, in the module header:
  //
  //   A group is a contiguous run of active items of one macro that share one
  //   8-byte beat. An item joins only if the macro is coalescing, is not an
  //   atomic class access, is not one of the ordered forms, is a unit-stride or
  //   constant-stride item, and every byte it enables is normal memory as the
  //   generated platform map defines normal (idempotent, not a device). A store
  //   joins only if its bytes are disjoint from the group's, so two overlapping
  //   stores are never merged and element order is what the memory side sees.
  logic        coal_eff_c;
  logic        merge_ok_c;
  logic        store_overlap_c;
  logic        can_join_c;
  logic [7:0]  acc_mask_n_c;
  int          scan_end_c;
  logic [6:0]  gf0_first_elem_c;
  logic [6:0]  gf0_last_elem_c;

  always_comb begin
    coal_eff_c    = coalesce_q && !replay_q;
    merge_ok_c    = coal_eff_c && !atomic_q && !ordered_q &&
                    !is_seg && !is_indexed && !is_whole && !is_mask &&
                    active_c && nm_bytes_normal(beat_c, wmask_c);
`ifdef MOSAIC_VEC_LSU_MUTANT_COALESCE_DEVICE
    // NEGATIVE CONTROL: the platform map is not consulted, so a device or MMIO
    // element is merged with normal memory.
    merge_ok_c = coal_eff_c && !atomic_q && !ordered_q &&
                 !is_seg && !is_indexed && !is_whole && !is_mask && active_c;
`endif
    store_overlap_c = we_q && ((acc_mask_q & wmask_c) != 8'd0);
`ifdef MOSAIC_VEC_LSU_MUTANT_COALESCE_STORE_ORDER
    // NEGATIVE CONTROL: overlapping-byte stores are merged too, so the winning
    // value is the group's accumulated data (the earlier store) rather than the
    // later element's.
    store_overlap_c = 1'b0;
`endif
    can_join_c = merge_ok_c && acc_valid_q && (acc_beat_q == beat_c) &&
                 !store_overlap_c && (acc_n_q < 4'(COAL_MAX));
    acc_mask_n_c = acc_mask_q | wmask_c;
`ifdef MOSAIC_VEC_LSU_MUTANT_COALESCE_MASK_LOSS
    // NEGATIVE CONTROL: a joining member's byte mask is dropped from the group.
    acc_mask_n_c = acc_mask_q;
`endif
    scan_end_c = replay_q ? (int'(replay_last_q) + 1) : elem_end;

    gf0_first_elem_c  = gf_me_q[0][0 +: 7];
    gf0_last_elem_c   = (gf_n_q[0] > 4'd1) ?
        gf_me_q[0][(gf_n_q[0] - 4'd1) * 7 +: 7] : gf_me_q[0][0 +: 7];
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
  assign merge_ctr_o = merge_r;

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
  // A coalesced load's member is written straight to the VRF, one per cycle, so
  // the write-back queue only ever holds size-1 responses. The queue drains
  // first, so a member never displaces a queued response.
  logic dist_wr_c;

  always_comb begin
    dist_wr_c = (state_q == ST_DIST) && dist_valid_q && (dist_idx_q < dist_n_q) &&
                (wf_count_q == 3'd0);
  end

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
    end else if (dist_wr_c) begin
      // a coalesced load's member: extract its bytes from the response beat and
      // write its own destination element
      wr_valid_c = 1'b1;
      wr_base_c  = vd_q;
      wr_elem_c  = dist_me_q[dist_idx_q * 7 +: 7];
      wr_sew_c   = eew_log2_q;
      wr_lmul_c  = lmul_exp_q;
      wr_data_c  = (dist_rdata_q >> (8 * {61'b0, dist_ml_q[dist_idx_q * 3 +: 3]})) &
                   nm_width_mask(1 << eew_log2_q);
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
    if (coal_eff_c) begin
      // the issued request is the coalesced group, not the item being scanned
      mreq_addr_c  = acc_beat_q;
      mreq_mask_c  = acc_mask_q;
      mreq_wdata_c = acc_wdata_q;
      mreq_elem_c  = acc_lead_elem_q;
      mreq_field_c = acc_lead_field_q;
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

  assign mem_req_valid_o  = (state_q == ST_REQ) && !abort_q &&
                            (coal_eff_c ? acc_valid_q : active_c);
  assign mem_req_elem_o   = mreq_elem_c;
  assign mem_req_field_o  = mreq_field_c;
  assign mem_req_addr_o   = mreq_addr_c;
  assign mem_req_wmask_o  = mreq_mask_c;
  assign mem_req_wdata_o  = mreq_wdata_c;
  assign mem_req_we_o     = we_q;
  // a real merge spans the 8-byte beat; a size-1 group keeps the element's size
  assign mem_req_size_o   = (coal_eff_c && (acc_n_q > 4'd1)) ? 4'd3
                                                           : 4'(eew_log2_q - 3'd3);
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
  logic wf_room_c;
  logic rsp_push_c;
  logic [1:0] gf_tail_c;

  always_comb begin
    accept_c = (state_q == ST_REQ) && !abort_q && mem_req_ready_i;
    rsp_c    = mem_rsp_valid_i && (outstanding_q != 3'd0);
    wf_pop_c = (wf_count_q != 3'd0) && vrf_wr_gnt_i;
    // the FIFO holds at most WFIFO_D entries; the push index is the count after
    // any pop this cycle, expressed in two bits because the push is refused at
    // the full count
    wf_tail_c = 2'(wf_count_q) - (wf_pop_c ? 2'd1 : 2'd0);
    gf_tail_c = 2'(gf_count_q) - (rsp_c ? 2'd1 : 2'd0);
    lane_tail_c = 2'(lane_count_q) - (rsp_c ? 2'd1 : 2'd0);
    wf_room_c = (wf_count_q != 3'(WFIFO_D)) || wf_pop_c;
    // one write-back per response for a size-1 group; a coalesced group is
    // distributed one member per cycle straight to the VRF by ST_DIST, so the
    // write-back queue only ever holds size-1 entries (and always has room)
    rsp_push_c = rsp_c && !we_q && !mem_rsp_fault_i && !abort_q &&
                 !(gf_coal_q[0] && (gf_n_q[0] > 4'd1)) && wf_room_c;
    wf_push_c = rsp_push_c;
  end


  // a coalesced response either de-coalesces on a fault (replay) or is handed
  // to its members (distribution success)
  logic rsp_replay_c;
  logic rsp_dist_c;

  always_comb begin
    rsp_replay_c = rsp_c && mem_rsp_fault_i && !abort_q && !replay_q &&
                   gf_coal_q[0] && (gf_n_q[0] > 4'd1);
    rsp_dist_c   = rsp_c && !mem_rsp_fault_i && !we_q && !replay_q &&
                   gf_coal_q[0] && (gf_n_q[0] > 4'd1);
`ifdef MOSAIC_VEC_LSU_MUTANT_COALESCE_GROUP_FAULT
    rsp_replay_c = 1'b0;   // NEGATIVE CONTROL: report the whole group instead
`endif
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
      coalesce_q     <= 1'b0;
      atomic_q       <= 1'b0;
      ret_q          <= RET_SCAN;
      merge_r        <= 32'd0;
      acc_valid_q    <= 1'b0;
      acc_beat_q     <= 64'd0;
      acc_mask_q     <= 8'd0;
      acc_wdata_q    <= 64'd0;
      acc_lead_elem_q  <= 7'd0;
      acc_lead_field_q <= 4'd0;
      acc_n_q        <= 4'd0;
      acc_me_q       <= {COAL_MAX*7{1'b0}};
      acc_mf_q       <= {COAL_MAX*4{1'b0}};
      acc_ml_q       <= {COAL_MAX*3{1'b0}};
      gf_count_q     <= 3'd0;
      dist_valid_q   <= 1'b0;
      dist_idx_q     <= 4'd0;
      dist_n_q       <= 4'd0;
      dist_rdata_q   <= 64'd0;
      dist_me_q      <= {COAL_MAX*7{1'b0}};
      dist_ml_q      <= {COAL_MAX*3{1'b0}};
      replay_q       <= 1'b0;
      replay_first_q <= 7'd0;
      replay_last_q  <= 7'd0;
      replay_code_q  <= 4'd0;
      for (int unsigned k = 0; k < GF_D; ++k) begin
        gf_coal_q[k] <= 1'b0;
        gf_n_q[k]    <= 4'd0;
        gf_me_q[k]   <= {COAL_MAX*7{1'b0}};
        gf_mf_q[k]   <= {COAL_MAX*4{1'b0}};
        gf_ml_q[k]   <= {COAL_MAX*3{1'b0}};
      end
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
      if (mem_rsp_valid_i && (outstanding_q != 3'd0)) begin
        if (mem_rsp_fault_i && !abort_q) begin
          // a fault supersedes a boundary stop that was decided but whose group
          // is still in flight: `stopped_o` and `trap_o` are never both set
          stopped_r <= 1'b0;
          if (rsp_replay_c) begin
            // A coalesced request faulted as a unit. A merged request cannot
            // say which element faulted, so the group is de-coalesced: its
            // items are replayed one at a time, in element order, which
            // performs the elements before the faulting one and lets the
            // faulting one report its own index. Hiding a per-element access
            // check is exactly what a group-level fault would do.
            replay_q        <= 1'b1;
            replay_first_q  <= gf0_first_elem_c;
            replay_last_q   <= gf0_last_elem_c;
            replay_code_q   <= mem_rsp_fault_code_i;
            elem_q   <= {1'b0, gf0_first_elem_c};
            field_q  <= 4'd0;
          end else begin
            abort_q <= 1'b1;
            trap_r  <= 1'b1;
            trap_code_r <= mem_rsp_fault_code_i;
`ifdef MOSAIC_VEC_LSU_MUTANT_WHOLE_FAULT
            // NEGATIVE CONTROL: the fault is reported with vstart reset to zero.
            trap_elem_r <= 7'd0;
`elsif MOSAIC_VEC_LSU_MUTANT_COALESCE_GROUP_FAULT
            // NEGATIVE CONTROL: a coalesced fault is reported for the group --
            // at its first element -- instead of the element that caused it.
            trap_elem_r <= gf_coal_q[0] ? gf_me_q[0][0 +: 7] : mem_rsp_elem_i;
`else
            trap_elem_r <= mem_rsp_elem_i;
`endif
          end
        end else if (rsp_dist_c) begin
          // a coalesced load succeeded: hand the beat to each member in turn
          dist_valid_q <= 1'b1;
          dist_idx_q   <= 4'd0;
          dist_n_q     <= gf_n_q[0];
          dist_rdata_q <= mem_rsp_rdata_i;
          dist_me_q    <= gf_me_q[0];
          dist_ml_q    <= gf_ml_q[0];
        end
      end

      // ---- lane FIFO (issue order) -------------------------------------
      if (rsp_c) begin
        for (int unsigned k = 0; k < 3; ++k) lane_fifo_q[k] <= lane_fifo_q[k+1];
      end
      if (accept_c) begin
        // a coalesced request's response is extracted with the *group's* lane,
        // not the lane of the item the scan has moved on to
        if (lane_count_q < 3'(WFIFO_D)) begin
          lane_fifo_q[lane_tail_c] <= (coal_eff_c && acc_valid_q) ? acc_ml_q[0 +: 3] : lane_c;
        end
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

      // ---- group descriptors, in issue order (a response is in order) ---
      if (rsp_c) begin
        for (int unsigned k = 0; k < GF_D - 1; ++k) begin
          gf_coal_q[k] <= gf_coal_q[k+1];
          gf_n_q[k]    <= gf_n_q[k+1];
          gf_me_q[k]   <= gf_me_q[k+1];
          gf_mf_q[k]   <= gf_mf_q[k+1];
          gf_ml_q[k]   <= gf_ml_q[k+1];
        end
        gf_coal_q[GF_D-1] <= 1'b0;
        gf_n_q[GF_D-1]    <= 4'd0;
      end
      if (accept_c && (gf_count_q < 3'(GF_D))) begin
        gf_coal_q[gf_tail_c] <= coal_eff_c && (acc_n_q > 4'd1);
        gf_n_q[gf_tail_c]    <= coal_eff_c ? acc_n_q : 4'd1;
        gf_me_q[gf_tail_c]   <= coal_eff_c ? acc_me_q : {COAL_MAX*7{1'b0}};
        gf_mf_q[gf_tail_c]   <= coal_eff_c ? acc_mf_q : {COAL_MAX*4{1'b0}};
        gf_ml_q[gf_tail_c]   <= coal_eff_c ? acc_ml_q : {COAL_MAX*3{1'b0}};
      end
      gf_count_q <= gf_count_q + (accept_c ? 3'd1 : 3'd0) - (rsp_c ? 3'd1 : 3'd0);

      // ---- the transaction reduction: an element is "merged" once the group
      // it joined has completed without a fault. A group that faults is
      // replayed member by member, so it removed nothing.
      if (rsp_c && !mem_rsp_fault_i && gf_coal_q[0] && (gf_n_q[0] > 4'd1)) begin
        merge_r <= merge_r + {28'd0, gf_n_q[0]} - 32'd1;
      end

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
            merge_r     <= 32'd0;
            abort_q     <= 1'b0;
            outstanding_q <= 3'd0;
            wf_count_q  <= 3'd0;
            loaded_bm_q <= 128'd0;
            mask_valid_q <= 1'b0;
            index_val_q <= 64'd0;
            data_val_q  <= 64'd0;
            coalesce_q  <= exec_coalesce_i;
            atomic_q    <= exec_atomic_i;
            ret_q       <= RET_SCAN;
            acc_valid_q <= 1'b0;
            acc_n_q     <= 4'd0;
            gf_count_q  <= 3'd0;
            dist_valid_q <= 1'b0;
            replay_q    <= 1'b0;

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
          if (int'(elem_q) >= scan_end_c) begin
            if (replay_q) begin
              // The replay range is exhausted and no member faulted: the
              // group's fault did not reproduce element by element, so it is
              // reported for the group's first element rather than dropped.
              replay_q    <= 1'b0;
              abort_q     <= 1'b1;
              trap_r      <= 1'b1;
              trap_elem_r <= replay_first_q;
              trap_code_r <= replay_code_q;
              state_q     <= ST_WAIT;
            end else if (coalesce_q && acc_valid_q) begin
              ret_q   <= RET_DRAIN;
              state_q <= ST_REQ;
            end else begin
              state_q <= ST_DRAIN;
            end
          end else if (stop_pending_q && !abort_q && !replay_q) begin
            // the boundary stop: nothing at or after elem_q may be offered, so
            // elem_q is the first element not performed. A pending group's
            // items lie before that boundary and must be performed first.
            stopped_r   <= 1'b1;
            stop_elem_r <= elem_q[6:0];
            if (coalesce_q && acc_valid_q) begin
              ret_q   <= RET_STOP;
              state_q <= ST_REQ;
            end else begin
              abort_q <= 1'b1;
              state_q <= ST_WAIT;
            end
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
          end else if (coal_eff_c) begin
            state_q <= ST_CUR;
          end else begin
            ret_q   <= RET_SCAN;
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
            end else if (coal_eff_c) begin
              state_q <= ST_CUR;
            end else begin
              ret_q   <= RET_SCAN;
              state_q <= ST_REQ;
            end
          end
        end

        // ------------------------------------------ a ready item (I-061)
        // The item's address, mask and data are in hand. It joins the pending
        // group when the mergeability rule admits it; otherwise the pending
        // group is issued first (the item waits) or this item is issued alone.
        ST_CUR: begin
          if (abort_q) begin
            state_q <= ST_WAIT;
          end else if (can_join_c) begin
            acc_mask_q  <= acc_mask_n_c;
            acc_wdata_q <= acc_wdata_q | wdata_c;
            acc_me_q[acc_n_q * 7 +: 7] <= elem_q[6:0];
            acc_mf_q[acc_n_q * 4 +: 4] <= field_q;
            acc_ml_q[acc_n_q * 3 +: 3] <= lane_c;
            acc_n_q     <= acc_n_q + 4'd1;
            elem_q      <= main_nelem_c;
            field_q     <= main_nfield_c;
            state_q     <= ST_SCAN;
          end else if (acc_valid_q) begin
            ret_q   <= RET_CUR;
            state_q <= ST_REQ;
          end else begin
            acc_valid_q      <= 1'b1;
            acc_beat_q       <= beat_c;
            acc_mask_q       <= wmask_c;
            acc_wdata_q      <= wdata_c;
            acc_lead_elem_q  <= elem_q[6:0];
            acc_lead_field_q <= field_q;
            acc_me_q[0 +: 7] <= elem_q[6:0];
            acc_mf_q[0 +: 4] <= field_q;
            acc_ml_q[0 +: 3] <= lane_c;
            acc_n_q          <= 4'd1;
            if (merge_ok_c) begin
              elem_q  <= main_nelem_c;
              field_q <= main_nfield_c;
              state_q <= ST_SCAN;
            end else begin
              ret_q   <= RET_SCAN;
              state_q <= ST_REQ;
            end
          end
        end

        // ------------------------------------------------- offer request
        ST_REQ: begin
          if (abort_q) begin
            state_q <= ST_WAIT;
          end else if (mem_req_ready_i) begin
            reqctr_r <= reqctr_r + 32'd1;
            if (coal_eff_c) begin
              elems_r <= elems_r + {4'd0, acc_n_q};
              for (int unsigned k = 0; k < COAL_MAX; ++k) begin
                if (4'(k) < acc_n_q) loaded_bm_q[acc_me_q[k * 7 +: 7]] <= 1'b1;
              end
              acc_valid_q <= 1'b0;
            end else begin
              elems_r <= elems_r + 8'd1;
              if (elem_q <= 8'd127) loaded_bm_q[elem_q[6:0]] <= 1'b1;
            end
            if (limit_hit_c) begin
              state_q <= ST_WAIT;
            end else begin
              case (ret_q)
                RET_CUR:   state_q <= ST_CUR;
                RET_DRAIN: state_q <= ST_DRAIN;
                RET_STOP: begin
                  abort_q <= 1'b1;
                  state_q <= ST_WAIT;
                end
                default: begin
                  elem_q  <= main_nelem_c;
                  field_q <= main_nfield_c;
                  state_q <= ST_SCAN;
                end
              endcase
            end
          end else if (stop_pending_q && !(coal_eff_c && acc_valid_q)) begin
            // the memory cannot take this item and a boundary stop is pending:
            // stop before it, so nothing at elem_q is performed. A coalesced
            // group is different -- its items lie *before* the boundary and must
            // be performed -- so the unit waits for the memory instead.
            stopped_r   <= 1'b1;
            stop_elem_r <= elem_q[6:0];
            abort_q     <= 1'b1;
            state_q     <= ST_WAIT;
          end
        end

        // ------------------------------------- return distribution (I-061)
        // A coalesced load's beat is handed to its members one per cycle
        // through the write-back queue, which drains in parallel.
        ST_DIST: begin
          if (dist_idx_q >= dist_n_q) begin
            dist_valid_q <= 1'b0;
            state_q      <= ST_WAIT;
          end else if (dist_wr_c && vrf_wr_gnt_i) begin
            dist_idx_q <= dist_idx_q + 4'd1;
          end
        end

        // ----------------------------------------------------------- wait
        ST_WAIT: begin
          if (abort_q) begin
            if ((outstanding_q == 3'd0) && (wf_count_q == 3'd0)) state_q <= ST_DONE;
          end else if (outstanding_q < limit_c) begin
            case (ret_q)
              RET_CUR:   state_q <= ST_CUR;
              RET_DRAIN: state_q <= ST_DRAIN;
              RET_STOP: begin
                abort_q <= 1'b1;
                state_q <= ST_WAIT;
              end
              default: begin
                elem_q  <= main_nelem_c;
                field_q <= main_nfield_c;
                state_q <= ST_SCAN;
              end
            endcase
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

      // A coalesced response borrows the FSM's next state: a fault starts the
      // de-coalesce replay, a success starts the return distribution. A size-1
      // response leaves the FSM alone, so the I-056 path is untouched.
      if (rsp_replay_c) state_q <= ST_SCAN;
      if (rsp_dist_c)   state_q <= ST_DIST;
    end
  end

  // unused tie-off
  logic unused_ok;
  assign unused_ok = (rdw_grp_base[4:0] == 5'd0) | (rd_base_c[4]);

endmodule

`resetall
`default_nettype wire
