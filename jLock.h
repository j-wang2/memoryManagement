#include <stdio.h>
#include <windows.h>
#include "userMode-AWE-pageFile.h"

/*
 * acquireJLock: function to acquire a lock
 *  - spins until successful
 * 
 */
VOID
acquireJLock(volatile PLONG lock);

/*
 * releaseJLock: function to release lock
 * 
 */
VOID 
releaseJLock(volatile PLONG lock);

/*
 * tryAcquireJLock: function to try to acquire lock
 *  - returns false if currently held elsewhere
 * 
 */
BOOL 
tryAcquireJLock(volatile PLONG lock);


VOID
initPTELocks(ULONG_PTR numPages);


VOID
freePTELocks(PCRITICAL_SECTION LockArray, ULONG_PTR virtualMemPages);


ULONG_PTR
getLockIndex(PPTE currPTE);


VOID
acquirePTELock(PPTE currPTE);


VOID
releasePTELock(PPTE currPTE);


BOOLEAN
acquireOrHoldSubsequentPTELock(PPTE currPTE, PPTE prevPTE);
