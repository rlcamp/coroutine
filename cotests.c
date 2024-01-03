/*
 very simple examples for coroutine.c
 
 cc -Ofast -o cotests cotests.c coroutine.c
 */

/* just because we want to use asprintf in one of the examples */
#define _GNU_SOURCE

#include "coroutine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* the base case is generator functions, in which the parent starts the child, and the child repeatedly passes things to the parent. the things can be anything that fits in a void pointer. it is safe for the child to yield pointers to its own local variables - they are guaranteed to still be in scope */

void generator(struct channel * parent, void * context) {
    printf("%s: spawned from %s\n", __func__, (char *)context);
    
    for (size_t num = 0; num < 4; num++)
        yield_to(parent, &num);
    
    printf("%s: no more output is coming\n", __func__);
}

void consumer(void) {
    printf("%s: base case: generator pattern\n", __func__);
    
    struct channel * child = coroutine_create(generator, (void *)__func__);

    if (!child) abort();
    
    /* loop until accept_from returns null */
    for (size_t * nump; (nump = from(child)); )
        printf("%s: got %zu from generator\n", __func__, *nump);
    
    printf("%s: ok\n\n", __func__);
}

/* another generator function example, showing they can be nested */

void nested_generator_c(struct channel * parent, void * arg) {
    printf("%s: spawned from %s\n", __func__, (char *)arg);

    for (int num = 1; num < 5; num++) {
        printf("%s: yielding %d to parent\n", __func__, num);
        yield_to(parent, &num);
    }

    printf("%s: no more output is coming\n", __func__);
}

void nested_generator_b(struct channel * parent, void * arg) {
    printf("%s: spawned from %s\n", __func__, (char *)arg);
    
    struct channel * child = coroutine_create(nested_generator_c, (void *)__func__);
    
    int sum = 0;
    for (const int * nump; (nump = from(child)); ) {
        const int val = *nump;
        sum += val;
        printf("%s: got %d, yielding cumulative sum %d to parent\n", __func__, val, sum);
        yield_to(parent, &sum);
    }

    printf("%s: ok, no more output is coming\n", __func__);
}

void nested_generator_a(void) {
    printf("%s: example of multiple nested generator functions\n", __func__);
    struct channel * child = coroutine_create(nested_generator_b, (void *)__func__);
    
    for (const int * nump; (nump = from(child)); ) {
        const int sum = *nump;
        printf("%s: got %d\n", __func__, sum);
    }
    
    printf("%s: ok\n\n", __func__);
}

/* communication in both directions, involving pointers that are allocated in the child and freed in the parent */

void mirror(struct channel * parent, void * context) {
    printf("%s: spawned from %s\n", __func__, (char *)context);

    /* loop until parent sends NULL */
    for (const char * string; (string = from(parent)); ) {
        char * reflection;
        asprintf(&reflection, "%s with goatee", string);
        yield_to(parent, reflection);
    }
    
    printf("%s: ok\n", __func__);
}

void two_way_example(void) {
    printf("%s: communication in both directions\n", __func__);
    void * child = coroutine_create(mirror, (void *)__func__);
    
    char * crew[] = { "kirk", "spock", "mccoy" };
    
    for (size_t icrew = 0; icrew < sizeof(crew) / sizeof(crew[0]); icrew++) {
        printf("%s: sending %s to child\n", __func__, crew[icrew]);
        yield_to(child, crew[icrew]);
        
        char * reflection = from(child);
        printf("%s: got %s back from child\n", __func__, reflection);
        free(reflection);
    }
    
    printf("%s: no more input is coming\n", __func__);
    
    close_and_join(child);

    printf("\n");
}

/* communication in both directions, controlled by child */

void another_mirror(struct channel * parent, void * context) {
    printf("%s: spawned from %s\n", __func__, (char *)context);
    
    char * crew[] = { "kirk", "spock", "mccoy" };
    
    for (size_t icrew = 0; icrew < sizeof(crew) / sizeof(crew[0]); icrew++) {
        printf("%s: sending %s to parent\n", __func__, crew[icrew]);
        yield_to(parent, crew[icrew]);
        
        char * reflection = from(parent);
        printf("%s: got %s back from parent\n", __func__, reflection);
        free(reflection);
    }

    printf("%s: done, returning\n", __func__);
}

