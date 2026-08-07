/// SPDX-License-Identifier: Apache-2.0
/// @file vm_types.cxx
/// @brief Virtual Memory Engine — type definitions implementation

module zxfoundation.memory.vm_types;
import zxfoundation.memory.kmalloc;

namespace zxfoundation::memory::vm {

    constexpr u32 VM_PAGE_MAP_INITIAL = 16U;

    [[nodiscard]] auto vm_page_map::reserve_capacity(u32 min_capacity) noexcept -> bool {
        if (capacity >= min_capacity) return true;

        u32 new_cap = capacity ? capacity : VM_PAGE_MAP_INITIAL;
        while (new_cap < min_capacity) {
            if (new_cap > (~0U / 2U)) return false;
            new_cap *= 2U;
        }

        auto* new_slots = static_cast<vm_page_slot*>(
            kmalloc::kmalloc(static_cast<u64>(new_cap) * sizeof(vm_page_slot)));
        if (!new_slots) return false;
        for (u32 i = 0; i < count; ++i) {
            new (static_cast<void*>(&new_slots[i])) vm_page_slot{slots[i]};
        }
        if (slots) kmalloc::kfree(slots);
        slots = new_slots;
        capacity = new_cap;
        return true;
    }

    [[nodiscard]] auto vm_page_map::reserve_page(u64 page_va) noexcept -> vm_page_slot* {
        vm_page_slot* free_slot = nullptr;
        for (u32 i = 0; i < count; ++i) {
            if (slots[i].matches(page_va)) return &slots[i];
            if (!free_slot && slots[i].state == vm_page_state::empty) {
                free_slot = &slots[i];
            }
        }
        if (free_slot) {
            *free_slot = vm_page_slot{};
            free_slot->va_page = page_va;
            return free_slot;
        }
        if (count == capacity && !reserve_capacity(count + 1U)) return nullptr;
        auto* s = &slots[count++];
        new (static_cast<void*>(s)) vm_page_slot{};
        s->va_page = page_va;
        return s;
    }

    auto vm_page_map::release_page(u64 page_va) noexcept -> bool {
        for (u32 i = 0; i < count; ++i) {
            if (slots[i].matches(page_va)) {
                slots[i].clear();
                // rmap entries retain this slot index.  Do not compact by
                // moving the tail into a hole: that would retarget a live
                // reverse-map entry at a different VA/physical frame.
                while (count != 0U &&
                       slots[count - 1U].state == vm_page_state::empty) {
                    --count;
                }
                return true;
            }
        }
        return false;
    }

    auto vm_page_map::destroy() noexcept -> void {
        if (slots) {
            for (u32 i = 0; i < count; ++i) slots[i].clear();
            kmalloc::kfree(slots);
        }
        slots = nullptr;
        capacity = 0;
        count = 0;
    }

} // namespace zxfoundation::memory::vm
