# tests/programs/Makefile
#
# Bare-metal RV64IM_Zicsr_Zifencei test corpus for MosaicRV profile p0.
#
# Targets
#   all     build every (program, input) ELF and print the entry point and
#           segment map so the harness loader and this build provably agree
#   audit   disassemble every ELF and FAIL if any instruction outside
#           rv64im_zicsr_zifencei appears, or if the image links libc/libgcc
#   map     print entry point + segment map only
#   clean   remove build/
#
# The toolchain is pinned.  -march is never relaxed, there is no fallback
# march, and -nostdlib/-nostartfiles/-ffreestanding/-fno-builtin mean nothing
# from libc or libgcc can be pulled in behind our back.  If the resulting
# image needs a libgcc helper, that is a build failure to fix here, not
# something to work around.

PYTHON  ?= python3
CROSS   ?= riscv64-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf

# Frozen p0 ISA string.  Do not append extensions here without a profile
# change in config/profiles/p0.json first.
ARCHFLAGS := -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany

INCLUDES := -I src
# -mno-relax / --no-relax: keep the image free of linker-inserted
# gp-relative sequences so the disassembly audit sees exactly what we wrote.
CFLAGS := $(ARCHFLAGS) -nostdlib -nostartfiles -ffreestanding -fno-builtin \
          -fno-pic -mno-relax -fno-asynchronous-unwind-tables \
          -fno-stack-protector -fno-common $(INCLUDES) -Wall -Wa,--fatal-warnings

LDSCRIPT := linker/mosaic_p0.ld
LDFLAGS := $(ARCHFLAGS) -nostdlib -nostartfiles \
           -Wl,--no-relax -Wl,--build-id=none \
           -Wl,-T,$(LDSCRIPT) -Wl,--fatal-warnings -Wl,--no-warn-rwx-segments

SRCDIR  := src
BUILDDIR := build
OBJDIR  := $(BUILDDIR)/obj

# The program list and the per-ELF input values live in corpus.json, the
# single source of truth.  mkvars.py turns that into build/corpus.mk, which
# is then included: adding a (program, input) pair to corpus.json is the
# only edit needed to extend the build.  (GNU Make 3.81, the /usr/bin/make on
# this host, mishandles both `include $(shell ...)` and multi-line $(eval),
# hence the generated fragment.)
CORPUSMK := $(BUILDDIR)/corpus.mk

$(CORPUSMK): corpus.json mkvars.py
	@mkdir -p $(BUILDDIR)
	$(PYTHON) mkvars.py --emit-make > $@.tmp
	@mv $@.tmp $@

include $(CORPUSMK)

BOOT_OBJS := $(OBJDIR)/crt0.o $(OBJDIR)/trap.o

ELFS := $(foreach p,$(PROGRAMS),\
          $(foreach i,0 1 2,$(BUILDDIR)/$(p).i$(i).elf))

INPUT_SLOTS := 0 1 2 3

.PHONY: all audit map clean help
.DELETE_ON_ERROR:

all: $(ELFS) map

help:
	@echo "targets: all audit map clean"

# ---------------------------------------------------------------------
# Build rules
#
# The compiled-in program inputs reach the image through the PROGRAM object,
# not through shared boot code: each program emits its own three-word input
# table (see MOSAIC_PROGRAM_INPUTS in platform.h).  Program objects are
# therefore named <program>.i<n>.o and depend on corpus.json, so changing an
# input recompiles exactly the objects that embed it.  crt0.o and trap.o are
# input-independent and stay shared.
# ---------------------------------------------------------------------
$(OBJDIR)/%.i0.o: $(SRCDIR)/%.S $(SRCDIR)/platform.h corpus.json | $(OBJDIR)
	$(CC) $(CFLAGS) -DPROG_INPUT0=$(PROG_INPUT0) -DPROG_INPUT1=$(PROG_INPUT1) \
	    -DPROG_INPUT2=$(PROG_INPUT2) -c $< -o $@

$(OBJDIR)/%.i1.o: $(SRCDIR)/%.S $(SRCDIR)/platform.h corpus.json | $(OBJDIR)
	$(CC) $(CFLAGS) -DPROG_INPUT0=$(PROG_INPUT0) -DPROG_INPUT1=$(PROG_INPUT1) \
	    -DPROG_INPUT2=$(PROG_INPUT2) -c $< -o $@

$(OBJDIR)/%.i2.o: $(SRCDIR)/%.S $(SRCDIR)/platform.h corpus.json | $(OBJDIR)
	$(CC) $(CFLAGS) -DPROG_INPUT0=$(PROG_INPUT0) -DPROG_INPUT1=$(PROG_INPUT1) \
	    -DPROG_INPUT2=$(PROG_INPUT2) -c $< -o $@

$(OBJDIR)/crt0.o: $(SRCDIR)/crt0.S $(SRCDIR)/platform.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/trap.o: $(SRCDIR)/trap.S $(SRCDIR)/platform.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.i0.elf: $(OBJDIR)/%.i0.o $(BOOT_OBJS) $(LDSCRIPT) | $(BUILDDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJDIR)/$*.i0.o $(BOOT_OBJS)

$(BUILDDIR)/%.i1.elf: $(OBJDIR)/%.i1.o $(BOOT_OBJS) $(LDSCRIPT) | $(BUILDDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJDIR)/$*.i1.o $(BOOT_OBJS)

$(BUILDDIR)/%.i2.elf: $(OBJDIR)/%.i2.o $(BOOT_OBJS) $(LDSCRIPT) | $(BUILDDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJDIR)/$*.i2.o $(BOOT_OBJS)

$(BUILDDIR) $(OBJDIR):
	@mkdir -p $@

# ---------------------------------------------------------------------
# map: entry point + segment map, so the harness loader can be checked
# against this build without guessing.
# ---------------------------------------------------------------------
map: $(ELFS)
	@echo "=== MosaicRV p0 firmware: entry point and segment map ==="
	@echo "--- toolchain ---"
	@$(CC) --version | head -1
	@$(READELF) --version | head -1
	@echo "--- linker script: $(LDSCRIPT) ---"
	@$(CC) $(ARCHFLAGS) -nostdlib -nostartfiles -Wl,--verbose \
	    -T $(LDSCRIPT) -o /dev/null $(BOOT_OBJS) 2>&1 \
	    | grep -E '^(GNU ld|LOAD)' || true
	@for elf in $(ELFS); do \
	    echo "--- $$elf ---"; \
	    $(READELF) -h $$elf | grep -E 'Entry point'; \
	    $(READELF) -lW $$elf | grep -E '^  (Type|LOAD)'; \
	    $(READELF) -SW $$elf | grep -E '\.(text|data|bss|signature|traplog|input|scratch|stack|trapstack)\b'; \
	done

# ---------------------------------------------------------------------
# audit: disassemble everything and fail on anything outside p0.
# ---------------------------------------------------------------------
audit: $(ELFS)
	@$(PYTHON) audit/check_p0_isa.py --objdump $(OBJDUMP) --readelf $(READELF) \
	    --arch 'rv64i2p1_m2p0_zicsr2p0_zifencei2p0_zmmul1p0' $(ELFS)

clean:
	rm -rf $(BUILDDIR)