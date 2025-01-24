#include "test.h"
#include <unistd.h>
#include <cstdlib>
#include <thread>
#include <pthread.h>

volatile int n;

void NI f()
{
    n++;
}

void NI g()
{
    f();
    n++;
}

void NOFUNTRACE child_inf()
{
    g();
    while(1);
}

void NOFUNTRACE child_fin()
{
    pthread_setname_np(pthread_self(), "child");
    g();
    usleep(150*1000); //to get ftrace events
    for(volatile int i=0; i<1000000000; ++i);
}

int main()
{
    {
        scope_tracer empty;
        //just so funtrace.raw is created
    }
    g();

    std::thread t1(child_inf);
    std::thread t2(child_fin);

    t2.join();

    //this will leave an ftrace tracer instance that we want some other run
    //of a funtrace-instrumented program to collect
    abort();
}
