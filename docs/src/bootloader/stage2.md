# Stage 1

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/init/zxfl/stage2/`

---

## 1. Purpose

Stage 1 is the full production loader. It is a flat binary linked at `0x20000`, loaded there by Stage 0. It performs all hardware detection, kernel loading, integrity verification, page table construction, and the final transfer of control to the kernel.

---

## 2. Entry Point (`entry.S` — `stage2_entry`)

```
stage2_entry:
  1. Save schid from %r2 into a callee-saved register (%r13)
  2. Call zxfl_lowcore_setup() — install disabled-wait new PSWs
  3. SSM 0x00 — mask all interrupts off
  4. Clear BSS with MVCL (pad-fill mode, source length = 0)
  5. Set stack pointer to stage2_stack_top − 160
  6. Restore schid into %r2
  7. Call zxfl01_entry(schid)
```

`SSM 0x00` is issued immediately after `zxfl_lowcore_setup` installs safe new PSWs. Any interrupt that fires during the loader will hit a known disabled-wait rather than garbage.

BSS is cleared with `MVCL` in pad-fill mode (source length = 0, pad byte = 0x00). This is safe in 64-bit mode and faster than a byte loop for large BSS sections.

---

## 3. Main Function (`entry.c` — `zxfl01_entry`)

Execution order:

| Step | Action |
|------|--------|
| 1 | **STFLE** — store facility list into `proto.stfle_fac[]` |
| 2 | **CR setup** — clear I/O, external, machine-check masks in CR0; zero CR6 and CR14 |
| 3 | **Device probe** — `probe_ipl_device()`: ECKD Sense ID first, then FBA; populates `ipl_dev_type` and `ipl_dev_model` |
| 4 | **Parmfile** — read `ETC.ZXFOUNDATION.PARM`; parse `syssize=` |
| 5 | **Nucleus load** — `dasd_find_dataset_extents` + `zxfl_load_elf64` |
| 6 | **ZXVL** — structural lock check, handshake, SHA-256 segment checksums |
| 7 | **Memory probe** — write-pattern test at 1 MB granularity up to `syssize` or 512 MB |
| 8 | **Module loading** — load each `sysmodule=` dataset as a flat binary after the kernel image |
| 9 | **System detection** — `zxfl_system_detect`: STSI (manufacturer, model, LPAR), SIGP Sense (CPU map), STCK (TOD) |
| 10 | **Protocol finalization** — magic, version, binding token, stack canaries, CR snapshots |
| 11 | **MMU + jump** — `zxfl_mmu_setup_and_jump`: build page tables, translate pointers, `LPSWE` to kernel entry |

---

## 4. Linker Script (`stage2.ld`)

The binary is linked at `0x20000` as a flat ELF. The post-build step strips it to a raw binary with `objcopy -O binary`.

---

## 5. Stack

A 32 KB static array in BSS. The kernel receives a pointer to the top of this stack in `%r15` and in `proto->kernel_stack_top` (HHDM virtual). The kernel must switch to its own stack before consuming more than ~8 KB.

---

## 6. Shared Library (`common/`)

Stage 1 uses the full `common/` library:

| Module | Purpose |
|--------|---------|
| `dasd_io.c` | Low-level CCW I/O |
| `dasd_vtoc.c` | VTOC traversal |
| `dasd_eckd.c` | ECKD device driver |
| `dasd_fba.c` | FBA device driver |
| `dasd_tape.c` | Tape device driver |
| `elfload.c` | ELF64 segment loader |
| `mmu.c` | Bootloader page table builder |
| `lowcore.c` | Lowcore / new PSW setup |
| `zxvl_verify.c` | ZXVL integrity checks |
| `parmfile.c` | Parmfile parser |
| `stfle.c` | STFLE facility detection |
| `system.c` | STSI, SIGP Sense, STCK |
| `diag.c`, `ebcdic.c`, `panic.c`, `string.c` | Utilities |
