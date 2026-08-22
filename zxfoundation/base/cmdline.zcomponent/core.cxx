/// SPDX-License-Identifier: Apache 2.0
/// @file cmdline.cxxm
/// @brief Kernel command-line parameter registry and parser.

module zxfoundation.base.cmdline.core;
import zxfoundation.base.types;
import lib.error;
import lib.string_parser;
import lib.cstring;
import lib.static_vector;
import std;

namespace zxfoundation::base::cmdline::detail {

    no_export lib::static_vector<param, MAX_PARAMS> g_params{};

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
                        result = result * 10 + static_cast<u64>(c - '0');
                    }
                    *static_cast<u64*>(p->storage) = result;
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