# Hercules (Hyperion) DIAG X'288' watchdog

---

## Notes

The Linux `diag288_wdt` driver uses DIAGNOSE X'288' to arm a virtual-machine time bomb. On LPAR (the default in Hercules) the watchdog triggers a full system IPL from the boot device when the timer expires; on z/VM it sends a CP command.

Hercules (Hyperion) had a stub for this diagnose code but it was disabled at compile time (`#if 0`), contained a syntax error (`case 0x288;:`), and referenced an undefined function. Any guest issuing DIAG X'288' received a specification exception immediately.

## The fix

The patch in `scripts/hercules-diag288-watchdog.patch` replaces the dead stub with a working implementation:

- **INIT** (function code 0) — arms the timer and spawns a background thread that waits for expiry
- **CHANGE** (function code 1) — resets the expiry deadline
- **CANCEL** (function code 2) — disarms the timer and stops the thread

When the timer expires, the thread stops all CPUs and issues an IPL from the device that was last used to boot (`sysblk.ipldev`).

## Applying the patch

An example:
```shell
cd ~/hercules
patch -p1 < ~/zxfoundation/scripts/hercules-diag288-watchdog.patch
```

After patching, rebuild Hyperion the usual way (reconfigure if needed).
