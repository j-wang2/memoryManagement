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
dequeueSpecificVAD(PlistData listHead, PVADNode removeNode);


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


PVADNode
createVAD(void* startVA, ULONG_PTR size, PTEpermissions permissions, BOOLEAN isMemCommit);


BOOLEAN
deleteVAD(void* VA);
