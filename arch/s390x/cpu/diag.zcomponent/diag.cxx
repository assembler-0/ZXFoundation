/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/diag.zcomponent/diag.cxx
/// @brief z/Architecture DIAGNOSE instruction — generic low-level interface +
///        typed helper wrappers for all standard diagnose codes (0x00–0x500).

module arch.s390x.cpu.diag;
import arch.s390x.cpu.types;
import zxfoundation.base.types;
import lib.bit;

#define __zxmacro_want_extable
#include macro_extensions

namespace arch::s390x::cpu::diag {

    /// Generic diagnose — issue any code with arbitrary R1/R2 register values.
    /// @tparam Code  The 16-bit diagnose code (e.g. 0x044, 0x308).
    /// @param  r1    Value placed in R1 (and R1+1 for processor::register_pair codes).
    /// @param  r2    Value placed in R2 (and R2+1).
    /// @return       Condition code (0–3) set by the hypervisor/hardware.
    template <u16 Code>
    [[gnu::always_inline]] static auto diagnose12(u64& r1, u64& r2) noexcept -> condition_code {
        condition_code cc;
        __asm__ volatile(
            "0: diag    %[r1],%[r2],%[code]\n"
            "1: ipm     %[cc]\n"
            "   srl     %[cc],28\n"
            EX_TABLE(0b, 1b, EX_TYPE_FIXUP, 0)
            : [r1] "+d" (r1), [r2] "+d" (r2), [cc] "=d" (cc)
            : [code] "K" (Code)
            : "cc", "memory"
        );
        return cc;
    }

    /// @brief Generic diagnose12 (register-pair variant) — issue any code with full
    ///        128-bit R1:R1+1 and R3:R3+1 register pairs.
    /// @tparam Code  The 16-bit diagnose code (e.g. 0x288).
    /// @param  r1    128-bit register pair (even=high, odd=low).
    /// @param  r2    128-bit register pair (even=high, odd=low).
    /// @return       Condition code (0–3) set by the hypervisor/hardware.
    template <u16 Code>
    [[gnu::always_inline]] static auto diagnose12(
        processor::register_pair& r1,
        processor::register_pair& r2
    ) noexcept -> condition_code {
        condition_code cc;
        __asm__ volatile(
            "0: diag    %[r1],%[r2],%[code]\n"
            "1: ipm     %[cc]\n"
            "     srl     %[cc],28\n"
            EX_TABLE(0b, 1b, EX_TYPE_FIXUP, 0)
            : [r1] "+d" (r1.pair), [r2] "+d" (r2.pair), [cc] "=d" (cc)
            : [code] "K" (Code)
            : "cc", "memory"
        );
        return cc;
    }

    /// Generic diagnose23 — issue any code with arbitrary R2/R3 register values.
    /// @tparam Code  The 16-bit diagnose code (e.g. 0x044, 0x308).
    /// @param  r2    Value placed in R2 (and R1+1 for processor::register_pair codes).
    /// @param  r3    Value placed in R2 (and R2+1).
    /// @return       Condition code (0–3) set by the hypervisor/hardware.
    template <u16 Code>
    [[gnu::always_inline]] static auto diagnose23(u64& r2, u64& r3) noexcept -> condition_code {
        condition_code cc;
        __asm__ volatile(
            "0: diag    %[r2],%[r3],%[code]\n"
            "1: ipm     %[cc]\n"
            "     srl     %[cc],28\n"
            EX_TABLE(0b, 1b, EX_TYPE_FIXUP, 0)
            : [r2] "+d" (r2), [r3] "+d" (r3), [cc] "=d" (cc)
            : [code] "K" (Code)
            : "cc", "memory"
        );
        return cc;
    }

