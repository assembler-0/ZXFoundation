# ZX-Discovery Manifest Specification

## Overview
`zx-discovery` is a Domain Specific Language (DSL) used by ZXFoundation to orchestrate manifest-based source discovery, C++23 module classification, and conditional compilation. It allows the kernel's structural architecture to be defined in `manifest.zxd` files scattered across the source tree.

## 1. Syntax Fundamentals
* **Comments**: Lines starting with `#` are ignored.
* **Keywords**: All DSL keywords start with an exclamation mark (`!`).
* **Blocks**: Scoped logic is enclosed in braces `{ ... }`.
* **Ordering**: Manifests are processed line-by-line. Variables defined in `!defines` or `!options` apply to all subsequent `!files` blocks within the same manifest.

---

## 2. Block Directives

### 2.1 `!defines { ... }`
Sets file-level compile definitions (`-D`) for all subsequent files in the manifest.
```c++
!defines {
    __S390X_CPU_FEATURE__
    DEBUG_LEVEL=4
}
```

### 2.2 `!options { ... }`
Sets file-level compiler flags for all subsequent files in the manifest.
```c++
!options {
    -mbackchain
    -msoft-float
}
```

### 2.3 `!files : !compile_group -> [type] { ... }`
Defines a set of source files and their compilation strategy.
* **Types**:
    * `[modules]`: Forces files to be treated as C++23 modules (`.cxxm`).
    * `[standard]`: Forces files to be treated as standard translation units (`.S`, `.c`, `.cpp`).
    * `[auto]`: Uses extension-based detection (`.cxxm` is a module, others are standard).

**Example:**
```c++
!files : !compile_group -> [auto] {
    processor.cxxm
    trap_entry.S
}
```

---

## 3. Conditional Compilation (`!if`)
The `!if` block allows conditional inclusion of files or configuration based on CMake variables.

**Syntax:**
```c++
!if CONFIG_FEATURE_XYZ {
    # Included only if CONFIG_FEATURE_XYZ is TRUE in CMake
    !files : !compile_group -> [modules] {
        feature_xyz.cxxm
    }
}

!if !CONFIG_LEGACY_BOOT {
    # Included only if CONFIG_LEGACY_BOOT is FALSE or undefined
}
```
* **Nesting**: `!if` blocks can be nested arbitrarily.
* **Evaluation**: Conditions are evaluated against the CMake variable namespace at configuration time.

---

## 4. Per-File Metadata
Individual files within a `!files` block can have specific overrides.

### 4.1 `!compile_options -> [ flags ]`
Overrides the manifest's global options for a specific file.
```c++
!files : !compile_group -> [standard] {
    critical_path.c !compile_options -> [ -O3 -fno-inline ]
    generic_path.c
}
```

---

## 5. Scope & Lifecycle
1. **Discovery**: CMake performs a recursive glob for `manifest.zxd`.
2. **Parsing**: The `zx_discover_nucleus` function parses each file.
3. **Property Injection**: Properties (defines/options) are attached to the source files via `set_source_files_properties`.
4. **Targeting**: Discovered files are aggregated into `ZX_SOURCES_64` and `ZX_SOURCES_MODULES_64` for the `core.zxfoundation.nucleus` target.
