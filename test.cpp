#include "test.h"
#include "funtrace.h"
#include <thread>
#define NL __attribute__((noinline))

volatile char x[64*3];

NL void f(int index)
{
    inlined();
    x[index] = 5;
}

void g(int i)
{
    f(i);
}

void h(int i) {
    g(i);
    g(i);
}
volatile int done = 0;

void shared_g(int a1, int a2, int a3, int a4, int a5, int a6);

int main()
{
    int iter = 0;
//    funtrace_save_maps();
    std::thread t([]{
            while(!done) {
                h(64);
            }
    });
    while(1) {
        g(128);
        shared_g(1,2,3,4,5,6);
        iter++;
        if(iter == 100000) {
            funtrace_pause_and_write_current_snapshot();
        }
        if(iter == 200000) {
            funtrace_pause_and_write_current_snapshot();
            break;
        }
    }
    done = 1;
    t.join();
}
