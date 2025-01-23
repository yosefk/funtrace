#include "test.h"

volatile int shared_n;

void NI f_shared()
{
    shared_n++;
}

void NI g_shared()
{
    f_shared();
    shared_n++;
    f_shared();
    shared_n++;
}

void NI h_shared()
{
    g_shared();
    shared_n++;
    f_shared();
    shared_n++;
}
