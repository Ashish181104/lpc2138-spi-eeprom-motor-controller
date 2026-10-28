.syntax unified
.cpu arm7tdmi
.arm

.section .vectors, "ax", %progbits
.align 2
.global _start

_start:
    b Reset_Handler
    b Undefined_Handler
    b SWI_Handler
    b PrefetchAbort_Handler
    b DataAbort_Handler
    b Reserved_Handler
    b IRQ_Handler
    b FIQ_Handler

.section .text.startup, "ax", %progbits
.align 2
.global Reset_Handler
.extern main

Reset_Handler:
    ldr sp, =_stack_top

    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata

copy_data:
    cmp r1, r2
    bhs clear_bss_start
    ldrb r3, [r0], #1
    strb r3, [r1], #1
    b copy_data

clear_bss_start:
    ldr r1, =_sbss
    ldr r2, =_ebss
    mov r3, #0

clear_bss:
    cmp r1, r2
    bhs call_main
    strb r3, [r1], #1
    b clear_bss

call_main:
    bl main

stop:
    b stop

Undefined_Handler:
    b Undefined_Handler

SWI_Handler:
    b SWI_Handler

PrefetchAbort_Handler:
    b PrefetchAbort_Handler

DataAbort_Handler:
    b DataAbort_Handler

Reserved_Handler:
    b Reserved_Handler

IRQ_Handler:
    b IRQ_Handler

FIQ_Handler:
    b FIQ_Handler
