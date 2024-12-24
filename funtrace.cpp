#include <x86intrin.h>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <mutex>
#include <vector>
#include <dlfcn.h>
#include <pthread.h>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include "funtrace.h"
#include "funtrace_buf_size.h"

//we could ask the user to compile this file without -finstrument-functions/-pg
//instead of peppering all of the code with NOINSTR. but this file wants
//to be compiled into the executable (rather than be its own shared object
//with its own build script - which would make TLS access slower), and we
//don't want the user to have to figure out how to compile all of their program
//with -finstrument-functions/-pg but not use these compiler flags with this one file
//which compiles together with the other files in that program - could be hard
//in some build systems.
//
//note that we do things like add explicit ctors/dtors to non-POD structs
//which would have these generated by the compiler - because this way we can
//put NOINSTR on them...
#define NOINSTR __attribute__((no_instrument_function))

const int funtrace_buf_size = FUNTRACE_BUF_SIZE; //for debug info
//(#define macros aren't visible with plain -g and need fancier flags that most
//build systems don't pass; and FUNTRACE_BUF_SIZE must be a #define
//to be useable from funtrace_pg.s)

//if you modify this, update funtrace_pg.S as well
struct trace_entry
{
    void* func;
    uint64_t cycle;
};

struct trace_data
{
    trace_entry* pos; //pos points into buf which must be aligned to FUNTRACE_BUF_SIZE
    bool enabled;
    trace_entry* buf;

    inline void NOINSTR pause_tracing() { enabled = false; }
    inline void NOINSTR resume_tracing() { enabled = true; }

    void NOINSTR allocate()
    {
        trace_entry* entries = nullptr;
        //we align the allocation to twice the buffer size so that after we increment the pos pointer,
        //we can clear the bit FUNTRACE_LOG_BUF_SIZE without worrying that the increment
        //carried values to bits at higher positions
        int memret = posix_memalign((void**)&entries, FUNTRACE_BUF_SIZE*2, FUNTRACE_BUF_SIZE);
        (void)memret;
        buf = entries;
        pos = entries;
        enabled = true;
    }

    void NOINSTR free()
    {
        ::free(buf);
        enabled = false;
        buf = nullptr;
        pos = nullptr;
    }

    inline void NOINSTR trace(void* ptr, uint64_t is_ret);
};

inline void NOINSTR trace_data::trace(void* ptr, uint64_t is_ret)
{
    static_assert(sizeof(trace_entry) == 16);

    bool paused = !enabled;
    trace_entry* entry = pos;

    uint64_t cycle = __rdtsc();
    uint64_t func = ((uint64_t)ptr) | (is_ret << FUNTRACE_RETURN_BIT);

    if(paused) {
        return;
    }
    //this generates prefetchw and doesn't impact povray's runtime (with any of +4, 8, 12, 16)
    //__builtin_prefetch(entry+4, 1, 0);

    //straightforward writing:
    entry->func = (void*)func;
    entry->cycle = cycle;

    //this generates movntq and makes povray slower relatively to straightforward writing
    //__m64* pm64 = (__m64*)entry;
    //_mm_stream_pi(pm64, _m_from_int64(func));
    //_mm_stream_pi(pm64+1, _m_from_int64(cycle));

    //this generates vmovntdq and also makes povray slower
    //__m128i xmm = _mm_set_epi64x(cycle, func);
    //_mm_stream_si128((__m128i*)entry, xmm);

    entry = (trace_entry*)(uint64_t(entry + 1) & ~(1 << FUNTRACE_LOG_BUF_SIZE));
    //printf("%p 0x%x\n", (void*)entry, uint64_t(entry) & (FUNTRACE_BUF_SIZE*2-1));

    pos = entry;
}

thread_local trace_data g_thread_trace;

extern "C" void NOINSTR __cyg_profile_func_enter(void* func, void*) { g_thread_trace.trace(func, 0); }
extern "C" void NOINSTR __cyg_profile_func_exit(void* func, void*) { g_thread_trace.trace(func, 1); }

//funtrace_pg.S is based on assembly generated from these C functions, massaged
//to avoid clobbering the registers where arguments might have been passed to
//the caller of __fentry__ and __return__ (and in __return__'s case,
//also to avoid clobbering the caller's return value) - unfortunately didn't
//find a way to tell gcc not to do that given C code, other than that there's
//not really any need to use assembly rather than C here
//
//(the other changes to funtrace_pg.S in addition to register renaming are
//to put macros like FUNTRACE_LOG_BUF_SIZE instead of the constants generated by gcc)
#if 0
extern "C" void NOINSTR __fentry__() {
    g_thread_trace.trace(__builtin_return_address(0),0);
}
extern "C" void NOINSTR __return__() { 
    g_thread_trace.trace(__builtin_return_address(0),1); 
}
#endif

