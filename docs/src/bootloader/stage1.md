# Stage 0

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/init/zxfl/stage1/`

---

## 1. Purpose

Stage 0 is the minimal IPL loader. It occupies the first record on the IPL DASD volume and is loaded by the channel subsystem into physical address `0x0`. Its sole responsibility is to locate Stage 1 (`CORE.ZXFOUNDATIONLOADER01.SYS`) in the VTOC, read it to `0x20000`, and jump to it.

---

## 2. Entry Point (`head.S`)

The channel subsystem loads the IPL record and executes the PSW at offset `0x0`. This PSW is a 31-bit ESA/390 PSW pointing to `stage1_entry`.

The entry sequence:

```
stage1_entry:
  1. SIGP SET ARCHITECTURE (order 0x12) → switch to z/Architecture
     Retry with "restore PSWs" flag if first attempt fails.
  2. SAM64 → enable 64-bit addressing mode
  3. Clear BSS (byte loop — MVCL is unsafe before architecture switch)
  4. Set stack pointer to stage1_stack_top − 160
  5. Load schid from lowcore offset 0xB8
  6. Call zxfl00_entry(schid)
  7. Disabled-wait PSW (fallback — zxfl00_entry is [[noreturn]])
```

The 160-byte stack offset is the standard z/Architecture register save area size.

---

## 3. Main Function (`entry.c` — `zxfl00_entry`)

Execution order:

1. `diag_setup()` — flush any partial DIAG 8 output line.
2. Print the Stage 0 banner via DIAG 8.
3. `dasd_find_dataset(schid, "CORE.ZXFOUNDATIONLOADER01.SYS", &ext)` — locate Stage 1 in the VTOC.
4. Read the dataset track-by-track into `0x20000` using `dasd_read_next`.
5. Sanity-check: verify the loaded image is not a disabled-wait PSW.
6. Jump to `0x20000` with `schid` in `%r2`.

---

## 4. Linker Script (`stage1.ld`)

| Section | Address | Notes |
|---------|---------|-------|
| `.text.ipl` | `0x0` | IPL PSW (8 bytes) |
| `.text` | `0x58` | Code (after lowcore reserved area) |
| `.bss` | after `.text` | Zero-initialized data |

An `ASSERT` in the linker script enforces that the entire stage fits within 12 KB. The build will fail if this limit is exceeded.

---

## 5. Stack

An 8 KB static array in BSS. The stack pointer is initialized to `stage1_stack_top − 160`.

---

## 6. Shared Library (`common/`)

Stage 0 uses a subset of the shared `common/` library:

| Module | Purpose |
|--------|---------|
| `dasd_io.c` | Low-level CCW I/O (SSCH/TSCH) |
| `dasd_vtoc.c` | VTOC traversal and dataset lookup |
| `diag.c` | DIAG 8 console output |
| `ebcdic.c` | EBCDIC ↔ ASCII conversion |
| `panic.c` | Disabled-wait on fatal error |
| `string.c` | Minimal `memcpy`, `memset`, `strcmp` |
