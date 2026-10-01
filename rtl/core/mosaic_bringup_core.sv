// ============================================================================
// mosaic_bringup_core -- work package I-008.  BRING-UP ONLY.  NOT THE PROCESSOR.
//
// WHAT THIS IS
// ------------
// A single-issue, in-order, non-pipelined, non-speculative machine-mode scalar
// core whose entire job is to produce a trustworthy *architectural retire
// stream* so that frontend/ISA defects in the real MosaicRV core can be
// localised by differential comparison against an independent reference.
//
// WHAT THIS IS NOT
// ----------------
// It is not the MosaicRV processor, and no measurement taken with it is
// evidence about the eventual out-of-order fabric.  There is no branch
// prediction, no speculation, no out-of-order execution, no rename, no ROB, no
// caches, no store buffer and no pipeline at all.  docs/implementation-plan.md
// section 1.1 lists what p0 may claim; nothing in this file adds to that list,
// and section 1.3's fabric contract (generation-tagged credits, multi-entry
// queues, remote completion) is deliberately *not* modelled here.  Every
// performance, occupancy or hazard number must come from the real core.
//
// docs/stage-0-contracts-bringup.md `### I-008` names the blocking risk
// precisely: reporting processor-RTL completion on the strength of a software
// or simplified path.  The module header, the `misa` documentation in
// results/reports/I-008-bringup.md and every event this core emits say
// "bring-up" for that reason.
//
// ISA SCOPE OF THIS BUILD
// ----------------------
//   RV64 I  M  Zicsr  Zifencei, machine mode only.  Exactly the p0 string in
//   docs/implementation-plan.md section 1.1.  Deliberately absent: A, C, F, D,
//   V, S, U, Sv39 and every cache.  A 16-bit (compressed) instruction raises
//   illegal instruction; I-041 owns C in p1 and will arrive as its own front
//   end.  `fence` and `fence.i` are accepted and retire as architectural
//   no-ops: this machine has no cache and no store buffer, so there is nothing
//   for a fence to order.  That is a stated simplification of the *ordering*
//   machinery, not an oversight, and it is safe only because there is no
//   reorder to suppress.
//
// DEPENDENCIES AND WHY THEY ARE INLINE
// ------------------------------------
// The shared `mosaic_pkg` types are used (opcodes, alu_op_e, md_op_e,
// mem_kind_e, csr_op_e, decode_ctl_t, the EXC_* codes from Privileged
// Specification v1.12).  The decode and the integer ALU are *private to this
// module* rather than instantiated from rtl/core/mosaic_decoder.sv and
// rtl/core/mosaic_alu.sv.  That is a deliberate choice for a reference path,
// not duplication for its own sake: the bring-up path has to be readable and
// auditable end to end as one file, and it has to be buildable while those
// shared blocks are still landing.  It also gives the differential comparison a
// second, independently written decode, which is the point of the package.
//
// Microarchitecture
// -----------------
// One instruction is in flight at any time.  There is no forwarding network at
// all: the register file is read combinationally in the execute state and
// written on the clock edge that ends the committing state, so every operand an
// instruction sees is the architecturally committed value.  That is why the
// machine is in-order by construction rather than by enforcement.
//
//   S_FETCH  post one instruction-fetch request at `pc`.  The only work done
//            here is the instruction-address-misaligned check (cause 0), which
//            is the one trap that does not need an instruction.
//   S_FWAIT  wait for `ifetch_ack`.  A fetch fault is cause 1, tval = pc.
//   S_EXEC   decode + execute + commit, entirely combinational except for the
//            architectural writes made on this state's closing edge.  Every
//            synchronous trap except the data-access ones is taken here.
//   S_DREQ   post one data request for the load/store latched by S_EXEC.
//   S_DWAIT  wait for `dmem_ack`.  A fault is cause 5 (load) or 7 (store),
//            tval = the faulting address.  On `dmem_ack` with no fault the
//            instruction commits here.
//
// There is no S_COMMIT state: a machine with one instruction in flight has
// nothing to wait for between "execute finished" and "commit", so the commit
// happens in whichever state finished the instruction.  The register file, the
// CSR file and the architectural PC are written on the closing edge of S_EXEC
// or S_DWAIT and nowhere else.  There is no flush, no squash, no redirect
// queue and no kill, because no instruction after the current one exists to be
// redirected.
//
// Where a stall can come from -- exhaustively:
//   * `ifetch_ack_i` not asserted (the fetch memory is not ready).  S_FWAIT.
//   * `dmem_ack_i` not asserted (the data memory is not ready).  S_DWAIT.
//   * `rst_i` asserted, which holds the machine in S_FETCH with the PC at the
//     reset vector.
//   That is the complete list.  There is no hazard unit, no structural
//   interlock, no backpressure path and no arbitration, because there is
//   exactly one requester per port and one instruction per machine.
//
// MEMORY INTERFACE CONTRACT (the harness author implements this text)
// -------------------------------------------------------------------
// Two ports.  One outstanding request per port.  A port request is asserted for
// exactly one cycle and the core accepts nothing new on that port until the
// matching ack has been observed.
//
// Instruction fetch port:
//   ifetch_req_o   High for exactly one cycle, in state S_FETCH.  ifetch_addr_o
//                  holds the byte address being fetched and is aligned to 4.
//   ifetch_ack_i   The environment must assert it for exactly one cycle, on a
//                  later cycle than ifetch_req_o (at least one cycle of
//                  latency).  It may be returned in any later cycle; the core
//                  holds state and ignores every port signal until it arrives.
//   ifetch_rdata_i Valid on the cycle ifetch_ack_i is high: the 32-bit
//                  instruction word at ifetch_addr_o, little-endian.
//   ifetch_fault_i Valid on the cycle ifetch_ack_i is high: the fetch did not
//                  complete.  The core takes instruction access fault (cause 1)
//                  with tval = the faulting address.  rdata is ignored.
//   The core does not interpret rdata and does not check alignment on the
//   returned data; it checks alignment on the *address* in S_FETCH.
//
// Data port:
//   dmem_req_o     High for exactly one cycle, in state S_DREQ.
//   dmem_we_o      1 for a store, 0 for a load.  Valid with dmem_req_o.
//   dmem_addr_o    Byte address, valid with dmem_req_o.
//   dmem_size_o    Access width in bytes as an encoded size (0=1, 1=2, 2=4,
//                  3=8; mosaic_pkg::SZ_*).  Valid with dmem_req_o.
//   dmem_wdata_o   Store data, right-justified in the low dmem_size_o bytes.
//                  Undefined for a load.
//   dmem_ack_i     Exactly one cycle, on a later cycle than dmem_req_o.  The
//                  core accepts nothing new until it arrives.
//   dmem_rdata_i   Valid on the dmem_ack_i cycle of a load: the dmem_size_o
//                  bytes starting at dmem_addr_o, right-justified.  The core
//                  performs all sign/zero extension itself.
//   dmem_fault_i   Valid on the dmem_ack_i cycle: the access did not complete.
//                  The core takes load access fault (cause 5) for a load and
//                  store/AMO access fault (cause 7) for a store, with
//                  tval = dmem_addr_o.  For a store the environment must not
//                  have performed the write.
//
// Two contract points the environment must honour, because the core depends on
// them:
//   1. A store to boot_rom faults even though the region is readable and
//      mappable, because config/memory/p0.json marks it `"writable": false`
//      with `"error_response": "access_fault"`.  "Fault" here means the
//      environment asserts dmem_fault_i, not that it silently drops the write.
//   2. The core performs every misalignment check itself (causes 4 and 6) and
//      never issues a misaligned request, so the environment is not required to
//      implement a misalignment policy.  A permissive environment that
//      assembles a misaligned access from adjacent bytes is therefore a
//      conforming environment, and that is what makes the misalignment
//      negative control meaningful: skipping the check produces a load that
//      succeeds instead of a trap, not a differently-numbered trap.
//
// ARCHITECTURAL EVENT OUTPUT
// --------------------------
// Exactly one event per architectural instruction, emitted at commit, in
// program order.  `evt_valid_o` is a one-cycle pulse.  A trapping instruction
// produces a TRAP event and *never* an ordinary retire: `evt_has_rd_o` is 0,
// `evt_is_store_o` is 0, `evt_next_pc_o` is the trap vector, and cause/tval/epc
// are populated.  docs/implementation-plan.md section 1.3 calls this out as an
// ABA/aliasing hazard, and `MOSAIC_BRINGUP_MUTANT_5` injects exactly that defect.
//
// Acceptance evidence for this module is results/reports/I-008-bringup.md.
// ============================================================================

