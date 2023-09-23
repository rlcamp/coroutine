### What is this for?

Coroutines are a kind of thread, typically cooperatively rather than preemptively multitasked. The intended use case is when concurrency, but not parallelism, is required, with a possibly very high rate of switching between coroutines, in a possibly hard-realtime context (such as an audio subsystem).

### Performance

Switching time repends on register usage at the call site, and can be as low as 3 nanoseconds on commodity hardware.

### Supported platforms

The task switching relies only on some inline assembly and requires no assistance whatsoever from an OS kernel or supervisor or anything of the sort. You can use these within a pthread on a POSIX OS, within a preemptively-multitasked RTOS thread, whatever you want. The code has been tested on Cortex-M4 bare metal, Cortex-A7 Linux, aarch64 iOS, aarch64 Linux, x86_64 macOS, x86_64 linux actual hardware, others (riscv64, i386, m68k) via qemu.

### Why is there no scheduler?

Because it's not an RTOS. You could build a cooperatively-multitasked RTOS using these, though. Or you could use these within a single task of a preemptively-multitasked RTOS. It's up to you.

### Why not just use protothreads / C++20 coroutines?

Those don't have their own call stacks - the resulting code can look superficially similar to proper stackful coroutines, but under the hood they work very differently. Proper stackful coroutines have far fewer restrictions.

### Why can't a coroutine yield to something other than its own immediate parent or children?

Because that would break structured concurrency. In theory you could build such a thing, by adding a scheduler that is the immediate parent of all other coroutines, but the API is not designed to help you do that.

### What's up with all this assembly? Why not just use setjmp/longjmp?

A longjmp to anywhere but upward within the same call stack is undefined behaviour. Some libc's enforce this at runtime.

### What about set/get/swapcontext?

A proof-of-concept alternative implementation of the API is given that uses these. In most use cases it's almost as fast, but it relies on either libc or libucontext to provide the underlying functionality, and at the time of this writing, availability is not sufficiently universal. The clobber-based method used in the main implementation has a speed advantage due to only saving and restoring registers that are known by the compiler to be in use at each call site, rather than saving or pushing to stack everything that the function call ABI requires.

### Why not just use pthreads?

It IS possible to implement this API with pthreads, at the cost of losing the hard-realtime capability. A proof-of-concept alternative implementation of the API is given that uses pthreads. The average round-trip task switching time goes up by a factor of several hundred, but the worst-case round trip time goes up by far more, such that it would generally not be a good idea to use a pthread-based implementation of this API within, say, an audio callback.

Applications which want generator functions within a preemptively multitasked, truly parallel environment, have better options than to use the pthreads implementation of this API, as this imposes additional synchronization slowdown in order to guarantee that only a parent or child are running at any given time, as would be the case within strictly cooperative multitasking. This is required to, for example, allow a child to yield to a parent a pointer to something on the child's stack, knowing that its lifetime will outlast the parent's interest in it, and that the child will not be modifying it further while any logic within the parent is running. Application code which is expecting that the child will continue to run while the parent consumes the child's previous output can benefit from generator function implementations which do not impose this extra synchronization.
