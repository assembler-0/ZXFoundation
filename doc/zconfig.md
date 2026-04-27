# The zconfig fornat  

### Basic types and declaration
Every symbol is declared with a type, a default, and optional metadata in a trailing block.
```c
# Simple bool flag
option SMP : bool = false {
    label  "Symmetric multiprocessing"
    help   "Enable support for multiple CPU cores."
}

# Integer with range constraint
option NR_CPUS : int = 4 {
    label  "Max CPU count"
    range  1 .. 512
    when   SMP
}

# String with validation pattern
option KERNEL_VERSION : string = "6.1.0" {
    label   "Kernel version string"
    pattern "^\d+\.\d+\.\d+"
}

# Enum type
option ARCH : enum = x86_64 {
    label   "Target architecture"
    choices x86_64 | arm64 | riscv64 | loongarch
}
```

### Grouping — menu
Menus create nested hierarchy with their own visibility condition. Options inside inherit their parent's when automatically.
```c
menu "Memory management" {
    option SPARSEMEM : bool = false {
        label "Sparse memory model"
    }

    option SPARSEMEM_VMEMMAP : bool = true {
        label "Use vmemmap for pfn_to_page"
        when  SPARSEMEM
    }

    menu "NUMA" when SMP {
        option NUMA : bool = false {
            label "Non-uniform memory access"
        }
    }
}
```

### Dependencies
Instead of depends on A && (B || !C), use when with natural boolean expressions. select becomes implies, which is clearer — enabling X doesn't silently smash Y anymore; the user sees it as a suggestion.

```c
option VIRTIO_NET : bool = false {
    label   "VirtIO network driver"
    when    VIRTIO and NET
    implies VIRTIO_RING          # softer than select: shows a prompt, not a silent force
    implies VIRTIO_PCI or VIRTIO_MMIO
}

option EXPERT : bool = false {
    label   "Expose expert/dangerous options"
}

option KALLSYMS_ALL : bool = false {
    label   "Include all symbols in kallsyms"
    when    KALLSYMS and EXPERT
}
```

### Choice group
Instead of the choice...endchoice block that has its own scoping quirks, choices are just an option of enum type. Exclusive and non-exclusive variants are explicit.
```c
# Exclusive — only one may be active (radio)
option PREEMPT_MODEL : enum = voluntary {
    label   "Preemption model"
    choices none | voluntary | full | realtime
    # Each choice can carry its own label/help
    choice none      { label "No preemption (server/HPC)" }
    choice voluntary { label "Voluntary (desktop default)" }
    choice full      { label "Full preemption" }
    choice realtime  { label "PREEMPT_RT (low latency)"; when EXPERT }
}

# Non-exclusive — multiple allowed (checkboxes)
option DEBUG_FEATURES : set = {} {
    label   "Debug feature flags"
    choices lockdep | kasan | ubsan | kcsan
}
```

### Conditional defaults
Defaults can vary based on environment without separate default X if Y lines scattered everywhere.
```c
option HZ : int {
    label  "Timer frequency (Hz)"
    range  100 .. 1000
    default {
        1000 when PREEMPT_MODEL == realtime
        250  when PREEMPT_MODEL == full
        100  # fallback
    }
}
```

### Computed (read-only) symbols
Derived values that the user cannot edit — they reflect the state of other options. Useful for feature-flag aggregation.
```c
computed HAVE_KVM : bool = ARCH == x86_64 or ARCH == arm64
```

### Includes and namespacing
Clean modular splits without the source keyword oddness. Optional namespace prefixing so arch-specific symbols don't bleed globally.
```c
include "arch/x86/Kconfig"          # flat include
include "drivers/net/Kconfig" as net  # namespaced: net.VIRTIO_NET etc.
include "drivers/net/Kconfig" when NET # conditional include
```

### Host awareness (Probing)
Zconfig can probe the host environment during the parsing phase. This is useful for defaults that match the current system.
```c
option HOSTNAME : string = shell("hostname") {
    label "Host system name"
}

option BUILD_USER : string = env("USER") {
    label "User performing the build"
}
```

### String interpolation
Symbols of type `string` can use `${SYMBOL}` syntax to dynamically construct values from other options.
```c
option PREFIX : string = "/usr/local"

computed BIN_DIR : string = "${PREFIX}/bin"
computed LIB_DIR : string = "${PREFIX}/lib/${ARCH}"
```

### Cross-symbol validation
While `range` and `pattern` protect single symbols, `validate` blocks enforce system-wide invariants. If a validation fails, the configuration is considered "invalid" and cannot be saved/applied.
```c
option RAM_SIZE : int = 1024
option THREADS  : int = 8

validate "Insufficient memory per thread" {
    RAM_SIZE >= THREADS * 128
}

validate "RAM must be power of two" {
    (RAM_SIZE & (RAM_SIZE - 1)) == 0
}
```

### Meta-Programming (Macros)
Macros allow for boilerplate reduction by defining reusable configuration templates. Identifier splicing is supported via `${}` syntax.
```c
macro DRIVER(id, label_name) {
    option DRV_${id} : bool = false {
        label "${label_name} Support"
    }
}

DRIVER(VIRTIO, "VirtIO Virtual Block")
DRIVER(NVME,   "Non-Volatile Memory Express")
```

### Output Orchestration (Generators)
Generators define how the final configuration state is exported to external build tools.
```c
generate header   "config.h"    # C/C++ Header
generate makefile "config.mk"   # GNU Make fragment
generate json     "config.json" # Machine-readable state
generate cmake    "config.cmake" # CMake fragment
generate meson    "config.meson" # Meson fragment
generate rust     "config.rs"   # Rust constants
generate typescript "config.ts" # TypeScript constants
generate dotenv   "config.env"  # Dotenv format
generate toml     "config.toml" # TOML format
```

### Advanced Expressions
Support for bitwise operators (`&`, `|`, `^`, `<<`, `>>`), math (`+`, `-`, `*`, `/`, `%`), and functions (`env`, `shell`, `is_defined`, `abs`).
```c
computed MASK : int = (1 << 8) - 1
```