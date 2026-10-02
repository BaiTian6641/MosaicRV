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
//
// ------------------------------------------------------ LR/SC (I-040)
//
// `lr` and `sc` are two access *classes* the request carries (`is_lr`/`is_sc`),
// not two accesses bolted onto the load/store path:
//
//   * an **LR** is the ordinary read path with one extra act on the memory
//     response: it establishes the hart's reservation on the granule containing
//     the address, and only if the read did not fault. Its response is the old
//     value, exactly as a load's.
//   * an **SC** is decided *before the memory is asked*. The reservation manager
//     answers combinationally, and:
//       - on a **miss** the endpoint never leaves `ST_IDLE` for `ST_REQ`: it
//         builds the response (rd = 1) and moves to `ST_DONE`. The memory system
//         does not see the instruction at all, which is the structural statement
//         of "an SC with no reservation does not write".
//       - on a **hit** it performs exactly one write beat -- `ST_REQ` presenting
//         the store data, `ST_WAIT` taking the acknowledgement -- and returns
//         rd = 0. The `ST_REQ`/`ST_WAIT` states are the *same* states an
//         ordinary store uses, so there is no separate write path to keep in
//         step, and the SC is one beat on the port by construction rather than
//         by a second rule.
//
// The reservation manager (`mosaic_reservation`) sits beside the port because
// this module is the only place that can see every event the reservation is a
// function of: this hart's stores, AMOs and SCs, another agent's writes
// (`ext_write_valid_i`), and a trap or context switch (`flush_i`). Its contract
// -- the granule, the invalidation set, and why over-invalidation is a defect
// here rather than a liberty -- is documented in that module.
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

    // ------------------------------------------ the coherence notification (A)
    // Work package I-040. A write another agent performed, delivered to the
    // reservation manager so a reservation that a conflicting write has
    // invalidated cannot outlive it. In this profile there is no cache and one
    // hart on the port, so there is no fabric to snoop and the notification is a
    // port: a deeper design drives it from the same invalidation traffic its
    // caches see, and the case drives it from the second agent it models. It has
    // no effect on any access -- it is neither a request nor a response -- so an
    // ordinary machine that ties it low is unchanged.
    input  logic                          ext_write_valid_i,
    input  logic [63:0]                   ext_write_addr_i,
    input  logic [3:0]                    ext_write_bytes_i,

    // A trap or a context switch: the hart's reservation does not survive it.
    input  logic                          flush_i,

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
    output logic                          o_txn_dev,
    // I-040. The kind of the transaction the port is carrying: 0 ordinary,
    // 1 AMO, 2 LR, 3 SC. It rides beside the transaction like the device
    // attribute, so a case can attribute every data-port beat to the access
    // class the instruction asked for instead of inferring it from the address.
    output logic [1:0]                    o_txn_kind,
    // The reservation, and what happened to it. `o_res_valid`/`o_res_granule`
    // are the state itself, so a case can require an LR to establish it and each
    // invalidation source to destroy it rather than only observing the SC status
    // it produces.
    output logic                          o_res_valid,
    output logic [63:0]                   o_res_granule,
    output logic [31:0]                   o_lr_ctr,
    output logic [31:0]                   o_sc_ok_ctr,
    output logic [31:0]                   o_sc_fail_ctr,
    output logic [31:0]                   o_res_set_ctr,
    output logic [31:0]                   o_res_clear_ctr,
    output logic [31:0]                   o_res_ext_inval_ctr,
    output logic [31:0]                   o_res_hit_ctr,
    output logic [31:0]                   o_res_miss_ctr
);

  // --------------------------------------------------------------- localparams
  localparam int unsigned XLEN = mosaic_cfg_pkg::MOSAIC_XLEN;

  // The exception codes are mosaic_pkg's, not a second table: a second table is
  // how a load misalignment starts reporting a store misalignment.
  localparam logic [63:0] EXC_LOAD_MISALIGNED  = mosaic_pkg::EXC_LOAD_MISALIGNED;
  localparam logic [63:0] EXC_LOAD_ACCESS      = mosaic_pkg::EXC_LOAD_ACCESS;
  localparam logic [63:0] EXC_STORE_MISALIGNED = mosaic_pkg::EXC_STORE_MISALIGNED;
  localparam logic [63:0] EXC_STORE_ACCESS     = mosaic_pkg::EXC_STORE_ACCESS;

  // The transaction-class label published beside the memory port (I-040). Small
  // and local: it exists so a test can attribute each data-port beat to the
  // access class the instruction asked for.
  localparam logic [1:0] KIND_ORDINARY = 2'd0;
  localparam logic [1:0] KIND_AMO      = 2'd1;
  localparam logic [1:0] KIND_LR       = 2'd2;
  localparam logic [1:0] KIND_SC       = 2'd3;

  // The SC status the instruction writes to `rd`: 0 when the store-conditional
  // performed its write, 1 when it did not.
  localparam logic [63:0] SC_OK   = 64'd0;
  localparam logic [63:0] SC_FAIL = 64'd1;

  //   IDLE --accept--> REQ --mem ready--> WAIT --mem response--> DONE
  //     |                                                         |
  //     |                    (atomic read-modify-write)           |
  //     +-----------------------> AMO_W --mem ready--> AMO_WAIT --+
  //
  // An ordinary load or store is the three-state path the module always had. An
  // **atomic read-modify-write** (I-039) adds the two `AMO_W`/`AMO_WAIT`
  // states: the read beat returns in WAIT, its doubleword is latched, the new
  // field is computed by `mosaic_amo_alu` and presented as the write beat, and
  // the write's acknowledgement completes the transaction. The whole sequence
  // is one indivisible operation: the endpoint is not in ST_IDLE between the
  // two beats, so it accepts no other request and offers the memory port to
  // nothing else -- the shared serialization point the card asks for. Under
  // `MOSAIC_AMO_MUTANT_SPLIT` the endpoint *does* return to IDLE between the
  // beats, which is exactly the "decomposition that loses atomicity" the case
  // must catch.
  typedef enum logic [2:0] {
    ST_IDLE     = 3'd0,
    ST_REQ      = 3'd1,
    ST_WAIT     = 3'd2,
    ST_AMO_W    = 3'd3,   // offering the atomic write beat
    ST_AMO_WAIT = 3'd4,   // waiting for its acknowledgement
    ST_DONE     = 3'd5
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
    // A extension (I-039): an atomic read-modify-write. `store_data` is the
    // operand and `amo_op` names the operation. The read and the write are two
    // *beats of one transaction*: the endpoint owns the operation across both
    // and is not in ST_IDLE between them, so no other request is accepted and no
    // other access can interleave. `amo` travels with both beats so the memory
    // system -- and the case -- can see that they belong to one atomic
    // operation.
    logic                    is_amo;
    mosaic_pkg::amo_op_e     amo_op;
    logic                    aq;
    logic                    rl;
    // LR/SC (I-040). `is_lr` completes as a read whose response is also the
    // moment the reservation is established; `is_sc` is checked against the
    // reservation manager before the memory system is touched at all, and is
    // performed as exactly one write beat when -- and only when -- the check
    // hits.
    logic                    is_lr;
    logic                    is_sc;
  } txn_t;

  txn_t            req_q;
  logic [XLEN-1:0] addr_q;

  // The aligned doubleword the atomic read beat returned. Latched between the
  // read and the write so the new field can be computed from it and the *old*
  // value can still be reported to the consumer (a load takes its value from
  // the response the memory has just presented; an AMO's response comes one
  // beat later, so the value has to survive).
  logic [XLEN-1:0] amo_old_q;

  // `MOSAIC_AMO_MUTANT_SPLIT` only: the deferred write beat. The mutation
  // releases the endpoint to ST_IDLE after the read and issues the write from a
  // later, independent cycle, so the memory port is free in between -- the
  // "load+store decomposition" that loses atomicity.
