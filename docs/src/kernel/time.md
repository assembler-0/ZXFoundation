# Time Subsystem

**Document:** ZXF-KRN-TIME-001
**Revision:** 26h1.0
**Status:** Draft

---

## 1. Overview

The time subsystem provides three services to the rest of the kernel:

1. **Monotonic kernel time** (`ktime_t`) — nanoseconds since boot, readable from any context.
2. **Scheduler preemption** — CPU timer fires EXT 0x1004 every 10 ms to enforce quanta.
3. **Deferred execution** — clock comparator fires EXT 0x1005 to advance the per-CPU timer wheel.

All hardware access (STCKF, SPTC, STPTC, SCKC, STCKC, CR0 manipulation) is confined to `arch/s390x/time/tod.c`. The portable kernel layer in `zxfoundation/time/` calls only the functions declared in `include/arch/s390x/time/tod.h`.

---

## 2. Hardware Sources

z/Architecture provides three per-CPU time mechanisms:

| Source | Instruction | Type | Resolution | Kernel use |
|---|---|---|---|---|
| TOD clock | STCKF | Global, monotonic | ~0.244 ns | `ktime_get()`, sleep deadline |
| CPU timer | SPTC / STPTC | Per-CPU countdown | Same as TOD | Scheduler quantum (10 ms) |
| Clock comparator | SCKC / STCKC | Per-CPU absolute | Same as TOD | Timer wheel advance |

The TOD clock is shared across all CPUs and is monotonic. STCKF reads it without serialization and is safe from hard-IRQ context.

---

## 3. TOD Unit Conversion

```
1 TOD unit = 1000/4096 ns = 125/512 ns

ktime_ns = tod_delta × 125 / 512
tod_units = ns × 512 / 125
```

Constants used throughout the subsystem:

```
TOD_1MS  = 4 096 000 units
TOD_10MS = 40 960 000 units
TOD_1S   = 4 096 000 000 units
```

---

## 4. Initialization Sequence

```
BSP:
  time_init()
    tod_set_boot_offset(STCKF)   ← recorded once; never modified
    timer_wheel_init()           ← per-CPU wheel, level/slot arrays zeroed
    tod_enable_ext_interrupts()  ← CR0 bits 52+53 set
    tod_cpu_timer_set(-10ms)     ← first quantum armed
    tod_clock_comparator_set(now + 1s)  ← safe initial value

Each AP (from ap_startup):
  time_init_ap()
    timer_wheel_init()
    tod_enable_ext_interrupts()
    tod_cpu_timer_set(-10ms)
    tod_clock_comparator_set(now + 1s)
```

`tod_boot_offset` is set on the BSP before any AP is started. APs call `ktime_get()` using the same offset — this is correct because the TOD clock is global.

---

## 5. Interrupt Dispatch

The EXT interrupt handler (`do_ext_interrupt`) intercepts the two time-critical subclasses before the generic `irq_dispatch()` path:

```
do_ext_interrupt:
  ext_code = lowcore.ext_int_code
  if ext_code == 0x1004 → time_cpu_timer_handler()   // CPU timer
  if ext_code == 0x1005 → time_clock_comparator_handler()  // clock comparator
  else → irq_dispatch(ZX_IRQ_BASE_EXT + ext_code, frame)
```

This avoids routing through the `irqdesc` table, whose 0x0400-entry limit cannot accommodate the full 16-bit EXT subclass space.

---

## 6. Timer Wheel

### 6.1 Structure

8 levels × 64 slots per CPU. Level 0 has 1 ms slot width; each subsequent level is 64× wider.

```
Level 0: slot = 1 ms,   range = 64 ms
Level 1: slot = 64 ms,  range = ~4 s
Level 2: slot = ~4 s,   range = ~4 min
Level 3: slot = ~4 min, range = ~4.5 h
...
Level 7: slot = ~2 y,   range = ~140 y
```

### 6.2 Placement

A timer with expiry delta `d` from now is placed in the lowest level `l` such that `d < range(l)`, at slot `(current_slot[l] + d/slot_width[l] + 1) % 64`.

### 6.3 Advance

On EXT 0x1005, `timer_wheel_advance(now)` steps level-0 slot by slot, firing all expired timers. When level 0 completes a full revolution, it cascades timers from level 1 into lower levels, and so on.

### 6.4 Constraints

- All wheel operations require IRQs disabled on the calling CPU.
- Callbacks execute in hard-IRQ context. They must not block or acquire locks held by process context.

---

## 7. `ktime_sleep()`

Current implementation is a busy-wait:

```
deadline = STCKF + ns_to_tod(ns)
SCKC(deadline)
while STCKF < deadline: cpu_relax()
```

This is correct for early boot and short delays. Once the scheduler is operational, this will be replaced with a block/wake implementation using the timer wheel.

---

## 8. Strict Requirements

| # | Requirement |
|---|---|
| TIME-1 | `ktime_get()` is callable from any context. No lock, no sleep. |
| TIME-2 | Timer callbacks execute in hard-IRQ context. No blocking, no process-context locks. |
| TIME-3 | CPU timer must be reloaded on every `time_cpu_timer_handler()` invocation. |
| TIME-4 | Clock comparator must be reprogrammed after every `timer_wheel_advance()` call. |
| TIME-5 | `tod_boot_offset` is set once in `time_init()` and never modified. |
| TIME-6 | `time_init_ap()` must be called on every AP before the AP enters its idle loop. |
