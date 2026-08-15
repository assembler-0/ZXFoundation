/// SPDX-License-Identifier: Apache 2.0
/// @file diag.cxxm
/// @brief idiomatic C++23 DIAG 8 hypervisor console driver.

module drivers.console.diag.core;
import zxfoundation.base.types;
import zxfoundation.sys.printk.types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sched.context;
import arch.s390x.cpu.diag;
import arch.s390x.cpu.irq;
import lib.charconv;
import std;

namespace drivers::console::diag::detail {

    /// @brief Encapsulates a Hercules-compatible DIAG 8 message.
    class hercules_message {
    public:
        static constexpr usize MAX_CAPACITY = 256;
        static constexpr usize PREFIX_SIZE  = 9;   // "MSGNOH * " = 9 bytes

    private:
        static constexpr u8 EBCDIC_M  = 0xD4U;  // M
        static constexpr u8 EBCDIC_S  = 0xE2U;  // S
        static constexpr u8 EBCDIC_G  = 0xC7U;  // G
        static constexpr u8 EBCDIC_N  = 0xD5U;  // N
        static constexpr u8 EBCDIC_O  = 0xD6U;  // O
        static constexpr u8 EBCDIC_H  = 0xC8U;  // H
        static constexpr u8 EBCDIC_SP = 0x40U;  // space
        static constexpr u8 EBCDIC_ST = 0x5CU;  // *

        char m_data[MAX_CAPACITY];
        usize m_total_len{PREFIX_SIZE};

    public:
        /// @brief Constructs a message with the MSGNOH prefix.
        constexpr hercules_message() noexcept {
            m_data[0] = static_cast<char>(EBCDIC_M);   // M
            m_data[1] = static_cast<char>(EBCDIC_S);   // S
            m_data[2] = static_cast<char>(EBCDIC_G);   // G
            m_data[3] = static_cast<char>(EBCDIC_N);   // N
            m_data[4] = static_cast<char>(EBCDIC_O);   // O
            m_data[5] = static_cast<char>(EBCDIC_H);   // H
            m_data[6] = static_cast<char>(EBCDIC_SP);  // ' '
            m_data[7] = static_cast<char>(EBCDIC_ST);  // *
            m_data[8] = static_cast<char>(EBCDIC_SP);  // ' '
        }

        /// @brief Transcribes an ASCII string into the EBCDIC message buffer.
        /// @param s The ASCII input string.
        auto transcribe(std::string_view s) noexcept -> void {
            constexpr usize payload_limit = MAX_CAPACITY - PREFIX_SIZE;
            const usize payload_len   = std::min(s.size(), payload_limit);

            for (usize i = 0; i < payload_len; ++i) {
                const u8 ebcdic_byte = lib::charconv::ascii_to_ebcdic(static_cast<u8>(s[i]));
                m_data[PREFIX_SIZE + i] = static_cast<char>(ebcdic_byte);
            }

            m_total_len = PREFIX_SIZE + payload_len;
        }

        /// @brief Transmit the message via the DIAG 8 instruction.
        auto transmit() const noexcept -> void {
            arch::s390x::cpu::diag::diag8(m_data, m_total_len);
        }
    };

    /// @brief Hardware-level DIAG emission lock.
    alignas(4) no_export zxfoundation::sync::qspinlock::qspinlock g_diag_lock;

} // namespace detail

namespace drivers::console::diag {

    /// @brief Flush ASCII content to the DIAG 8 console.
    /// @param[in] s The ASCII string view to transmit.
    auto write(const std::string_view s) noexcept -> void {
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        detail::g_diag_lock.lock();

        std::string_view remaining = s;
        constexpr usize payload_limit = detail::hercules_message::MAX_CAPACITY - detail::hercules_message::PREFIX_SIZE;

        while (!remaining.empty()) {
            const usize chunk_len = std::min(remaining.size(), payload_limit);
            detail::hercules_message msg;
            msg.transcribe(remaining.substr(0, chunk_len));
            msg.transmit();
            remaining = remaining.substr(chunk_len);
        }

        detail::g_diag_lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);
    }

} // namespace drivers::console::diag
