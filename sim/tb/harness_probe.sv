// Test double for the simulation harness. **This is not the processor core.**
//
// It exists so I-004 (harness, loader, memory model, event tap, cycle limit,
// exit path) can be proven end to end before I-008 provides the real scalar
// bring-up path. It implements only what the harness self-test needs: LUI, ADDI,
// SD and a self-loop JAL, driven one instruction per step over a stepping
// interface that the real core wrapper will implement later.
//
// It is deliberately small enough to audit by eye. Nothing it does is ever
// reported as architectural evidence about the core.

`default_nettype none

module harness_probe (
    input  wire         clk,
    input  wire         rst,

    // Stepping interface: the harness presents one instruction and the probe
    // answers with at most one retirement.
    input  wire         step_valid,
    output wire         step_ready,
    input  wire  [63:0] pc_in,
    input  wire  [31:0] insn_in,

    // Single-beat store port, asserted only for a store instruction. The harness
    // performs the write and reports the outcome, so memory faults are modelled
    // in exactly one place.
    output wire         mem_en,
    output wire  [63:0] mem_addr,
    output wire  [63:0] mem_wdata,
    output wire  [7:0]  mem_byte_en,

    output wire         retire,
    output wire  [63:0] next_pc,
    output wire         illegal,

    // Architectural register write, so the harness can record the destination
    // and its value in the event stream without re-decoding the instruction.
    output wire         rd_we,
    output wire  [4:0]  rd_index,
    output wire  [63:0] rd_value_out
);

  localparam logic [6:0] OP_IMM   = 7'b0010011;  // addi
  localparam logic [6:0] OP_LUI   = 7'b0110111;  // lui
  localparam logic [6:0] OP_STORE = 7'b0100011;  // sd
  localparam logic [6:0] OP_JAL   = 7'b1101111;  // jal

  wire [6:0] opcode = insn_in[6:0];
  wire [4:0] rd     = insn_in[11:7];
  wire [2:0] funct3 = insn_in[14:12];
  wire [4:0] rs1    = insn_in[19:15];
  wire [4:0] rs2    = insn_in[24:20];

  // Immediate format depends on the opcode: I-type puts the whole 12 bits in
  // [31:20], but S-type splits them as [31:25] and [11:7]. Decoding a store with
  // the I-type layout silently folds funct3 and rs2 into the address.
  wire signed [11:0] imm_i = insn_in[31:20];
  wire               is_store_op = (opcode == OP_STORE);
  wire signed [11:0] imm_sel = is_store_op ? {insn_in[31:25], insn_in[11:7]} : imm_i;
  wire signed [63:0] imm_x = {{52{imm_sel[11]}}, imm_sel};

  logic [63:0] xregs [0:31];
  logic [63:0] next_value;
  logic        does_store;
  logic        trap_illegal;
  logic        does_write_rd;

  // x0 is hardwired to zero; every write to it is discarded.
  logic write_rd;

  always_comb begin
    next_value    = 64'd0;
    does_store    = 1'b0;
    trap_illegal  = 1'b0;
    does_write_rd = 1'b0;

    unique case (opcode)
      OP_LUI: begin
        // LUI places imm[31:12] at bits [31:12] and zeroes everything above.
        next_value    = {{32{1'b0}}, insn_in[31:12], 12'd0};
        does_write_rd = 1'b1;
      end
      OP_IMM: begin
        if (funct3 == 3'b000) begin
          next_value    = xregs[rs1] + imm_x;
          does_write_rd = 1'b1;
        end else begin
          trap_illegal = 1'b1;
        end
      end
      // Only full-width doubleword stores are modelled: sb/sh/sw have narrower
      // funct3 encodings and a byte-enable pattern the harness would have to
      // carry, so they are reported illegal rather than silently mis-stored.
      OP_STORE: begin
        if (funct3 == 3'b011) begin
          does_store = 1'b1;
        end else begin
          trap_illegal = 1'b1;
        end
      end
      OP_JAL: begin
        next_value    = pc_in + 64'd4;
        does_write_rd = 1'b1;
      end
      default: trap_illegal = 1'b1;
    endcase

    write_rd = does_write_rd && (rd != 5'd0);
  end

  // The probe has no branches and no memory reads: every supported instruction
  // falls through, and an unsupported one retires as illegal without writing a
  // register or issuing a store.
  assign next_pc    = pc_in + 64'd4;
  assign retire     = step_valid;
  assign illegal    = trap_illegal;
  assign step_ready = 1'b1;

  assign rd_we        = step_valid && write_rd && !trap_illegal;
  assign rd_index     = rd;
  assign rd_value_out = next_value;

  assign mem_en      = step_valid && does_store;
  assign mem_addr    = xregs[rs1] + imm_x;
  // A store takes its data from rs2; rd is not a destination for a store.
  assign mem_wdata   = does_store ? xregs[rs2] : ((rd == 5'd0) ? 64'd0 : next_value);
  assign mem_byte_en = 8'hff;

  always_ff @(posedge clk) begin
    if (rst) begin
      integer i;
      for (i = 0; i < 32; i = i + 1) begin
        xregs[i] <= 64'd0;
      end
    end else if (step_valid && write_rd) begin
      xregs[rd] <= next_value;
    end
  end

  // Memory-fault handling deliberately lives in the harness, not here: the probe
  // reports the store, the harness performs it against the PMA-aware memory model
  // and records the fault in the event stream. Duplicating the memory model inside
  // the DUT would make the two disagree about what counts as a fault.

endmodule

`default_nettype wire
