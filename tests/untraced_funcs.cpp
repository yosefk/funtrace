#include "test.h"

volatile int n;

inline void NOFUNTRACE nop() {}

#define UNTRACED(name, callee) void NI NOFUNTRACE name() { n++; callee(); n++; }
#define TRACED(name, callee) void NI name() { n++; callee(); n++; }

UNTRACED(un1, nop);
TRACED(tr1, un1);
UNTRACED(un2, tr1);
TRACED(tr2, un2);

UNTRACED(un3, nop);
UNTRACED(un4, un3);
TRACED(tr3, un4);
TRACED(tr4, tr3);
UNTRACED(un5, tr4);
UNTRACED(un6, un5);

int main()
{
    scope_tracer tracer;

    tr2();
    un2();

    un6();
    tr4();
}
