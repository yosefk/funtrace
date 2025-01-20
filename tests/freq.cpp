#include "test.h"
#include <unistd.h>

volatile int n=0;

void NI usleep_1500()
{
    usleep(1500);
    n++;
}

int main()
{
    //test that we convert TSC to us correctly
    scope_tracer tracer;
    usleep_1500();
}
