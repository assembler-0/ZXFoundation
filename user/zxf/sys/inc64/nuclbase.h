/// SPDX-License-Identifier: Apache-2.0
/// @file user/zxf/sys/inc64/nuclbase.h
/// @brief ZXFoundation nucleus raw syscall API.
///
/// @name Resilient/OS package info
/// @{
///     vendor:      ZXF
///     class:       SYS
///     component:   INC64
///     install_ds:  'ZXF.SYS.INC64(NUCLBASE)'
///     unix_path:   $/zxf/sys/inc64/nuclbase
/// @}
///
/// @name Vendor-specific info
/// @{
///     cmake_master_target:   libzxf
///     cmake_this_target:     libzxf.nuclbase
///     cmake_depends:         <none>
/// @}
///
/// @note This header contains the raw definitions and no documentation on how to use it.
///       Please refer to the kernel documentation/interface for further guidance.

#pragma once

/// TODO: Get build system working and finish `libzxf.internal` for foundational types
typedef __UINT64_TYPE__  nb_u64;
typedef __UINT32_TYPE__  nb_u32;
typedef __UINT16_TYPE__  nb_u16;
typedef __UINT8_TYPE__   nb_u8;
typedef __INT64_TYPE__   nb_i64;
typedef __UINTPTR_TYPE__ nb_uptr;

#define NB_ERR ((nb_u64) ~ (nb_u64)0)
#define NB_OK(v) ((v) != NB_ERR)

// namespace 0: dom
#define NB_DOM_CREATE            0
#define NB_DOM_DESTROY           1
#define NB_DOM_EXIT              2
#define NB_DOM_INFO              3
#define NB_DOM_QUERY             4
#define NB_DOM_STRAND_CREATE     5
#define NB_DOM_STRAND_SET_POLICY 6
#define NB_DOM_STRAND_SET_AFFIN  7
#define NB_DOM_STRAND_QUERY      8
#define NB_DOM_STRAND_JOIN       9
#define NB_DOM_STRAND_EXIT       10
#define NB_DOM_EXEC 11

// namespace 1: mem
#define NB_MEM_ALLOC   32
#define NB_MEM_FREE    33
#define NB_MEM_MAP     34
#define NB_MEM_UNMAP   35
#define NB_MEM_PROTECT 36
#define NB_MEM_FUTEX_WAIT 38
#define NB_MEM_FUTEX_WAKE 39

// namespace 2: gate / pc-ss
#define NB_GATE_CREATE  64
#define NB_GATE_DESTROY 65
#define NB_GATE_QUERY   66
#define NB_GATE_DEFER   67
#define NB_GATE_RESUME  68

// namespace 3: portal
#define NB_PORTAL_CREATE  96
#define NB_PORTAL_DESTROY 97

// namespace 4: io
#define NB_IO_BIND               128
#define NB_IO_UNBIND             129
#define NB_IO_SUBMIT             130
#define NB_IO_DMA_EXTENT         132
#define NB_IO_COMPLETION_DEQUEUE 133

// namespace 5: sgn
#define NB_SGN_REGISTER 160
#define NB_SGN_POLL     161
#define NB_SGN_ACK      162
#define NB_SGN_SEND     163
#define NB_SGN_MASK_SET 164
#define NB_SGN_MASK_GET 165
#define NB_SGN_WAIT     166

// namespace 6: cap
#define NB_CAP_ACQUIRE  192
#define NB_CAP_RELEASE  193
#define NB_CAP_VALIDATE 194
#define NB_CAP_REVOKE   195

// namespace 7: sys / ipc / shm / policy
#define NB_SYS_YIELD             224
#define NB_SYS_DEADLINE          225
#define NB_SYS_NOW               226
#define NB_SYS_SLEEP             244
/// @warning This syscall number will be removed in the near future
#define NB_SYS_CONSOLE           227
#define NB_SVC_REGISTER          228
#define NB_SVC_LOOKUP            229
#define NB_SVC_UNREGISTER        230
#define NB_IPC_ENDPOINT_CREATE   231
#define NB_IPC_ENDPOINT_DESTROY  232
#define NB_IPC_SEND              233
#define NB_IPC_RECEIVE           234
#define NB_IPC_REPLY             241
#define NB_IPC_CALL              242
#define NB_SIGNAL_RETURN         243
#define NB_SHM_EXPORT            235
#define NB_SHM_IMPORT            236
#define NB_SHM_MAP               237
#define NB_POLICY_REGISTER       238
#define NB_POLICY_UNREGISTER     239
#define NB_SYS_POLICY_CONSULT    245
#define NB_POLICY_CONSULT        NB_SYS_POLICY_CONSULT
#define NB_SYS_LAST_ERROR        240

