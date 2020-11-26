#ifndef VADNODES_H
#define VADNODES_H
#include "userMode-AWE-pageFile.h"

typedef struct _VADNode {
    LIST_ENTRY links;
    PVOID startVA;
    ULONG64 numPages;
    ULONG64 permissions: PERMISSIONS_BITS;
    ULONG64 commitBit: 1;
    ULONG64 deleteBit: 1;
    ULONG64 commitCount;
    // ULONG64 refCount;
    HANDLE faultEvent;
} VADNode, *PVADNode;


/*
 * getVAD: function to find and return VAD associated with a given virtual address
 *  - VAD list lock must be held prior to calling of function (responsibility of caller)
 * 
 * Returns PVADNode:
 *  - associated VAD if found successfully
 *  - NULL if VA does not correspond to a VAD in list
 */
PVADNode
getVAD(void* virtualAddress);


/*
 * enqueueVAD: function to enqueue a provided VAD to list
 *  - Both acquires and releases list lock 
 * 
 * No return value
 */
VOID
enqueueVAD(PlistData listHead, PVADNode newNode);


/*
 * dequeueSpecificVAD: function to dequeue a particular node from list
 *  - Both acquires and releases list lock
 * 
 * No return value
 */
VOID
dequeueSpecificVAD(PVADNode removeNode);


/*
 * checkVADRange: function to see whether a given startVA/size overlaps with
 * pre-existing VADs
 *  - Note: can be updated to utilize bitmap for faster search time
 * 
 * Returns BOOLEAN
 *  - TRUE if no overlap (VAD can then be allocated by createVAD)
 *  - FALSE if overlap
 */
BOOLEAN
checkVADRange(void* startVA, ULONG_PTR size);


/*
 * createVAD: function to create a new VAD
 *  - acquires "read" and "write" locks 
 *  - calls checkVAD range to verify that the range is clear
 * 
 * Returns PVADNode
 *  - New node if successful
 *  - NULL if unsuccessful
 */
PVADNode
createVAD(void* startVA, ULONG_PTR size, PTEpermissions permissions, BOOLEAN isMemCommit);


/*
 * deleteVAD: function to delete a new VAD
 *  - acquires "read " and "write" locks, setting a delete bit
 *    and releasing both before calling decommit
 *  - calls decommitVA (which in turn acquires "read" and PTE locks in that order)
 *  - Re-acquires locks before dequeing from list
 * 
 * Returns BOOLEAN
 *  - TRUE if successful
 *  - FALSE if unsuccessful
 */
BOOLEAN
deleteVAD(void* VA);


/*
 * checkVADCommit: function to compare VAD commit count with actual commited PTEs
 *  - called when VAD_COMMIT_CHECK is toggled on
 * 
 * No return val
 */
VOID 
checkVADCommit(PVADNode currVAD);


/*
 * decrementCommit: function to decrement both VAD and global commit charge
 *  - VAD "read" lock must be held by caller (since VAD is read)
 *  - VAD "write" lock is also acquired and released within this function
 *    in order to modify vad commit count
 *  - global count is decremented via interlocked synchro ( no need for locking)
 * 
 * No return val
 * 
 */
VOID
decrementCommit(PVADNode currVAD);


/*
 * decrementMultipleCommit: function to decrement both VAD and global commit charge
 *  - like decrementCommit, but will decrement commit by multiple rather than a single
 *    decrement
 *  - VAD "read" lock must be held by caller
 * 
 * No return val
 */
VOID
decrementMultipleCommit(PVADNode currVAD, ULONG_PTR numPages);

#endif