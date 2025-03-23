# funtrace - a C/C++ function call tracer for x86/Linux

A function call tracer is a kind of profiler showing **a timeline of function call and return events**. Here's an example trace captured by funtrace from [Krita](https://krita.org):

![image](images/krita-trace.png)

Here we can see 2 threads - whether they're running or waiting, and the changes to their callstack over time - and the source code of a selected function.

Unlike a sampling profiler such as perf, **a tracing profiler must be told what to trace** using some runtime API, and also has a **higher overhead** than the fairly low-frequency sampling of the current callstack a-la perf. What do you get in return for the hassle and the overhead (and the hassle of culling the overhead, by disabling tracing of short functions called very often)? Unlike flamegraphs showing where the program spends its time on average, traces let you **debug cases of unusually high latency**, including in production (and it's a great idea to collect traces in production, and not just during development!)

If you're interested in why tracing profilers are useful and how funtrace works, see [Profiling in production with function call traces](https://yosefk.com/blog/profiling-in-production-with-function-call-traces.html). What follows is a funtrace user guide.

- [Why funtrace?](#why-funtrace)
- [Trying funtrace](#trying-funtrace)
- [Runtime API for taking & saving trace snapshots](#runtime-api-for-taking--saving-trace-snapshots)
- ["Coretime" API for saving trace snapshots](#coretime-api-for-saving-trace-snapshots)
- [Choosing a compiler instrumentation method](#choosing-a-compiler-instrumentation-method)
- [Integrating funtrace into your build system](#integrating-funtrace-into-your-build-system)
- [Culling overhead with `funcount`](#culling-overhead-with-funcount)
- [Decoding traces](#decoding-traces)
- [Compile time & runtime configuration](#compile-time--runtime-configuration)
  - [Controlling which functions are traced](#controlling-which-functions-are-traced)
  - [Disabling & enabling tracing](#disabling--enabling-tracing)
  - [Controlling buffer sizes & lifetimes](#controlling-buffer-sizes--lifetimes)
- [Limitations](#limitations)
- [Funtrace file format](#funtrace-file-format)

# Why funtrace?

* **Low overhead tracing** - FWIW, in my microbenchmark I get <10 ns per instrumented call or return
  * **6x faster** than an LLVM XRay microbenchmark with "flight recorder logging" and 15-18x faster than "basic logging"
  * **4.5x faster** than a uftrace microbenchmark (note that uftrace isn't just designed for a somewhat different workflow than funtrace - in that it's similar to XRay - but it also has many more features; [check it out](https://github.com/namhyung/uftrace)!)
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

You can clone the repo & build the trace decoder (or uinzip [a binary release](https://github.com/yosefk/funtrace/releases)), compile & run a simple example program, and decode its output traces as follows:

``` shell
# clone the source...
git clone https://github.com/yosefk/funtrace
# ...or unzip a binary release
unzip funtrace.zip

cd funtrace
./simple-example/build.sh
./simple-example/run.sh
```

This actually tests 4 different instrumented builds - 2 with gcc and 2 with clang; we'll discuss below how to choose the best method for you. Troubleshooting:

* With an older clang, you'll get `clang: error: unknown argument: '-fxray-shared'` - in that case, you can use 3 instrumentation methods out of the 4.
* You might have issues accessing ftrace data. This is not a problem for _function tracing_ but it prevents _thread state tracing_, which could tell us when threads are running and when they're waiting:

```
WARNING: funtrace - error initializing ftrace (...), compile with -DFUNTRACE_FTRACE_EVENTS_IN_BUF=0
  or run under `env FUNTRACE_FTRACE_EVENTS_IN_BUF=0` if you don't want to collect ftrace / see this warning
```

You can ignore this message, or disable ftrace as described in the message, or you can try making ftrace work. The problem is usually permissions, and one way to make ftrace usable permissions-wise is **`sudo chown -R $USER /sys/kernel/tracing`**. Inside containers, things are more involved, and you might want to consult a source knowing more than this guide.

You can view the traces produced from the simple example above as follows:

```
pip install viztracer
rehash
vizviewer out/funtrace-fi-gcc.json
vizviewer out/funtrace-pg.json
vizviewer out/funtrace-fi-clang.json
vizviewer out/funtrace-xray.json
```

Funtrace uses [viztracer](https://github.com/gaogaotiantian/viztracer) for visualizing traces, in particular because of its ability to show source code, unlike stock [Perfetto](https://perfetto.dev/) (the basis for vizviewer.)

To build your own program with tracing enabled, you can use `compiler-wrappers/funtrace-pg-g++`, `compiler-wrappers/funtrace-finstr-clang++` or the other two compiler wrapper scripts, just like `simple-example/build.sh` does. If the program uses autoconf/configure, you can set the `$CXX` env var to point to one of these scripts, and if it uses cmake, you can pass `-DCMAKE_CXX_COMPILER=/your/chosen/wrapper` to cmake.

Note that the compiler wrappers slow down the configuration stage, because they compile & link funtrace.cpp, and this is costly at build system config time if the build system compiles many small programs to test for compiler features, library availability and such. For the build itself, the overhead of compiling funtrace.cpp is lower, but might still be annoying if you use a fast linker like mold and are used to near-instantaneous linking. The good thing about the compiler wrappers is that they make trying funtrace easy; if you decide to use funtrace in your program, however, you will probably want to pass the required compiler flags yourself as described below, which will eliminate the build-time overhead of the compiler wrappers.

Once the program compiles, you can run it as usual, and then `killall -SIGTRAP your-program` (or `kill -SIGTRAP <pid>`) when you want to get a trace. The trace will go to `funtrace.raw`; if you use SIGTRAP multiple times, many trace samples will be written to the file. Now you can run `funtrace2viz` the way `simple-example/run.sh` does. You get the funtrace2viz binary from `funtrace.zip`; if you cloned the source repo, you should have funtrace2viz compiled if you ran `simple-example/build.sh`. funtrace2viz will produce a vizviewer JSON file from each trace sample in funtrace.raw, and you can open each JSON file in vizviewer.

Troubleshooting vizviewer issues:

* If you see **`Error: RPC framing error`** in the browser tab opened by vizviewer, **reopen the JSON from the web UI**. (Note that you want to run vizviewer on every new JSON file, _even if_ it gives you "RPC framing error" when you do it - you _don't_ want to just open the JSON from the web UI since then you won't see source code!)
* If **the timeline looks empty**, it's likely due to some mostly-idle threads having very old events causing the timeline to zoom out too much. (You can simply open the JSON with `less` or whatever - there's a line per function call; if the JSON doesn't look empty, funtrace is working.) **Try passing `--max-event-age` or `--oldest-event-time` to funtrace2viz**; it prints the time range of events recorded for each thread in each trace sample (by default, the oldest event in every sample gets the timestamp 0) and you can use these printouts to decide on the value of the flags. In the next section we'll discuss how to take snapshots at the time you want, of the time range you want, so that you needn't fiddle with flags this way.

If you build the program, run it, and decode its trace on the same machine/in the same container, life is easy. If not, note that in order for funtrace2viz to work, you need the program and its shared libraries to be accessible at the paths where they were loaded from _in the traced program run_, on the machine _where funtrace2viz runs_. And to see the source code of the functions (as opposed to just function names), you need the source files to be accessible on that machine, at the paths _where they were when the program was built_. If this is not the case, you can remap the paths using a file called `substitute-path.json` in the current directory of funtrace2viz, as described below.
As a side note, if you don't like having to remap source file paths - not just in funtrace but eg in gdb - see [refix](https://github.com/yosefk/refix) which can help to mostly avoid this.

Note that if you choose to try XRay instrumentation (`compiler-wrappers/funtrace-xray-clang++`), you need to run with `env XRAY_OPTIONS="patch_premain=true"` like simple-examples/run.sh does. With the other instrumentation options, tracing is on by default.

The above is how you can give funtrace a quick try. The rest tells how to integrate it in your program "for real."

# Runtime API for taking & saving trace snapshots

The next thing after trying funtrace with SIGTRAP is probably using the runtime API to take snapshots of interesting time ranges. (Eventually you'll want proper build system integration - but you probably want to "play some more" beforehand, and since snapshots taken with SIGTRAP aren't taken at "the really interesting times" and capture too much, you'll want to see better snapshots.)

The recommended method for taking & saving snapshots is:

* using `funtrace_time()` to find unusually high latency in every flow you care about
* ...then use `funtrace_pause_and_get_snapshot_starting_at_time()` to capture snapshots when a high latency is observed
* ...finally, use `funtrace_write_snapshot()` when you want to save the snapshot(s) taken upon the highest latencies

In code, it looks something like this:

```c++
#include "funtrace.h"

void Server::handleRequest() {
  uint64_t start_time = funtrace_time();

  doStuff();

  uint64_t latency = funtrace_time() - start_time;
  if(latency > _slowest) {
    funtrace_free_snapshot(_snapshot);
    _snapshot = funtrace_pause_and_get_snapshot_starting_at_time(start_time);
    _slowest = latency;
  }
}

Server::~Server() {
  funtrace_write_snapshot("funtrace-request.raw", _snapshot);
  funtrace_free_snapshot(_snapshot);
}
```

There's also `funtrace_pause_and_get_snapshot_up_to_age(max_event_age)` - very similar to `funtrace_pause_and_get_snapshot_starting_at_time(start_time)`; and if you want the full content of the trace buffers without an event age limit, there's `funtrace_pause_and_get_snapshot()`. And you can write the snapshot straight from the threads' trace buffers to a file, without allocating memory for a snapshot, using `funtrace_pause_and_write_current_snapshot()` (this is exactly what the SIGTRAP handler does.)

As implied by their names, **all of these functions pause tracing until they're done** (so that traced events aren't overwritten with new events before we have the chance to save them.) This means that, for example, a concurrent server where `Server::handleRequest()` is called from multiple threads might have a gap in one of the snapshots taken by 2 threads at about the same time; hopefully, unusual latency in 2 threads at the same time is rare, and even if does happen, you'll get at least one good snapshot.

All of the snapshot-saving functions write to files; an interface for sending the data to some arbitrary stream could be added given demand.

Finally, a note on the time functions:

* `funtrace_time()` is a thin wrapper around `__rdtsc()` so you needn't worry about its cost
* `funtrace_ticks_per_second()` gives you the TSC frequency in case you want to convert timestamps or time diffs to seconds/ns

# "Coretime API" for saving trace snapshots

While we're on the subject of snapshots - you can get trace data from a core dump by loading `funtrace_gdb.py` from gdb - by running `gdb -x funtrace_gdb.py`, or using the gdb command `python execfile("funtrace_gdb.py")`, or somewhere in `.gdbinit`. Then you'll get the extension command `funtrace` which works something like this:

```
(gdb) funtrace
funtrace: saving proc mappings
funtrace: core dump generated by `your-program arg1 arg2`
funtrace: thread 1287700 your-program - saving 1048576 bytes of data read from 0x7fb199c00000
funtrace: thread 1287716 child - saving 1048576 bytes of data read from 0x7fb17c200000
funtrace: saving 22 ftrace events
funtrace: done - decode with `funtrace2viz funtrace.raw out` and then view in viztracer (pip install viztracer) with `vizviewer out.json`
```

Basically it's what SIGTRAP would save to `funtrace.raw`, had it been called right when the core was dumped. Can be very useful to see what the program was doing right before it crashed.

# Choosing a compiler instrumentation method

Once you have snapshots of the right time ranges, you might want to settle on a particular compiler instrumentation method. For that, the below can be helpful as well as the next section, which talks about culling overhead with the `funcount` tool (one thing which will help you choose the instrumentation method is how much overhead it adds, which differs between programs, and funcount can help estimate that overhead.)

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

You can postpone "real" build system integration for as long as you want, if the compiler wrappers don't slow things down too much for you.
Once you do want to integrate funtrace into your build system, the short story is, **choose an instrumentation method and then compile in the way the respective wrapper in compiler-wrappers does.** However, here are some points worth noting explicitly:

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
* **Link the program as C++** - even if it's a C program, the funtrace runtime is in C++ and you'll need to link with g++ or clang++ for things to work
 
All the compiler wrappers execute `compiler-wrappers/funtrace++`, itself a compiler wrapper which implements a few flags - `-funtrace-instr-thresh=N`, `-funtrace-ignore-loops`, `-funtrace-do-trace=file`, and `-funtrace-no-trace=file` - for controlling which function get traced, by changing the assembly code produced by the compiler. If you don't need any of these flags, you needn't prefix your compilation command with `funtrace++` like the wrappers do. (Funtrace needn't touch the code generated by the compiler for any reason other than supporting these flags.)

# Culling overhead with `funcount`

If tracing slows down your program too much, you might want to exclude some functions from tracing. You can do this on some "wide basis", such as "no tracing inside this bunch of libraries, we do compile higher-level logic to trace the overall flow" or such. You can also use `-fxray-instruction-threshold` or `-funtrace-instr-thresh` to automatically exclude short functions without loops. But you might also want to do some "targeted filtering" where you **find functions called very often, and exclude those** (to save both cycles and space in the trace buffer - with many short calls, you need a much larger snapshot to see far enough into the past.)

`funcount` is a tool for counting function calls, which is recommended for finding "frequent callees" to exclude from traces. Funcount is:

* **Fast** (about as fast as funtrace and unlike the very slow callgrind)
* **Accurate** (unlike perf which doesn't know how many time a function was called, only how many cycles were spent there and only approximately with its low frequenchy sampling)
* **Thread-safe** (unlike gprof which produces garbage call counts with multithreaded programs)
* **Small** (~300 LOC) and easy to port

Finally, funcount **counts exactly the calls funtrace would trace** - nothing that's not traced is counted, and nothing that's traced is left uncounted.

You enable funcount by passing `-DFUNTRACE_FUNCOUNT` on the command line (only `funtrace.cpp` and `funtrace_pg.S` need this -D, you don't really need to recompile the whole program), or by compiling & linking `funcount.cpp` and `funcount_pg.S` instead of `funtrace.cpp` and `funtrace_pg.S` into your program - whichever is easier in your build system. If the program runs much slower than with funtrace (which can be very slow if you instrument before inlining but otherwise is fairly fast), it must be multithreaded, with the threads running the same concurrently and fighting over the ownership of the cache lines containing the call counters maintained by funcount. You can compile with `-DFUNCOUNT_PAGE_TABLES=16` or whatever number to have each CPU core update its own copy of each call counter, getting more speed in exchange for space (not that much space - each page table is at worst the size of the executable sections, though on small machines this might matter.)

At the end of the run, you will see the message:

`function call count report saved to funcount.txt - decode with funcount2sym to get: call_count, dyn_addr, static_addr, num_bytes, bin_file, src_file:src_line, mangled_func_name`

`funcount2sym funcount.txt` prints the columns described in the message to standard output; the most commonly interesting ones are highlighted in bold:

* **`call_count` - the number of times the function was called**
* `dyn_addr` - the dynamic address of the function as loaded into the process (eg what you'd see in `gdb`)
* `static_addr` - the static address of the function in the binary file (what you'd see with `nm`)
* `num_bytes` - the number of bytes making up the function, a proxy for how many instructions long it is
* `bin_file` - the executable or shared library containing the function
* **`src_file:src_line` - the source file & line where the function is defined**, separated by ":"
* **`mangled_func_name` - the mangled function name**; you can pipe funcount2sym through `c++filt` to demangle it, though often you will want the mangled name

You can sort this report with `sort -nr` and add reports from multiple runs together with `awk`. To exclude frequently called functions from tracing, you can use the `NOFUNTRACE` attribute (as in `void NOFUNTRACE myfunc()`); `#include "funtrace.h"` to access the macro. You can also use the `-funtrace-no-trace=file` flag implemented by `funtrace++`, and pass it a file with a list of _mangled_ function names. See also "Disabling and enabling tracing" below. This might be faster than opening every relevant source file and adding `NOFUNTRACE` to every excluded function definition, and it avoids issues where the function attribute doesn't exclude the function for whatever reason.

The advantage of the NOFUNTRACE attribute, apart from being kept together with the function definition (so you know easily what's traced and what's not), is that the overhead is **fully** removed, whereas `-funtrace-no-trace=file` only removes most of the overhead - it removes the calls to the entry/exit hooks, but the code is still "scarred" by the code having been generated. This is a small fraction of the overhead but if lots and lots of functions are "scarred" this way, it can add up.

If the source files aren't where the debug info says they are, and/or the executable or shared objects are not where they were when the process was running, you can use `substitute-path.json` in the current directory of `funcount2sym` same as with `funtrace2viz`, as described in the next section.

# Decoding traces

`funtrace2viz funtrace.raw out` will produce an `out.json`, `out.1.json`, `out.2.json` etc. per trace sample in the file. (The snapshot-saving functions only put one sample into a file; the `funtrace.raw` file appended to by SIGTRAP and its programmatic equivalent can contain multiple samples.)

If funtrace2viz can't find some of the source files or binaries it needs, it will print warnings; you can make it find the files using a `substitute-path.json` in its current directory. This JSON file should contain an array of arrays of length 2, for example:

``` json
[
  ["/build/server/source-dir/","/home/user/source-dir/"],
  ["/deployment/machine/binary-dir/","/home/user/binary-dir/"],
]
```
For every path string, funtrace2viz iterates over every pair in the array, replacing every occurence of the first string with the second string in the pair.

Command line flags:

* `-r/--raw-timestamps`: report the raw timestamps, rather than defining the earliest timestamp in each sample as 0 and counting from there
* `-e/--executable-file-info`: on top of a function's name, file & line, show the binary it's from and its static address
* `-m/--max-event-age`: ignore events older than this age; this is most likely to be useful for SIGTRAP-type snapshots where you have very old events from mostly idle threads and they cause the GUI timeline to zoom out so much you can't see anything. You can guess what the age is in part by looking at the printouts of funtrace2viz which tells the time range of the events traced from each thread
* `-e/--oldest-event-time`: like `--max-event-age` but with the threshold defined as a timestamp instead of age
* `-t/--threads`: a comma-separated list of thread TIDs - threads outside this list are ignored (including for the purpose of interpreting `--max-event-age` - if you ignore the thread with the most recent event, then the most recent event from threads you didn't ignore becomes "the most recent event" for age calculation purposes.) This is also something that's mostly useful for SIGTRAP-type snapshots to exclude mostly idle threads
* `-s/--samples`: a comma-separated list of sample indexes - samples outside this list are ignored. Useful for the multi-sample `funtrace.raw` file appended to by SIGTRAP
* `-d/--dry`: useful for a very large multi-sample `funtrace.raw` file if you want to decide what samples to focus on; this prints the time ranges of the threads in each sample, but doesn't decode anything (decoding runs at a rate of about 1MB of binary data per second)

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

# Funtrace file format

You don't need to know this format unless you want to generate or process `funtrace.raw` files, or extend funtrace for your needs.

Funtrace data is binary, using little endian encoding for integers. It consists of "chunks" where each chunk has an 8-byte magic number, a 64-bit size integer, and then a sequence of data bytes of the length specified by the size integer. Here are the chunk types and the format of the data:

* **`PROCMAPS`**: the content of `/proc/self/maps` can go here; only the start, end, offset and path fields are used, and only the executable segments are listed at this stage (funtrace uses `dl_iterate_phdr` rather than `/proc/self/maps` to speed up snapshotting), but readonly data segments might go here eventually, too, eg if we implement custom log messages with [delayed formatting](https://yosefk.com/blog/delayed-printf-for-real-time-logging.html). Only the start, end, offset and path fields are used; permissions and inode info are ignored.
* **`FUNTRACE`**: an 8-byte chunk indicating the start of a snapshot, with an 8-byte frequency of the timestamp counter, used to convert counter values into nanoseconds. A snapshot is interpreted according to the memory map reported by the last encountered `PROCMAPS` chunk (there may be many snapshots in the same file; currently the funtrace runtime saves a `PROCMAPS` chunk every time it takes a snapshot but if you know that your memory map remains stable over time and you want to shave off a little bit of latency, you could tweak this.)
* **`CMD LINE`**: the process command line, used as the process name when generating the JSON. A wart worth mentioning is that currently, the funtrace runtime reads this from `/proc/self/cmdline` and replaces null characters separating the arguments with spaces, which means that the shell command `prog "aaa bbb"`, which passes a single string argument `aaa bbb`, will be saved as `prog aaa bbb` (two string arguments). So we save enough to help you see "the trace of what you're looking at" but not enough to eg use the saved command line for reproducing the run.
* **`THREADID`**: a 64b PID integer, a 64b TID integer, and a null-terminated 16-byte name string (the content of `/proc/self/comm` aka the output of `pthread_getname_np(pthread_self(),...)`.) This precedes every `TRACEBUF` chunk (documented next.)
* **`TRACEBUF`**: a variable sized chunk of length which is a multiple of 16. It contains trace entries; each entry is a 64b code pointer, and a 64b timestamp counter value. The entries are _not_ sorted by the timestamp, for 2 reasons - they come from a cyclic buffer, and the funtrace writeout code is racy, so you can have rare cases of `new_entry, old_entry, new_entry` near the end of the cyclic buffer because one of the newest entries didn't make it into the buffer so you got a much older entry. So you need to sort the entries for processing, and you need to "defend" against missing events (meaning, you could see a return without a call or a call without a return; this is not just because of the raciness of the writeout but because the cyclic buffer ends before "the end of program execution" and starts after "the start of execution" and you can have various other niceties like longjmp.) The code pointer can have the following flags set in its high bits:
  * `RETURN` (63): a return event, where the code pointer points into the returning function
  * `RETURN_WITH_CALLER_ADDRESS` (62): a return event where the code pointer points _into the function we're returning to_. This unfortunate tracing artifact happens under XRay instrumentation; funtrace2viz mostly recovers the flow despite this. When this bit and the previous bit are both set, this is a `CATCH` event, and the code pointer points into the function that caught the exception.
  * `CALL_RETURNING_UPON_THROW` (61): marks call events that will have a return event logged for them if an exception is thrown. Under most instrumentation methods this does not happen and so funtrace2viz guesses which functions effectively returned during stack unwinding. When it sees a call entry with this flag set, it knows that this function wouldn't return without logging a return event even if an exception was thrown, which prevents it from wrongly guessing that the function returned due to unwinding.
* **`FTRACETX`**: a variable-sized chunk containing textual ftrace data (one event per line - what you read from `/sys/kernel/tracing/trace_pipe`). The timestamps in this data and the trace entries from `TRACEBUF` are from the same time source.
* **`ENDTRACE`**: an zero-sized chunk marking the end of a snapshot.
