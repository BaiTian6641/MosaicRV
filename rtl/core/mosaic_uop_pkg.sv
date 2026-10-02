// ============================================================================
// mosaic_uop_pkg -- the core-internal packets, frozen at the integration
// boundary (I-023 and everything that plugs into it).
//
// This file exists for one reason, and it is the reason the project keeps
// rediscovering: **a field that two modules each define for themselves is a
// field that will disagree**, and the disagreement will not be a compile error.
// The uop the issue queue holds, the uop the cluster executes, the completion
// the writeback path carries and the request the LSU sends must all describe
// the same instruction with the same bits in the same places. Defining them
// once, here, is what makes that checkable by the elaborator instead of by a
// testbench that noticed too late.
//
// ---------------------------------------------------------------- ownership
//
// Every width below is derived from a generated package and none of them is
// written down twice:
//
//   mosaic_cfg_pkg  the geometry (PRF size, ROB size, XLEN, memory map)
//   mosaic_id_pkg   the *identities* (I-002's frozen field widths and counter
//                   moduli: rob_index, rob_gen, prf_tag, prf_gen, uop_index)
//
// The identity of one in-flight instruction is `mosaic_id_pkg::macro_id_t` --
// `{hart, rob_index, rob_gen, uop_index}` -- reused here rather than redefined,
// because the whole point of I-002 was that there is exactly one. p0 has one
// hart, so `hart` is a constant zero in this build; it is kept in the packet
// because removing it would make the multi-hart profile a change of every
// interface in the core rather than a change of one field's meaning.
//
// ------------------------------------------------------- what a uop carries
//
// `uop_meta_t` is the *execution* metadata: everything the execution units, the
// branch resolver and the memory path need that is not an operand and not a
// destination. It deliberately carries the PC:
//
//   * a branch needs its own PC to compute a target, to compute the link value
//     (PC+4) and to know where the fall-through goes when the prediction was
//     wrong;
//   * the ROB also holds a PC, but reading it from there would need a second
//     ROB read port and a second timing path into execution, and would make the
//     branch unit's correctness depend on ROB arbitration;
//   * so the copy in the issue queue's entry is not a second source of *truth*
//     -- it is the same value, written once at dispatch, and the ROB remains
//     the authority for what retires.
//
// `wb_event_t` is what an execution unit produces and what the writeback path
// consumes: the identity, the destination, the value, and the exception
// payload if the uop faulted. `x0` is a field rather than a tag value because
// "writes x0" and "writes physical tag 0" are different facts and a design that
// confuses them installs a result into a live real register.
//
// `lsu_req_t`/`lsu_rsp_t` are the memory uop's request and response. The
// address is computed in the LSU as `base + imm` for the same reason the PC is
// carried in the entry: it keeps address generation off the ALU, so an ALU op
// and a memory op occupy the same issue slot with the same operand shape and
// neither has to wait for the other's resource.
//
// ------------------------------------------------------------ what is frozen
//
// A change to any type or field in this file is an interface change: every
// producer and consumer of the packet must be re-verified, and the plan's
// §1.3 rule applies -- the integrator changes it, and the consumers are told.
// Adding a field at the *end* of a packed struct is still an interface change
// (the width moves), which is why the widths are all derived rather than
// literal and every consumer takes its width from the type.
// ============================================================================

`ifndef MOSAIC_UOP_PKG_SV_
`define MOSAIC_UOP_PKG_SV_

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
// The generated identity package declares every identity width and counter
// modulus, and a consumer uses a subset of them. Without this guard the unused
// ones are reported against whichever module happens to pull this package in,
// which turns a clean gate into noise. It has no include guard of its own
// either (the same defect the config package used to have, reported to the
// generator's owner), so the same body can be seen twice in one compilation
// unit and Verilator reports MODDUP.
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off MODDUP */
`include "mosaic_id_pkg.svh"
/* verilator lint_on MODDUP */
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
`include "mosaic_pkg.sv"

