/*
 Copyright 2015-2022 Richard Campbell
 
 Permission to use, copy, modify, and/or distribute this software for any purpose with or without
 fee is hereby granted, provided that the above copyright notice and this permission notice appear
 in all copies.
 
 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
 THIS SOFTWARE.
 
 Coroutines for generator functions, sequential pipelines, state machines, and other uses in C
 
 Significant conceptual contribution by Rich Felker and others
 Implementation by Richard Campbell
 
 Tested on cortex-m bare metal, avr bare metal, cortex-a7 linux, aarch64 iOS, aarch64 linux, mips,
 x86_64 darwin, x86_64 linux hardware, others (i386, m68k, riscv, ppc64, sh4, or1k) via qemu
 
 The intended use case is where concurrency, but not parallelism, is required, with a possibly very
 high rate of switching between coroutines, in a possibly hard-realtime context (such as an audio
 subsystem).
 
 This implementation relies only on a couple platform-specific inline assembly macros, and is
 therefore useful in uncooperative environments such as iOS or bare metal. The remainder of the
 implementation is platform-agnostic C.
 
 Unlike protothreads and other stackless methods, each coroutine has its own fully functional call
 stack, so local variables in the coroutines preserve their values, multiple instances of the same
 function may be in flight at once, and coroutines may yield from anywhere within the call stack,
 not just the top level function.
 
 Unlike pthreads and other kernel-scheduled preemptive multitasking thread implementations, it is
 extremely cheap to switch between coroutines, and hard-realtime requirements may be met while doing
 so. Thus, it is possible to have a generator-type coroutine that yields individual audio samples
 (or small batches of them) to a hard-realtime callback function inside an audio subsystem. It would
 be possible to implement this API using pthreads, but the cost of switching between coroutines
 would be many orders of magnitude greater, and it would not be possible to meet hard-realtime
 deadlines.
 
 This code consists of the coroutine starting and switching primitives, as well as some convenience
 functions (yield, from) for implementing generator functions, sequential pipelines, and things of
 that sort. These generator functions may yield a pointer to local storage, a pointer to heap
 storage (which may or may not be expected to be freed by the yielded-to code), or any nonzero value
 that can fit within the memory occupied by a void pointer. A coroutine may NOT yield NULL, as this
 has special meaning to the accepting code (it indicates to a waiting parent that a child function
 has already returned, or indicates to a waiting child that a parent function wants it to clean up
 and return).
 */

#include "coroutine.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)

#define CONTEXT_BUF_SIZE_BYTES 24
#define STACK_ALIGNMENT 64

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("rdx") = buf; /* force the argument into this register. this is necessary so that it doesn't choose rbp even though it's not in the clobber list */ \
register void * _func asm("rsi") = func; /* force the argument into this register */ \
asm volatile( \
"lea 1f(%%rip), %%r8\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"movq %%r8, 0(%0)\n" /* store it to the beginning of the buffer */ \
"movq %%rsp, 8(%0)\n" /* store the stack pointer */ \
"movq %%rbp, 16(%0)\n" /* store the frame pointer */ \
"movq %0, %%rsp\n" /* switch to the new stack pointer (the new stack is immediately below the context buffer) */ \
"movq %0, %%rdi\n" /* copy the stack pointer, which is also the argument, to the first argument register */ \
"call *%1\n" /* call the springboard fn (never returns, but using call instead of jmp properly aligns the stack) */ \
"1:\n" /* marker for address of end of this block, so we can jump to here */ \
: "+r"(_buf), "+r"(_func) : : "rdi", "rcx", "rax", "rbx", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc", "fpsr", "flags", "memory"); } while(0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("rdi") = buf; /* force the argument into this register */ \
asm volatile( \
"movq 0(%0), %%rsi\n" /* load the saved instruction pointer */ \
"movq 8(%0), %%rdx\n" /* load the saved stack pointer */ \
"movq 16(%0), %%rcx\n" /* load the saved frame pointer */ \
"lea 1f(%%rip), %%r8\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"movq %%r8, 0(%0)\n" /* store it to the beginning of the buffer */ \
"movq %%rsp, 8(%0)\n" /* store the stack pointer */ \
"movq %%rbp, 16(%0)\n" /* store the frame pointer */ \
"movq %%rdx, %%rsp\n" /* switch to the loaded stack pointer */ \
"movq %%rcx, %%rbp\n" /* switch to the loaded frame pointer */ \
"jmp *%%rsi\n" /* jump to the loaded address (the end of this block of asm, but in the buf context) */ \
"1:\n" /* marker for address of end of this block, so we can jump to here */ \
: "+r"(_buf) : : "rsi", "rdx", "rcx", "rax", "rbx", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc", "fpsr", "flags", "memory"); } while(0)

