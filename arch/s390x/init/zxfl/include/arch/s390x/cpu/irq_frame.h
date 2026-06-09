#pragma once

/// Frame field offsets — used by entry.S to store/load without C structs.
#define IRQ_FRAME_GPRS      0x00U
#define IRQ_FRAME_PSW_MASK  0x80U
#define IRQ_FRAME_PSW_ADDR  0x88U
/// Total frame size: 16 GPRs (128) + PSW mask (8) + PSW addr (8) = 144 = 0x90.
#define IRQ_FRAME_SIZE      0x90U