#include "userMode-AWE-pageFile.h"
#include "VADNodes.h"
#include "enqueue-dequeue.h"
#include "PTEpermissions.h"
#include "bitOps.h"
#include "VApermissions.h"




PVADNode
getVAD(void* virtualAddress)
{

    PVADNode currVAD;
    PPTE currPTE;
    PLIST_ENTRY currLinks;

    currPTE = getPTE(virtualAddress);

    for (currLinks = VADListHead.head.Flink; currLinks != &VADListHead.head; currLinks = currVAD->links.Flink) {

        currVAD = CONTAINING_RECORD(currLinks, VADNode, links);

        ASSERT(currVAD->startVA != NULL);
        ASSERT(currVAD->numPages != 0);

        PPTE currStartPTE;
        PPTE currEndPTE;

        currStartPTE = getPTE(currVAD->startVA);

        //
        // Derive inclusive endPTE from starting PTE as well
        // as the number of pages in the VAD
        //

        currEndPTE = currStartPTE + currVAD->numPages - 1;

        //
        // If the current PTE is greater than the startPTE and less than the 
        // end PTE of the VAD, return pointer to the VAD
        //

        if (currStartPTE <= currPTE && currPTE <= currEndPTE) {  // TODO

            return currVAD;
            
        }

    }

    PRINT(" not in VAD List\n");
    return NULL;

}


VOID
enqueueVAD(PlistData listHead, PVADNode newNode)
{

    // EnterCriticalSection(&listHead->lock);

    enqueue(&listHead->head, &newNode->links);

    // LeaveCriticalSection(&listHead->lock);


}


VOID
dequeueSpecificVAD(PlistData listHead, PVADNode removeNode)
{

    // EnterCriticalSection(&listHead->lock);

    dequeueSpecific(&removeNode->links);

    // LeaveCriticalSection(&listHead->lock);

}


BOOLEAN
checkVADRange(void* startVA, ULONG_PTR size)
{

    PVADNode currVAD;
    PVOID endVAInclusive;

    PPTE startPTE;
    PPTE endPTE;
    
    PPTE VADStartPTE;
    PPTE VADEndPTE;

    PLIST_ENTRY currLinks;


    if (size == 0) {
        PRINT_ERROR("Invalid size : 0\n");
        return FALSE;
    }

    endVAInclusive = (PVOID) ( (ULONG_PTR) startVA + size - 1);
    
    startPTE = getPTE(startVA);
    endPTE = getPTE(endVAInclusive);

    for (currLinks = VADListHead.head.Flink; currLinks != &VADListHead.head; currLinks = currVAD->links.Flink) {

        currVAD = CONTAINING_RECORD(currLinks, VADNode, links);

        ASSERT(currVAD->startVA != NULL);

        VADStartPTE = getPTE(currVAD->startVA);
        VADEndPTE = VADStartPTE + currVAD->numPages;

        // 
        // If a VAD's start or end PTE falls within proposed/requested 
        // start or end PTE, flunk it (return false)
        //

        if ( ( startPTE <= VADStartPTE && VADStartPTE <= endPTE )
            || (startPTE <= VADEndPTE && VADEndPTE <= endPTE) ) {
            
            return FALSE;
            
        }


    }

    return TRUE;

}


PVADNode
createVAD(void* startVA, ULONG_PTR numPages, PTEpermissions permissions, BOOLEAN isMemCommit)
{

    PVADNode newNode;
    PVOID endVA;
    PPTE startPTE;
    PPTE endPTE;
    ULONG_PTR numVADPages;

    if (isMemCommit > 1) {
        
        PRINT_ERROR("[commitVAD] invalid boolean value\n");
        return NULL;

    }

    newNode = malloc( sizeof(VADNode) );

    if (newNode == NULL) {

        PRINT("[commitVAD] Unable to create new vad node\n");
        return NULL;

    }

    EnterCriticalSection(&VADListHead.lock);

    if (isMemCommit) {

        BOOLEAN bRes;

        bRes = commitPages(numPages);

        if (bRes == FALSE) {

            LeaveCriticalSection(&VADListHead.lock);
            free(newNode);
            PRINT("[commitVAD] Unable to create new VAD node (insufficient commit charge)\n");
            return NULL;

        }
    }


    //
    // Check if startVA or endVA fall within any preexisting VADs (for specified address)
    //

    if (startVA != NULL) {

        if ( ! checkVADRange(startVA, numPages)) {

            LeaveCriticalSection(&VADListHead.lock);
            free(newNode);
            PRINT("[commitVAD] Unable to create new VAD node (address overlap)\n");
            return NULL;

        }

    }
    else {

        //
        // Get a free vad range and overwrite (NULL) startva accordingly
        //

        ULONG_PTR bitIndex;

        bitIndex = reserveBitRange(numPages, VADBitArray, virtualMemPages);

        if (bitIndex == INVALID_BITARRAY_INDEX) {
            
            LeaveCriticalSection(&VADListHead.lock);
            free(newNode);
            PRINT("[commitVAD] Unable to create new VAD node (address overlap)\n");
            return NULL;

        }

        startVA = (PVOID) ( (bitIndex << PAGE_SHIFT) + (ULONG_PTR) leafVABlock );

    }

    newNode->startVA = startVA;

    endVA = (PVOID) ( (ULONG_PTR) startVA + ( numPages << PAGE_SHIFT) - 1 );

    startPTE = getPTE(startVA);

    endPTE = getPTE(endVA);

    //
    // Calculate number of pages (NOT zero-indexed) "covered" by VAD
    //
    numVADPages = endPTE - startPTE + 1;

    newNode->numPages = numVADPages;

    // update permissions
    newNode->permissions = permissions;

    // set commitBit
    newNode->commitBit = (ULONG64) isMemCommit;

    // put into VAD node list
    enqueueVAD(&VADListHead, newNode);

    LeaveCriticalSection(&VADListHead.lock);

    return newNode;

}


BOOLEAN
deleteVAD(void* VA, ULONG_PTR size)
{

    PVADNode removeVAD;
    BOOLEAN bRes;

    //
    // Acquire VAD list lock
    //

    EnterCriticalSection(&VADListHead.lock);

    removeVAD = getVAD(VA);

    if (removeVAD == NULL) {

        LeaveCriticalSection(&VADListHead.lock);
        PRINT_ALWAYS("[deleteVAD] Provided VA does not correspond to any VAD\n");
        return FALSE;

    }

    //
    // Decommit virtual address range within VAD, starting from startVA field and
    // spanning number of pages specified by numPages field
    //

    bRes = decommitVA(removeVAD->startVA, (removeVAD->numPages) << PAGE_SHIFT);

    if (bRes == FALSE) {
        
        LeaveCriticalSection(&VADListHead.lock);
        PRINT_ERROR("[deleteVAD] Unable to delete VAD, could not decommitVAs\n");
        return FALSE;

    }

    dequeueSpecificVAD(&VADListHead, removeVAD);

    LeaveCriticalSection(&VADListHead.lock);

    free(removeVAD);

    return TRUE;

}