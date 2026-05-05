# ZXFoundation Development Guide

**Document Revision:** 26h1.0  
**Applies to:** ZXFoundation release 26h1 and later  
**Status:** Active development

---

## About This Document

This guide is the primary technical reference for the ZXFoundation kernel and its associated toolchain. It is written for:

- **OS developers** who wish to understand the z/Architecture boot and execution environment.
- **Kernel contributors** who need a precise description of subsystem contracts and initialization order.
- **Integrators** who want to load their own kernel or module using the ZXFL bootloader.

Familiarity with C23, ELF64, and general operating-system concepts is assumed. Background on IBM z/Architecture is provided in the Architecture chapter.

---

## What Is ZXFoundation?

ZXFoundation is a freestanding, SMP-capable kernel for IBM z/Architecture (s390x) mainframes and emulators. It is written in C23 and targets the `s390x-unknown-none-elf` ABI.

The project comprises three independently versioned components:

| Component      | Output artifact                                                  | Description          |
|----------------|------------------------------------------------------------------|----------------------|
| **ZXFL**       | `core.zxfoundationloader00.sys`, `core.zxfoundationloader01.sys` | Two-stage bootloader |
| **Nucleus**    | `core.zxfoundation.nucleus`                                      | Kernel ELF64 image   |
| **Host tools** | `bin2rec`, `gen_checksums`                                       | Build-time utilities |

All three are built from a single CMake project using a cross-compiler toolchain targeting s390x.

---

## Version Scheme

Releases follow the scheme `YYhN`, where `YY` is the two-digit year and `N` is the half-year index (1 = first half, 2 = second half). The current release is **26h1**.

The boot protocol carries its own version field (`ZXFL_VERSION_*`). A kernel must check this field and refuse to boot if the version is not one it understands.

---

## Document Organization

| Chapter | Contents |
|---------|----------|
| [Architecture](architecture/overview.md) | z/Architecture fundamentals: PSW, DAT, CCW, IPL, paging |
| [Bootloader](bootloader/overview.md) | ZXFL design, stage descriptions, boot protocol |
| [Kernel](kernel/overview.md) | Subsystem table, initialization sequence, memory management |
| [Build System](build/overview.md) | CMake modules, toolchains, configuration variables |
| [Host Tools](tools/bin2rec.md) | `bin2rec` and `gen_checksums` reference |

---

## Quick Start

```sh
# Configure with the Clang toolchain (recommended)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/zxfoundation-clang.cmake

# Build everything
cmake --build build

# Generate the DASD image and launch Hercules
cmake --build build --target dasd
hercules -f build/hercules.cnf
```

In the Hercules console, issue `ipl 0100` to start the boot sequence.

See [Build System](build/overview.md) for full configuration options and [Build Targets](build/targets.md) for a description of each output artifact.
