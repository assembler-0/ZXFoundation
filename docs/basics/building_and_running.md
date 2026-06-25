# Building and Running ZXFoundation™

Throughout its development ZXFoundation utilize CMake as its main build system orchestrator.

---

## Building

### Prerequisite

| Tool           | (Suggested) Version        | Required | Note                                  |
|----------------|----------------------------|----------|---------------------------------------|
| CMake          | 3.30+                      | Yes      | v3.28 could be used                   |
| Cross Compiler | clang 18+ w/LLD or gcc 15+ | Yes      | C++23-compatible compiler             |
| Host Compiler  | n/a                        | No       | Need for making post-build artifacts  |
| Compiler tools | llvm 18+ or gcc 15+        | No       | C++23-compatible compiler tools       |
| Build tool     | n/a                        | Yes      | Ninja or Visual Studio 22+            |
| Hercules       | n/a                        | No       | Needed for emulation                  |
| Hercules Tools | n/a                        | No       | Needed for 3390 disk creation         |
| Doxygen        | n/a                        | No       | Needed for documentation              |

### Toolchain

A sample toolchain would look like (cmake/toolchain/zxfoundation-clang.cmake):

```cmake
# ZXFoundation clang toolchain for s390x

# Choose your flavors
if (NOT DEFINED ENV{CLANG_VERSION})
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
    set(CMAKE_ASM_COMPILER clang)
else()
    set(CMAKE_ENV_CLANG_VERSION "$ENV{CLANG_VERSION}")
    set(CMAKE_C_COMPILER clang-${CMAKE_ENV_CLANG_VERSION})
    set(CMAKE_CXX_COMPILER clang++-${CMAKE_ENV_CLANG_VERSION})
    set(CMAKE_ASM_COMPILER clang-${CMAKE_ENV_CLANG_VERSION})
endif()

set(CMAKE_LINKER ld.lld)
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_OBJCOPY llvm-objcopy)
set(ZX_NM llvm-nm)
set(ZX_CXXFILT llvm-cxxfilt)
set(ZX_HOST_CC clang)

set(COMPILER_ID "clang") # supported values are "clang", "gcc" and "unknown"-behaves different depending on the value

set(EXTRA_KERNEL_FLAGS "") # extra flags added to kernel compilation
set(EXTRA_LOADER_FLAGS "") # same as above but for ZXFL

set(OPT_LEVEL "2" CACHE STRING "Optimization level (0, 1, 2, 3, s, z)")
set(DSYM_LEVEL "0" CACHE STRING "Debug symbol level (0, 1, 2, 3)")
```

### Configuring and build

You may now you cmake to configure the build directory and build it, for example:

```shell
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path_to_your_toolchain> -GNinja # or "Visual Studio 22"
cmake --build build --parallel <cpus> # "nproc" on linux or "sysctl -n hw.logicalcpu"
```

Now, after that, you will get the artifacts of the build, namely:

```
-rwxr-xr-x. 1 assembler-0 assembler-0   13288 Jun 24 08:24 bin2rec                               # binary to record utility
-rw-r--r--. 1 assembler-0 assembler-0 1070759 Jun 24 12:22 build.ninja
-rw-r--r--. 1 assembler-0 assembler-0   17310 Jun 23 19:46 CMakeCache.txt
drwxr-xr-x. 1 assembler-0 assembler-0     512 Jun 24 12:22 CMakeFiles
-rw-r--r--. 1 assembler-0 assembler-0    2233 Jun 24 12:22 cmake_install.cmake
-rw-r--r--. 1 assembler-0 assembler-0    8160 Jun 24 10:11 core.zxfoundationloader00.sys         # stage 1 loader (record)
-rwxr-xr-x. 1 assembler-0 assembler-0   34388 Jun 24 10:11 core.zxfoundationloader01.sys         # stage 2 loader (binary) 
-rwxr-xr-x. 1 assembler-0 assembler-0 2248560 Jun 24 12:33 core.zxfoundation.nucleus             # kernel image with symbol table
-rwxr-xr-x. 1 assembler-0 assembler-0 2064240 Jun 24 12:33 core.zxfoundation.nucleus.pass1       # kernel image without symbol table
-rw-r--r--. 1 assembler-0 assembler-0      28 Jun 24 12:33 etc.zxfoundation.parm                 # boot comfiguration and cmdline
-rw-r--r--. 1 assembler-0 assembler-0     447 Jun 24 12:33 hercules.cnf                          # hercules configuration script provided
-rw-r-----. 1 assembler-0 assembler-0  534659 Jun 24 12:34 sysres.3390                           # 3390 bootable disk image
drwxr-xr-x. 1 assembler-0 assembler-0      18 Jun 23 08:34 Testing
drwxr-xr-x. 1 assembler-0 assembler-0       8 Jun 23 08:06 user
-rw-r--r--. 1 assembler-0 assembler-0 1105930 Jun 24 12:33 zxallsyms_data.cxx                    # symbol table source 
-rwxr-xr-x. 1 assembler-0 assembler-0   13432 Jun 24 08:24 zxallsyms_gen                         # host tool for generating symbol table
-rwxr-xr-x. 1 assembler-0 assembler-0   22424 Jun 24 12:20 zxf.init                              # test user space program
-rwxr-xr-x. 1 assembler-0 assembler-0    5612 Jun 24 10:11 zxfl_stage1.bin                       # stage 1 loader (binary)
-rwxr-xr-x. 1 assembler-0 assembler-0   12312 Jun 24 10:11 zxfl_stage1.elf                       # stage 1 loader (elf)
-rwxr-xr-x. 1 assembler-0 assembler-0   56576 Jun 24 10:11 zxfl_stage2.elf                       # stage 2 loader (elf)
-rwxr-xr-x. 1 assembler-0 assembler-0   17848 Jun 24 08:24 zxsign                                # kernel signer
```

### Documentation and misc. targets

- `releaseinfo`
  - prints all versioning info at the time of configuration
- `docs`
  - when the target is run, it will only display a message a exists as the kernel focus on minimal dependencies. You can, of course build and view the document manually, for example from project root: `doxygen && python -m http.server -d doxygen-build/html`

## Running

In the artifacts above, the build pipeline generated `hercules.cnf` configuration file for the Hercules emulator. You can try out the kernel immediately by:
```shell
hercules -f hercules.cnf # or omit the "-f hercules.cnf" if you are in the same directory as the configuration file.
```

To configure your own instance of the emulator, please follow the [guide](https://sdl-hercules-390.github.io/html/hercconf.html) from the developers of hercules
A minimal configuration that can be used for ZXFoundation testing purposes:
```text
ARCHLVL      z/Arch   # Set architecture mode z/Arch or ESAME are the same 
MAINSIZE     4G       # Desired amount of CS (Central Storage)
MAXCPU       8        # Maximum CPUs (ZXFoundation supports up to 768 CPUs - see patching_hercules.md)
NUMCPU       8        # CPUs active at IPL time
DIAG8CMD     ENABLE   # Needed for ZXFoundation, if this is disabled a Specification Exception will be thrown
0100         3390    sysres.3390 # Declare our generated 3390 disk image as device 0100

IPL 0100 # Start the emulator by IPL-ing the 0100 device
```