struct trace_global_state
{
    //a set is easier to erase from but slower to iterate over and we want
    //to iterate quickly when pausing the tracing
    std::vector<trace_data*> thread_traces;
    std::ofstream trace_file;
    std::mutex mutex; //guards both thread_traces (from newly created
    //and destroyed threads adding/removing their trace buffers to the set)
    //and trace_file (from calls to funtrace_pause_and_write_current_snapshot()
    //which might come from multiple threads)

    NOINSTR trace_global_state() {}
    NOINSTR ~trace_global_state() {}

    std::ostream& NOINSTR file()
    {
        if(!trace_file.is_open()) {
            trace_file.open("funtrace.raw", std::ios::binary);
        }
        return trace_file;
    }

    //should be called once - can't be called again before unregister_this_thread()
    void NOINSTR register_this_thread()
    {
        std::lock_guard<std::mutex> guard(mutex);
        thread_traces.push_back(&g_thread_trace);
    }

    //safe to call many times without calling register_this_thread() between the calls
    void NOINSTR unregister_this_thread()
    {
        std::lock_guard<std::mutex> guard(mutex);
        for(int i=0; i<(int)thread_traces.size(); ++i) {
            if(thread_traces[i] == &g_thread_trace) {
                thread_traces[i] = thread_traces.back();
                thread_traces.pop_back();
                break;
            }
        }
    }
};

trace_global_state g_trace_state;

// the code below for getting the TSC frequency is straight from an LLM,
// and I'm not sure when it doesn't work. when we see at runtime
// that it doesn't we fall back on parsing dmesg, and if that fails,
// on usleeping and measuring the number of ticks it took.
//
// LLVM XRay uses /sys/devices/system/cpu/cpu0/tsc_freq_khz but it
// recommends against using it in production and it's not available
// by default; see https://blog.trailofbits.com/2019/10/03/tsc-frequency-for-all-better-profiling-and-benchmarking/
//
// a better method for getting the TSC frequency would be most welcome.

// Structure to hold CPUID leaf 15H return values
struct cpuid_15h_t {
    uint32_t eax;  // denominator
    uint32_t ebx;  // numerator
    uint32_t ecx;  // nominal frequency in Hz
    uint32_t edx;  // reserved
};

// Function to execute CPUID instruction
static inline void NOINSTR cpuid(uint32_t leaf, uint32_t subleaf, 
                        uint32_t *eax, uint32_t *ebx,
                        uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__ (
        "cpuid"
        : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
        : "a" (leaf), "c" (subleaf)
    );
}

// Function to get TSC frequency using CPUID leaf 15H
static uint64_t NOINSTR get_tsc_freq(void) {
    struct cpuid_15h_t res;
    uint32_t max_leaf;
    
    // First check if CPUID leaf 15H is supported
    cpuid(0, 0, &max_leaf, &res.ebx, &res.ecx, &res.edx);
    if (max_leaf < 0x15) {
        //printf("CPUID leaf 15H not supported\n");
        return 0;
    }
    
    // Get values from leaf 15H
    cpuid(0x15, 0, &res.eax, &res.ebx, &res.ecx, &res.edx);
    
    // Check if values are valid
    if (res.eax == 0 || res.ebx == 0) {
        //printf("Invalid CPUID.15H values returned\n");
        return 0;
    }
    
    // If ECX is non-zero, it provides the nominal frequency directly
    if (res.ecx) {
        uint64_t tsc_hz = ((uint64_t)res.ecx * res.ebx) / res.eax;
        return tsc_hz;
    } else {
        // If ECX is zero, we need crystal clock frequency
        // This is typically 24MHz or 25MHz for Intel processors
        // You might want to get this from BIOS or assume a common value
        const uint64_t crystal_hz = 24000000; // Assume 24MHz
        uint64_t tsc_hz = (crystal_hz * res.ebx) / res.eax;
        return tsc_hz;
    }
}

