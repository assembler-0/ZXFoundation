# Toolchains

**Document Revision:** 26h1.0

---

## 1. Clang (`cmake/toolchain/zxfoundation-clang.cmake`)

Uses LLVM's built-in cross-compilation support — no separate cross-compiler installation is required on most systems.

```sh
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/zxfoundation-clang.cmake \
  -DMARCH_MODE=z14
```

| Role       | Tool                                |
|------------|-------------------------------------|
| C compiler | `clang` (or `clang-$CLANG_VERSION`) |
| Linker     | `ld.lld`                            |
| Archiver   | `llvm-ar`                           |
| objcopy    | `llvm-objcopy`                      |
| Host CC    | `clang`                             |

Set `CLANG_VERSION` in the environment to select a versioned binary (e.g. `CLANG_VERSION=18` → `clang-18`). If unset, unversioned `clang` is used.

The target triple `--target=s390x-unknown-none-elf` is passed as a compile option (not via `CMAKE_C_COMPILER_TARGET`) to avoid CMake's compiler detection interfering with the freestanding build.

---

## 2. GCC (`cmake/toolchain/zxfoundation-gcc.cmake`)

Requires a `s390x-ibm-linux-gnu-*` cross-compiler toolchain installed on the host.

```sh
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/zxfoundation-gcc.cmake
```

| Role       | Tool                          |
|------------|-------------------------------|
| C compiler | `s390x-ibm-linux-gnu-gcc`     |
| Linker     | `s390x-ibm-linux-gnu-ld`      |
| Archiver   | `s390x-ibm-linux-gnu-ar`      |
| objcopy    | `s390x-ibm-linux-gnu-objcopy` |
| Host CC    | `gcc`                         |

GCC-specific flags added to the kernel target:

| Flag                              | Reason                                                                             |
|-----------------------------------|------------------------------------------------------------------------------------|
| `-static-libgcc`                  | Avoid libgcc DSO dependency                                                        |
| `-Wno-array-bounds`               | Suppress false positives from GCC's array-bounds analysis on lowcore pointer casts |
| `-fno-delete-null-pointer-checks` | The kernel legitimately dereferences physical address `0x0` (the lowcore)          |
| `-mzarch`                         | Force z/Architecture mode                                                          |

---

## 3. Common Compiler Flags

Applied to all targets (loader and kernel):

| Flag                              | Reason                                                               |
|-----------------------------------|----------------------------------------------------------------------|
| `-ffreestanding`                  | No hosted C library assumptions                                      |
| `-nostdlib`                       | No implicit library linking                                          |
| `-fno-builtin`                    | Prevent compiler from substituting builtins with libc calls          |
| `-fno-strict-aliasing`            | Kernel code casts between unrelated pointer types                    |
| `-fwrapv`                         | Signed integer overflow wraps (defined behavior)                     |
| `-ftrivial-auto-var-init=pattern` | Auto-initialize locals to a poison pattern — catches use-before-init |
| `-fno-stack-protector`            | No `__stack_chk_guard` — freestanding, no libc                       |
| `-msoft-float`                    | No FPU use in kernel                                                 |
| `-mno-vx`                         | No vector instructions in kernel                                     |

Kernel-only additional flag:

| Flag             | Reason                                                    |
|------------------|-----------------------------------------------------------|
| `-mpacked-stack` | Use packed register save areas (reduces stack frame size) |

---

## 4. Custom Toolchain

To use a non-standard toolchain, copy one of the provided toolchain files and adjust the compiler/linker paths. The following CMake variables must be set:

| Variable                | Description                                                |
|-------------------------|------------------------------------------------------------|
| `CMAKE_C_COMPILER`      | Path to the C compiler                                     |
| `CMAKE_LINKER`          | Path to the linker                                         |
| `CMAKE_OBJCOPY`         | Path to objcopy                                            |
| `ZX_HOST_CC`            | Host C compiler for building `bin2rec` and `gen_checksums` |
| `COMPILER_ID`           | `"clang"` or `"gcc"` (selects compiler-specific flag sets) |
| `TARGET_EMULATION_MODE` | `elf64_s390`                                               |
| `MARCH_MODE`            | Target microarchitecture (e.g. `z10`, `z14`, `z16`)        |
