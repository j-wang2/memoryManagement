#include <stdio.h>
#include <windows.h>
#include "jlock.h"
#include "userMode-AWE-pageFile.h"

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

VOID
acquirePTELock(PPTE currPTE) {

    EnterCriticalSection(&PTELock);
}

VOID
releasePTELock(PPTE currPTE) {

    LeaveCriticalSection(&PTELock);

}