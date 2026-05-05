# Subsystem Stubs

**Document Revision:** 26h1.1

---

The following subsystems have source directories and header files but are not yet implemented.

---

## IRQ (`arch/s390x/irq/`)

Handles I/O interrupts from the channel subsystem. The I/O new PSW at lowcore `0x1E0` must point to the I/O interrupt handler. The handler calls `TSCH` to read the IRB and dispatches to the appropriate device driver.

**Status:** Stub — new PSW installed as disabled-wait.

---

## Time (`arch/s390x/time/`)

Provides kernel timekeeping using the TOD (Time-of-Day) clock. The TOD clock is a 64-bit counter incremented at 4096 Hz. The boot timestamp is available in `proto->tod_boot`. The clock comparator interrupt (external interrupt subclass) drives the scheduler tick once the IRQ subsystem is active.

**Status:** Stub.
