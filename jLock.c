

#include <stdio.h>
#include <windows.h>


#define NUM_LOOPS 1000000000

#define NUM_HANDLES 4

volatile int counter;


VOID
acquireJLock(volatile PLONG lock)
{
    while (TRUE) {

        if (InterlockedCompareExchange(lock, 1, 0) == 0) {
            return;
        }
    }
}

VOID
releaseJLock(volatile PLONG lock)
{
    *lock = 0;
    return;
}

BOOL
tryAcquireJLock(volatile PLONG lock)
{
    if (InterlockedCompareExchange(lock, 1, 0) == 0) {
        return TRUE;
    } 
    else {
        return FALSE;
    }
}