`ifdef MOSAIC_AMO_MUTANT_SPLIT
  logic            amo_pending_q;
`endif

  // `MOSAIC_LRSC_MUTANT_SC_DOUBLE_WRITE` only: a successful SC has already
  // performed its write and this bit sends it back round for one more.
`ifdef MOSAIC_LRSC_MUTANT_SC_DOUBLE_WRITE
  logic            sc_rewrite_q;
`endif

  // The response being held for the consumer.
  mosaic_uop_pkg::lsu_rsp_t rsp_q;
  logic                     rsp_valid_q;

  // ---------------------------------------------------------------- counters
  logic [31:0] load_ctr_q, store_ctr_q, txn_ctr_q;
  logic [31:0] misaligned_ctr_q, access_fault_ctr_q, rsp_ctr_q;
  logic [63:0] last_fault_cause_q, last_fault_tval_q;

  // I-040: what the LR/SC path did, and what the reservation did.
  logic [31:0] lr_ctr_q, sc_ok_ctr_q, sc_fail_ctr_q;

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
`ifdef MOSAIC_AMO_MUTANT_SPLIT
  // The endpoint holds the port free but refuses the *next real* request until
  // the deferred write has gone, so the only thing that can use the window is
  // the competing agent the case models.
  assign req_ready_o = (state_q == ST_IDLE) && !amo_pending_q;
`else
  assign req_ready_o = (state_q == ST_IDLE);
