#include "userMode-AWE-pageFile.h"

typedef struct _VADNode {
    LIST_ENTRY links;
    PVOID startVA;
    // PVOID endVA;
    ULONG64 numPages;
    ULONG64 permissions: PERMISSIONS_BITS;
    ULONG64 commitBit: 1;
    ULONG64 commitCount;
} VADNode, *PVADNode;


PVADNode
getVAD(void* virtualAddress);


VOID
enqueueVAD(PlistData listHead, PVADNode newNode);


VOID
dequeueSpecificVAD(PlistData listHead, PVADNode removeNode);


BOOLEAN
checkVADRange(void* startVA, ULONG_PTR size);


PVADNode
createVAD(void* startVA, ULONG_PTR size, PTEpermissions permissions, BOOLEAN isMemCommit);


BOOLEAN
deleteVAD(void* VA, ULONG_PTR size);