`default_nettype none

// File-scope, not inside the module: an import written in the module body does
// not reach the port list, which is elaborated first.
import mosaic_pkg::*;

module mosaic_bringup_core #(
    parameter int XLEN = 64
) (
    input  wire             clk_i,
    input  wire             rst_i,
    // config/profiles/p0.json -> reset.reset_vector.  An input rather than a
    // localparam so the frozen value has exactly one owner: the harness.
    input  wire  [XLEN-1:0] reset_vector_i,

    // ---------------------------------------------------- instruction fetch
    output wire             ifetch_req_o,
    output wire  [XLEN-1:0] ifetch_addr_o,
    input  wire             ifetch_ack_i,
    input  wire  [31:0]     ifetch_rdata_i,
    input  wire             ifetch_fault_i,

    // ------------------------------------------------------------ data port
    output wire             dmem_req_o,
    output wire             dmem_we_o,
    output wire  [XLEN-1:0] dmem_addr_o,
    output wire  [2:0]      dmem_size_o,
    output wire  [XLEN-1:0] dmem_wdata_o,
    input  wire             dmem_ack_i,
    input  wire  [XLEN-1:0] dmem_rdata_i,
    input  wire             dmem_fault_i,

    // ------------------------------------------------- architectural event
    output logic            evt_valid_o,
    output logic            evt_trap_o,
    output logic  [XLEN-1:0] evt_pc_o,
    output logic  [XLEN-1:0] evt_next_pc_o,
    output logic  [31:0]    evt_insn_o,
    output logic            evt_has_rd_o,
    output logic  [4:0]     evt_rd_o,
    output logic  [XLEN-1:0] evt_rd_value_o,
    output logic  [XLEN-1:0] evt_cause_o,
    output logic  [XLEN-1:0] evt_tval_o,
    output logic  [XLEN-1:0] evt_epc_o,
    output logic            evt_is_store_o,
    output logic  [XLEN-1:0] evt_store_addr_o,
    output logic  [XLEN-1:0] evt_store_data_o,
    output logic  [2:0]     evt_store_size_o,

    // -------------------------------------------------------- observability
    // The harness reads CSRs for the negative controls (an illegal CSR access
    // must leave no architectural state behind) and for the reset values.  One
    // combinational read port is enough for a testbench; the real core will get
    // the full CSRR port for free because it really implements the file.
    input  wire  [11:0]      dbg_csr_addr_i,
    output logic [XLEN-1:0]  dbg_csr_data_o,
    output logic [XLEN-1:0]  dbg_pc_o,
    output logic [2:0]       dbg_state_o
);
  // ==========================================================================
  // Frozen constants, all traceable to configuration
  // ==========================================================================

  // config/csr/mode_m.json -> "misa": reset 9223372036854780160 =
  // 0x8000000000001100 = MXL=0b10 (RV64) in bits 63:62 plus extension bits
  // I (bit 8) and M (bit 12).  Transcribed, not invented; tb_bringup.cpp re-reads
  // the JSON and fails if this literal ever stops matching it.
  localparam logic [63:0] MISA_RESET = 64'h8000000000001100;

  // config/csr/mode_m.json -> "mstatus": reset 6144 = 0x1800, i.e. MPP = 0b11
  // (M-mode) and nothing else set.
  localparam logic [63:0] MSTATUS_RESET = 64'h0000000000001800;

  localparam int MSTATUS_MIE_BIT  = 3;
  localparam int MSTATUS_MPIE_BIT = 7;

  // mstatus field masks.  MPP (12:11) is WARL and this machine implements only
  // M-mode, so it is fixed at 0b11 on every read and every write.  MIE (3) and
  // MPIE (7) are the two fields this profile actually manipulates.  The JSON
  // also lists SIE (1) and SPIE (5) as writable fields, but p0 has no S-mode
  // (config/profiles/p0.json -> privilege_modes = ["M"]) and Privileged Spec
  // v1.12 requires SIE/SPIE to read as zero when S-mode is not implemented, so
  // a write to them is accepted and has no effect.  That is WARL-legal and it
  // is the only place this implementation and the JSON's writable_fields list
  // are reconciled; see results/reports/I-008-bringup.md.
  localparam logic [63:0] MSTATUS_MIE   = 64'h0000000000000008;
  localparam logic [63:0] MSTATUS_MPIE  = 64'h0000000000000080;
  localparam logic [63:0] MSTATUS_MPP_M = 64'h0000000000001800;
  localparam logic [63:0] MSTATUS_WMASK = 64'h0000000000000088;

  // mie/mip writable fields (config/csr/mode_m.json): only bits 7 and 3.
  localparam logic [63:0] MIE_WMASK = 64'h0000000000000088;

  // CSR numbers, decimal in config/csr/mode_m.json, hex here.
  localparam logic [11:0] CSR_MSTATUS    = 12'h300;
  localparam logic [11:0] CSR_MISA       = 12'h301;
  localparam logic [11:0] CSR_MEDELEG    = 12'h302;
  localparam logic [11:0] CSR_MIDELEG    = 12'h303;
  localparam logic [11:0] CSR_MIE        = 12'h304;
  localparam logic [11:0] CSR_MTVEC      = 12'h305;
  localparam logic [11:0] CSR_MCOUNTEREN = 12'h306;
  localparam logic [11:0] CSR_MSCRATCH   = 12'h340;
  localparam logic [11:0] CSR_MEPC       = 12'h341;
  localparam logic [11:0] CSR_MCAUSE     = 12'h342;
  localparam logic [11:0] CSR_MTVAL      = 12'h343;
  localparam logic [11:0] CSR_MIP        = 12'h344;
  localparam logic [11:0] CSR_MVENDORID  = 12'hF11;
  localparam logic [11:0] CSR_MARCHID    = 12'hF12;
  localparam logic [11:0] CSR_MIMPID     = 12'hF13;
  localparam logic [11:0] CSR_MHARTID    = 12'hF14;
  localparam logic [11:0] CSR_MCYCLE     = 12'hB00;
  localparam logic [11:0] CSR_MINSTRET   = 12'hB02;
  localparam logic [11:0] CSR_CYCLE      = 12'hC00;
  localparam logic [11:0] CSR_TIME       = 12'hC01;
  localparam logic [11:0] CSR_INSTRET    = 12'hC02;


  // ==========================================================================
  // Architectural state
  // ==========================================================================

  typedef enum logic [2:0] {
    S_FETCH = 3'd0,  // post the fetch request
    S_FWAIT = 3'd1,  // wait for ifetch_ack
    S_EXEC  = 3'd2,  // decode, execute, commit
    S_DREQ  = 3'd3,  // post the data request
    S_DWAIT = 3'd4   // wait for dmem_ack, then commit
  } state_e;

  state_e         state_q;
  logic [XLEN-1:0] pc_q;
  logic [31:0]     ir_q;

  // Architectural integer registers.  x0 is storage like any other index so
  // that a read of an unwritten index is defined; the hardwired-zero rule is
  // applied at the read mux and at the commit write enable, not by leaving a
  // hole in the array.
  logic [XLEN-1:0] regs_q [0:31];

  // Data request, latched by S_EXEC and consumed by S_DREQ/S_DWAIT.
  logic [XLEN-1:0] daddr_q;
  logic [XLEN-1:0] dwdata_q;
  logic [2:0]      dsize_q;

  // CSR file.  Only registers that actually change are stored; the rest are
  // either constants or counters derived from another register.
  logic [XLEN-1:0] mstatus_q;
  logic [XLEN-1:0] medeleg_q;
  logic [XLEN-1:0] mideleg_q;
  logic [XLEN-1:0] mie_q;
  logic [XLEN-1:0] mtvec_q;
  logic [XLEN-1:0] mscratch_q;
  logic [XLEN-1:0] mepc_q;
  logic [XLEN-1:0] mcause_q;
  logic [XLEN-1:0] mtval_q;
  logic [XLEN-1:0] mip_q;
  logic [XLEN-1:0] mcycle_q;
  logic [XLEN-1:0] minstret_q;

  // ==========================================================================
  // Pure functions
  // ==========================================================================

  // Sign-extend the low `bits` of `value` to XLEN.
  function automatic logic [XLEN-1:0] sext(input logic [XLEN-1:0] value,
                                           input int unsigned bits);
    logic [XLEN-1:0] mask;
    mask = {{(XLEN - 1){1'b0}}, 1'b1} << bits;
    sext = (value & ~mask) | (mask & {XLEN{value[bits - 1]}});
  endfunction

  // Byte mask for an encoded access size.
  function automatic logic [63:0] size_mask(input logic [2:0] sz);
    case (sz)
      SZ_BYTE:  size_mask = 64'h00000000000000ff;
      SZ_HALF:  size_mask = 64'h000000000000ffff;
      SZ_WORD:  size_mask = 64'h00000000ffffffff;
      default:  size_mask = 64'hffffffffffffffff;
    endcase
  endfunction

  function automatic logic [63:0] size_bytes(input logic [2:0] sz);
    case (sz)
      SZ_BYTE:  size_bytes = 64'd1;
      SZ_HALF:  size_bytes = 64'd2;
      SZ_WORD:  size_bytes = 64'd4;
      default:  size_bytes = 64'd8;
    endcase
  endfunction

  // Load data placement.  The environment returns the accessed bytes
  // right-justified; every sign/zero extension happens here, so the memory
  // interface itself carries no signedness.
  function automatic logic [XLEN-1:0] load_extend(input logic [XLEN-1:0] raw,
                                                  input logic [2:0] sz,
                                                  input logic is_signed);
    case (sz)
      SZ_BYTE:  load_extend = is_signed ? {{(XLEN - 8){raw[7]}},  raw[7:0]}
                                        : {{(XLEN - 8){1'b0}},  raw[7:0]};
      SZ_HALF:  load_extend = is_signed ? {{(XLEN - 16){raw[15]}}, raw[15:0]}
                                        : {{(XLEN - 16){1'b0}}, raw[15:0]};
      SZ_WORD:  load_extend = is_signed ? {{(XLEN - 32){raw[31]}}, raw[31:0]}
                                        : {{(XLEN - 32){1'b0}}, raw[31:0]};
      default:  load_extend = raw;
    endcase
  endfunction

  // The integer ALU, private to this module.  Three RV64 rules are implemented
  // here and nowhere else: the W forms operate on 32 bits and sign-extend, the
  // shift amount is masked to the width of the *destination* (6 bits for XLEN,
  // 5 bits for W), and comparisons are full-width 0/1 with per-op signedness.
  function automatic logic [XLEN-1:0] alu_eval(input logic [XLEN-1:0] a,
                                                input logic [XLEN-1:0] b,
                                                input logic [3:0]      op);
    logic [31:0]     word;
    logic [XLEN-1:0] shifted;

    shifted = $signed(a) >>> b[5:0];

    case (op)
      ALU_ADD:   alu_eval = a + b;
      ALU_SUB:   alu_eval = a - b;
      ALU_SLL:   alu_eval = a << b[5:0];
      ALU_SLT:   alu_eval = ($signed(a) < $signed(b)) ? 64'd1 : 64'd0;
      ALU_SLTU:  alu_eval = (a < b) ? 64'd1 : 64'd0;
      ALU_XOR:   alu_eval = a ^ b;
      ALU_SRL:   alu_eval = a >> b[5:0];
      ALU_SRA:   alu_eval = shifted;
      ALU_OR:    alu_eval = a | b;
      ALU_AND:   alu_eval = a & b;

      ALU_ADDW: begin
        word = a[31:0] + b[31:0];
        alu_eval = {{(XLEN - 32){word[31]}}, word};
      end
      ALU_SUBW: begin
        word = a[31:0] - b[31:0];
        alu_eval = {{(XLEN - 32){word[31]}}, word};
      end
      ALU_SLLW: begin
        word = a[31:0] << b[4:0];
        alu_eval = {{(XLEN - 32){word[31]}}, word};
      end
      ALU_SRLW: begin
        word = a[31:0] >> b[4:0];
        alu_eval = {{(XLEN - 32){word[31]}}, word};
      end
      ALU_SRAW: begin
        word = $signed(a[31:0]) >>> b[4:0];
        alu_eval = {{(XLEN - 32){word[31]}}, word};
      end

      ALU_PASSB: alu_eval = b;
      default:   alu_eval = 64'd0;
    endcase
  endfunction

  // The M extension, private to this module.  Complete, not partial: every
  // RV64 division corner case is handled, because a bring-up path that got
  // divide-by-zero wrong would produce a stream that disagrees with the
  // reference for reasons that have nothing to do with the defect under test.
  // rtl/core/mosaic_muldiv.sv (I-012) does not exist in this build; this
  // combinational version is the bring-up stand-in and is labelled as such.
  // The signed high multiplies are unsigned 128-bit products of sign-extended
  // halves rather than operand casts, so the signedness is visible in the
  // expression instead of hidden in a promotion rule.
  function automatic logic [XLEN-1:0] muldiv_eval(input logic [XLEN-1:0] a,
                                                  input logic [XLEN-1:0] b,
                                                  input logic [2:0]      op);
    logic [XLEN-1:0]        prod_ss_hi;
    logic [XLEN-1:0]        prod_su_hi;
    logic [XLEN-1:0]        prod_uu_hi;
    logic [XLEN-1:0]        prod_uu_lo;
    logic signed [XLEN-1:0] sa;
    logic signed [XLEN-1:0] sb;

    sa = a;
    sb = b;

    // Only the halves each operation actually returns are kept, so no 128-bit
    // temporary is left holding bits nobody reads.
    prod_ss_hi = XLEN'(({{XLEN{a[XLEN-1]}}, a} * {{XLEN{b[XLEN-1]}}, b}) >> XLEN);
    prod_su_hi = XLEN'(({{XLEN{a[XLEN-1]}}, a} * {{XLEN{1'b0}},   b}) >> XLEN);
    prod_uu_hi = XLEN'(({{XLEN{1'b0}},   a} * {{XLEN{1'b0}},   b}) >> XLEN);
    prod_uu_lo = {{XLEN{1'b0}}, a} * {{XLEN{1'b0}}, b};

    case (op)
      MD_MUL:    muldiv_eval = prod_uu_lo;
      MD_MULH:   muldiv_eval = prod_ss_hi;
      MD_MULHSU: muldiv_eval = prod_su_hi;
      MD_MULHU:  muldiv_eval = prod_uu_hi;


      // Division by zero is defined, not undefined: the quotient is all ones
      // and the remainder is the dividend.  Signed overflow (most-negative
      // divided by minus one) is likewise defined rather than a trap.
      MD_DIV: begin
        if (b == {XLEN{1'b0}})                     muldiv_eval = {XLEN{1'b1}};
        else if (a[XLEN-1] && (b == {XLEN{1'b1}})) muldiv_eval = a;
        else                                      muldiv_eval = sa / sb;
      end
      MD_DIVU: begin
        if (b == {XLEN{1'b0}}) muldiv_eval = {XLEN{1'b1}};
        else                  muldiv_eval = a / b;
      end
      MD_REM: begin
        if (b == {XLEN{1'b0}})                     muldiv_eval = a;
        else if (a[XLEN-1] && (b == {XLEN{1'b1}})) muldiv_eval = {XLEN{1'b0}};
        else                                      muldiv_eval = sa % sb;
      end
      MD_REMU: begin
        if (b == {XLEN{1'b0}}) muldiv_eval = a;
        else                  muldiv_eval = a % b;
      end
      default: muldiv_eval = {XLEN{1'b0}};
    endcase
  endfunction

  // A decode_ctl_t that is unambiguously illegal: nothing valid, nothing
  // usable, illegal set.  One construction, so no illegal path can leave a
  // half-populated field behind.
  function automatic decode_ctl_t illegal_op();
    decode_ctl_t d;
    d = '0;
    d.illegal = 1'b1;
    illegal_op = d;
  endfunction

  // ==========================================================================
  // Decode
  // ==========================================================================

  // Opcode 1000011 is OP-FP: FMADD/FMSUB/FNMSUB/FNMADD, i.e. the F and D
  // extensions.  It is absent from mosaic_pkg's opcode map precisely because
  // those extensions are not in p0, so it has no localparam there.  The
  // constant exists only inside the negative control: in the shipping build the
  // opcode simply falls through to `illegal_op()` and a constant with no
  // reader would be dead weight the linter is right to complain about.
`ifdef MOSAIC_BRINGUP_MUTANT_2
  localparam logic [6:0]  OP_FP         = 7'b1000011;
