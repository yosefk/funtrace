# funtrace - a C++ function call tracer for x86/Linux

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

# Compiling & linking with funtrace

# Runtime API for taking & saving trace snapshots

# Decoding traces

# Compile-time & runtime configuration

# Limitations