void another_two_way_example(void) {
    printf("%s: communication in both directions, controlled by child\n", __func__);
    void * child = coroutine_create(another_mirror, (void *)__func__);

    for (char * string; (string = from(child)); ) {
        char * reflection;
        asprintf(&reflection, "%s with goatee", string);
        
        yield_to(child, reflection);
    }
    printf("%s: ok\n\n", __func__);
}

/* test generator that doesn't yield anything */

void generator_trivial(struct channel * parent __attribute((unused)), void * context) {
    printf("%s: spawned from %s, just returning\n", __func__, (char *)context);
}

void consumer_trivial(void) {
    printf("%s: this should not crash\n", __func__);
    struct channel * child = coroutine_create(generator_trivial, (void *)__func__);
    
    printf("%s: got here, just created child\n", __func__);
    while (from(child));
    
    printf("%s: done\n\n", __func__);
}

/* test generator with a parent that doesn't yield anything */

void child_consumer_trivial(struct channel * parent, void * context) {
    printf("%s: spawned from %s\n", __func__, (char *)context);
    
    while (from(parent));
    
    printf("%s: ok\n", __func__);
}

void parent_to_child_trivial(void) {
    printf("%s: this should not crash\n", __func__);
    struct channel * child = coroutine_create(child_consumer_trivial, (void *)__func__);
    
    printf("%s: no more input is coming\n", __func__);

    close_and_join(child);
    
    printf("%s: done\n\n", __func__);
}

/* test generator using the non-malloc interface for stack. warning this might blow up if you are using some instrumented version of fprintf that uses more than 8K of stack space */

void test_child_on_parent_stack(void) {
    printf("%s\n", __func__);
    fflush(stdout);
    char block_unaligned[32768], * block = (char *)(((size_t)block_unaligned + 63) & ~63);
    
    struct channel * child = coroutine_create_given_memory(generator_trivial, (void *)__func__, block, sizeof(block_unaligned) - 64);
    while (from(child));
    
    printf("%s: done\n\n", __func__);
}

/* star network - communication between children via a parent broker */

void star_network_first_child(struct channel * parent, void * context __attribute((unused))) {
    yield_to(parent, "message for parent: hello");
    yield_to(parent, "message for second child: hi");

    printf("%s: done\n", __func__);
}

void star_network_second_child(struct channel * parent, void * context __attribute((unused))) {
    for (char * string; (string = from(parent)); )
        printf("%s: got message: %s\n", __func__, string);
    
    printf("%s: ok\n", __func__);
}

void star_network(void) {
    printf("%s: mediate communication between multiple children\n", __func__);
    struct channel * first_child = coroutine_create(star_network_first_child, NULL);
    struct channel * second_child = coroutine_create(star_network_second_child, NULL);
    
    for (const char * string; (string = from(first_child)); ) {
        printf("%s: from first child: %s\n", __func__, string);
        if (strstr(string, "for second child: "))
            yield_to(second_child, strchr(string, ':') + 2);
    }
    
    printf("%s: ok, telling second child no more input is coming\n", __func__);

    close_and_join(second_child);
    
    printf("%s: done\n\n", __func__);
}

/* passes a buffer to a coroutine which fills it and passes it back */

void child_that_modifies_buffer_provided_by_parent(struct channel * parent, void * context) {
    const size_t bytes_per_yield = *((size_t *)context);
    
    char letter = 'a';
    
    /* child loops over buffers to fill from parent */
    for (char * string; (string = from(parent)); ) {
        /* and fills them */
        for (size_t ibyte = 0; ibyte < bytes_per_yield; ibyte++) {
            string[ibyte] = letter;
            
            letter++;
            if (letter > 'z')
                letter = 'a';
        }

        /* and yields them back to parent */
        yield_to(parent, string);
    }
}

void parent_that_provides_buffer_for_child_to_fill(void) {
    size_t bytes_per_yield = 13;
    
    char buffer[14] = { 0 };
    
    struct channel * child = coroutine_create(child_that_modifies_buffer_provided_by_parent, &bytes_per_yield);
    
    for (size_t ipass = 0; ipass < 2; ipass++) {
        /* parent yields buffer to child...*/
        yield_to(child, buffer);
        
        /* ...which fills it and passes it back */
        from(child);

        printf("%s: %s\n", __func__, buffer);
    }
    
    close_and_join(child);
    printf("\n");
}

