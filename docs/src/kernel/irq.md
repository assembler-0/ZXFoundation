# Interrupt Subsystem

**Document Revision:** 26h1.0  
**Subsystem:** `arch/s390x/trap`, `zxfoundation/irq`

---

## 1. Overview

The interrupt subsystem handles all four z/Architecture interrupt classes
delivered to the kernel: program check, external, I/O, and machine check.
It is structured in two layers:

- **Architecture layer** (`arch/s390x/trap/`) — low-level entry stubs and
  class-specific C handlers that decode hardware state from the lowcore.
- **Generic layer** (`zxfoundation/irq/`) — a flat IRQ descriptor table
  that routes decoded interrupt codes to registered handlers.

Supervisor calls (SVC) are reserved for the future syscall layer and are
not dispatched through this subsystem.

---

## 2. Interrupt Delivery on z/Architecture

When an interrupt fires, the hardware atomically:

1. Saves the current PSW into the class-specific **old PSW** slot in the
   lowcore (prefix area).
2. Writes interrupt parameters into fixed lowcore fields.
3. Loads the class-specific **new PSW** slot, transferring control to the
   kernel entry stub.

```
Hardware fires interrupt
        │
        ▼
  Save current PSW → lowcore old PSW slot (0x0130/0x0150/0x0160/0x0170)
        │
        ▼
  Write interrupt parameters to lowcore (pgm_code, ext_int_code, …)
        │
        ▼
  Load new PSW slot (0x01B0/0x01D0/0x01E0/0x01F0) → entry stub
```

The new PSW slots are installed by `zx_lowcore_setup_late()` after DAT is
enabled.  Before that point they hold disabled-wait sentinels.

---

## 3. Lowcore Interrupt Slots

| Class         | Old PSW  | New PSW  | Parameter fields              |
|---------------|----------|----------|-------------------------------|
| External      | `0x0130` | `0x01B0` | `ext_int_code` (0x0086)       |
| Program check | `0x0150` | `0x01D0` | `pgm_code` (0x008E)           |
| Machine check | `0x0160` | `0x01E0` | `mcck_interruption_code` (0x00E8) |
| I/O           | `0x0170` | `0x01F0` | `subchannel_nr` (0x00BA)      |

---

## 4. Entry Stubs (`arch/s390x/trap/entry.S`)

Each entry stub performs the following sequence without touching any kernel
data structure:

```
entry stub
  │
  ├─ Load dedicated stack pointer from lowcore
  │    async_stack (0x0350) for PGM / EXT / IO
  │    mcck_stack  (0x0368) for MCCK
  │
  ├─ Allocate 160-byte ABI save area + 160-byte interrupt frame
  │
  ├─ Store GPRs r0–r15 into frame.gprs[0..15]
  │
  ├─ Copy old PSW (mask + addr) from lowcore into frame.psw_mask/psw_addr
  │
  ├─ Set %r2 = &frame  (first argument to C handler)
  │
  ├─ BRASL → C handler (do_pgm_check / do_ext_interrupt / …)
  │
  └─ Restore GPRs r0–r14, LPSWE from frame.psw_mask
```

The machine-check stub uses a separate stack (`mcck_stack`) so that the
handler runs even if the async stack is corrupt.

### 4.1 Interrupt Frame Layout

```
Offset  Size  Field
------  ----  -----
0x00    128   gprs[0..15]   — GPRs at interrupt time
0x80    8     psw_mask      — old PSW mask word
0x88    8     psw_addr      — old PSW instruction address
```

Total: 160 bytes (`IRQ_FRAME_SIZE`).

---

## 5. IRQ Number Space

The generic layer uses a 16-bit IRQ number partitioned by interrupt class:

```
0x0000 – 0x00FF   Program check codes  (pgm_code & 0x7FFF)
0x0100 – 0x01FF   External codes       (ext_int_code)
0x0200 – 0x02FF   I/O subchannel numbers (subchannel_nr & 0xFF)
0x0300 – 0x03FF   Machine-check sub-codes (mcic >> 56)
```

The descriptor table has `ZX_IRQ_NR_MAX` = 0x400 entries.

---

## 6. IRQ Descriptor Table (`zxfoundation/irq/`)

The table is a flat, statically-allocated BSS array.  Each entry holds:

- A handler function pointer (`irq_handler_t`).
- An opaque `data` pointer forwarded to the handler.
- `flags` (`ZX_IRQF_SHARED`, `ZX_IRQF_DISABLED`).
- A `count` field incremented on every dispatch.

### 6.1 Dispatch Path

```
C handler (do_pgm_check / do_ext_interrupt / …)
  │
  ├─ Read hardware code from lowcore
  ├─ Compute irq = ZX_IRQ_BASE_* + code
  └─ irq_dispatch(irq, frame)
        │
        ├─ Bounds check irq < ZX_IRQ_NR_MAX
        ├─ Increment desc->count
        └─ Call desc->handler (or default handler if NULL)
```

### 6.2 Default Handler Behavior

| IRQ range      | Default action                                      |
|----------------|-----------------------------------------------------|
| PGM (0x0–0xFF) | `zx_system_check(ARCH_UNHANDLED_TRAP)` — fatal      |
| EXT (0x100–0x1FF) | `printk` + drop                                  |
| IO  (0x200–0x2FF) | `printk` + drop                                  |
| MCCK (0x300–0x3FF) | `zx_system_check(ARCH_MCHECK)` — fatal          |

---

## 7. Machine-Check Special Case

Before dispatching, `do_mcck_interrupt` checks the **system damage** bit
(bit 0) of the MCIC.  If set, `zx_system_check()` is called immediately —
the descriptor table itself may reside in damaged storage and cannot be
trusted.

---

## 8. Registration API

```
irq_register(irq, handler, data, flags)  → 0 or -1
irq_unregister(irq)
irq_dispatch(irq, frame)
irq_get_desc(irq)                        → const irq_desc_t *
```

`irq_register` and `irq_unregister` are not SMP-safe at this revision.
They must be called during single-threaded initialization or with external
serialization.

---

## 9. Revision History

| Revision | Change          |
|----------|-----------------|
| 26h1.0   | Initial release |
