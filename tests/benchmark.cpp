#include "test.h"
#include <cstdio>

volatile int n;

void NI NOFUNTRACE notrace()
{
    n=0;
}

void NI withtrace()
{
    n=0;
}

const int iter=1000000;

template<class F>
inline uint64_t time(F f, const char* msg, uint64_t base=0)
{
    int n=(iter/8)*8;
    auto start = funtrace_time();
    for(int i=0; i<iter/8; ++i) {
        f();
        f();
        f();
        f();
        f();
        f();
        f();
        f();
    }
    auto finish = funtrace_time();
    auto average = (finish-start)/n;
    if(base) {
        printf("%s: %ld cycles on average (%ld cycles of overhead)\n", msg, average, average-base);
    }
    else {
        printf("%s: %ld cycles on average\n", msg, average);
    }
    return average;
}

//this microbenchmark is of course not representative of performance impact in
//real code; it gives a rough idea of the overhead of instrumentation and tracing.
int main()
{
    uint64_t base_cost = time(notrace, "compiled without tracing");
    time(withtrace, "compiled with tracing, enabled at runtime", base_cost);
    funtrace_disable_tracing();
    time(withtrace, "compiled with tracing, disabled at runtime", base_cost);
    funtrace_pause_and_write_current_snapshot();
}
