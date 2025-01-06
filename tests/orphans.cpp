#include "test.h"

volatile int n;
volatile uint64_t start_time;

void NI called_and_returned()
{
    n++;
}

void NI orphan_call_2()
{
    n++;
    called_and_returned();
    funtrace_write_snapshot("funtrace.raw", funtrace_pause_and_get_snapshot_starting_at_time(start_time));
    n++;
}

void NI orphan_call_1()
{
    n++;
    called_and_returned();
    orphan_call_2();
    n++;
}

void NI orphan_return_3()
{
    n++;
    start_time = funtrace_time();
    //we deliberately don't call a function here since
    //under XRay, this call is where the info on the identify of orphan_call_2
    //would come from (under XRay we record the returning function's _caller's_
    //return address, not the address of the returning function itself.)
    //
    //we also test funtrace2viz's ability to figure out the orphan's identify
    //from a previous return to it when returning from orphan_return_1 (which
    //does call functions, so its address gets recorded when they return)
    //
    //the call we don't have here:
    //called_and_returned();
    n++;
}

void NI orphan_return_2()
{
    n++;
    orphan_return_3();
    n++;
}

void NI orphan_return_1()
{
    n++;
    orphan_return_2();
    called_and_returned();
    n++;
}

void NI neither_call_nor_return_recorded()
{
    n++;
    orphan_return_1();
    called_and_returned();
    orphan_call_1();
    n++;
}

int main()
{
    neither_call_nor_return_recorded();
}
