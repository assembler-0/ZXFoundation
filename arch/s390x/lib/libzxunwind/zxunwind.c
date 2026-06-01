// SPDX-License-Identifier: Apache-2.0
// arch/s390x/lib/libzxunwind/zxunwind.c
//
/// @brief Raw stack frame unwinder for s390x.

#include <arch/s390x/lib/libzxunwind/zxunwind.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/sys/zxallsyms.h>
#include <zxfoundation/memory/hhdm.h>
#include <lib/vsprintf.h>

/// @brief Verify if a given stack pointer is aligned and points to valid kernel memory.
/// @param sp The stack pointer value to verify.
/// @param lc The lowcore pointer (can be NULL).
/// @return True if the stack pointer is valid to dereference, false otherwise.
static bool is_valid_sp(uint64_t sp, const zx_lowcore_t *lc) {
    if ((sp & 7U) != 0) {
        return false;
    }
    if (sp == 0) {
        return false;
    }

    if (sp < CONFIG_KERNEL_VIRT_OFFSET || sp >= 0xFFFFC00000000000ULL) {
        return false;
    }

    if (lc != nullptr) {
        if (lc->kernel_stack != 0 && sp >= (lc->kernel_stack - 65536) && sp <= lc->kernel_stack) {
            return true;
        }
        if (lc->async_stack != 0 && sp >= (lc->async_stack - 16384) && sp <= lc->async_stack) {
            return true;
        }
        if (lc->mcck_stack != 0 && sp >= (lc->mcck_stack - 16384) && sp <= lc->mcck_stack) {
            return true;
        }
        if (lc->restart_stack != 0 && sp >= (lc->restart_stack - 16384) && sp <= lc->restart_stack) {
            return true;
        }
    }

    return true;
}

uint32_t zx_unwind_backtrace(zx_unwind_frame_t *frames, uint32_t max_depth,
                             const arch_s390x_irq_frame_t *irq_frame) {
    if (frames == nullptr || max_depth == 0) {
        return 0;
    }

    const zx_lowcore_t *lc = nullptr;
    const zx_lowcore_t *raw_lc = zx_lowcore();
    if (raw_lc != nullptr && raw_lc->kernel_asce != 0) {
        lc = raw_lc;
    }

    uint64_t sp = 0;
    uint64_t pc = 0;

    if (irq_frame != nullptr) {
        sp = irq_frame->gprs[15];
        pc = irq_frame->psw_addr;
    } else {
        __asm__ volatile("lgr %0, %%r15" : "=r"(sp));
        __asm__ volatile("lgr %0, %%r14" : "=r"(pc));
    }

    uint32_t depth = 0;
    uint64_t last_sp = 0;



    while (depth < max_depth) {
        if (!is_valid_sp(sp, lc)) {
            break;
        }

        frames[depth].sp = sp;
        frames[depth].pc = pc;

        frames[depth].symbol_name[0] = '\0';
        frames[depth].symbol_offset = 0;
        zxallsyms_lookup_addr(pc, frames[depth].symbol_name, sizeof(frames[depth].symbol_name), &frames[depth].symbol_offset);

        depth++;

        // Move to the next frame in the back chain.
        last_sp = sp;
        uint64_t *next_sp_ptr = (uint64_t *)sp;
        uint64_t next_sp = *next_sp_ptr;

        if (next_sp == 0 && lc != nullptr) {
            bool on_irq_stack = (lc->async_stack != 0 && sp >= (lc->async_stack - 16384) && sp <= lc->async_stack) ||
                                (lc->mcck_stack != 0 && sp >= (lc->mcck_stack - 16384) && sp <= lc->mcck_stack);
            if (on_irq_stack) {
                const arch_s390x_irq_frame_t *iframe = (const arch_s390x_irq_frame_t *)(sp + 160U);
                if (is_valid_sp(iframe->gprs[15], lc)) {
                    sp = iframe->gprs[15];
                    pc = iframe->psw_addr;
                    continue; // Pivot and continue unwinding from the interrupted context!
                }
            }
        }

        bool backchain_valid = (next_sp != 0 && is_valid_sp(next_sp, lc) && next_sp > last_sp);

        if (backchain_valid) {
            sp = next_sp;
            uint64_t *parent_sp_ptr = (uint64_t *)sp;
            uint64_t parent_sp = *parent_sp_ptr;
            if (parent_sp != 0 && is_valid_sp(parent_sp, lc)) {
                uint64_t *ra_ptr = (uint64_t *)(parent_sp + 112U);
                pc = *ra_ptr;
            } else {
                // If there's no parent_sp, this is the root frame of the stack.
                // It has no caller, so it has no return address. Stop unwinding.
                break;
            }
        } else {
            // BACKCHAIN IS CORRUPTED OR CYCLIC. Fall back to heuristic scanning.
            bool found = false;
            uint64_t scan_ptr = last_sp + 8;
            uint64_t max_ptr = (last_sp & ~(65535ULL)) + 65536ULL;
            
            if (lc != nullptr) {
                 if (lc->kernel_stack != 0 && last_sp >= (lc->kernel_stack - 65536) && last_sp <= lc->kernel_stack) max_ptr = lc->kernel_stack;
                 else if (lc->async_stack != 0 && last_sp >= (lc->async_stack - 16384) && last_sp <= lc->async_stack) max_ptr = lc->async_stack;
                 else if (lc->mcck_stack != 0 && last_sp >= (lc->mcck_stack - 16384) && last_sp <= lc->mcck_stack) max_ptr = lc->mcck_stack;
                 else if (lc->restart_stack != 0 && last_sp >= (lc->restart_stack - 16384) && last_sp <= lc->restart_stack) max_ptr = lc->restart_stack;
            }

            while (scan_ptr < max_ptr) {
                 uint64_t val = *(uint64_t *)scan_ptr;
                 if (val > CONFIG_KERNEL_VIRT_OFFSET && val < 0xFFFFC00000000000ULL && (val & 1) == 0) {
                      char sym[128];
                      uint64_t offset;
                      if (zxallsyms_lookup_addr(val, sym, sizeof(sym), &offset) && sym[0] != '\0') {
                           uint64_t candidate_sp = (scan_ptr >= 112) ? (scan_ptr - 112) : 0;
                           // Only accept this return address if it forms a frame strictly above the previous one
                           if (candidate_sp > last_sp) {
                               sp = candidate_sp;
                               pc = val;
                               found = true;
                               break;
                           }
                      }
                 }
                 scan_ptr += 8;
            }
            if (!found) {
                break;
            }
        }
    }

    return depth;
}

void zx_unwind_print(const arch_s390x_irq_frame_t *irq_frame, const zx_unwind_write_cb_t cb) {
    static zx_unwind_frame_t frames[ZX_UNWIND_MAX_DEPTH];
    
    uint32_t count = zx_unwind_backtrace(frames, ZX_UNWIND_MAX_DEPTH, irq_frame);

    for (uint32_t i = 0; i < count; i++) {
        char buf[256];
        if (frames[i].symbol_name[0] != '\0') {
            snprintf(buf, sizeof(buf), "  [%02u] 0x%016llx in %s+0x%llx (sp=0x%016llx)\n",
                     i, (unsigned long long)frames[i].pc,
                     frames[i].symbol_name,
                     (unsigned long long)frames[i].symbol_offset,
                     (unsigned long long)frames[i].sp);
        } else {
            snprintf(buf, sizeof(buf), "  [%02u] 0x%016llx (sp=0x%016llx)\n",
                     i, (unsigned long long)frames[i].pc,
                     (unsigned long long)frames[i].sp);
        }
        cb(buf);
    }
}
