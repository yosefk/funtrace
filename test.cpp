#include "test.h"
#include "funtrace.h"
#include <thread>
#define NL __attribute__((noinline))

volatile int x;

NL void f()
{
    inlined();
    x = 5;
}

void g()
{
    f();
}

void h() {
    g();
    g();
}
volatile int done = 0;
int main()
{
    int iter = 0;
//    funtrace_save_maps();
    std::thread t([]{
            while(!done) {
                h();
            }
    });
    while(1) {
        g();
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
