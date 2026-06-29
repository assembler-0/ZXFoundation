# Hercules (Hyperion) DIAG X'288' watchdog

---

## Notes

The Linux `diag288_wdt` driver uses the DIAGNOSE X'288' instruction to control a virtual-machine time bomb (watchdog). On LPAR the watchdog triggers a system restart when the timer expires; on z/VM it sends a CP command.

Hercules (Hyperion) had a stub for this diagnose code but it was disabled at compile time (`#if 0`), contained a syntax error (`case 0x288;:`), and referenced an undefined function (`ARCH_DEP(vm_timebomb)`). Any guest issuing DIAG X'288' received a specification exception immediately.

## The fix

The patch in `scripts/hercules-diag288-watchdog.patch` replaces the dead stub with a working handler that accepts the three standard function codes:

- **0 (INIT)** — arm the timer
- **1 (CHANGE)** — reset the timer
- **2 (CANCEL)** — disarm the timer

Each completes with condition code 0. Invalid function codes still raise a specification exception.

## Applying the patch

An example:
```shell
cd ~/hercules
patch -p1 < ~/zxfoundation/scripts/hercules-diag288-watchdog.patch
```

After patching, rebuild Hyperion the usual way (reconfigure if needed).
