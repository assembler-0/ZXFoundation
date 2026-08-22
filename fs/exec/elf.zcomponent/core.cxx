/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/exec/loader.cxxm
/// @brief ELF64 loader — loads s390x executables into domain address spaces.

module fs.exec.elf.core;
import fs.exec.elf.types;
import zxfoundation.dgp.domain.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.vm;
import zxfoundation.memory.vm.types;
import zxfoundation.memory.rmap;
import zxfoundation.sys.printk.core;
import zxfoundation.base.types;
import arch.s390x.mmu.core;
import arch.s390x.mmu.types;
import arch.s390x.mmu.consts;
import arch.s390x.cpu.skey;
import lib.error;
import std;

namespace fs::exec::elf::detail {

    namespace pmm  = zxfoundation::memory::pmm;
    namespace hhdm = zxfoundation::memory::hhdm;
    namespace mmu  = arch::s390x::mmu;
    namespace skey = arch::s390x::cpu::skey;
    namespace vm  = zxfoundation::memory::vm;
    namespace rmap = zxfoundation::memory::rmap;

    /// @brief Default user stack virtual address (top of stack, grows down).
    constexpr u64 USER_STACK_TOP  = 0x0000'0080'0000'0000ULL; // 512 GiB
    constexpr u32 USER_STACK_PAGES = 4;  // 16 KiB stack

    /// @brief Convert ELF segment flags to MMU page protection.
    [[nodiscard]] auto flags_to_prot(u32 p_flags) noexcept -> mmu::page_prot {
        const bool w = (p_flags & elf::PF_W) != 0;
        const bool x = (p_flags & elf::PF_X) != 0;

        if (w && x)  return mmu::PAGE_KERNEL_RWX;
        if (w)       return mmu::PAGE_KERNEL_RW;
        if (x)       return mmu::PAGE_KERNEL_RX;
        return mmu::PAGE_KERNEL_RO;
    }

    /// @brief Convert ELF segment flags to high-level vm_prot.
    [[nodiscard]] auto elf_flags_to_vm_prot(u32 p_flags) noexcept
        -> vm::vm_prot
    {
        vm::vm_prot p = vm::vm_prot::read;
        if (p_flags & elf::PF_W) p = p | vm::vm_prot::write;
        if (p_flags & elf::PF_X) p = p | vm::vm_prot::exec;
        return p;
    }

