#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
#include "jLock.h"
#include "PTEpermissions.h"

PPFNdata
getZeroPage(BOOLEAN returnLocked)
{
    PPFNdata returnPFN;
    if (zeroListHead.count != 0) {

        // get a locked page from the zero list head
        returnPFN = dequeueLockedPage(&zeroListHead, returnLocked);

        if (returnPFN == NULL) {
            PRINT("[getPage] zero list empty\n");
            return NULL;
        }

        returnPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        return returnPFN;        

    } else {

        return NULL;

    }
}


PPFNdata 
getFreePage(BOOLEAN returnLocked)
{

    PPFNdata returnPFN;

    if (freeListHead.count != 0) {

        returnPFN = dequeueLockedPage(&freeListHead, returnLocked);

        if (returnPFN == NULL) {
            PRINT("[getPage] free list empty\n");
            return NULL;
        }

        return returnPFN;

    } else {

        return NULL;
        
    }
}


PPFNdata 
getStandbyPage(BOOLEAN returnLocked)
{

    PPFNdata returnPFN;

    if (standbyListHead.count != 0) {

        //
        // dequeue a page from standby page (with lock bits set, since PTE lock acquisition could cause deadlock)
        //

        returnPFN = dequeueLockedPageFromTail(&standbyListHead, TRUE);

        if (returnPFN == NULL) {
            PRINT("[getPage] standby list empty\n");
            return NULL;
        }

        // get PTE
        PPTE currPTE;
        currPTE = PTEarray + returnPFN->PTEindex;

        // create copy of the currPTE to reference
        PTE oldPTE;
        oldPTE = *currPTE;

        //  create newPTE initialized to zero (blank slate)
        PTE newPTE;
        newPTE.u1.ulongPTE = 0;

        // if page is not already in pagefile, it MUST be a zero page 
        // (i.e. faulted into active but never written, then trimmed to standby)
        // Therefore, the PTE can be set to demand zero 
        if (returnPFN->pageFileOffset == INVALID_BITARRAY_INDEX) {

            // copy permissions to dz format PTE
            newPTE.u1.dzPTE.permissions = oldPTE.u1.tPTE.permissions;

            // put PF index into dz format PTE
            newPTE.u1.dzPTE.pageFileIndex = INVALID_BITARRAY_INDEX;

            returnPFN->statusBits = FREE;

            // copy newPTE back into currPTE
            writePTE(currPTE, newPTE);
        
            return returnPFN;

        }

        // copy permissions to pf format PTE
        newPTE.u1.pfPTE.permissions = oldPTE.u1.tPTE.permissions;

        // put PF index into pf format PTE
        newPTE.u1.pfPTE.pageFileIndex = returnPFN->pageFileOffset;

        returnPFN->statusBits = FREE;

        // copy newPTE back into currPTE
        writePTE(currPTE, newPTE);
       
        //
        // release PFN lock once PTE is written out (if returnLocked is FALSE)
        //

        if (returnLocked == FALSE) {
    
            releaseJLock(&returnPFN->lockBits);

        }
    
        return returnPFN;

    } else {

        return NULL;

    }
} 


PPFNdata
getPage(BOOLEAN returnLocked)
{

    PPFNdata returnPFN;

    #ifdef CHECK_PAGEFILE                               // to check standby -> pf repurposing
    // standby list
    returnPFN = getStandbyPage(returnLocked);
    if (returnPFN != NULL) {
        ULONG_PTR PFN;
        PFN = returnPFN - PFNarray;

        // zeroPage (does not update status bits in PFN metadata)
        zeroPage(PFN);

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        PRINT("[getPage] Allocated PFN from standby list\n");

        return returnPFN;   
    }
    #endif


    // Zero list
    returnPFN = getZeroPage(returnLocked);
    if (returnPFN != NULL) {

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        PRINT("[getPage] Allocated PFN from zero list\n");

        return returnPFN;

    }


    // free list
    returnPFN = getFreePage(returnLocked);
    if (returnPFN != NULL) {

        ULONG_PTR PFN;
        PFN = returnPFN - PFNarray;

        // zeroPage (does not update status bits in PFN metadata)
        zeroPage(PFN);

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        PRINT("[getPage] Allocated PFN from free list\n");

        return returnPFN;
    }


    // standby list
    returnPFN = getStandbyPage(returnLocked);
    if (returnPFN != NULL) {
        ULONG_PTR PFN;
        PFN = returnPFN - PFNarray;

        // zeroPage (does not update status bits in PFN metadata)
        zeroPage(PFN);

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        PRINT("[getPage] Allocated PFN from standby list\n");

        return returnPFN;   
    }


    PRINT("[getPage] All lists empty - unable to get page\n");
    
    return returnPFN;                                           // should be NULL

}


PPFNdata
getPageAlways(BOOLEAN returnLocked) 
{
    PPFNdata freedPFN;

    while (TRUE) {

        // dequeue and return a LOCKED page (PFN lock must be released at a later point)
        freedPFN = getPage(returnLocked);

        if (freedPFN == NULL) {

            HANDLE pageEventHandles[] = {zeroListHead.newPagesEvent, freeListHead.newPagesEvent, standbyListHead.newPagesEvent};

            WaitForMultipleObjects(STANDBY + 1, pageEventHandles, FALSE, INFINITE);

            continue;

        } else {

            //
            // PFN is valid: break out of while-loop and return
            //

            break;

        }
    }
    return freedPFN;
}