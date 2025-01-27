# funtrace - a C++ function call tracer for x86/Linux

A function call tracer is a kind of profiler showing **a timeline of function call and return events**. Here's an example trace captured by funtrace from [Krita](https://krita.org):

![image](images/krita-trace.png)

Here we can see 2 threads - whether they're running or waiting, and the changes to their callstack over time - and the source code of a selected function.

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

# Compiling & linking with funtrace

# Runtime API for taking & saving trace snapshots

# Decoding traces

# Compile-time & runtime configuration

# Limitations

