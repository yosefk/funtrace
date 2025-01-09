#include "test.h"

volatile int dyn_shared_n;

char buf_shared[256*1024]={1};

struct glob_dyn
{
    glob_dyn() { dyn_shared_n++; }
} gg_dyn;

void NI f_dyn_shared()
{
    dyn_shared_n++;
}

void NI g_dyn_shared()
{
    f_dyn_shared();
    dyn_shared_n++;
    f_dyn_shared();
}

void NI h_dyn_shared()
{
    g_dyn_shared();
    dyn_shared_n++;
    f_dyn_shared();
}

extern "C" void NI h_dyn_shared_c()
{
    h_dyn_shared();
}
