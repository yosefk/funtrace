# funtrace - a C++ function call tracer for x86/Linux

A function call tracer is a kind of profiler showing **a timeline of function call and return events**. Here's an example trace captured by funtrace from [Krita](https://krita.org):

![image](images/krita-trace.png)

Here we can see 2 threads - whether they're running or waiting, and the changes to their callstack over time - and the source code of a selected function.

Unlike a sampling profiler such as perf, **a tracing profiler must be told what to trace** using some runtime API, and also has a **higher overhead**
than the fairly low-frequency sampling of the current callstack a-la perf. What do you get in return for the hassle and the overhead (and the hassle of culling
the overhead, by disabling tracing of short functions called very often)? Unlike flamegraphs showing where the program spends its time on average,
traces let you **debug cases of unusually high latency**, including in production (and it's a great idea to collect traces in production, and not just during development!)

For a long read about why tracing profilers are useful and how funtrace works, see [Profiling in production with function call traces](https://yosefk.com/blog/profiling-in-production-with-function-call-traces.html).
What follows is a shorter funtrace user guide.

# Why funtrace?

* **Low overhead tracing** (FWIW, in my microbenchmark I get <10 ns per instrumented call or return -
  6 times faster than an LLVM XRay microbenchmark with "flight recorder logging" and 15-18 times faster than "basic logging")
* Supports **threads, shared libraries and exceptions**
* Supports ftrace events, showing **thread scheduling states** alongside function calls & returns, so you see when time is spent waiting as opposed to computing
* Works with **stock gcc or clang** - no custom compilers or compiler passes
* Easy to integrate into a build system, and even easier to **try *without* touching the build system** using tiny compiler-wrapping scripts “passing all the right flags”
* Small (just ~1K LOC for the runtime) and thus:
  * **easy to port**
  * **easy to extend** (say, to support some variant of “green threads”/fibers)
  * **easy to audit** in case you’re reluctant to add something intrusive like this into your system without understanding it well (as I personally would be!)
* **Relatively comprehensive** – it comes with its own **tool for finding and cutting instrumentation overhead** in test runs too large to fully trace;
  support for remapping file paths to locate debug information and source code; a way to **extract trace data from core dumps**, etc.

# Trying funtrace

You can clone the repo, build the trace decoder, compile & run a simple example program, and decode its output traces as follows:

```
git clone https://github.com/yosefk/funtrace
cd funtrace
./simple-example/build.sh
./simple-example/run.sh
```

This is actually 4 different instrumented builds - 2 with gcc and 2 with clang; funtrace supports 2 different instrumentation methods for each compiler,
and we'll discuss below how to choose the best method for you. You can view the traces produced from the simple example above as follows:

```
pip install viztracer
rehash
vizviewer out/funtrace-fi-gcc.json
vizviewer out/funtrace-pg.json
vizviewer out/funtrace-fi-clang.json
vizviewer out/funtrace-xray.json
```

Funtrace uses [viztracer](https://github.com/gaogaotiantian/viztracer) for visualizing traces, in particular because of its ability to show source code, unlike stock [Perfetto](https://perfetto.dev/) (the basis for vizviewer.)

To build your own program with tracing enabled, you can use `compiler-wrappers/funtrace-pg-g++`, `compiler-wrappers/funtrace-finstr-clang++` or the other two compiler wrapper scripts, just like `simple-example/build.sh` does.
If the program uses autoconf/configure, you can set the `$CXX` env var to point to one of these scripts, and if it uses cmake, you can pass `-DCMAKE_CXX_COMPILER=/your/chosen/wrapper` to cmake.

Note that the compiler wrappers
slow down the configuration stage, because they compile & link funtrace.cpp, and this is costly at build system config time if the build system compiles many small programs to test for compiler features, library availability and such.
For the build itself, the overhead of compiling funtrace.cpp is lower, but might still be annoying if you use a fast linker like mold and are used to near-instantaneous linking. The good thing about the compiler wrappers is that
they make trying funtrace easy; if you decide to use funtrace in your program, however, you will probably want to pass the required compiler flags yourself as described below, which will eliminate the build-time overhead of the
compiler wrappers.

Once the program compiles, you can run it as usual, and then `killall -SIGTRAP your-program` (or `kill -SIGTRAP <pid>`) when you want to get a trace. The trace will go to `funtrace.raw`; if you use SIGTRAP multiple times, many
trace samples will be written to the file. Now you can run `funtrace2viz` the way `simple-example/run.sh` does; you should have it compiled if you ran `simple-example/build.sh` or just `cargo build` (the trace decoder is a Rust
program.) funtrace2viz will produce a vizviewer JSON file from each trace sample in funtrace.raw, and you can open each JSON file in vizviewer.

If you build the program, run it, and decode its trace on the same machine/in the same container, life is easy. If not, note that in order for funtrace2viz to work, you need the program and its shared libraries to be accessible at the paths where they were loaded from _in the traced program run_, on the machine _where funtrace2viz runs_. And to see the source code of the functions (as opposed to just function names), you need the source files to be accessible on that
machine, at the paths _where they were when the program was built_. If this is not the case, you can remap the paths using a file called `substitute-path.json` in the current directory of funtrace2viz, as described below.
As a side note, if you don't like having to remap source file paths - not just in funtrace but eg in gdb - see [refix](https://github.com/yosefk/refix) which can help to mostly avoid this.

Note that if you choose to try XRay instrumentation (`compiler-wrappers/funtrace-xray-clang++`), you need to run with `env XRAY_OPTIONS="patch_premain=true"` like simple-examples/run.sh does. With the other instrumentation options,
tracing is on by default.

**TODO** binary releases of the decoding tools

The above is how you can give funtrace a quick try. The rest tells how to integrate it in your program "for real."

# Choosing compiler instrumentation

Funtrace relies on the compiler inserting hooks upon function calls and returns. Funtrace supports 4 instrumentation methods (2 for gcc and 2 for clang), and comes with a compiler wrapper script passing the right flags to use each:

* **funtrace-finstr-g++** - gcc with `-finstrument-functions`
* **funtrace-pg-g++** - gcc with `-pg -mfentry -minstrument-return=call`
* **funtrace-finstr-clang++** - clang with `-finstrument-functions`
* **funtrace-xray-clang++** - clang with `-fxray-instrument`

**"By default," the method used by funtrace-pg-g++ and funtrace-finstr-clang++ is recommended for gcc and clang, respectively**. However, for each compiler, there are reasons to use the other method. Here's a table of the methods and their pros and cons, followed by a detailed explanation:

Method | gcc -finstr | gcc -pg | clang -finstr | clang XRay
--- | --- | --- | --- | --- 
before or after inlining? | ❌ before | ✅ after | ✅✅ before or after! | ✅ after
control tracing by source path | ✅ yes | ❌ no | ❌ no | ❌ no
control tracing by function length | ✅ asm | ✅ asm | ✅ asm | ✅✅ compiler
control tracing by function name list | ✅ asm | ✅ asm | ✅ asm | ❌ no
tail call artifacts | ✅ no | ❌ yes | ✅ no | ❌ yes
untraced exception catcher artifacts | ✅ no | ❌ yes | ❌ yes | ❌ yes
needs questionable linker flags | ✅ no | ❌ yes | ✅ no | ❌ yes

We'll now explain these items in detail, and add a few points about XRay which "don't fit into the table."

* **Instrument before or after inlining?** You usually prefer "after" - "before" is likely to hurt performance too much (and you can use the NOFUNTRACE macro to suppress the tracing of a function, but you'll need to do this in too many places.) Still, instrumenting before inlining has its uses, eg you can trace the program flow and follow it in vizviewer - for an interactive and/or multithreaded program, this might be easier than using a debugger or an IDE. clang -finstrument-functions is the nicest here - it instruments before inlining, but has a sister flag -finstrument-functions-after-inlining that does what you expect.
* **Control tracing by source path** - gcc's `-finstrument-functions-exclude-file-list=.h,.hpp,/usr/include` (for example) will disable tracing in functions with filenames having the substrings on the comma-separated list. This can somewhat compensate for -finstrument-functions instrumenting before inlining, and you might otherwise use this feature for "targeted tracing." 
* **Control tracing by function length** - XRay has `-fxray-instruction-threshold=N` which excludes short functions from tracing, unless they have loops that XRay assumes will run for a long time. For other instrumentation methods, funtrace comes with its own flag, `-funtrace-instr-thresh=N`, which is implemented by post-processing the assembly code produced by the compiler (funtrace supplies a script, `funtrace++`, which calls the compiler with `-S` instead of `-c` and then post-processes the assembly output and assembles it to produce the final `.o` object file.) XRay's method has 2 advantages, however. Firstly, it removes 100% of the overhead, while funtrace's method removes most (the on-entry/return hooks aren't called), but not all overhead (some extra instructions will appear relatively to the case where the function wasn't instrumented by the compiler in the first place.) Secondly, while the rest of funtrace is very solid, this bit is "hacky"/somewhat heuristical text processing of your compiler-generated assembly, and while it "seems to work" on large programs, you might have reservations against using this in production.
* **Control tracing by function name list** - for all methods other than XRay instrumentation, funtrace provides the flags `-funtrace-do-trace=file` and `-funtrace-no-trace=file` which let you specify which functions to exclude - or not to exclude - from tracing during assembly postprocessing (if you decide to use this postprocessing, of course.) This is nice for functions coming from .h files you cannot edit (and thus can't add the `NOFUNTRACE` attribute to the functions you want to exclude); it can also be nice to take a bunch of "frequently callees" reported by the funcount tool (described below) and suppress them using a list of mangled function names, instead of going to the source location of each and adding `NOFUNTRACE` there, especially during experimentation where you trying to check what suppressing this or that does for the overhead. This doesn't work for XRay ATM (assembly postprocessing could probably be implemnted for XRay but would require editing compiler-generated metdata used by the XRay runtime.)
* **Tail call artifacts** is when f calls g, the last thing g does is calling h, and instead of seeing f calling g _which calls h_, you see f calling g _and then h_. This happens because the compiler calls the "on return" hook from g before g's tail call to h. An annoyance if not a huge deal.
* **Untraced exception catcher artifacts** is when you have a function with a `try/catch` block _and_ tracing is disabled for it. In such a case, when an exception is thrown & caught, it looks like _all_ the functions returned and you start from a freshly empty call stack - instead of the correct picture (returning to the function that caught the exception.) This artifact comes from most instrumentation methods not calling the "on return" hook when unwinding the stack. This annoyance is avoided as long as you enable tracing for functions catching exceptions (in which case funtrace traces enough info to get around the return hook not being called upon unwinding.)
* **Questionable linker flags**:
  * **clang XRay requires --allow-multiple-definition**. That's because funtrace needs to redefine XRay's on-call/on-return hooks, and there doesn't seem to be another way to do it. If XRay defines its hooks as "weak", this flag will no longer be needed.
  * **gcc -pg _precludes_ -Wl,--no-undefined**. That's because its on-return hook, `__return__`, doesn't have a default definition (though its on-entry hook, `__fentry__`, apprently does, as do the entry/return hooks called by -finstrument-functions); your shared objects will get it from the executable but they won't link with `-Wl,--no-undefined`. Note that _all_ the wrappers filter out `-Wl,--no-undefined` so that shared libraries can use the `funtrace_` runtime APIs exported by the executable. However, you don't have to use the runtime APIs in shared objects - you can take snapshots only from code linked into the executable - so except for the -pg mode, this flag is not strictly necessary.

A few more words about XRay:

* **XRay instrumentation was enabled in shared libraries in late 2024** and is not yet available in officially released versions. clang versions with XRay shared library support have the `-fxray-shared` flag.
* **XRay uses dynamic code patching for enabling/disabling tracing at runtime.** This is why tracing is off unless you run under `env XRAY_OPTIONS="patch_premain=true"`, or use XRay's runtime APIs to patch the code. Funtrace has its own API, `funtrace_enable/disable_tracing()`, but it deliberately _doesn't_ call XRay's code-patching APIs. Funtrace's API is a quick way to cut most of the overhead of tracing without any self-modifying code business. It's up to you to decide, if you use XRay, whether you want to cut even more overhead by using runtime patching - downsides include creating copies of the code pages, for which you might not have the extra space, and taking more time than funtrace_enable/disable_tracing().

# Integrating funtrace into your build system

The short story is, **choose an instrumentation method and then compile in the way the respective wrapper in compiler-wrappers does.** However, here are some points worth noting explicitly:

* **It's fine to compile funtrace.cpp with its own compilation command.** You probably don't want to compile funtrace.cpp when linking your binary the way the wrappers do. They only do it to save you the trouble of adding funtrace.cpp to the list of files for the build system to build (which is harder/more annoying than it sounds, if you're trying to trace someone else's program with a build system you don't really know.)
* **It's best to compile funtrace.cpp without tracing, but "it can handle" being compiled with tracing.** Many build systems make it hard to compile a given file with its own compiler flags. funtrace.cpp uses NOFUNTRACE heavily to suppress tracing; the worst that can happen if you compile it with tracing is that some of its code will be traced despite its best efforts, but it should otherwise work.
* **funtrace.cpp must be compiled _into the executable_, not any of the shared libraries.** Funtrace uses TLS (thread-local storage) and accessing a `thread_local` object is a simple register+offset access when you link the code into an executable, but requires a function call if you link the code into a shared library, because now you need to find _this shared library's TLS area_. So funtrace puts its on-entry/return hooks into the executable, which exports them to the shared libraries.
* **Linker flag requirements** (XRay/`--allow-multiple-definition`, -pg/`-Wl,--no-undefined`) are documented in the previous section; for XRay, you also **need a linker wrapper** like `compiler-wrappers/xray/ld` to make sure funtrace's on-entry/return hooks from funtrace.o are passed before XRay's own hooks on the linker command line.
* **Pass -pthread** or things will break annoyingly
* **-Wl,--dynamic-list=funtrace.dyn** exports the funtrace runtime API from the executable for the shared libraries
* **-g is for source line info** (it's generally a good idea to use -g in release builds and not just debug builds - if it slows down linking, mold takes care of that; but, if you don't want to compile with -g, funtrace will still give you the function names using the ELF symbol table, only the source code will be missing from vizviewer)
* **Do _not_ pass -pg _to the linker_** - if you use gcc with -pg, and do pass it to the linker, the linker will think that you're compiling for gprof (even if you also pass `-mfentry -minstrument-return=call` which are guaranteed to break gprof, -pg's original application...), and then your program will write a useless gmon.out file in the current directory every time you run it.
* **Some flags in the wrappers are "defaults" that you can change**, specifically:
  * `g++ -finstrument-functions-exclude-file-list=.h,.hpp,/usr/include` - of course you can pass a different exclude list
  * `clang++ -finstrument-functions-after-inlining` - you can instead pass -finstrument-functions to instrument before inlining
  * `-fxray-instruction-threshold=...` is _not_ passed by the XRay wrapper - you can set your own threshold
 
**TODO** document funtrace++
  
# Runtime API for taking & saving trace snapshots

# Decoding traces

# Compile-time & runtime configuration

## Controlling which functions are traced

Control at function granularity is only available at build time, as follows:

* **Compiler function attributes**:
  * `NOFUNTRACE` - a function attribute excluding a function from tracing (eg `void NOFUNTRACE func()` - this is the `__attribute__((...))` syntax of gcc/clang).
  * `DOFUNTRACE` - a function attribute forcing the inclusion of a function in tracing - currently only meaningful for XRay, which might otherwise exclude functions due to the `-fxray-instruction-threshold=N` flag
* **Assembly filtering flags**: if you use the `funtrace++` wrapper around g++/clang++ in your build system (which you'd want to do solely to get the flags below), you get the option to filter compiler-generated assembly code to exclude some functions from tracing; this is convenient with foreign code (eg functions in standard or external library header files) as well as "to cast a wide net" based on function length a-la XRay's `-fxray-instruction-threshold=N` (_note that assembly filtering is not supported with XRay_):
  * `-funtrace-do-trace=file` - the file should contain a list of whitespace-separated mangled function names, these functions will NOT excluded from tracing
  * `-funtrace-no-trace=file` - the file should contain a list of whitespace-separated mangled function names, these functions WILL be excluded from tracing
  * `-funtrace-instr-thresh=N` - functions with less than N instructions will be excluded from tracing together with function calls inlined into them, UNLESS they have loops
  * `-funtrace-ignore-loops` - if -funtrace-instr-thresh=N was passed, functions with less than N instructions will be excluded from tracing together with function calls inlined into them, EVEN IF they have loops

There are thus several ways to ask to include or exclude a function from tracing; what happens if they conflict?

* NOFUNTRACE "always wins" (unless there's a compiler issue where it's ignored for whatever reason) - you can't trace a function successfully excluded with NOFUNTRACE
* DOFUNTRACE currently only means the function will survive XRay filtering; it does nothing for other instrumentation methods, so the function might be exluded from tracing with these methods (eg by -finstrument-functions-after-inling or -finstrument-functions-exclude-file-list)
* For functions which "survived exclusion by the compiler":
  * A function on the list passed to -funtrace-do-trace is always kept
  * Otherwise, a function on the list passed to -funtrace-no-trace is excluded, and so are function calls inlined into it
  * Otherwise, a function with less than N instructions where N was defined with -funtrace-instr-thresh=N and has no loops is excluded, and so are function calls inlined into it. If it has loops but -funtrace-ignore-loops was passed, it is also excluded, and so are function calls inlined into it.
 
## Disabling & enabling tracing

* `funtrace_ignore_this_thread()` excludes the calling thread from tracing "forever" (there's currently no way to undo this)
* `funtrace_disable_tracing()` disables tracing globally (note that taking a snapshot effectively does the same thing until the snapshot is ready)
* `funtrace_enable_tracing()` (re-)enables the tracing globally (by default, tracing is on when the program starts so you needn't do it; "on by default" means you can get a trace from a core dump and from a live process with SIGTRAP without any tweaking to the program source)

Additionally, compiling with -DFUNTRACE_FTRACE_EVENTS_IN_BUF=0 or setting $FUNTRACE_FTRACE_EVENTS_IN_BUF to 0 at runtime effectively disables ftrace scheduling event tracing, as mentioned again in the next section.

## Controlling buffer sizes & lifetimes

* `funtrace_set_thread_log_buf_size(log_buf_size)` sets the trace buffer size of the calling thread to `pow(2, log_buf_size)`. Passing 0 (or a value smaller than log(size of 2 trace entries), so currently 5) is equivalent to calling `funtrace_ignore_this_thread()`
* The following parameters can be controlled by passing `-DNAME=VALUE` to the compiler (the command line equivalent of `#define NAME VALUE`), and/or reconfigured at runtime by setting the environment variable `$NAME` to `VALUE`:
  * `FUNTRACE_LOG_BUF_SIZE`: each thread starts with a thread-local trace buffer of this size (the default is 20, meaning 1M bytes = 32K trace entries ~= 16K most recent function calls.) This initial buffer size can then be changed using `funtrace_set_thread_log_buf_size()`
  * `FUNTRACE_FTRACE_EVENTS_IN_BUF`: the number of entries in this process's userspace ftrace buffer (the default is 20000; the size in bytes can vary since each entry keeps one line of textual ftrace data.) Passing `-DFUNTRACE_FTRACE_EVENTS_IN_BUF=0` disables ftrace at compile time - this **cannot** be changed by setting the env var at runtime to a non-zero value.
  * `FUNTRACE_GC_MAX_AGE_MS`: when set to 0, a thread's thread-local trace buffer is freed upon thread exit - which means the trace data will be missing from future snapshots, even though the events in that buffer might have been recorded during the time range covered by the snapshot. When set to a non-zero value (default: 300 ms), thread trace buffers are kept after thread exit, and garbage-collected every FUNTRACE_GC_PERIOD_MS (see below); only buffers with age exceeding FUNTRACE_GC_MAX_AGE_MS are freed. Passing `-DFUNTRACE_GC_MAX_AGE_MS` disables garbage collection at compile time - this **cannot** be changed by setting the env var at runtime to a non-zero value.
  * `FUNTRACE_GC_PERIOD_MS`: unless compiled out by #defining FUNTRACE_GC_MAX_AGE_MS to 0, the thread trace buffer garbage collection runs every FUNTRACE_GC_PERIOD_MS ms (default: the compile-time value of FUNTRACE_GC_MAX_AGE_MS.)

# Limitations

* **Can't trace inside shared libraries unless they're loaded by an executable containing the funtrace runtime** - for example, a Python extension module written in C++ can't be traced, similarly to any other kind of plugin loaded by a program not compiled with funtrace. This is because of the TLS issue explained above.
* **Thread creation/exit and saving a trace snapshot take the same lock** - this can slow things down; hopefully not too badly since saving a snapshot is pretty fast, and creating lots of threads at runtime (rather than reusing from a thread pool) should be rare
* **ftrace / thread scheduling events might have issues near the snapshot time range boundaries**:
  * Perfetto might not render thread status very clearly near the boundaries even when it's clear from the ftrace log
  * There's a latency between a thread scheduling event and the moment it's delivered to funtrace's userspace thread collecting the events (we try to give this thread a high priority but will typically lack permissions to give it a real-time priority.) One way around this could be *a mechanism for "late delivery" of ftrace events into snapshots* - since most of the time, snapshots are written to the file system much later than they're captured, we could put ftrace events into those already-captured, but not-yet-written-out snapshots whose time range contains a given newly arrived event. Doable, but a bit of a hassle, could be done given demand.
* **Threads which exited by the time a snapshot was taken might be invisble in the trace** - unless the thread trace GC parameters were tuned such that the trace buffer is still around when the snapshot is taken, as explained above
* **Funcount misses constructor calls** - shouldn't matter for its goal of finding functions called so often that you want to exclude them from tracing to avoid the overhead
* **Overlapping time ranges** should never happen but might in some cases. The Perfetto/Chromium JSON spec requires events' time ranges to be nested within each other or not overlap at all. funtrace2viz takes this requirement seriously (rather than breaking it on the currently seemingly correct theory that some ways of breaking it are actually supported.) So when funtrace2viz observes that 20 functions have just returned (by seeing that f which called 19 functions has just returned, perhaps because of a longjmp or an exception being caught), it produces 20 different timestamps apart by at least 1 ns, the smallest time unit in the JSON. Some of these made-up return timestamps might cause overlap with later function calls.
* **Tail call artifacts** with some instrumentation methods, as documented in the section "Choosing compiler instrumentation"
* **Untraced exception catcher artifacts** with some instrumentation methods, as documented in the section "Choosing compiler instrumentation." A related but likely extremely rare artifact you might see with these instrumentation methods is mixing recursion and exception handling where you have a recursive function that doesn't catch an exception at the innermost recursion level but then does catch it at another level - funtrace trace analysis will incorrectly assume the exception was caught at the innermost level (unless `gcc -finstrument-functions` was used, which calls the on-return hook when unwinding the stack and doesn't require guesswork at trace analysis time.)
* **Unloading traced shared libraries within the time range of a snapshot is unsupported** - a trace snapshot contains an address space snapshot made at the end of the time range, so if a shared library was unloaded, functions traced from it will not be decodable in the trace; reusing the executable address space for new addresses will mess up decoding further. A need to dlclose libraries midway thru the tracing is probably extremely rare.
* **Mixing instrumentation methods in the same build or process wasn't tested** and might not work for various reasons; this feels like a fairly esoteric need, but can almost certainly be made to work given demand.
