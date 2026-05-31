# Service Call Control Program (SCLP)

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/cpu/sclp.c`, `include/arch/s390x/cpu/sclp.h`

---

## 1. Overview

The Service Call Control Program (SCLP) is the primary interface between the z/Architecture OS and the Support Element (SE) or Hardware Management Console (HMC). It provides access to hardware configuration, capacity information, and console services.

ZXFoundation™ implements a **Raw SCLP Interface** that provides standardized protocol access while adhering to freestanding requirements.

---

## 2. Protocol Specification

Communication with SCLP is performed via a **Service Call Control Block (SCCB)**.

### 2.1 SCCB Structure

An SCCB must be 4 KB aligned and reside below the 2 GB physical boundary. It consists of a header followed by command-specific data.

```cpp
struct sccb_header {
    uint16_t length;          // Total SCCB length
    uint8_t  function_code;   // Command-specific
    uint8_t  control_mask[3]; // Command-specific
    uint16_t response_code;   // Status returned by SCLP
} __attribute__((packed, aligned(8)));
```

### 2.2 Execution Flow

1.  **Preparation**: Initialize the SCCB with the appropriate command data.
2.  **Instruction**: Execute the `SERVC` (Service Call) instruction.
3.  **Busy Wait**: If SCLP returns Condition Code 2, the facility is busy. ZXFoundation™ implements a retry mechanism with configurable delays.
4.  **Verification**: Check the `response_code` in the SCCB header (e.g., `0x0010` for normal completion).

---

## 3. Memory Detection

ZXFoundation™ uses SCLP to accurately detect physical memory, replacing or augmenting manual probing.

### 3.1 Read SCP Information

The `SCLP_CMDW_READ_SCP_INFO` command returns system-wide information including:
*   **RNMAX**: Maximum number of storage increments.
*   **RNSIZE**: Size of each storage increment (typically 1 MB to 256 MB).

### 3.2 Detection Algorithm

```pseudocode
function detect_memory():
    sccb = allocate_aligned_4k()
    sccb.command = READ_SCP_INFO
    
    if execute_sclp(sccb) == SUCCESS:
        total_ram = sccb.rnmax * sccb.rnsize
        return total_ram
    else:
        return fallback_to_manual_probing()
```

---

## 4. Implementation Details

*   **SMP Safety**: SCLP calls are synchronized at the hardware level. The kernel ensures that SCCBs used during early boot are statically allocated or carefully managed to avoid contention.
*   **Context Awareness**: The `sclp_service_call` helper is designed to be used both in early boot (ZXFL) and within the kernel nucleus.
*   **Error Handling**: Comprehensive response code mapping ensures that hardware-specific errors (boundary violations, invalid commands) are correctly identified.
