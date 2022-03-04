/* note this test is a bit of a strawman given that pthreads+pipes is not the fastest possible pthread-based version of a generator function framework */

#include "coroutine.h"
#include "timing.h"

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#define YIELD_COUNT 8388608

static void child_that_yields_a_lot(struct channel * parent, void * context __attribute((unused))) {
    for (size_t ipass = 0; ipass < YIELD_COUNT; ipass++) {
//        do_some_expensive_work();
        yield_to(parent, &ipass);
    }
}

int main(void) {
    const unsigned long long time_start = current_monotonic_time_in_nanoseconds();
    
    struct channel * child = coroutine_create(child_that_yields_a_lot, (void *)__func__);
    while (from(child));// do_some_expensive_work();
    
    const unsigned long long time_elapsed = current_monotonic_time_in_nanoseconds() - time_start;
    
    fprintf(stderr, "%s: %.1f ns per round-trip between coroutines (%.1f ns per switch)\n", __func__, time_elapsed / (double)YIELD_COUNT, time_elapsed / (2.0 * YIELD_COUNT));

    return 0;
}
