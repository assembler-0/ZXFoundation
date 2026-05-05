# ELF64 Loader

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/init/zxfl/common/elfload.c`

---

## 1. Overview

`zxfl_load_elf64` loads the kernel ELF64 image from DASD into physical memory. It processes only `PT_LOAD` program headers; all other segment types are ignored.

---

## 2. Load Sequence

```
zxfl_load_elf64(schid, dataset_name, load_base_out)
  │
  ├─ Read ELF header (first 64 bytes)
  ├─ Validate: magic 0x7F 'E' 'L' 'F', EI_CLASS=2 (64-bit),
  │            EI_DATA=2 (big-endian), e_machine=0x16 (s390)
  ├─ Read program header table (e_phoff, e_phnum entries)
  ├─ For each PT_LOAD segment:
  │    ├─ Compute physical load address:
  │    │    pa = p_paddr − CONFIG_KERNEL_VIRT_OFFSET
  │    ├─ Read p_filesz bytes from file offset p_offset → pa
  │    └─ Zero-fill [pa + p_filesz, pa + p_memsz)
  └─ Return load_min (lowest p_paddr seen, stripped of HHDM offset)
```

---

## 3. Address Computation

The kernel is linked with virtual addresses in the HHDM range (`p_vaddr ≥ 0xFFFF800000000000`). The physical load address is derived by subtracting `CONFIG_KERNEL_VIRT_OFFSET`:

$$pa = p\_paddr - \texttt{CONFIG\_KERNEL\_VIRT\_OFFSET}$$

The loader does not use `p_vaddr` directly; it uses `p_paddr` to avoid ambiguity when the linker script sets `AT()` addresses.

---

## 4. Constraints

- The kernel ELF must be `ET_EXEC` (executable, not shared object).
- `e_machine` must be `0x16` (EM_S390). Any other value causes an immediate panic.
- All `PT_LOAD` segments must have `p_paddr ≥ CONFIG_KERNEL_VIRT_OFFSET`. A segment below the HHDM offset is rejected.
- The kernel entry point (`e_entry`) must be ≥ `0xFFFF800000040000` (HHDM + 256 KB). The loader enforces this before the final jump.
- The total loaded image (all PT_LOAD segments) must fit within the memory probed by the write-pattern test.

---

## 5. BSS Zeroing

Segments where `p_memsz > p_filesz` have a BSS tail. The loader zeros this region with `memset` immediately after reading the file data. This ensures the kernel's BSS is clean before any ZXVL verification.
