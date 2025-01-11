#include "test.h"

volatile int n = 0;

void NI f()
{
    n++;
}

int main()
{
    scope_tracer tracer;
    funtrace_set_thread_log_buf_size(5);
    //check that only one function call out of these 100 is logged into the small buffer
    for(int i=0; i<100; ++i) {
        f();
    }
}
