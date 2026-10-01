// GENERATED FILE - do not edit.
// Produced by tools/gen_manifest.py --profile p0
// Source of truth: config/profiles/p0.json and everything it references.
// Any edit here is lost on the next build and will not match the manifest.

package mosaic_cfg_pkg;

  localparam int unsigned MOSAIC_PROFILE_INDEX = 0;
  localparam int unsigned MOSAIC_XLEN           = 64;
  localparam int unsigned MOSAIC_HARTS          = 1;
  localparam int unsigned MOSAIC_CLUSTERS       = 2;
  localparam int unsigned MOSAIC_ARCH_INT_REGS  = 32;

  // Derived from the advertised capability list, not from the claimed list:
  // an extension whose implementation has not delivered is not in misa.
  localparam logic [63:0] MOSAIC_MISA_RESET = 64'h8000000000000000;
  localparam logic [63:0] MOSAIC_RESET_VECTOR = 64'h0000000080000000;

  // Frontend
  localparam int unsigned MOSAIC_FETCH_OUTSTANDING = 4;
  localparam int unsigned MOSAIC_BTB_ENTRIES       = 64;
  localparam int unsigned MOSAIC_BPU_ENTRIES       = 512;
  localparam int unsigned MOSAIC_RAS_ENTRIES       = 16;
  localparam int unsigned MOSAIC_TARGET_QUANTUM    = 32;

  // Rename / commit
  localparam int unsigned MOSAIC_RENAME_WIDTH = 2;
  localparam int unsigned MOSAIC_DISPATCH_WIDTH = 2;
  localparam int unsigned MOSAIC_RETIRE_WIDTH = 2;
  localparam int unsigned MOSAIC_ROB_ENTRIES = 64;
  localparam int unsigned MOSAIC_ROB_INDEX_W = $clog2(64);
  localparam int unsigned MOSAIC_MAX_UOPS_PER_MACRO = 8;

  // Integer PRF: allocated from a free list, so the depth need not be a
  // power of two; only the bank decode has to be unambiguous.
  localparam int unsigned MOSAIC_INT_PRF_ENTRIES = 96;
  localparam int unsigned MOSAIC_INT_PRF_TAG_W   = $clog2(96);
  localparam int unsigned MOSAIC_PRF_BANKS       = 4;
  localparam int unsigned MOSAIC_PRF_BANK_W      = $clog2(4);

  // Execution fabric
  localparam int unsigned MOSAIC_IQ_ENTRIES      = 8;
  localparam int unsigned MOSAIC_ALU_PER_CLUSTER = 1;
  localparam int unsigned MOSAIC_MULDIV_UNITS    = 1;
  localparam int unsigned MOSAIC_RESULT_FIFO     = 2;
  localparam int unsigned MOSAIC_REMOTE_LATENCY  = 4;

  // LSU
  localparam int unsigned MOSAIC_LSU_UNITS  = 1;
  localparam int unsigned MOSAIC_LQ_ENTRIES = 8;
  localparam int unsigned MOSAIC_SQ_ENTRIES = 8;

  // Physical memory map
  localparam logic [63:0] MOSAIC_BOOT_ROM_BASE               = 64'h0000000000000000;
  localparam logic [63:0] MOSAIC_BOOT_ROM_SIZE               = 64'h0000000000001000;
  localparam logic [63:0] MOSAIC_UART_BASE                   = 64'h0000000000100000;
  localparam logic [63:0] MOSAIC_UART_SIZE                   = 64'h0000000000000100;
  localparam logic [63:0] MOSAIC_TEST_HARNESS_BASE           = 64'h0000000000102000;
  localparam logic [63:0] MOSAIC_TEST_HARNESS_SIZE           = 64'h0000000000000010;
  localparam logic [63:0] MOSAIC_CLINT_BASE                  = 64'h0000000002000000;
  localparam logic [63:0] MOSAIC_CLINT_SIZE                  = 64'h0000000000001000;
  localparam logic [63:0] MOSAIC_RAM_BASE                    = 64'h0000000080000000;
  localparam logic [63:0] MOSAIC_RAM_SIZE                    = 64'h0000000000200000;

endpackage : mosaic_cfg_pkg