`endif

  function automatic decode_ctl_t decode(input logic [31:0] ir);
    decode_ctl_t      d;
    logic [6:0]       opcode;
    logic [2:0]       funct3;
    logic [6:0]       funct7;
    logic [XLEN-1:0]  imm;

    d      = '0;
    opcode = ir[6:0];
    funct3 = ir[14:12];
    funct7 = ir[31:25];
    imm    = {XLEN{1'b0}};

    d.rd  = ir[11:7];
    d.rs1 = ir[19:15];
    d.rs2 = ir[24:20];

`ifdef MOSAIC_BRINGUP_MUTANT_2
    // NEGATIVE CONTROL: accept the F/D mul-add opcode (OP-FP) as if it were an
    // ordinary add.  The real build has no F and no D and must raise illegal
    // instruction on every one of those encodings.
    if (opcode == OP_FP) begin
      d.uses_rs1  = 1'b1;
      d.uses_rs2  = 1'b1;
      d.uses_alu  = 1'b1;
      d.alu_op    = ALU_ADD;
      d.reg_write = 1'b1;
      d.valid     = 1'b1;
      decode      = d;
      return;
    end
`endif

    case (opcode)
      // ---------------------------------------------------------------- LOAD
      OP_LOAD: begin
        imm = sext({{(XLEN - 12){ir[31]}}, ir[31:20]}, 12);
        d.uses_rs1  = 1'b1;
        d.uses_imm  = 1'b1;
        d.imm       = imm;
        d.mem_kind  = MEM_LOAD;
        d.reg_write = 1'b1;
        d.valid     = 1'b1;
        case (funct3)
          F3_ADD_SUB:  begin d.mem_size = SZ_BYTE; d.mem_signed = 1'b1; end  // lb
          F3_SLL:      begin d.mem_size = SZ_HALF; d.mem_signed = 1'b1; end  // lh
          F3_SLT:      begin d.mem_size = SZ_WORD; d.mem_signed = 1'b1; end  // lw
          F3_SLTU:     begin d.mem_size = SZ_DBL;  d.mem_signed = 1'b1; end  // ld
          F3_XOR:      begin d.mem_size = SZ_BYTE; d.mem_signed = 1'b0; end  // lbu
          F3_SRL_SRA:  begin d.mem_size = SZ_HALF; d.mem_signed = 1'b0; end  // lhu
          F3_OR:       begin d.mem_size = SZ_WORD; d.mem_signed = 1'b0; end  // lwu
          default:     d = illegal_op();                                      // 111
        endcase
      end

      // --------------------------------------------------------------- STORE
      OP_STORE: begin
        imm = sext({{(XLEN - 12){ir[31]}}, ir[31:25], ir[11:7]}, 12);
        d.uses_rs1 = 1'b1;
        d.uses_rs2 = 1'b1;
        d.uses_imm = 1'b1;
        d.imm      = imm;
        d.mem_kind = MEM_STORE;
        d.valid    = 1'b1;
        case (funct3)
          F3_ADD_SUB: d.mem_size = SZ_BYTE;
          F3_SLL:     d.mem_size = SZ_HALF;
          F3_SLT:     d.mem_size = SZ_WORD;
          F3_SLTU:    d.mem_size = SZ_DBL;
          default:    d = illegal_op();
        endcase
      end

      // ------------------------------------------------------- integer imm
      OP_IMM: begin
        imm = sext({{(XLEN - 12){ir[31]}}, ir[31:20]}, 12);
        d.uses_rs1  = 1'b1;
        d.uses_imm  = 1'b1;
        d.imm       = imm;
        d.uses_alu  = 1'b1;
        d.reg_write = 1'b1;
        d.valid     = 1'b1;
        case (funct3)
          F3_ADD_SUB: d.alu_op = ALU_ADD;
          F3_SLL: begin
            // RV64 shifts the XLEN register by 6 bits; bit 25 belongs to the
            // immediate in RV32 and is reserved here.
            if (ir[31:26] != 6'b000000) d = illegal_op();
            else                          d.alu_op = ALU_SLL;
          end
          F3_SLT:     d.alu_op = ALU_SLT;
          F3_SLTU:    d.alu_op = ALU_SLTU;
          F3_XOR:     d.alu_op = ALU_XOR;
          F3_SRL_SRA: begin
            if (ir[31:26] != 6'b000000) d = illegal_op();
            else if (ir[25])            d.alu_op = ALU_SRA;
            else                        d.alu_op = ALU_SRL;
          end
          F3_OR:      d.alu_op = ALU_OR;
          F3_AND:     d.alu_op = ALU_AND;
          default:    d = illegal_op();
        endcase
      end

      // ---------------------------------------------------- integer imm, 32-bit
      OP_IMM_32: begin
        imm = sext({{(XLEN - 12){ir[31]}}, ir[31:20]}, 12);
        d.uses_rs1  = 1'b1;
        d.uses_imm  = 1'b1;   // the shift amount travels in imm, not in rs2
        d.imm       = imm;
        d.uses_alu  = 1'b1;
        d.reg_write = 1'b1;
        d.valid     = 1'b1;
        case (funct3)
          F3_ADD_SUB: d.alu_op = ALU_ADDW;
          F3_SLL: begin
            if (ir[25]) d = illegal_op();
            else        d.alu_op = ALU_SLLW;
          end
          F3_SRL_SRA: begin
            if (ir[31:26] != 6'b000000) d = illegal_op();
            else if (ir[25])            d.alu_op = ALU_SRAW;
            else                        d.alu_op = ALU_SRLW;
          end
          default: d = illegal_op();
        endcase
      end

      // --------------------------------------------------------------- AUIPC
      OP_AUIPC: begin
        imm = sext({{(XLEN - 32){ir[31]}}, ir[31:12], 12'b0}, 32);
        d.uses_rs1  = 1'b0;
        d.uses_imm  = 1'b1;
        d.imm       = imm;
        d.is_auipc  = 1'b1;
        d.uses_alu  = 1'b1;
        d.alu_op    = ALU_ADD;
        d.reg_write = 1'b1;
        d.valid     = 1'b1;
      end

      // -------------------------------------------------------------- BRANCH
      OP_BRANCH: begin
        imm = sext({{(XLEN - 13){ir[31]}}, ir[31], ir[7], ir[30:25], ir[11:8],
                    1'b0}, 13);
        d.uses_rs1     = 1'b1;
        d.uses_rs2     = 1'b1;
        d.uses_imm     = 1'b1;
        d.imm          = imm;
        d.is_branch    = 1'b1;
        d.branch_funct = funct3;
        d.valid        = 1'b1;
        // funct3 010 and 011 are reserved.
        if ((funct3 == 3'b010) || (funct3 == 3'b011)) d = illegal_op();
      end

      // --------------------------------------------------------- JAL / JALR
      OP_JAL: begin
        imm = sext({{(XLEN - 21){ir[31]}}, ir[31], ir[19:12], ir[20], ir[30:21],
                    1'b0}, 21);
        d.uses_imm   = 1'b1;
        d.imm        = imm;
        d.is_jal     = 1'b1;
        d.writes_link = 1'b1;
        d.reg_write  = 1'b1;
        d.valid      = 1'b1;
      end

      OP_JALR: begin
        imm = sext({{(XLEN - 12){ir[31]}}, ir[31:20]}, 12);
        d.uses_rs1    = 1'b1;
        d.uses_imm    = 1'b1;
        d.imm         = imm;
        d.is_jalr     = 1'b1;
        d.writes_link = 1'b1;
        d.reg_write   = 1'b1;
        d.valid       = 1'b1;
        if (funct3 != F3_ADD_SUB) d = illegal_op();
      end

      // ---------------------------------------------------------- FENCE / .I
      OP_MISC_MEM: begin
        d.is_miscmem = 1'b1;
        d.valid      = 1'b1;
        if (funct3 == F3_ADD_SUB) begin
          d.is_fence_i = 1'b0;                     // fence
        end else if (funct3 == F3_SLL) begin
          d.is_fence_i = 1'b1;                     // fence.i
          if (ir[31:28] != 4'b0000) d = illegal_op();
        end else begin
          d = illegal_op();
        end
        // rs1 and rd are reserved and architecturally zero in both forms.
        if ((ir[19:15] != 5'b00000) || (ir[11:7] != 5'b00000)) d = illegal_op();
      end

      // ------------------------------------------------------- OP (reg, reg)
      OP_MUL_DIV: begin
        d.uses_rs1 = 1'b1;
        d.uses_rs2 = 1'b1;
        d.valid    = 1'b1;
        if (funct7 == 7'b0000001) begin
          d.is_muldiv = 1'b1;
          d.reg_write = 1'b1;
          case (funct3)
            F3_ADD_SUB: d.md_op = MD_MUL;
            F3_SLL:     d.md_op = MD_MULH;
            F3_SLT:     d.md_op = MD_MULHSU;
            F3_SLTU:    d.md_op = MD_MULHU;
            F3_SRL_SRA: begin d.md_op = MD_DIV;  d.md_signed = 1'b1; end
            F3_XOR:     begin d.md_op = MD_DIVU; d.md_signed = 1'b0; end
            F3_OR:      begin d.md_op = MD_REM;  d.md_signed = 1'b1; end
            F3_AND:     begin d.md_op = MD_REMU; d.md_signed = 1'b0; end
            default:    d = illegal_op();
          endcase
        end else if (funct7 == 7'b0000000) begin
          d.uses_alu  = 1'b1;
          d.reg_write = 1'b1;
          case (funct3)
            F3_ADD_SUB: d.alu_op = ALU_ADD;
            F3_SLL:     d.alu_op = ALU_SLL;
            F3_SLT:     d.alu_op = ALU_SLT;
            F3_SLTU:    d.alu_op = ALU_SLTU;
            F3_XOR:     d.alu_op = ALU_XOR;
            F3_SRL_SRA: d.alu_op = ALU_SRL;
            F3_OR:      d.alu_op = ALU_OR;
            F3_AND:     d.alu_op = ALU_AND;
            default:    d = illegal_op();
          endcase
        end else if (funct7 == 7'b0100000) begin
          // Only sub exists in this form.  srawi and its relatives are
          // immediate-form instructions and are not encodable with rs2.
          d.uses_alu  = 1'b1;
          d.reg_write = 1'b1;
          if (funct3 == F3_ADD_SUB) d.alu_op = ALU_SUB;
          else                      d = illegal_op();
        end else begin
          d = illegal_op();
        end
      end

      // -------------------------------------------------------------- SYSTEM
      OP_SYSTEM: begin
        d.is_system = 1'b1;
        if (funct3 == F3_ADD_SUB) begin
          if (ir == 32'h00000073) begin
            d.is_ecall = 1'b1;
          end else if (ir == 32'h00100073) begin
            d.is_ebreak = 1'b1;
          end else if (ir == 32'h30200073) begin
            d.is_mret = 1'b1;
          end else begin
            d = illegal_op();   // sfence.vma, sret, wfi, everything else
          end
          d.valid = 1'b1;
        end else if (funct3 == F3_SLTU) begin
          d = illegal_op();     // funct3 == 100 is reserved
        end else begin
          d.valid        = 1'b1;
          d.uses_rs1     = 1'b1;
          d.csr_addr     = ir[31:20];
          d.csr_imm_form = 1'b0;
          case (funct3)
            F3_ADD_SUB: d.csr_op = CSR_RW;   // csrrw  / csrrwi
            F3_SLL:     d.csr_op = CSR_RS;   // csrrs  / csrrsi
            F3_SLT:     d.csr_op = CSR_RC;   // csrrc  / csrrci
            F3_SLTU:    begin d.csr_op = CSR_RW; d.csr_imm_form = 1'b1; end
            F3_SRL_SRA: begin d.csr_op = CSR_RS; d.csr_imm_form = 1'b1; end
            F3_XOR:     begin d.csr_op = CSR_RC; d.csr_imm_form = 1'b1; end
            default:    d = illegal_op();
          endcase
        end
      end

      default: d = illegal_op();
    endcase

    decode = d;
  endfunction

  // --------------------------------------------------------------------------
  // The CSR file.
  //
  // `csr_is_legal` is exactly the CSR list of config/csr/mode_m.json.  Any
  // number outside that list raises illegal instruction, which is a required
  // negative case rather than an optional one.  `csr_is_writable` is that file's
  // "access" column: a register whose entry has no writable field is read-only,
  // and writing it raises illegal instruction.
  // --------------------------------------------------------------------------

  function automatic logic csr_is_legal(input logic [11:0] addr);
    case (addr)
      CSR_MSTATUS, CSR_MISA, CSR_MEDELEG, CSR_MIDELEG, CSR_MIE, CSR_MTVEC,
      CSR_MCOUNTEREN, CSR_MSCRATCH, CSR_MEPC, CSR_MCAUSE, CSR_MTVAL, CSR_MIP,
      CSR_MVENDORID, CSR_MARCHID, CSR_MIMPID, CSR_MHARTID, CSR_MCYCLE,
      CSR_MINSTRET, CSR_CYCLE, CSR_TIME, CSR_INSTRET: csr_is_legal = 1'b1;
      default: csr_is_legal = 1'b0;
    endcase
  endfunction

  function automatic logic csr_is_writable(input logic [11:0] addr);
    case (addr)
      // misa: unmodifiable_bits 63:0, no writable field.
      // mcounteren, mvendorid, marchid, mimpid, mhartid, cycle, time, instret:
      // access "ro" / "fixed".
      CSR_MISA, CSR_MCOUNTEREN, CSR_MVENDORID, CSR_MARCHID, CSR_MIMPID,
      CSR_MHARTID, CSR_CYCLE, CSR_TIME, CSR_INSTRET: csr_is_writable = 1'b0;
      default: csr_is_writable = 1'b1;
    endcase
  endfunction

  function automatic logic [XLEN-1:0] csr_read(input logic [11:0] addr);
    case (addr)
      CSR_MSTATUS:    csr_read = mstatus_q | MSTATUS_MPP_M;
      CSR_MISA:       csr_read = MISA_RESET;
      CSR_MEDELEG:    csr_read = medeleg_q;
      CSR_MIDELEG:    csr_read = mideleg_q;
      CSR_MIE:        csr_read = mie_q;
      CSR_MTVEC:      csr_read = mtvec_q;
      // mcounteren has all 32 bits unmodifiable in this profile, so this build
      // reports no counter-enable capability.
      CSR_MCOUNTEREN: csr_read = {XLEN{1'b0}};
      CSR_MSCRATCH:   csr_read = mscratch_q;
      CSR_MEPC:       csr_read = mepc_q;
      CSR_MCAUSE:     csr_read = mcause_q;
      CSR_MTVAL:      csr_read = mtval_q;
      CSR_MIP:        csr_read = mip_q;
      CSR_MVENDORID:  csr_read = {XLEN{1'b0}};
      CSR_MARCHID:    csr_read = {XLEN{1'b0}};
      CSR_MIMPID:     csr_read = {XLEN{1'b0}};
      // config/profiles/p0.json -> harts = 1, and the mhartid entry says the
      // single MosaicRV hart is hart 0.
      CSR_MHARTID:    csr_read = {XLEN{1'b0}};
      CSR_MCYCLE:     csr_read = mcycle_q;
      CSR_MINSTRET:   csr_read = minstret_q;
      CSR_CYCLE:      csr_read = mcycle_q;   // read-only shadow of mcycle
      // time shadows the memory-mapped mtime register.  This core has no mtime
      // and config/csr/mode_m.json declares time fixed at reset 0, so it reads
      // as zero rather than inventing a clock the platform does not define.
      CSR_TIME:       csr_read = {XLEN{1'b0}};
      CSR_INSTRET:    csr_read = minstret_q;  // read-only shadow of minstret
      default:        csr_read = {XLEN{1'b0}};
    endcase
  endfunction


  // CSR write port, as a pure function of (address, value) rather than as a task
  // that assigns registers.  Every entry is masked to its
  // config/csr/mode_m.json writable field.  The read-only entries are absent on
  // purpose: an access that would write one raises illegal instruction and never
  // reaches here.  Keeping this combinational means every architectural register
  // has exactly one writer, which is what keeps the commit point unambiguous.
  function automatic logic [XLEN-1:0] csr_written(input logic [11:0] addr,
                                                input logic [XLEN-1:0] value);
    csr_written = {XLEN{1'b0}};
    case (addr)
      CSR_MSTATUS:   csr_written = (value & MSTATUS_WMASK) | MSTATUS_MPP_M;
      CSR_MEDELEG:   csr_written = value;
      CSR_MIDELEG:   csr_written = value;
      CSR_MIE:       csr_written = value & MIE_WMASK;
      // mtvec: BASE[1:0] is MODE and is WARL.  Only Direct mode (00) is
      // implemented, so a write of 1 (Vectored) reads back as 0 and every trap
      // vectors to BASE.  The spec also requires BASE to be 4-byte aligned,
      // which is the same condition as bits 1:0 being zero.
      CSR_MTVEC:     csr_written = {value[XLEN-1:2], 2'b00};
      CSR_MSCRATCH:  csr_written = value;
      // mepc[1:0] are always zero on an IALIGN=32 machine.
      CSR_MEPC:      csr_written = {value[XLEN-1:2], 2'b00};
      CSR_MCAUSE:    csr_written = value;
      CSR_MTVAL:     csr_written = value;
      CSR_MIP:       csr_written = value & MIE_WMASK;
      CSR_MCYCLE:    csr_written = value;
      CSR_MINSTRET:  csr_written = value;
      // misa is WARL with all 64 bits unmodifiable, so a write would be absorbed
      // without effect -- but it never arrives here at all, because writing
      // misa is illegal instruction.
      default: ;
    endcase
  endfunction

  // ==========================================================================
  // Execute / commit.  Everything below is combinational except for the
  // architectural writes made on the closing edge of S_EXEC or S_DWAIT.
  // ==========================================================================

  // Verilator reports the bits of `ctl` that no elaborated mux arm reads as
  // unused.  That is an artefact of which branch of the operand-selection mux a
  // given path takes, not a defect: the decode struct is shared with the real
  // front end and this single-issue path simply does not consume every field
  // (is_fence_i, md_signed and the immediate of a memory instruction, for
  // example).  The waiver covers this one declaration.
  /* verilator lint_off UNUSEDSIGNAL */
  decode_ctl_t      ctl;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [XLEN-1:0]  rs1_val;
  logic [XLEN-1:0]  rs2_val;
  logic [XLEN-1:0]  alu_a;
  logic [XLEN-1:0]  alu_b;
  logic [3:0]       alu_op;
  logic [XLEN-1:0]  alu_result;
  logic [XLEN-1:0]  md_result;
  logic [XLEN-1:0]  eff_addr;

  logic [XLEN-1:0]  csr_rdata;
  logic [XLEN-1:0]  csr_operand;
  logic [XLEN-1:0]  csr_wdata;
  logic             csr_reads;
  logic             csr_writes;
  logic             csr_illegal;

  logic             branch_taken;
  logic [XLEN-1:0]  next_pc;
  logic [XLEN-1:0]  rd_val;
  logic             rd_we;
  logic             commit_store;

  logic             trap_now;
  logic [XLEN-1:0]  trap_cause;
  logic [XLEN-1:0]  trap_tval;

  assign ifetch_req_o  = (state_q == S_FETCH);
  assign ifetch_addr_o = pc_q;
  assign dmem_req_o    = (state_q == S_DREQ);
  assign dmem_we_o     = (ctl.mem_kind == MEM_STORE);
  assign dmem_addr_o   = daddr_q;
  assign dmem_size_o   = dsize_q;
  assign dmem_wdata_o  = dwdata_q;
  assign dbg_pc_o      = pc_q;
  assign dbg_state_o   = state_q;

  always_comb begin
    dbg_csr_data_o = csr_read(dbg_csr_addr_i);
  end

  // x0.  The hardwired-zero rule lives in exactly two places: this read mux and
  // the commit write enable below.  Both have to be right -- a core that gets
  // only one of them wrong produces a stream that still looks plausible.
`ifdef MOSAIC_BRINGUP_MUTANT_1
  // NEGATIVE CONTROL: x0 becomes an ordinary writable register.  Reads return
  // whatever was last written to index 0 instead of zero.
  assign rs1_val = regs_q[ctl.rs1];
  assign rs2_val = regs_q[ctl.rs2];
