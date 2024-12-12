#include <x86intrin.h>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <set>
#include <mutex>
#include <vector>
#include "funtrace.h"

#ifndef FUNTRACE_BUF_SIZE
//must be a power of 2
#define FUNTRACE_BUF_SIZE (1024*16)
#endif

#define NOINSTR __attribute__((no_instrument_function))

struct trace_entry
{
    void* func;
    uint64_t cycle;
};

struct trace_data
{
    int pos;
    trace_entry buf[FUNTRACE_BUF_SIZE/sizeof(trace_entry)];
    pthread_t thread;

//    trace_data();
//    ~trace_data();
};

struct trace_global_state
{
    std::set<trace_data*> thread_traces;
    std::mutex mutex;
    std::ofstream trace_file;

    NOINSTR trace_global_state() {}
    NOINSTR ~trace_global_state() {}

    std::ostream& NOINSTR file()
    {
        if(!trace_file.is_open()) {
            trace_file.open("funtrace.raw");
        }
        return trace_file;
    }
};

thread_local trace_data g_thread_trace;
trace_global_state g_trace_state;

//FIXME: don't do this via a global ctor!
/*
trace_data::trace_data()
{
    thread = pthread_self();
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    g_trace_state.thread_traces.insert(this);
}

trace_data::~trace_data()
{
}
*/

static inline void NOINSTR trace(void* ptr, uint64_t is_ret)
{
    uint64_t cycle = __rdtsc();
    static_assert(sizeof(trace_entry) == 16);
    int pos = g_thread_trace.pos;
    trace_entry* entry = (trace_entry*)((uint64_t)&g_thread_trace.buf + pos);
    pos = (pos+16) & (sizeof(g_thread_trace.buf)-1);
    entry->func = (void*)(((uint64_t)ptr) | (is_ret << 63));
    entry->cycle = cycle;
    g_thread_trace.pos = pos;
}

extern "C" void NOINSTR __cyg_profile_func_enter(void* func, void* caller) { trace(func, 0); }
extern "C" void NOINSTR __cyg_profile_func_exit(void* func, void* caller) { trace(func, 1); }

extern "C" void NOINSTR funtrace_save_maps()
{
    std::ifstream maps_file("/proc/self/maps", std::ios::binary);
    if (!maps_file.is_open()) {
        std::cerr << "funtrace - failed to open /proc/self/maps, traces will be impossible to decode" << std::endl;
        return;
    }

    std::vector<char> maps_data(
        (std::istreambuf_iterator<char>(maps_file)),
        std::istreambuf_iterator<char>());

    maps_file.close();

    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    std::ostream& file = g_trace_state.file();
    file.write("PROCMAPS", 8);
    uint64_t size = maps_data.size();
    file.write((char*)&size, 8);
    file.write(&maps_data[0], size);
}

extern "C" void NOINSTR funtrace_save_trace()
{
    //TODO: group traces better so we know which of them came together
    std::lock_guard<std::mutex> guard(g_trace_state.mutex);
    std::ostream& file = g_trace_state.file();
    for(auto trace : g_trace_state.thread_traces) {
        file.write("FUNTRACE", 8);
        uint64_t size = sizeof(trace->buf);
        file.write((char*)&size, 8);
        file.write((char*)&trace->buf, size);
    }
}

// we interpose pthread_create in order 