static uint64_t cpu_cycles_per_second()
{
    uint64_t freq = 0;
    freq = get_tsc_freq();
    if(!freq) {
        FILE* f = popen("dmesg | grep -o '[^ ]* MHz TSC'", "r");
        float freq_mhz = 0;
        if(fscanf(f, "%f", &freq_mhz)) {
            freq = freq_mhz * 1000000;
        }
        pclose(f);
    } 
    if(!freq) {
        uint64_t start = __rdtsc();
        usleep(100*1000); //sleep for 100ms
        uint64_t finish = __rdtsc();
        freq = (finish - start)*10; //not too accurate but seriously we shouldn't ever need this code
    }
    return freq;
}

//we need this as a global variable for funtrace_gdb.py which might not run on the same CPU
//as the core dump it is used on
uint64_t g_funtrace_cpu_freq = cpu_cycles_per_second();

extern "C" uint64_t NOINSTR funtrace_ticks_per_second() { return g_funtrace_cpu_freq; }

struct funtrace_procmaps
{
    std::vector<char> data;
    NOINSTR funtrace_procmaps() {}
    NOINSTR ~funtrace_procmaps() {}
};

const int MAGIC_LEN = 8;

static void NOINSTR write_chunk(std::ostream& file, const char* magic, const void* data, uint64_t bytes)
{
    assert(strlen(magic) == MAGIC_LEN);
    file.write(magic, MAGIC_LEN);
    file.write((char*)&bytes, sizeof bytes);
    file.write((char*)data, bytes);
}

static void NOINSTR write_procmaps(std::ostream& file, funtrace_procmaps* procmaps)
{
    write_chunk(file, "PROCMAPS", &procmaps->data[0], procmaps->data.size());
}

struct event_buffer
{
    trace_entry* buf;
    uint64_t bytes;
};

static void NOINSTR write_tracebufs(std::ostream& file, const std::vector<event_buffer>& thread_traces)
{
    write_chunk(file, "FUNTRACE", &g_funtrace_cpu_freq, sizeof g_funtrace_cpu_freq);
    for(auto trace : thread_traces) {
        write_chunk(file, "TRACEBUF", trace.buf, trace.bytes);
    }
    write_chunk(file, "ENDTRACE", "", 0);
}

extern "C" void NOINSTR funtrace_pause_and_write_current_snapshot()
{
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);

    for(auto trace : g_trace_state.thread_traces) {
        trace->pause_tracing();
    }
    std::ostream& file = g_trace_state.file();
    funtrace_procmaps* procmaps = funtrace_get_procmaps();
    write_procmaps(file, procmaps);
    funtrace_free_procmaps(procmaps);

    //we don't allocate a snapshot - we save the memory for this by writing
    //straight from the trace buffers (at the expense of pausing tracing
    //for more time)
    //
    //(we didn't mind briefly allocating procmaps because it's very little data)
    std::vector<event_buffer> traces;
    for(auto trace : g_trace_state.thread_traces) {
        traces.push_back(event_buffer{trace->buf, FUNTRACE_BUF_SIZE});
    }
    write_tracebufs(file, traces);

    for(auto trace : g_trace_state.thread_traces) {
        trace->resume_tracing();
    }

    file.flush();
}

extern "C" funtrace_procmaps* NOINSTR funtrace_get_procmaps()
{
    std::ifstream maps_file("/proc/self/maps", std::ios::binary);
    if (!maps_file.is_open()) {
        std::cerr << "funtrace - failed to open /proc/self/maps, traces will be impossible to decode" << std::endl;
        return nullptr;
    }

    funtrace_procmaps* p = new funtrace_procmaps;

    p->data = std::vector<char>(
        (std::istreambuf_iterator<char>(maps_file)),
        std::istreambuf_iterator<char>());

    return p;
}

struct funtrace_snapshot
{
    std::vector<event_buffer> thread_traces;
    NOINSTR funtrace_snapshot() {}
    NOINSTR ~funtrace_snapshot() {}
};

funtrace_snapshot* NOINSTR funtrace_pause_and_get_snapshot()
{
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    for(auto trace : g_trace_state.thread_traces) {
        trace->pause_tracing();
    }
    funtrace_snapshot* snapshot = new funtrace_snapshot;
    for(auto trace : g_trace_state.thread_traces) {
        trace_entry* copy = (trace_entry*)new char[FUNTRACE_BUF_SIZE];
        memcpy(copy, trace->buf, FUNTRACE_BUF_SIZE);
        snapshot->thread_traces.push_back(event_buffer{copy, FUNTRACE_BUF_SIZE});
    }
    for(auto trace : g_trace_state.thread_traces) {
        trace->resume_tracing();
    }
    return snapshot;
}