    /// @brief Fail-closed validation of a physical frame before it is touched
    ///        through the HHDM or installed into a domain mapping.
    /// @param[in] phys  Candidate physical base address (page-aligned).
    /// @return Empty on success; kernel_error if @p phys is outside the recorded
    ///         HHDM coverage or beyond the PMM-managed range.
    [[nodiscard]] auto validate_user_frame(u64 phys) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        const bool aligned = (phys & (PAGE_SIZE - 1U)) == 0U;
        const bool hhdm_mapped = hhdm::phys_range_is_mapped(phys, PAGE_SIZE);
        const auto* desc = pmm::frame_desc_of(phys);
        if (!aligned || !hhdm_mapped || desc == nullptr) [[unlikely]] {
            zxfoundation::sys::printk::perr(
                "elf: user frame rejected phys={:#x} aligned={} hhdm={} coverage_end={:#x} desc={}",
                phys, aligned, hhdm_mapped, hhdm::phys_coverage_end(), desc != nullptr);
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::invalid_address));
        }
        return {};
    }

    /// @brief Allocate a single physical page, zero it, and return its phys address.
    [[nodiscard]] auto alloc_user_page() noexcept
        -> std::expected<u64, lib::kernel_error>
    {
        auto result = pmm::alloc(
            0, // order 0 = single page
            pmm::zone::normal,
            pmm::purpose::generic,
            pmm::page_init::zeroed);
        if (!result) [[unlikely]]
            return std::unexpected(result.error());

        // Fail closed before the caller dereferences hhdm::phys_to_virt(phys).
        if (auto v = validate_user_frame(result->phys_base); !v) [[unlikely]]
            return std::unexpected(v.error());
        return result->phys_base;
    }

    /// @brief Load a single PT_LOAD segment into the domain's address space.
    [[nodiscard]] auto load_segment(
        zxfoundation::dgp::domain::domain* d,
        const elf::elf64_phdr& phdr,
        const u8* elf_data
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const u64 seg_start  = phdr.p_vaddr & PAGE_MASK;
        const u64 seg_end    = (phdr.p_vaddr + phdr.p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
        const u64 seg_size   = seg_end - seg_start;
        const mmu::page_prot mmu_prot = flags_to_prot(phdr.p_flags);
        const vm::vm_prot    vprot    = elf_flags_to_vm_prot(phdr.p_flags);

        const vm::vm_flags vflags =
            vm::vm_flags::fixed | vm::vm_flags::user |
            vm::vm_flags::anonymous | vm::vm_flags::no_merge;
        auto map_r = vm::vm_map_fixed(
            d->vm_space, seg_start, seg_size, vprot, vflags,
            vm::vm_backing::anonymous, 0);
        if (!map_r) [[unlikely]] {
            zxfoundation::sys::printk::perr(
                "elf: segment vm region rejected va={:#x} size={:#x} prot={} error={}",
                seg_start, seg_size, static_cast<u32>(vprot), map_r.error());
            return std::unexpected(map_r.error());
        }

        auto* rgn = vm::vm_find(d->vm_space, seg_start);
        if (!rgn) [[unlikely]] {
            discard_value vm::vm_unmap(
                d->vm_space, d->root_phys, seg_start, seg_size);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }

        for (u64 va = seg_start; va < seg_end; va += PAGE_SIZE) {
            if (!vm::vm_space_try_charge_commit(d->vm_space, 1U)) {
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, seg_start, seg_size);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::out_of_memory));
            }
            const auto release_commit = [&]() noexcept -> void {
                vm::vm_space_uncharge_commit(d->vm_space, 1U);
            };

            // Allocate a physical page.
            auto phys_r = alloc_user_page();
            if (!phys_r) [[unlikely]] {
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, seg_start, seg_size);
                return std::unexpected(phys_r.error());
            }
            const u64 phys = *phys_r;

            auto pin = pmm::frame_ref(phys);
            if (!pin) [[unlikely]] {
                discard_value pmm::free(pmm::folio_active{
                    phys, 0U, pmm::purpose::generic, pmm::zone::normal});
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, seg_start, seg_size);
                return std::unexpected(pin.error());
            }

            auto metadata = pmm::frame_set_mapping_metadata(
                phys, rgn->storage_object.id, rgn->storage_object.generation,
                (vprot & vm::vm_prot::write) != vm::vm_prot::none);
            if (!metadata) [[unlikely]] {
                discard_value pmm::frame_release_pin(phys);
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, seg_start, seg_size);
                return std::unexpected(metadata.error());
            }

            const u64 page_virt_start = va;
            auto* page_ptr = reinterpret_cast<u8*>(hhdm::phys_to_virt(phys));

            if (page_virt_start < phdr.p_vaddr + phdr.p_filesz) {
                const u64 copy_start = (page_virt_start > phdr.p_vaddr)
                    ? page_virt_start : phdr.p_vaddr;
                const u64 copy_end = (page_virt_start + PAGE_SIZE < phdr.p_vaddr + phdr.p_filesz)
                    ? page_virt_start + PAGE_SIZE : phdr.p_vaddr + phdr.p_filesz;

                if (copy_end > copy_start) {
                    const u64 file_offset = phdr.p_offset + (copy_start - phdr.p_vaddr);
                    const u64 page_offset = copy_start - page_virt_start;
                    const u64 copy_len = copy_end - copy_start;
                    std::copy_n(elf_data + file_offset, copy_len, page_ptr + page_offset);
                }
            }

            const mmu::dat_table<mmu::dat_level::region_1> root{d->root_phys};
            auto hw_r = mmu::map_page<mmu::dat_level::region_1>(
                root, va, phys, mmu_prot);
            if (!hw_r) [[unlikely]] {
                discard_value pmm::frame_release_pin(phys);
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, seg_start, seg_size);
                return std::unexpected(hw_r.error());
            }

            const u8 key_byte = skey::make_skey(d->storage_key, false);
            skey::sske(phys, key_byte);

            auto* slot = rgn->reserve_page(va);
            if (!slot) [[unlikely]] {
                discard_value mmu::unmap_page_dispatch(
                    d->vm_space.asce_val, va);
                discard_value pmm::frame_release_pin(phys);
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, seg_start, seg_size);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::out_of_memory));
            }
            slot->phys  = phys;
            slot->state = vm::vm_page_state::resident;
            slot->flags = 0U;
            discard_value rgn->resident.fetch_add(1, std::memory_order::relaxed);
            discard_value rgn->committed.fetch_add(1, std::memory_order::relaxed);
            d->vm_space.total_resident += 1;

            const u32 slot_index = static_cast<u32>(slot - rgn->pages.slots);
            auto rmap_result = rmap::insert(*rgn, slot_index, phys);
            if (!rmap_result) [[unlikely]] {
                zxfoundation::sys::printk::perr(
                    "elf: segment rmap insertion rejected va={:#x} phys={:#x} error={}",
                    va, phys, rmap_result.error());
                slot->clear();
                discard_value mmu::unmap_page_dispatch(d->vm_space.asce_val, va);
                discard_value pmm::frame_release_pin(phys);
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, seg_start, seg_size);
                return std::unexpected(rmap_result.error());
            }
        }

        return {};
    }

    /// @brief Set up a user stack for the domain.
    [[nodiscard]] auto setup_user_stack(zxfoundation::dgp::domain::domain* d) noexcept
        -> std::expected<u64, lib::kernel_error>
    {
        const u64 stack_bottom = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
        const u64 stack_size   = static_cast<u64>(USER_STACK_PAGES) * PAGE_SIZE;

        const vm::vm_prot  vprot  = vm::vm_prot::rw;
        const vm::vm_flags vflags =
            vm::vm_flags::fixed | vm::vm_flags::user |
            vm::vm_flags::anonymous | vm::vm_flags::stack |
            vm::vm_flags::no_merge;
        auto map_r = vm::vm_map_fixed(
            d->vm_space, stack_bottom, stack_size, vprot, vflags,
            vm::vm_backing::anonymous, 0);
        if (!map_r) [[unlikely]]
            return std::unexpected(map_r.error());

        auto* rgn = vm::vm_find(d->vm_space, stack_bottom);
        if (!rgn) [[unlikely]] {
            discard_value vm::vm_unmap(
                d->vm_space, d->root_phys, stack_bottom, stack_size);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }

        for (u32 i = 0; i < USER_STACK_PAGES; ++i) {
            const u64 va = stack_bottom + i * PAGE_SIZE;

            if (!vm::vm_space_try_charge_commit(d->vm_space, 1U)) {
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, stack_bottom, stack_size);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::out_of_memory));
            }
            const auto release_commit = [&]() noexcept -> void {
                vm::vm_space_uncharge_commit(d->vm_space, 1U);
            };

            auto phys_r = alloc_user_page();
            if (!phys_r) [[unlikely]] {
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, stack_bottom, stack_size);
                return std::unexpected(phys_r.error());
            }
            const u64 phys = *phys_r;

            auto pin = pmm::frame_ref(phys);
            if (!pin) [[unlikely]] {
                discard_value pmm::free(pmm::folio_active{
                    phys, 0U, pmm::purpose::generic, pmm::zone::normal});
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, stack_bottom, stack_size);
                return std::unexpected(pin.error());
            }

            auto metadata = pmm::frame_set_mapping_metadata(
                phys, rgn->storage_object.id, rgn->storage_object.generation, true);
            if (!metadata) [[unlikely]] {
                discard_value pmm::frame_release_pin(phys);
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, stack_bottom, stack_size);
                return std::unexpected(metadata.error());
            }

            const mmu::dat_table<mmu::dat_level::region_1> root{d->root_phys};
            auto hw_r = mmu::map_page<mmu::dat_level::region_1>(
                root, va, phys, mmu::PAGE_KERNEL_RW);
            if (!hw_r) [[unlikely]] {
                discard_value pmm::frame_release_pin(phys);
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, stack_bottom, stack_size);
                return std::unexpected(hw_r.error());
            }

            const u8 key_byte = skey::make_skey(d->storage_key, false);
            skey::sske(phys, key_byte);

            auto* slot = rgn->reserve_page(va);
            if (!slot) [[unlikely]] {
                discard_value mmu::unmap_page_dispatch(
                    d->vm_space.asce_val, va);
                discard_value pmm::frame_release_pin(phys);
                release_commit();
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, stack_bottom, stack_size);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::out_of_memory));
            }
            slot->phys  = phys;
            slot->state = vm::vm_page_state::resident;
            slot->flags = 0U;
            discard_value rgn->resident.fetch_add(1, std::memory_order::relaxed);
            discard_value rgn->committed.fetch_add(1, std::memory_order::relaxed);
            d->vm_space.total_resident += 1;

            const u32 slot_index = static_cast<u32>(slot - rgn->pages.slots);
            auto rmap_result = rmap::insert(*rgn, slot_index, phys);
            if (!rmap_result) [[unlikely]] {
                slot->clear();
                discard_value mmu::unmap_page_dispatch(d->vm_space.asce_val, va);
                discard_value pmm::frame_release_pin(phys);
                discard_value vm::vm_unmap(
                    d->vm_space, d->root_phys, stack_bottom, stack_size);
                return std::unexpected(rmap_result.error());
            }
        }

        // Return stack top (SP points to top, grows down).
        return USER_STACK_TOP;
    }

} // namespace zxfoundation::exec::detail

