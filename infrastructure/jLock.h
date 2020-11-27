#include <stdio.h>
#include <windows.h>
#include "../usermodeMemoryManager.h"

/*
 * acquireJLock: function to acquire a lock
 *  - spins until successful
 * 
 * No return value
 */
VOID
acquireJLock(volatile PLONG lock);


/*
 * releaseJLock: function to release lock
 * 
 * No return value
 */
VOID 
releaseJLock(volatile PLONG lock);


/*
 * tryAcquireJLock: function to try to acquire lock
 * 
 * Returns BOOLEAN:
 *  - TRUE if successful
 *  - FALSE if currently held elsewhere
 */
BOOL 
tryAcquireJLock(volatile PLONG lock);


VOID
acquireJCritical(PCRITICAL_SECTION cs);


VOID
releaseJCritical(PCRITICAL_SECTION cs);


/*
 * initPTELocks: function to initialize PTE locks
 *  - VirtualAllocs and initializes virtual sections for assigns PTELockArray global
 *  - Array of critical sections
 *  - exits if unsuccessful
 * 
 * No return value
 */
VOID
initPTELocks(ULONG_PTR totalVirtualMemPages);


/*
 * freePTELocks: function to free PTE locks
 *  - deletes each critical section and VirtualFrees alloc'd memory
 * 
 * Returns BOOLEAN
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
freePTELocks(PCRITICAL_SECTION LockArray, ULONG_PTR totalVirtualMemPages);


/*
 * getLockIndex: function to get lock index for a given PTE
 * 
 * Returns ULONG_PTR
 *  - Lock index for
 */
ULONG_PTR
getLockIndex(PPTE currPTE);


/* 
 * acquirePTELock: function to acquire PTE lock corresponding to given PTE
 * 
 * No return value
 */
VOID
acquirePTELock(PPTE currPTE);


/*
 * acquirePTELock: function to release PTE lock corresponding to given PTE
 * 
 * No return value
 */
VOID
releasePTELock(PPTE currPTE);


/*
 * acquireOrHoldSubsequentPTELock: maintains overall PTE locking
 *  - If PTE lock index remains the same, do nothing
 *  - else, if switches, release old and acquire new
 * 
 * Returns BOOLEAN
 *  - TRUE either if lock index remains same or switch occurs successfully
 *  - FALSE if either param is null
 */
BOOLEAN
acquireOrHoldSubsequentPTELock(PPTE currPTE, PPTE prevPTE);
