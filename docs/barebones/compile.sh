#!/bin/sh

set -e

export CFLAGS="-ffreestanding -nostdlib -msoft-float -mno-vx -m64 -march=z196 -mtune=z196 -pipe -fno-stack-protector -mzarch -static-libgcc -O2 -g0"
export INC="I../../include/arch/s390x/init/zxfl"
export LDFLAGS="-nostdlib -static --no-dynamic-linker -ztext -zmax-page-size=0x1000 --no-pie -g -melf64_s390"

s390x-ibm-linux-gnu-gcc $CFLAGS -I$INC -isystem -std=c23 -c head64.S -o head64.o
s390x-ibm-linux-gnu-gcc $CFLAGS -I$INC -isystem -std=c23 -c main.c -o main.o
s390x-ibm-linux-gnu-gcc $CFLAGS -I$INC -isystem -std=c23 -c zxvl_cksum.c -o zxvl_cksum.o
s390x-ibm-linux-gnu-ld -T link.ld $LDFLAGS head64.o main.o zxvl_cksum.o -o core.zxfoundation.nucleus
./gen_checksums core.zxfoundation.nucleus
dasdload -z sysres.conf sysres.3390