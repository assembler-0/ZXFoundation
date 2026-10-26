/// @file arch/s390x/init/zxfl/include/arch/s390x/cpu/irq_frame.h
#pragma once

#define IRQ_FRAME_GPRS      0x00U
#define IRQ_FRAME_PSW_MASK  0x80U
#define IRQ_FRAME_PSW_ADDR  0x88U
#define IRQ_FRAME_ARS       0x90U
#define IRQ_FRAME_FPC       0xD0U
#define IRQ_FRAME_VRS       0xD8U
#define IRQ_FRAME_SIZE      0x02D8U

#define IRQ_FRAME_BIAS      2048U