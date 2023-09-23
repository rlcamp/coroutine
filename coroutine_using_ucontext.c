/*
 Copyright 2015-2022 Richard Campbell
 
 Permission to use, copy, modify, and/or distribute this software for any purpose with or
 without fee is hereby granted, provided that the above copyright notice and this permission
 notice appear in all copies.

 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
 DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
 OR PERFORMANCE OF THIS SOFTWARE.

 Coroutines for generator functions, sequential pipelines, state machines, and other uses in C
 
 Alternative implementation using ucontext
 */
#define _XOPEN_SOURCE 600
#include "coroutine.h"

#include <stddef.h>
#include <stdint.h>

#if defined (__unix__) || defined (__APPLE__)
#include <stdlib.h>
#endif

#ifndef USE_SYSTEM_UCONTEXT
#include <libucontext/libucontext.h>
#define ucontext_t libucontext_ucontext_t
#define getcontext libucontext_getcontext
#define swapcontext libucontext_swapcontext
#define makecontext libucontext_makecontext
#define setcontext libucontext_setcontext
#else
#warning attempting to use system ucontext
#include <ucontext.h>
#endif

/* sentinel value */
static void * const not_filled = &(void *){ NULL };

struct channel {
    /* points to whichever ucontext_t should be swapcontext'd to on the next parent-to-child or child-to-parent context switch */
    ucontext_t * inactive;
    
    /* the actual function to run. this pointer is nulled when the function has returned (a condition used within from()) */
    void (* func)(struct channel *, void *);
    
    /* argument to pass to the above function, also used to pass values between parent and child via yield(). this is set to the special value not_filled when it is logically empty */
    void * value;
    
    /* if a dynamic allocator was used, give from() the info it needs to clean up when the child returns */
    void * free_after;
    void (* free_func)(void *);
};

const size_t sizeof_struct_channel = sizeof(struct channel);

__attribute((noreturn))
static void springboard(struct channel * channel) {
    void * arg = channel->value;
    channel->value = not_filled;
    
    /* run the main body of the child coroutine */
    channel->func(channel, arg);
    
    /* reached when coroutine function finishes. the parent may be watching for this condition */
    channel->func = NULL;
    
    /* explicitly jump out of this forever to wherever the parent is waiting, which was not yet know when uc_link would need to be set */
    setcontext(channel->inactive);
    __builtin_unreachable();
}

static void swap_context_symmetric(struct channel * channel) {
    ucontext_t buf, * tmp = channel->inactive;
    channel->inactive = &buf;
    swapcontext(&buf, tmp);
}

static void bootstrap_context(struct channel * channel, void * stack_bottom, const size_t stack_size) {
    ucontext_t dest;
    getcontext(&dest);
    dest.uc_stack.ss_sp = stack_bottom;
    dest.uc_stack.ss_size = stack_size;
    dest.uc_stack.ss_flags = 0;
    makecontext(&dest, (void (*)())springboard, 1, channel);
    
    channel->inactive = &dest;
    swap_context_symmetric(channel);
}

void coroutine_switch(struct channel * channel) {
    /* do the actual context swap */
    if (channel->func) swap_context_symmetric(channel);
}

#define STACK_ALIGNMENT 64

struct channel * coroutine_create_given_memory(void (* func)(struct channel *, void *), void * arg, void * block, const size_t blocksize) {
    /* position the top of the new stack within the block, respecting alignment requirements */
    struct channel * channel = (void *)((char *)block + ((blocksize - sizeof(struct channel)) & ~(STACK_ALIGNMENT - 1)));
    
    /* copy values into the struct. all variables not listed here are zeroed */
    *channel = (struct channel) {
        .func = func,
        .value = arg,
    };
    
    bootstrap_context(channel, block, (char *)channel - (char *)block);
    
    /* control flow returns here via the first coroutine_switch in the child */
    return channel;
}

void yield_to(struct channel * channel, void * pointer) {
    channel->value = pointer;
    swap_context_symmetric(channel);
}

void * from(struct channel * channel) {
    /* when this is called from the parent, it will free the child if the child has exited */
    if (channel->func && not_filled == channel->value)
        swap_context_symmetric(channel);
    
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
