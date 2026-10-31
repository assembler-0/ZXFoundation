/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/base/cmdline.zcomponent/core.cxx
/// @brief Kernel command-line parameter registry and parser.

module zxfoundation.base.cmdline.core;
import zxfoundation.base.types;
import lib.error;
import lib.string_parser;
import lib.cstring;
import lib.static_vector;
import lib.stl;

namespace zxfoundation::base::cmdline::detail {

    static lib::static_vector<param, MAX_PARAMS> g_params{};

}

namespace zxfoundation::base::cmdline {
    
    /// @brief Look up a registered parameter by name.
    /// @return Pointer to the param, or nullptr.
    [[nodiscard]] auto find(std::string_view name) noexcept -> param* {
        using namespace detail;
        for (auto & g_param : g_params) {
            if (g_param.name == name) return &g_param;
        }
        return nullptr;
    }

    /// @brief Register a cmdline parameter.
    /// @return true on success, false if table is full or duplicate name.
    auto add(const param& p) noexcept -> bool {
        using namespace detail;

        if (find(p.name)) return false;
        if (g_params.full()) return false;

        auto result = g_params.push_back(p);
        return result.has_value();
    }

    /// @brief Parse a complete command line against registered parameters.
    auto parse(std::string_view cmdline) noexcept -> void {
        lib::tokenizer tok{cmdline};

        while (auto kv = tok.next_pair()) {
            auto& [key, val] = *kv;
            auto* p = find(key);
            if (!p) continue; // unknown parameter, skip silently

            if (p->handler) {
                p->handler(val);
                continue;
            }

            switch (p->type) {
            case value_type::flag:
                if (p->storage) *static_cast<bool*>(p->storage) = true;
                break;

            case value_type::boolean:
                if (p->storage) {
                    if (val.empty() || val == "1" || val == "on"
                        || val == "true" || val == "yes" || val == "y") {
                        *static_cast<bool*>(p->storage) = true;
                    } else {
                        *static_cast<bool*>(p->storage) = false;
                    }
                }
                break;

            case value_type::u64:
                if (p->storage && !val.empty()) {
                    u64 result = 0;
                    for (char c : val) {
                        if (c < '0' || c > '9') break;
                        const u64 next
                            = result * 10U + static_cast<u64>(c - '0');
                        if (next < result) {
                            result = ~0ULL; // saturate on overflow
                            break;
                        }
                        result = next;
                    }
                    // Store exactly the declared target width — a fixed
                    // 8-byte store corrupts narrower storage fields.
                    const usize width =
                        p->storage_size != 0U ? p->storage_size : 8U;
                    if (width == 1U) {
                        *static_cast<u8*>(p->storage) =
                            static_cast<u8>(result);
                    } else if (width == 2U) {
                        *static_cast<u16*>(p->storage) =
                            static_cast<u16>(result);
                    } else if (width == 4U) {
                        *static_cast<u32*>(p->storage) =
                            static_cast<u32>(result);
                    } else {
                        *static_cast<u64*>(p->storage) = result;
                    }
                }
                break;

            case value_type::str:
                if (p->storage && p->storage_size > 0 && !val.empty()) {
                    lib::string_copy(
                        static_cast<char*>(p->storage),
                        p->storage_size,
                        val
                    );
                }
                break;

            case value_type::callback:
                break;
            }
        }
    }

} // namespace zxfoundation::base::cmdline