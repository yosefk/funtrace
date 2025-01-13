#include "test.h"
#include <thread>
#include <pthread.h>

volatile int n = 0;

void NI f()
{
    n++;
}

int main()
{
    scope_tracer tracer;

    //this incidentally tests garbage collection (the thread dies by the time
    //the scope tracer is destroyed and we check that we get both threads' traces)
    //in addition to checking that we can set per-thread buffer sizes
    std::thread t([] {
        funtrace_set_thread_log_buf_size(5+4);
        pthread_setname_np(pthread_self(), "event_buf_16");
        //check that only 16 function calls out of these 100 are logged into the small buffer
        for(int i=0; i<100; ++i) {
            f();
        }
    });

    funtrace_set_thread_log_buf_size(5);
    pthread_setname_np(pthread_self(), "event_buf_1");
    //check that only one function call out of these 100 is logged into the small buffer
    for(int i=0; i<100; ++i) {
        f();
    }
    t.join();
}
