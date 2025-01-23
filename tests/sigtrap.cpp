#include "test.h"
#include <thread>
#include <unistd.h>
#include <csignal>

volatile int n;

void NI traced_func()
{
    n++;
}

void NI traced_thread()
{
    while(true) {
        traced_func();
        n++;
    }
}

int main()
{
    funtrace_ignore_this_thread();

    std::thread t(traced_thread);
    t.detach();

    while(n < 100);

    kill(getpid(), SIGTRAP);
}
