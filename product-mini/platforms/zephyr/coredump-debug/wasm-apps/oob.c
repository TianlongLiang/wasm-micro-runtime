/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* Deliberate out-of-bounds memory access for coredump debug demo.
   Call chain: app_main -> trigger_oob -> do_bad_access */

void
do_bad_access(int offset)
{
    volatile int *p = (volatile int *)0;
    /* Write to an address way beyond linear memory to trigger OOB trap */
    p[offset] = 0xDEAD;
}

void
trigger_oob(void)
{
    /* 0x7FFFFFFF is well beyond any WASM linear memory */
    do_bad_access(0x7FFFFFFF);
}

void
app_main(void)
{
    trigger_oob();
}
