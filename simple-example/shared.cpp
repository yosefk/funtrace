
volatile int glob;

void __attribute__((noinline)) shared_f(int n)
{
    glob = n;
}

void __attribute__((noinline)) shared_g(int a1, int a2, int a3, int a4, int a5, int a6)
{
    shared_f(a1+a2+a3+a4+a5+a6);
    shared_f(a1*a2*a3*a4*a5*a6);
}
