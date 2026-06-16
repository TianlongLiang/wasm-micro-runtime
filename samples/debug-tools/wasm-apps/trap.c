__attribute__((always_inline))
static inline int
trap_helper(int n)
{
    /* Forced-inline helper to demonstrate inline call stack expansion.
       This function gets inlined into c() even at -O0 due to always_inline. */
    return n + 100;
}

int
c(int n)
{
    int x = trap_helper(n);
    (void)x;
    __builtin_trap();
}

int
b(int n)
{
    n += 3;
    return c(n);
}

int
a(int n)
{
    return b(n);
}

int
main(int argc, char **argv)
{
    int i = 5;
    a(i);

    return 0;
}