package mosaic_uop_pkg;
  // A package is types and constants; a consumer uses the subset it needs. The
  // same guard mosaic_pkg carries: without it every unused localparam here is
  // reported against whichever module happens to be elaborated with it, which
  // turns a clean gate into noise.
  /* verilator lint_off UNUSEDPARAM */

  // ------------------------------------------------------------- geometry
  localparam int unsigned XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN;
  localparam int unsigned ROB_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
  localparam int unsigned ROB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
  localparam int unsigned UOP_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;
  localparam int unsigned TAG_W       = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
  localparam int unsigned GEN_W       = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;
  localparam int unsigned HART_W      = mosaic_id_pkg::MOSAIC_ID_W_HART;
  // Deliberately no `PRF_ENTRIES` here: it was unused, and a file-scope name in
  // another module that happened to match it made Verilator report VARHIDDEN --
  // an error under this project's `-Wall` gate. A package that declares a name
  // nobody uses is a collision waiting for the first module that includes both.

  // -------------------------------------------------------------- identity
  // One in-flight instruction, as I-002 froze it. Aliased rather than
  // redefined: two structurally identical structs would compare unequal in a
  // type check and would silently be two contracts.
  typedef mosaic_id_pkg::macro_id_t uop_id_t;

  // ---------------------------------------------------------- uop classes
  // The class decides *which* execution resource a grant goes to. It is a
  // separate field from the opcode on purpose: the opcode says what to compute,
  // the class says where, and a design that derives one from the other ends up
  // with the routing decision duplicated in every consumer.
  //
  //   UOP_ALU     an integer ALU op; the cluster's ALU consumes it.
  //   UOP_BRANCH  a branch, JAL or JALR; the branch resolver consumes it and can
  //               redirect fetch. It may still write a link register.
  //   UOP_MULDIV  an M-extension op; the shared iterative unit consumes it.
  //   UOP_LOAD    a load; the LSU consumes it and the *result* is its value.
  //   UOP_STORE   a store; the LSU consumes it and it produces no register
  //               value at all -- its completion is a side-effect authorization.
  //   UOP_SYSTEM  a CSR access, ECALL/EBREAK/MRET, FENCE/FENCE.I or a hint. It
  //               completes at the architectural boundary, not in an execution
  //               unit, so it is issued straight to the retire path.
  typedef enum logic [2:0] {
    UOP_ALU    = 3'd0,
    UOP_BRANCH = 3'd1,
    UOP_MULDIV = 3'd2,
    UOP_LOAD   = 3'd3,
    UOP_STORE  = 3'd4,
    UOP_SYSTEM = 3'd5
  } uop_class_e;

  // ---------------------------------------------------------- uop metadata
  // Everything execution needs that is not an operand, a destination or the
  // identity. Kept as one packed struct so a consumer stores one field and
  // cannot accidentally store eight of them inconsistently.
  typedef struct packed {
    uop_class_e        class_;      // `class` is a reserved word
    logic [XLEN-1:0]   pc;          // the macro's own PC
    // The instruction's own length in bytes: 4 for a base-ISA instruction, 2 for
    // a 16-bit compressed one (I-041). It rides in the meta because the link
    // value every jump writes is `pc + length`, and the length is a property of
    // the instruction the branch unit is executing -- deriving it from the PC
    // (alignment) would be wrong, because a 32-bit instruction may start at a
    // two-byte-aligned address.
    logic [2:0]        insn_len;
    mosaic_pkg::alu_op_e alu_op;    // valid for UOP_ALU and UOP_BRANCH
    mosaic_pkg::md_op_e  md_op;     // valid for UOP_MULDIV
    logic              md_w;        // M-extension W form (32-bit operation)
    logic [2:0]        br_funct;    // branch condition, as encoded
    logic              is_jal;
    logic              is_jalr;
    logic              writes_link; // JAL/JALR write rd = PC + 4
    logic [2:0]        mem_size;    // SZ_BYTE..SZ_DBL
    logic              mem_signed;  // load sign extension
    // A extension (I-039). `is_amo` marks the memory macro as an atomic
    // read-modify-write; `amo_op` names it; `amo_aq`/`amo_rl` are the ordering
    // bits. They ride in the meta so the dispatch entry carries them with the
    // macro and the memory insert bus re-presents them without a second decode.
    logic              is_amo;
    mosaic_pkg::amo_op_e amo_op;
    logic              amo_aq;
    logic              amo_rl;
    logic              is_fence;    // FENCE / FENCE.I, drained by the memory path
    logic              is_fence_i;
  } uop_meta_t;

  // A source operand. `ready` means "the value in this packet is the final
  // value of that operand"; `tag`/`gen` identify the producer so a wakeup can
  // find it. A source that does not exist is presented ready with value 0 --
  // that keeps one readiness rule in one place instead of a second "has this
  // source" case in every consumer.
  typedef struct packed {
    logic [TAG_W-1:0]  tag;
    logic [GEN_W-1:0]  gen;
    logic              ready;
    logic [XLEN-1:0]   value;
  } src_operand_t;

  // The destination. `x0` means the architectural destination was x0 and
  // nothing may be allocated or written; it is not the same as tag 0.
  typedef struct packed {
    logic [TAG_W-1:0]  tag;
    logic [GEN_W-1:0]  gen;
    logic              x0;
  } dst_operand_t;

  // ------------------------------------------------------------- exceptions
  // A completion's exception payload. Kept separate from the value so a normal
  // result can never overwrite an exception: a design that merges them is the
  // failure I-025 names.
  typedef struct packed {
    logic              valid;
    logic [XLEN-1:0]   cause;
    logic [XLEN-1:0]   tval;
  } exc_payload_t;

  // ==========================================================================
  // completion: what an execution unit produces and the writeback path carries
  // ==========================================================================
  // `value_valid` is low for a uop that produces no architectural value (a
  // store, a fence, a branch without a link). The completion is still owed to
  // the ROB -- "this uop is done" is separate from "here is a value" -- which
  // is why the two are distinct fields rather than one `valid` bit.
  typedef struct packed {
    uop_id_t           id;
    dst_operand_t      dst;
    logic              value_valid;
    logic [XLEN-1:0]   value;
    exc_payload_t      exc;
    logic              is_store;   // carries store payload for the SQ
    logic              is_load;
  } wb_event_t;

  // ==========================================================================
  // memory: the LSU's request, its response, and the core's port to the SoC
  // ==========================================================================
  // The LSU request carries the *uncomputed* address: base and immediate. One
  // adder, in the LSU, so an ALU op and a memory op have the same operand shape
  // in the issue queue.
  typedef struct packed {
    uop_id_t           id;
    logic              we;          // 0 = load, 1 = store
    logic [XLEN-1:0]   base;
    logic [XLEN-1:0]   imm;
    logic [2:0]        size;        // mosaic_pkg::SZ_*
    logic              signed_;     // load sign extension
    logic [XLEN-1:0]   store_data;  // src2's value for a store, or the AMO operand
    // A extension (I-039). When `is_amo` is set this request is an atomic
    // read-modify-write of `size` bytes at `base + imm`, with the operand in
    // `store_data`; the endpoint performs it as *one* memory transaction (never
    // a load followed by a store) and returns the old value. `aq`/`rl` are
    // carried to the transaction, not dropped after decode.
    logic              is_amo;
    mosaic_pkg::amo_op_e amo_op;
    logic              aq;
    logic              rl;
  } lsu_req_t;

  typedef struct packed {
    uop_id_t           id;
    logic              fault;
    logic [XLEN-1:0]   cause;       // EXC_LOAD_* / EXC_STORE_* when fault
    logic [XLEN-1:0]   tval;
    logic [XLEN-1:0]   data;        // the loaded, extended value
  } lsu_rsp_t;

  // The core's port to the memory system, used by *both* the instruction side
  // and the data side so the SoC has one protocol to implement. Valid/ready on
  // both request and response, like the fetch unit's own contract: the request
  // may be offered before the memory is ready, and the response may take as
  // long as it takes. A response is matched to its request by `id`, and the id
  // space is wide enough that a stale response from a flushed epoch is
  // recognisable rather than merely unlikely (see I-009's epoch rule).
  typedef struct packed {
    logic              we;
    logic [XLEN-1:0]   addr;
    logic [2:0]        size;
    logic [XLEN/8-1:0] wstrb;      // byte strobes: a byte store must not clobber
    logic [XLEN-1:0]   wdata;
    // A extension (I-039). `amo` marks this memory transaction as an atomic
    // read-modify-write: the memory system must read `size` bytes at `addr`
    // (lane-aligned, `wdata` is the operand at lane 0), apply `amo_op`, write the
    // result back, and return the *old* window in `rdata`. It is one request and
    // one response -- the whole point of "not a loose load+store pair". `aq`/`rl`
    // travel with the transaction so an interconnect that honours them has them.
    // The fetch path and every ordinary load/store drive `amo` low, so the
    // memory system's behaviour for them is unchanged.
    logic              amo;
    mosaic_pkg::amo_op_e amo_op;
    logic              aq;
    logic              rl;
  } mem_req_t;

  typedef struct packed {
    logic [XLEN-1:0]   rdata;
    logic              fault;
  } mem_rsp_t;

  // ------------------------------------------------------------- helpers
  // `expected_wstrb` is the byte-strobe mask for a size and address. It is a
  // function rather than an expression at each call site so a byte store, a
  // halfword store and a doubleword store cannot disagree about which bytes
  // they own -- and so a misaligned store's mask is derived, not special-cased.
  function automatic logic [XLEN/8-1:0] expected_wstrb(input logic [2:0] size,
                                                       input logic [2:0] addr_low);
    logic [8:0] mask;
    begin
      case (size)
        mosaic_pkg::SZ_BYTE: mask = 9'h001;
        mosaic_pkg::SZ_HALF: mask = 9'h003;
        mosaic_pkg::SZ_WORD: mask = 9'h00f;
        default:             mask = 9'h0ff;
      endcase
      expected_wstrb = (XLEN/8)'(mask << addr_low);
    end
  endfunction

  // The number of bytes one access covers. Derived from the size code, so an
  // access cannot be one size for its address computation and another for its
  // strobes.
  function automatic logic [XLEN-1:0] size_bytes(input logic [2:0] size);
    begin
      case (size)
        mosaic_pkg::SZ_BYTE: size_bytes = 64'd1;
        mosaic_pkg::SZ_HALF: size_bytes = 64'd2;
        mosaic_pkg::SZ_WORD: size_bytes = 64'd4;
        default:             size_bytes = 64'd8;
      endcase
    end
  endfunction

  // ------------------------------------------------------------ the device map
  // `is_device_addr` is the PMA's *device* predicate (work package I-038): true
  // when an access to this address must be serialized on the non-speculative
  // path rather than executed like ordinary memory.
  //
  // The source of truth is `config/memory/p0.json`, which records for every
  // region whether it is idempotent. The three regions it marks **not**
  // idempotent -- uart, test_harness, clint -- are exactly the three named
  // here, through the generated `mosaic_cfg_pkg` bases and sizes. boot_rom and
  // ram are idempotent: a read of them has no side effect, so a repeated or
  // speculative read is allowed and no serialization is needed.
  //
  // It is one function, called by the serializer that gates an access and by the
  // two queues that decide whether a store may feed a load, so "which addresses
  // are devices" cannot differ between the module that orders the access and the
  // module that decides whether it may be merged with the RAM path.
  function automatic logic is_device_addr(input logic [XLEN-1:0] addr);
    logic in_uart;
    logic in_harness;
    logic in_clint;
    begin
      // The subtraction is the containment test: for an address below the base
      // the wrapped difference is far larger than any region size, so an
      // address outside a region can never be mistaken for one inside it.
      in_uart    = (addr - mosaic_cfg_pkg::MOSAIC_UART_BASE) <
                   mosaic_cfg_pkg::MOSAIC_UART_SIZE;
      in_harness = (addr - mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_BASE) <
                   mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_SIZE;
      in_clint   = (addr - mosaic_cfg_pkg::MOSAIC_CLINT_BASE) <
                   mosaic_cfg_pkg::MOSAIC_CLINT_SIZE;
      is_device_addr = in_uart || in_harness || in_clint;
    end
  endfunction

  // A uop id matches only when every field matches, including the generation.
  // Comparing the wrapping rob_index alone is how a late completion from a
  // recycled slot is mistaken for a live one.
  function automatic logic uop_id_eq(input uop_id_t a, input uop_id_t b);
    begin
      uop_id_eq = (a.hart == b.hart) && (a.rob_index == b.rob_index)
               && (a.rob_gen == b.rob_gen) && (a.uop_index == b.uop_index);
    end
  endfunction

  /* verilator lint_on UNUSEDPARAM */
endpackage : mosaic_uop_pkg

`endif  // MOSAIC_UOP_PKG_SV_