extern "C" void NOINSTR funtrace_free_procmaps(funtrace_procmaps* procmaps)
{
    delete procmaps;
}

extern "C" void NOINSTR funtrace_free_snapshot(funtrace_snapshot* snapshot)
{
    for(auto trace : snapshot->thread_traces) {
        delete [] (char*)trace.buf;
    }
    delete snapshot;
}

extern "C" void NOINSTR funtrace_write_saved_snapshot(const char* filename, funtrace_procmaps* procmaps, funtrace_snapshot* snapshot)
{
    std::ofstream file(filename);
    write_procmaps(file, procmaps);
    write_tracebufs(file, snapshot->thread_traces);
}

extern "C" uint64_t NOINSTR funtrace_time()
{
    return __rdtsc();
}

static trace_entry* find_earliest_event_after(trace_entry* begin, trace_entry* end, uint64_t time_threshold, uint64_t pause_time)
{
    trace_entry e;
    e.cycle = time_threshold;
    trace_entry* p = std::lower_bound(begin, end, e, [=](const trace_entry& e1, const trace_entry& e2) {
        //treat events recorded later than pause_time as ordered _before_ the rest.
        //that's because we're passing this function ranges of events logged in the order of time
        //(so sorted by time), but they can potentially be overwritten at the beginning by events
        //recorded after we paused tracing (because we don't have a mechanism to wait for the actual pause
        //to take effect and threads can take time to notice the write to their pause flag.)
        //
        //so if binary search finds an event ordered after the pause time, it should look to its right,
        //and the array can be thought of as "events after pause time from oldest to newest followed
        //by "events before pause time from oldest to newest."
        //
        //note that we could avoid all this by a simple linear search from the pos pointer backwards,
        //but it's slower than a binary search finding the exact earliest event followed by memcpy
        //into an array of the correctly allocated size
        //
        //also note that strictly speaking, since the buffers are written by the traced threads
        //and read by a snapshotting thread, we could theoretically see stale data in the buffer
        //s.t. the entries would not be sorted. I think this will result in very few events being
        //lost on very rare occasions but maybe there's a pathology where find_earliest_event_after()
        //misses most of the relevant entries because of this with reasonably high likelihood.
        if(e1.cycle > pause_time && e2.cycle <= pause_time) {
            return true; //events after pause time are ordered before events before pause time
        }
        if(e2.cycle > pause_time && e1.cycle <= pause_time) {
            return false;
        }
        return e1.cycle < e2.cycle;
    });
    return p==end ? nullptr : p;
}

extern "C" struct funtrace_snapshot* NOINSTR funtrace_pause_and_get_snapshot_starting_at_time(uint64_t time)
{
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    for(auto trace : g_trace_state.thread_traces) {
        trace->pause_tracing();
    }
    funtrace_snapshot* snapshot = new funtrace_snapshot;
    uint64_t pause_time = funtrace_time();
    for(auto trace : g_trace_state.thread_traces) {
        trace_entry* pos = trace->pos;
        trace_entry* buf = trace->buf;
        trace_entry* end = buf + FUNTRACE_BUF_SIZE/sizeof(trace_entry);
        trace_entry* earliest_right = find_earliest_event_after(pos, end, time, pause_time);
        trace_entry* earliest_left = find_earliest_event_after(buf, pos, time, pause_time);
        uint64_t entries = (earliest_left ? pos-earliest_left : 0) + (earliest_right ? end-earliest_right : 0);
        trace_entry* copy = (trace_entry*)new char[entries*sizeof(trace_entry)];
        trace_entry* copy_to = copy;
        //we don't really care if the events in the output are sorted but the ones to the right of pos, if we have any,
        //would be the earlier ones
        if(earliest_right) {
            copy_to = std::copy(earliest_right, end, copy_to);
        }
        if(earliest_left) {
            copy_to = std::copy(earliest_left, pos, copy_to);
        }
        assert(uint64_t(copy_to - copy) == entries);
        snapshot->thread_traces.push_back(event_buffer{copy, entries*sizeof(trace_entry)});
    }
    for(auto trace : g_trace_state.thread_traces) {
        trace->resume_tracing();
    }
    return snapshot;
}

extern "C" struct funtrace_snapshot* NOINSTR funtrace_pause_and_get_snapshot_up_to_age(uint64_t max_event_age)
{
    return funtrace_pause_and_get_snapshot_starting_at_time(funtrace_time() - max_event_age);
}

