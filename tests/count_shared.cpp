#include "test.h"

volatile int shared_n;

//we want the libraries to be loaded far apart to make sure
//funcount actually finds the newly mapped executable segments
//as opposed to "being lucky" with them mapped where it already
//has pages in its page table
char buf[256*1024]={1};

struct glob
{
    glob() { shared_n++; }
} gg;

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
