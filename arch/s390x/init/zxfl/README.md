# ZXFoundation Bootloader (ZXFL) Architecture Specification

## 1. Introduction

This specification delineates the architectural requirements, initial machine state, and memory layout constraints imposed upon any operating system nucleus intended to be loaded and executed by the ZXFoundation Bootloader (ZXFL) on the s390x (z/Architecture) hardware platform. Compliance with these structural definitions is mandatory to ensure successful transition of execution control from the Stage 2 loader environment to the target kernel nucleus.

## 2. Nucleus Image Format Constraints

### 2.1 File Format
The kernel image must be formatted as a 64-bit Executable and Linkable Format (ELF64) binary. The binary type must be strictly `ET_EXEC` (Executable file). Relocatable objects (`ET_REL`) and shared objects (`ET_DYN`) are not supported and will be rejected by the loader.

### 2.2 Addressing Mode
The executable must be compiled for the z/Architecture (s390x) instruction set. The loader requires the kernel to be fully 64-bit compliant.

### 2.3 Virtual Address Space and Entry Point
The ZXFL mandates a Higher-Half Direct Map (HHDM) architecture. The kernel's virtual memory addresses (as specified in the ELF program headers) must reside within the high virtual memory segment. 

* **Required HHDM Offset:** `0xFFFF800000000000` (i.e., `-128TB`).
* **Physical Load Address:** The loader will physicalize the kernel segments by stripping the HHDM offset. The kernel's physical load boundary begins at physical address `0x40000`.
* **Entry Point Constraint:** The ELF `e_entry` field must represent a virtual address greater than or equal to `0xFFFF800000040000`. The bootloader validates this condition and aborts execution if the entry point resides in the lower half of the address space.

## 3. Initial Machine State

At the precise moment of handover (the instruction branching to the kernel entry point), the ZXFL guarantees the following architectural state:

### 3.1 Processor State
* **Architecture Mode:** z/Architecture (64-bit mode).
* **Execution State:** Supervisor state (PSW bit 15 is 0).
* **Addressing Mode:** 64-bit addressing (PSW bits 31 and 32 are 1).
* **Interrupts:** All maskable interrupts (I/O, External, and Machine-Check) are disabled.
* **Dynamic Address Translation (DAT):** Enabled (PSW bit 5 is 1).

### 3.2 Register State
* **General Purpose Register 2 (`%r2`):** Contains the 64-bit virtual address of the ZXFL Boot Protocol structure (`zxfl_boot_protocol_t`).
* **General Purpose Register 15 (`%r15`):** Contains a valid, correctly aligned 64-bit virtual stack pointer provisioned by the loader. This stack is strictly intended for initial kernel bootstrapping and should be discarded after secondary processor initialization.
* All other general-purpose and floating-point registers contain undefined data and must not be relied upon.

### 3.3 Translation-Lookaside Buffer (TLB) and Page Tables
The hardware ASCE (Address Space Control Element) in Control Register 1 (`%cr1`) points to a 5-level (or 4-level fallback) paging hierarchy constructed by the loader. This hierarchy implements the HHDM, mapping the entirety of discovered physical memory starting at virtual address `0xFFFF800000000000`. The lowest 2GB of the virtual address space is identity-mapped to facilitate the transition; however, the kernel is expected to execute solely from the HHDM segment.

## 4. Boot Protocol Interface

The kernel receives hardware detection and mapping data via the `zxfl_boot_protocol_t` structure passed in `%r2`. The structure conforms to Version 4 of the ZXFL specification.

### 4.1 Structure Layout
The layout of the boot protocol structure can be found in `include/arch/s390x/init/zxfl/zxfl.h`

### 4.2 Data Integrity
The magic number must be validated by the kernel upon entry. A mismatch implies corruption or invalid loader transition. The data structures are located within the HHDM and are safe to access immediately with DAT enabled.

## 5. Post-Handover Kernel Obligations

Upon gaining execution control, the kernel assumes absolute authority over the hardware and must fulfill the following operational requirements:

1. **Vector Table Initialization:** The loader does not provide a functional New PSW table for interrupts. The kernel must allocate and populate its own Lowcore (Prefix Area) and issue the `Set Prefix` instruction to establish interrupt handlers prior to enabling maskable interrupts via `STOSM` or `SSM`.
2. **Page Table Ownership:** The kernel may continue utilizing the loader-provided ASCE or construct its own translation tables. If memory isolation or advanced paging features (e.g., EDAT-2) are required, the kernel must transition to its own ASCE.
3. **Application Processor (AP) Wakeup:** APs remain in the stopped state (`SIGP` Sense state 2). The kernel's Bootstrap Processor (BSP) is responsible for allocating private prefix areas and stacks for the APs before issuing `SIGP Initial CPU Reset` and `SIGP Restart` to initiate multi-processor execution.

Failure to adhere to these structural constraints will result in unrecoverable architectural exceptions.

## 6. Security and Verification Mechanisms

To enforce supply-chain integrity and prevent arbitrary payloads from hijacking the boot sequence, the ZXFL employs a proprietary multi-stage verification mechanism. The kernel nucleus must embed specific cryptographic signatures and algorithmic stubs exactly as defined below.

### 6.1 Handshake Routine (Offset `0x00000`)
The lowest virtual address of the loaded kernel (`load_min`) must contain a 64-bit ABI-compliant C-callable function stub. Prior to relinquishing control, the loader invokes this stub to verify algorithmic authenticity.

* **Invocation Signature:** `uint64_t hs_fn_t(uint64_t nonce)`
* **Challenge Operation:** The loader passes an obfuscated nonce (`ZXVL_SEED ^ hs_nonce`).
* **Expected Response:** The kernel must return the result of `((nonce << 17) | (nonce >> 47)) + ZXVL_HS_RESPONSE`.

If the kernel fails to return the exact calculated response, the loader will abort with a "handshake failed" condition.

### 6.2 Structural Lock (Offset `0x70000`)
The kernel must define a dedicated `.zxfl_lock` memory section strictly anchored at offset `0x70000` relative to the base load address.

* **Sentinel Verification:** At offset `0x70004`, the kernel must place the 32-bit ASCII sentinel `0x5A58464C` ("ZXFL").
* **Key Verification:** At offset `0x70000` (High 32-bits) and offset `0x71000` (Low 32-bits), the kernel must place the pre-computed lock values `0xCCBBCC35` and `0xE5664311` respectively. The loader concatenates these values and verifies that `(KEY ^ ZXVL_LOCK_MASK) == ZXVL_LOCK_EXPECTED`.

## 7. Deployment Constraints

### 7.1 Dataset Naming Convention
The Stage 2 ZXFL is rigidly programmed to search the Initial Program Load (IPL) volume VTOC for a statically named dataset. The kernel nucleus must be deployed to the DASD volume strictly under the dataset name:

`CORE.ZXFOUNDATION.NUCLEUS`

Dataset name variations will result in a fatal `FILE NOT FOUND` condition during IPL.

### 7.2 Boot Stack Dimensions
The handover stack injected into General Purpose Register 15 (`%r15`) is constrained. It is typically sized at 16KB to 32KB. The kernel must limit its stack frame consumption during the very early `main()` initialization phase. Heavy allocations or deep recursion prior to transitioning to an internally managed kernel stack will result in a stack overflow and subsequent memory corruption.
