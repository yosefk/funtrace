#include "test.h"
#include <pthread.h>
#include <unistd.h>
#include <thread>

void NI spin()
{
    volatile int n=0;
    for(n=0; n<100000000; ++n);
}

volatile int n = 0;

void NI sleep()
{
    usleep(150*1000);
    n++;
}

void NI child()
{
    pthread_setname_np(pthread_self(), "child");
    spin();
    sleep();
    spin();
}

void NI parent()
{
    spin();
    sleep();
    spin();
}

int main()
{
    //the trouble with ftrace is that there's no guarantee on event
    //delivery latency from the kernel to the userspace, so when you
    //take a snapshot, you might be missing some events; our sleeping
    //and busy loops are hopefully long enough for events to be consistently
    //observed when testing
    scope_tracer tracer;

    pthread_setname_np(pthread_self(), "parent");

    std::thread t(child);
    parent();

    t.join();
}
