// ============================================================================
// mosaic_bringup_tb -- work package I-008.  TESTBENCH, NOT RTL.
//
// This wrapper exists for one reason: it makes the I-008 package
// self-contained.  It owns a memory model, so the bring-up core can be built,
// run and compared without any other part of the project's simulation
// infrastructure, and so that "the memory interface" has exactly one
// implementation whose behaviour can be read in one place.
//
// It is obviously a testbench:
//   * every signal name says who owns it -- `c_*` is the core's, `m_*` is this
//     file's memory-model state, `h_*` is driven by the C++ harness;
//   * the memory arrays are plain RAM loaded through the `h_img_*` port;
//   * nothing here is synthesizable and nothing here claims to be.
//
// WHO OWNS WHAT
// -------------
//   c_*  the DUT.  This wrapper never drives or overrides them; it only wires
//        them and exposes them to the harness.
//   m_*  this file's memory model.  Plain RAM arrays plus the platform devices.
//   h_*  the C++ harness (tb_bringup.cpp).  Image loading, readback and the
//        debug CSR read address all arrive this way.
//
// MEMORY MAP  (config/memory/p0.json, profile p0, transcribed)
// ------------------------------------------------------------
//   boot_rom      0x00000000  4 KiB   r-x, NOT writable
//   uart          0x00100000  256 B   rw-
//   test_harness  0x00102000  16 B    rw-   TOHOST at +0, FROMHOST at +8
//   clint         0x02000000  4 KiB   rw-
//   ram           0x80000000  2 MiB   rwx
//   anything else                     -> access fault
//
// Deliberate behaviours, each one required by a frozen input:
//   * A store to boot_rom asserts the fault even though the region is mapped and
//     readable, because config/memory/p0.json gives it `"writable": false` with
//     `"error_response": "access_fault"`.  The write is not performed.
//   * Misalignment is *not* policed here.  config/profiles/p0.json assigns the
//     misalignment policy (trap on fetch, load, store and atomic) to the hart,
//     and the core's memory-interface contract says the core performs every
//     misalignment check itself and never issues a misaligned request.  So this
//     model assembles an unaligned access byte by byte from whatever the request
//     covers and stays out of the hart's way.  That is what makes the
//     misalignment negative control meaningful: if the hart stops checking, the
//     access silently succeeds instead of becoming a differently numbered trap.
//   * An access that straddles a region boundary faults, so an 8-byte access at
//     the last word of a small MMIO region cannot silently bleed into a
//     neighbour.
//   * TOHOST is an ordinary RAM word with a watch on it.  It reads as 0 until
//     the program writes it, which is what tests/programs/src/crt0.S documents
//     ("reads 0 while the program runs") and what makes its boot-time sample
//     well defined.  A non-zero write ends the run and is reported to the
//     harness; a zero write is an ordinary store.
//   * CLINT is a plain readable/writable region here.  config/csr/mode_m.json
//     declares the `time` CSR fixed at reset 0 and p0 has no timer interrupt
//     logic, so there is no mtime behaviour to model; inventing one would create
//     a second source of truth about time.
//
// LATENCY
// -------
// Both ports answer on the cycle after the request, so a request asserted in
// cycle N is acked in cycle N+1.  That is the only timing behaviour here and it
// is what the core's interface contract specifies.  There is no queueing: the
// model asserts ack for exactly one cycle and ignores a request that arrives
// while a previous one is outstanding, which cannot happen because the core holds
// state until it sees the ack.
//
// IMAGE LOADING
// -------------
// The harness loads the ELF itself (sim/common/elf_loader.h validates it and the
// harness refuses a non-kOk load) and pushes the bytes in here as whole,
// 8-byte-aligned words through `h_img_we`.  The loader is deliberately
// word-granular: the harness builds each word from its own shadow after a
// zero-fill pass, so this file needs no byte-merge logic and every untouched word
// is a defined zero.
// ============================================================================

// The shared package is *included* here rather than only listed as a source
// file.  The elaborator reads sources in command-line order, and
// tools/run_unit.py puts the testbench sources before the RTL sources, so a
// package that is merely listed would be read after the module that uses it and
// every package type would look like an undeclared identifier.  The package has
// an include guard, so listing it separately as well costs nothing.
`include "mosaic_pkg.sv"

`default_nettype none

