#!/bin/bash
sysaddr=$(hexdump -X bin65/coreboot.llvm.prg | tr -d '\n' | sed 's/.*5d  1e  9e \(.\{17\}\).*/\1/' | sed 's/3\([0-9]\) */\1/g')
echo Code starts at "$sysaddr"
sed 's/^\([[:space:]]\)jmp \$\(abcd\).*/\1jmp '"$sysaddr"'/g' src/c65toc64wrapper.asm > src/c65toc64wrapper-retargeted.asm

