// ============================================================================
// mosaic_lsu_endpoint -- work package I-033: the ordered memory endpoint and
// the access-fault boundary.
//
// This is the module that turns "the pipeline wants to load or store" into
// "the memory system performed exactly one access". Everything about the
// misalignment policy, the size/sign handling and the fault reporting point
// lives here and nowhere else.
//
// --------------------------------------------------------------- the policy
//
// config/profiles/p0.json fixes the misalignment policy:
//
//     instruction_fetch: trap   load: trap   store: trap   atomic: trap
//
// so a misaligned access is a trap and **the memory system never sees it**. The
// check is made here, before a request leaves this module, for a reason that is
// not stylistic: the bring-up memory model assembles an unaligned access byte by
// byte and would happily serve it. If this module stopped checking, a misaligned
// load would silently succeed instead of trapping, and the negative control that
// catches exactly that would stop being able to catch it. The hart owns the
// check; the memory model deliberately does not.
//
// The precedence is fixed and tested: misalignment is decided from the address
// alone, so a misaligned access to an unmapped page reports the *misaligned*
// cause, not the access fault. That is the architecturally correct order for an
// implementation that traps on misalignment, and it is a behaviour a test can
// observe -- the same address accessed aligned faults with cause 5/7 and
// unaligned with cause 4/6.
//
// ------------------------------------------------------- one access at a time
//
// The endpoint holds exactly one outstanding memory transaction. That is the
// conservative start the plan asks for ("首先一次一个 normal-memory
// transaction"), and it makes the ordering contract trivially true: the order
// in which this module accepts requests is the order the memory system sees
// them, and there is no response reordering to reason about. I-043's MSHR and
// nonblocking responses come later and must not change the *architectural*
// order, only the concurrency.
//
// The handshake is valid/ready on both sides, with the payload held stable
// while `valid && !ready` -- the project-wide transport rule. A request is
// consumed on `req_valid_i && req_ready_o`; a response is consumed on
// `rsp_valid_o && rsp_ready_i`.
//
// ------------------------------------------------------------- byte lanes
//
// `mem_req_wdata` and `mem_rsp_rdata` carry a doubleword whose byte lanes are
// aligned to the *address*: the byte at `addr` is lane `addr[2:0]`. The strobes
// mark which lanes this access owns. That convention is what makes a byte store
// a byte store: the alternative -- always putting the data at lane 0 and
// letting the memory shift it -- puts the shift in the memory model, where a
// later cache would have to reimplement it.
//
// A load returns the doubleword it read; this module extracts the addressed
// lanes and performs the sign or zero extension, so the consumer sees the
// architectural value and never a lane.
//
// ------------------------------------------------------- the state machine
//
//   IDLE  --accept-->  REQ  --mem ready-->  WAIT  --mem response-->  DONE
//     |                                                               |
//     +--accept, misaligned--> (response built immediately)---> DONE  |
//                                                                     |
//                        IDLE  <--response accepted------------------+
//
// A misaligned access goes from IDLE to DONE without ever entering REQ, which
// is the structural statement of "the memory never sees it". A response is
// *held* in DONE until the consumer accepts it: the payload does not change and
// `rsp_valid_o` stays high, so a consumer that is busy costs cycles and never a
// result.
//
// The response is built in the cycle the memory answers and presented from the
// next one, i.e. the memory response sits on a register boundary. That is one
// deliberate beat of latency: it keeps this endpoint reusable behind a cache
// whose response arrives whenever it arrives, instead of putting the extraction
// shifter combinationally between the memory and the writeback path.
// ============================================================================