#elif defined(__aarch64__)

#define CONTEXT_BUF_SIZE_BYTES 32
#define STACK_ALIGNMENT 64

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("x0") = buf; \
register void * _func asm("x1") = func; \
asm volatile( \
"adr x4, 0f\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"mov x5, sp\n" /* get the current stack pointer */ \
"stp x4, x5, [%0]\n" /* store the old pc and sp */ \
"stp x29, x18, [%0, #16]\n" /* store the old fp and r18 (which cannot be clobbered on apple, and harmless to special case it on others) */ \
"mov x5, %0\n" /* load new sp */ \
"mov sp, x5\n" /* switch to the new sp */ \
"br %1\n" /* jump to the child function */ \
"0:\n" \
: "+r"(_buf), "+r"(_func) : : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory"); } while (0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("x0") = buf; \
asm volatile( \
"ldp x6, x7, [%0]\n" /* load the saved pc and sp */ \
"ldp x8, x9, [%0, #16]\n" /* load the saved fp and apple thing */ \
"adr x4, 0f\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"mov x5, sp\n" /* get the current stack pointer */ \
"stp x4, x5, [%0]\n" /* store the old pc and sp */ \
"stp x29, x18, [%0, #16]\n" /* store the old fp and apple thing */ \
"mov x29, x8\n" /* move the new fp into place */ \
"mov x18, x9\n" /* move the new apple thing into place */ \
"mov sp, x7\n" /* switch to the new sp */ \
"br x6\n" /* jump to the new pc */ \
"0:\n" \
: "+r"(_buf) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory"); } while (0)

#elif defined(__arm__)
#define CONTEXT_BUF_SIZE_BYTES 4
#define STACK_ALIGNMENT 8

#if defined(__thumb2__) || !defined(__thumb__)
/* everything but armv6-m */

#if __thumb__
#define SET_LSB_IN_LR_IF_THUMB "orr lr, #1\n"
#else
#define SET_LSB_IN_LR_IF_THUMB ""
#endif

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("r0") = buf; /* ensure the compiler places this where it will be the argument to func */ \
register void * _func asm("r1") = func; /* ensure the compiler does not place this in a frame pointer register */ \
asm volatile( \
"adr lr, 0f\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
SET_LSB_IN_LR_IF_THUMB /* handle thumb addressing where the lsb is set if the jump target should remain in thumb mode */ \
"push {r11, lr}\n" /* save the future pc value as well as possible frame pointer (which is not allowed in the clobber list) */ \
"mov r5, sp\n" /* grab the current stack pointer... */ \
"str r5, [%0]\n" /* and save it the context buffer */ \
"mov sp, %0\n" /* set the stack pointer to the top of the space below the context buffer */ \
"bx %1\n" /* jump to the child function */ \
".balign 4\n" /* not sure if this is necessary except on thumb-1 */ \
"0:\n" : "+r"(_buf), "+r"(_func) : : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r12", "lr", "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "q8", "q9", "q10", "q11", "q12", "q13", "q14", "q15", "cc", "memory"); } while(0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("r0") = buf; \
asm volatile( \
"adr lr, 0f\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
SET_LSB_IN_LR_IF_THUMB /* handle thumb addressing where the lsb is set if the jump target should remain in thumb mode */ \
"push {r11, lr}\n" /* save the future pc value as well as possible frame pointer (which is not allowed in the clobber list) */ \
"ldr r6, [%0]\n" /* load the saved stack pointer from the context buffer */ \
"mov r4, sp\n" /* grab the current value of the stack pointer... */ \
"str r4, [%0]\n" /* and save it in the context buffer */ \
"mov sp, r6\n" /* restore the previously saved stack pointer */ \
"pop {r11, pc}\n" /* jump to the previously saved pc value */ \
".balign 4\n" \
"0:\n" : "+r"(_buf) : : "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r12", "lr", "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "q8", "q9", "q10", "q11", "q12", "q13", "q14", "q15", "cc", "memory"); } while(0)

#else
/* armv6-m */

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("r0") = buf; /* ensure the compiler places this where it will be the argument to func */ \
register void * _func asm("r1") = func; /* ensure the compiler does not place this in a frame pointer register */ \
asm volatile( \
"adr r6, 0f\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"add r6, #1\n" /* handle thumb addressing where the lsb is set if the jump target should remain in thumb mode */ \
"push {r6}\n" /* save the resulting future pc for restoration when switching back to here */ \
"push {r7}\n" /* explicitly save the frame pointer too (which is not allowed in the clobber list) */ \
"mov r5, sp\n" /* grab the current stack pointer... */ \
"str r5, [%0]\n" /* and save it the context buffer */ \
"mov sp, %0\n" /* set the stack pointer to the top of the space below the context buffer */ \
"bx %1\n" /* jump to the child function */ \
".balign 4\n" /* thumb-1 requires this additional alignment constraint for adr targets */ \
"0:\n" : "+r"(_buf), "+r"(_func) : : "r2", "r3", "r4", "r5", "r6", "r8", "r9", "r10", "r11", "r12", "lr", "cc", "memory"); } while(0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("r0") = buf; \
asm volatile( \
"adr r6, 0f\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"add r6, #1\n" /* handle thumb addressing where the lsb is set if the jump target should remain in thumb mode */ \
"push {r6}\n" /* save the resulting future pc for restoration when switching back to here */ \
"push {r7}\n" /* explicitly save the frame pointer too (which is not allowed in the clobber list) */ \
"ldr r6, [%0]\n" /* load the saved stack pointer from the context buffer */ \
"mov r4, sp\n" /* grab the current value of the stack pointer... */ \
"str r4, [%0]\n" /* and save it in the context buffer */ \
"mov sp, r6\n" /* restore the previously saved stack pointer */ \
"pop {r7, pc}\n" /* jump to the previously saved pc value */ \
".balign 4\n" \
"0:\n" : "+r"(_buf) : : "r1", "r2", "r3", "r4", "r5", "r6", "r8", "r9", "r10", "r11", "r12", "lr", "cc", "memory"); } while(0)

#endif

#elif defined(__i386__)

#define CONTEXT_BUF_SIZE_BYTES 12
#define STACK_ALIGNMENT 16

#define CLOBBER_BASE "edi", "ecx", "eax", "ebx", "cc", "fpsr", "flags", "memory"
#define CLOBBER_MMX "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7"
#define CLOBBER_SSE "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",  "xmm6", "xmm7"

#if defined(__SSE__)
#define CLOBBER CLOBBER_BASE, CLOBBER_MMX, CLOBBER_SSE
#elif defined(__MMX__)
#define CLOBBER CLOBBER_BASE, CLOBBER_MMX
#else
#define CLOBBER CLOBBER_BASE
#endif

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("edx") = buf; \
register void * _func asm("esi") = func; \
asm volatile( \
"call 0f\n" /* in i386 and other architectures, we need to do some trickery to load the address of a label */ \
"0: pop %%eax\n" \
"add $(1f-0b), %%eax\n" \
"mov %%eax, 0(%0)\n" \
"mov %%esp, 4(%0)\n" \
"mov %%ebp, 8(%0)\n" \
"mov %0, %%esp\n" \
"mov %0, %%edi\n" \
"sub $12, %%esp\n" /* decrement sp by 12 before pushing, to enforce 16 byte alignment */ \
"push %0\n" /* argument is now both on stack and in edi, so either call convention will succeed */ \
"call *%1\n" \
"1:\n" \
: "+r"(_buf), "+r"(_func) : : CLOBBER); } while(0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("edx") = buf; \
asm volatile( \
"mov 0(%0), %%esi\n" \
"mov 4(%0), %%edi\n" \
"mov 8(%0), %%ecx\n" \
"call 0f\n" /* in i386 and other architectures, we need to do some trickery to load the address of a label */ \
"0: pop %%eax\n" \
"add $(1f-0b), %%eax\n" \
"mov %%eax, 0(%0)\n" \
"mov %%esp, 4(%0)\n" \
"mov %%ebp, 8(%0)\n" \
"mov %%edi, %%esp\n" \
"mov %%ecx, %%ebp\n" \
"jmp *%%esi\n" \
"1:\n" \
: "+r"(_buf) : : "esi", CLOBBER); } while(0)

#elif defined(__riscv)

#define CONTEXT_BUF_SIZE_BYTES 32
#define STACK_ALIGNMENT 64

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("x10") = buf; \
register void * _func asm("x11") = func; \
asm volatile( \
"la x6, 1f\n" \
"sd x6, 0(%0)\n" \
"sd x2, 8(%0)\n" \
"sd x8, 16(%0)\n" \
"sd x1, 24(%0)\n" \
"mv x2, %0\n" \
"jr %1\n" \
"1:\n" \
: "+r"(_buf), "+r"(_func) : : "x0", "x1", "x3", "x4", "x5", "x6", "x7", "x9", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31", "cc", "memory"); } while (0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("x10") = buf; \
asm volatile( \
"ld x11, 0(%0)\n" \
"ld x12, 8(%0)\n" \
"ld x13, 16(%0)\n" \
"ld x14, 24(%0)\n" \
"la x6, 1f\n" \
"sd x6, 0(%0)\n" \
"sd x2, 8(%0)\n" \
"sd x8, 16(%0)\n" \
"sd x1, 24(%0)\n" \
"mv x2, x12\n" \
"mv x8, x13\n" \
"mv x1, x14\n" \
"jr x11\n" \
"1:\n" \
: "+r"(_buf) : : "x0", "x1", "x3", "x4", "x5", "x6", "x7", "x9", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31", "cc", "memory"); } while (0)

#elif defined(__m68k__)

#define CONTEXT_BUF_SIZE_BYTES 16
#define STACK_ALIGNMENT 16

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("d0") = buf; \
register void * _func asm("d1") = func; \
asm volatile( \
"lea 1f, %%a0\n" \
"move.l %%a0, 0(%0)\n" \
"move.l %%sp, 4(%0)\n" \
"move.l %%a6, 8(%0)\n" \
"move.l %%a5, 12(%0)\n" \
"move.l %0, %%sp\n" \
"subq #4, %%sp\n" /* enforce stack alignment */ \
"move.l %0, (%%sp)\n" \
"jsr (%1)\n" /* this is a call rather than jmp because of stack alignment requirements */ \
"1:\n" \
: "+r"(_buf), "+r"(_func) : : "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "cc", "memory"); } while (0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("d0") = buf; \
asm volatile( \
"move.l 0(%0), %%a1\n" \
"move.l 4(%0), %%a2\n" \
"move.l 8(%0), %%a3\n" \
"move.l 12(%0), %%a4\n" \
"lea 1f, %%a0\n" \
"move.l %%a0, 0(%0)\n" \
"move.l %%sp, 4(%0)\n" \
"move.l %%a6, 8(%0)\n" \
"move.l %%a5, 12(%0)\n" \
"move.l %%a2, %%sp\n" \
"move.l %%a3, %%a6\n" \
"move.l %%a4, %%a5\n" \
"jmp (%%a1)\n" \
"1:\n" \
: "+r"(_buf) : : "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "cc", "memory"); } while (0)

#elif defined(__AVR__)

#define CONTEXT_BUF_SIZE_BYTES 8
#define STACK_ALIGNMENT 1

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("r30") = buf; /* address of the context struct (placed immediately above the new stack) */ \
register void * _func asm("r22") = func; /* function pointer to the function to run on the new stack */ \
asm volatile( \
"ldi r24, pm_lo8(1f)\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"ldi r25, pm_hi8(1f)\n" \
"in r10, __SP_L__\n" /* retrieve the stack pointer */ \
"in r11, __SP_H__\n" \
"in r12, __SREG__\n" /* retrieve sreg */ \
"std z+0, r24\n" /* store jump address to the beginning of the buffer */ \
"std z+1, r25\n" \
"std z+2, r10\n" /* store stack pointer to next slot */ \
"std z+3, r11\n" \
"std z+4, r28\n" /* store frame pointer */ \
"std z+5, r29\n" \
"std z+6, r12\n" /* store sreg */ \
"movw r24, r30\n" /* copy buf address to first argument register pair */ \
"sbiw r30, 1\n" /* subtract 1 from it to get the new stack pointer because avr is postdecrement */ \
"out __SP_L__, r30\n" /* switch to the new stack pointer */ \
"out __SP_H__, r31\n" \
"movw r30, r22\n" /* put the address of the springboard function in the indirect jump address pair */ \
"ijmp\n" /* jump to the springboard */  \
"1:\n" /* marker for address of end of this block, so we can jump back to here */ \
: "+z"(_buf), "+r"(_func) : : "r0", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r24", "r25", "r26", "r27", "r28", "r29", "cc", "memory"); } while(0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("r30") = buf; /* force the argument into this register */ \
asm volatile( \
"ldd r2, z+0\n" /* load the saved instruction pointer */ \
"ldd r3, z+1\n" \
"ldd r4, z+2\n" /* load the saved stack pointer */ \
"ldd r5, z+3\n" \
"ldd r6, z+4\n" /* load the saved frame pointer */ \
"ldd r7, z+5\n" \
"ldd r8, z+6\n" /* load sreg */ \
"ldi r24, pm_lo8(1f)\n" /* compute address of end of this block of asm, which will be jumped to when returning to this context */ \
"ldi r25, pm_hi8(1f)\n" \
"std z+0, r24\n" /* store it to the beginning of the buffer */ \
"std z+1, r25\n" \
"in r10, __SP_L__\n" /* retrieve the stack pointer */ \
"in r11, __SP_H__\n" \
"std z+2, r10\n" /* and store it */ \
"std z+3, r11\n" \
"std z+4, r28\n" /* store the frame pointer */ \
"std z+5, r29\n" \
"in r12, __SREG__\n" /* retrieve sreg */ \
"std z+6, r12\n" /* store sreg */ \
"out __SP_L__, r4\n" /* switch to the loaded stack pointer */ \
"out __SP_H__, r5\n" \
"out __SREG__, r8\n" /* switch to the loaded sreg */ \
"movw r28, r6\n" /* switch to the loaded frame pointer */ \
"movw r30, r2\n" /* jump to the loaded address (the end of this block of asm, but in the buf context) */ \
"ijmp\n" \
"1:\n" /* marker for address of end of this block, so we can jump to here */ \
: "+z"(_buf) : : "r0", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29",  "cc", "memory"); } while(0)

#elif defined(__PPC64__)
#if defined(__GNUC__) && !defined(__clang__) && GCC_VERSION < 10100
#warning does not work correctly with position independent executables when built with older gcc
#endif
#define CONTEXT_BUF_SIZE_BYTES 32
#define STACK_ALIGNMENT 64

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("r3") = buf; \
register void * _func asm("r4") = func; \
asm volatile( \
"bl 0f\n" /* calculate address of end of this block, so we can jump back to it from the created child */ \
"0: mflr 8\n" \
"addi 8, 8, 1f-0b\n" \
"std 8, 0(%0)\n" /* store it in the first slot of the buffer */ \
"std 1, 8(%0)\n" /* store the parent's stack pointer in the next slot */ \
"std 31, 16(%0)\n" /* store the parent's frame pointer in the next slot */ \
"std 2, 24(%0)\n" /* store the parent's toc pointer in the last slot */ \
"mr 1, %0\n" /* use "buf" as the stack pointer for the created child */ \
"subi 1, 1, 24\n" /* subtract 24 to enforce stack alignment */ \
"mr 3, %0\n" /* put "buf" in the first function call argument */ \
"mtctr %1\n" /* put address of "func" in ctr register */ \
"bctr\n" /* call "func" (this does not return) */ \
"1:\n" \
: "+r"(_buf), "+r"(_func) : : "r0", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "32", "33", "34", "35", "36", "37", "38", "39", "40", "41", "42", "43", "44", "45", "46", "47", "48", "49", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "60", "61", "62", "63", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory", "cc", "xer", "ctr", "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "lr"); } while (0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("r3") = buf; \
asm volatile( \
"ld 9, 0(%0)\n" /* retrieve inactive thread's pc from buffer */ \
"ld 10, 8(%0)\n" /* retrieve inactive stack pointer */ \
"ld 11, 16(%0)\n" /* retrieve inactive frame pointer */ \
"ld 12, 24(%0)\n" /* retrieve inactive toc pointer */ \
"bl 0f\n" /* calculate address of end of this block, so we can jump back to it on the next context switch */ \
"0: mflr 8\n" \
"addi 8, 8, 1f-0b\n" \
"std 8, 0(%0)\n" /* store the active thread's pc in the buffer */ \
"std 1, 8(%0)\n" /* store the active thread's stack pointer */ \
"std 31, 16(%0)\n" /* store the active thread's frame pointer */ \
"std 2, 24(%0)\n" /* store the active thread's toc pointer */ \
"mr 1, 10\n" /* switch the stack pointer to the loaded value */ \
"mr 31, 11\n" /* switch the frame pointer to the loaded value */ \
"mr 2, 12\n" /* switch the toc pointer to the loaded value */ \
"mtctr 9\n" /* jump to the stored program counter */ \
"bctr\n" \
"1:\n" \
: "+r"(_buf) : : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "32", "33", "34", "35", "36", "37", "38", "39", "40", "41", "42", "43", "44", "45", "46", "47", "48", "49", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "60", "61", "62", "63", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory", "cc", "xer", "ctr", "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "lr"); } while (0)

#elif defined(__mips__)
/* note: this code works, with or without pic, because gp is explicitly saved and restored, but the assembler still emits warnings about not using .cprestore in PIC mode. also, there are more nops than should be necessary */

#define CONTEXT_BUF_SIZE_BYTES 16
#define STACK_ALIGNMENT 64

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("4") = buf; \
register void * _func asm("5") = func; \
asm volatile( \
"nop\n" /* wtf */ \
"jal 0f\n" \
"nop\n" /* branch delay slot */ \
"0: addiu $6, $31, (1f-0b)\n" /* register 6 now contains address of "1:" */ \
"sw $6, 0(%0)\n" /* store it here */ \
"sw $29, 4(%0)\n" /* store the sp */ \
"sw $30, 8(%0)\n" /* store the fp */ \
"sw $28, 12(%0)\n" /* store the pic register just in case */ \
"move $25, %1\n" /* copy callee address to this register as per the pic abi requirement */ \
"move $4, %0\n" /* copy argument to first argument register */ \
"add $29, %0, -64\n" /* decrement stack pointer because mips */ \
"nop\n" /* wtf */ \
"jr %1\n" /* jump to springboard function */ \
"nop\n" /* branch delay slot */ \
"1:\n" \
: "+r"(_buf), "+r"(_func) : : "0", "1", "2", "3", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "31", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31", "cc", "memory", "lo", "hi"); } while (0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("4") = buf; \
asm volatile( \
"lw $11, 0(%0)\n" /* load saved pc */ \
"lw $12, 4(%0)\n" /* load saved sp */ \
"lw $13, 8(%0)\n" /* load saved fp */ \
"lw $14, 12(%0)\n" /* load saved pic register */ \
"nop\n" /* wtf */ \
"jal 0f\n" \
"nop\n" /* branch delay slot */ \
"0: addiu $6, $31, (1f-0b)\n" /* register 6 now contains address of "1:" */ \
"sw $6, 0(%0)\n" /* store address of 1: in ip */ \
"sw $29, 4(%0)\n" /* store sp */ \
"sw $30, 8(%0)\n" /* store fp */ \
"sw $28, 12(%0)\n" /* store pic */ \
"move $29, $12\n" /* switch to loaded sp */ \
"move $30, $13\n" /* switch to loaded fp */ \
"move $28, $14\n" /* switch to loaded pic */ \
"nop\n" /* wtf */ \
"jr $11\n" /* jump to loaded pc */ \
"nop\n" /* branch delay slot */ \
"1:\n" \
: "+r"(_buf) : : "0", "1", "2", "3", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "31", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31", "cc", "memory", "lo", "hi"); } while (0)

