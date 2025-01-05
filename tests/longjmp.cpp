//we use longjmp just as an example of something that breaks the assumption
//that you get a return-from-function event eventually after it was called -
//instead you have here a bunch of functions that are called and never returned
//from. we use this to test the ability of funtrace2viz to (somewhat) recover from such
//scenarios, of which the use of longjmp is one potential cause [which we could
//try to eliminate by interposing longjmp but it doesn't seem popular enough to
//bother and there are probably others]

#include <csetjmp>
#include "test.h"

volatile int n;
jmp_buf jmpbuf;

void NI jumper()
{
    n++;
    longjmp(jmpbuf, 1);
}

void NI wrapper_call()
{
    n++;
    jumper();
    n++;
}

void NI wrapper_call_outer()
{
    n++;
    wrapper_call();
    n++;
}

void NI before_setjmp()
{
    n++;
}

void NI after_longjmp()
{
    n++;
}

void NI setter()
{
    n++;
    before_setjmp();
    if(setjmp(jmpbuf)) {
        after_longjmp();
    }
    else {
        wrapper_call_outer();
    }
}

int main()
{
    scope_tracer tracer;
    for(int i=0; i<3; ++i) {
        setter();
    }
}

