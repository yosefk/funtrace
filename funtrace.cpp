//an easy way to make the compiler wrappers add the funcount runtime
//to the program instead of the funtrace runtime
#ifdef FUNTRACE_FUNCOUNT
#include "funcount.cpp"
#else 

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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "funtrace.h"
#include <link.h>
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
#define NOINSTR NOFUNTRACE

//(#define macros aren't visible with plain -g and need fancier flags that most
//build systems don't pass; and FUNTRACE_BUF_SIZE must be a #define
//to be useable from funtrace_pg.s)

//if you modify this, update funtrace_pg.S as well
struct trace_entry
{
    void* func;
    uint64_t cycle;
};

struct thread_id
{
    uint64_t pid;
    uint64_t tid;
    char name[16];
};

struct trace_data
{
    trace_entry* pos; //pos points into buf which must be aligned to FUNTRACE_BUF_SIZE
    bool enabled;
    trace_entry* buf;
    pthread_t thread;
    thread_id id;

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

    void NOINSTR update_name()
    {
        pthread_getname_np(thread, id.name, sizeof id.name);
    }
};

inline void NOINSTR trace_data::trace(void* ptr, uint64_t flags)
{
    static_assert(sizeof(trace_entry) == 16, "funtrace_pg.S assumes 16 byte trace_entry structs");

    bool paused = !enabled;
    trace_entry* entry = pos;

    uint64_t cycle = __rdtsc();
    uint64_t func = ((uint64_t)ptr) | flags;

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
extern "C" void NOINSTR __cyg_profile_func_exit(void* func, void*) { g_thread_trace.trace(func, 1ULL<<FUNTRACE_RETURN_BIT); }

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
    g_thread_trace.trace(__builtin_return_address(0),1ULL<<FUNTRACE_RETURN_BIT); 
}
#endif

static std::string NOINSTR get_cmdline()
{
    std::ifstream cmdline_file("/proc/self/cmdline", std::ios::binary);
    if(!cmdline_file) {
        return "UNKNOWN";
    }

    std::vector<char> buffer((std::istreambuf_iterator<char>(cmdline_file)), 
                             std::istreambuf_iterator<char>());
    
    // Replace null characters with spaces. this will misprepresent
    // the argument "A B" as two arguments, "A" and "B"; and if there are
    // special characters that must be escaped in the shell, you will get them
    // without any escaping. But, should be better than nothing when wondering
    // where some trace came from
    for (char &c : buffer) {
        if (c == '\0') {
            c = ' ';
        }
    }
    
    return std::string(buffer.begin(), buffer.end()-1); //-1 for the last null
}

static uint64_t cpu_cycles_per_second();

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
    uint64_t pid;
    std::string cmdline;
    uint64_t cpu_freq;
    int buf_size = FUNTRACE_BUF_SIZE; //for funtrace_gdb.py
    char* exe_path = nullptr;

    NOINSTR trace_global_state()
    {
        pid = getpid();
        cmdline = get_cmdline();
        cpu_freq = cpu_cycles_per_second();
        exe_path = realpath("/proc/self/exe", nullptr);
    }
    NOINSTR ~trace_global_state()
    {
        ::free(exe_path);
    }

    std::ostream& NOINSTR file()
    {
        if(!trace_file.is_open()) {
            trace_file.open("funtrace.raw", std::ios::binary);
        }
        return trace_file;
    }

    //can't be called more than once before unregister_this_thread()
    void NOINSTR register_this_thread()
    {
        std::lock_guard<std::mutex> guard(mutex);
        g_thread_trace.id.pid = pid;
        g_thread_trace.id.tid = gettid();
        g_thread_trace.thread = pthread_self();
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
        // let's try other methods
        return 0;
        /* this might have been an OK fallback but we probably have better fallbacks
        // If ECX is zero, we need crystal clock frequency
        // This is typically 24MHz or 25MHz for Intel processors
        // You might want to get this from BIOS or assume a common value
        const uint64_t crystal_hz = 24000000; // Assume 24MHz
        uint64_t tsc_hz = (crystal_hz * res.ebx) / res.eax;
        return tsc_hz;
        */
    }
}

