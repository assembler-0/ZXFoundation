# System Check (syschk)

**Document Revision:** 26h1.1  
**Status:** Active

---

## 1. Overview

The System Check subsystem (syschk) is the kernel's structured mechanism for
reporting and handling fault conditions that arise during execution.  It
replaces the flat `zx_system_check(2)` interface with a typed, three-axis
error taxonomy and a defined halt sequence that includes SMP teardown.

A system check is issued when the kernel detects a condition it cannot safely
recover from, or chooses to evaluate for recovery via a registered filter.

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

| Class    | Value | Halt Behavior                              |
|----------|-------|--------------------------------------------|
| FATAL    | 0xF   | Always halts; SMP teardown performed       |
| CRITICAL | 0xC   | Always halts; SMP teardown performed       |
| WARNING  | 0x3   | Filter consulted; may suppress halt        |

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

The following diagram describes the execution path when a system check is
issued.

```
zx_syschk(code, fmt, ...)
        │
        ▼
  arch_local_irq_disable()
        │
        ├─── CLASS == WARNING ──► filter registered?
        │                               │
        │                         YES ──► call filter(code, msg)
        │                               │
        │                         SUPPRESS ──► restore IRQ flags, return
        │                               │
        │                         HALT  ──► fall through
        │
        ├─── g_halting already set? ──► arch_sys_halt()  (re-entrant path)
        │
        ▼
  g_halting = 1
  g_filter  = NULL
        │
        ▼
  printk system check header + message
        │
        ▼
  smp_teardown()
  ┌─ for each CPU in percpu_areas[] except self:
  │    sigp_busy(cpu_addr, SIGP_STOP, ...)
  └─ CC=3 (not operational) silently skipped
        │
        ▼
  arch_sys_halt()   ← disabled-wait PSW loaded; machine stops
```

---

## 4. WARNING-Class Filter

A single filter function may be registered to intercept WARNING-class system
checks before the halt decision is made.

```
Caller ──► zx_syschk(WARNING code, ...) ──► filter(code, msg)
                                                    │
                                          ┌─────────┴──────────┐
                                     SUPPRESS              HALT
                                          │                    │
                                    return to caller     halt sequence
```

**Contract:**
- The filter is called only for WARNING-class codes.
- It must be async-signal-safe: no locks, no memory allocation.
- It is cleared atomically to `NULL` before any FATAL or CRITICAL halt.
- It is not called during a re-entrant system check.

Registration is not thread-safe and must occur before SMP bringup.

---

## 5. Re-entrancy

If a second system check fires on any CPU while a halt is already in progress
(for example, a fault inside `smp_teardown`), the re-entrant call detects the
`g_halting` flag and proceeds directly to `arch_sys_halt()` without attempting
another teardown.  This prevents infinite recursion and double-teardown races.

---

## 6. SMP Teardown

Before entering the disabled-wait state, the issuing CPU sends `SIGP STOP` to
every other CPU recorded in `percpu_areas[]`.  The `sigp_busy()` helper retries
on condition code 2 (busy) until the order is accepted.  Condition code 3 (not
operational) is silently ignored — a CPU that is already stopped cannot corrupt
shared state.

The teardown is best-effort.  If a CPU does not respond, the issuing CPU
proceeds to halt regardless.

---

## 7. Revision History

| Revision | Change                                      |
|----------|---------------------------------------------|
| 26h1.1   | Initial release; replaces `zx_system_check` |