void child_that_modifies_contents_of_pointer(struct channel * parent, void * context) {
    (void)context;
    
    /* child fills a buffer provided by the parent */
    for (int * nump, value = 0; (nump = from(parent)); ) {
        *nump = value++;
        
        /* and yields the buffer back to parent */
        yield_to(parent, nump);
    }
}

void test_child_modifying_pointer_to_local_variable_in_parent(void) {
    struct channel * child = coroutine_create(child_that_modifies_contents_of_pointer, NULL);
    
    for (size_t ipass = 0; ipass < 4; ipass++) {
        int num;
        
        /* parent yields pointer to local variable to child...*/
        yield_to(child, &num);
        
        /* ...which fills it and passes it back */
        int * nump = from(child);

        /* this should print the same value twice, but the compiler doesn't know that */
        printf("%s: %d %d\n", __func__, num, *nump);
    }
    
    close_and_join(child);
    printf("\n");
}

void child_that_modifies_prearranged_buffer(struct channel * parent, void * buffer) {
    while (from(parent))
        for (char * cursor = buffer; *cursor; cursor++)
            *cursor += ('A' - 'a');
}

void test_prearranged_string_buffer(void) {
    char buffer[5];

    struct channel * child = coroutine_create(child_that_modifies_prearranged_buffer, buffer);

    for (size_t ipass = 0; ipass < 3; ipass++) {
        char * strings[] = { "abcd", "efgh", "ijkl" };
        strcpy(buffer, strings[ipass]);

        /* yield a non-NULL token that isn't the buffer */
        yield_to(child, "");

        /* contents of buffer has changed, do we know it? */
        printf("%s: %s\n", __func__, buffer);
    }

    close_and_join(child);
    printf("\n");
}

void child_that_modifies_prearranged_int(struct channel * parent, void * context) {
    int * nump = context;

    while (from(parent))
        *nump += 5;
}

void test_prearranged_int(void) {
    int num;

    struct channel * child = coroutine_create(child_that_modifies_prearranged_int, &num);

    for (size_t ipass = 0; ipass < 10; ipass++) {
        num = (int)ipass;

        /* yield a non-NULL token that isn't a pointer to num */
        yield_to(child, "");

        /* num has changed, do we know it? */
        printf("%s: %d\n", __func__, num);
    }

    close_and_join(child);
    printf("\n");
}

/* demo of under-the-hood functionality, where the two threads are merely handing off execution and not otherwise cooperating on logic */

void cooperative_multitasking_child(struct channel * parent, void * context) {
    (void)context;

    for (size_t iwork = 0; iwork < 6; iwork++) {
        printf("%s: %zu/6\n", __func__, iwork);
        
        coroutine_switch(parent);
    }
}

void cooperative_multitasking_parent_that_finishes_before_child(void) {
    struct channel * child = coroutine_create(cooperative_multitasking_child, NULL);
        
    for (size_t iwork = 0; iwork < 3; iwork++) {
        printf("%s: %zu/3\n", __func__, iwork);

        coroutine_switch(child);
    }
    
    close_and_join(child);
    
    printf("\n");
}

void cooperative_multitasking_parent_that_finishes_after_child(void) {
    struct channel * child = coroutine_create(cooperative_multitasking_child, NULL);
        
    for (size_t iwork = 0; iwork < 9; iwork++) {
        printf("%s: %zu/9\n", __func__, iwork);
        
        coroutine_switch(child);
    }
    
    close_and_join(child);
    
    printf("\n");
}

/* do the very simplest thing first, with no printf statements */

void generator_silent(struct channel * parent, void * context) {
    (void)context;
    
    for (size_t num = 0; num < 4; num++)
        yield_to(parent, &num);
}

void consumer_silent(void) {
    struct channel * child = coroutine_create(generator_silent, (void *)__func__);
    if (!child) abort();
    
    size_t sum = 0;
    
    /* loop until accept_from returns null */
    for (size_t * nump; (nump = from(child)); )
        sum += *nump;

    if (6 != sum) abort();
}

