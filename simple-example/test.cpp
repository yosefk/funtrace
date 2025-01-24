#include "funtrace.h"
#include <pthread.h>
#include <thread>

#define NL __attribute__((noinline))

volatile int n;

NL void f(int i)
{
    n = i;
}

void NL g(int i)
{
    f(i);
}

void NL h(int i) {
    g(i);
    g(i);
}

volatile int done = 0;

void shared_g(int a1, int a2, int a3, int a4, int a5, int a6);

int main()
{
    std::thread t([]{
            pthread_setname_np(pthread_self(), "child");
            for(int i=0; i<100000; ++i) {
                h(1);
            }
    });
    for(int i=0; i<100000; ++i) {
        g(2);
        shared_g(1,2,3,4,5,6);
    }
    t.join();

    funtrace_pause_and_write_current_snapshot();
}