namespace fs::exec::elf {

    /// @brief Load an ELF64 binary into a domain's address space.
    /// @param d         Target domain (must have valid root_phys and storage_key).
    /// @param elf_data  Pointer to the ELF binary in kernel memory.
    /// @param elf_size  Size of the ELF binary in bytes.
    /// @return Entry point virtual address on success, or kernel_error.
    [[nodiscard]] auto load(
        zxfoundation::dgp::domain::domain* d,
        const void* elf_data,
        u64 elf_size
    ) noexcept -> std::expected<u64, lib::kernel_error> {
        using namespace detail;

        if (!d || !elf_data || elf_size < sizeof(elf::elf64_ehdr))
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        const auto* raw = static_cast<const u8*>(elf_data);
        const auto& ehdr = *reinterpret_cast<const elf::elf64_ehdr*>(raw);

        if (!elf::elf64_validate(ehdr))
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        const u64 ph_end = ehdr.e_phoff +
            static_cast<u64>(ehdr.e_phnum) * sizeof(elf::elf64_phdr);
        if (ph_end > elf_size)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        const auto* phdrs = reinterpret_cast<const elf::elf64_phdr*>(raw + ehdr.e_phoff);
        u32 loaded = 0;

        for (u16 i = 0; i < ehdr.e_phnum; ++i) {
            const auto& ph = phdrs[i];

            if (ph.p_type != elf::PT_LOAD) continue;
            if (ph.p_memsz == 0) continue;

            if (ph.p_offset + ph.p_filesz > elf_size)
                return std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

            auto r = load_segment(d, ph, raw);
            if (!r) [[unlikely]]
                return std::unexpected(r.error());

            ++loaded;
        }

        if (loaded == 0)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));

        auto stack_r = setup_user_stack(d);
        if (!stack_r) [[unlikely]]
            return std::unexpected(stack_r.error());

        d->primary_strand.body->uregs.gprs[15] = *stack_r;

        return ehdr.e_entry;
    }

} // namespace zxfoundation::exec
