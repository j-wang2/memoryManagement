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

        while (returnPFN->refCount != 0) {      // TODO - miscounts real available pages count

            enqueuePage(&zeroListHead, returnPFN);

            releaseJLock(&returnPFN->lockBits);

            returnPFN = dequeueLockedPage(&zeroListHead, TRUE);
            
            if (returnPFN == NULL) {

                PRINT("[getPage] zero list empty\n");
                return NULL;

            } 
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

        while (returnPFN->refCount != 0) {      // TODO - miscounts real available pages count

            enqueuePage(&freeListHead, returnPFN);

            releaseJLock(&returnPFN->lockBits);

            returnPFN = dequeueLockedPage(&freeListHead, TRUE);
            if (returnPFN == NULL) {
                PRINT("[getPage] free list empty\n");
                return NULL;
            } 
        }

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        return returnPFN;

    } else {

        return NULL;
        
    }
}


PPFNdata 
getStandbyPage(BOOLEAN returnLocked)
{

    PPFNdata returnPFN;
    PPTE currPTE;
    PTE oldPTE;
    PTE newPTE;
    ULONG_PTR pageNum;


    if (standbyListHead.count != 0) {

        //
        // dequeue a page from standby page (with lock bits set, 
        // since PTE lock acquisition could cause deadlock).
        // Since PTE lock is not acquired, other functions MUST
        // verify page has not changed state post acquisition of a 
        // transition PFn from trans state PTE.
        //

        returnPFN = dequeueLockedPageFromTail(&standbyListHead, TRUE);

        if (returnPFN == NULL) {
            PRINT("[getPage] standby list empty\n");
            return NULL;
        } 

        while (returnPFN->refCount != 0) {      // TODO - miscounts real available pages count

            enqueuePage(&standbyListHead, returnPFN);

            releaseJLock(&returnPFN->lockBits);

            returnPFN = dequeueLockedPageFromTail(&standbyListHead, TRUE);
            if (returnPFN == NULL) {
                PRINT("[getPage] standby list empty\n");
                return NULL;
            } 
        }

        //
        // Derive currPTE pointer (page lock, NOT PTE lock, is held)
        //

        currPTE = PTEarray + returnPFN->PTEindex;

        //
        // Create local copy of the currPTE to reference
        //

        oldPTE = *currPTE; 

        pageNum = returnPFN - PFNarray;

        ASSERT(oldPTE.u1.tPTE.transitionBit == 1 && oldPTE.u1.tPTE.PFN == pageNum);

        //
        //  Create newPTE initialized to zero (blank slate)
        //

        newPTE.u1.ulongPTE = 0;

        //
        // If standby page is not already in pagefile, it MUST be a zero page 
        // (i.e. faulted into active but never written, then trimmed to standby)
        // Therefore, the PTE can be set to demand zero 
        //

        if (returnPFN->pageFileOffset == INVALID_BITARRAY_INDEX) {

            // copy permissions to dz format PTE
            newPTE.u1.dzPTE.permissions = oldPTE.u1.tPTE.permissions;

            // put an invalid index into dz format PTE
            newPTE.u1.dzPTE.pageFileIndex = INVALID_BITARRAY_INDEX;

            returnPFN->statusBits = FREE;

            // copy newPTE back into currPTE
            writePTE(currPTE, newPTE);
        
            return returnPFN;

        }

        //
        // Copy old permissions to new pf format PTE
        //
        
        newPTE.u1.pfPTE.permissions = oldPTE.u1.tPTE.permissions;

        //
        // Put PF index into pf format PTE
        //

        newPTE.u1.pfPTE.pageFileIndex = returnPFN->pageFileOffset;

        returnPFN->statusBits = FREE;

        //
        // set PF offset to our "null" value in the PFN metadata
        //

        returnPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        //
        // Write newPTE back into currPTE
        //

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

    // Zero list
    returnPFN = getZeroPage(returnLocked);
    if (returnPFN != NULL) {

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