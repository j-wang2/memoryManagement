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
        
        PPTE currStartPTE;
        PPTE currEndPTE;

        currVAD = CONTAINING_RECORD(currLinks, VADNode, links);

        ASSERT(currVAD->startVA != NULL);

        ASSERT(currVAD->numPages != 0);

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

        if (currStartPTE <= currPTE && currPTE <= currEndPTE) {

            return currVAD;
            
        }

    }

    PRINT(" not in VAD List\n");
    return NULL;

}


VOID
enqueueVAD(PlistData listHead, PVADNode newNode)
{

    enqueue(&listHead->head, &newNode->links);

}


VOID
dequeueSpecificVAD(PVADNode removeNode)
{

    dequeueSpecific(&removeNode->links);

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

    //
    // Calculate inclusive endVA from startVA and size
    //

    endVAInclusive = (PVOID) ( (ULONG_PTR) startVA + size - 1);
    
    startPTE = getPTE(startVA);

    endPTE = getPTE(endVAInclusive);

    for (currLinks = VADListHead.head.Flink; currLinks != &VADListHead.head; currLinks = currVAD->links.Flink) {

        currVAD = CONTAINING_RECORD(currLinks, VADNode, links);

        ASSERT(currVAD->startVA != NULL);

        //
        // Derive starting and ending PTEs within current VAD itself
        //

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

    if (numPages == 0) {

        PRINT("No pages allocated\n");
        return NULL;

    }

    if (isMemCommit > 1) {
        
        PRINT_ERROR("[commitVAD] invalid boolean value\n");
        return NULL;

    }

    newNode = malloc( sizeof(VADNode) );

    if (newNode == NULL) {

        PRINT("[commitVAD] Unable to create new vad node\n");
        return NULL;

    }

    //
    // Acquire both VAD locks in order to delete a VAD
    // (Read lock first, then write lock) 
    //

    EnterCriticalSection(&VADListHead.lock);

    EnterCriticalSection(&VADWriteLock);

    if (isMemCommit) {

        BOOLEAN bRes;

        bRes = commitPages(numPages);

        if (bRes == FALSE) {

            LeaveCriticalSection(&VADWriteLock);

            LeaveCriticalSection(&VADListHead.lock);

            free(newNode);

            PRINT("[commitVAD] Unable to create new VAD node (insufficient commit charge)\n");

            return NULL;

        }

        //
        // Update node's commit count to the number of pages initially allocated
        //

        newNode->commitCount = numPages;

    } else {

        //
        // For a reserve vad, initialize commit count to zero
        //

        newNode->commitCount = 0;

    }


    //
    // Check if startVA or endVA fall within any preexisting VADs (for specified address)
    //

    if (startVA != NULL) {

        if ( ! checkVADRange(startVA, numPages)) {

            LeaveCriticalSection(&VADWriteLock);

            LeaveCriticalSection(&VADListHead.lock);

            free(newNode);

            decommitPages(numPages);

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

            LeaveCriticalSection(&VADWriteLock);

            LeaveCriticalSection(&VADListHead.lock);

            free(newNode);

            decommitPages(numPages);

            PRINT("[commitVAD] Unable to create new VAD node (insufficient space in VM range)\n");

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

    //
    // Set permissions to permissions param
    //

    newNode->permissions = permissions;

    //
    // Set commitBit to parameter isMemCommit value
    //

    newNode->commitBit = (ULONG64) isMemCommit;

    //
    // Clear deleteBit (set by deleteVAD)
    //

    newNode->deleteBit = 0;

    //
    // Enqueue new VAD into VAD node list
    //

    enqueueVAD(&VADListHead, newNode);

    //
    // Exit critical sections in inverse order of acquisition
    // (release write lock first, then release list lock)
    //

    LeaveCriticalSection(&VADWriteLock);

    LeaveCriticalSection(&VADListHead.lock);

    return newNode;

}


BOOLEAN
deleteVAD(void* VA)
{

    PVADNode removeVAD;
    BOOLEAN bRes;
    ULONG_PTR bitIndex;

    //
    // Acquire both VAD locks in order to delete a VAD
    // (Read lock first, then write lock) 
    //

    EnterCriticalSection(&VADListHead.lock);

    EnterCriticalSection(&VADWriteLock);

    removeVAD = getVAD(VA);

    if (removeVAD == NULL || removeVAD->deleteBit == 1) {

        LeaveCriticalSection(&VADWriteLock);

        LeaveCriticalSection(&VADListHead.lock);

        PRINT("[deleteVAD] Provided VA does not correspond to any VAD or is already being deleted\n");

        return FALSE;

    }

    //
    // Set deleteBit to signify to prospective faulters that the VAD is currently
    // being deleted and to reject a pagefault accordingly.
    //

    removeVAD->deleteBit = 1;

    //
    // Release locks (in inverse order) prior to decommit (since decommit call
    // acquires read lock and then PTE lock, which could cause AB/BA deadlock 
    // with a competing VAD fault thread)
    //

    LeaveCriticalSection(&VADWriteLock);

    LeaveCriticalSection(&VADListHead.lock);

    //
    // Decommit virtual address range within VAD, starting from startVA field and
    // spanning number of pages specified by numPages field
    //

    bRes = decommitVA(removeVAD->startVA, (removeVAD->numPages) << PAGE_SHIFT);

    if (bRes == FALSE) {

        PRINT_ERROR("[deleteVAD] Unable to delete VAD, could not decommitVAs\n");

        return FALSE;

    }

    // 
    // Re-acquire locks in read-write order bbefore removing VAD from list
    //
    
    EnterCriticalSection(&VADListHead.lock);

    EnterCriticalSection(&VADWriteLock);

    //
    // Remove VAD from list and release VAD lock before
    // freeing struct
    //

    dequeueSpecificVAD(removeVAD);

    //
    // If VAD is commit, assert that there are no remaining committed pages within it
    //

    if (removeVAD->commitBit == 0) {
        
        ASSERT(removeVAD->commitCount == 0);    // todo

    }


    // ULONG_PTR numDecommitted;        // todo

    // numDecommitted = checkDecommitted(FALSE, getPTE(removeVAD->startVA), getPTE((ULONG_PTR)removeVAD->startVA + (removeVAD->numPages << PAGE_SHIFT) - 1));

    // ASSERT(numDecommitted == removeVAD->numPages);

    //
    // Exit critical sections in inverse order of acquisition
    // (release write lock first, then release list lock)
    //

    LeaveCriticalSection(&VADWriteLock);

    LeaveCriticalSection(&VADListHead.lock);

    //
    // Calculate starting bitindex to clear from PF bitarray
    //

    bitIndex = ((ULONG_PTR) removeVAD->startVA - (ULONG_PTR) leafVABlock) >> PAGE_SHIFT;

    setBitRange(FALSE, bitIndex, removeVAD->numPages, VADBitArray );

    //
    // Free VAD struct
    //

    free(removeVAD);

    return TRUE;

}



VOID 
checkVADCommit(PVADNode currVAD)
{

    PVOID startVA;
    PVOID endVA;
    PPTE startPTE;
    PPTE endPTE;
    ULONG_PTR commitSizeInBytes;
    ULONG_PTR numCommitted;
    ULONG_PTR numDecommitted;

    startVA = currVAD->startVA;

    commitSizeInBytes = currVAD->numPages << PAGE_SHIFT;

    endVA = (PVOID) ((ULONG_PTR) startVA + commitSizeInBytes - 1);

    startPTE = getPTE(startVA);

    if (startPTE == NULL) {

        PRINT("[commitVA] Starting address does not correspond to a valid PTE\n");
        return;

    }

    endPTE = getPTE(endVA);

    if (endPTE == NULL) {

        PRINT("[commitVA] Ending address does not correspond to a valid PTE\n");
        
        return;

    }

#if 1

    //
    // VAD "read" lock MUST be held by caller so currVAD can be passed and read 
    // accurately as a parameter to checkDecommitted
    //

    numDecommitted = checkDecommitted(currVAD, startPTE, endPTE);
#else
    numDecommitted = checkDecommitted( (BOOLEAN)currVAD->commitBit, startPTE, endPTE);
#endif


    numCommitted = currVAD->numPages - numDecommitted;

    ASSERT(numCommitted == currVAD->commitCount);

}


VOID
decrementCommit(PVADNode currVAD)
{
    
    //
    // Acquire and release VAD write lock to decrement commit count
    //

    EnterCriticalSection(&VADWriteLock);

    ASSERT(currVAD->commitCount != 0);

    currVAD->commitCount--;

    LeaveCriticalSection(&VADWriteLock);

    #ifdef VAD_COMMIT_CHECK

        //
        // VAD read lock must remain held at this point so that 
        // currVAD can be read accurately by callee commit
        // checks
        //
    
        checkVADCommit(currVAD);

    #endif
    
    //
    // Decrement global committed page count regardless of whether VAD is 
    // commit or reserve
    //

    ASSERT(totalCommittedPages != 0);

    InterlockedDecrement64(&totalCommittedPages);

}