static uint64_t NOINSTR cpu_cycles_per_second()
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

extern "C" uint64_t NOINSTR funtrace_ticks_per_second() { return g_trace_state.cpu_freq; }

const int MAGIC_LEN = 8;

static void NOINSTR write_chunk(std::ostream& file, const char* magic, const void* data, uint64_t bytes)
{
    assert(strlen(magic) == MAGIC_LEN);
    file.write(magic, MAGIC_LEN);
    file.write((char*)&bytes, sizeof bytes);
    file.write((char*)data, bytes);
}

static void NOINSTR write_procmaps(std::ostream& file, std::stringstream& procmaps)
{
    std::string s = std::move(procmaps).str();
    write_chunk(file, "PROCMAPS", &s[0], s.size());
    procmaps.str(std::move(s));
}

struct event_buffer
{
    trace_entry* buf;
    uint64_t bytes;
    thread_id id;
};

static void NOINSTR write_funtrace(std::ostream& file)
{
    write_chunk(file, "FUNTRACE", &g_trace_state.cpu_freq, sizeof g_trace_state.cpu_freq);
    write_chunk(file, "CMD LINE", g_trace_state.cmdline.c_str(), g_trace_state.cmdline.size());
}

static void NOINSTR write_endtrace(std::ostream& file)
{
    write_chunk(file, "ENDTRACE", "", 0);
}

static void NOINSTR write_tracebufs(std::ostream& file, const std::vector<event_buffer>& thread_traces)
{
    for(auto trace : thread_traces) {
        write_chunk(file, "THREADID", &trace.id, sizeof trace.id);
        write_chunk(file, "TRACEBUF", trace.buf, trace.bytes);
    }
}

//the default timestamp threshold of 1 cuts off uninitialized events with timestamp=0
static void ftrace_events_snapshot(std::vector<std::string>& snapshot, uint64_t earliest_timestamp=1);

static void NOINSTR write_ftrace(std::ostream& file, const std::vector<std::string>& events)
{
    if(events.empty()) {
        return;
    }
    uint64_t size = 0;
    for(const auto& s : events) {
        size += s.size() + 1; //+1 for the newline
    }
    file.write("FTRACETX", MAGIC_LEN);
    file.write((char*)&size, sizeof size);
    for(const auto& s : events) {
        file << s << '\n';
    }
}

//finding the executable segments using dl_iterate_phdr() is faster than reading /proc/self/maps
//and produces less segments since we ignore the non-executable ones
static int phdr_callback (struct dl_phdr_info *info,
                          size_t size, void *data) {
    std::stringstream& s = *(std::stringstream*)data;
    for(int i=0; i<info->dlpi_phnum; ++i ) {
        const auto& phdr = info->dlpi_phdr[i];
        //we only care about loadable executable segments (the likes of .text)
        if(phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X)) {
            //we print in "roughly" the format of /proc/self/maps, with arbitrary values for the fields we don't really care about
            uint64_t start_addr = info->dlpi_addr + phdr.p_vaddr;
            uint64_t end_addr = start_addr + phdr.p_memsz;
            const char* name = info->dlpi_name[0] ? info->dlpi_name : g_trace_state.exe_path;
            s << start_addr << '-' << end_addr << " r-xp " << phdr.p_vaddr << " 0:0 0 " << name << '\n';
        }
    }
    return 0;
}

static void NOINSTR get_procmaps(std::stringstream& procmaps)
{
    procmaps << std::hex;
    dl_iterate_phdr(phdr_callback, &procmaps);
}