`else
  assign rs1_val = (ctl.rs1 == 5'd0) ? {XLEN{1'b0}} : regs_q[ctl.rs1];
  assign rs2_val = (ctl.rs2 == 5'd0) ? {XLEN{1'b0}} : regs_q[ctl.rs2];
`endif

  always_comb begin
    ctl = decode(ir_q);

    // ---- ALU operand selection -------------------------------------------
    alu_a  = {XLEN{1'b0}};
    alu_b  = {XLEN{1'b0}};
    alu_op = ALU_PASSB;
    if (ctl.valid && !ctl.illegal) begin
      if (ctl.mem_kind != MEM_NONE) begin
        // Address generation is always rs1 + imm, including for a store where
        // rs2 is the value rather than a second address operand.
        alu_a  = rs1_val;
        alu_b  = ctl.imm;
        alu_op = ALU_ADD;
      end else if (ctl.is_branch) begin
        alu_a  = rs1_val;
        alu_b  = rs2_val;
        // Branch funct3 is a *branch* encoding, not the F3_* ALU encoding, so
        // the literals are written out rather than aliased to misleading names.
        case (ctl.branch_funct)
          3'b000, 3'b001:  alu_op = ALU_XOR;    // beq / bne
          3'b100, 3'b101:  alu_op = ALU_SLT;    // blt / bge
          3'b110, 3'b111:  alu_op = ALU_SLTU;   // bltu / bgeu
          default:         alu_op = ALU_PASSB;
        endcase
      end else if (ctl.is_muldiv || ctl.is_system) begin
        alu_a  = rs1_val;
        alu_b  = ctl.imm;
        alu_op = ALU_PASSB;
      end else begin
        alu_a  = ctl.is_auipc ? pc_q : rs1_val;
        alu_b  = ctl.uses_rs2 ? rs2_val : ctl.imm;
        alu_op = ctl.alu_op;
      end
    end
    alu_result = alu_eval(alu_a, alu_b, alu_op);

    eff_addr = alu_result;
    md_result = muldiv_eval(rs1_val, rs2_val, ctl.md_op);

    // ---- CSR access -------------------------------------------------------
    // The immediate forms carry a zero-extended 5-bit zimm in the rs1 field, so
    // folding csr_operand covers both encodings of each rule.
    csr_operand = ctl.csr_imm_form ? {{(XLEN - 5){1'b0}}, ir_q[19:15]} : rs1_val;
    csr_rdata   = csr_read(ctl.csr_addr);
    case (ctl.csr_op)
      CSR_RW:  csr_wdata = csr_operand;
      CSR_RS:  csr_wdata = csr_rdata | csr_operand;
      CSR_RC:  csr_wdata = csr_rdata & ~csr_operand;
      default: csr_wdata = csr_rdata;
    endcase

    // The two read/write suppression rules from the Privileged Specification:
    // csrrw with rd == x0 does not read, and csrrs/csrrc with rs1 == x0 do not
    // write.
    csr_reads = (ctl.csr_op == CSR_RS) || (ctl.csr_op == CSR_RC) ||
                ((ctl.csr_op == CSR_RW) && (ctl.rd != 5'd0));

`ifdef MOSAIC_BRINGUP_MUTANT_4
    // NEGATIVE CONTROL: csrrs/csrrc with a zero source still writes the CSR, so
    // a read-only probe such as `csrrs mscratch, x0, x0` clears the register.
    csr_writes = (ctl.csr_op == CSR_RW) ||
                 (((ctl.csr_op == CSR_RS) || (ctl.csr_op == CSR_RC)) &&
                  ((csr_operand != {XLEN{1'b0}}) ||
                   (csr_is_legal(ctl.csr_addr) && csr_is_writable(ctl.csr_addr))));
`else
    csr_writes = (ctl.csr_op == CSR_RW) ||
                 (((ctl.csr_op == CSR_RS) || (ctl.csr_op == CSR_RC)) &&
                  (csr_operand != {XLEN{1'b0}}));
`endif

    // An access to a CSR number config/csr/mode_m.json does not list is illegal
    // instruction, and so is a write to one of its read-only entries.  Only a
    // genuine CSR access can be illegal: every other instruction leaves
    // csr_addr at 0, which is not in the table.
    csr_illegal = (ctl.csr_op != CSR_NONE) &&
                  (!csr_is_legal(ctl.csr_addr) ||
                   (csr_writes && !csr_is_writable(ctl.csr_addr)));

    // ---- branch resolution ------------------------------------------------
    branch_taken = 1'b0;
    case (ctl.branch_funct)
      3'b000:   branch_taken = (alu_result == {XLEN{1'b0}});  // beq
      3'b001:   branch_taken = (alu_result != {XLEN{1'b0}});  // bne
      3'b100:   branch_taken = alu_result[0];                 // blt
      3'b101:   branch_taken = !alu_result[0];                // bge
      3'b110:   branch_taken = alu_result[0];                 // bltu
      3'b111:   branch_taken = !alu_result[0];                // bgeu
      default:  branch_taken = 1'b0;
    endcase

    // ---- architectural next PC -------------------------------------------
    next_pc = pc_q + 64'd4;
    if (ctl.is_jal) begin
      next_pc = pc_q + ctl.imm;
    end else if (ctl.is_jalr) begin
      next_pc = (rs1_val + ctl.imm) & ~64'd1;
    end else if (ctl.is_branch && branch_taken) begin
      next_pc = pc_q + ctl.imm;
    end else if (ctl.is_mret) begin
      next_pc = mepc_q;
    end

    // ---- commit values ----------------------------------------------------
    rd_val = alu_result;
    if (ctl.writes_link) begin
      rd_val = pc_q + 64'd4;
    end else if (ctl.is_muldiv) begin
      rd_val = md_result;
    end else if (ctl.is_system) begin
      rd_val = csr_reads ? csr_rdata : {XLEN{1'b0}};
    end else if (ctl.mem_kind == MEM_LOAD) begin
      rd_val = load_extend(dmem_rdata_i, ctl.mem_size, ctl.mem_signed);
    end

`ifdef MOSAIC_BRINGUP_MUTANT_1
    // NEGATIVE CONTROL: the rd == x0 qualification on the commit write enable
    // goes away with the read mux above, so the `jalr x0, ...` that every `ret`
    // compiles to stores a return address in x0.
    rd_we = ctl.reg_write | ctl.writes_link | csr_reads;
`else
    rd_we = (ctl.reg_write | ctl.writes_link | csr_reads) && (ctl.rd != 5'd0);
`endif

    commit_store = ctl.valid && !ctl.illegal && (ctl.mem_kind == MEM_STORE);

    // ---- synchronous traps, in the order the spec resolves them -----------
    trap_now   = 1'b0;
    trap_cause = {XLEN{1'b0}};
    trap_tval  = {XLEN{1'b0}};
    if (ctl.valid && !ctl.illegal) begin
      if (csr_illegal) begin
        trap_now   = 1'b1;
        trap_cause = EXC_ILLEGAL_INSN;
        trap_tval  = {{(XLEN - 32){1'b0}}, ir_q};
      end else if (ctl.is_ecall) begin
        trap_now   = 1'b1;
        trap_cause = EXC_ECALL_M;
        trap_tval  = {XLEN{1'b0}};
      end else if (ctl.is_ebreak) begin
        trap_now   = 1'b1;
        trap_cause = EXC_BREAKPOINT;
        // The spec lets the platform choose what a breakpoint writes to mtval;
        // the breakpoint's own PC is the informative choice.
        trap_tval  = pc_q;
      end else if (ctl.mem_kind == MEM_LOAD) begin
`ifdef MOSAIC_BRINGUP_MUTANT_3
        // NEGATIVE CONTROL: the load-misalignment check is skipped, so a
        // misaligned load succeeds instead of trapping with cause 4.
        trap_now = 1'b0;
`else
        if ((eff_addr & (size_bytes(ctl.mem_size) - 64'd1)) != {XLEN{1'b0}}) begin
          trap_now   = 1'b1;
          trap_cause = EXC_LOAD_MISALIGNED;
          trap_tval  = eff_addr;
        end
`endif
      end else if (ctl.mem_kind == MEM_STORE) begin
        if ((eff_addr & (size_bytes(ctl.mem_size) - 64'd1)) != {XLEN{1'b0}}) begin
          trap_now   = 1'b1;
          trap_cause = EXC_STORE_MISALIGNED;
          trap_tval  = eff_addr;
        end
      end
    end

    // A 16-bit instruction is illegal in this build: C is p1 (I-041).  mtval for
    // an illegal instruction is the instruction bits, and for a 16-bit
    // instruction those are the low half of the fetched word.
    if (!trap_now && (ir_q[1:0] != 2'b11)) begin
      trap_now   = 1'b1;
      trap_cause = EXC_ILLEGAL_INSN;
      trap_tval  = {{(XLEN - 16){1'b0}}, ir_q[15:0]};
    end else if (!trap_now && (!ctl.valid || ctl.illegal)) begin
      trap_now   = 1'b1;
      trap_cause = EXC_ILLEGAL_INSN;
      trap_tval  = {{(XLEN - 32){1'b0}}, ir_q};
    end
  end

  // ==========================================================================
  // The commit point
  //
  // One instruction finishes in exactly one of four states, and each of them
  // says so here.  Everything downstream -- the event, the CSR file, the PC --
  // is driven from these five signals, so there is one commit point rather than
  // four copies of it.
  // ==========================================================================

  logic            commit_now;      // this cycle closes an instruction
  logic            commit_is_trap;  // ... and it trapped
  logic [XLEN-1:0] commit_cause;
  logic [XLEN-1:0] commit_tval;
  logic [31:0]     commit_insn;

  always_comb begin
    commit_now     = 1'b0;
    commit_is_trap = 1'b0;
    commit_cause   = {XLEN{1'b0}};
    commit_tval    = {XLEN{1'b0}};
    commit_insn    = ir_q;
    case (state_q)
      S_FETCH: begin
        // IALIGN is 32 in this build.  This is the one trap that needs no
        // instruction, so it is taken before the fetch request is posted.
        if (pc_q[1:0] != 2'b00) begin
          commit_now     = 1'b1;
          commit_is_trap = 1'b1;
          commit_cause   = EXC_INSN_MISALIGNED;
          commit_tval    = pc_q;
        end
      end
      S_FWAIT: begin
        if (ifetch_ack_i && ifetch_fault_i) begin
          commit_now     = 1'b1;
          commit_is_trap = 1'b1;
          commit_cause   = EXC_INSN_ACCESS;
          commit_tval    = pc_q;
          commit_insn    = 32'h00000000;   // nothing was fetched
        end
      end
      S_EXEC: begin
        if (trap_now) begin
          commit_now     = 1'b1;
          commit_is_trap = 1'b1;
          commit_cause   = trap_cause;
          commit_tval    = trap_tval;
        end else if (ctl.mem_kind == MEM_NONE) begin
          commit_now     = 1'b1;
          commit_is_trap = 1'b0;
        end
      end
      S_DWAIT: begin
        if (dmem_ack_i) begin
          commit_now     = 1'b1;
          commit_is_trap = dmem_fault_i;
          commit_cause   = (ctl.mem_kind == MEM_STORE) ? EXC_STORE_ACCESS
                                                       : EXC_LOAD_ACCESS;
          commit_tval    = daddr_q;
        end
      end
      default: ;
    endcase
  end

  // Next value of every architectural register, so the sequential block below is
  // one unconditional assignment per register and there is exactly one writer
  // each.  The defaults are "unchanged", which is what lets a trap leave the
  // register file alone without an explicit inhibit.
  logic [XLEN-1:0] mstatus_n;
  logic [XLEN-1:0] medeleg_n;
  logic [XLEN-1:0] mideleg_n;
  logic [XLEN-1:0] mie_n;
  logic [XLEN-1:0] mtvec_n;
  logic [XLEN-1:0] mscratch_n;
  logic [XLEN-1:0] mepc_n;
  logic [XLEN-1:0] mcause_n;
  logic [XLEN-1:0] mtval_n;
  logic [XLEN-1:0] mip_n;
  logic [XLEN-1:0] mcycle_n;
  logic [XLEN-1:0] minstret_n;
  logic [XLEN-1:0] pc_n;
  state_e          state_n;

  // MIE <- MPIE, MPIE <- old MIE: the trap-entry swap.
  function automatic logic [XLEN-1:0] mstatus_on_trap(input logic [XLEN-1:0] s);
    mstatus_on_trap = ((s & ~MSTATUS_WMASK) |
                       (s[MSTATUS_MIE_BIT]  ? MSTATUS_MPIE : {XLEN{1'b0}}) |
                       (s[MSTATUS_MPIE_BIT] ? MSTATUS_MIE  : {XLEN{1'b0}})) |
                      MSTATUS_MPP_M;
  endfunction

  // MIE <- MPIE, MPIE <- 1: mret.  MPP is fixed at M by the CSR file.
  function automatic logic [XLEN-1:0] mstatus_on_mret(input logic [XLEN-1:0] s);
    mstatus_on_mret = ((s & ~MSTATUS_WMASK) |
                       (s[MSTATUS_MPIE_BIT] ? MSTATUS_MIE : {XLEN{1'b0}}) |
                       MSTATUS_MPIE) | MSTATUS_MPP_M;
  endfunction

  always_comb begin
    mstatus_n  = mstatus_q;
    medeleg_n  = medeleg_q;
    mideleg_n  = mideleg_q;
    mie_n      = mie_q;
    mtvec_n    = mtvec_q;
    mscratch_n = mscratch_q;
    mepc_n     = mepc_q;
    mcause_n   = mcause_q;
    mtval_n    = mtval_q;
    mip_n      = mip_q;
    mcycle_n   = mcycle_q + 64'd1;      // one increment per clock, not per event
    minstret_n = minstret_q;
    pc_n       = next_pc;
    state_n    = S_FETCH;

    case (state_q)
      S_FETCH:  if (pc_q[1:0] == 2'b00) state_n = S_FWAIT;
      S_FWAIT:  if (ifetch_ack_i && !ifetch_fault_i) state_n = S_EXEC;
      S_EXEC:   if (!trap_now && (ctl.mem_kind != MEM_NONE)) state_n = S_DREQ;
      S_DREQ:   state_n = S_DWAIT;
      S_DWAIT:  if (dmem_ack_i && !dmem_fault_i) state_n = S_FETCH;
      default:  state_n = S_FETCH;
    endcase

    if (commit_now) begin
      if (commit_is_trap) begin
        mepc_n    = pc_q;
        mcause_n  = commit_cause;
        mtval_n   = commit_tval;
        mstatus_n = mstatus_on_trap(mstatus_q);
        pc_n      = mtvec_q;
        state_n   = S_FETCH;
      end else begin
        minstret_n = minstret_q + 64'd1;
        pc_n       = next_pc;
        state_n    = S_FETCH;
        if (csr_writes) begin
          case (ctl.csr_addr)
            CSR_MSTATUS:   mstatus_n  = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MEDELEG:   medeleg_n  = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MIDELEG:   mideleg_n  = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MIE:       mie_n      = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MTVEC:     mtvec_n    = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MSCRATCH:  mscratch_n = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MEPC:      mepc_n     = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MCAUSE:    mcause_n   = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MTVAL:     mtval_n    = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MIP:       mip_n      = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MCYCLE:    mcycle_n   = csr_written(ctl.csr_addr, csr_wdata);
            CSR_MINSTRET:  minstret_n = csr_written(ctl.csr_addr, csr_wdata);
            default: ;
          endcase
        end
        if (ctl.is_mret) mstatus_n = mstatus_on_mret(mstatus_q);
      end
    end
  end

  // ==========================================================================
  // Sequential.  One writer per register, one commit point, no tasks.
  // ==========================================================================

  integer ri;
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      state_q    <= S_FETCH;
      pc_q       <= reset_vector_i;
      ir_q       <= 32'h00000000;
      mstatus_q  <= MSTATUS_RESET;
      medeleg_q  <= {XLEN{1'b0}};
      mideleg_q  <= {XLEN{1'b0}};
      mie_q      <= {XLEN{1'b0}};
      mtvec_q    <= {XLEN{1'b0}};
      mscratch_q <= {XLEN{1'b0}};
      mepc_q     <= {XLEN{1'b0}};
      mcause_q   <= {XLEN{1'b0}};
      mtval_q    <= {XLEN{1'b0}};
      mip_q      <= {XLEN{1'b0}};
      mcycle_q   <= {XLEN{1'b0}};
      minstret_q <= {XLEN{1'b0}};
      daddr_q    <= {XLEN{1'b0}};
      dwdata_q   <= {XLEN{1'b0}};
      dsize_q    <= SZ_DBL;
      evt_valid_o      <= 1'b0;
      evt_trap_o       <= 1'b0;
      evt_pc_o         <= {XLEN{1'b0}};
      evt_next_pc_o    <= {XLEN{1'b0}};
      evt_insn_o       <= 32'h00000000;
      evt_has_rd_o     <= 1'b0;
      evt_rd_o         <= 5'd0;
      evt_rd_value_o   <= {XLEN{1'b0}};
      evt_cause_o      <= {XLEN{1'b0}};
      evt_tval_o       <= {XLEN{1'b0}};
      evt_epc_o        <= {XLEN{1'b0}};
      evt_is_store_o   <= 1'b0;
      evt_store_addr_o <= {XLEN{1'b0}};
      evt_store_data_o <= {XLEN{1'b0}};
      evt_store_size_o <= 3'b000;
      for (ri = 0; ri < 32; ri = ri + 1) begin
        regs_q[ri] <= {XLEN{1'b0}};
      end
    end else begin
      // At most one architectural event per cycle, because one instruction is in
      // flight; the default clears the pulse.
      evt_valid_o      <= 1'b0;
      evt_trap_o       <= 1'b0;
      evt_cause_o      <= {XLEN{1'b0}};
      evt_tval_o       <= {XLEN{1'b0}};
      evt_epc_o        <= {XLEN{1'b0}};
      evt_is_store_o   <= 1'b0;
      evt_store_addr_o <= {XLEN{1'b0}};
      evt_store_data_o <= {XLEN{1'b0}};
      evt_store_size_o <= 3'b000;

      state_q    <= state_n;
      pc_q       <= pc_n;
      mstatus_q  <= mstatus_n;
      medeleg_q  <= medeleg_n;
      mideleg_q  <= mideleg_n;
      mie_q      <= mie_n;
      mtvec_q    <= mtvec_n;
      mscratch_q <= mscratch_n;
      mepc_q     <= mepc_n;
      mcause_q   <= mcause_n;
      mtval_q    <= mtval_n;
      mip_q      <= mip_n;
      mcycle_q   <= mcycle_n;
      minstret_q <= minstret_n;

      if ((state_q == S_FWAIT) && ifetch_ack_i && !ifetch_fault_i) begin
        ir_q <= ifetch_rdata_i;
      end

      if ((state_q == S_EXEC) && !trap_now && (ctl.mem_kind != MEM_NONE)) begin
        daddr_q  <= eff_addr;
        dsize_q  <= ctl.mem_size;
        dwdata_q <= (ctl.mem_kind == MEM_STORE)
                      ? (rs2_val & size_mask(ctl.mem_size))
                      : {XLEN{1'b0}};
      end

      // ---- the commit point ------------------------------------------------
      if (commit_now) begin
        evt_valid_o  <= 1'b1;
        evt_pc_o     <= pc_q;
        evt_insn_o   <= commit_insn;
        evt_rd_o     <= ctl.rd;

`ifdef MOSAIC_BRINGUP_MUTANT_5
        // NEGATIVE CONTROL: report the trapping instruction as an ordinary
        // retire, with a register write and a fall-through PC.  The machine
        // still redirects correctly, so the program still finishes and every
        // signature still matches -- only the event stream is wrong, which is
        // exactly the ABA/aliasing hazard section 1.3 names.
        evt_trap_o     <= 1'b0;
        evt_next_pc_o  <= pc_q + 64'd4;
        evt_has_rd_o   <= 1'b1;
        evt_rd_value_o <= rd_val;
        if (!commit_is_trap && rd_we) regs_q[ctl.rd] <= rd_val;
`else
        if (commit_is_trap) begin
          // A trap is never dressed up as a retire: no destination register, no
          // store, and the next PC is the trap vector.
          evt_trap_o     <= 1'b1;
          evt_next_pc_o  <= mtvec_q;
          evt_has_rd_o   <= 1'b0;
          evt_rd_value_o <= {XLEN{1'b0}};
          evt_cause_o    <= commit_cause;
          evt_tval_o     <= commit_tval;
          evt_epc_o      <= pc_q;
        end else begin
          evt_trap_o     <= 1'b0;
          evt_next_pc_o  <= next_pc;
          evt_has_rd_o   <= rd_we;
          evt_rd_value_o <= rd_val;
          if (rd_we) regs_q[ctl.rd] <= rd_val;
          if (commit_store) begin
            evt_is_store_o   <= 1'b1;
            evt_store_addr_o <= daddr_q;
            evt_store_data_o <= dwdata_q;
            evt_store_size_o <= dsize_q;
          end
        end
`endif
      end
    end
  end


endmodule : mosaic_bringup_core

`default_nettype wire
