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
  localparam logic [6:0] OP_SYSTEM    = 7'b1110011;  // ecall ebreak mret csr*

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
    MEM_STORE = 3'b010
  } mem_kind_e;

  localparam logic [2:0] SZ_BYTE = 3'd0;
  localparam logic [2:0] SZ_HALF = 3'd1;
  localparam logic [2:0] SZ_WORD = 3'd2;
  localparam logic [2:0] SZ_DBL  = 3'd3;

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

    logic        is_system;
    logic        is_ecall;
    logic        is_ebreak;
    logic        is_mret;
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

endpackage : mosaic_pkg

`endif  // MOSAIC_PKG_SV_