    /// @brief Generic diagnose23 (register-pair variant) — issue any code with full
    ///        128-bit R1:R1+1 and R3:R3+1 register pairs.
    /// @tparam Code  The 16-bit diagnose code (e.g. 0x288).
    /// @param  r2    128-bit register pair (even=high, odd=low).
    /// @param  r3    128-bit register pair (even=high, odd=low).
    /// @return       Condition code (0–3) set by the hypervisor/hardware.
    template <u16 Code>
    [[gnu::always_inline]] static auto diagnose23(
        processor::register_pair& r2,
        processor::register_pair& r3
    ) noexcept -> condition_code {
        condition_code cc;
        __asm__ volatile(
            "0: diag    %[r2],%[r3],%[code]\n"
            "1: ipm     %[cc]\n"
            "     srl     %[cc],28\n"
            EX_TABLE(0b, 1b, EX_TYPE_FIXUP, 0)
            : [r2] "+d" (r2.pair), [r3] "+d" (r3.pair), [cc] "=d" (cc)
            : [code] "K" (Code)
            : "cc", "memory"
        );
        return cc;
    }

    /// Generic diagnose13 — issue any code with arbitrary R2/R3 register values.
    /// @tparam Code  The 16-bit diagnose code (e.g. 0x044, 0x308).
    /// @param  r1    Value placed in R1 (and R1+1 for processor::register_pair codes).
    /// @param  r3    Value placed in R3 (and R2+1).
    /// @return       Condition code (0–3) set by the hypervisor/hardware.
    template <u16 Code>
    [[gnu::always_inline]] static auto diagnose13(u64& r1, u64& r3) noexcept -> condition_code {
        condition_code cc;
        __asm__ volatile(
            "0: diag    %[r1],%[r3],%[code]\n"
            "1: ipm     %[cc]\n"
            "     srl     %[cc],28\n"
            EX_TABLE(0b, 1b, EX_TYPE_FIXUP, 0)
            : [r1] "+d" (r1), [r3] "+d" (r3), [cc] "=d" (cc)
            : [code] "K" (Code)
            : "cc", "memory"
        );
        return cc;
    }

    /// @brief Generic diagnose13 (register-pair variant) — issue any code with full
    ///        128-bit R1:R1+1 and R3:R3+1 register pairs.
    /// @tparam Code  The 16-bit diagnose code (e.g. 0x288).
    /// @param  r1    128-bit register pair (even=high, odd=low).
    /// @param  r3    128-bit register pair (even=high, odd=low).
    /// @return       Condition code (0–3) set by the hypervisor/hardware.
    template <u16 Code>
    [[gnu::always_inline]] static auto diagnose13(
        processor::register_pair& r1,
        processor::register_pair& r3
    ) noexcept -> condition_code {
        condition_code cc;
        __asm__ volatile(
            "0: diag    %[r1],%[r3],%[code]\n"
            "1: ipm     %[cc]\n"
            "     srl     %[cc],28\n"
            EX_TABLE(0b, 1b, EX_TYPE_FIXUP, 0)
            : [r1] "+d" (r1.pair), [r3] "+d" (r3.pair), [cc] "=d" (cc)
            : [code] "K" (Code)
            : "cc", "memory"
        );
        return cc;
    }

    /// @brief Issue a shutdown command
    /// @warning DIAG X'1F' is only available on the 9221
    ///          An SPECIFICATION EXCEPTION will be thrown if not the correct model.
    auto diag1f() noexcept -> void {
        // "POWEROFF in EBCDIC
        u64 r1 = 0xd7d6e6c5;
        u64 r3 = 0xd9d6c6c6;
        diagnose13<DIAG_SHUTDOWN>(r1, r3);
    }

    /// @brief Issue a CP command string with an optional response buffer.
    /// @param cmd     CP command (EBCDIC, null-terminated).
    /// @param cmd_len Length of the command string.
    /// @param resp    Response buffer (may be nullptr).
    /// @param resp_len Response buffer capacity (bytes).
    /// @return        Bytes written to resp, or 0 on failure.
    auto diag8_cpcmd(const char* cmd, u64 cmd_len,
                     char* resp, u64 resp_len) noexcept -> u64 {
        u64 r1_even = reinterpret_cast<uptr>(cmd);
        u64 r2_even = reinterpret_cast<uptr>(resp);
        processor::register_pair r1{ .even = r1_even, .odd = cmd_len };
        processor::register_pair r2{ .even = r2_even, .odd = resp_len };
        if (diagnose12<DIAG_CONSOLE_FN>(r1, r2) != 0) {
            return 0;
        }
        // R2.odd returns response length on success
        return processor::register_pair{ .pair = r2.pair }.odd;
    }

