#pragma once

/// Frame field offsets — used by entry.S to store/load without C structs.
#define IRQ_FRAME_GPRS      0x00U
#define IRQ_FRAME_PSW_MASK  0x80U
#define IRQ_FRAME_PSW_ADDR  0x88U
#define IRQ_FRAME_ARS       0x90U
#define IRQ_FRAME_FPC       0xD0U
#define IRQ_FRAME_VRS       0xD8U
/// Total frame size: 144 + 64 (ARs) + 4 (FPC) + 4 (pad) + 512 (VRs) = 728 = 0x2D8.
#define IRQ_FRAME_SIZE      0x02D8U

/// Size of the ABI save area (C++ register save + padding) between r15 and
/// the IRQ frame data.  Increased from 160 to 2048 to ensure the C++ call
/// chain in the handler (do_pgm_check → ... → cap_release → domain_find)
/// cannot overflow its callee-saved register save area into the IRQ frame.
#define IRQ_FRAME_BIAS      2048U