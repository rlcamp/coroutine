/* demo of coroutines as a hard-realtime audio tone generator */

#include "coroutine.h"

#include <math.h>
#include <complex.h>

/* requires SDL, but not until main(). should work equally well with any other
 callback-based audio subsystem */
#include <SDL.h>

static float cmagsquaredf(const float complex x) {
    /* get you a compiler that optimizes these function calls away */
    return crealf(x) * crealf(x) + cimagf(x) * cimagf(x);
}

struct audio_generator_context {
    float sample_rate;
    float * cursor, * end;
};

static void yield_sample(struct channel * parent, struct audio_generator_context * context, const float sample) {
    /* store the given example at the current cursor position, and advance the cursor */
    *(context->cursor++) = sample;
    
    /* if we've reached the end of the buffer the parent wanted us to fill, yield it */
    if (context->cursor == context->end)
        coroutine_switch(parent);
}

static void tone(struct channel * parent, struct audio_generator_context * context, const float tone_frequency, const float duration) {
    /* initial value of complex sinusoid */
    float complex carrier = 1.0f;
    
    /* rotation of sinusoid after every sample */
    const float complex advance = cexpf(I * 2.0f * M_PI * tone_frequency / context->sample_rate);
    
    /* number of samples to yield */
    const size_t S = duration * context->sample_rate;
    
    for (size_t is = 0; is < S; is++) {
        /* either component of the carrier forms a sine wave vs time */
        yield_sample(parent, context, cimagf(carrier));
        
        /* rotate the carrier */
        carrier *= advance;
        
        /* renormalize the carrier, exploiting that 1/|x| ≈ (3 - |x|^2) / 2, for |x| near 1 */
        carrier *= (3.0f - cmagsquaredf(carrier)) * 0.5f;
    }
}

static void silence(struct channel * parent, struct audio_generator_context * context, const float duration) {
    const size_t S = duration * context->sample_rate;
    
    for (size_t is = 0; is < S; is++)
        yield_sample(parent, context, 0);
}

static void tone_generator(struct channel * parent, void * context) {
    /* main loop of the child coroutine. note that the function is not run from start to
     finish on each callback invocation, and unlike the callback, this can have
     arbitrary, right-side-out loop structure, local variables that persist, &c. */
    while (1) {
        /* play a 2525 Hz tone for a quarter second */
        tone(parent, context, 2525.0f, 0.249901f);
        
        /* then wait a bit */
        silence(parent, context, 0.5f);
        
        /* now play the 2475 Hz tone for a quarter second */
        tone(parent, context, 2475.0f, 0.250101f);
        
        /* and wait a bit longer */
        silence(parent, context, 2.0f);
    }
}

static void regular_ass_sdl_audio_callback(void * userdata, uint8_t * stream, int len) {
    /* this callback gets run from start to finish by the audio subsystem whenever it
     needs new samples to play. expressing arbitrary logic directly in this function
     would require it to have essentially inside-out loop structure, since the outermost
     loop must be a loop over individual samples. this can be done, but is unintuitive
     and error-prone compared to expressing the logic right-side-out as if one were
     simply yielding samples. additionally, any internal state which must persist across
     callback invocations must be static, or stored elsewhere.
     
     the solution is to decouple the callback into the parent callback function, run
     once per buffer, and a child coroutine, which simply yields samples according to
     whatever logic and loop structures it wants.
     
     it would be possible to do this with a regular thread, except that context switches
     between soft-realtime OS threads would break the hard-realtime requirement for code
     running within the audio subsystem. */

    struct audio_generator_context * context = userdata;
    context->cursor = (void *)stream; /* must cast because sdl api is a bit dumb */
    context->end = context->cursor + len / sizeof(float);
    
    static struct channel * child;
    if (!child) {
        /* allocate stack space for child in bss, mostly just so we're not
         calling malloc() in an audio callback, which would be daft */
        static unsigned char stack_space_for_child[32768] __attribute((aligned(64)));
        child = coroutine_create_given_memory(tone_generator, context, stack_space_for_child, sizeof(stack_space_for_child));
    }
    
    /* guard the context switch, because we should not care whether or not the coroutine
     implementation initially runs the child up to its first context switch upon creation */
    if (context->cursor != context->end)
        coroutine_switch(child);
    
    /* when we get here, the child has switched back, after filling the buffer. unlike
     with actual threads, we have a guarantee that this has happened in bounded time */
}

int main(void) {
    SDL_Init(SDL_INIT_AUDIO);
    
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &(SDL_AudioSpec) {
        .freq = 11025,
        .format = AUDIO_F32,
        .channels = 1,
        .samples = 1024,
        .callback = regular_ass_sdl_audio_callback,
        .userdata = &(struct audio_generator_context) {
            .sample_rate = 11025
        }
    }, NULL, 0);
    
    if (!dev) {
        fprintf(stderr, "error: %s: %s\n", __func__, SDL_GetError());
        exit(EXIT_FAILURE);
    }
    
    /* unpause audio and sleep forever */
    SDL_PauseAudioDevice(dev, 0);
    while (1) SDL_Delay(~0U);
}
