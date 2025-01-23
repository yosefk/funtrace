#include "test.h"
#include <thread>

volatile int n = 0;

void NI should_be_traced() { n++; }
//shouldn't be traced since it's called from an ignored thread
void NI shouldnt_be_traced() { n++; }

const char* g_child_name = "none";

void NI traced_thread()
{
    n++;
    pthread_setname_np(pthread_self(), g_child_name);
    should_be_traced();
    n++;
}

void NI ignored_thread()
{
    n++;
    shouldnt_be_traced();
    n++;
    funtrace_ignore_this_thread();
    shouldnt_be_traced();
    n++;
}

void run_threads()
{
    std::thread t1(traced_thread);
    std::thread t2(ignored_thread);
    should_be_traced();
    t1.join();
    t2.join();
}

int main()
{
    pthread_setname_np(pthread_self(), "main");
    scope_tracer tracer;

    g_child_name = "child1";
    run_threads();

    funtrace_disable_tracing();
    g_child_name = "child2";
    run_threads();

    funtrace_enable_tracing();
    g_child_name = "child3";
    run_threads();
}
