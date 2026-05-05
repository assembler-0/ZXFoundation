# Subsystem Stubs

**Document Revision:** 26h1.0

---

The following subsystems have source directories and header files but are not yet implemented. They are listed here for completeness and to document the intended interface.

---

## IRQ (`arch/s390x/irq/`)

Handles I/O interrupts from the channel subsystem. The interrupt new PSW at lowcore `0x1E0` must point to the I/O interrupt handler. The handler calls `TSCH` to read the IRB and dispatches to the appropriate device driver.

**Status:** Stub — new PSW installed as disabled-wait.

---

## Trap (`arch/s390x/trap/`)

Handles program interrupts (illegal instruction, protection exception, addressing exception, etc.). The program new PSW at lowcore `0x1D0` must point to the trap handler.

**Status:** Stub — new PSW installed as disabled-wait.

---

## Time (`arch/s390x/time/`)

Provides kernel timekeeping using the TOD (Time-of-Day) clock. The TOD clock is a 64-bit counter incremented at 4096 Hz. The boot timestamp is available in `proto->tod_boot`.

**Status:** Stub.

---

## Scheduler (`zxfoundation/sched/`)

Process/thread scheduling. Depends on IRQ (for timer interrupts) and SMP (for per-CPU run queues).

**Status:** Stub.

---

## kobject (`zxfoundation/object/kobject.c`)

Reference-counted kernel object base type. Active but not yet integrated into all subsystems. `zx_page_t` explicitly does not use kobject to avoid the 32-byte descriptor size constraint.