#elif __SH4__
/* todo: figure out whether/how to clobber sr or save it explicitly */
#define CONTEXT_BUF_SIZE_BYTES 16
#define STACK_ALIGNMENT 8

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("4") = buf; \
register void * _func asm("5") = func; \
asm volatile( \
"bsr 0f\n" \
"nop\n" /* branch delay slot */ \
"0: sts pr, r6\n" \
"add #(1f-0b), r6\n" \
"mov.l r6, @(0, %0)\n" /* store it here */ \
"mov.l r15, @(4, %0)\n" /* sp */ \
"mov.l r14, @(8, %0)\n" /* fp */ \
"mov.l r12, @(12, %0)\n" /* pic */ \
"mov %0, r4\n" /* copy argument to first argument register */ \
"mov %0, r15\n" /* */ \
"jmp @%1\n" \
"nop\n" /* branch delay slot */ \
"1:\n" \
: "+r"(_buf), "+r"(_func) : : "0", "1", "2", "3", "6", "7", "8", "9", "10", "11", "13", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12", "fr13", "fr14", "fr15", "mach", "macl", "gbr", "pr", "cc", "memory"); } while (0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("4") = buf; \
asm volatile( \
"mov.l @(0, %0), r7\n" /* load saved pc */ \
"mov.l @(4, %0), r8\n" /* load saved sp */ \
"mov.l @(8, %0), r9\n" /* load saved fp */ \
"mov.l @(12, %0), r10\n" /* load saved pic */ \
"bsr 0f\n" \
"nop\n" /* branch delay slot */ \
"0: sts pr, r6\n" \
"add #(1f-0b), r6\n" \
"mov.l r6, @(0, %0)\n" /* store address of 1: in ip */ \
"mov.l r15, @(4, %0)\n" /* store sp */ \
"mov.l r14 @(8, %0)\n" /* store fp */ \
"mov.l r12, @(12, %0)\n" /* store pic */ \
"mov r8, r15\n" /* switch to loaded sp */ \
"mov r9, r14\n" /* switch to loaded fp */ \
"mov r10, r12\n" /* switch to loaded pic */ \
"jmp @r7\n" \
"nop\n" /* branch delay slot */ \
"1:\n" \
: "+r"(_buf) : : "0", "1", "2", "3", "5", "6", "7", "8", "9", "10", "11", "13", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12", "fr13", "fr14", "fr15", "mach", "macl", "gbr", "pr", "cc", "memory"); } while (0)

#elif __OR1K__
/* experimental. does not clobber fp regs (are there any fp regs?) */
#define CONTEXT_BUF_SIZE_BYTES 12
#define STACK_ALIGNMENT 64

#define BOOTSTRAP_CONTEXT(buf, func) do { \
register void * _buf asm("3") = buf; \
register void * _func asm("4") = func; \
asm volatile( \
"l.jal 0f\n" \
"l.nop\n" /* branch delay slot */ \
"0: l.addi r6, r9, (1f-0b)\n" /* register 6 now contains address of "1:" */ \
"l.sw 0(%0), r6\n" /* store it here */ \
"l.sw 4(%0), r1\n" /* store the sp */ \
"l.sw 8(%0), r2\n" /* store the fp */ \
"l.ori r3, %0, 0\n" /* copy argument to first argument register */ \
"l.addi r1, %0, -64\n" /* decrement stack pointer because mips */ \
"l.jr %1\n" /* jump to springboard function */ \
"l.nop\n" /* branch delay slot */ \
"1:\n" \
: "+r"(_buf), "+r"(_func) : : "0", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "cc", "memory"); } while (0)

