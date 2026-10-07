COPT_M65=       -Iinclude       -Isrc/mega65 -Isrc/mega65-libc/include

COMPILER=llvm
COMPILER_PATH=/usr/local/llvm-mos/bin
CC=   $(COMPILER_PATH)/mos-c64-clang -mcpu=mos45gs02 -Iinclude -Isrc/mega65 -Isrc/mega65-libc/include -DLLVM -fno-unroll-loops -ffunction-sections -fdata-sections -mllvm -inline-threshold=0 -fvisibility=hidden -Oz -Wall -Wextra -Wtype-limits

# Uncomment to include stacktraces on calls to fail()
CC+=    -g -finstrument-functions -DWITH_BACKTRACE

LD=   $(COMPILER_PATH)/ld.lld
CL=   $(COMPILER_PATH)/mos-c64-clang -DLLVM -mcpu=mos45gs02
HELPERS=        src/helper-llvm.c

LDFLAGS += -Wl,-T,src/asserts.ld
# Produce reproducer tar when required for assisting with debugging
LDFLAGS += -Wl,--reproduce=repro.tar

M65LIBC_INC=-I $(SRCDIR)/mega65-libc/include
M65LIBC_SRCS=$(wildcard $(SRCDIR)/mega65-libc/src/*.c) $(wildcard $(SRCDIR)/mega65-libc/src/$(COMPILER)/*.c) $(wildcard $(SRCDIR)/mega65-libc/src/$(COMPILER)/*.s)
CL65+=-I include $(M65LIBC_INC)

SRC_MEGA65_LIBC_LLVM=	src/mega65-libc/src/shres.c \
			src/mega65-libc/src/llvm/shres_asm.s \
			src/mega65-libc/src/memory.c \
			src/mega65-libc/src/llvm/memory_asm.s \
			src/mega65-libc/src/llvm/fileio.s \
			src/mega65-libc/src/hal.c \
			src/mega65-libc/src/conio.c

HELPER_SRCS=src/helper-llvm.s src/mega65/hal.c src/mega65/hal_asm_llvm.s src/uart.c

C1541 = flatpak run --command=c1541 net.sf.VICE


coreboot.d81: bin65/coreboot
	$(C1541) -format "coreboot,agi" d81 coreboot.d81
	$(C1541) -attach coreboot.d81 -write bin65/coreboot coreboot,p
	$(C1541) -attach coreboot.d81 -write coreconfig "core config,s"

bin65/coreboot.llvm.prg: src/main.c src/ascii-font.c src/c65reboot.h
	mkdir -p bin65
	rm -f src/mega65/function_table.c
	echo "struct function_table function_table[]={}; const unsigned int function_table_count=0; const unsigned char __wp_regs[9];" > src/mega65/function_table.c
	$(CC) -o bin65/coreboot.llvm.prg -Iinclude -DMEGA65 -Isrc/mega65-libc/include $< $(HELPER_SRCS) $(SRC_MEGA65_LIBC_LLVM) $(LDFLAGS)  -Wl,-Map,bin65/coreboot.map
	tools/function_table.py bin65/coreboot.map src/mega65/function_table.c
	$(CC) -o bin65/coreboot.llvm.prg -Iinclude -DMEGA65 -Isrc/mega65-libc/include $< $(HELPER_SRCS) $(SRC_MEGA65_LIBC_LLVM) $(LDFLAGS) -Wl,-Map,bin65/coreboot.map
	$(COMPILER_PATH)/llvm-objdump -drS --print-imm-hex bin65/coreboot.llvm.prg.elf >bin65/coreboot.llvm.dump

bin65/coreboot:   bin65/coreboot.llvm.prg src/ascii-font.c
	m65wrap -o $@ $<

src/ascii-font.c:	tools/make-ascii-font-c.sh asciifont.bin
	tools/make-ascii-font-c.sh

bin65/c65reboot.bin: src/c65reboot.asm
	mkdir -p bin65
	acme --setpc 0x0380 --cpu m65 --format plain --outfile bin65/c65reboot.bin src/c65reboot.asm

src/c65reboot.h: bin65/c65reboot.bin
	bin2c -H src/c65reboot.h bin65/c65reboot.bin

clean:
	rm -rf bin65
	rm -f src/c65toc64wrapper-retargeted.asm
	rm -f src/c65reboot.h
	rm -f src/ascii-font.c
	rm -f src/mega65/function_table.c
	rm -f repro.tar
	rm -f *.d81

ftp: coreboot.d81
	mega65_ftp -e -c "cd C65" -c "put coreboot.d81"
	etherload -5