extern "C" void NOINSTR funtrace_ignore_this_thread()
{
    g_trace_state.unregister_this_thread();
    g_thread_trace.free();
}

extern "C" void funtrace_disable_tracing()
{
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    for(auto trace : g_trace_state.thread_traces) {
        trace->pause_tracing();
    }
}


extern "C"  __attribute__((visibility("default")))  void funtrace_enable_tracing()
{
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    for(auto trace : g_trace_state.thread_traces) {
        trace->resume_tracing();
    }
}

// we interpose pthread_create in order to implement the thing that having
// a ctor & dtor for the trace_data struct would do, but more efficiently.
//
// we need a thread's thread_local trace_data to be added to g_trace_state.thread_traces
// set when a new thread is created, and we need it to be removed from this set
// when it is destroyed. a ctor & dtor would do it but the ctor slows down the trace()
// function which would need to check every time if the thread_local object was
// already constructed or not, and call the ctor if it isn't.
//
// interposing pthread_create lets us avoid this check in the trace() function.
//
// a more portable and/or succinct yet still efficient approach would be great!

typedef int (*original_pthread_create_type)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

struct pthread_args
{
    void* (*func)(void*);
    void* arg;
};

void* NOINSTR pthread_entry_point(void* arg)
{
    g_thread_trace.allocate();
    g_trace_state.register_this_thread();

    pthread_args* args = (pthread_args*)arg;
    void* ret = args->func(args->arg);
    delete args;

    g_trace_state.unregister_this_thread();
    g_thread_trace.free();
    return ret;
}

int NOINSTR pthread_create(pthread_t *thread, const pthread_attr_t *attr, 
                   void *(*start_routine)(void *), void *arg) {
    // Find the original pthread_create using dlvsym; using dlsym might give us
    // an older version without support for the attr argument... spoken from experience!
    static original_pthread_create_type original_pthread_create = NULL;
    if (!original_pthread_create) {
        original_pthread_create = (original_pthread_create_type)dlvsym(RTLD_NEXT, "pthread_create", "GLIBC_2.2.5");
        if (!original_pthread_create) {
            fprintf(stderr, "Error locating original pthread_create: %s\n", dlerror());
            exit(EXIT_FAILURE);
        }
    }

    pthread_args* args = new pthread_args;
    args->func = start_routine;
    args->arg = arg;
    // Call the original pthread_create
    return original_pthread_create(thread, attr, pthread_entry_point, args);
}

//...and we need to register the main thread's trace separately, this time using a ctor
//(it's not global ctors that are the problem, it's the thread_locals' ctors); we don't
//need a dtor - the main thread never "goes out of scope" until the program terminates
struct register_main_thread
{
    NOINSTR register_main_thread()
    {
        g_thread_trace.allocate();
        g_trace_state.register_this_thread();
    }

    NOINSTR ~register_main_thread()
    {
        g_trace_state.unregister_this_thread();
        g_thread_trace.free();
    }
}
g_funtrace_register_main_thread;

//we register a signal handler for SIGTRAP, and have a thread waiting for the signal
//to arrive and dumping trace data when it does. this is good for programs you don't
//want to modify beyond rebuilding (otherwise it's not so great since you can't time
//the event very well, but it might still be enough to get a feeling of what the program
//is doing)
#ifndef FUNTRACE_NO_SIGTRAP

#include <thread>
#include <atomic>
#include <csignal>

struct sigtrap_handler
{
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> quit;
    std::atomic<bool> done;

    static void NOINSTR signal_handler(int);
    void NOINSTR thread_func();
    NOINSTR sigtrap_handler()
    {
        mutex.lock();
        quit = false;
        done = false;
        thread = std::thread([this] {
            thread_func();
        });
        signal(SIGTRAP, signal_handler);
    }
    NOINSTR ~sigtrap_handler()
    {
        quit = true;
        while(!done) {
            mutex.unlock();
        }
        thread.join();
    }
}
g_funtrace_sigtrap_handler;

void NOINSTR sigtrap_handler::signal_handler(int)
{
    g_funtrace_sigtrap_handler.mutex.unlock();
}

void NOINSTR sigtrap_handler::thread_func()
{
    //we don't want to trace the SIGTRAP-handling thread
    funtrace_ignore_this_thread();
    while(true) {
        mutex.lock();
        if(quit) {
            done = true;
            break;
        }
        funtrace_pause_and_write_current_snapshot();
    }
}

#endif //FUNTRACE_NO_SIGTRAP
