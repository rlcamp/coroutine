#include "coroutine.h"

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#ifdef __APPLE__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

unsigned long long current_monotonic_time_in_nanoseconds(void) {
#ifdef __APPLE__
    clock_serv_t clock_serv;
    mach_timespec_t timespec;
    host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &clock_serv);
    clock_get_time(clock_serv, &timespec);
    mach_port_deallocate(mach_task_self(), clock_serv);
#else
    struct timespec timespec;
    clock_gettime(CLOCK_MONOTONIC, &timespec);
#endif
    return timespec.tv_sec * 1000000000ULL + timespec.tv_nsec;
}


#define YIELD_COUNT 268435456

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
    
    fprintf(stderr, "%s: %.3f ns per round-trip between coroutines (%.3f ns per switch)\n", __func__, time_elapsed / (double)YIELD_COUNT, time_elapsed / (2.0 * YIELD_COUNT));

    return 0;
}
