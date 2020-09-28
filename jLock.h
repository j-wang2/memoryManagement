#include <stdio.h>
#include <windows.h>


VOID acquireJLock(volatile PLONG lock);

VOID releaseJLock(volatile PLONG lock);

BOOL tryAcquireJLock(volatile PLONG lock);