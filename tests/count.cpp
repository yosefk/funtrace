#include <thread>
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

void loop()
{
    for(int i=0; i<1000; ++i) {
        h();
        h_shared();
    }
}

int main()
{
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