module mosaic_bringup_tb (
    // ------------------------------------------------------- harness -> model
    input  wire         h_clk,
    input  wire         h_rst,
    input  wire         h_img_we,       // write h_img_data to h_img_addr
    input  wire  [63:0] h_img_addr,     // 8-byte aligned
    input  wire  [63:0] h_img_data,
    input  wire         h_rb_req,       // read one word back through h_rb_addr
    input  wire  [63:0] h_rb_addr,
    output logic [63:0] h_rb_data,
    output logic        h_rb_fault,
    output logic        h_rb_valid,
    input  wire  [11:0] h_dbg_csr_addr,
    input  wire         h_clear_mem,     // one-cycle pulse: zero every array

    // -------------------------------------------- observation (DUT + devices)
    output wire         c_evt_valid,
    output wire         c_evt_trap,
    output wire  [63:0] c_evt_pc,
    output wire  [63:0] c_evt_next_pc,
    output wire  [31:0] c_evt_insn,
    output wire         c_evt_has_rd,
    output wire  [4:0]  c_evt_rd,
    output wire  [63:0] c_evt_rd_value,
    output wire  [63:0] c_evt_cause,
    output wire  [63:0] c_evt_tval,
    output wire  [63:0] c_evt_epc,
    output wire         c_evt_is_store,
    output wire  [63:0] c_evt_store_addr,
    output wire  [63:0] c_evt_store_data,
    output wire  [3:0]  c_evt_store_size,
    output wire  [63:0] c_dbg_pc,
    output wire  [2:0]  c_dbg_state,
    output wire  [63:0] c_dbg_csr_data,
    output wire  [63:0] h_tohost_value,
    output wire         h_tohost_written,
    output wire  [63:0] h_uart_count,
    // The memory model's outstanding-request ownership.  A harness cannot see a
    // transaction the environment started and did not finish any other way, and
    // "no transaction may cross a reset" is a property of the environment as
    // much as of the core.  Exposed as observation only; nothing here drives it.
    output wire         h_if_pending,
    output wire         h_d_pending,
    // The core's data-port request, exposed so a harness can assert on it
    // without a hierarchical reference.  V-009 reads these to check that the
    // machine posts no data request while reset is asserted, which is half of
    // "no device write during reset".
    output wire         c_dmem_req_o,
    output wire         c_dmem_we_o,
    output wire  [63:0] c_dmem_addr_o
);

  // --------------------------------------------------------------------------
  // Frozen map, transcribed from config/memory/p0.json.
  // --------------------------------------------------------------------------
  localparam logic [63:0] ROM_BASE     = 64'h0000_0000_0000_0000;
  localparam logic [63:0] ROM_SIZE     = 64'h0000_0000_0000_1000;
  localparam logic [63:0] UART_BASE    = 64'h0000_0000_0010_0000;
  localparam logic [63:0] UART_SIZE    = 64'h0000_0000_0000_0100;
  localparam logic [63:0] HARN_BASE    = 64'h0000_0000_0010_2000;
  localparam logic [63:0] HARN_SIZE    = 64'h0000_0000_0000_0010;
  localparam logic [63:0] CLINT_BASE   = 64'h0000_0000_2000_0000;
  localparam logic [63:0] CLINT_SIZE   = 64'h0000_0000_0000_1000;
  localparam logic [63:0] RAM_BASE     = 64'h0000_0000_8000_0000;
  localparam logic [63:0] RAM_SIZE     = 64'h0000_0000_0020_0000;

  // config/profiles/p0.json -> test_protocol.  TOHOST and FROMHOST are ordinary
  // RAM words; the only thing that is special about TOHOST is that a non-zero
  // write to it ends the run.  It is latched as well as stored, so the value the
  // program wrote is what the harness reports.
  localparam logic [63:0] TOHOST_BASE  = 64'h0000_0000_8000_1000;
  localparam logic [63:0] RESET_VECTOR = 64'h0000_0000_8000_0000;

  localparam int unsigned ROM_WORDS    = 512;      //  4 KiB / 8
  localparam int unsigned UART_WORDS   = 32;       // 256 B  / 8
  localparam int unsigned HARN_WORDS   = 2;        //  16 B  / 8
  localparam int unsigned CLINT_WORDS  = 512;      //  4 KiB / 8
  localparam int unsigned RAM_WORDS    = 262144;   //  2 MiB / 8

  localparam logic [2:0] R_NONE    = 3'd0;
  localparam logic [2:0] R_ROM     = 3'd1;
  localparam logic [2:0] R_UART    = 3'd2;
  localparam logic [2:0] R_HARNESS = 3'd3;
  localparam logic [2:0] R_CLINT   = 3'd4;
  localparam logic [2:0] R_RAM     = 3'd5;

