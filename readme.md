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
