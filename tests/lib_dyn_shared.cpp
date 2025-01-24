#include "test.h"

volatile int dyn_shared_n;

void NI f_dyn_shared()
{
    dyn_shared_n++;
}

void NI g_dyn_shared()
{
    f_dyn_shared();
    f_dyn_shared();
    dyn_shared_n++;
}

void NI h_dyn_shared()
{
    g_dyn_shared();
    f_dyn_shared();
    dyn_shared_n++;
}

extern "C" void NI h_dyn_shared_c()
{
    h_dyn_shared();
    dyn_shared_n++;
}