extern "C" void NOINSTR funtrace_pause_and_write_current_snapshot()
{
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);

    for(auto trace : g_trace_state.thread_traces) {
        trace->pause_tracing();
    }
    std::ostream& file = g_trace_state.file();
    std::stringstream procmaps;
    get_procmaps(procmaps);
    write_procmaps(file, procmaps);

    //we don't allocate a snapshot - we save the memory for this by writing
    //straight from the trace buffers (at the expense of pausing tracing
    //for more time)
    //
    //(we didn't mind briefly allocating procmaps because it's very little data)
    std::vector<event_buffer> traces;
    for(auto trace : g_trace_state.thread_traces) {
        trace->update_name();
        traces.push_back(event_buffer{trace->buf, FUNTRACE_BUF_SIZE, trace->id});
    }
    write_funtrace(file);
    write_tracebufs(file, traces);

    for(auto trace : g_trace_state.thread_traces) {
        trace->resume_tracing();
    }

    std::vector<std::string> ftrace_snapshot;
    ftrace_events_snapshot(ftrace_snapshot);
    write_ftrace(file, ftrace_snapshot);
     
    write_endtrace(file);
    file.flush();
}

struct funtrace_snapshot
{
    std::vector<event_buffer> thread_traces;
    std::vector<std::string> ftrace_events;
    std::stringstream procmaps;
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
        trace->update_name();
        snapshot->thread_traces.push_back(event_buffer{copy, FUNTRACE_BUF_SIZE, trace->id});
    }
    for(auto trace : g_trace_state.thread_traces) {
        trace->resume_tracing();
    }
    ftrace_events_snapshot(snapshot->ftrace_events);
    get_procmaps(snapshot->procmaps);
    return snapshot;
}

extern "C" void NOINSTR funtrace_free_snapshot(funtrace_snapshot* snapshot)
{
    for(auto trace : snapshot->thread_traces) {
        delete [] (char*)trace.buf;
    }
    delete snapshot;
}

extern "C" void NOINSTR funtrace_write_snapshot(const char* filename, funtrace_snapshot* snapshot)
{
    std::ofstream file(filename);
    write_procmaps(file, snapshot->procmaps);
    write_funtrace(file);
    write_tracebufs(file, snapshot->thread_traces);
    write_ftrace(file, snapshot->ftrace_events);
    write_endtrace(file);
    file.flush();
}

extern "C" uint64_t NOINSTR funtrace_time()
{
    return __rdtsc();
}

static trace_entry* NOINSTR find_earliest_event_after(trace_entry* begin, trace_entry* end, uint64_t time_threshold, uint64_t pause_time)
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
        trace->update_name();
        snapshot->thread_traces.push_back(event_buffer{copy, entries*sizeof(trace_entry), trace->id});
    }
    for(auto trace : g_trace_state.thread_traces) {
        trace->resume_tracing();
    }
    ftrace_events_snapshot(snapshot->ftrace_events, time);
    get_procmaps(snapshot->procmaps);
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

extern "C" void NOINSTR funtrace_disable_tracing()
{
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    for(auto trace : g_trace_state.thread_traces) {
        trace->pause_tracing();
    }
}


extern "C" void NOINSTR funtrace_enable_tracing()
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

//interposing __cxa_begin_catch to handle exceptions (eg if f calls g which throws a exception
//that f catches and calls h, we want this to be understood as g returning and f calling h,
//rather than g calling h; which our interposing & tracing doesn't completely ensure but does make
//work in many cases)
extern "C" void* NOFUNTRACE __cxa_begin_catch(void *thrown_exception) throw() {
    void* caller = __builtin_return_address(0);

    g_thread_trace.trace(caller, FUNTRACE_CATCH_MASK);
    g_thread_trace.trace((void*)&__cxa_begin_catch, 0);

    static void *(*real_cxa_begin_catch)(void *) = nullptr;
    if (!real_cxa_begin_catch) {
        real_cxa_begin_catch = (void *(*)(void *))dlsym(RTLD_NEXT, "__cxa_begin_catch");
        if (!real_cxa_begin_catch) {
            fprintf(stderr, "Error locating original __cxa_begin_catch: %s\n", dlerror());
            return nullptr;
        }
    }

    void* ret = real_cxa_begin_catch(thrown_exception);

    g_thread_trace.trace((void*)&__cxa_begin_catch, 1ULL<<FUNTRACE_RETURN_BIT);
    return ret;
}