`endif
  assign accept_c    = req_valid_i && req_ready_o;

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

  // The read-modify-write datapath. It is fed the doubleword the read beat
  // returned and the operand, and produces the doubleword to write back. It is
  // stable in ST_AMO_W (both inputs are registered), so the write beat's payload
  // cannot change while the memory is not ready.
  logic [XLEN-1:0] amo_new_c;

  mosaic_amo_alu u_amo_alu (
      .op_i      (req_q.amo_op),
      .size_i    (req_q.size),
      .lane_i    (addr_q[2:0]),
      .old_win_i (amo_old_q),
      .operand_i (req_q.store_data),
      .new_win_o (amo_new_c)
  );

  // The read beat is offered in ST_REQ, the atomic write beat in ST_AMO_W.
  // Nothing else is offered from any other state, so the two beats of one AMO
  // are adjacent on the port with no other access between them.
  assign mem_req_valid_o = (state_q == ST_REQ) || (state_q == ST_AMO_W);

  // `ST_AMO_W` is always a write; `ST_REQ` is whatever the transaction is.
  logic amo_write_c;
  assign amo_write_c = (state_q == ST_AMO_W);

  always_comb begin
    // The write beat's payload is the doubleword the datapath produced, laid
    // out by lane (the datapath already shifted the field into place). An
    // ordinary store keeps its own payload; only the atomic write uses the
    // datapath's output.
    mem_req_o.wdata = amo_write_c ? amo_new_c : shifted_store_c;
    // An SC performs its write on the ST_REQ beat, which is a write for it even
    // though the load queue issued the request as a load class.
    mem_req_o.we    = amo_write_c ? 1'b1 : (req_q.we || req_q.is_sc);
    mem_req_o.addr  = addr_q;
    mem_req_o.size  = req_q.size;
    mem_req_o.wstrb = mosaic_uop_pkg::expected_wstrb(req_q.size, addr_q[2:0]);
    // The atomic attribute travels with *both* beats, so the memory system --
    // and the case -- can attribute them to one atomic operation.
`ifdef MOSAIC_AMO_MUTANT_SPLIT
    mem_req_o.amo    = 1'b0;
    mem_req_o.amo_op = mosaic_pkg::AMO_ADD;
    mem_req_o.aq     = 1'b0;
    mem_req_o.rl     = 1'b0;
`elsif MOSAIC_AMO_MUTANT_IGNORE_AQR
    // NEGATIVE CONTROL: the operation is carried but the ordering bits are
    // dropped, so aq/rl are treated as hints. The case requires every atomic
    // transaction to carry the instruction's aq/rl.
    mem_req_o.amo    = req_q.is_amo;
    mem_req_o.amo_op = req_q.amo_op;
    mem_req_o.aq     = 1'b0;
    mem_req_o.rl     = 1'b0;
`else
    mem_req_o.amo    = req_q.is_amo;
    mem_req_o.amo_op = req_q.amo_op;
    mem_req_o.aq     = req_q.aq;
    mem_req_o.rl     = req_q.rl;