#define SWAP_CONTEXT(buf) do { \
register void * _buf asm("3") = buf; \
asm volatile( \
"l.lwz r13, 0(%0)\n" /* load saved pc */ \
"l.lwz r14, 4(%0)\n" /* load saved sp */ \
"l.lwz r15, 8(%0)\n" /* load saved fp */ \
"l.jal 0f\n" \
"l.nop\n" /* branch delay slot */ \
"0: l.addi r6, r9, (1f-0b)\n" /* register 6 now contains address of "1:" */ \
"l.sw 0(%0), r6\n" /* store address of 1: in ip */ \
"l.sw 4(%0), r1\n" /* store sp */ \
"l.sw 8(%0), r2\n" /* store fp */ \
"l.ori r1, r14, 0\n" /* switch to loaded sp */ \
"l.ori r2, r15, 0\n" /* switch to loaded fp */ \
"l.jr r13\n" /* jump to loaded pc */ \
"l.nop\n" /* branch delay slot */ \
"1:\n" \
: "+r"(_buf) : : "0", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "cc", "memory"); } while (0)

#endif

/* end architecture specific macros */

/* sentinel value */
static void * const not_filled = &(void *){ NULL };

struct channel {
    /* holds the pc, sp, fp, and (on some architectures) a fourth value, for the inactive coroutine */
    unsigned char context_inactive[CONTEXT_BUF_SIZE_BYTES];
    
