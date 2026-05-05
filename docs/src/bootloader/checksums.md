# Checksum Protocol

**Document Revision:** 26h1.0

---

## 1. Purpose

The checksum protocol ensures that the kernel image loaded into memory matches the image that was built and signed. It operates at two points:

| Point | Actor | Action |
|-------|-------|--------|
| Build time | `gen_checksums` | Compute SHA-256 per `PT_LOAD` segment; patch into `.zxvl_checksums` |
| Boot time (loader) | `zxvl_verify_nucleus_checksums` | Recompute and compare before DAT is enabled |
| Boot time (kernel) | `verify_kernel_checksums` | Recompute and compare from HHDM after DAT is enabled |

The double verification (loader + kernel) ensures that neither a compromised loader nor a post-load memory modification can go undetected.

---

## 2. Table Location

The checksum table is embedded in the kernel ELF at a fixed offset:

$$\texttt{table\_phys} = \texttt{load\_min} + \texttt{ZXVL\_CKSUM\_TABLE\_OFFSET}$$

where `ZXVL_CKSUM_TABLE_OFFSET = 0x80000`.

The table is placed in the `.zxvl_checksums` ELF section. The linker script must anchor this section at the correct virtual address.

---

## 3. Table Format

See [gen_checksums §3](../tools/gen_checksums.md#3-checksum-table-layout) for the full `zxvl_checksum_table_t` layout.

Key fields:

| Field | Value |
|-------|-------|
| `magic` | `0x5A58564C` ("ZXVL") |
| `version` | `0x00000001` |
| `algo` | `0x00000001` (SHA-256) |
| `count` | Number of verified segments |

---

## 4. Excluded Segments

The segment containing `.zxvl_checksums` itself is excluded from the checksum computation. Hashing the table while building it would be circular. `gen_checksums` identifies and skips this segment automatically.

---

## 5. Kernel Re-verification

After the kernel initializes the PMM and VMM, `verify_kernel_checksums` re-reads the table from the HHDM virtual address and recomputes SHA-256 for each segment. This catches:

- Memory corruption between loader verification and kernel execution.
- A loader that passed verification but then modified segments before the jump.

A mismatch at this stage calls `panic("sys: kernel segment checksum mismatch — image tampered")`.
