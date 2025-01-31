#include "test.h"

volatile int n;

void NI short_function()
{
    n++;
}

void NI short_but_whitelisted()
{
    n++;
}

void NI long_enough_function()
{
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
    n++;
    short_function();
}

void NI long_but_blacklisted()
{
    short_function();
    n++;
    short_function();
    n++;
    short_function();
}

void NI short_with_loop()
{
    while(!n);
}

int main()
{
    scope_tracer tracer;

    short_function();
    short_but_whitelisted();
    long_enough_function();
    long_but_blacklisted();
    short_with_loop();
}
