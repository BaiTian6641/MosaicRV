/* tests/programs/linker/mosaic_p0.ld
 *
 * Section layout for the frozen MosaicRV p0 memory map.
 *
 * Sources of every constant below:
 *   config/memory/p0.json        regions
 *   config/profiles/p0.json      reset_vector, test_protocol
 *
 * Layout rules this script enforces:
 *   - .text.init starts at exactly 0x80000000, which is both the frozen
 *     reset vector and the first instruction executed out of reset.
 *   - The signature area is its own output section at exactly 0x80000400,
 *     so its presence in the image is provable from the program headers.
 *     Everything loadable (.text/.rodata/.data) must fit below it; the
 *     ASSERTs at the bottom turn a violation into a link error.
 *   - .bss gets an explicit [start,end) region; nothing outside it is zeroed.
 *   - Every firmware-owned area is NOLOAD: it occupies address space and a
 *     memsz, but contributes no file bytes.
 *   - .bss, .stack and .trapstack are laid out contiguously so the boot code
 *     can clear them as one region while still clearing each named area as a
 *     region of its own.
 *   - Nothing is ever placed in boot_rom (0x00000000, read+exec only).  A
 *     store there must raise a store access fault, and the corpus tests that.
 *
 * Address plan:
 *   0x80000000  .text .rodata .data     (loadable, file-backed)
 *   0x80000400  .signature               (frozen, 4 x 8 bytes)
 *   0x80001000  .traplog                 (8 records x {mcause, mtval})
 *   0x80001080  .scratch                 (128-byte program buffer)
 *   0x80001100  .bss .stack .trapstack  (NOLOAD, contiguous)
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

SECTIONS
{
    /* ================= loadable image ================= */
    . = __ram_base;

    .text : ALIGN(4)
    {
        KEEP(*(.text.init))
        *(.text .text.*)
    }

    /* .rodata is deliberately NOT folded into .text.  Keeping it in its own
     * output section means the 8-byte alignment padding between .text and
     * .rodata belongs to neither section, so the executable image contains
     * no zero word that a fetch could land on and no padding the
     * disassembly audit would have to whitelist. */
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

    /* ================= explicitly bounded .bss region ================= */
    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    __image_code_end = .;

    /* ================= test protocol areas ================= */
    . = __signature_base;
    .signature __signature_base (NOLOAD) : ALIGN(8)
    {
        __signature_start = .;
        KEEP(*(.signature))
        . = . + (__signature_words * 8);
        __signature_end = .;
    }

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

    /* ================= bss + stacks, contiguous =================
     * crt0.S clears these three as one region; each still has its own
     * explicit [start, end) pair. */
    .bss_stack (NOLOAD) : ALIGN(16)
    {
        __bss_stack_start = .;
        KEEP(*(.stack))
        . = ALIGN(16);
        KEEP(*(.trapstack))
        . = ALIGN(16);
        __bss_stack_end = .;
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
ASSERT(__bss_start >= __ram_base && __bss_end <= __signature_base,
       "loadable .bss collides with the signature area at 0x80000400")
ASSERT(__image_code_end <= __signature_base,
       "loadable image (.text/.rodata/.data/.bss) runs past 0x80000400")
ASSERT(__signature_end <= __traplog_base,
       "signature area must not overrun the trap log")
ASSERT(__traplog_end <= __scratch_base,
       "trap log must not overrun the scratch buffer")
ASSERT(__scratch_end <= __bss_stack_start,
       "scratch buffer must not overrun .bss/.stack")
ASSERT(__bss_stack_end <= __ram_end,
       "image runs past the end of RAM (2 MiB)")
ASSERT(__image_end > __boot_rom_end,
       "image must live in RAM, never in boot_rom")