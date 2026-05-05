# gen_checksums

**Document Revision:** 26h1.0  
**Source:** `tools/gen_checksums.c`

---

## 1. Purpose

`gen_checksums` is a post-build host tool that computes SHA-256 digests for each `PT_LOAD` segment of the kernel ELF and patches them into the `.zxvl_checksums` section in-place.

```sh
gen_checksums <core.zxfoundation.nucleus>
```

The file is modified in place. It must be a valid ELF64 file with a `.zxvl_checksums` section.

---

## 2. Operation

1. Read and validate the ELF64 header (magic, `EI_CLASS = ELFCLASS64`).
2. Locate `.zxvl_checksums` by walking the section header table and the section name string table.
3. Collect all `PT_LOAD` program headers. Skip segments with `p_filesz = 0` and the segment containing `.zxvl_checksums` itself (hashing the table while building it would be circular).
4. For each remaining `PT_LOAD` segment, read `p_filesz` bytes from `p_offset` and compute SHA-256.
5. Build a `zxvl_checksum_table_t` with magic `0x5A58564C`, version 1, algorithm `ZXVL_CKSUM_ALGO_SHA256`, and one entry per segment. Physical addresses are computed by stripping `CONFIG_KERNEL_VIRT_OFFSET` from `p_paddr`.
6. Seek to the file offset of `.zxvl_checksums` and write the complete table in one `fwrite`.

---

## 3. Checksum Table Layout

```
zxvl_checksum_table_t (packed):
  uint32_t  magic;       // 0x5A58564C
  uint32_t  version;     // 0x00000001
  uint32_t  algo;        // 0x00000001 (SHA-256)
  uint32_t  count;       // number of entries
  entries[16]:
    uint64_t  phys_start  // physical address of segment
    uint64_t  size        // p_filesz
    uint8_t   digest[32]  // SHA-256
```

The table is located at `load_min + ZXVL_CKSUM_TABLE_OFFSET` (0x80000) in the loaded kernel. The bootloader reads it from physical memory after loading all ELF segments.

---

## 4. Kernel Requirements

The kernel must define a `.zxvl_checksums` section anchored at the correct virtual address:

```c
__attribute__((section(".zxvl_checksums")))
static volatile zxvl_checksum_table_t zxvl_cksum_table = { 0 };
```

The linker script must place `.zxvl_checksums` at:

$$va = \texttt{HHDM\_BASE} + \texttt{load\_min\_offset} + \texttt{0x80000}$$