`endif
  end

  // The memory response is taken in the read-wait and the write-ack states.
  assign mem_rsp_ready_o = (state_q == ST_WAIT) || (state_q == ST_AMO_WAIT);

  // ==========================================================================
  // I-040: the LR/SC reservation manager
  // ==========================================================================
  // The reservation lives next to the only structure that can see every event
  // the ISA makes it a function of: the endpoint performs this hart's stores,
  // AMOs and SCs, and it is where another agent's writes are notified to.
  //
  // The four sources, and where each comes from:
  //
  //   own write    `mem_req_valid_o && mem_req_o.we` -- the offered write beat.
  //                One strobe covers an ordinary store, an AMO's write beat and
  //                an SC's write beat, so "a store, an AMO, or an SC" cannot be
  //                three rules that disagree.
  //   external     `ext_write_valid_i`, the notification described at the port.
  //   SC consumed  `accept_c && req_i.is_sc`: an SC pairs with the most recent
  //                LR whether it succeeds or fails, so it consumes the
  //                reservation either way.
  //   flush        `flush_i`: an exception or context switch, wired from the
  //                core's redirect.
  //
  // The LR's establishment is the read's completion -- the cycle the memory
  // returns the old value -- and only when that read did not fault: a faulting
  // LR has not read the location, so it cannot reserve it.
  logic        res_valid_c;
  logic [63:0] res_granule_c;
  logic        res_hit_c;
  logic        res_set_c;
  logic        res_consume_c;
  logic        res_check_c;
  logic        res_own_write_c;
  logic [3:0]  res_own_bytes_c;
  logic [31:0] res_set_ctr_c, res_clear_ctr_c, res_ext_inval_ctr_c;
  logic [31:0] res_hit_ctr_c, res_miss_ctr_c;

  assign res_set_c     = (state_q == ST_WAIT) && mem_rsp_valid_i &&
                         req_q.is_lr && !mem_rsp_i.fault;
  assign res_consume_c = accept_c && req_i.is_sc;
  assign res_check_c   = accept_c && req_i.is_sc;
  assign res_own_write_c   = mem_req_valid_o && mem_req_o.we;
  assign res_own_bytes_c   = 4'(mosaic_uop_pkg::size_bytes(req_q.size));

  mosaic_reservation u_reservation (
      .clk                (clk),
      .rst                (rst),
      .set_valid_i        (res_set_c),
      .set_addr_i         (addr_q),
      .check_addr_i       (addr_c),
      .check_valid_i      (res_check_c),
      .hit_o              (res_hit_c),
      .own_write_valid_i  (res_own_write_c),
      .own_write_addr_i   (addr_q),
      .own_write_bytes_i  (res_own_bytes_c),
      .ext_write_valid_i  (ext_write_valid_i),
      .ext_write_addr_i   (ext_write_addr_i),
      .ext_write_bytes_i  (ext_write_bytes_i),
      .flush_valid_i      (flush_i),
      .consume_valid_i    (res_consume_c),
      .valid_o            (res_valid_c),
      .granule_o          (res_granule_c),
      .set_ctr_o          (res_set_ctr_c),
      .clear_ctr_o        (res_clear_ctr_c),
      .clear_ext_ctr_o    (res_ext_inval_ctr_c),
      .hit_ctr_o          (res_hit_ctr_c),
      .miss_ctr_o         (res_miss_ctr_c)
  );

  // ------------------------------------------------------- value extraction
  // The load's architectural value: the addressed byte lanes, sign- or
  // zero-extended. `mem_rsp_rdata` carries the lane-aligned doubleword. For an
  // AMO the response is the *old* value, and it is built one beat later than the
  // read that fetched it, so it comes from the latched doubleword.
  logic [XLEN-1:0] lane_shifted_c;
  logic [63:0]     extracted_c;

  // The window the value comes from: the latched one in the atomic write-ack
  // state (the read is one beat behind there), the presented one everywhere else
  // -- including the read beat's own cycle, which is where a decomposed AMO
  // builds its response and where the still-latched `amo_old_q` would be a beat
  // stale.
  assign lane_shifted_c = ((state_q == ST_AMO_WAIT) ? amo_old_q : mem_rsp_i.rdata)
                          >> {addr_q[2:0], 3'b000};

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
  assign o_txn_kind      = req_q.is_amo ? KIND_AMO
                         : req_q.is_lr  ? KIND_LR
                         : req_q.is_sc  ? KIND_SC
                         : KIND_ORDINARY;

  assign o_res_valid           = res_valid_c;
  assign o_res_granule         = res_granule_c;
  assign o_lr_ctr              = lr_ctr_q;
  assign o_sc_ok_ctr           = sc_ok_ctr_q;
  assign o_sc_fail_ctr         = sc_fail_ctr_q;
  assign o_res_set_ctr         = res_set_ctr_c;
  assign o_res_clear_ctr       = res_clear_ctr_c;
  assign o_res_ext_inval_ctr   = res_ext_inval_ctr_c;
  assign o_res_hit_ctr         = res_hit_ctr_c;
  assign o_res_miss_ctr        = res_miss_ctr_c;

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
      amo_old_q          <= 64'd0;
`ifdef MOSAIC_AMO_MUTANT_SPLIT
      amo_pending_q      <= 1'b0;
`endif
`ifdef MOSAIC_LRSC_MUTANT_SC_DOUBLE_WRITE
      sc_rewrite_q       <= 1'b0;
