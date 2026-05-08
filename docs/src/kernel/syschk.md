# System Check (syschk)

**Document Revision:** 26h1.3  
**Status:** Active

---

## 1. Overview

The System Check subsystem (syschk) is the kernel's mechanism for halting the
system when a condition is detected from which execution cannot safely continue.

The halt path acquires no locks, calls no kernel subsystems, and dereferences
no kernel data structures.  It is safe to call from any context: exception
handlers, IRQ handlers, early init, or a state where kernel memory is corrupt.

---

## 2. Error Code Encoding

Every system check is identified by a 16-bit code with three fields:

```
 15      12 11       8 7             0
 ┌────────┬──────────┬───────────────┐
 │ CLASS  │  DOMAIN  │     TYPE      │
 │  4 b   │   4 b    │     8 b       │
 └────────┴──────────┴───────────────┘
```

| Field  | Bits  | Purpose                              |
|--------|-------|--------------------------------------|
| CLASS  | 15–12 | Severity class                       |
| DOMAIN | 11–8  | Originating subsystem                |
| TYPE   | 7–0   | Specific condition within the domain |

### 2.1 Severity Classes

| Class    | Value | Behavior      |
|----------|-------|---------------|
| FATAL    | 0xF   | Always halts  |
| CRITICAL | 0xC   | Always halts  |
| WARNING  | 0x3   | Always halts  |

All classes halt unconditionally.  The class field exists for post-mortem
triage, not for runtime branching.

### 2.2 Domains

| Domain | Value | Subsystem                    |
|--------|-------|------------------------------|
| CORE   | 0x0   | Core kernel / initialization |
| MEM    | 0x1   | Memory subsystem             |
| SYNC   | 0x2   | Synchronization primitives   |
| ARCH   | 0x3   | Architecture / hardware      |
| SCHED  | 0x4   | Scheduler                    |
| IO     | 0x5   | I/O subsystem                |

---

## 3. Halt Sequence

```
zx_system_check(code, msg)
        │
        ▼
  arch_local_irq_disable()
        │
        ▼
  g_halting set? ──YES──► arch_sys_halt()
        │
        │ NO
        ▼
  g_halting = 1
        │
        ▼
  write zx_crash_record_t to lowcore + 0x1400
  (magic, code, PSW snapshot, reason string)
        │
        ▼
  raw SIGP STOP loop over g_cpu_map[]
  (boot protocol array; no percpu_areas lookup)
  CC=2 retried; CC=3 skipped
        │
        ▼
  arch_sys_halt()  ← disabled-wait PSW; machine stops
```

---

## 4. Crash Record

Before halting, the issuing CPU writes a `zx_crash_record_t` to a fixed
offset (0x1400) within the BSP lowcore.  The lowcore is a fixed physical
address, always mapped, and accessible regardless of kernel heap or DAT state.

```
Offset  Size  Field
------  ----  -----
0x00    8     magic  (0x5A584352554E4348 "ZXCRUNCH")
0x08    2     code   (zx_syschk_code_t)
0x0A    6     pad
0x10    8     psw_mask  (EPSW at time of syschk)
0x18    8     psw_addr  (0; not available from EPSW)
0x20    128   msg    (NUL-terminated reason string)
```

The record is read post-mortem by a debugger or operator console.  It is not
printed to the console during the halt sequence.

---

## 5. Re-entrancy

If a second system check fires on any CPU while a halt is already in progress,
the re-entrant call detects `g_halting` immediately after IRQ disable and
proceeds directly to `arch_sys_halt()`.  The crash record is not overwritten.

`g_halting` is a `volatile int`, not an atomic.  If the memory subsystem is
corrupt, atomic operations cannot be trusted.

---

## 6. SMP Teardown

The halt path iterates `g_cpu_map[]` — the boot protocol's CPU map, registered
at init time via `zx_syschk_register_cpu_map()`.  This array is loader-written,
physically contiguous, and never freed.  It does not depend on `percpu_areas[]`
or any kernel allocator.

`sigp()` is a single inline assembly instruction.  It acquires no locks.
CC=2 (busy) is retried in a tight loop.  CC=3 (not operational) is skipped.

---

## 7. WARNING-Class Codes

WARNING codes halt unconditionally.  There is no filter mechanism.  If a
subsystem needs to log a recoverable condition, it should call `printk`
directly and not use `zx_system_check`.

---

## 8. Revision History

| Revision | Change                                                                                                    |
|----------|-----------------------------------------------------------------------------------------------------------|
| 26h1.3   | Removed filter API; all classes halt unconditionally; crash record written to lowcore; raw SIGP loop; no printk on halt path |
| 26h1.2   | Re-entrant guard moved first; SMP teardown before printk; static BSS message buffer                      |
| 26h1.1   | Initial release                                                                                           |
