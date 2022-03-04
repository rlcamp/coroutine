/* see accompanying coroutine.c for full details */

/* start the given function, with the given argument, and return a channel between it and the calling code */
struct channel * coroutine_create(void (* func)(struct channel * parent, void *), void * arg);

/* a generator-type coroutine calls this to pass things back to its parent */
void yield_to(struct channel * channel, void * pointer);

/* for generators, the parent calls this in a loop. when generator has returned, this returns NULL.
 example: for (struct thing * thing; (thing = from(parent)); ) { ... do something with thing ... }
 */
void * from(struct channel * channel);

/* if the parent is passing things to the child, it calls this to signal to the child that no more is coming */
void close_and_join(struct channel * channel);

/* internal functionality exposed so that more advanced things can be done */
void coroutine_switch(struct channel * channel);

/* needed for size_t */
#include <stddef.h>

struct channel * coroutine_create_given_memory(void (* func)(struct channel *, void *), void * arg, void * block, const size_t blocksize);

extern const size_t sizeof_struct_channel;