`endif
      load_ctr_q         <= 32'd0;
      store_ctr_q        <= 32'd0;
      txn_ctr_q          <= 32'd0;
      misaligned_ctr_q   <= 32'd0;
      access_fault_ctr_q <= 32'd0;
      rsp_ctr_q          <= 32'd0;
      last_fault_cause_q <= 64'd0;
      last_fault_tval_q  <= 64'd0;
      lr_ctr_q           <= 32'd0;
      sc_ok_ctr_q        <= 32'd0;
      sc_fail_ctr_q      <= 32'd0;
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
`ifdef MOSAIC_AMO_MUTANT_SPLIT
          // NEGATIVE CONTROL: the deferred second half of the decomposition.
          // `amo_pending_q` is set after the read beat and the endpoint is back
          // in ST_IDLE, presenting nothing and holding no busy -- one whole
          // cycle in which the memory port is free. The case's competing agent
          // writes in that window, and the write that follows overwrites it with
          // a value computed from the stale read: the read-modify-write is no
          // longer atomic.
          if (amo_pending_q) begin
            state_q <= ST_AMO_W;
          end else if (accept_c) begin
`else
          if (accept_c) begin
`endif
            req_q.id         <= req_i.id;
            req_q.we         <= req_i.we;
            req_q.size       <= req_i.size;
            req_q.is_signed  <= req_i.signed_;
            req_q.store_data <= req_i.store_data;
            req_q.dev        <= req_dev_i;
            req_q.is_amo     <= req_i.is_amo;
            req_q.amo_op     <= req_i.amo_op;
            req_q.aq         <= req_i.aq;
            req_q.rl         <= req_i.rl;
            req_q.is_lr      <= req_i.is_lr;
            req_q.is_sc      <= req_i.is_sc;
            addr_q           <= addr_c;

            if (req_i.we) store_ctr_q <= store_ctr_q + 32'd1;
            else          load_ctr_q  <= load_ctr_q  + 32'd1;

            if (req_i.is_lr) lr_ctr_q <= lr_ctr_q + 32'd1;

            if (misaligned_c) begin
              // The trap path. The response is built from the address alone,
              // so the memory is never asked. Cause and tval are the pair the
              // architectural exception carries.
              misaligned_ctr_q  <= misaligned_ctr_q + 32'd1;
              rsp_q.id          <= req_i.id;
              rsp_q.fault       <= 1'b1;
              // The privileged spec folds AMO -- and, with it, every atomic
              // access whose *effect* is a write -- into the store/AMO pair for
              // causes 6 and 7. An SC is a conditional store and takes cause 6,
              // not cause 4; an LR is a load and takes cause 4.
              rsp_q.cause       <= (req_i.we || req_i.is_amo || req_i.is_sc)
                                   ? EXC_STORE_MISALIGNED : EXC_LOAD_MISALIGNED;
              rsp_q.tval        <= addr_c;
              rsp_q.data        <= 64'd0;
              rsp_valid_q       <= 1'b1;
              last_fault_cause_q <= (req_i.we || req_i.is_amo || req_i.is_sc)
                                    ? EXC_STORE_MISALIGNED : EXC_LOAD_MISALIGNED;
              last_fault_tval_q  <= addr_c;
              rsp_ctr_q         <= rsp_ctr_q + 32'd1;
              state_q           <= ST_DONE;
            end else if (req_i.is_sc && !res_hit_c) begin
              // --------------------------------------------------- SC failure
              // The reservation does not cover this address, so the
              // store-conditional performs **no memory access at all**: it does
              // not even enter ST_REQ, which is the structural statement of "an
              // SC with no reservation does not write". `rd` receives 1.
              //
              // MOSAIC_LRSC_MUTANT_SC_WRITES_ON_FAIL removes the refusal and
              // lets the write beat go out anyway -- the case's "an SC that did
              // not succeed performs no write" check names it.
`ifdef MOSAIC_LRSC_MUTANT_SC_WRITES_ON_FAIL
              state_q <= ST_REQ;
`else
              sc_fail_ctr_q <= sc_fail_ctr_q + 32'd1;
              rsp_q.id      <= req_i.id;
              rsp_q.fault   <= 1'b0;
              rsp_q.cause   <= EXC_STORE_ACCESS;
              rsp_q.tval    <= addr_c;
              rsp_q.data    <= SC_FAIL;
              rsp_valid_q   <= 1'b1;
              rsp_ctr_q     <= rsp_ctr_q + 32'd1;
              state_q       <= ST_DONE;
