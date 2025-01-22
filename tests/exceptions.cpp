#include "test.h"

volatile int n=0;

void NI thrower()
{
    n++;
    //interestingly if this is conditional, the test will fail
    //because of tail calls which we will "return" from before throwing
    //(and we could make the test pass by making it expect this ofc);
    //with unconditional throwing the compiler (-pg/-xray) doesn't bother
    //to generate any __return__/FunctionTailExit calls since it knows
    //they will never return
    throw "error";
}

void NI wrapper_call()
{
    n++;
    thrower();
    n++;
}

void NI wrapper_tailcall_1()
{
    n++;
    wrapper_call();
}

void NI wrapper_tailcall_2()
{
    n++;
    wrapper_tailcall_1();
}

void NI wrapper_call_outer()
{
    n++;
    wrapper_tailcall_2();
    n++;
}

void NI before_try()
{
    n++;
}

void NI after_catch()
{
    n++;
}

#ifndef UNTRACED_CATCHER
#define TRACE
#else
#define TRACE NOFUNTRACE
#endif

void NI TRACE catcher()
{
    n++;
    before_try();
    try {
        wrapper_call_outer();
    }
    catch(...) {
        after_catch();
    }
    n++;
}

void NI caller()
{
    n++;
    catcher();
    n++;
}

int main()
{
    scope_tracer tracer;
    for(int i=0; i<3; ++i) {
        caller();
    }
}
