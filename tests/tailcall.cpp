#include "test.h"

volatile int n;

void NI callee()
{
    n++;
}; 

void NI tail_caller()
{
    n++;
    callee();
}

void NI NOFUNTRACE callee_untraced()
{
    n++;
}

void NI tail_caller_untraced()
{
    n++;
    callee_untraced();
}

int main()
{
    scope_tracer tracer;
    for(int i=0; i<3; ++i) {
        tail_caller();
        tail_caller_untraced();
    }
}