//we interpose __cxa_end_catch just in order to instrument it - so as to make it visible
//in the trace that an exception was caught. we don't instrument it "normally" however
//since in some cases funtrace.cpp might be compiled without instrumentation flags -
//so we actually make sure it's __NOT__ instrumented but call trace() directly
extern "C" void NOFUNTRACE __cxa_end_catch(void) throw() {
    static void (*real_cxa_end_catch)(void) = nullptr;

    g_thread_trace.trace((void*)&__cxa_end_catch, 0);

    if (!real_cxa_end_catch) {
        real_cxa_end_catch = (void (*)(void))dlsym(RTLD_NEXT, "__cxa_end_catch");
        if (!real_cxa_end_catch) {
            fprintf(stderr, "Error locating original __cxa_end_catch: %s\n", dlerror());
            return;
        }
    }

    real_cxa_end_catch();

    g_thread_trace.trace((void*)&__cxa_end_catch, 1ULL<<FUNTRACE_RETURN_BIT);
}

extern "C" void NOFUNTRACE
#ifdef __GNUC__
__cxa_throw(void* thrown_object, void* type_info, void (*dest)(void *))
#elif defined(__clang__)
__cxa_throw(void* thrown_object, std::type_info* type_info, void (_GLIBCXX_CDTOR_CALLABI *dest)(void*))
#endif
{
    static void (*real_cxa_throw)(void*, void*, void (*)(void*)) = nullptr;

    g_thread_trace.trace((void*)&__cxa_throw, 0);

    if(!real_cxa_throw) {
        real_cxa_throw = (void (*)(void*, void*, void (*)(void*)))dlsym(RTLD_NEXT, "__cxa_throw");
        if(!real_cxa_throw) {
            fprintf(stderr, "Error locating original __cxa_throw: %s\n", dlerror());
            return; 
        }
    } 

    //__cxa_throw doesn't return so we record it as a "point event", without logging
    //the actual time it takes
    g_thread_trace.trace((void*)&__cxa_throw, 1ULL<<FUNTRACE_RETURN_BIT);
    real_cxa_throw(thrown_object, type_info, dest);
}

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

    static void NOINSTR signal_handler(int);
    void NOINSTR thread_func();
    NOINSTR sigtrap_handler()
    {
        quit = false;
        mutex.lock();
        thread = std::thread([this] {
            thread_func();
        });
        signal(SIGTRAP, signal_handler);
    }
    NOINSTR ~sigtrap_handler() {
        quit = true;
        mutex.unlock();
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
    pthread_setname_np(pthread_self(), "funtraceSIGTRAP");
    while(true) {
        mutex.lock();
        if(quit) {
            break;
        }
        funtrace_pause_and_write_current_snapshot();
    }
}

#endif //FUNTRACE_NO_SIGTRAP

#ifndef FUNTRACE_NO_FTRACE

struct ftrace_event
{
    NOINSTR ftrace_event() {}
    NOINSTR ~ftrace_event() {}

    uint64_t timestamp;
    std::string line;
};

struct ftrace_handler
{
    std::mutex mutex;
    std::thread thread;
    std::string base;
    bool init_errors = false;
    std::vector<ftrace_event> events; //cyclic buffer
    int pos = 0;
    std::atomic<bool> quit;

