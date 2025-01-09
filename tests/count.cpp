#include <thread>
#include <dlfcn.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "test.h"

//TODO: add threads and sos

volatile int n;

void NI f()
{
    n++;
}

void NI g()
{
    f();
    n++;
    f();
}

void NI h()
{
    g();
    n++;
    f();
}

void h_shared();
void (*h_shared_2)();

const int64_t iters = 1000;

void loop()
{
    for(int64_t i=0; i<iters; ++i) {
        h();
        h_shared();
        h_shared_2();
    }
}

int main()
{
    void* lib = dlopen(LIBS, RTLD_NOW);
    h_shared_2 = (void (*)())dlsym(lib, "h_dyn_shared_c");

    std::thread t([] {
        loop();
    });
    std::thread t2([] {
        loop();
    });
    loop();
    t.join();
    t2.join();
}
