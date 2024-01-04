/* campbell, boyle 2021
 Implements the same coroutine.h api as the canonical coroutine.c, but uses pthreads. Context
 switching is about a thousand times slower and hard-realtime guarantees cannot be met, but
 this implementation should work anywhere posix is served

 ISC license
 */

#include "coroutine.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <pthread.h>
#include <unistd.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#define sem_t dispatch_semaphore_t
#define sem_init(x, p, v) ({ *(x) = dispatch_semaphore_create(v); })
#define sem_post(x) dispatch_semaphore_signal(*(x))
#define sem_wait(x) dispatch_semaphore_wait(*(x), DISPATCH_TIME_FOREVER)
#define sem_destroy(x) dispatch_release(*(x))
#else
#include <semaphore.h>
#endif

/* sentinel value */
static void * const not_filled = &(void *){ NULL };

struct channel {
    sem_t sem[2];

    pthread_t thread;
    int in_child;
    
    /* the actual function to run. this pointer is nulled when the function has returned
     (a condition used within from()) */
    void (* func)(struct channel *, void *);
    
    /* argument to pass to the above function, also used to pass values between parent and
     child via yield(). this is set to the special value not_filled when it is logically empty */
    void * value;
};

const size_t sizeof_struct_channel = sizeof(struct channel);

static void context_switch(struct channel * channel) {
    const int in_child_at_entry = channel->in_child;
    
    channel->in_child = !channel->in_child;

    sem_post(channel->sem + !in_child_at_entry);
    sem_wait(channel->sem + in_child_at_entry);
}

void coroutine_switch(struct channel * channel) {
    if (!channel->func) return;
    context_switch(channel);
}

static void * springboard(void * channelv) {
    struct channel * channel = channelv;
    void * arg = channel->value;
    channel->value = not_filled;

    /* run the main body of the child coroutine */
    channel->func(channel, arg);

    /* reached when coroutine function finishes. the parent may be watching for this condition */
    channel->func = NULL;

    sem_post(channel->sem + 0);

    return NULL;
}

struct channel * coroutine_create(void (* func)(struct channel *, void *), void * arg) {
    struct channel * channel = malloc(sizeof(struct channel));
    *channel = (struct channel) {
        .func = func,
        .value = arg,
        .in_child = 1,
    };

    sem_init(channel->sem + 0, 0, 0);
    sem_init(channel->sem + 1, 0, 0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 524288);
    pthread_create(&channel->thread, &attr, springboard, channel);
    pthread_attr_destroy(&attr);

    sem_wait(channel->sem + 0);

    return channel;
}

struct channel * coroutine_create_given_memory(void (* func)(struct channel *, void *), void * arg, void * block, const size_t blocksize) {
    (void)block;
    (void)blocksize;
    return coroutine_create(func, arg);
}

void yield_to(struct channel * channel, void * pointer) {
    channel->value = pointer;
    context_switch(channel);
}

static void channel_join_and_cleanup(struct channel * channel) {
    pthread_join(channel->thread, NULL);
    sem_destroy(channel->sem + 0);
    sem_destroy(channel->sem + 1);
    free(channel);
}

void * from(struct channel * channel) {
    /* when this is called from the parent, it will free the child if the child has exited */
    
    if (channel->func && not_filled == channel->value)
        context_switch(channel);

    if (!channel->func) {
        channel_join_and_cleanup(channel);
        return NULL;
    }
    
    /* take the value */
    void * ret = channel->value;
    channel->value = not_filled;
    
    return ret;
}

void close_and_join(struct channel * channel) {
    /* for parent-to-child data flow, if child is waiting in from(), give it a NULL, which it
     should react to by cleaning up and returning */
    while (channel->func) yield_to(channel, NULL);
    channel_join_and_cleanup(channel);
}
