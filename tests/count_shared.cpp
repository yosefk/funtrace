#include "test.h"

volatile int shared_n;

char buf[256*1024]={1};

void NI f_shared()
{
    shared_n++;
}

void NI g_shared()
{
    f_shared();
    shared_n++;
    f_shared();
}

void NI h_shared()
{
    g_shared();
    shared_n++;
    f_shared();
}
