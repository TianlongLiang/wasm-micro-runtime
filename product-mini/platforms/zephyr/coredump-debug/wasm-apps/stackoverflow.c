/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* Recursive stack overflow for coredump debug demo.
   Call chain: app_main -> recurse_deep (x N until stack exhaustion) */

static int depth = 0;

int
recurse_deep(int n)
{
    depth++;
    /* Allocate some stack space each frame to accelerate overflow */
    volatile char buf[128];
    buf[0] = (char)n;
    buf[127] = (char)depth;
    return recurse_deep(n + 1) + buf[0] + buf[127];
}

void
app_main(void)
{
    recurse_deep(0);
}