#include <complex.h>

/* workaround for newlib and certain uncooperative combinations of compiler and libc */
#if !defined(CMPLXF) && __has_builtin(__builtin_complex)
#define CMPLXF __builtin_complex
#elif !defined(CMPLXF)
#define CMPLXF(r, i) (float complex)((float)(r) + (float complex)I * (i))
#endif

static void fft8_with_intermission(struct channel * bathroom, float complex y[8], const float complex x[8]) {
    /* perform four dfts of size 2, two of which are multiplied by a twiddle factor (a -90 degree phase shift) */
    const float complex a0 = x[0] + x[4];
    const float complex a1 = x[0] - x[4];
    const float complex a2 = x[2] + x[6];
    const float complex a3 = CMPLXF( __imag__ x[2] - __imag__ x[6], __real__ x[6] - __real__ x[2] );
    const float complex a4 = x[1] + x[5];
    const float complex a5 = x[1] - x[5];
    const float complex a6 = x[3] + x[7];
    const float complex a7 = CMPLXF( __imag__ x[3] - __imag__ x[7], __real__ x[7] - __real__ x[3] );
    
    /* perform two more dfts of size 2 */
    const float complex c0 = a0 + a2;
    const float complex c1 = a1 + a3;
    const float complex c2 = a0 - a2;
    const float complex c3 = a1 - a3;
    const float complex c4 = a4 + a6;
    const float complex b5 = a5 + a7;
    const float complex b6 = a4 - a6;
    const float complex b7 = a5 - a7;

    /* intermission */
    coroutine_switch(bathroom);

    /* apply final twiddle factors */
    const float complex c5 = CMPLXF((__imag__ b5 + __real__ b5) * (float)M_SQRT1_2,  (__imag__ b5 - __real__ b5) * (float)M_SQRT1_2);
    const float complex c6 = CMPLXF( __imag__ b6, -__real__ b6 );
    const float complex c7 = CMPLXF((__imag__ b7 - __real__ b7) * (float)M_SQRT1_2, -(__real__ b7 + __imag__ b7) * (float)M_SQRT1_2);

    /* intermission */
    coroutine_switch(bathroom);

    /* perform four dfts of length two */
    y[0] = c0 + c4;
    y[1] = c1 + c5;
    y[2] = c2 + c6;
    y[3] = c3 + c7;
    y[4] = c0 - c4;
    y[5] = c1 - c5;
    y[6] = c2 - c6;
    y[7] = c3 - c7;
}

void child_fft(struct channel * parent, void * arg) {
    (void)arg;

    float complex y[8], x[8] = { 1, I, -1, -I, 1, I, -1, -I };
    fft8_with_intermission(parent, y, x);

    for (size_t ix = 0; ix < 8; ix++)
        printf("%s: y[%zu] = %g %+gi\n", __func__, ix, __real__ y[ix], __imag__ y[ix]);
}

void parent_fft(void) {
    printf("%s: two concurrent tasks which use as many fp regs as possible\n", __func__);
    
    struct channel * child = coroutine_create(child_fft, NULL);

    float complex y[8], x[8] = { 0.25f, 0.25f, 1.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };
    fft8_with_intermission(child, y, x);

    close_and_join(child);

    for (size_t ix = 0; ix < 8; ix++)
        printf("%s: y[%zu] = %g %+gi\n", __func__, ix, __real__ y[ix], __imag__ y[ix]);

    printf("\n");
}

int main(void) {
    /* disable stdout buffering so that stdout and stderr are well-ordered relative to each other and we can figure out where any segfaults happen, even on godbolt */
    setvbuf(stdout, NULL, _IONBF, 0);
    
    consumer_silent();
    consumer();
    nested_generator_a();
    two_way_example();
    another_two_way_example();
    consumer_trivial();
    parent_to_child_trivial();
    test_child_on_parent_stack();
    star_network();
    parent_that_provides_buffer_for_child_to_fill();
    test_child_modifying_pointer_to_local_variable_in_parent();
    test_prearranged_string_buffer();
    test_prearranged_int();
    cooperative_multitasking_parent_that_finishes_before_child();
    cooperative_multitasking_parent_that_finishes_after_child();
    parent_fft();
    return 0;
}