    NOINSTR ftrace_handler() {
        quit = false;
        if(getenv("FUNTRACE_NO_FTRACE")) {
            init_errors = true;
            return;
        }
        ftrace_init();
        if(init_errors) {
            //no point in spawning a thread to collect ftrace events
            return;
        }
        mutex.lock();
        events.resize(FUNTRACE_FTRACE_EVENTS_IN_BUF);
        thread = std::thread([this] {
            thread_func();
        });
        //wait for the thread to unlock the mutex to make sure it started
        mutex.lock();
        mutex.unlock();
    }
    NOINSTR ~ftrace_handler() {
        //we make sure the thread is awakened by a thread-spawning event,
        //and then wait for it to quit.
        quit = true;
        auto dummy = std::thread([] {});
        thread.join();
        dummy.join();
    }
    void NOINSTR thread_func();
    void NOINSTR ftrace_init();
    void NOINSTR warn(const char* what)
    {
        if(init_errors) {
            return;
        }
        printf("WARNING: funtrace - error initializing ftrace (%s - %s), compile with -DFUNTRACE_NO_FTRACE or "
               "setenv FUNTRACE_NO_FTRACE at runtime if you don't want to collect ftrace / see this warning\n", what, strerror(errno));
        init_errors = true;
    }
    void NOINSTR write_file(const char* file, const char* contents)
    {
        std::string fullpath = base+file;
        std::ofstream f(fullpath);
        if(!f) {
            warn(("failed to open " + fullpath).c_str());
        }
        f << contents;
    }
    //we're parsing the timestamp from a line like this:
    //  main-58704   [010] d.... 1473223221396767: sched_switch: prev_comm=main prev_pid=58704 prev_prio=120 prev_state=D ==> next_comm=swapper/10 next_pid=0 next_prio=120
    static uint64_t NOINSTR parse_timestamp(const std::string& line)
    {
        const char* p = line.c_str();
        //we're expecting ": "...
        const char* q = strstr(p, ": ");
        if(!q) {
            return 0;
        }
        //...preceded by " [0-9]+"
        uint64_t time = 0;
        uint64_t mult = 1;
        while(q > p && *--q != ' ') {
            if(*q < '0' || *q > '9') {
                return 0;
            }
            time += (*q - '0')*mult;
            mult *= 10;
        }
        if(*q != ' ') {
            return 0;
        }
        return time;
    }
    void NOINSTR events_snapshot(std::vector<std::string>& snapshot, uint64_t earliest_timestamp);
}
g_ftrace_handler;

void NOINSTR ftrace_handler::ftrace_init()
{
    //create our own tracer instance (note that we name it "after ourselves" but we don't
    //eg mangle the name by PID to ensure it's unique since that would require cleaning it up
    //upon process termination to avoid creating too many and not sure how this should be done;
    //there's some trick using mount which apparently requires root permissions/capabilities
    //or we could have a process removing it when this process dies but this sounds like it
    //could come with its own problems)
    char buf[128]={0};
    pthread_getname_np(pthread_self(), buf, sizeof buf);
    base = std::string("/sys/kernel/tracing/instances/funtrace.") + buf + "/";
    struct stat s;
    if(stat(base.c_str(), &s) && mkdir(base.c_str(), 0666)) {
        warn(("failed to create ftrace instance directory "+base).c_str());
        return;
    }
    //disable tracing clear the trace buffer (our instance could have kept some data from last time)
    write_file("tracing_on", "0");
    write_file("trace", "");

    //the events Perfetto traces & looks at judging by a simple experiment
    //(running their Linux tracing tutorial and checking the collected trace)
    write_file("events/sched/sched_switch/enable", "1");
    write_file("events/sched/sched_waking/enable", "1");
    write_file("events/task/task_newtask/enable", "1");
    write_file("events/task/task_rename/enable", "1");

    //only trace events from this PID...
    char pid[128];
    sprintf(pid,"%d",getpid());
    write_file("set_event_pid", pid);
    //...and threads & processes forked by it
    write_file("options/event-fork", "1");

    //use TSC for timestamps so we can sync with funtrace timestamps
    write_file("trace_clock", "x86-tsc");
}

