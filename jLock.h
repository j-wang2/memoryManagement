#include <stdio.h>
#include <windows.h>

/*
 * acquireJLock: function to acquire a lock
 *  - spins until successful
 * 
 */
VOID acquireJLock(volatile PLONG lock);

/*
 * releaseJLock: function to release lock
 * 
 */
VOID releaseJLock(volatile PLONG lock);

/*
 * tryAcquireJLock: function to try to acquire lock
 *  - returns false if currently held elsewhere
 * 
 */
BOOL tryAcquireJLock(volatile PLONG lock);