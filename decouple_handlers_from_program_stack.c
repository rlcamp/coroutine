#include "decouple_handlers_from_program_stack.h"

void decouple_handlers_from_program_stack(void) {
#ifdef __arm__
    /* calling this function before any stack switching ensures that interrupt handlers
     effectively get their own single dedicated call stack, rather than each coroutine stack
     needing to have enough extra headroom to support running the largest interrupt handler.
     this can significantly reduce the total amount of sram required to dedicate to coroutine
     call stacks on an embedded processor. call this from main(), or setup() in arduino code.
     should work on all cortex-m processors. you still need to make sure task stacks have at
     least 104 extra bytes for register storage (less if not using FPU or there isn't one) */
    static char handler_stack[4096] __attribute((aligned(16)));
    asm volatile("cpsid i\n" /* disable irq */
                 "mrs r0, msp\n" /* assuming we are in thread mode using msp, copy current sp */
                 "msr psp, r0\n" /* and store it in psp */
                 "mrs r0, control\n" /* get value of control register */
                 "mov r1, #2\n" /* must be done in two insns because of thumb restrictions */
                 "orr r0, r1\n" /* set bit 1 of control register, to use psp in thread mode */
                 "msr control, r0\n" /* store modified value in control register */
                 "isb\n" /* memory barrier after switching stacks */
                 "msr msp, %0\n" /* set handler stack pointer to top of handler stack */
                 "cpsie i\n" /* enable irq */
                 : : "r"(handler_stack + sizeof(handler_stack)) : "r0", "r1");
#endif
}
