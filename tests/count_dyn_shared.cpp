#include "test.h"

volatile int shared_n;

char buf[256*1024]={1};

void NI f_dyn_shared()
{
    shared_n++;
}

void NI g_dyn_shared()
{
    f_dyn_shared();
    shared_n++;
    f_dyn_shared();
}

void NI h_dyn_shared()
{
    g_dyn_shared();
    shared_n++;
    f_dyn_shared();
}

extern "C" void NI h_dyn_shared_c()
{
    h_dyn_shared();
}
