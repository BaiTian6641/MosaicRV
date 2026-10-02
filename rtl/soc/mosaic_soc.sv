// ============================================================================
// mosaic_soc -- the platform interconnect and its peripherals (work package
// I-047: "implement the SoC interconnect and error completion").
//
// This is the first *device implementation* in the tree. Until now the memory
// map, the UART, the timer and the test-end register existed only as models:
// `sim/common/memory_model.cpp` for the harness, and the device models inside
// `tb_core_mmio_model.cpp` / `tb_core_mmio.cpp` for the MMIO cases. Those
// models are the *contract* this module is written to; they are not hardware.
// The register layout below is the one the V-019 model exercises, so a program
// that ran against that model runs against this fabric unchanged.
//
// ------------------------------------------------------------------- shape
//
// Two requester ports, each exactly the port `mosaic_lsu_endpoint.sv` presents
// to the memory system (`mem_req_t` on the request side, `mem_rsp_t` on the
// response side) plus the transaction identity the endpoint publishes beside
// that port (`o_txn_id`):
//
//   * `mX_req_we/size/wstrb/wdata/addr` are `mem_req_t` field for field;
//   * `mX_req_amo` is `mem_req_t.amo` (the endpoint presents an atomic as two
//     beats on this port and computes the arithmetic itself, so the fabric
//     never needs `amo_op`; see "what is not carried");
//   * `mX_req_id` is the requester's identity for this transaction;
//   * `mX_rsp_rdata/fault` are `mem_rsp_t`, and `mX_rsp_id` returns the identity
//     so the requester can match the completion to its request.
//
// The ports are ordinary flattened `logic` with explicit widths rather than
// packed structs or an interface -- this project's RTL style -- and both tools
// read them the same way.
//
// --------------------------------------------------------- the rules (1..5)
//
// 1. **One completion per accepted request.** A request is *accepted* on a
//    rising edge where `mX_req_valid && mX_req_ready`. It is then owned by one
//    transaction-table entry until its completion is *consumed* on a rising
//    edge where `mX_rsp_valid && mX_rsp_ready`. The counters are a conservation
//    identity checked every cycle:
//
//        accepted == completed_normal + completed_error + outstanding
//
//    `o_conservation_ok` is that identity, and the registered counters are
//    exported so a testbench can compare them against its own bookkeeping.
//    "Every accepted request receives exactly one completion" is the card's
//    pass condition; a counter leak is precisely the card's fail mode ("a
//    response error is dropped and the ROB waits for ever").
//
// 2. **Identity is preserved.** The requester supplies `mX_req_id`; it travels
//    with the transaction to the slave port and back, and the completion is
//    presented on the *originating* requester's port carrying that same id. A
//    slave that returned a different id than it was given raises
//    `o_id_mismatch` and is a defect, not a liberty.
//
// 3. **Byte strobes are preserved.** `wstrb` and the lane-aligned `wdata` are
//    carried unchanged from the requester to the slave. A narrow store into a
//    wider peripheral register updates only the strobed lanes (the UART scratch
//    and the timer compare register are the proof).
//
// 4. **Backpressure costs cycles, never a request.** `mX_req_ready` is low
//    exactly when the transaction table is full, and a slave's request port is
//    free only when that slave is not already answering; in both cases the
//    offered payload is held and accepted once, later. No `ready` is a function
//    of the *same* port's `valid`, so there is no combinational ready loop
//    (stated as structure; a loop cannot be exhibited by a module that does not
//    contain one). `stall_i` holds every dispatch, which is how the memory
//    system's own backpressure is modelled.
//
// 5. **Errors are completions, not silence.** An address no region covers
//    produces a `DECERR` completion; a region that is mapped but refuses the
//    access (a write to the read-only boot ROM, an access that is misaligned,
//    crosses a register boundary, or names a register the device does not
//    implement) produces a `SLVERR` completion. Both are delivered as
//    `mX_rsp_fault = 1` with the class on `mX_rsp_err`; the core's endpoint
//    turns the fault into a load/store access fault of the class its own `we`
//    selects. Neither is ever dropped.
//
// ------------------------------------------------- reset with work in flight
//
// Reset is synchronous and clears the transaction table, the slave busy
// registers and all four counters **together**, so the conservation identity
// survives the reset unchanged: the accepted and outstanding counts fall by the
// same amount. A transaction that was in flight when `rst` asserted is
// therefore *aborted*: it is discarded, it is never delivered later (the table
// is empty after reset), and `o_abort_pulse` is high in the cycle reset is
// asserted while `outstanding != 0`, so the loss is reported rather than
// hidden. That is the specified behaviour, and the case checks all three
// consequences. The requester is expected to be reset with the fabric -- a hart
// does not survive the reset that resets its SoC -- so no completion is owed
// across the boundary, and a fresh request after reset completes normally.
//
// ------------------------------------------------- the platform map (p0/p1)
//
// The decode is the union of `config/memory/p0.json` and `config/memory/p1.json`
// (the p1 map is p0 plus the CLINT MSIP word), taken from the generated
// `mosaic_cfg_pkg` so the RTL and the C++ harness read one source of truth:
//
//     boot_rom      0x0000_0000 + 0x1000   read-only, loaded via a load port
//     uart          0x0010_0000 + 0x0100   RX/TX/STATUS/SCRATCH
//     test_harness  0x0010_2000 + 0x0010   identity register, reads only
//     clint         0x0200_0000 + 0x1000   mtime / mtimecmp / test-end
//     ram           0x8000_0000 + 2 MiB    byte-strobed read/write
//     clint_msip    0x000C_0000 + 0x4      p1 only (software interrupt)
//
// `HAS_MSIP` is set by the top-level wrapper from the profile, so a p0 build
// decodes 0x000C_0000 as unmapped (`DECERR`) exactly as p0's map says, and a p1
// build decodes it as the MSIP word.
//
// ----------------------------------------------------------- the peripherals
//
// * **RAM** -- a byte-strobed synchronous read/write array, no reset on the
//   storage (the same reset contract as `rtl/common/mosaic_ram.sv`: validity is
//   the consumer's). One access per two cycles per slave, the conservative
//   start the plan asks for.
// * **boot ROM** -- loaded through a dedicated load port (a mask ROM behind a
//   board wrapper), read-only to the requester; a write is `SLVERR`.
// * **UART** -- 0x00 RX (a read pops one queued input word; the pop is one side
//   effect), 0x04 TX (a write transmits the strobed bytes; one side effect per
//   accepted access), 0x08 STATUS (read only, no side effect; bit 0 = RX not
//   empty, bit 8 = TX ready), 0x0C SCRATCH (read/write, byte-strobed 32-bit).
// * **CLINT** -- 0x00 mtime (free-running 64-bit counter, read only), 0x08
//   mtimecmp (read/write, byte-strobed 64-bit), 0x10 the test-end register
//   (write only, one side effect). The timer interrupt is `mtime >= mtimecmp`.
// * **test_harness** -- 0x00 the identity register (reads "TEST"); everything
//   else in the window is `SLVERR`.
// * **MSIP** -- one 32-bit word; writing bit 0 sets the software interrupt,
//   writing 0 clears it.
//
// Every device access must be aligned to its size and lie inside one register
// window; anything else is `SLVERR` and performs nothing. A read has a side
// effect only where the platform says so (the UART RX pop), and a write's side
// effect happens exactly once, in the cycle the fabric dispatches it to the
// slave -- the fabric dispatches each table entry exactly once, which is where
// "MMIO side effects are not duplicated" is enforced.
//
// ------------------------------------------------------- what is not carried
//
// `amo_op`, `aq` and `rl` are not ported. The endpoint presents an atomic
// read-modify-write as two beats on this port and computes the arithmetic
// itself (`mosaic_amo_alu`), so `amo_op` is never acted on by a memory system
// that receives the port; and this fabric serves one requester per port in the
// order it is offered, so it has no reordering for `aq`/`rl` to constrain. Both
// are consequences of the one-requester-per-port structure, not omissions; a
// future cross-bar that reorders must add them.
//
// ---------------------------------------------------------------- mutants
//
// -DMOSAIC_SOC_MUTANT_* selects a deliberately broken variant (shipping defines
// none of them). See results/reports/I-047-soc.md and tools/run_soc_controls.py.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_soc #(
  // Identity width. Wide enough that a flushed request's completion is
  // recognisable rather than merely unlikely; the case uses well over one value.
  parameter int unsigned ID_W            = 4,
  // Transaction-table depth: the number of requests that may be accepted and
  // not yet consumed.
  parameter int unsigned MAX_OUTSTANDING = 4,
  // p1 declares a CLINT MSIP word that p0 does not. The wrapper sets this from
  // the profile, so each profile decodes exactly its own map.
  parameter bit          HAS_MSIP        = 1'b0,
  parameter logic [63:0] MSIP_BASE       = 64'h00000000000C0000
) (
  input  logic            clk,
  input  logic            rst,

  // ------------------------------------------------- requester 0 (data side)
  input  logic            m0_req_valid_i,
  output logic            m0_req_ready_o,
  input  logic            m0_req_we_i,
  input  logic [63:0]     m0_req_addr_i,
  input  logic [2:0]      m0_req_size_i,
  input  logic [7:0]      m0_req_wstrb_i,
  input  logic [63:0]     m0_req_wdata_i,
  input  logic            m0_req_amo_i,
  input  logic [ID_W-1:0] m0_req_id_i,
  output logic            m0_rsp_valid_o,
  input  logic            m0_rsp_ready_i,
  output logic [63:0]     m0_rsp_rdata_o,
  output logic            m0_rsp_fault_o,
  output logic [1:0]      m0_rsp_err_o,
  output logic [ID_W-1:0] m0_rsp_id_o,

  // ------------------------------------------- requester 1 (instruction side)
  input  logic            m1_req_valid_i,
  output logic            m1_req_ready_o,
  input  logic            m1_req_we_i,
  input  logic [63:0]     m1_req_addr_i,
  input  logic [2:0]      m1_req_size_i,
  input  logic [7:0]      m1_req_wstrb_i,
  input  logic [63:0]     m1_req_wdata_i,
  input  logic            m1_req_amo_i,
  input  logic [ID_W-1:0] m1_req_id_i,
  output logic            m1_rsp_valid_o,
  input  logic            m1_rsp_ready_i,
  output logic [63:0]     m1_rsp_rdata_o,
  output logic            m1_rsp_fault_o,
  output logic [1:0]      m1_rsp_err_o,
  output logic [ID_W-1:0] m1_rsp_id_o,

  // ------------------------------- the memory system's own backpressure line
  // High = no dispatch to any slave this cycle. Entries are still accepted if
  // the table has room, so the case can hold a request in the table and check it
  // is neither lost nor duplicated when the stall releases.
  input  logic            stall_i,

  // ---------------------------------------------------- boot ROM load port
  input  logic            rom_load_en_i,
  input  logic [8:0]      rom_load_index_i,
  input  logic [63:0]     rom_load_data_i,

  // ------------------------------------------------ UART external serial in
  input  logic            uart_rx_push_i,
  input  logic [7:0]      uart_rx_data_i,
  output logic            uart_rx_full_o,
  output logic            uart_tx_valid_o,
  output logic [7:0]      uart_tx_data_o,

  // --------------------------------------------------------------- interrupts
  output logic            irq_timer_o,
  output logic            irq_soft_o,
  output logic            irq_ext_o,
  output logic            exit_valid_o,
  output logic [31:0]     exit_code_o,

  // ------------------------------------------------------------ observability
  output logic [31:0]     o_accepted_ctr,
  output logic [31:0]     o_completed_normal_ctr,
  output logic [31:0]     o_completed_error_ctr,
  output logic [31:0]     o_outstanding_ctr,
  output logic            o_conservation_ok,
  output logic            o_abort_pulse,
  output logic            o_id_mismatch,
  output logic [31:0]     o_uart_rx_pop_ctr,
  output logic [31:0]     o_uart_tx_ctr,
  output logic [31:0]     o_clint_exit_ctr,
  output logic [31:0]     o_dec_err_ctr,
  output logic [31:0]     o_slv_err_ctr
);

  // ---------------------------------------------------------------- geometry
  localparam int unsigned IDX_W = (MAX_OUTSTANDING <= 1) ? 1 : $clog2(MAX_OUTSTANDING);
  localparam int unsigned NSLV  = 6;

  localparam logic [2:0] SLV_RAM     = 3'd0;
  localparam logic [2:0] SLV_ROM     = 3'd1;
  localparam logic [2:0] SLV_UART    = 3'd2;
  localparam logic [2:0] SLV_CLINT   = 3'd3;
  localparam logic [2:0] SLV_HARNESS = 3'd4;
  localparam logic [2:0] SLV_MSIP    = 3'd5;

  localparam logic [1:0] ERR_OK     = 2'd0;
  localparam logic [1:0] ERR_SLVERR = 2'd1;
  localparam logic [1:0] ERR_DECERR = 2'd2;

  // The frozen map, from the generated package (never re-typed here).
  localparam logic [63:0] ROM_BASE   = mosaic_cfg_pkg::MOSAIC_BOOT_ROM_BASE;
  localparam logic [63:0] ROM_SIZE   = mosaic_cfg_pkg::MOSAIC_BOOT_ROM_SIZE;
  localparam logic [63:0] UART_BASE  = mosaic_cfg_pkg::MOSAIC_UART_BASE;
  localparam logic [63:0] UART_SIZE  = mosaic_cfg_pkg::MOSAIC_UART_SIZE;
  localparam logic [63:0] HARN_BASE  = mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_BASE;
  localparam logic [63:0] HARN_SIZE  = mosaic_cfg_pkg::MOSAIC_TEST_HARNESS_SIZE;
  localparam logic [63:0] CLINT_BASE = mosaic_cfg_pkg::MOSAIC_CLINT_BASE;
  localparam logic [63:0] CLINT_SIZE = mosaic_cfg_pkg::MOSAIC_CLINT_SIZE;
  localparam logic [63:0] RAM_BASE   = mosaic_cfg_pkg::MOSAIC_RAM_BASE;
  localparam logic [63:0] RAM_SIZE   = mosaic_cfg_pkg::MOSAIC_RAM_SIZE;

  localparam int unsigned RAM_WORDS = int'(RAM_SIZE / 64'd8);   // 2 MiB / 8
  localparam int unsigned ROM_WORDS = int'(ROM_SIZE / 64'd8);   // 4 KiB / 8
  localparam int unsigned RAM_IDX_W = $clog2(RAM_WORDS);
  localparam int unsigned ROM_IDX_W = $clog2(ROM_WORDS);
  localparam int unsigned RX_DEPTH  = 8;
  localparam logic [3:0]  RX_DEPTH_V = 4'd8;

  // Device register addresses, relative to their region base.
  localparam logic [63:0] UART_RX      = 64'h00;
  localparam logic [63:0] UART_TX      = 64'h04;
  localparam logic [63:0] UART_STATUS  = 64'h08;
  localparam logic [63:0] UART_SCRATCH = 64'h0C;
  localparam logic [63:0] CLINT_MTIME    = 64'h00;
  localparam logic [63:0] CLINT_MTIMECMP = 64'h08;
  localparam logic [63:0] CLINT_EXIT     = 64'h10;
  localparam logic [63:0] HARN_ID        = 64'h00;
  localparam logic [31:0] HARN_ID_VALUE  = 32'h54455354;  // "TEST"

  // ------------------------------------------------------------- helpers
  // Containment by wrapped subtraction: an address below the base cannot be
  // mistaken for one inside, whatever the base is.
  function automatic logic in_region(input logic [63:0] a,
                                     input logic [63:0] base,
                                     input logic [63:0] size);
    in_region = ((a - base) < size);
  endfunction

  // True when [addr, addr+size) lies inside the register window [base, base+width)
  // and the access is aligned to its own size. Misaligned or straddling device
  // accesses perform nothing.
  function automatic logic fits(input logic [63:0] addr,
                                input logic [2:0]  size,
                                input logic [63:0] base,
                                input logic [63:0] width);
    logic [63:0] nbytes;
    begin
      nbytes = (64'd1 << size);
      fits = (addr >= base) &&
             ((addr + nbytes) <= (base + width)) &&
             ((addr % nbytes) == 64'd0);
    end
  endfunction

  // The byte window of a register, positioned at the access's address lane.
  // This is the lane-aligned convention the LSU endpoint documents: the byte at
  // `addr` is lane `addr[2:0]` of the doubleword. `off` is the byte offset of
  // the access inside the register, `lane` is the access's address lane.
  function automatic logic [63:0] lane_extract(input logic [63:0] value,
                                               input logic [3:0]  off,
                                               input logic [2:0]  size,
                                               input logic [2:0]  lane);
    logic [63:0] mask;
    begin
      case (size)
        3'd0:    mask = 64'h00000000000000FF;
        3'd1:    mask = 64'h000000000000FFFF;
        3'd2:    mask = 64'h00000000FFFFFFFF;
        default: mask = 64'hFFFFFFFFFFFFFFFF;
      endcase
      lane_extract = ((value >> (8 * off)) & mask) << (8 * lane);
    end
  endfunction

  // ----------------------------------------------------------- the RAM / ROM
  // No reset on the storage, matching rtl/common/mosaic_ram.sv: a consumer must
  // write before it reads. The read is registered at the dispatch edge, which is
  // what makes it a synchronous RAM rather than a combinational mux.
  logic [63:0] ram [RAM_WORDS];
  logic [63:0] rom [ROM_WORDS];

  // -------------------------------------------------------------- the state
  // The transaction table, parallel arrays rather than a struct so every field
  // is written explicitly (this project's RTL style).
  logic [MAX_OUTSTANDING-1:0]            txn_valid;
  logic [MAX_OUTSTANDING-1:0]            txn_master;
  logic [MAX_OUTSTANDING-1:0][ID_W-1:0]  txn_id;
  logic [MAX_OUTSTANDING-1:0][63:0]      txn_addr;
  logic [MAX_OUTSTANDING-1:0]            txn_we;
  logic [MAX_OUTSTANDING-1:0][2:0]       txn_size;
  logic [MAX_OUTSTANDING-1:0][7:0]       txn_wstrb;
  logic [MAX_OUTSTANDING-1:0][63:0]      txn_wdata;
  logic [MAX_OUTSTANDING-1:0]            txn_amo;
  logic [MAX_OUTSTANDING-1:0][2:0]       txn_slave;
  logic [MAX_OUTSTANDING-1:0]            txn_decoded;
  logic [MAX_OUTSTANDING-1:0]            txn_inflight;
  logic [MAX_OUTSTANDING-1:0]            txn_done;
  logic [MAX_OUTSTANDING-1:0][1:0]       txn_err;
  logic [MAX_OUTSTANDING-1:0][63:0]      txn_rdata;

  // Per-slave dispatch state. `sl_responding_q[s]` is high in the cycle after a
  // dispatch to slave s, when the slave's registered response is presented back
  // to the fabric: it is both "the response is valid" and "the slave is busy",
  // because the response is always exactly one cycle behind the dispatch.
  logic [NSLV-1:0]            sl_responding_q;
  logic [NSLV-1:0][IDX_W-1:0] sl_entry_q;
  logic [NSLV-1:0][ID_W-1:0]  sl_id_q;
  logic [NSLV-1:0][1:0]       sl_err_q;
  logic [NSLV-1:0][63:0]      sl_rdata_q;

  // Counters (the conservation identity and the device side-effect counts).
  logic [31:0] accepted_q;
  logic [31:0] completed_norm_q;
  logic [31:0] completed_err_q;
  logic [31:0] outstanding_q;
  logic [31:0] uart_tx_ctr_q;
  logic [31:0] uart_rx_pop_ctr_q;
  logic [31:0] clint_exit_ctr_q;
  logic [31:0] dec_err_ctr_q;
  logic [31:0] slv_err_ctr_q;
  logic        id_mismatch_q;

  // Peripheral state.
  logic [31:0] uart_scratch_q;
  logic [31:0] rx_mem [RX_DEPTH];
  logic [3:0]  rx_wr_q;
  logic [3:0]  rx_rd_q;
  logic [3:0]  rx_count_q;
  logic [31:0] msip_q;
  logic [63:0] mtime_q;
  logic [63:0] mtimecmp_q;
  logic [31:0] exit_code_q;
  logic        exit_valid_q;
  logic        uart_tx_valid_q;
  logic [7:0]  uart_tx_data_q;

  // Round-robin bit for accept arbitration between the two requesters.
  logic rr_q;

  // ------------------------------------------------------- accept arbitration
  logic             free_c;
  logic [IDX_W-1:0] free_idx_c;
  logic             accept_c;
  logic             accept_m0_c;

  always_comb begin
    free_c     = 1'b0;
    free_idx_c = '0;
    for (int unsigned i = 0; i < MAX_OUTSTANDING; i++) begin
      if (!free_c && !txn_valid[i]) begin
        free_c     = 1'b1;
        free_idx_c = i[IDX_W-1:0];
      end
    end
  end

  // The accept decision depends on the table (state), the requester valids and
  // the round-robin bit -- never on the accepted requester's own ready, and
  // never on a slave. There is no path from an acceptance back to its own
  // offer, so no combinational ready loop exists here.
  always_comb begin
    accept_c    = 1'b0;
    accept_m0_c = 1'b0;
    if (free_c) begin
      if (m0_req_valid_i && m1_req_valid_i) begin
        accept_c    = 1'b1;
        accept_m0_c = ~rr_q;
      end else if (m0_req_valid_i) begin
        accept_c    = 1'b1;
        accept_m0_c = 1'b1;
      end else if (m1_req_valid_i) begin
        accept_c    = 1'b1;
        accept_m0_c = 1'b0;
      end
    end
  end

  assign m0_req_ready_o = accept_c &&  accept_m0_c;
  assign m1_req_ready_o = accept_c && !accept_m0_c;

  // The accepted request, selected combinationally for the table write.
  logic            acc_we_c;
  logic [63:0]     acc_addr_c;
  logic [2:0]      acc_size_c;
  logic [7:0]      acc_wstrb_c;
  logic [63:0]     acc_wdata_c;
  logic            acc_amo_c;
  logic [ID_W-1:0] acc_id_c;

  always_comb begin
    acc_we_c    = accept_m0_c ? m0_req_we_i    : m1_req_we_i;
    acc_addr_c  = accept_m0_c ? m0_req_addr_i  : m1_req_addr_i;
    acc_size_c  = accept_m0_c ? m0_req_size_i  : m1_req_size_i;
    acc_wstrb_c = accept_m0_c ? m0_req_wstrb_i : m1_req_wstrb_i;
    acc_wdata_c = accept_m0_c ? m0_req_wdata_i : m1_req_wdata_i;
    acc_amo_c   = accept_m0_c ? m0_req_amo_i   : m1_req_amo_i;
    acc_id_c    = accept_m0_c ? m0_req_id_i    : m1_req_id_i;
  end

  // ------------------------------------------------------------------ decode
  logic       dec_hit_c;
  logic [2:0] dec_slv_c;

  always_comb begin
    logic in_rom, in_uart, in_harness, in_clint, in_msip, in_ram;
    in_rom     = in_region(acc_addr_c, ROM_BASE,   ROM_SIZE);
    in_uart    = in_region(acc_addr_c, UART_BASE,  UART_SIZE);
    in_harness = in_region(acc_addr_c, HARN_BASE,  HARN_SIZE);
    in_clint   = in_region(acc_addr_c, CLINT_BASE, CLINT_SIZE);
    in_msip    = HAS_MSIP && in_region(acc_addr_c, MSIP_BASE, 64'd4);
    in_ram     = in_region(acc_addr_c, RAM_BASE,   RAM_SIZE);

    dec_hit_c = in_rom | in_uart | in_harness | in_clint | in_msip | in_ram;
    if (in_ram)          dec_slv_c = SLV_RAM;
    else if (in_rom)     dec_slv_c = SLV_ROM;
    else if (in_uart)    dec_slv_c = SLV_UART;
    else if (in_clint)   dec_slv_c = SLV_CLINT;
    else if (in_harness) dec_slv_c = SLV_HARNESS;
    else if (in_msip)    dec_slv_c = SLV_MSIP;
    else                 dec_slv_c = SLV_RAM;  // don't care; `dec_hit_c` is 0
  end

  // ------------------------------------------------------------- dispatch
  logic             sl_disp_c [NSLV];
  logic [IDX_W-1:0] sl_disp_idx_c [NSLV];

  always_comb begin
    for (int unsigned s = 0; s < NSLV; s++) begin
      sl_disp_c[s]     = 1'b0;
      sl_disp_idx_c[s] = '0;
      if (!sl_responding_q[s] && !stall_i) begin
        for (int unsigned i = 0; i < MAX_OUTSTANDING; i++) begin
          if (!sl_disp_c[s] && txn_valid[i] && !txn_done[i] && !txn_inflight[i] &&
              txn_decoded[i] && (txn_slave[i] == s[2:0])) begin
            sl_disp_c[s]     = 1'b1;
            sl_disp_idx_c[s] = i[IDX_W-1:0];
          end
        end
      end
    end
  end

  // The dispatched request's fields, per slave.
  logic            sl_d_we_c    [NSLV];
  logic [63:0]     sl_d_addr_c  [NSLV];
  logic [2:0]      sl_d_size_c  [NSLV];
  logic [7:0]      sl_d_wstrb_c [NSLV];
  logic [63:0]     sl_d_wdata_c [NSLV];
  logic            sl_d_amo_c   [NSLV];
  logic [ID_W-1:0] sl_d_id_c    [NSLV];
  logic [3:0]      sl_d_nbytes_c [NSLV];

  always_comb begin
    for (int unsigned s = 0; s < NSLV; s++) begin
      sl_d_we_c[s]     = txn_we[sl_disp_idx_c[s]];
      sl_d_addr_c[s]   = txn_addr[sl_disp_idx_c[s]];
      sl_d_size_c[s]   = txn_size[sl_disp_idx_c[s]];
      sl_d_wstrb_c[s]  = txn_wstrb[sl_disp_idx_c[s]];
      sl_d_wdata_c[s]  = txn_wdata[sl_disp_idx_c[s]];
      sl_d_amo_c[s]    = txn_amo[sl_disp_idx_c[s]];
      sl_d_id_c[s]     = txn_id[sl_disp_idx_c[s]];
      sl_d_nbytes_c[s] = 4'd1 << sl_d_size_c[s];
    end
  end

  // ------------------------------------------------------- slave responses
  // Every response is a pure function of the dispatched request and the device's
  // current state; the side effects are applied at the same edge (below).
  logic [63:0] sl_rdata_c [NSLV];
  logic [1:0]  sl_err_c   [NSLV];
  logic        uart_rx_pop_c;
  logic        uart_tx_c;
  logic        clint_exit_c;
  logic        msip_write_c;

  // Register-relative byte offsets.
  logic [3:0] uart_rx_off_c, uart_status_off_c, uart_scratch_off_c;
  logic [3:0] clint_mtime_off_c, clint_mtimecmp_off_c, clint_exit_off_c;
  logic [3:0] harn_off_c, msip_off_c;

  logic uart_sel_rx_c, uart_sel_tx_c, uart_sel_status_c, uart_sel_scratch_c;
  logic clint_sel_mtime_c, clint_sel_mtimecmp_c, clint_sel_exit_c;
  logic harn_sel_id_c;
  logic uart_status_value_c;

  always_comb begin
    uart_rx_off_c      = sl_d_addr_c[SLV_UART][3:0]  - UART_RX[3:0];
    uart_status_off_c  = sl_d_addr_c[SLV_UART][3:0]  - UART_STATUS[3:0];
    uart_scratch_off_c = sl_d_addr_c[SLV_UART][3:0]  - UART_SCRATCH[3:0];
    clint_mtime_off_c    = sl_d_addr_c[SLV_CLINT][3:0] - CLINT_MTIME[3:0];
    clint_mtimecmp_off_c = sl_d_addr_c[SLV_CLINT][3:0] - CLINT_MTIMECMP[3:0];
    clint_exit_off_c     = sl_d_addr_c[SLV_CLINT][3:0] - CLINT_EXIT[3:0];
    harn_off_c           = sl_d_addr_c[SLV_HARNESS][3:0] - HARN_ID[3:0];
    msip_off_c           = sl_d_addr_c[SLV_MSIP][3:0];

    uart_sel_rx_c      = fits(sl_d_addr_c[SLV_UART],  sl_d_size_c[SLV_UART],
                              UART_BASE + UART_RX,      64'd4);
    uart_sel_tx_c      = fits(sl_d_addr_c[SLV_UART],  sl_d_size_c[SLV_UART],
                              UART_BASE + UART_TX,      64'd4);
    uart_sel_status_c  = fits(sl_d_addr_c[SLV_UART],  sl_d_size_c[SLV_UART],
                              UART_BASE + UART_STATUS,  64'd4);
    uart_sel_scratch_c = fits(sl_d_addr_c[SLV_UART],  sl_d_size_c[SLV_UART],
                              UART_BASE + UART_SCRATCH, 64'd4);
    clint_sel_mtime_c    = fits(sl_d_addr_c[SLV_CLINT], sl_d_size_c[SLV_CLINT],
                                CLINT_BASE + CLINT_MTIME,    64'd8);
    clint_sel_mtimecmp_c = fits(sl_d_addr_c[SLV_CLINT], sl_d_size_c[SLV_CLINT],
                                CLINT_BASE + CLINT_MTIMECMP, 64'd8);
    clint_sel_exit_c     = fits(sl_d_addr_c[SLV_CLINT], sl_d_size_c[SLV_CLINT],
                                CLINT_BASE + CLINT_EXIT,     64'd4);
    harn_sel_id_c        = fits(sl_d_addr_c[SLV_HARNESS], sl_d_size_c[SLV_HARNESS],
                                HARN_BASE + HARN_ID, 64'd4);

    uart_status_value_c = (rx_count_q != 4'd0);
    uart_rx_pop_c       = sl_disp_c[SLV_UART] && !sl_d_we_c[SLV_UART] && uart_sel_rx_c;
    uart_tx_c           = sl_disp_c[SLV_UART] &&  sl_d_we_c[SLV_UART] && uart_sel_tx_c;
    clint_exit_c        = sl_disp_c[SLV_CLINT] && sl_d_we_c[SLV_CLINT] && clint_sel_exit_c;
    msip_write_c        = sl_disp_c[SLV_MSIP] && sl_d_we_c[SLV_MSIP] && !sl_d_amo_c[SLV_MSIP];

    for (int unsigned s = 0; s < NSLV; s++) begin
      sl_rdata_c[s] = 64'd0;
      sl_err_c[s]   = ERR_OK;
    end

    // --- RAM: a registered read; a write acknowledges with zero data.
    sl_rdata_c[SLV_RAM] = (sl_disp_c[SLV_RAM] && !sl_d_we_c[SLV_RAM])
                          ? ram[sl_d_addr_c[SLV_RAM][3 +: RAM_IDX_W]]
                          : 64'd0;

    // --- ROM: read-only.
    if (sl_disp_c[SLV_ROM]) begin
      if (sl_d_we_c[SLV_ROM]) begin
        sl_err_c[SLV_ROM] = ERR_SLVERR;
      end else begin
        sl_rdata_c[SLV_ROM] = rom[sl_d_addr_c[SLV_ROM][3 +: ROM_IDX_W]];
      end
    end

    // --- UART.
    if (sl_disp_c[SLV_UART]) begin
      if (sl_d_amo_c[SLV_UART]) begin
        sl_err_c[SLV_UART] = ERR_SLVERR;     // no atomic access to a device register
      end else if (uart_sel_rx_c) begin
        if (sl_d_we_c[SLV_UART]) begin
          sl_err_c[SLV_UART] = ERR_SLVERR;
        end else begin
          sl_rdata_c[SLV_UART] = lane_extract({32'd0, rx_mem[rx_rd_q]},
                                              uart_rx_off_c, sl_d_size_c[SLV_UART],
                                              sl_d_addr_c[SLV_UART][2:0]);
        end
      end else if (uart_sel_tx_c) begin
        if (!sl_d_we_c[SLV_UART]) begin
          sl_err_c[SLV_UART] = ERR_SLVERR;
        end
      end else if (uart_sel_status_c) begin
        if (sl_d_we_c[SLV_UART]) begin
          sl_err_c[SLV_UART] = ERR_SLVERR;
        end else begin
          sl_rdata_c[SLV_UART] = lane_extract({31'b0, uart_status_value_c},
                                              uart_status_off_c, sl_d_size_c[SLV_UART],
                                              sl_d_addr_c[SLV_UART][2:0]);
        end
      end else if (uart_sel_scratch_c) begin
        if (!sl_d_we_c[SLV_UART]) begin
          sl_rdata_c[SLV_UART] = lane_extract({32'd0, uart_scratch_q},
                                              uart_scratch_off_c, sl_d_size_c[SLV_UART],
                                              sl_d_addr_c[SLV_UART][2:0]);
        end
      end else begin
        sl_err_c[SLV_UART] = ERR_SLVERR;     // mapped but unmodelled register
      end
    end

    // --- CLINT.
    if (sl_disp_c[SLV_CLINT]) begin
      if (sl_d_amo_c[SLV_CLINT]) begin
        sl_err_c[SLV_CLINT] = ERR_SLVERR;
      end else if (clint_sel_mtime_c) begin
        if (sl_d_we_c[SLV_CLINT]) begin
          sl_err_c[SLV_CLINT] = ERR_SLVERR;  // the timer counter is read-only
        end else begin
          sl_rdata_c[SLV_CLINT] = lane_extract(mtime_q, clint_mtime_off_c,
                                               sl_d_size_c[SLV_CLINT],
                                               sl_d_addr_c[SLV_CLINT][2:0]);
        end
      end else if (clint_sel_mtimecmp_c) begin
        if (!sl_d_we_c[SLV_CLINT]) begin
          sl_rdata_c[SLV_CLINT] = lane_extract(mtimecmp_q, clint_mtimecmp_off_c,
                                               sl_d_size_c[SLV_CLINT],
                                               sl_d_addr_c[SLV_CLINT][2:0]);
        end
      end else if (clint_sel_exit_c) begin
        if (!sl_d_we_c[SLV_CLINT]) begin
          sl_err_c[SLV_CLINT] = ERR_SLVERR;  // the test-end register is write only
        end
      end else begin
        sl_err_c[SLV_CLINT] = ERR_SLVERR;
      end
    end

    // --- test harness: one readable identity register, nothing writable.
    if (sl_disp_c[SLV_HARNESS]) begin
      if (!sl_d_we_c[SLV_HARNESS] && harn_sel_id_c) begin
        sl_rdata_c[SLV_HARNESS] = lane_extract({32'd0, HARN_ID_VALUE}, harn_off_c,
                                               sl_d_size_c[SLV_HARNESS],
                                               sl_d_addr_c[SLV_HARNESS][2:0]);
      end else begin
        sl_err_c[SLV_HARNESS] = ERR_SLVERR;
      end
    end

    // --- MSIP (p1). A 32-bit word: a read returns the pending bit.
    if (sl_disp_c[SLV_MSIP] && !msip_write_c) begin
      sl_rdata_c[SLV_MSIP] = lane_extract({32'd0, msip_q[0]}, msip_off_c,
                                          sl_d_size_c[SLV_MSIP],
                                          sl_d_addr_c[SLV_MSIP][2:0]);
    end

    if (sl_disp_c[SLV_MSIP] && sl_d_amo_c[SLV_MSIP]) begin
      sl_err_c[SLV_MSIP] = ERR_SLVERR;
    end
  end

  // ------------------------------------------------------------- delivery
  // One completion per requester per cycle, lowest table index first.
  logic             dlv_valid_c [2];
  logic [IDX_W-1:0] dlv_idx_c   [2];

  // The requester an entry's completion belongs to. The identity mutant swaps
  // this, so the completion is presented on the *other* requester's port.
`ifdef MOSAIC_SOC_MUTANT_ID_SWAP
  function automatic logic dlv_master(input logic m);
    dlv_master = ~m;
  endfunction
`else
  function automatic logic dlv_master(input logic m);
    dlv_master = m;
  endfunction
`endif

  always_comb begin
    for (int unsigned m = 0; m < 2; m++) begin
      dlv_valid_c[m] = 1'b0;
      dlv_idx_c[m]   = '0;
      for (int unsigned i = 0; i < MAX_OUTSTANDING; i++) begin
        if (!dlv_valid_c[m] && txn_valid[i] && txn_done[i] &&
            (dlv_master(txn_master[i]) == m[0])) begin
          dlv_valid_c[m] = 1'b1;
          dlv_idx_c[m]   = i[IDX_W-1:0];
        end
      end
    end
  end

  assign m0_rsp_valid_o = dlv_valid_c[0];
  assign m0_rsp_rdata_o = txn_rdata[dlv_idx_c[0]];
  assign m0_rsp_fault_o = (txn_err[dlv_idx_c[0]] != ERR_OK);
  assign m0_rsp_err_o   = txn_err[dlv_idx_c[0]];
  assign m0_rsp_id_o    = txn_id[dlv_idx_c[0]];

  assign m1_rsp_valid_o = dlv_valid_c[1];
  assign m1_rsp_rdata_o = txn_rdata[dlv_idx_c[1]];
  assign m1_rsp_fault_o = (txn_err[dlv_idx_c[1]] != ERR_OK);
  assign m1_rsp_err_o   = txn_err[dlv_idx_c[1]];
  assign m1_rsp_id_o    = txn_id[dlv_idx_c[1]];

  logic dlv_take0_c, dlv_take1_c;
  assign dlv_take0_c = dlv_valid_c[0] && m0_rsp_ready_i;
  assign dlv_take1_c = dlv_valid_c[1] && m1_rsp_ready_i;

  logic dlv_err0_c, dlv_err1_c;
  assign dlv_err0_c = (txn_err[dlv_idx_c[0]] != ERR_OK);
  assign dlv_err1_c = (txn_err[dlv_idx_c[1]] != ERR_OK);

  // ----------------------------------------------------------- the sequence
  always_ff @(posedge clk) begin
    if (rst) begin
      txn_valid        <= '0;
      txn_master       <= '0;
      txn_id           <= '0;
      txn_addr         <= '0;
      txn_we           <= '0;
      txn_size         <= '0;
      txn_wstrb        <= '0;
      txn_wdata        <= '0;
      txn_amo          <= '0;
      txn_slave        <= '0;
      txn_decoded      <= '0;
      txn_inflight     <= '0;
      txn_done         <= '0;
      txn_err          <= '0;
      txn_rdata        <= '0;
      sl_responding_q  <= '0;
      sl_entry_q       <= '0;
      sl_id_q          <= '0;
      sl_err_q         <= '0;
      sl_rdata_q       <= '0;
      accepted_q       <= 32'd0;
      completed_norm_q <= 32'd0;
      completed_err_q  <= 32'd0;
      outstanding_q    <= 32'd0;
      uart_tx_ctr_q    <= 32'd0;
      uart_rx_pop_ctr_q <= 32'd0;
      clint_exit_ctr_q <= 32'd0;
      dec_err_ctr_q    <= 32'd0;
      slv_err_ctr_q    <= 32'd0;
      id_mismatch_q    <= 1'b0;
      uart_scratch_q   <= 32'd0;
      rx_wr_q          <= 4'd0;
      rx_rd_q          <= 4'd0;
      rx_count_q       <= 4'd0;
      msip_q           <= 32'd0;
      mtime_q          <= 64'd0;
      mtimecmp_q       <= 64'hFFFFFFFFFFFFFFFF;
      exit_code_q      <= 32'd0;
      exit_valid_q     <= 1'b0;
      uart_tx_valid_q  <= 1'b0;
      uart_tx_data_q   <= 8'd0;
      rr_q             <= 1'b0;
      // No reset branch for `ram` / `rom`: storage without a reset, exactly as
      // rtl/common/mosaic_ram.sv. Only written/loaded entries are ever read.
    end else begin
      // ---------------------------------------------------------- accept
      if (accept_c) begin
        txn_valid[free_idx_c]    <= 1'b1;
        txn_master[free_idx_c]   <= accept_m0_c ? 1'b0 : 1'b1;
        txn_id[free_idx_c]       <= acc_id_c;
        txn_addr[free_idx_c]     <= acc_addr_c;
        txn_we[free_idx_c]       <= acc_we_c;
        txn_size[free_idx_c]     <= acc_size_c;
        txn_wstrb[free_idx_c]    <= acc_wstrb_c;
        txn_wdata[free_idx_c]    <= acc_wdata_c;
        txn_amo[free_idx_c]      <= acc_amo_c;
        txn_slave[free_idx_c]    <= dec_slv_c;
        txn_decoded[free_idx_c]  <= dec_hit_c;
        txn_inflight[free_idx_c] <= 1'b0;
        // An unmapped address is a DECERR completion that is ready immediately:
        // the error is produced, not waited for.
        txn_done[free_idx_c]     <= ~dec_hit_c;
        txn_err[free_idx_c]      <= dec_hit_c ? ERR_OK : ERR_DECERR;
        txn_rdata[free_idx_c]    <= 64'd0;
        accepted_q               <= accepted_q + 32'd1;
        outstanding_q            <= outstanding_q + 32'd1;
        if (m0_req_valid_i && m1_req_valid_i) rr_q <= ~rr_q;
      end

      // -------------------------------------------------------- dispatch
      // Each entry is dispatched at most once: `txn_inflight` is set here and
      // cleared only when the slave's response is captured. This is where "MMIO
      // side effects are not duplicated" is enforced.
      uart_tx_valid_q <= 1'b0;
      exit_valid_q    <= 1'b0;

      for (int unsigned s = 0; s < NSLV; s++) begin
        if (sl_disp_c[s]) begin
          sl_responding_q[s] <= 1'b1;
          sl_entry_q[s]      <= sl_disp_idx_c[s];
          sl_id_q[s]         <= sl_d_id_c[s];
          sl_err_q[s]        <= sl_err_c[s];
          sl_rdata_q[s]      <= sl_rdata_c[s];
          txn_inflight[sl_disp_idx_c[s]] <= 1'b1;
        end else begin
          sl_responding_q[s] <= 1'b0;
        end
      end

      // ---------------------------------------------------- slave effects
      // RAM. A write merges only the strobed lanes; that is what makes a narrow
      // store into a wide memory word a narrow store.
      if (sl_disp_c[SLV_RAM] && sl_d_we_c[SLV_RAM]) begin
`ifdef MOSAIC_SOC_MUTANT_STROBE_DROP
        for (int unsigned b = 0; b < 8; b++) begin
          ram[sl_d_addr_c[SLV_RAM][3 +: RAM_IDX_W]][8*b +: 8] <=
              sl_d_wdata_c[SLV_RAM][8*b +: 8];
        end
`else
        for (int unsigned b = 0; b < 8; b++) begin
          if (sl_d_wstrb_c[SLV_RAM][b]) begin
            ram[sl_d_addr_c[SLV_RAM][3 +: RAM_IDX_W]][8*b +: 8] <=
                sl_d_wdata_c[SLV_RAM][8*b +: 8];
          end
        end
`endif
      end

      // ROM load port.
      if (rom_load_en_i) begin
        rom[rom_load_index_i] <= rom_load_data_i;
      end

      // UART RX queue: push from the serial side, pop on an RX read dispatch.
      if (uart_rx_push_i && (rx_count_q != RX_DEPTH_V)) begin
        rx_mem[rx_wr_q] <= {24'd0, uart_rx_data_i};
        rx_wr_q         <= rx_wr_q + 4'd1;
      end
      if (uart_rx_pop_c) begin
        rx_rd_q           <= rx_rd_q + 4'd1;
        uart_rx_pop_ctr_q <= uart_rx_pop_ctr_q + 32'd1;
      end
      if (uart_rx_push_i && (rx_count_q != RX_DEPTH_V) && !uart_rx_pop_c) begin
        rx_count_q <= rx_count_q + 4'd1;
      end else if (!uart_rx_push_i && uart_rx_pop_c) begin
        rx_count_q <= rx_count_q - 4'd1;
      end
      // (a simultaneous push and pop leaves the count unchanged)

      // UART TX: one side effect per accepted access, the strobed bytes appended.
      if (uart_tx_c) begin
        uart_tx_valid_q <= 1'b1;
        uart_tx_ctr_q   <= uart_tx_ctr_q + 32'd1;
        for (int unsigned b = 0; b < 8; b++) begin
          if (sl_d_wstrb_c[SLV_UART][b]) begin
            uart_tx_data_q <= sl_d_wdata_c[SLV_UART][8*b +: 8];
          end
        end
      end

      // UART scratch and CLINT mtimecmp: byte-strobed register writes. The
      // register byte index is the offset inside the register window; the data
      // lane is the access's address lane -- the convention the MMIO models use,
      // so a byte store into a wide register touches exactly one byte.
      if (sl_disp_c[SLV_UART] && sl_d_we_c[SLV_UART] && uart_sel_scratch_c
          && !sl_d_amo_c[SLV_UART]) begin
        for (int unsigned i = 0; i < 8; i++) begin
          if (i[3:0] < sl_d_nbytes_c[SLV_UART]) begin
            if (sl_d_wstrb_c[SLV_UART][sl_d_addr_c[SLV_UART][2:0] + i[2:0]]) begin
              uart_scratch_q[(uart_scratch_off_c + i[3:0])*8 +: 8] <=
                  sl_d_wdata_c[SLV_UART][(sl_d_addr_c[SLV_UART][2:0] + i[2:0])*8 +: 8];
            end
          end
        end
      end

      if (sl_disp_c[SLV_CLINT] && sl_d_we_c[SLV_CLINT] && clint_sel_mtimecmp_c
          && !sl_d_amo_c[SLV_CLINT]) begin
        for (int unsigned i = 0; i < 8; i++) begin
          if (i[3:0] < sl_d_nbytes_c[SLV_CLINT]) begin
            if (sl_d_wstrb_c[SLV_CLINT][sl_d_addr_c[SLV_CLINT][2:0] + i[2:0]]) begin
              mtimecmp_q[(clint_mtimecmp_off_c + i[3:0])*8 +: 8] <=
                  sl_d_wdata_c[SLV_CLINT][(sl_d_addr_c[SLV_CLINT][2:0] + i[2:0])*8 +: 8];
            end
          end
        end
      end

      if (clint_exit_c) begin
        exit_valid_q     <= 1'b1;
        clint_exit_ctr_q <= clint_exit_ctr_q + 32'd1;
        for (int unsigned i = 0; i < 8; i++) begin
          if (i[3:0] < sl_d_nbytes_c[SLV_CLINT]) begin
            if (sl_d_wstrb_c[SLV_CLINT][sl_d_addr_c[SLV_CLINT][2:0] + i[2:0]]) begin
              exit_code_q[(clint_exit_off_c + i[3:0])*8 +: 8] <=
                  sl_d_wdata_c[SLV_CLINT][(sl_d_addr_c[SLV_CLINT][2:0] + i[2:0])*8 +: 8];
            end
          end
        end
      end

      if (msip_write_c) begin
        for (int unsigned i = 0; i < 8; i++) begin
          if (i[3:0] < sl_d_nbytes_c[SLV_MSIP]) begin
            if (sl_d_wstrb_c[SLV_MSIP][sl_d_addr_c[SLV_MSIP][2:0] + i[2:0]]) begin
              msip_q[(msip_off_c + i[3:0])*8 +: 8] <=
                  sl_d_wdata_c[SLV_MSIP][(sl_d_addr_c[SLV_MSIP][2:0] + i[2:0])*8 +: 8];
            end
          end
        end
      end

      // The free-running timer.
      mtime_q <= mtime_q + 64'd1;

      // ------------------------------------------------ slave completion
      // The slave's registered response is captured into its table entry. An
      // error response is *never* dropped: it is the completion.
      for (int unsigned s = 0; s < NSLV; s++) begin
        if (sl_responding_q[s]) begin
`ifdef MOSAIC_SOC_MUTANT_DROP_ERROR
          // NEGATIVE CONTROL 1 (the card's fail mode): an error completion is
          // dropped, so the request is accepted and never completes.
          if (sl_err_q[s] == ERR_OK) begin
`endif
            txn_done[sl_entry_q[s]]     <= 1'b1;
            txn_inflight[sl_entry_q[s]] <= 1'b0;
            txn_err[sl_entry_q[s]]      <= sl_err_q[s];
            txn_rdata[sl_entry_q[s]]    <= sl_rdata_q[s];
`ifdef MOSAIC_SOC_MUTANT_DROP_ERROR
          end
`endif
          if (sl_id_q[s] != txn_id[sl_entry_q[s]]) id_mismatch_q <= 1'b1;
        end
      end

      // ---------------------------------------------------------- deliver
      // A completion is consumed when the requester takes it; only then does
      // the entry free and the counter move. Delivery is atomic per entry.
      if (dlv_take0_c) begin
`ifndef MOSAIC_SOC_MUTANT_DUP_COMPLETE
        txn_valid[dlv_idx_c[0]] <= 1'b0;
`endif
        txn_done[dlv_idx_c[0]] <= 1'b0;
        if (dlv_err0_c) begin
          completed_err_q <= completed_err_q + 32'd1;
          slv_err_ctr_q   <= slv_err_ctr_q + 32'd1;
        end else begin
          completed_norm_q <= completed_norm_q + 32'd1;
        end
        outstanding_q <= outstanding_q - 32'd1;
      end

      if (dlv_take1_c) begin
`ifndef MOSAIC_SOC_MUTANT_DUP_COMPLETE
        txn_valid[dlv_idx_c[1]] <= 1'b0;
`endif
        txn_done[dlv_idx_c[1]] <= 1'b0;
        if (dlv_err1_c) begin
          completed_err_q <= completed_err_q + 32'd1;
          slv_err_ctr_q   <= slv_err_ctr_q + 32'd1;
        end else begin
          completed_norm_q <= completed_norm_q + 32'd1;
        end
        outstanding_q <= outstanding_q - 32'd1;
      end

      // A DECERR completion is ready the cycle it is accepted, so it is
      // delivered without ever reaching a slave. Count the class here.
      if (accept_c && !dec_hit_c) begin
        dec_err_ctr_q <= dec_err_ctr_q + 32'd1;
      end
    end
  end

  // The conservation identity. It is a property of the registered state, so it
  // is checked by the case *every cycle* rather than only at the end.
  assign o_conservation_ok =
      (accepted_q == (completed_norm_q + completed_err_q + outstanding_q));
  assign o_abort_pulse = rst && (outstanding_q != 32'd0);

  assign o_accepted_ctr         = accepted_q;
  assign o_completed_normal_ctr = completed_norm_q;
  assign o_completed_error_ctr  = completed_err_q;
  assign o_outstanding_ctr      = outstanding_q;
  assign o_id_mismatch          = id_mismatch_q;
  assign o_uart_rx_pop_ctr      = uart_rx_pop_ctr_q;
  assign o_uart_tx_ctr          = uart_tx_ctr_q;
  assign o_clint_exit_ctr       = clint_exit_ctr_q;
  assign o_dec_err_ctr          = dec_err_ctr_q;
  assign o_slv_err_ctr          = slv_err_ctr_q;

  // ------------------------------------------------------------ interrupts
  assign irq_timer_o = (mtime_q >= mtimecmp_q);
  assign irq_soft_o  = HAS_MSIP ? msip_q[0] : 1'b0;
  assign irq_ext_o   = (rx_count_q != 4'd0);

  assign exit_valid_o = exit_valid_q;
  assign exit_code_o  = exit_code_q;

  assign uart_rx_full_o  = (rx_count_q == RX_DEPTH_V);
  assign uart_tx_valid_o = uart_tx_valid_q;
  assign uart_tx_data_o  = uart_tx_data_q;

endmodule

`resetall