#define NB_CALL(nr, a0, a1, a2, a3, a4)                       \
    __extension__({                                           \
        register nb_u64 _r2 __asm__("r2") = (nb_u64)(a0);     \
        register nb_u64 _r3 __asm__("r3") = (nb_u64)(a1);     \
        register nb_u64 _r4 __asm__("r4") = (nb_u64)(a2);     \
        register nb_u64 _r5 __asm__("r5") = (nb_u64)(a3);     \
        register nb_u64 _r6 __asm__("r6") = (nb_u64)(a4);     \
        __asm__ volatile("svc %5"                             \
                         : "+r"(_r2), "+r"(_r3), "+r"(_r4),   \
                           "+r"(_r5), "+r"(_r6)               \
                         : "i"(nr)                            \
                         : "r0", "r1", "memory", "cc");       \
        _r2;                                                  \
    })

#define NB_GENERIC(name, nr)                                             \
    static inline nb_u64 name(nb_u64 a0, nb_u64 a1, nb_u64 a2,          \
                              nb_u64 a3, nb_u64 a4) {                    \
        return NB_CALL(nr, a0, a1, a2, a3, a4);                         \
    }

static inline int nb_console_write(const char* msg, nb_u64 len) {
    return NB_OK(NB_CALL(NB_SYS_CONSOLE, msg, len, 0, 0, 0));
}

static inline void nb_dom_exit() {
    NB_CALL(NB_DOM_EXIT, 0, 0, 0, 0, 0);
}

static inline nb_u64 nb_dom_info(nb_u64 id) {
    return NB_CALL(NB_DOM_INFO, id, 0, 0, 0, 0);
}

static inline nb_u64 nb_dom_query(nb_u64 index) {
    return NB_CALL(NB_DOM_QUERY, index, 0, 0, 0, 0);
}

static inline nb_u64 nb_dom_create(nb_u64 token, const char* name) {
    return NB_CALL(NB_DOM_CREATE, token, name, 0, 0, 0);
}

static inline nb_u64 nb_dom_destroy(nb_u64 token, nb_u64 domain_id) {
    return NB_CALL(NB_DOM_DESTROY, token, domain_id, 0, 0, 0);
}

static inline nb_u64 nb_gate_create(nb_u64 token, nb_u64 type, nb_u64 auth,
                                    nb_u64 target_domain, nb_u64 entry_va) {
    return NB_CALL(NB_GATE_CREATE, token, type, auth, target_domain, entry_va);
}

static inline nb_u64 nb_gate_query(nb_u64 token, nb_u64 gate_id) {
    return NB_CALL(NB_GATE_QUERY, token, gate_id, 0, 0, 0);
}

static inline nb_u64 nb_gate_destroy(nb_u64 token, nb_u64 gate_id) {
    return NB_CALL(NB_GATE_DESTROY, token, gate_id, 0, 0, 0);
}

NB_GENERIC(nb_gate_defer, NB_GATE_DEFER)
NB_GENERIC(nb_gate_resume, NB_GATE_RESUME)

NB_GENERIC(nb_dom_strand_create, NB_DOM_STRAND_CREATE)
NB_GENERIC(nb_dom_strand_join, NB_DOM_STRAND_JOIN)
NB_GENERIC(nb_dom_strand_exit, NB_DOM_STRAND_EXIT)
/// dom_op::exec — (dom_id, image_va, image_len): exec-analog on a fresh
/// domain; loads the ELF and admits its primary strand (hereditary caps).
NB_GENERIC(nb_dom_exec, NB_DOM_EXEC)
NB_GENERIC(nb_dom_strand_set_policy, NB_DOM_STRAND_SET_POLICY)
NB_GENERIC(nb_dom_strand_set_affin, NB_DOM_STRAND_SET_AFFIN)
NB_GENERIC(nb_dom_strand_query, NB_DOM_STRAND_QUERY)

