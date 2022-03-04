#include "coroutine.h"

#include <stdio.h>
#include <ctype.h>

static char * morsetable[256] = {
    [' '] = "      ",
    ['A'] = " - ---  ",
    ['B'] = " --- - - -  ",
    ['C'] = " --- - --- -  ",
    ['D'] = " --- - -  ",
    ['E'] = " -  ",
    ['F'] = " - - --- -  ",
    ['G'] = " --- --- -  ",
    ['H'] = " - - - -  ",
    ['I'] = " - -  ",
    ['J'] = " --- --- --- -  ",
    ['K'] = " --- - ---  ",
    ['L'] = " - --- - -  ",
    ['M'] = " --- ---  ",
    ['N'] = " --- -  ",
    ['O'] = " --- --- ---  ",
    ['P'] = " - --- --- -  ",
    ['Q'] = " --- --- - ---  ",
    ['R'] = " - --- -  ",
    ['S'] = " - - -  ",
    ['T'] = " ---  ",
    ['U'] = " - - ---  ",
    ['V'] = " - - - ---  ",
    ['W'] = " - --- ---  ",
    ['X'] = " --- - - ---  ",
    ['Y'] = " --- - --- ---  ",
    ['Z'] = " --- --- - -  ",
    ['1'] = " - --- --- --- ---  ",
    ['2'] = " - - --- --- ---  ",
    ['3'] = " - - - --- ---  ",
    ['4'] = " - - - - ---  ",
    ['5'] = " - - - - -  ",
    ['6'] = " --- - - - -  ",
    ['7'] = " --- --- - - -  ",
    ['8'] = " --- --- --- - -  ",
    ['9'] = " --- --- --- --- -  ",
    ['0'] = " --- --- --- --- ---  ",
    ['+'] = " - --- - --- -  ",
    ['-'] = " --- - - - - ---  ",
    ['?'] = " - - --- --- - -  ",
    ['/'] = " --- - - --- -  ",
    ['.'] = " - --- - --- - ---  ",
    [','] = " --- --- - - --- ---  ",
    ['\''] = " --- - - --- -  ",
    [')'] = " --- - --- --- - ---  ",
    ['('] = " --- - --- --- -  ",
    [':'] = " --- --- --- - - -  ",
};

void morse_generator(struct channel * parent, void * sentence) {
    /* this is a simple demonstration of the benefit of a generator function, for producing samples according to logic that requires internal state. if this were written as a callback, the loop structure would have to be "inside out", with additional opportunities for error, and with the loop control state stored in either static/global memory or in a separate data structure */
    
    /* loop over letters within the sentence */
    for (const char * letter = sentence; *letter != '\0'; letter++) {
        /* get the pixel array for the uppercase version of the given character, or that of whitespace if not defined */
        char * pixels = morsetable[toupper(*letter)] ?: morsetable[' '];
        
        /* loop over pixels within the current letter */
        for (char * pixel = pixels; *pixel != '\0'; pixel++)
            yield_to(parent, pixel);
    }
    /* generators implicitly yield null when they return, as seen by a parent blocked in from() */
}

int main(const int argc, char ** const argv) {
    /* sentence to transmit will be "test" unless another was provided */
    char * sentence = argc > 1 ? argv[1] : "test";
    
    /* start a generator function with the given sentence as the argument */
    struct channel * child = coroutine_create(morse_generator, sentence);
    
    /* loop over pixels produced by the generator, until from() returns NULL */
    for (const char * pixel; (pixel = from(child)); )
        /* write each pixel to stdout */
        putchar(*pixel);
    
    /* when we exit the above loop, the coroutine has returned, and all resources associated with it have been freed */
    
    /* write a newline */
    putchar('\n');
    
    return 0;
}
