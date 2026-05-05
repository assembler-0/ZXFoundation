# Build Targets

**Document Revision:** 26h1.0

---

## `tools`

Builds host-native `bin2rec` and `gen_checksums` using `ZX_HOST_CC`. This target is an implicit dependency of all other targets — it always runs first.

---

## `zxfl_stage1.elf` → `core.zxfoundationloader00.sys`

Compiles Stage 0. Post-build steps:

1. `objcopy -O binary zxfl_stage1.elf zxfl_stage1.bin` — strip ELF headers to raw binary.
2. `bin2rec zxfl_stage1.bin core.zxfoundationloader00.sys` — wrap in DASD IPL record format.

The linker script `stage1.ld` enforces a 12 KB size limit with `ASSERT`. The build fails if this limit is exceeded.

---

## `zxfl_stage2.elf` → `core.zxfoundationloader01.sys`

Compiles Stage 1. Post-build step:

1. `objcopy -O binary zxfl_stage2.elf core.zxfoundationloader01.sys` — flat binary at `0x20000`.

---

## `core.zxfoundation.nucleus`

Compiles the kernel. Post-build step:

1. `gen_checksums core.zxfoundation.nucleus` — computes SHA-256 for each `PT_LOAD` segment and patches the digests into the `.zxvl_checksums` ELF section in-place.

The kernel linker script is `arch/s390x/init/link.ld`.

---

## `dasd` → `sysres.3390`

Requires `dasdload` (from the Hercules package) on `PATH`.

1. Remove any existing `sysres.3390`.
2. Copy `scripts/etc.zxfoundation.parm` to the build directory.
3. Run `dasdload -z scripts/sysres.conf sysres.3390` — create a 3390 DASD image and write all datasets.
4. Copy `scripts/hercules.cnf` to the build directory.

`sysres.conf` defines the dataset layout: Stage 0, Stage 1, nucleus, and parmfile.

---

## Running

```sh
cmake --build build --target dasd
hercules -f build/hercules.cnf
```

In the Hercules console:

```
ipl 0200
```

`0200` is the subchannel address of the IPL device as defined in `hercules.cnf`.