NB_GENERIC(nb_mem_alloc, NB_MEM_ALLOC)
NB_GENERIC(nb_mem_free, NB_MEM_FREE)
NB_GENERIC(nb_mem_map, NB_MEM_MAP)
NB_GENERIC(nb_mem_unmap, NB_MEM_UNMAP)
NB_GENERIC(nb_mem_protect, NB_MEM_PROTECT)

NB_GENERIC(nb_portal_create, NB_PORTAL_CREATE)
NB_GENERIC(nb_portal_destroy, NB_PORTAL_DESTROY)

NB_GENERIC(nb_io_bind, NB_IO_BIND)
NB_GENERIC(nb_io_unbind, NB_IO_UNBIND)
NB_GENERIC(nb_io_submit, NB_IO_SUBMIT)
NB_GENERIC(nb_io_dma_extent, NB_IO_DMA_EXTENT)
NB_GENERIC(nb_io_completion_dequeue, NB_IO_COMPLETION_DEQUEUE)

NB_GENERIC(nb_sgn_register, NB_SGN_REGISTER)
NB_GENERIC(nb_sgn_poll, NB_SGN_POLL)
NB_GENERIC(nb_sgn_ack, NB_SGN_ACK)
NB_GENERIC(nb_sgn_send, NB_SGN_SEND)
NB_GENERIC(nb_sgn_mask_set, NB_SGN_MASK_SET)
NB_GENERIC(nb_sgn_mask_get, NB_SGN_MASK_GET)
NB_GENERIC(nb_sgn_wait, NB_SGN_WAIT)

NB_GENERIC(nb_cap_acquire, NB_CAP_ACQUIRE)
NB_GENERIC(nb_cap_release, NB_CAP_RELEASE)
NB_GENERIC(nb_cap_validate, NB_CAP_VALIDATE)
NB_GENERIC(nb_cap_revoke, NB_CAP_REVOKE)

NB_GENERIC(nb_sys_yield, NB_SYS_YIELD)
NB_GENERIC(nb_sys_deadline, NB_SYS_DEADLINE)
NB_GENERIC(nb_sys_now, NB_SYS_NOW)
NB_GENERIC(nb_sys_sleep, NB_SYS_SLEEP)

NB_GENERIC(nb_svc_register, NB_SVC_REGISTER)
NB_GENERIC(nb_svc_lookup, NB_SVC_LOOKUP)
NB_GENERIC(nb_svc_unregister, NB_SVC_UNREGISTER)

NB_GENERIC(nb_mem_futex_wait, NB_MEM_FUTEX_WAIT)
NB_GENERIC(nb_mem_futex_wake, NB_MEM_FUTEX_WAKE)

NB_GENERIC(nb_ipc_endpoint_create, NB_IPC_ENDPOINT_CREATE)
NB_GENERIC(nb_ipc_endpoint_destroy, NB_IPC_ENDPOINT_DESTROY)
NB_GENERIC(nb_ipc_send, NB_IPC_SEND)
NB_GENERIC(nb_ipc_receive, NB_IPC_RECEIVE)
NB_GENERIC(nb_ipc_reply, NB_IPC_REPLY)
NB_GENERIC(nb_ipc_call, NB_IPC_CALL)
NB_GENERIC(nb_signal_return, NB_SIGNAL_RETURN)

NB_GENERIC(nb_shm_export, NB_SHM_EXPORT)
NB_GENERIC(nb_shm_import, NB_SHM_IMPORT)
NB_GENERIC(nb_shm_map, NB_SHM_MAP)

NB_GENERIC(nb_policy_register, NB_POLICY_REGISTER)
NB_GENERIC(nb_policy_unregister, NB_POLICY_UNREGISTER)
NB_GENERIC(nb_last_error, NB_SYS_LAST_ERROR)
NB_GENERIC(nb_policy_consult, NB_POLICY_CONSULT)

#undef NB_GENERIC