`ifndef MOSAIC_LSU_ENDPOINT_SV_
`define MOSAIC_LSU_ENDPOINT_SV_

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names the subset it needs; the rest belong to other
// modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_lsu_endpoint (
    input  logic                          clk,
    input  logic                          rst,

    // ------------------------------------------------------- upstream (uop)
    // A load or store uop's memory request. `base + imm` is computed here, so
    // an ALU op and a memory op have the same shape in the issue queue and
    // neither waits for the other's resource.
    input  logic                          req_valid_i,
    output logic                          req_ready_o,
    input  mosaic_uop_pkg::lsu_req_t      req_i,
    // The PMA device attribute of this request (I-038). It rides beside the
    // packet rather than inside it because `lsu_req_t` is a frozen interface
    // (mosaic_uop_pkg's header) and the attribute is produced by the serializer
    // that owns the classification. It is latched with the transaction and
    // exported so a testbench can require every access presented to the memory
    // system to carry the attribute its address's region demands -- the signal a
    // coalescer keys on when it refuses to merge an MMIO access with RAM.
    input  logic                          req_dev_i,

    output logic                          rsp_valid_o,
    input  logic                          rsp_ready_o,
    output mosaic_uop_pkg::lsu_rsp_t      rsp_o,

    // ---------------------------------------------------- downstream (memory)
    // The core's port to the memory system. The request side is offered until
    // accepted; the response side completes the transaction.
    output logic                          mem_req_valid_o,
    input  logic                          mem_req_ready_i,
    output mosaic_uop_pkg::mem_req_t      mem_req_o,

    input  logic                          mem_rsp_valid_i,
    output logic                          mem_rsp_ready_o,
    input  mosaic_uop_pkg::mem_rsp_t      mem_rsp_i,

    // ------------------------------------------------------------ observability
    output logic                          o_busy,
    output logic [31:0]                   o_load_ctr,
    output logic [31:0]                   o_store_ctr,
    output logic [31:0]                   o_txn_ctr,          // requests offered to memory
    output logic [31:0]                   o_misaligned_ctr,
    output logic [31:0]                   o_access_fault_ctr,
    output logic [31:0]                   o_rsp_ctr,
    // The last fault reported, so a test can compare the cause/tval pair that
    // actually reached the pipeline without inferring it from the request.
    output logic [63:0]                   o_last_fault_cause,
    output logic [63:0]                   o_last_fault_tval,
    // Address of the access being served, after the base+imm addition.
    output logic [63:0]                   o_inflight_addr,
    output logic [2:0]                    o_inflight_size,
    // The identity and the device attribute of the transaction being served, so
    // a testbench can attribute every access the memory system sees to one
    // instruction and require each identity to appear exactly once (I-038's
    // "one side effect per transaction identity"). Valid while a transaction is
    // in flight -- live from the cycle the request is accepted until its
    // response leaves -- and aligned with the memory request port, because both
    // are read from the same latched transaction.
    output mosaic_uop_pkg::uop_id_t       o_txn_id,
    output logic                          o_txn_dev
);

  // --------------------------------------------------------------- localparams
  localparam int unsigned XLEN = mosaic_cfg_pkg::MOSAIC_XLEN;

  // The exception codes are mosaic_pkg's, not a second table: a second table is
  // how a load misalignment starts reporting a store misalignment.
  localparam logic [63:0] EXC_LOAD_MISALIGNED  = mosaic_pkg::EXC_LOAD_MISALIGNED;
  localparam logic [63:0] EXC_LOAD_ACCESS      = mosaic_pkg::EXC_LOAD_ACCESS;
  localparam logic [63:0] EXC_STORE_MISALIGNED = mosaic_pkg::EXC_STORE_MISALIGNED;
  localparam logic [63:0] EXC_STORE_ACCESS     = mosaic_pkg::EXC_STORE_ACCESS;

  typedef enum logic [1:0] {
    ST_IDLE = 2'd0,
    ST_REQ  = 2'd1,
    ST_WAIT = 2'd2,
    ST_DONE = 2'd3
  } state_e;

  state_e state_q;

  // The transaction being served. Held across the memory round trip, so the
  // response can name the uop it belongs to even if that uop has since been
  // squashed: the identity is what lets that be detected rather than assumed.
  //
  // Only the fields that are read after acceptance are latched. `base` and
  // `imm` are deliberately *not* here: they are consumed by the one address
  // addition at acceptance, and a register that holds them would be 128 dead
  // bits that Verilator correctly reports as unused. What survives is the
  // identity plus what the response needs.
  typedef struct packed {
    mosaic_uop_pkg::uop_id_t id;
    logic                    we;
    logic [2:0]              size;
    logic                    is_signed;
    logic [XLEN-1:0]         store_data;
    logic                    dev;
  } txn_t;

  txn_t            req_q;
  logic [XLEN-1:0] addr_q;

  // The response being held for the consumer.
  mosaic_uop_pkg::lsu_rsp_t rsp_q;
  logic                     rsp_valid_q;

  // ---------------------------------------------------------------- counters
  logic [31:0] load_ctr_q, store_ctr_q, txn_ctr_q;
  logic [31:0] misaligned_ctr_q, access_fault_ctr_q, rsp_ctr_q;
  logic [63:0] last_fault_cause_q, last_fault_tval_q;

  // --------------------------------------------------------- combinatorial
  logic [XLEN-1:0] addr_c;

  assign addr_c     = req_i.base + req_i.imm;

  // Misaligned exactly when the access crosses a boundary the size demands.
  // `size_bytes = 8` requires all three low bits zero; a byte requires none.
  // Derived from the same function the strobes come from, so "which bytes does
  // this access own" and "is this access legal" cannot disagree.
  function automatic logic is_misaligned(input logic [2:0] size, input logic [2:0] low);
    logic [3:0] bytes;
    begin
      bytes = 4'(mosaic_uop_pkg::size_bytes(size));
      is_misaligned = ((4'({1'b0, low}) & (bytes - 4'd1)) != 4'd0);
    end
  endfunction

  logic misaligned_c;
