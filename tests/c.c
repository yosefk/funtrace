#include "funtrace.h"

#define NI __attribute__((noinline))

volatile int n;

void NI f()
{
    n++;
}

void NI g()
{
    f();
    n++;
    f();
    n++;
}

int main()
{
    uint64_t start = funtrace_time();

    g();

    funtrace_snapshot* snapshot = funtrace_pause_and_get_snapshot_starting_at_time(start);
    funtrace_write_snapshot("funtrace.raw", snapshot);
    funtrace_free_snapshot(snapshot);
}
