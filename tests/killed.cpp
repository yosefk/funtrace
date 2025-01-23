#include "test.h"
#include <unistd.h>
#include <csignal>

int main()
{
    {
        scope_tracer empty;
    }
    //this will leave a tracer instance that we want some other run
    //of a funtrace-instrumented program to collect
    kill(getpid(), SIGKILL);
}