//experimentally, it takes ~100 ms to read /sys/kernel/tracing/trace and count
//its lines using wc -l, with /sys/kernel/tracing/buffer_total_size_kb at 448K
//and ~10K events of the type we're listening to logged. if this was faster
//(which maybe it could be, if we were using the binary ring buffer format
//rather than the textual format - but that requires quite a bit more knowledge
//of ftrace), it could make sense to read the entire trace buffer when taking
//a snapshot (and delay its parsing done in order to trim the time range of the
//events to the time range of funtrace events to when the snapshot is saved).
//but given the relatively high latency it seems better to accumulate events
//
//into our own buffer incrementally in a background thread; as an added
//bonus we can easily trim the events by a time range by the time we need
//to take a snapshot since we're parsing the timestamps incrementally as well.
//
//a nice side effect of reading ftrace data into an internal buffer
//is being able to read it from a core dump without any provisions made at
//core dump time to save ftrace data aside
void NOINSTR ftrace_handler::thread_func()
{
    funtrace_ignore_this_thread();

    pthread_setname_np(pthread_self(), "funtrace-ftrace");
    //ignore scheduling events related to this thread (or it will read them from the pipe,
    //with this processing generating more events for it to read from the pipe...)
    char buf[128]={0};
    int tid = gettid();
    snprintf(buf, sizeof buf, "prev_pid != %d && next_pid != %d", tid, tid);
    write_file("events/sched/sched_switch/filter", buf);
    snprintf(buf, sizeof buf, "pid != %d && common_pid != %d", tid, tid);
    write_file("events/sched/sched_waking/filter", buf);

    //enable tracing
    write_file("tracing_on", "1");

    //attempt to set a high priority; SCHED_FIFO requires permissions
    //and is likely to fail, fall back on nice -20
    struct sched_param param;
    param.sched_priority = 99;
    if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &param)) {
        setpriority(PRIO_PROCESS, tid, -20);
    }

    std::ifstream trace_pipe(base+"trace_pipe");

    //signals that we started
    mutex.unlock();

    while(!quit) {
        std::string line;
        std::getline(trace_pipe, line);

        //std::cout << line << std::endl;
        mutex.lock();

        uint64_t timestamp = parse_timestamp(line);
        //std::cout << "  " << timestamp << std::endl;
        if(timestamp) { //some lines aren't events, eg ftrace might say that
            //CPU so and so lost so many events (though we hope our diligent
            //readout and careful event filtering will prevent this unfortunate
            //condition...)
            events[pos].line = line;
            events[pos].timestamp = timestamp;
            pos = (pos + 1) % events.size();
        }

        mutex.unlock();
    }
}

void NOINSTR ftrace_handler::events_snapshot(std::vector<std::string>& snapshot, uint64_t earliest_timestamp)
{
    auto copy = [](std::vector<std::string>::iterator to, const ftrace_event* from_b, const ftrace_event* from_e) -> std::vector<std::string>::iterator {
        while(from_b < from_e) {
            *to++ = from_b->line;
            from_b++;
        }
        return to;
    };
    std::lock_guard<std::mutex> guard(mutex);
    //the logic here is similar to that in funtrace_pause_and_get_snapshot_starting_at_time -
    //we treat the cyclic buffer as 2 sorted arrays - except there are no complications around
    //data getting overwritten while we're reading it since we're holding a mutex protecting
    //the cyclic buffer
    auto find_earliest_event_after = [&](const ftrace_event* b, const ftrace_event* e) {
        ftrace_event evt;
        evt.timestamp = earliest_timestamp;
        auto p = std::lower_bound(b, e, evt, [=](const ftrace_event& e1, const ftrace_event& e2) {
            return e1.timestamp < e2.timestamp;
        });
        return p == e ? nullptr : p;
    };
    auto buf = &events[0];
    auto pos = buf + this->pos;
    auto end = buf + events.size();
    auto earliest_right = find_earliest_event_after(pos, end);
    auto earliest_left = find_earliest_event_after(buf, pos);
    uint64_t entries = (earliest_left ? pos-earliest_left : 0) + (earliest_right ? end-earliest_right : 0);
    snapshot.resize(entries);
    auto copy_to = snapshot.begin();
    //for ftrace we want the events in the output to be sorted; those to the right of pos are the earlier ones
    if(earliest_right) {
        copy_to = copy(copy_to, earliest_right, end);
    }
    if(earliest_left) {
        copy_to = copy(copy_to, earliest_left, pos);
    }
    assert(uint64_t(copy_to - snapshot.begin()) == entries);
}

static void NOINSTR ftrace_events_snapshot(std::vector<std::string>& snapshot, uint64_t earliest_timestamp) 
{
    g_ftrace_handler.events_snapshot(snapshot, earliest_timestamp);
}

#else

static void NOINSTR ftrace_events_snapshot(std::vector<std::string>& snapshot, uint64_t earliest_timestamp) 
{
}

#endif //FUNTRACE_NO_FTRACE

#endif //FUNTRACE_FUNCOUNT