`endif
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
            if (req_q.is_amo) begin
              // The read beat returned. Latch the old doubleword and move to the
              // write beat in the very next cycle -- the endpoint is not idle in
              // between, so nothing else can touch the location.
              amo_old_q <= mem_rsp_i.rdata;
`ifdef MOSAIC_AMO_MUTANT_SPLIT
              // The decomposition: complete the read as an ordinary load,
              // remember the write for later, and give the port back.
              rsp_q.id      <= req_q.id;
              rsp_q.fault   <= mem_rsp_i.fault;
              rsp_q.tval    <= addr_q;
              rsp_q.cause   <= EXC_STORE_ACCESS;
              rsp_q.data    <= extracted_c;
              rsp_valid_q   <= 1'b1;
              amo_pending_q <= 1'b1;
              rsp_ctr_q     <= rsp_ctr_q + 32'd1;
              state_q       <= ST_DONE;
`else
              state_q <= ST_AMO_W;
`endif
            end else begin
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
            rsp_q.cause <= (req_q.we || req_q.is_amo || req_q.is_sc)
                           ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS;
            // An AMO's response is the *old* value, extracted exactly like a
            // load of the access width (the atomic op itself happened in the
            // memory system, inside the one transaction). An SC reports its
            // status, not a memory value: 0 when it performed its write, 1 when
            // it did not (and then there was no write at all).
            rsp_q.data  <= req_q.is_sc ? SC_OK
                         : (req_q.we ? 64'd0 : extracted_c);
            rsp_valid_q <= 1'b1;

            if (mem_rsp_i.fault) begin
              access_fault_ctr_q <= access_fault_ctr_q + 32'd1;
              last_fault_cause_q <= (req_q.we || req_q.is_amo || req_q.is_sc)
                                    ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS;
              last_fault_tval_q  <= addr_q;
            end

            if (req_q.is_sc) begin
              if (mem_rsp_i.fault) sc_fail_ctr_q <= sc_fail_ctr_q + 32'd1;
              else                 sc_ok_ctr_q   <= sc_ok_ctr_q   + 32'd1;
            end

            rsp_ctr_q <= rsp_ctr_q + 32'd1;
`ifdef MOSAIC_LRSC_MUTANT_SC_DOUBLE_WRITE
            // NEGATIVE CONTROL: a successful SC performs its write a second
            // time -- "an SC that succeeds but writes twice". The final memory
            // value is unchanged (the second write stores the same data), which
            // is exactly why the case counts transactions per SC rather than
            // only comparing the end state.
            if (req_q.is_sc && !mem_rsp_i.fault && !sc_rewrite_q) begin
              sc_rewrite_q <= 1'b1;
              state_q      <= ST_REQ;
            end else begin
              sc_rewrite_q <= 1'b0;
              state_q      <= ST_DONE;
            end
`else
            state_q   <= ST_DONE;
`endif
            end
          end
        end

        // ------------------------------------------------- the atomic write beat
        // The write beat is offered while this state holds. `mem_req_valid_o`
        // is high here, so the read and the write are adjacent on the port with
        // no other request accepted in between: the two beats are one
        // indivisible operation.
        ST_AMO_W: begin
          if (mem_req_ready_i) begin
            txn_ctr_q <= txn_ctr_q + 32'd1;
            state_q   <= ST_AMO_WAIT;
          end
        end

        // -------------------------------------------------- the write's ack
        ST_AMO_WAIT: begin
          if (mem_rsp_valid_i) begin
            // The transaction completes. The value reported is the *old* one,
            // latched from the read beat and shifted out by `extracted_c`
            // (which reads `amo_old_q` in this state); the fault is the write's.
            rsp_q.id    <= req_q.id;
            rsp_q.fault <= mem_rsp_i.fault;
            rsp_q.tval  <= addr_q;
            rsp_q.cause <= EXC_STORE_ACCESS;
            rsp_q.data  <= extracted_c;
            rsp_valid_q <= 1'b1;
            if (mem_rsp_i.fault) begin
              access_fault_ctr_q <= access_fault_ctr_q + 32'd1;
              last_fault_cause_q <= EXC_STORE_ACCESS;
              last_fault_tval_q  <= addr_q;
            end
            rsp_ctr_q <= rsp_ctr_q + 32'd1;
`ifdef MOSAIC_AMO_MUTANT_SPLIT
            amo_pending_q <= 1'b0;
`endif
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
