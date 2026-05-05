# Boot Protocol

**Document Revision:** 26h1.0  
**Protocol version:** `ZXFL_VERSION_4` (`0x00000004`)

---

## 1. Overview

The kernel receives a pointer to `zxfl_boot_protocol_t` in `%r2` at entry. All pointer fields are HHDM virtual addresses. The struct is version 4.

The kernel **must** validate `proto->magic == ZXFL_MAGIC` (`0x5A58464C`, "ZXFL") before using any other field. A mismatch indicates the wrong value is in `%r2` or the loader did not complete correctly.

---

## 2. Header Fields

| Field | Type | Value / Description |
|-------|------|---------------------|
| `magic` | `u32` | `0x5A58464C` ("ZXFL") |
| `version` | `u32` | `0x00000004` |
| `flags` | `u32` | Bitmask of `ZXFL_FLAG_*` (see §8) |
| `binding_token` | `u64` | `ZXVL_SEED ^ stfle_fac[0] ^ ipl_schid` |

---

## 3. Loader Identity

| Field | Type | Description |
|-------|------|-------------|
| `loader_major` | `u16` | Major version (1) |
| `loader_minor` | `u16` | Minor version (0) |
| `loader_timestamp` | `u32` | Build time encoded as `HHMMSSZx` |

---

## 4. IPL Device

| Field | Type | Description |
|-------|------|-------------|
| `ipl_schid` | `u32` | Subchannel ID of the IPL device |
| `ipl_dev_type` | `u16` | Device type from Sense ID (e.g. `0x3390`) |
| `ipl_dev_model` | `u16` | Device model from Sense ID |

---

## 5. Kernel Image

| Field | Type | Description |
|-------|------|-------------|
| `kernel_phys_start` | `u64` | Physical base of loaded kernel |
| `kernel_phys_end` | `u64` | Physical end (exclusive), after modules |
| `kernel_entry` | `u64` | ELF entry point (HHDM virtual) |

---

## 6. Memory Map

| Field | Type | Description |
|-------|------|-------------|
| `mem_total_bytes` | `u64` | Total usable + kernel RAM |
| `mem_map_addr` | `u64` | HHDM virtual address of `zxfl_mem_region_t[]` |
| `mem_map_count` | `u32` | Number of valid entries |

Memory region types:

| Constant | Value | Meaning |
|----------|-------|---------|
| `ZXFL_MEM_USABLE` | 1 | Free for kernel use |
| `ZXFL_MEM_RESERVED` | 2 | Hardware-reserved |
| `ZXFL_MEM_LOADER` | 3 | Loader code/data (reclaimable after init) |
| `ZXFL_MEM_KERNEL` | 4 | Kernel image and modules |

---

## 7. Page Table Pool

| Field | Type | Description |
|-------|------|-------------|
| `pgtbl_pool_end` | `u64` | Physical end of bootloader page-table bump pool |

Pool base is the first 1 MB-aligned address after `kernel_phys_end`, floored at 32 MB. The kernel PMM must mark `[pool_base, pgtbl_pool_end)` as reserved.

---

## 8. Kernel Stack

| Field | Type | Description |
|-------|------|-------------|
| `kernel_stack_top` | `u64` | HHDM virtual address of initial stack top (32 KB) |

The kernel should switch to its own stack as early as possible and treat this region as reserved.

---

## 9. Control Register Snapshots

| Field | Type | Description |
|-------|------|-------------|
| `cr0_snapshot` | `u64` | CR0 at time of kernel jump |
| `cr1_snapshot` | `u64` | CR1 (ASCE) at time of jump |
| `cr14_snapshot` | `u64` | CR14 at time of jump |

---

## 10. SMP / CPU Map

| Field | Type | Description |
|-------|------|-------------|
| `cpu_map[]` | `zxfl_cpu_info_t[64]` | Up to 64 CPU entries |
| `cpu_count` | `u32` | Valid entries in `cpu_map` |
| `bsp_cpu_addr` | `u16` | CPU address of the boot processor |

Each `zxfl_cpu_info_t`:

| Field | Description |
|-------|-------------|
| `cpu_addr` | CPU address (0–65535) |
| `type` | `ZXFL_CPU_TYPE_*` |
| `state` | `ZXFL_CPU_ONLINE` or `ZXFL_CPU_STOPPED` |

Valid when `ZXFL_FLAG_SMP` is set.

---

## 11. System Identification

Populated from STSI when `ZXFL_FLAG_SYSINFO` is set:

| Field | Description |
|-------|-------------|
| `manufacturer[16]` | ASCII, e.g. `"IBM"` |
| `type[4]` | Machine type, e.g. `"2964"` |
| `model[16]` | Model identifier |
| `sequence[16]` | Machine serial number |
| `plant[4]` | Manufacturing plant code |
| `lpar_name[8]` | LPAR name (STSI 2.2.2); empty on bare metal |
| `lpar_number` | LPAR number |
| `cpus_total` | Total CPUs in CEC |
| `cpus_configured` | Configured CPUs |
| `cpus_standby` | Standby CPUs |
| `capability` | CPU capability rating |

---

## 12. Modules

Up to 16 modules loaded from `sysmodule=` parmfile entries:

| Field | Description |
|-------|-------------|
| `modules[i].name[32]` | Dataset name (NUL-terminated) |
| `modules[i].phys_start` | Physical load address |
| `modules[i].size_bytes` | Size in bytes |

---

## 13. Flags

| Flag | Bit | Meaning |
|------|-----|---------|
| `ZXFL_FLAG_SMP` | 0 | `cpu_map[]` is valid |
| `ZXFL_FLAG_MEM_MAP` | 1 | `mem_map` is valid |
| `ZXFL_FLAG_CMDLINE` | 2 | `cmdline_addr` is valid |
| `ZXFL_FLAG_LOWCORE` | 3 | `lowcore_phys` is valid |
| `ZXFL_FLAG_STFLE` | 4 | `stfle_fac[]` is valid |
| `ZXFL_FLAG_SYSINFO` | 5 | `sysinfo` is valid |
| `ZXFL_FLAG_TOD` | 6 | `tod_boot` is valid |

---

## 14. Binding Token

The binding token ties the boot session to the specific hardware and IPL device:

$$\texttt{binding\_token} = \texttt{ZXVL\_SEED} \oplus \texttt{stfle\_fac[0]} \oplus \texttt{ipl\_schid}$$

The kernel must recompute this value and compare it to `proto->binding_token`. A mismatch means the protocol was tampered with or the kernel is running on unexpected hardware.

The binding token is also used as a component of the ZXVL handshake nonce and the stack frame canary. See [ZXVL Verification](../security/zxvl.md).