    /// @brief Issue DIAG 8 with a simple message (no response).
    /// @param addr  Message buffer address (EBCDIC).
    /// @param len   Message length in bytes.
    /// @note  Used by the Hercules console driver.
    auto diag8(const char* addr, const u64 len) noexcept -> void {
        register u64 r2 __asm__("2") = reinterpret_cast<uptr>(addr);
        register u64 r3 __asm__("3") = len;
        u64 r2v = r2;
        u64 r3v = r3;
        diagnose23<DIAG_CONSOLE_FN>(r2v, r3v);
    }

    /// @brief Query CPU timing data via Pseudo Timer.
    /// @param cpu     Physical CPU address.
    /// @param buf     Output buffer (must be below 2 GiB for z/VM).
    /// @param len     Buffer length.
    /// @return        0 on success, non-zero on failure.
    auto diag0c(u16 cpu, void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(cpu) << 32) | len;
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_PSEUDO_TIMER>(r1, r2);
    }

    /// @brief Inform the hypervisor that a range of pages is no longer needed.
    /// @param start First byte address (page-aligned).
    /// @param end   Last byte address (exclusive, page-aligned).
    /// @return      Condition code.
    auto diag10(u64 start, u64 end) noexcept -> condition_code {
        processor::register_pair r1{ .even = start, .odd = end };
        processor::register_pair r2{};
        return diagnose12<DIAG_RELEASE_PAGES>(r1, r2);
    }

    /// @brief Interact with z/VM spool files.
    /// @param fn     Function (subcode).
    /// @param buf    Parameter buffer.
    /// @param len    Buffer length.
    /// @return       Condition code.
    auto diag14(diag14_fn fn, void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(fn) << 32) | len;
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_SPOOL_FILE>(r1, r2);
    }

    /// @brief Yield the current virtual CPU's timeslice (z/VM / KVM).
    /// @note  Used inside spin-loops; equivalent to cpu_relax() under a hypervisor.
    auto diag44() noexcept -> void {
        u64 r1 = 0;
        u64 r2 = 0;
        diagnose12<DIAG_TIMESLICE_END>(r1, r2);
    }

    /// @brief Manipulate named saved segments (DCSS).
    /// @param fn   Function subcode.
    /// @param name Segment name (8-byte EBCDIC).
    /// @param buf  Parameter block.
    /// @return     Condition code.
    auto diag64(diag64_fn fn, const void* name, void* buf) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(fn) << 32) | reinterpret_cast<uptr>(name);
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_NSS_MANIP>(r1, r2);
    }

    /// @brief Query 3270 terminal device attributes from z/VM.
    /// @param devno  Device number (0–0xFFFF).
    /// @param buf    Output parameter block (below 2 GiB).
    /// @return       Condition code.
    auto diag8c(u16 devno, void* buf) noexcept -> condition_code {
        u64 r1 = static_cast<u64>(devno) << 32;
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_3270_DISPLAY>(r1, r2);
    }

    /// @brief Yield the current virtual CPU and favor a target virtual CPU.
    /// @param[in] cpu_addr Target virtual CPU address in bits 48–63 of Rx.
    auto diag9c(u16 cpu_addr) noexcept -> void {
        u64 r1 = static_cast<u64>(cpu_addr) << 48;
        u64 r2 = 0;
        diagnose12<DIAG_RELINQUISH_DIR>(r1, r2);
    }

    /// @brief Interface with the z/VM Application Data Service.
    /// @param subcode  0 = query, 1 = collect, etc.
    /// @param buf      Parameter block.
    /// @param len      Buffer length.
    /// @return         Condition code.
    auto diag0dc(u32 subcode, void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(subcode) << 32) | len;
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_APPLDATA>(r1, r2);
    }

    /// @brief Query LPAR CPU utilization data.
    /// @param subcode  4 (STIB4 simple), 5 (RSI), 6 (STIB6 extended), 7 (STIB7).
    /// @param buf      Output buffer (must be below 2 GiB — GFP_DMA).
    /// @param len      Buffer length / returned length.
    /// @return         Condition code.
    auto diag204(u32 subcode, void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(subcode) << 32) | len;
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_CPU_UTIL>(r1, r2);
    }

    /// @brief Query virtual device information from z/VM.
    /// @param blk  Parameter block (set vdevnr + dev_fc on input).
    /// @return     Condition code.
    auto diag210(diag210_t& blk) noexcept -> condition_code {
        u64 r1 = reinterpret_cast<uptr>(&blk);
        u64 r2 = 0;
        return diagnose12<DIAG_DEVICE_INFO>(r1, r2);
    }

    /// @brief Retrieve the CPU type name table from z/VM.
    /// @param buf   Output buffer (must be below 2 GiB — GFP_DMA).
    /// @param len   Buffer capacity.
    /// @return      Condition code.
    auto diag224(void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = reinterpret_cast<uptr>(buf);
        u64 r2 = len;
        return diagnose12<DIAG_EBCDIC_NAME>(r1, r2);
    }

    /// @brief Perform DASD block I/O through z/VM (DIAG250).
    /// @param fn   0 = init_io, 1 = rw_io.
    /// @param blk  I/O parameter block.
    /// @return     Condition code / device status.
    auto diag250(u32 fn, void* blk) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(fn) << 32) | reinterpret_cast<uptr>(blk);
        u64 r2 = 0;
        return diagnose12<DIAG_BLOCK_IO>(r1, r2);
    }

    /// @brief Establish or cancel page-reference token (collaborative memory mgmt).
    /// @param establish  true = establish, false = cancel.
    /// @param token      Page-reference token (R1.odd on establish; unused on cancel).
    /// @return           Condition code.
    auto diag258(bool establish, u64 token) noexcept -> condition_code {
        processor::register_pair r1{};
        processor::register_pair r2{};
        if (establish) {
            r1.even = 0;       // establish subcode
            r1.odd  = token;
        } else {
            r1.even = 1;       // cancel subcode
            r1.odd  = 0;
        }
        return diagnose12<DIAG_PAGE_REF>(r1, r2);
    }

    /// @brief Query machine memory layout.
    /// @param buf   Output buffer.
    /// @param len   Buffer capacity.
    /// @return      Condition code.
    auto diag260(void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = reinterpret_cast<uptr>(buf);
        u64 r2 = len;
        return diagnose12<DIAG_STORAGE_CFG>(r1, r2);
    }

    /// @brief Query VNIC / MAC information from z/VM.
    /// @param subcode 0x24 (VNIC info) or 0x30 (MAC address).
    /// @param buf     Parameter / output block.
    /// @return        Condition code.
    auto diag26c(u32 subcode, void* buf) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(subcode) << 32) | reinterpret_cast<uptr>(buf);
        u64 r2 = 0;
        return diagnose12<DIAG_CERTAIN_SYSINFO>(r1, r2);
    }

    /// @brief Issue DIAGNOSE 0x288 (LPAR/hypervisor watchdog).
    /// @tparam func    Watchdog function.
    /// @param  timeout Timeout in seconds (15–3600).
    /// @param  action  LPAR: LPARWDT_RESTART (0); z/VM: physical address of an
    ///                 EBCDIC CP command string.
    /// @param  len     Length of the z/VM command string (LPAR: 0).
    /// @return 0 on success, nonzero condition code on failure.
    auto diag288(diag288_fn func, u64 action, u32 timeout, u32 len) noexcept
        -> condition_code
    {
        processor::register_pair r1 { .even = static_cast<u32>(func), .odd = timeout };
        processor::register_pair r3 { .even = action, .odd = len };
        return diagnose13<DIAG_WATCHDOG>(r1, r3);
    }

    /// @brief Interface with z/VM FTP server.
    /// @param buf   Parameter block.
    /// @param len   Buffer length.
    /// @return      Condition code.
    auto diag2c4(void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = reinterpret_cast<uptr>(buf);
        u64 r2 = len;
        return diagnose12<DIAG_FTP>(r1, r2);
    }

    /// @brief Retrieve z/VM guest performance data (hypfs).
    /// @param buf   Output buffer.
    /// @param len   Buffer capacity.
    /// @return      Condition code.
    auto diag2fc(void* buf, u32 len) noexcept -> condition_code {
        u64 r1 = reinterpret_cast<uptr>(buf);
        u64 r2 = len;
        return diagnose12<DIAG_GUEST_PERF>(r1, r2);
    }

    /// @brief Set or query LPAR partition resource parameters.
    /// @param subcode 0 (set weights), 1 (query PRP), 2 (set capping).
    /// @param buf     Parameter block.
    /// @return        Condition code.
    auto diag304(u32 subcode, void* buf) noexcept -> condition_code {
        u64 r1 = (static_cast<u64>(subcode) << 32) | reinterpret_cast<uptr>(buf);
        u64 r2 = 0;
        return diagnose12<DIAG_PART_RESOURCE>(r1, r2);
    }

    /// @brief Perform list-directed IPL functions.
    /// @param subcode IPL subcode.
    /// @param addr    Parameter block address (or 0 for reset functions).
    /// @return        Condition code.
    auto diag308(diag308_fn subcode, void* addr) noexcept -> condition_code {
        u64 r1 = static_cast<u64>(subcode);
        u64 r2 = reinterpret_cast<uptr>(addr);
        return diagnose12<DIAG_IPL>(r1, r2);
    }

    /// @brief Query memory topology map.
    /// @param subcode 0 (query subcodes), 1 (stride), 4 (size), 5 (store map).
    /// @param buf     Output / parameter buffer.
    /// @return        Condition code.
    auto diag310(u32 subcode, void* buf) noexcept -> condition_code {
        u64 r1 = static_cast<u64>(subcode);
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_MEM_TOPOLOGY>(r1, r2);
    }

    /// @brief Set the CP Name Code and CP Version Code to identify the OS.
    /// @param cpnc  CP Name Code (use CPNC_LINUX for Linux).
    /// @param cpvc  CP Version Code (typically 0).
    /// @return      Condition code.
    auto diag318(u64 cpnc, u64 cpvc) noexcept -> condition_code {
        processor::register_pair r1{ .even = cpnc, .odd = cpvc };
        processor::register_pair r2{};
        return diagnose12<DIAG_CP_NAME_VER>(r1, r2);
    }

    /// @brief Manage hardware certificate store.
    /// @param subcode Query / store / list subcode.
    /// @param buf     Parameter / certificate block.
    /// @return        Condition code.
    auto diag320(u32 subcode, void* buf) noexcept -> condition_code {
        u64 r1 = static_cast<u64>(subcode);
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_CERT_STORE>(r1, r2);
    }


    /// @brief Read power/energy consumption data.
    /// @param subcode 0 (query caps), 1 (query PIB format), 2 (store PIB).
    /// @param buf     Output buffer.
    /// @return        Condition code.
    auto diag324(u32 subcode, void* buf) noexcept -> condition_code {
        u64 r1 = static_cast<u64>(subcode);
        u64 r2 = reinterpret_cast<uptr>(buf);
        return diagnose12<DIAG_POWER_INFO>(r1, r2);
    }

    /// @brief Register for or acknowledge a Warning-Track Interruption.
    /// @param subcode 0 (ACK), 1 (REG).
    /// @return        Condition code.
    auto diag49c(u32 subcode) noexcept -> condition_code {
        u64 r1 = static_cast<u64>(subcode);
        u64 r2 = 0;
        return diagnose12<DIAG_WARN_TRACK>(r1, r2);
    }

    /// @brief KVM virtio-ccw hypercall or boot-time storage limit query.
    /// @param subcode  0 (virtio notify) or 4 (storage limit).
    /// @param param2   virtio: subchannel_id; storage-limit: unused (0).
    /// @return         Condition code.
    auto diag500(u64 subcode, u64 param2) noexcept -> condition_code {
        processor::register_pair r1{ .even = subcode, .odd = param2 };
        processor::register_pair r2{};
        return diagnose12<DIAG_VIRTIO>(r1, r2);
    }

} // namespace arch::s390x::cpu::diag
