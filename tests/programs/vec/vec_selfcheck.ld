/* tests/programs/vec/vec_selfcheck.ld
 *
 * Section layout for the self-checking RVV programs in this directory.
 *
 * These programs are NOT the p01..p13 scalar firmware corpus.  They are not
 * listed in tests/programs/corpus.json, they are not built by
 * `make -C tests/programs all`, and they do not touch tests/programs/golden.json
 * or tools/host_oracle.py (which models no vector state at all).  Their reason
 * for existing is stated in README.md beside this file.
 *
 * The reset vector is the frozen one (0x80000000, config/profiles/p0.json).
 * Unlike the corpus linker script, there is no frozen four-word signature area
 * here: these programs publish their verdict through the frozen TOHOST word
 * only, and carry their expected values in .rodata.  That lets the whole image
 * -- text, constant inputs, expected results, the result buffer and the stack --
 * live below the TOHOST/FROMHOST device window at 0x80001000.
 *
 * Address plan:
 *   0x80000000  .text     reset path + helpers + the program
 *               .rodata   constant inputs and hand-derived expected results
 *               .data     (unused, reserved)
 *               .bss      vec_status + result buffers
 *               .stack    1 KiB stack
 *   0x80001000  TOHOST / FROMHOST device window (never mapped by this image)
 *
 * The one ASSERT that matters: the image must not reach the device window.
 */

OUTPUT_FORMAT("elf64-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

__ram_base = 0x80000000;
__tohost   = 0x80001000;

/* Conventional HTIF names, so a host that looks for them finds them.  They
 * are plain assignments (not PROVIDE) for the same reason the corpus linker
 * script uses plain assignments. */
tohost   = 0x80001000;
fromhost = 0x80001008;

SECTIONS
{
    . = __ram_base;

    .text : ALIGN(4)
    {
        KEEP(*(.text.init))
        *(.text .text.*)
    }

    .rodata : ALIGN(8)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    .data : ALIGN(8)
    {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    .stack (NOLOAD) : ALIGN(16)
    {
        KEEP(*(.stack))
    }

    __image_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

ASSERT(__ram_base == 0x80000000,
       "reset vector must be 0x80000000 (config/profiles/p0.json)")
ASSERT(__image_end <= __tohost,
       "vec image reaches the TOHOST/FROMHOST device window at 0x80001000")