`ifndef MOSAIC_LSU_MUTANT_NO_MISALIGN_CHECK
  assign misaligned_c = is_misaligned(req_i.size, addr_c[2:0]);
`else
  // Mutant: the misalignment check never fires, so a misaligned access is
  // forwarded to memory and silently served. This is exactly the failure the
  // profile's trap policy exists to prevent, and the case must catch it.
  assign misaligned_c = 1'b0;
`endif

  // -------------------------------------------------------- request accept
  // A request is taken only from IDLE: one transaction at a time, so the
  // acceptance order *is* the memory order.
  logic accept_c;
  assign accept_c    = req_valid_i && (state_q == ST_IDLE);
  assign req_ready_o = (state_q == ST_IDLE);

  // ---------------------------------------------------- downstream request
  // Offered in ST_REQ and only then. The payload is the latched transaction, so
  // it cannot change while the memory is not ready. The address used for the
  // strobes and the lane shift is the *latched* address, not the incoming
  // `req_i`'s: after acceptance the request port may carry the next uop.
  logic [XLEN-1:0] shifted_store_c;
`ifndef MOSAIC_LSU_MUTANT_NO_STORE_SHIFT
  assign shifted_store_c = req_q.store_data << {addr_q[2:0], 3'b000};
`else
  // Mutant: store data is placed at lane 0 instead of at the addressed lane, so
  // a byte store to an odd address writes the wrong byte.
  assign shifted_store_c = req_q.store_data;
`endif

  assign mem_req_valid_o = (state_q == ST_REQ);
  assign mem_req_o.we    = req_q.we;
  assign mem_req_o.addr  = addr_q;
  assign mem_req_o.size  = req_q.size;
  assign mem_req_o.wstrb = mosaic_uop_pkg::expected_wstrb(req_q.size, addr_q[2:0]);
  assign mem_req_o.wdata = shifted_store_c;

  // The memory response is taken in ST_WAIT. Taking it in the same cycle it is
  // shown is what keeps the transaction to one round trip.
  assign mem_rsp_ready_o = (state_q == ST_WAIT);

  // ------------------------------------------------------- value extraction
  // The load's architectural value: the addressed byte lanes, sign- or
  // zero-extended. `mem_rsp_rdata` carries the lane-aligned doubleword.
  logic [XLEN-1:0] lane_shifted_c;
  logic [63:0]     extracted_c;

  assign lane_shifted_c = mem_rsp_i.rdata >> {addr_q[2:0], 3'b000};

`ifndef MOSAIC_LSU_MUTANT_NO_SIGN_EXTEND
  always_comb begin
    case (req_q.size)
      mosaic_pkg::SZ_BYTE: begin
        extracted_c = req_q.is_signed ? {{56{lane_shifted_c[7]}},  lane_shifted_c[7:0]}
                                    : {56'd0, lane_shifted_c[7:0]};
      end
      mosaic_pkg::SZ_HALF: begin
        extracted_c = req_q.is_signed ? {{48{lane_shifted_c[15]}}, lane_shifted_c[15:0]}
                                    : {48'd0, lane_shifted_c[15:0]};
      end
      mosaic_pkg::SZ_WORD: begin
        extracted_c = req_q.is_signed ? {{32{lane_shifted_c[31]}}, lane_shifted_c[31:0]}
                                    : {32'd0, lane_shifted_c[31:0]};
      end
      default: begin
        extracted_c = lane_shifted_c;
      end
    endcase
  end
`else
  // Mutant: a load zero-extends regardless of the instruction, so `lb`/`lh`/`lw`
  // differ from the architectural value exactly when the sign bit is set.
  always_comb begin
    case (req_q.size)
      mosaic_pkg::SZ_BYTE: extracted_c = {56'd0, lane_shifted_c[7:0]};
      mosaic_pkg::SZ_HALF: extracted_c = {48'd0, lane_shifted_c[15:0]};
      mosaic_pkg::SZ_WORD: extracted_c = {32'd0, lane_shifted_c[31:0]};
      default:             extracted_c = lane_shifted_c;
    endcase
  end
`endif

  // ---------------------------------------------------------------- outputs
  assign rsp_valid_o     = rsp_valid_q;
  assign rsp_o           = rsp_q;
  assign o_busy          = (state_q != ST_IDLE) || rsp_valid_q;
  assign o_inflight_addr = addr_q;
  assign o_inflight_size = req_q.size;
  assign o_txn_id        = req_q.id;
  assign o_txn_dev       = req_q.dev;

  assign o_load_ctr         = load_ctr_q;
  assign o_store_ctr        = store_ctr_q;
  assign o_txn_ctr          = txn_ctr_q;
  assign o_misaligned_ctr   = misaligned_ctr_q;
  assign o_access_fault_ctr = access_fault_ctr_q;
  assign o_rsp_ctr          = rsp_ctr_q;
  assign o_last_fault_cause = last_fault_cause_q;
  assign o_last_fault_tval  = last_fault_tval_q;

  logic commit_rsp_c;
  assign commit_rsp_c = rsp_valid_q && rsp_ready_o;

  always_ff @(posedge clk) begin
    if (rst) begin
      state_q            <= ST_IDLE;
      // `req_q` and `rsp_q` are deliberately NOT reset: neither is read
      // before it is written, because the state machine's state is the
      // validity of both. Resetting them would cost 200 bits of reset fanout
      // to hold a value nothing can observe, which is the trade
      // rtl/common/mosaic_ram.sv documents for storage in general.
      addr_q             <= 64'd0;
      rsp_valid_q        <= 1'b0;
      load_ctr_q         <= 32'd0;
      store_ctr_q        <= 32'd0;
      txn_ctr_q          <= 32'd0;
      misaligned_ctr_q   <= 32'd0;
      access_fault_ctr_q <= 32'd0;
      rsp_ctr_q          <= 32'd0;
      last_fault_cause_q <= 64'd0;
      last_fault_tval_q  <= 64'd0;
    end else begin
      // A held response leaves only when the consumer takes it. This is the
      // one transition that is not a function of the memory or of a new
      // request, and it is deliberately outside the case below so a response
      // cannot be overwritten by a new transaction in the same cycle.
      if (commit_rsp_c) begin
        rsp_valid_q <= 1'b0;
      end

      case (state_q)
        ST_IDLE: begin
          if (accept_c) begin
            req_q.id         <= req_i.id;
            req_q.we         <= req_i.we;
            req_q.size       <= req_i.size;
            req_q.is_signed  <= req_i.signed_;
            req_q.store_data <= req_i.store_data;
            req_q.dev        <= req_dev_i;
            addr_q           <= addr_c;

            if (req_i.we) store_ctr_q <= store_ctr_q + 32'd1;
            else          load_ctr_q  <= load_ctr_q  + 32'd1;

            if (misaligned_c) begin
              // The trap path. The response is built from the address alone,
              // so the memory is never asked. Cause and tval are the pair the
              // architectural exception carries.
              misaligned_ctr_q  <= misaligned_ctr_q + 32'd1;
              rsp_q.id          <= req_i.id;
              rsp_q.fault       <= 1'b1;
              rsp_q.cause       <= req_i.we ? EXC_STORE_MISALIGNED : EXC_LOAD_MISALIGNED;
              rsp_q.tval        <= addr_c;
              rsp_q.data        <= 64'd0;
              rsp_valid_q       <= 1'b1;
              last_fault_cause_q <= req_i.we ? EXC_STORE_MISALIGNED : EXC_LOAD_MISALIGNED;
              last_fault_tval_q  <= addr_c;
              rsp_ctr_q         <= rsp_ctr_q + 32'd1;
              state_q           <= ST_DONE;
            end else begin
              state_q <= ST_REQ;
            end
          end
        end

        ST_REQ: begin
          if (mem_req_ready_i) begin
            txn_ctr_q <= txn_ctr_q + 32'd1;
            state_q   <= ST_WAIT;
          end
        end

        ST_WAIT: begin
          if (mem_rsp_valid_i) begin
            rsp_q.id    <= req_q.id;
`ifdef MOSAIC_LSU_MUTANT_DEV_ERR_OK
            // NEGATIVE CONTROL: a device access the device rejected is reported
            // as a successful read of zero, so the error response never traps.
            // CASE=mmio.exactly_once's "the error device traps precisely" check
            // names it: mepc/mcause/mtval never appear.
            rsp_q.fault <= mem_rsp_i.fault && !req_q.dev;
`elsif MOSAIC_LSU_MUTANT_FAULT_AS_ZERO
            // Mutant: a memory access fault is reported as a successful read of
            // zero -- the "out-of-range read defaults to zero" blocker.
            rsp_q.fault <= 1'b0;
`else
            rsp_q.fault <= mem_rsp_i.fault;
`endif
            rsp_q.tval  <= addr_q;
            rsp_q.cause <= req_q.we ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS;
            rsp_q.data  <= req_q.we ? 64'd0 : extracted_c;
            rsp_valid_q <= 1'b1;

            if (mem_rsp_i.fault) begin
              access_fault_ctr_q <= access_fault_ctr_q + 32'd1;
              last_fault_cause_q <= req_q.we ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS;
              last_fault_tval_q  <= addr_q;
            end

            rsp_ctr_q <= rsp_ctr_q + 32'd1;
            state_q   <= ST_DONE;
          end
        end

        ST_DONE: begin
          // The response is held. A new request cannot be accepted until it
          // leaves, which is what makes `req_ready_o` an honest statement about
          // this module's capacity rather than a guess.
          if (commit_rsp_c) begin
            state_q <= ST_IDLE;
          end
        end

        default: state_q <= ST_IDLE;
      endcase
    end
  end

endmodule

`endif  // MOSAIC_LSU_ENDPOINT_SV_