    /* the actual function to run. this pointer is nulled when the function has returned (a condition used within from()) */
    void (* func)(struct channel *, void *);
    
    /* argument to pass to the above function, also used to pass values between parent and child via yield(). this is set to the special value not_filled when it is logically empty */
    void * value;
    
    /* if a dynamic allocator was used, give from() the info it needs to clean up when the child returns */
    void * free_after;
    void (* free_func)(void *);
};

const size_t sizeof_struct_channel = sizeof(struct channel);

void coroutine_switch(struct channel * channel) {
    /* do the actual context swap */
    if (channel->func) SWAP_CONTEXT(channel);
}

__attribute((noreturn)) static void springboard(struct channel * channel) {
    void * arg = channel->value;
    channel->value = not_filled;
    
    /* run the main body of the child coroutine */
    channel->func(channel, arg);
    
    /* reached when coroutine function finishes. the parent may be watching for this condition */
    channel->func = NULL;
    
    /* leave the child context forever */
    SWAP_CONTEXT(channel);
    __builtin_unreachable();
}

struct channel * coroutine_create_given_memory(void (* func)(struct channel *, void *), void * arg, void * block, const size_t blocksize) {
    /* position the top of the new stack within the block, respecting alignment requirements */
    struct channel * channel = (void *)((char *)block + ((blocksize - sizeof(struct channel)) & ~(STACK_ALIGNMENT - 1)));
    
