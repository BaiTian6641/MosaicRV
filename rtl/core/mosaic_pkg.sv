// ============================================================================
// mosaic_pkg -- shared encodings and control types for the scalar core.
//
// Everything here is either a literal from the RISC-V ISA manual or a control
// encoding defined once so the decoder, the functional units and the testbenches
// cannot disagree about what a signal means. There is deliberately no behaviour
// in this file: it is types and constants, not logic.
// ============================================================================

`ifndef MOSAIC_PKG_SV_
`define MOSAIC_PKG_SV_

package mosaic_pkg;
  /* verilator lint_off UNUSEDPARAM */

  // ------------------------------------------------------------- opcode map
  // RV64IM only in p0. Compressed instructions are decoded by I-041 in p1 and
  // will arrive as a separate front end; nothing here pretends to know them.
  localparam logic [6:0] OP_LOAD      = 7'b0000011;  // lb lh lw ld
  localparam logic [6:0] OP_MISC_MEM  = 7'b0001111;  // fence, fence.i
  localparam logic [6:0] OP_IMM       = 7'b0010011;  // addi slti sltiu xori ori andi
  localparam logic [6:0] OP_AUIPC     = 7'b0010111;
  localparam logic [6:0] OP_STORE     = 7'b0100011;  // sb sh sw sd
  localparam logic [6:0] OP_IMM_32    = 7'b0011011;  // addiw slliw srliw sraiw
  localparam logic [6:0] OP_BRANCH    = 7'b1100011;  // beq bne blt bge bltu bgeu
  localparam logic [6:0] OP_JALR      = 7'b1100111;
  localparam logic [6:0] OP_JAL       = 7'b1101111;
  localparam logic [6:0] OP_MUL_DIV   = 7'b0110011;  // mul div rem + *u variants
  // Opcode 0111011 is OP-32, the 32-bit-result arithmetic forms. RV64I defines
  // addw subw sllw srlw sraw on it (funct7 0000000 / 0100000) and RV64M adds
  // mulw divw divuw remw remuw (funct7 0000001). They are RV64 instructions:
  // RV32I has no OP-32 at all, and the W forms have no RV32 meaning.
  localparam logic [6:0] OP_32        = 7'b0111011;
  localparam logic [6:0] OP_SYSTEM    = 7'b1110011;  // ecall ebreak mret csr*
  // OP_AMO is the A extension's atomic-memory opcode. It is *not* decoded by
  // mosaic_decoder: CASE=decode.rv64im_reserved pins every opcode outside
  // RV64IM as illegal, and that enumeration belongs to the decoder's owner.
  // The integration recognises it from the raw word in mosaic_core.sv, the same
  // division of ownership WFI uses (see mosaic_core.sv section 2a). It is named
  // here because both the core's front end and the testbench's reference need
  // the same literal.
  localparam logic [6:0] OP_AMO       = 7'b0101111;  // amoadd..amomaxu, lr, sc

  // funct3
  localparam logic [2:0] F3_ADD_SUB  = 3'b000;
  localparam logic [2:0] F3_SLL      = 3'b001;
  localparam logic [2:0] F3_SLT      = 3'b010;
  localparam logic [2:0] F3_SLTU     = 3'b011;
  localparam logic [2:0] F3_XOR      = 3'b100;
  localparam logic [2:0] F3_SRL_SRA  = 3'b101;
  localparam logic [2:0] F3_OR       = 3'b110;
  localparam logic [2:0] F3_AND      = 3'b111;

  // ---------------------------------------------------------------- ALU ops
  // Sixteen operations, which is exactly what a 4-bit field holds. The word
  // forms sign-extend their 32-bit result, as RV64 requires.
  typedef enum logic [3:0] {
    ALU_ADD   = 4'd0,
    ALU_SUB   = 4'd1,
    ALU_SLL   = 4'd2,
    ALU_SLT   = 4'd3,
    ALU_SLTU  = 4'd4,
    ALU_XOR   = 4'd5,
    ALU_SRL   = 4'd6,
    ALU_SRA   = 4'd7,
    ALU_OR    = 4'd8,
    ALU_AND   = 4'd9,
    ALU_ADDW  = 4'd10,
    ALU_SUBW  = 4'd11,
    ALU_SLLW  = 4'd12,
    ALU_SRLW  = 4'd13,
    ALU_SRAW  = 4'd14,
    ALU_PASSB = 4'd15
  } alu_op_e;

  // ------------------------------------------------------------- M extension
  typedef enum logic [2:0] {
    MD_MUL    = 3'b000,
    MD_MULH   = 3'b001,
    MD_MULHSU = 3'b010,
    MD_MULHU  = 3'b011,
    MD_DIV    = 3'b100,
    MD_DIVU   = 3'b101,
    MD_REM    = 3'b110,
    MD_REMU   = 3'b111
  } md_op_e;

  // ------------------------------------------------------------------ memory
  typedef enum logic [2:0] {
    MEM_NONE  = 3'b000,
    MEM_LOAD  = 3'b001,
    MEM_STORE = 3'b010,
    // An atomic read-modify-write (A extension, work package I-039). It is a
    // third class and not a load or a store because its effect is neither: the
    // old value is read *and* the new one written as one indivisible step at a
    // shared serialization point. A consumer that treated it as either one
    // would either write memory speculatively or return a stale value.
    MEM_AMO   = 3'b011,
    // LR/SC (work package I-040). They are their own access classes because
    // neither is a plain load or store: an LR returns a value *and* establishes
    // a reservation, and an SC conditionally performs exactly one write and
    // returns a status (0 = succeeded, 1 = failed) instead of a memory value.
    // Both are issued through the load queue -- they return a value to `rd`
    // before retirement -- and both take the atomic serialization path, because
    // an LR must not establish a reservation on a path that can be squashed and
    // an SC must not perform its write speculatively.
    MEM_LR    = 3'b100,
    MEM_SC    = 3'b101
  } mem_kind_e;

  localparam logic [2:0] SZ_BYTE = 3'd0;
  localparam logic [2:0] SZ_HALF = 3'd1;
  localparam logic [2:0] SZ_WORD = 3'd2;
  localparam logic [2:0] SZ_DBL  = 3'd3;

  // --------------------------------------------------------- A extension ops
  // The nine AMO operations, in the order the encoding's funct5 field names
  // them. `AMO_MIN`/`AMO_MAX` are the *signed* comparisons and `AMO_MINU`/
  // `AMO_MAXU` the unsigned ones -- the boundary the card calls out, and the
  // reason signedness is a property of the operation and not of the access.
  // LR and SC share the opcode (funct5 00010/00011) but are I-040's; they are
  // not named here because this package enumerates only what I-039 builds.
  typedef enum logic [3:0] {
    AMO_ADD  = 4'd0,
    AMO_SWAP = 4'd1,
    AMO_XOR  = 4'd2,
    AMO_AND  = 4'd3,
    AMO_OR   = 4'd4,
    AMO_MIN  = 4'd5,
    AMO_MAX  = 4'd6,
    AMO_MINU = 4'd7,
    AMO_MAXU = 4'd8
  } amo_op_e;

  // ------------------------------------------------------------- F/D ops (I-049)
  // The F and D extension operation list, added additively by work package
  // I-049 so that `mosaic_fpu` and its testbench name an operation once instead
  // of twice. The decoder does *not* decode OP-FP yet: `mosaic_decoder` still
  // rejects every encoding outside RV64IM (that enumeration belongs to the
  // decoder's owner, and CASE=decode.rv64im_reserved asserts it). This is the
  // operation list the FPU implements, and the list the capability matrix in
  // results/reports/I-049-fpu.md reports as implemented or declared-absent.
  //
  // The list is deliberately operation-shaped rather than encoding-shaped: an
  // FPU does not care whether fadd.s came from OP-FP funct7 0000000 or from a
  // future compressed form, and `mosaic_fpu` is handed the format as a side
  // band (`req_fmt_i`) rather than re-deriving it from the instruction word.
  //
  // FP_SQRT is enumerated because the F/D *instruction* list contains fsqrt.s/d
  // and a capability matrix has to name every operation in that list even when
  // the answer is "declared not implemented". It is declared absent, not faked:
  // see the header of rtl/core/mosaic_fpu.sv and the report.
  typedef enum logic [4:0] {
    FP_ADD     = 5'd0,   // fadd.s / fadd.d
    FP_SUB     = 5'd1,   // fsub.s / fsub.d
    FP_MUL     = 5'd2,   // fmul.s / fmul.d
    FP_DIV     = 5'd3,   // fdiv.s / fdiv.d
    FP_SQRT    = 5'd4,   // fsqrt.s / fsqrt.d -- DECLARED NOT IMPLEMENTED
    FP_SGNJ    = 5'd5,   // fsgnj.s / fsgnj.d
    FP_SGNJN   = 5'd6,   // fsgnjn.s / fsgnjn.d
    FP_SGNJX   = 5'd7,   // fsgnjx.s / fsgnjx.d
    FP_MIN     = 5'd8,   // fmin.s / fmin.d
    FP_MAX     = 5'd9,   // fmax.s / fmax.d
    FP_CMP_EQ  = 5'd10,  // feq.s / feq.d
    FP_CMP_LT  = 5'd11,  // flt.s / flt.d
    FP_CMP_LE  = 5'd12,  // fle.s / fle.d
    FP_CLASS   = 5'd13,  // fclass.s / fclass.d
    FP_MV_X    = 5'd14,  // fmv.x.w / fmv.x.d   (FP -> integer register)
    FP_MV_W    = 5'd15,  // fmv.w.x / fmv.d.x   (integer register -> FP)
    FP_CVT_FI  = 5'd16,  // fcvt.w[u].s / fcvt.l[u].s / .d  (FP -> integer)
    FP_CVT_IF  = 5'd17,  // fcvt.s.w[u] / fcvt.s.l[u] / .d  (integer -> FP)
    FP_CVT_FS  = 5'd18,  // fcvt.s.d             (double -> single, rounds)
    FP_CVT_SF  = 5'd19   // fcvt.d.s             (single -> double, exact)
  } fp_op_e;

  // The five rounding modes, in the encoding the instruction's rm field uses.
  // 101/110 are reserved and 111 is the dynamic mode; resolving 111 from
  // frm/fcsr is work package I-050's, so this enumeration names only the five
  // modes `mosaic_fpu` implements natively.
  typedef enum logic [2:0] {
    FP_RM_RNE = 3'b000,  // round to nearest, ties to even (IEEE default)
    FP_RM_RTZ = 3'b001,  // round toward zero
    FP_RM_RDN = 3'b010,  // round toward -infinity
    FP_RM_RUP = 3'b011,  // round toward +infinity
    FP_RM_RMM = 3'b100   // round to nearest, ties to maximum magnitude
  } fp_rm_e;

  // ------------------------------------------------------------------ system
  typedef enum logic [1:0] {
    CSR_NONE = 2'b00,
    CSR_RW   = 2'b01,  // csrrw  / csrrwi
    CSR_RS   = 2'b10,  // csrrs  / csrrsi
    CSR_RC   = 2'b11   // csrrc  / csrrci
  } csr_op_e;

  // ------------------------------------------------------------- exceptions
  // RISC-V privileged specification v1.12, Table "Environment Call and
  // Breakpoint": instruction address misaligned 0, instruction access fault 1,
  // illegal instruction 2, breakpoint 3, load address misaligned 4, load access
  // fault 5, store/AMO address misaligned 6, store/AMO access fault 7, ecall
  // from U 8, ecall from S 9, ecall from M 11.
  localparam logic [63:0] EXC_INSN_MISALIGNED = 64'd0;
  localparam logic [63:0] EXC_INSN_ACCESS     = 64'd1;
  localparam logic [63:0] EXC_ILLEGAL_INSN    = 64'd2;
  localparam logic [63:0] EXC_BREAKPOINT      = 64'd3;
  localparam logic [63:0] EXC_LOAD_MISALIGNED = 64'd4;
  localparam logic [63:0] EXC_LOAD_ACCESS     = 64'd5;
  localparam logic [63:0] EXC_STORE_MISALIGNED = 64'd6;
  localparam logic [63:0] EXC_STORE_ACCESS    = 64'd7;
  localparam logic [63:0] EXC_ECALL_U         = 64'd8;
  localparam logic [63:0] EXC_ECALL_S         = 64'd9;
  localparam logic [63:0] EXC_ECALL_M         = 64'd11;

  // ------------------------------------------------------------- decode ctrl
  // One struct, packed, so the decoder output can cross a module boundary as a
  // single vector and so a missing control signal is visible as a missing bit
  // rather than as a silent default.
  typedef struct packed {
    logic        valid;         // instruction decodes to something legal
    logic        illegal;

    logic        uses_rs1;
    logic        uses_rs2;
    logic        uses_imm;
    logic [4:0]  rs1;
    logic [4:0]  rs2;
    logic [4:0]  rd;
    logic [63:0] imm;

    alu_op_e     alu_op;
    logic        uses_alu;
    logic        reg_write;     // writes rd on commit

    mem_kind_e   mem_kind;
    logic [2:0]  mem_size;
    logic        mem_signed;
    // A extension (MEM_AMO only). `amo_op` names the read-modify-write; `aq` and
    // `rl` are the acquire/release ordering bits, carried through to the memory
    // transaction so the serialization point can honour them rather than
    // treating them as a performance hint (the card's named Fail mode).
    amo_op_e     amo_op;
    logic        amo_aq;
    logic        amo_rl;
    // I-040. `mem_kind` is MEM_LR/MEM_SC for the load-reserved and
    // store-conditional forms; these two bits are the same fact stated where a
    // consumer that only cares about "is this one of the paired atomics" can
    // read it without a second comparison of the kind enum.
    logic        is_lr;
    logic        is_sc;

    logic        is_branch;
    logic [2:0]  branch_funct;
    logic        is_jal;
    logic        is_jalr;
    logic        is_auipc;
    logic        writes_link;    // JAL/JALR write rd = PC + 4

    logic        is_miscmem;     // fence / fence.i
    logic        is_fence_i;

    logic        is_muldiv;
    md_op_e      md_op;
    logic        md_signed;
    logic        md_w;          // 1 = 32-bit W form (OP-32, funct7 0000001):
                                // operands are the low 32 bits, result is
                                // sign-extended to 64 bits

    logic        is_system;
    logic        is_ecall;
    logic        is_ebreak;
    logic        is_mret;
    // SRET (I-044): the supervisor return, funct12 0x102. Decoded by the decoder
    // exactly as MRET is, because unlike WFI it is a real instruction whose
    // encoding the reserved-value enumeration must account for.
    logic        is_sret;
    // A fetch the physical memory protection unit refused (I-044). The front end
    // could not read the instruction, so there is nothing to decode: the macro
    // is a system instruction whose whole architectural effect is an instruction
    // access fault at its own PC, which is what makes the fault precise instead
    // of a machine that stops.
    logic        is_fetch_fault;
    // WFI is decoded by the *core's* front end rather than by mosaic_decoder:
    // the decoder's case (CASE=decode.rv64im_reserved) pins every funct3-000
    // imm12 other than 000/001/302 as reserved, and that enumeration belongs to
    // the decoder's owner. So the core recognises the one encoding the
    // integration needs, states it here, and leaves the decoder's illegal set
    // exactly as its case asserts it. See mosaic_core.sv section 2a.
    logic        is_wfi;
    csr_op_e     csr_op;
    logic [11:0] csr_addr;
    logic        csr_writes;
    logic        csr_reads;
    logic        csr_imm_form;
  } decode_ctl_t;

  // ----------------------------------------------------------- write events
  // What actually happened to an architectural register, so the retire event
  // carries the destination and its value instead of leaving the testbench to
  // re-decode the instruction.
  typedef struct packed {
    logic        we;
    logic [4:0]  rd;
    logic [63:0] value;
  } reg_write_t;

  /* verilator lint_on UNUSEDPARAM */
endpackage : mosaic_pkg

`endif  // MOSAIC_PKG_SV_
