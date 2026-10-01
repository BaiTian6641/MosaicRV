// GENERATED FILE - do not edit.
// Produced by tools/gen_manifest.py --profile p0.
// The memory map and test protocol come from the frozen profile configuration.
#ifndef MOSAIC_PLATFORM_H_
#define MOSAIC_PLATFORM_H_

#include <stdint.h>

#define MOSAIC_PROFILE_NAME "p0"
#define MOSAIC_RESET_VECTOR UINT64_C(0x80000000)

// ---- physical memory map ----
#define MOSAIC_BOOT_ROM_BASE               UINT64_C(0x0000000000000000)
#define MOSAIC_BOOT_ROM_SIZE                  UINT64_C(0x0000000000001000)
#define MOSAIC_BOOT_ROM_CACHEABLE             0
#define MOSAIC_BOOT_ROM_ATOMIC_GRANULE        4
#define MOSAIC_UART_BASE                   UINT64_C(0x0000000000100000)
#define MOSAIC_UART_SIZE                      UINT64_C(0x0000000000000100)
#define MOSAIC_UART_CACHEABLE                 0
#define MOSAIC_UART_ATOMIC_GRANULE            4
#define MOSAIC_TEST_HARNESS_BASE           UINT64_C(0x0000000000102000)
#define MOSAIC_TEST_HARNESS_SIZE              UINT64_C(0x0000000000000010)
#define MOSAIC_TEST_HARNESS_CACHEABLE         0
#define MOSAIC_TEST_HARNESS_ATOMIC_GRANULE    4
#define MOSAIC_CLINT_BASE                  UINT64_C(0x0000000002000000)
#define MOSAIC_CLINT_SIZE                     UINT64_C(0x0000000000001000)
#define MOSAIC_CLINT_CACHEABLE                0
#define MOSAIC_CLINT_ATOMIC_GRANULE           4
#define MOSAIC_RAM_BASE                    UINT64_C(0x0000000080000000)
#define MOSAIC_RAM_SIZE                       UINT64_C(0x0000000000200000)
#define MOSAIC_RAM_CACHEABLE                  0
#define MOSAIC_RAM_ATOMIC_GRANULE             8

// ---- test protocol (must match tests/programs and rtl/soc) ----
#define MOSAIC_TOHOST          UINT64_C(0x102000)
#define MOSAIC_FROMHOST        UINT64_C(0x102008)
#define MOSAIC_SIGNATURE_ADDR  UINT64_C(0x80000400)
#define MOSAIC_SIGNATURE_WORDS 4
#define MOSAIC_TEST_PASS_CODE  UINT32_C(1)

#endif  // MOSAIC_PLATFORM_H_
