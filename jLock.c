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
initPTELocks(ULONG_PTR virtualMemPages)
{

    ULONG_PTR numLocks;
    numLocks = virtualMemPages / PAGES_PER_LOCK;

    // 
    // Allocate a lock array corresponding to PTEs in pagetable
    //

    PTELockArray = VirtualAlloc(NULL, numLocks * sizeof(CRITICAL_SECTION), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (PTELockArray == NULL) {

        PRINT_ERROR("Unable to allocate for PTE Locks\n");
        exit(-1);

    }

    for (int i = 0; i <= numLocks; i++) {

        InitializeCriticalSection(&PTELockArray[i]);

    }

}


BOOLEAN
freePTELocks(PCRITICAL_SECTION LockArray, ULONG_PTR virtualMemPages)
{
    
    ULONG_PTR numLocks;
    BOOL bRes;

    if (PTELockArray == NULL) {

        PRINT_ERROR("Lock array is NULL - already freed\n");
        return FALSE;

    }

    numLocks = virtualMemPages / PAGES_PER_LOCK;

    for (int i = 0; i <= numLocks; i++) {

        DeleteCriticalSection(&PTELockArray[i]);

    }

    bRes = VirtualFree(LockArray, 0, MEM_RELEASE);

    if (bRes == FALSE) {
      
        PRINT_ERROR("Unable to free lock array\n");
        return FALSE;
  
    }
    
    return TRUE;
}


ULONG_PTR
getLockIndex(PPTE currPTE) 
{

    ULONG_PTR PTEIndex;

    PTEIndex = currPTE - PTEarray;

    return PTEIndex / PAGES_PER_LOCK;

}


VOID
acquirePTELock(PPTE currPTE) 
{

    ULONG_PTR lockIndex;

    lockIndex = getLockIndex(currPTE);

    EnterCriticalSection(&PTELockArray[lockIndex]);

    // EnterCriticalSection(&PTELock);

}


VOID
releasePTELock(PPTE currPTE) 
{

    ULONG_PTR lockIndex;

    lockIndex = getLockIndex(currPTE);
    LeaveCriticalSection(&PTELockArray[lockIndex]);
    
    // LeaveCriticalSection(&PTELock);

}


BOOLEAN
acquireOrHoldSubsequentPTELock(PPTE currPTE, PPTE prevPTE)
{


    ULONG_PTR currLockIndex;
    ULONG_PTR prevLockIndex;


    if (currPTE == NULL || prevPTE == NULL) {

        PRINT_ERROR("[acquireOrHoldSubsequentPTELock] Invalid PTE params\n");
        return FALSE;

    }

    currLockIndex = getLockIndex(currPTE);

    prevLockIndex = getLockIndex(prevPTE);

    //
    // If lock index remains constant, return true without switching
    // Otherwise, leave the prev critical section and enter the current
    // critical section
    //

    if (currLockIndex == prevLockIndex) {

        return TRUE;

    } else {

        LeaveCriticalSection(&PTELockArray[prevLockIndex]);
        EnterCriticalSection(&PTELockArray[currLockIndex]);
        return TRUE;

    }

}