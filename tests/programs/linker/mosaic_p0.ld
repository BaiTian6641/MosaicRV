/* tests/programs/linker/mosaic_p0.ld
 *
 * Section layout for the frozen MosaicRV p0 memory map.
 *
 * Sources of every constant below:
 *   config/memory/p0.json        regions
 *   config/profiles/p0.json      reset_vector, test_protocol
 *
 * WHY TWO CODE REGIONS
 *
 * The reset vector (0x80000000) and the signature area (0x80000400) are both
 * frozen, so only 1 KiB separates them.  Everything that must be reachable
 * at reset -- the reset path, the shared runtime helpers and the machine trap
 * handler -- is placed contiguously in that window, and it fits (the ASSERTs
 * below turn an overflow into a link error).  The per-program body is placed
 * after the firmware-owned RAM areas, where it has room to grow without ever
 * pushing the reset path off the reset vector.  Both regions are ordinary RAM
 * and both are inside the single LOAD segment, so the harness loader has
 * nothing extra to map.
 *
 * Layout rules this script enforces:
 *   - .text.init starts at exactly 0x80000000, which is both the frozen
 *     reset vector and the first instruction executed out of reset.
 *   - The signature area is its own output section at exactly 0x80000400,
 *     so its presence in the image is provable from the program headers.
 *   - .bss gets an explicit [start, end) region; nothing outside it is zeroed.
 *   - Every firmware-owned area is NOLOAD: it occupies address space and a
 *     memsz, but contributes no file bytes.
 *   - Nothing is ever placed in boot_rom (0x00000000, read+exec only).  A
 *     store there must raise a store access fault, and the corpus tests that.
 *
 * Address plan:
 *   0x80000000  .text.init   reset path + runtime helpers + trap handler
 *               .rodata      compiled-in program inputs
 *               .data
 *   0x80000400  .signature   frozen, 4 x 8 bytes
 *   0x80001000  .traplog     8 records x {mcause, mtval}
 *   0x80001080  .scratch     128-byte program buffer
 *   0x80001100  .bss         firmware-private scalar state
 *   0x80001200  .progtext    the corpus program's main()
 *   ...         .stack, .trapstack
 */

OUTPUT_FORMAT("elf64-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

/* ---- frozen map (config/memory/p0.json) ---- */
__boot_rom_base = 0x00000000;
__boot_rom_end  = 0x00001000;
__ram_base      = 0x80000000;
__ram_end       = 0x80200000;      /* ram base + 2 MiB */

/* ---- frozen test protocol (config/profiles/p0.json -> test_protocol) ---- */
__signature_base  = 0x80000400;
__signature_words = 4;

/* ---- firmware-owned areas ---- */
__traplog_base    = 0x80001000;
__traplog_size    = 128;           /* 8 records x 2 words */
__scratch_base    = 0x80001080;
__scratch_size    = 128;
__bss_base        = 0x80001100;
__progtext_base   = 0x80001200;

/* Absolute symbols at the frozen test_protocol addresses.  They describe
 * what the firmware already does and let a host that looks for the
 * conventional `tohost`/`fromhost` symbol names (Spike's HTIF, among
 * others) find the device without a side-channel.  Defining them changes no
 * address and no behaviour. */
/* Absolute symbols at the frozen test_protocol addresses (config/profiles/
 * p0.json -> tohost 0x80001000, fromhost 0x80001008).
 *
 * These are PLAIN assignments, not PROVIDE.  PROVIDE only materialises a
 * symbol when a relocation actually references it, and nothing in the
 * firmware references these, so PROVIDE leaves them out of the symbol table
 * and any host that looks for the conventional names cannot find them --
 * Spike then prints "tohost and fromhost symbols not in ELF; can't
 * communicate with target" and the run never terminates.  That was observed
 * and is why they are plain.
 *
 * The protection PROVIDE was meant to give -- "a stale address cannot survive
 * silently" -- is instead provided by a real check:
 * tests/programs/audit/check_p0_isa.py asserts that both symbols exist in
 * every ELF and that their values equal test_protocol.tohost / .fromhost as
 * read from config/profiles/p0.json.  A profile change that moves them fails
 * the audit; it does not pass unnoticed. */
tohost   = 0x80001000;
fromhost = 0x80001008;

SECTIONS
{
    /* ============ boot region: everything reachable at reset ============ */
    . = __ram_base;

    .text.init : ALIGN(4)
    {
        KEEP(*(.text.init))
        KEEP(*(.text.boot))
    }

    /* .rodata is deliberately NOT folded into .text.init.  Keeping it in its
     * own output section means the 8-byte alignment padding between them
     * belongs to neither section, so the executable image contains no zero
     * word that a fetch could land on and no padding the disassembly audit
     * would have to whitelist. */
    .rodata : ALIGN(8)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    .data : ALIGN(8)
    {
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.got .got.plt)
    }
    __boot_end = .;

    /* ============ frozen signature area ============ */
    . = __signature_base;
    .signature __signature_base (NOLOAD) : ALIGN(8)
    {
        __signature_start = .;
        KEEP(*(.signature))
        . = . + (__signature_words * 8);
        __signature_end = .;
    }

    /* ============ firmware-owned RAM areas ============ */
    . = __traplog_base;
    .traplog (NOLOAD) : ALIGN(8)
    {
        __traplog_start = .;
        KEEP(*(.traplog))
        . = . + __traplog_size;
        __traplog_end = .;
    }

    . = __scratch_base;
    .scratch (NOLOAD) : ALIGN(8)
    {
        __scratch_start = .;
        KEEP(*(.scratch))
        . = . + __scratch_size;
        __scratch_end = .;
    }

    . = __bss_base;
    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* ============ program region ============ */
    . = __progtext_base;
    .progtext : ALIGN(4)
    {
        *(.text .text.*)
    }

    .stack (NOLOAD) : ALIGN(16)
    {
        __stack_section_start = .;
        KEEP(*(.stack))
        __stack_section_end = .;
    }

    .trapstack (NOLOAD) : ALIGN(16)
    {
        __trapstack_section_start = .;
        KEEP(*(.trapstack))
        __trapstack_section_end = .;
    }

    __image_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.riscv.attributes.unlinked)
    }
}

/* .riscv.attributes is deliberately NOT discarded: it carries the ISA
 * string the audit target compares against rv64im_zicsr_zifencei. */

/* ---------------------------------------------------------------- */
/* Invariants.  A violation is a link error, not a silent surprise.  */
/* ---------------------------------------------------------------- */

ASSERT(__ram_base == 0x80000000,
       "reset vector must be 0x80000000 (config/profiles/p0.json)")
ASSERT(__signature_base == 0x80000400,
       "signature area must be 0x80000400 (config/profiles/p0.json test_protocol)")
ASSERT(__boot_end <= __signature_base,
       "boot region (.text.init/.rodata/.data) runs past 0x80000400")
ASSERT(__signature_end <= __traplog_base,
       "signature area must not overrun the trap log")
ASSERT(__traplog_end <= __scratch_base,
       "trap log must not overrun the scratch buffer")
ASSERT(__scratch_end <= __bss_base,
       "scratch buffer must not overrun .bss")
ASSERT(__bss_end <= __progtext_base,
       ".bss must not overrun the program code region")
ASSERT(__trapstack_section_end <= __ram_end,
       "image runs past the end of RAM (2 MiB)")
ASSERT(__image_end > __boot_rom_end,
       "image must live in RAM, never in boot_rom")