# Build System Overview

**Document Revision:** 26h1.0

---

## 1. Prerequisites

| Tool               | Minimum version    | Notes                                  | Required |
|--------------------|--------------------|----------------------------------------|----------|
| CMake              | 3.20               | Build system generator                 | true     |
| Compiler and tools | toolchain-specific | See [toolchains.md](toolchains.md)     | true     |
| Ninja              | any                | Recommended generator                  | optional |
| dasdload           | any                | Needed for image generation (optional) | optional |
| Hercules           | 4.x                | Helpful for development                | optional |

---

## 2. Output Artifacts

| Artifact                        | Description                                 | Converted from                        |
|---------------------------------|---------------------------------------------|---------------------------------------|
| `core.zxfoundationloader00.sys` | Stage 0 IPL record (tape format)            | `zxfl_stage1.elf` → `zxfl_stage1.bin` |
| `core.zxfoundationloader01.sys` | Stage 1 flat binary                         | `zxfl_stage2.elf`                     |
| `core.zxfoundation.nucleus`     | Kernel ELF64 (SHA-256 checksums patched in) | N/A                                   |
| `sysres.3390`                   | Hercules 3390 DASD image                    | N/A                                   |
| `bin2rec`                       | Host tool                                   | N/A                                   |
| `gen_checksums`                 | Host tool                                   | N/A                                   |

---

## 3. CMake Modules

| Module                             | Purpose                                                |
|------------------------------------|--------------------------------------------------------|
| `cmake/dependencies.cmake`         | Host dependency checks                                 |
| `cmake/configuration.cmake`        | `OPT_LEVEL`, `DSYM_LEVEL` cache variables              |
| `cmake/platform.cmake`             | Platform detection                                     |
| `cmake/standard.cmake`             | C standard enforcement                                 |
| `cmake/hosttools.cmake`            | Build `bin2rec` and `gen_checksums` with host compiler |
| `cmake/source.cmake`               | Kernel source file lists (`ZX_SOURCES_64`)             |
| `cmake/zxfl-compile.cmake`         | ZXFL Stage 0 and Stage 1 targets                       |
| `cmake/zxfoundation-compile.cmake` | Kernel nucleus target                                  |
| `cmake/run.cmake`                  | `dasd` target — generates `sysres.3390`                |

---

## 4. Build Order

CMake enforces the following dependency chain:

```
tools  (bin2rec, gen_checksums — host compiler)
  │
  ├─► zxfl_stage1.elf
  │     └─► zxfl_stage1.bin  (objcopy)
  │           └─► core.zxfoundationloader00.sys  (bin2rec)
  │
  ├─► zxfl_stage2.elf
  │     └─► core.zxfoundationloader01.sys  (objcopy)
  │
  └─► core.zxfoundation.nucleus
        └─► gen_checksums patches .zxvl_checksums in-place
              └─► sysres.3390  (dasdload)
```

Host tools are always compiled first with `ZX_HOST_CC`. The kernel and loader are compiled with the cross-compiler.

---

## 5. Configuration Variables (non-toolchain-specific, for toolchain-specific, see [toolchains.md](toolchains.md))

| Variable                | Default                  | Description                      |
|-------------------------|--------------------------|----------------------------------|
| `OPT_LEVEL`             | `2`                      | `-O` level for all targets       |
| `DSYM_LEVEL`            | `0`                      | `-g` level (0 = no debug info)   |

Override at configure time:

```sh
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/zxfoundation-clang.cmake \
  -DOPT_LEVEL=3
```
