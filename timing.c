#include "timing.h"

#include <stdint.h>
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
