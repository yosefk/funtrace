#include "funtrace.h"

#define NI __attribute__((noinline))

struct scope_tracer
{
    uint64_t start_time = 0;
    const char* fname = nullptr;

    NOFUNTRACE scope_tracer(const char* f="funtrace.raw")
    {
        fname = f;
        start_time = funtrace_time();
    }

    NOFUNTRACE ~scope_tracer()
    {
        funtrace_snapshot* snapshot = funtrace_pause_and_get_snapshot_starting_at_time(start_time);
        funtrace_write_snapshot(fname, snapshot);
    };
};
