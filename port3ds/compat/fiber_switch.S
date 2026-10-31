@ Cooperative context switch for the Win32 fiber shim (compat/win32_compat.cpp).
@
@ A context is the AAPCS callee-saved state: r4-r11, lr, sp, then the VFP
@ callee-saved registers d8-d15 (hard-float ABI). 26 words.
@
@   void host3ds_fiber_switch(uint32_t *from, const uint32_t *to);
@
@ A new fiber's context holds r4 = start routine, r5 = its parameter and
@ lr = host3ds_fiber_entry, so the first switch into it "returns" there.

    .arm
    .syntax unified
    .fpu    vfp
    .text

    .global host3ds_fiber_switch
    .type   host3ds_fiber_switch, %function
host3ds_fiber_switch:
    mov     r2, sp
    stmia   r0!, {r4-r11, lr}
    str     r2, [r0], #4
    vstmia  r0!, {d8-d15}
    ldmia   r1!, {r4-r11, lr}
    ldr     r2, [r1], #4
    mov     sp, r2
    vldmia  r1!, {d8-d15}
    bx      lr
    .size   host3ds_fiber_switch, . - host3ds_fiber_switch

    .global host3ds_fiber_entry
    .type   host3ds_fiber_entry, %function
host3ds_fiber_entry:
    mov     r0, r5
    blx     r4
    @ A fiber routine must not return (Win32 would end the thread).
1:  b       1b
    .size   host3ds_fiber_entry, . - host3ds_fiber_entry