// Access sizes come from mosaic_pkg rather than from a second local copy: a
// testbench that declares its own SZ_* encoding can disagree with the core about
// what "size 2" means, and that disagreement would look like a memory defect.
/* verilator lint_off IMPORTSTAR */
import mosaic_pkg::*;
/* verilator lint_on IMPORTSTAR */


  // --------------------------------------------------------------------------
  // Memory model state.  `m_` prefix: owned by this file.
  // --------------------------------------------------------------------------
  logic [63:0] m_rom    [0:ROM_WORDS-1];
  logic [63:0] m_uart   [0:UART_WORDS-1];
  logic [63:0] m_harn   [0:HARN_WORDS-1];
  logic [63:0] m_clint  [0:CLINT_WORDS-1];
  logic [63:0] m_ram    [0:RAM_WORDS-1];

  logic        m_if_pending;
  logic [31:0] m_if_data;
  logic        m_if_fault;

  logic        m_d_pending;
  logic [63:0] m_d_data;
  logic        m_d_fault;

  logic [63:0] m_tohost_value;
  logic        m_tohost_written;
  // The end-of-program pulse is raised when the store *commits*, not when the
  // memory is asked to perform it.  A request-time pulse would end the run one
  // instruction early and would end it even for a store that faulted.
  logic        m_tohost_commit;
  logic [63:0] m_uart_count;

  // --------------------------------------------------------------------------
  // PMA decode
  // --------------------------------------------------------------------------

  function automatic logic [2:0] pma_region(input logic [63:0] addr);
    if (addr < (ROM_BASE + ROM_SIZE)) pma_region = R_ROM;  // ROM_BASE is 0
    else if ((addr >= UART_BASE) && (addr < (UART_BASE + UART_SIZE))) pma_region = R_UART;
    else if ((addr >= HARN_BASE) && (addr < (HARN_BASE + HARN_SIZE))) pma_region = R_HARNESS;
    else if ((addr >= CLINT_BASE) && (addr < (CLINT_BASE + CLINT_SIZE))) pma_region = R_CLINT;
    else if ((addr >= RAM_BASE) && (addr < (RAM_BASE + RAM_SIZE))) pma_region = R_RAM;
    else pma_region = R_NONE;
  endfunction

  // Every region in the frozen map is readable; the accessor exists so a future
  // map with an unreadable region does not need a new mechanism.
  function automatic logic region_readable(input logic [2:0] rid);
    region_readable = (rid != R_NONE);
  endfunction

  // boot_rom is the only non-writable region in the frozen map.
  function automatic logic region_writable(input logic [2:0] rid);
    region_writable = (rid == R_RAM) || (rid == R_UART) || (rid == R_HARNESS) ||
                      (rid == R_CLINT);
  endfunction

  function automatic logic region_executable(input logic [2:0] rid);
    region_executable = (rid == R_ROM) || (rid == R_RAM);
  endfunction

  function automatic logic [63:0] size_bytes(input logic [2:0] sz);
    case (sz)
      SZ_BYTE: size_bytes = 64'd1;
      SZ_HALF: size_bytes = 64'd2;
      SZ_WORD: size_bytes = 64'd4;
      default: size_bytes = 64'd8;
    endcase
  endfunction

  // An access is permitted when the region allows it and the whole access lies
  // inside that one region.
  function automatic logic access_ok(input logic [63:0] addr, input logic [2:0] sz,
                                     input logic is_write, input logic is_exec);
    logic [2:0]  rid;
    logic [63:0] last;
    rid  = pma_region(addr);
    last = addr + size_bytes(sz) - 64'd1;
    access_ok = 1'b0;
    if (region_readable(rid) && (pma_region(last) == rid)) begin
      if (is_exec)       access_ok = region_executable(rid);
      else if (is_write) access_ok = region_writable(rid);
      else               access_ok = 1'b1;
    end
  endfunction

  // 18 bits covers the largest region (2 MiB / 8 = 262144 words) and is exactly
  // as wide as it needs to be, so no index carries bits nothing can use.
  function automatic logic [17:0] word_index(input logic [2:0] rid,
                                              input logic [63:0] addr);
    logic [63:0] waddr;
    waddr = addr & ~64'd7;
    case (rid)
      R_ROM:     word_index = 18'((waddr - ROM_BASE)   >> 3);
      R_UART:    word_index = 18'((waddr - UART_BASE)  >> 3);
      R_HARNESS: word_index = 18'((waddr - HARN_BASE)  >> 3);
      R_CLINT:   word_index = 18'((waddr - CLINT_BASE) >> 3);
      default:   word_index = 18'((waddr - RAM_BASE)   >> 3);
    endcase
  endfunction

  function automatic logic [63:0] word_value(input logic [2:0] rid,
                                             input logic [63:0] addr);
    case (rid)
      R_ROM:     word_value = m_rom[word_index(rid, addr)[8:0]];
      R_UART:    word_value = m_uart[word_index(rid, addr)[4:0]];
      R_HARNESS: word_value = m_harn[word_index(rid, addr)[0:0]];
      R_CLINT:   word_value = m_clint[word_index(rid, addr)[8:0]];
      default:   word_value = m_ram[word_index(rid, addr)];
    endcase
  endfunction

  // One byte.  Only called for an address access_ok already accepted, so the
  // region is known and the word index is in range.
  function automatic logic [7:0] mem_byte(input logic [63:0] addr);
    logic [2:0]  rid;
    logic [63:0] word;
    rid  = pma_region(addr);
    word = word_value(rid, addr);
    mem_byte = word[{addr[2:0], 3'b000} +: 8];
  endfunction

  // A whole access, assembled byte by byte so a misaligned request is byte-exact
  // rather than silently returning the containing word.
  function automatic logic [63:0] mem_load(input logic [63:0] addr,
                                           input logic [2:0] sz,
                                           input logic is_exec,
                                           output logic fault);
    logic [63:0] value;
    int unsigned i;
    fault = 1'b1;
    value = 64'd0;
    if (access_ok(addr, sz, 1'b0, is_exec)) begin
      for (i = 0; i < 8; i = i + 1) begin
        if (64'(i) < size_bytes(sz)) value[8*i +: 8] = mem_byte(addr + 64'(i));
      end
      fault = 1'b0;
    end
    mem_load = value;
  endfunction

  // An instruction fetch is always exactly four bytes and always executes, so it
  // gets its own accessor rather than a narrowed copy of mem_load's word: the
  // two differ in exactly one place -- `is_exec` -- and having one place where
  // that differs is worth more than the parameter.
  function automatic logic [31:0] mem_fetch(input logic [63:0] addr,
                                            output logic fault);
    logic [31:0]  value;
    int unsigned i;
    fault = 1'b1;
    value = 32'h00000000;
    if (access_ok(addr, SZ_WORD, 1'b0, 1'b1)) begin
      for (i = 0; i < 4; i = i + 1) begin
        value[8*i +: 8] = mem_byte(addr + 64'(i));
      end
      fault = 1'b0;
    end
    mem_fetch = value;
  endfunction

  function automatic logic [63:0] store_mask_of(input logic [2:0] sz);
    case (sz)
      SZ_BYTE: store_mask_of = 64'h0000_0000_0000_00ff;
      SZ_HALF: store_mask_of = 64'h0000_0000_0000_ffff;
      SZ_WORD: store_mask_of = 64'h0000_0000_ffff_ffff;
      default: store_mask_of = 64'hffff_ffff_ffff_ffff;
    endcase
  endfunction

  // --------------------------------------------------------------------------
  // DUT instance.  The core's outputs land straight on the `c_*` output ports;
  // the model's answers come straight back out of `m_*`.
  // --------------------------------------------------------------------------
  wire        c_ifetch_req;
  wire [63:0] c_ifetch_addr;
  wire        c_dmem_req;
  wire        c_dmem_we;
  wire [63:0] c_dmem_addr;
  wire [2:0]  c_dmem_size;
  wire [63:0] c_dmem_wdata;

  mosaic_bringup_core u_core (
      .clk_i            (h_clk),
      .rst_i            (h_rst),
      .reset_vector_i   (RESET_VECTOR),

      .ifetch_req_o     (c_ifetch_req),
      .ifetch_addr_o    (c_ifetch_addr),
      .ifetch_ack_i     (m_if_pending),
      .ifetch_rdata_i   (m_if_data),
      .ifetch_fault_i   (m_if_fault),

      .dmem_req_o       (c_dmem_req),
      .dmem_we_o        (c_dmem_we),
      .dmem_addr_o      (c_dmem_addr),
      .dmem_size_o      (c_dmem_size),
      .dmem_wdata_o     (c_dmem_wdata),
      .dmem_ack_i       (m_d_pending),
      .dmem_rdata_i     (m_d_data),
      .dmem_fault_i     (m_d_fault),

      .evt_valid_o      (c_evt_valid),
      .evt_trap_o       (c_evt_trap),
      .evt_pc_o         (c_evt_pc),
      .evt_next_pc_o    (c_evt_next_pc),
      .evt_insn_o       (c_evt_insn),
      .evt_has_rd_o     (c_evt_has_rd),
      .evt_rd_o         (c_evt_rd),
      .evt_rd_value_o   (c_evt_rd_value),
      .evt_cause_o      (c_evt_cause),
      .evt_tval_o       (c_evt_tval),
      .evt_epc_o        (c_evt_epc),
      .evt_is_store_o   (c_evt_is_store),
      .evt_store_addr_o (c_evt_store_addr),
      .evt_store_data_o (c_evt_store_data),
      .evt_store_size_o (c_evt_store_size),

      .dbg_csr_addr_i   (h_dbg_csr_addr),
      .dbg_csr_data_o   (c_dbg_csr_data),
      .dbg_pc_o         (c_dbg_pc),
      .dbg_state_o      (c_dbg_state)
  );

  assign h_tohost_value   = m_tohost_value;
  assign h_tohost_written = m_tohost_written;
  assign h_uart_count     = m_uart_count;
  assign h_if_pending     = m_if_pending;
  assign h_d_pending      = m_d_pending;
  assign c_dmem_req_o     = c_dmem_req;
  assign c_dmem_we_o      = c_dmem_we;
  assign c_dmem_addr_o    = c_dmem_addr;

  // --------------------------------------------------------------------------
  // The model's one sequential process: reset, the data and fetch ports, the
  // harness image loader and the harness readback port.  One process per array,
  // so there is never any question about who writes a word.
  // --------------------------------------------------------------------------
  // Every value the sequential block below needs that is a function of a request
  // rather than of stored state is computed here, so the sequential block holds
  // nothing but nonblocking assignments.
  logic [63:0] rb_data_c;
  logic        rb_fault_c;
  logic [31:0] if_data_c;
  logic        if_fault_c;
  logic [63:0] d_data_c;
  logic        d_fault_c;

  logic [2:0]  store_rid;
  logic [17:0] store_idx;
  logic [63:0] store_mask_v;
  logic [63:0] store_shift;
  logic [63:0] store_value;
  logic        store_allowed;
  logic        store_is_tohost;

  logic [2:0]  img_rid;
  logic [17:0] img_idx;
  int unsigned zi;

  always_comb begin
    rb_data_c    = mem_load(h_rb_addr, SZ_DBL, 1'b0, rb_fault_c);
    if_data_c    = mem_fetch(c_ifetch_addr, if_fault_c);
    d_data_c     = mem_load(c_dmem_addr, c_dmem_size, 1'b0, d_fault_c);

    store_rid       = pma_region(c_dmem_addr);
    store_idx       = word_index(store_rid, c_dmem_addr);
    store_mask_v    = store_mask_of(c_dmem_size);
    // Where inside the containing 8-byte word the access lands.  The core
    // performs every alignment check itself and never issues a misaligned
    // request, and every access width is a power of two, so an access can never
    // straddle a word boundary and one word is always enough to update.
    store_shift     = 64'(c_dmem_addr[2:0]) * 64'd8;
    store_value     = c_dmem_wdata & store_mask_v;
    store_allowed   = access_ok(c_dmem_addr, c_dmem_size, 1'b1, 1'b0);
    store_is_tohost = ((c_dmem_addr & ~64'd7) == TOHOST_BASE);

    img_rid = pma_region(h_img_addr);
    img_idx = word_index(img_rid, h_img_addr);
  end

  always_ff @(posedge h_clk) begin
    // ---- harness image loader ----------------------------------------------
    // Deliberately outside the reset branch: the harness pushes the image in
    // while reset is still asserted, and a loader that only worked once reset
    // released would force it to release first.  Word-granular by contract:
    // `h_img_addr` is 8-byte aligned and `h_img_data` is the whole word, so a
    // plain assignment is correct.
    if (h_img_we) begin
      case (img_rid)
        R_ROM:     m_rom[img_idx[8:0]]   <= h_img_data;
        R_UART:    m_uart[img_idx[4:0]]  <= h_img_data;
        R_HARNESS: m_harn[img_idx[0:0]]  <= h_img_data;
        R_CLINT:   m_clint[img_idx[8:0]] <= h_img_data;
        R_RAM:     m_ram[img_idx]        <= h_img_data;
        // A segment outside the frozen map is a load error the harness reports
        // by name; the model drops it so the run continues and fails its own
        // checks rather than dying here.
        default: ;
      endcase
    end

    if (h_clear_mem) begin
      // Every array starts as a defined zero rather than as whatever the
      // simulator happened to leave behind.  Without this, a program that reads
      // an address nobody wrote would produce a different retire stream on every
      // run and the differential comparison would be worthless.
      for (zi = 0; zi < ROM_WORDS; zi = zi + 1)   m_rom[zi]   <= 64'd0;
      for (zi = 0; zi < UART_WORDS; zi = zi + 1)  m_uart[zi]  <= 64'd0;
      for (zi = 0; zi < HARN_WORDS; zi = zi + 1)  m_harn[zi]  <= 64'd0;
      for (zi = 0; zi < CLINT_WORDS; zi = zi + 1) m_clint[zi] <= 64'd0;
`ifdef MOSAIC_RESET_MUTANT_SKIP_CLEAR
      // MUTANT (V-009 control): the RAM data array is deliberately left alone
      // by the clear pass.  A word nobody wrote then keeps whatever the
      // previous run -- or the host simulator's own zero initialisation --
      // left there, which is the defect "the harness depends on C++ memory
      // happening to be zero".  The other four regions still clear, so only
      // the SRAM-initialisation check can see this.
`else
      for (zi = 0; zi < RAM_WORDS; zi = zi + 1)   m_ram[zi]   <= 64'd0;
`endif
      m_if_pending     <= 1'b0;
      m_if_data        <= 32'h00000000;
      m_if_fault       <= 1'b0;
      m_d_pending      <= 1'b0;
      m_d_data         <= 64'd0;
      m_d_fault        <= 1'b0;
      m_tohost_written <= 1'b0;
      m_tohost_commit  <= 1'b0;
      m_tohost_value   <= 64'd0;
      m_uart_count     <= 64'd0;
      h_rb_data        <= 64'd0;
      h_rb_fault       <= 1'b0;
      h_rb_valid       <= 1'b0;
    end else if (h_rst) begin
`ifdef MOSAIC_RESET_MUTANT_STALE_PENDING
      // MUTANT (V-009 control): an outstanding transaction is not cleared by
      // reset, so a request the environment accepted before the reset is
      // answered after it.  "An old transaction crossed the reset."
`else
      m_if_pending     <= 1'b0;
      m_if_data        <= 32'h00000000;
      m_if_fault       <= 1'b0;
      m_d_pending      <= 1'b0;
      m_d_data         <= 64'd0;
      m_d_fault        <= 1'b0;
`endif
      m_tohost_written <= 1'b0;
      // The end-of-run latch is a transaction like any other: a TOHOST store
      // that was accepted but whose report has not yet been registered must not
      // survive a reset.  Without this line a reset landing in the one-cycle
      // window between the store request and `m_tohost_written` leaves the run
      // ending itself on the first cycle after release, with no instruction
      // having executed -- the defect V-009 exists to catch, found by the
      // reset-during-TOHOST-store case in sim/unit/tb_reset.cpp.
`ifdef MOSAIC_RESET_MUTANT_STALE_TOHOST
      // MUTANT (V-009 control): the latch survives reset.
`else
      m_tohost_commit  <= 1'b0;
`endif
      m_tohost_value   <= 64'd0;
      m_uart_count     <= 64'd0;
      h_rb_data        <= 64'd0;
      h_rb_fault       <= 1'b0;
      h_rb_valid       <= 1'b0;
    end else begin
      m_tohost_written <= m_tohost_commit;
      m_tohost_commit  <= 1'b0;

      // ---- harness readback: request in cycle N, answer in cycle N+1 -------
      h_rb_valid  <= h_rb_req;
      h_rb_data   <= rb_data_c;
      h_rb_fault  <= rb_fault_c;

      // ---- instruction fetch: one cycle of latency, then ack ---------------
      if (c_ifetch_req && !m_if_pending) begin
        m_if_data    <= if_data_c;
        m_if_fault   <= if_fault_c;
        m_if_pending <= 1'b1;
      end else if (m_if_pending) begin
        m_if_pending <= 1'b0;
      end

      // ---- data port -------------------------------------------------------
      if (c_dmem_req && !m_d_pending) begin
        m_d_pending <= 1'b1;
        if (c_dmem_we) begin
          if (!store_allowed) begin
            // Unmapped, straddling a region, or a read-only region such as
            // boot_rom.  The write is not performed.
            m_d_fault <= 1'b1;
          end else begin
            case (store_rid)
              R_ROM: m_rom[store_idx[8:0]] <=
                        word_value(store_rid, c_dmem_addr) & ~(store_mask_v << store_shift) |
                        (store_value << store_shift);
              R_UART: begin
                // A UART THR write is a byte the firmware is printing.  Counted
                // so the harness can prove the device was actually driven; the
                // byte itself stays in the region's memory.
                m_uart[store_idx[4:0]] <=
                        word_value(store_rid, c_dmem_addr) & ~(store_mask_v << store_shift) |
                        (store_value << store_shift);
                m_uart_count      <= m_uart_count + 64'd1;
              end
              R_HARNESS: m_harn[store_idx[0:0]] <=
                              word_value(store_rid, c_dmem_addr) & ~(store_mask_v << store_shift) |
                              (store_value << store_shift);
              R_CLINT:   m_clint[store_idx[8:0]] <=
                              word_value(store_rid, c_dmem_addr) & ~(store_mask_v << store_shift) |
                              (store_value << store_shift);
              default:   m_ram[store_idx] <=
                        word_value(store_rid, c_dmem_addr) & ~(store_mask_v << store_shift) |
                        (store_value << store_shift);
            endcase
            // The protocol register is ordinary memory that also ends the run:
            // a non-zero write to TOHOST is the frozen end-of-program signal.
            if (store_is_tohost) begin
              m_tohost_value  <= store_value;
              m_tohost_commit <= (store_value != 64'd0);
            end
            m_d_fault <= 1'b0;
          end
        end else begin
          m_d_data  <= d_data_c;
          m_d_fault <= d_fault_c;
        end
      end else if (m_d_pending) begin
        m_d_pending <= 1'b0;
      end
    end
  end

endmodule : mosaic_bringup_tb

`default_nettype wire