    /* copy values into the struct. all variables not listed here are zeroed */
    *channel = (struct channel) {
        .func = func,
        .value = arg
    };
    
    /* call the bit of asm that fills the inactive context and jumps to the springboard */
    BOOTSTRAP_CONTEXT(channel, springboard);
    
    /* control flow returns here via the first coroutine_switch in the child */
    return channel;
}

void yield_to(struct channel * channel, void * pointer) {
    channel->value = pointer;
    SWAP_CONTEXT(channel);
}

void * from(struct channel * channel) {
    /* when this is called from the parent, it will free the child if the child has exited */
    if (channel->func && not_filled == channel->value)
        SWAP_CONTEXT(channel);
    
    if (!channel->func) {
        if (channel->free_func) channel->free_func(channel->free_after);
        return NULL;
    }
    
    /* take the value */
    void * ret = channel->value;
    channel->value = not_filled;
    
    return ret;
}

void close_and_join(struct channel * channel) {
    /* for parent-to-child data flow, if child is waiting in from(), give it a NULL, which it should react to by cleaning up and returning */
    while (channel->func) yield_to(channel, NULL);
    if (channel->free_func) channel->free_func(channel->free_after);
}

/* convenience function, not used on bare metal, caveats about calling abort() within library code apply */
#if defined (__unix__) || defined (__APPLE__)
#include <stdlib.h>

struct channel * coroutine_create(void (* func)(struct channel *, void *), void * arg) {
    /* allocate space for child stack and struct channel */
    unsigned char * block = NULL;
    if (posix_memalign((void *)&block, STACK_ALIGNMENT, 524288)) abort();
    
    /* do the rest of the coroutine init that does not have to do with memory allocation */
    struct channel * channel = coroutine_create_given_memory(func, arg, block, 524288);
    channel->free_after = block;
    channel->free_func = free;
    return channel;
}
#endif
