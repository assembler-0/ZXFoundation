// SPDX-License-Identifier: Apache-2.0
// @file user/init/init.S
// @brief ZXFoundation user-space init binary

    .text
    .align 8
    .globl _start
    .type _start, @function

_start:
    // Print startup message
    larl    %r2, msg_start
    lghi    %r3, msg_start_len
    svc     21                  // console_write(buf=r2, len=r3)

    // Print init complete message
    larl    %r2, msg_ready
    lghi    %r3, msg_ready_len
    svc     21                  // console_write(buf=r2, len=r3)

    // Idle loop — yield to scheduler forever
.Lidle:
    svc     18                  // time_yield
    j       .Lidle

    .align 8
msg_start:
    .ascii  "[init] ZXFoundation init domain started\n"
    .equ    msg_start_len, . - msg_start

msg_ready:
    .ascii  "[init] System initialization complete\n"
    .equ    msg_ready_len, . - msg_ready
