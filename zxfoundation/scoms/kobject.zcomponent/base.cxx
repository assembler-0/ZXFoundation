/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/scoms/scoms_types.cxxm
/// @brief SCOMS — Scalable Object Management System — core types.

module zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.types;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.sync.qspinlock.core;
import std;

namespace zxfoundation::scoms::kobject::detail {

    no_export bobject_finalizer_fn g_bobject_finalizer{nullptr};

}

namespace zxfoundation::scoms::kobject {

    /// @brief Install the SCOMS metadata finalizer used after the last put.
    auto install_bobject_finalizer(bobject_finalizer_fn finalizer) noexcept -> void {
        using detail::g_bobject_finalizer;
        if (!finalizer) __builtin_trap();
        if (g_bobject_finalizer && g_bobject_finalizer != finalizer) {
            __builtin_trap();
        }
        g_bobject_finalizer = finalizer;
    }

}

/// @name struct bobject
/// @{
namespace zxfoundation::scoms::kobject {

    /// @brief Validate and perform one canonical lifecycle transition.
    [[nodiscard]] auto bobject::transition(lifecycle_state target) noexcept -> bool {
        switch (target) {
            case lifecycle_state::embryo:
                if (state != lifecycle_state::destroyed) return false;
                break;
            case lifecycle_state::sealed:
                if (state != lifecycle_state::embryo) return false;
                break;
            case lifecycle_state::active:
                if (state != lifecycle_state::embryo &&
                    state != lifecycle_state::sealed &&
                    state != lifecycle_state::suspended) return false;
                break;
            case lifecycle_state::suspended:
                if (state != lifecycle_state::active) return false;
                break;
            case lifecycle_state::dying:
                if (state != lifecycle_state::embryo &&
                    state != lifecycle_state::active &&
                    state != lifecycle_state::suspended) return false;
                break;
            case lifecycle_state::destroyed:
                if (state != lifecycle_state::dying) return false;
                break;
        }
        state = target;
        return true;
    }

    /// @brief Acquire a lifetime reference without resurrecting retirement.
    [[nodiscard]] auto bobject::try_get() noexcept -> bool {
        return ref.try_get();
    }

    /// @brief Drop a lifetime reference and reclaim on the final put.
    auto bobject::put() noexcept -> void {
        if (!ref.put()) return;
        if (!detail::g_bobject_finalizer) __builtin_trap();
        detail::g_bobject_finalizer(*this);
    }

    /// @brief Reset identity and relationship metadata for a new slot use.
    auto bobject::initialize(u32 object_id, kobject::kobject_type object_type) noexcept -> void {
        id = object_id;
        type = object_type;
        state = lifecycle_state::embryo;
        ref.count.store(1, memory_order::relaxed);
        object_flags = 0;
    }

} // namespace zxfoundation::scoms
/// @}