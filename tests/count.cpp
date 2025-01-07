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

int main()
{
    for(int i=0; i<1000; ++i) {
        h();
        h_shared();
    }
    std::thread t([] {
        for(int i=0; i<1000; ++i) {
            h();
            h_shared();
        }
    });
    //this loop runs concurrently with the code in the thread,
    //and the call counts from both threads should be accumulated correctly;
    //note that this might not have happened without the "warmup"
    //code running before the thread is spawned and calling all the relevant
    //functions - the counting is thread-safe but only once the count pages
    //were allocated, and the allocation is racy.
    for(int i=0; i<1000; ++i) {
        h();
        h_shared();
    }

    t.join();
}
