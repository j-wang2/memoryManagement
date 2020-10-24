#include "userMode-AWE-pageFile.h"
#include "VApermissions.h"
#include "pageFault.h"
#include "pageFile.h"
#include "PTEpermissions.h"
#include "enqueue-dequeue.h"
#include "jLock.h"
// #include "VADNodes.c"


faultStatus 
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{

    // initialize PFstatus to success - only changes on pagefault return val
    faultStatus PFstatus;
    PFstatus = SUCCESS;

    while (PFstatus == SUCCESS || PFstatus == NO_AVAILABLE_PAGES || PFstatus == PAGE_STATE_CHANGE ) {
        
        #ifdef TEMP_TESTING

        //
        // For special pool/application verifier
        //

        PPTE currPTE;
        currPTE = getPTE(virtualAddress);

        PTE snapPTE;
        snapPTE = *currPTE;

        if (snapPTE.u1.hPTE.validBit == 1) {

            ULONG_PTR currPFNindex;

            currPFNindex = snapPTE.u1.hPTE.PFN;

            PPFNdata currPFN;
            currPFN = PFNarray + currPFNindex;

            acquireJLock(&currPFN->lockBits);


            if (snapPTE.u1.ulongPTE == currPTE->u1.ulongPTE) {

                PTEpermissions tempRWEpermissions = getPTEpermissions(snapPTE);

                if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

                    releaseJLock(&currPFN->lockBits);
                    PFstatus = pageFault(virtualAddress, RWEpermissions);
                    PRINT_ERROR("Invalid permissions\n");
                    return PFstatus;

                } 

                releaseJLock(&currPFN->lockBits);

                return SUCCESS;

            }
            else {

                releaseJLock(&currPFN->lockBits);

                continue;
                
            }

        } else {

            PFstatus = pageFault(virtualAddress, RWEpermissions);

        }


        #else

        _try {

            if (RWEpermissions == READ_ONLY || READ_EXECUTE) {

                *(volatile CHAR *)virtualAddress;                                       // read

            } else if (RWEpermissions == READ_WRITE || READ_WRITE_EXECUTE) {

                *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress;    // write

            } else {

                PRINT_ERROR("invalid permissions\n");    

            }

            break;

        } _except (EXCEPTION_EXECUTE_HANDLER) {

            PFstatus = pageFault(virtualAddress, RWEpermissions);

        }

        #endif

    }

    return PFstatus;

}


faultStatus
writeVA(PVOID virtualAddress, PVOID str) 
{

    faultStatus PFstatus;

    PFstatus = accessVA(virtualAddress, READ_WRITE);

    while (PFstatus == SUCCESS || PFstatus == NO_AVAILABLE_PAGES || PFstatus == PAGE_STATE_CHANGE ) {

        _try {

            //
            // Try to write parameter str to the VA
            //

            * (PVOID *) virtualAddress = str;
            
            break;

        } _except (EXCEPTION_EXECUTE_HANDLER) {

            PFstatus = accessVA(virtualAddress, READ_WRITE);

        }

    }

    return PFstatus;
    
}


faultStatus 
isVAaccessible (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{
    _try {
        if (RWEpermissions == READ_ONLY || RWEpermissions == READ_EXECUTE) {
            *(volatile CHAR *)virtualAddress;                                                      // read

        } else if (RWEpermissions == READ_WRITE || RWEpermissions == READ_WRITE_EXECUTE) {
            *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress;                   // write

        }  else {
            PRINT_ERROR("invalid permissions\n");
        }
        return SUCCESS;
    } _except (EXCEPTION_EXECUTE_HANDLER) {
        return ACCESS_VIOLATION;
    }
}


BOOLEAN
commitVA (PVOID startVA, PTEpermissions RWEpermissions, ULONG_PTR commitSize)
{

    PPTE startPTE;
    PVOID endVA;
    PPTE endPTE;
    PPTE currPTE;
    BOOLEAN lockHeld;
    // PVADNode currVAD;

    // inclusive (use <= as condition)
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    startPTE = getPTE(startVA);

    if (startPTE == NULL) {

        PRINT("[commitVA] Starting address does not correspond to a valid PTE\n");
        return FALSE;

    }


    endPTE = getPTE(endVA);

    if (endPTE == NULL) {

        PRINT("[commitVA] Ending address does not correspond to a valid PTE\n");
        return FALSE;

    }

    //
    // get a vad and check whether start address and size fit within a vad
    //
    
    // // todo - lock list and update getVAD routine
    // EnterCriticalSection(&VADListHead.lock);
 
    // currVAD = getVAD(startVA);

    // if (currVAD == NULL) {
    //     LeaveCriticalSection(&VADListHead.lock);

    //     PRINT("[commitVA] Requested commit startVA does not fall within a VAD\n");
    //     return FALSE;
    // }

    // //
    // // This should also check null endVA VAD (since NULL return val
    // // from getVAD(endVA) would not be equal to currVAD)
    // //

    // if (currVAD != getVAD(endVA)) {
    //     LeaveCriticalSection(&VADListHead.lock);
    //     PRINT("[commitVA] Requested commit does not fall within a single VAD\n");
    //     return FALSE;
    // }

    // if (currVAD->commitBit) {

    // }


    //
    // Acquire starting PTE lock and set lockHeld boolean flag to true
    //

    acquirePTELock(startPTE);

    lockHeld = TRUE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        //
        // Only acquire lock if the currentPTE is the first PTE OR
        // if current PTE's lock differs from the previous PTE's lock 
        //

        if (currPTE != startPTE) {

            lockHeld = acquireOrHoldSubsequentPTELock(currPTE, currPTE - 1);

        }
 
        if (lockHeld == FALSE) {

            PRINT_ERROR("[commitVA] unable to acquire lock - fatal error\n");
            return FALSE;

        }
        
        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;

        //
        // Check if valid/transition/demandzero bit is already set 
        // OR if the PTE is zero but the VAD is committed
        // (avoids double charging commit)
        //

        if (tempPTE.u1.hPTE.validBit == 1 
            || tempPTE.u1.tPTE.transitionBit == 1 
            || tempPTE.u1.pfPTE.permissions != NO_ACCESS
            // || (tempPTE.u1.ulongPTE == 0 && currVAD->commitBit)
            ) {

            PRINT("[commitVA] PTE is already valid, transition, or demand zero\n");
            continue;

        }

        //
        // if VAD is reserve and PTE is zero, do below code
        // OR if VAD is commit and PTE has decommitted bit set
        //


        //
        // Below code to synchronize the increment of totalCommittedPages
        //

        BOOLEAN bRes;
        bRes = commitPages(1);

        if (bRes == FALSE) {
            releasePTELock(currPTE);
            return FALSE;
            //
            // TODO what do i do? release PTE lock?
            //
        }
        else {
            PVOID currVA;

            //
            // commit with priviliges param (commits PTE)
            //

            tempPTE.u1.dzPTE.permissions = RWEpermissions;

            tempPTE.u1.dzPTE.pageFileIndex = INVALID_PAGEFILE_INDEX;

            currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );  // equiv to PTEindex*page_size
            PRINT("[commitVA] Committed PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, RWEpermissions);

        }

        //
        // Write out newPTE contents indivisibly
        //

        * (volatile PTE *) currPTE = tempPTE;
                            
    }

    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);


    return TRUE;

}


BOOLEAN
trimPTE(PPTE PTEaddress) 
{

    BOOLEAN wakeModifiedWriter;
    PTE oldPTE;
    ULONG_PTR pageNum;
    PPFNdata PFNtoTrim;
    PVOID currVA;

    if (PTEaddress == NULL) {
        PRINT_ERROR("could not trim - invalid PTE\n");
        return FALSE;
    }
    

    wakeModifiedWriter = FALSE;


    // acquire PTE lock
    acquirePTELock(PTEaddress);

    oldPTE = *PTEaddress;

    // check if PTE's valid bit is set - if not, can't be trimmed and return failure
    if (oldPTE.u1.hPTE.validBit == 0) {

        releasePTELock(PTEaddress);
        PRINT("could not trim - PTE is not valid\n");
        return FALSE;

    }

    // get pageNum
    pageNum = oldPTE.u1.hPTE.PFN;

    // get PFN
    PFNtoTrim = PFNarray + pageNum;


    ASSERT(PFNtoTrim->statusBits == ACTIVE);


    // zero new PTE
    PTE PTEtoTrim;
    PTEtoTrim.u1.ulongPTE = 0;


    currVA = (PVOID) ( (ULONG_PTR) leafVABlock + (PTEaddress - PTEarray) *PAGE_SIZE );
    
    // unmap page from VA (invalidates hardwarePTE)
    MapUserPhysicalPages(currVA, 1, NULL);


    // acquire page lock (prior to viewing/editing PFN fields)
    acquireJLock(&PFNtoTrim->lockBits);


    // if write in progress bit is set, modified writer re-enqueues page
    if (PFNtoTrim->writeInProgressBit == 1) {

        if (oldPTE.u1.hPTE.dirtyBit == 0) {
            PFNtoTrim->statusBits = STANDBY;
        }

        else if (oldPTE.u1.hPTE.dirtyBit == 1) {
            
            // notify modified writer that page has been re-modified since initial write began
            PFNtoTrim->remodifiedBit = 1;

            PFNtoTrim->statusBits = MODIFIED;

        }

    }
    else {

        // check dirtyBit to see if page has been modified
        if (oldPTE.u1.hPTE.dirtyBit == 0) {

            // add given VA's page to standby list
            enqueuePage(&standbyListHead, PFNtoTrim);

        } 
        else if (oldPTE.u1.hPTE.dirtyBit == 1) {

            // add given VA's page to modified list;
            wakeModifiedWriter = enqueuePage(&modifiedListHead, PFNtoTrim);

        }
    }

    releaseJLock(&PFNtoTrim->lockBits);


    // set transitionBit to 1
    PTEtoTrim.u1.tPTE.transitionBit = 1;  
    PTEtoTrim.u1.tPTE.PFN = pageNum;

    PTEtoTrim.u1.tPTE.permissions = getPTEpermissions(oldPTE);

    * (volatile PTE *) PTEaddress = PTEtoTrim;

    releasePTELock(PTEaddress);

    if (wakeModifiedWriter == TRUE) {

        BOOL bRes;
        bRes = SetEvent(wakeModifiedWriterHandle);

        if (bRes != TRUE) {
            PRINT_ERROR("[trimPTE] failed to set event\n");
        }
    }

    return TRUE;
}


BOOLEAN
trimVA(void* virtualAddress)
{

    PRINT("[trimVA] trimming page with VA %llu\n", (ULONG_PTR) virtualAddress);

    PPTE PTEaddress;
    PTEaddress = getPTE(virtualAddress);

    return trimPTE(PTEaddress);

}


BOOLEAN
protectVA(PVOID startVA, PTEpermissions newRWEpermissions, ULONG_PTR commitSize) {

    PPTE startPTE;
    PVOID endVA;
    PPTE endPTE;
    PPTE currPTE;
    BOOLEAN lockHeld;

    startPTE = getPTE(startVA);

    if (startPTE == NULL) {

        PRINT("[commitVA] Starting address does not correspond to a valid PTE\n");
        return FALSE;

    }

    // inclusive (use <= as condition)
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    endPTE = getPTE(endVA);

    if (endPTE == NULL) {

        PRINT("[commitVA] Ending address does not correspond to a valid PTE\n");
        return FALSE;
        
    }

    //
    // Acquire starting PTE lock and set lockHeld boolean flag to true
    //

    acquirePTELock(startPTE);

    lockHeld = TRUE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        //
        // Only acquire lock if the currentPTE is the first PTE OR
        // if current PTE's lock differs from the previous PTE's lock 
        //

        if (currPTE != startPTE) {
            
            lockHeld = acquireOrHoldSubsequentPTELock(currPTE, currPTE - 1);

        }

        if (lockHeld == FALSE) {

            PRINT_ERROR("[commitVA] unable to acquire lock - fatal error\n");
            return FALSE;

        }

        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;

        PTEpermissions oldPermissions;

        if (tempPTE.u1.hPTE.validBit == 1) {

            oldPermissions = getPTEpermissions(tempPTE);
            transferPTEpermissions(&tempPTE, newRWEpermissions);

        }
        else if ( tempPTE.u1.tPTE.transitionBit == 1 ) {

            oldPermissions = tempPTE.u1.tPTE.permissions;
            tempPTE.u1.tPTE.permissions = newRWEpermissions;

        }
        else if ( tempPTE.u1.pfPTE.permissions != NO_ACCESS) {      // handles both pfPTE and dzPTE formats (since are identical in format)
            
            oldPermissions = tempPTE.u1.pfPTE.permissions;
            tempPTE.u1.pfPTE.permissions = newRWEpermissions;

        }
        else {

            PRINT("[protectVA] PTE is not already valid, transition, or demand zero\n");
            continue;

        }


        PVOID currVA;
        currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );  // equiv to PTEindex*page_size

        PRINT("[protectVA] Updated permissions for PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, newRWEpermissions);

        * (volatile PTE *) currPTE = tempPTE;
                
    }
    
    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    return TRUE;

}


BOOLEAN
decommitVA (PVOID startVA, ULONG_PTR commitSize) {


    PPTE startPTE;
    PVOID endVA;
    PPTE endPTE;
    PPTE currPTE;
    PVOID currVA;
    BOOLEAN lockHeld;
    BOOLEAN reprocessPTE;
    
    startPTE = getPTE(startVA);

    reprocessPTE = FALSE;

    if (startPTE == NULL) {

        PRINT("[commitVA] Starting address does not correspond to a valid PTE\n");
        return FALSE;

    }

    // inclusive (use <= as condition)
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    endPTE = getPTE(endVA);

    if (endPTE == NULL) {

        PRINT("[commitVA] Ending address does not correspond to a valid PTE\n");
        return FALSE;
        
    }

    //
    // Acquire starting PTE lock and set lockHeld boolean flag to true
    //

    acquirePTELock(startPTE);

    lockHeld = TRUE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );  // equiv to PTEindex*page_size

        //
        // Only acquire lock if the currentPTE is the first PTE OR
        // if current PTE's lock differs from the previous PTE's lock 
        //

        if (currPTE != startPTE && reprocessPTE == FALSE) {
            
            lockHeld = acquireOrHoldSubsequentPTELock(currPTE, currPTE - 1);

        }

        if (lockHeld == FALSE) {

            PRINT_ERROR("[commitVA] unable to acquire lock - fatal error\n");
            return FALSE;

        }


        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;


        // check if PTE is already zeroed
        if (tempPTE.u1.ulongPTE == 0) {

            PRINT("VA is already decommitted\n");

            continue;

        }

        // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)

        else if (tempPTE.u1.hPTE.validBit == 1) {                       // valid/hardware format
        

            // get PFN
            PPFNdata currPFN;
            currPFN = PFNarray + tempPTE.u1.hPTE.PFN;

            // acquire PFN lock
            acquireJLock(&currPFN->lockBits);


            #ifdef TESTING_VERIFY_ADDRESSES

            if ((ULONG_PTR) currVA != * (ULONG_PTR*) currVA) {

                if (* (ULONG_PTR*) currVA == (ULONG_PTR)0) {

                    //
                    // This may occur if a thread attempts to decommit an address WHILE
                    // another thread is ACTIVELY WRITING IT (between pagefault and actual 
                    // writing out in writeVA)
                    //

                    PRINT("Another thread is currently between pagefault and write in writeVA function\n");

                } else {

                    PRINT_ERROR("decommitting (VA = 0x%llx) with contents 0x%llx\n", (ULONG_PTR) currVA, * (ULONG_PTR*) currVA);

                }    
                
            }
            
            #endif


            // PRINT_ALWAYS("decommitting (VA = 0x%llx) with contents 0x%llx\n", (ULONG_PTR) currVA, * (ULONG_PTR*) currVA);
            // PRINT("decommitting (VA = 0x%llx) with contents %s\n", (ULONG_PTR) currVA, * (PCHAR *) currVA);


            // unmap VA from page
            BOOL bResult;
            bResult = MapUserPhysicalPages(currVA, 1, NULL);

            if (bResult != TRUE) {

                PRINT_ERROR("[decommitVA] unable to decommit VA %llx - MapUserPhysical call failed\n", (ULONG_PTR) currVA);
            }


            if (currPFN->writeInProgressBit == 1) {

                currPFN->statusBits = AWAITING_FREE;
                
            }
            else {
                // if the PFN contents is also stored in pageFile
                if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
                    clearPFBitIndex(currPFN->pageFileOffset);
                }

                // enqueue Page to free list
                enqueuePage(&freeListHead, currPFN);
            }

            releaseJLock(&currPFN->lockBits);


        } 
        else if (tempPTE.u1.tPTE.transitionBit == 1) {                  // transition format

            // get PFN
            PPFNdata currPFN;
            currPFN = PFNarray + tempPTE.u1.tPTE.PFN;


            acquireJLock(&currPFN->lockBits);

            // Verify the PTE is still in transition format and pointed to by PTE index
            if ( tempPTE.u1.ulongPTE != currPTE->u1.ulongPTE
            || (currPFN->statusBits != STANDBY && currPFN->statusBits != MODIFIED)
            || currPFN->PTEindex != (ULONG64) (currPTE - PTEarray) ) {
                
                releaseJLock(&currPFN->lockBits);

                PRINT("[decommitVA] currPFN has changed from transition state\n");

                //
                // Reprocess same PTE since it has since been changed
                //

                reprocessPTE = TRUE;

                currPTE--;

                continue;

            }
            
            //
            // only dequeue if both read in progress and write in progress bits are zero
            //

            if (currPFN->writeInProgressBit == 1 || currPFN->readInProgressBit == 1) {

                currPFN->statusBits = AWAITING_FREE;

            }
            else { 
                // dequeue from standby/modified list
                dequeueSpecificPage(currPFN);

                // if the PFN contents are also stored in pageFile
                if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
                    clearPFBitIndex(currPFN->pageFileOffset);
                }

                // enqueue Page to free list (setting status bits in process)
                enqueuePage(&freeListHead, currPFN);
            }
           

            releaseJLock(&currPFN->lockBits);

        }  
        else if (tempPTE.u1.pfPTE.pageFileIndex != INVALID_PAGEFILE_INDEX) {     // pagefile format

            clearPFBitIndex(tempPTE.u1.pfPTE.pageFileIndex);
            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.dzPTE.permissions != NO_ACCESS) {                   // demand zero format

            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.dzPTE.decommitBit == 1) {

            releasePTELock(currPTE);
            PRINT_ERROR("[decommitVA] already decommitted\n");
            return TRUE;

        }
        else if (tempPTE.u1.ulongPTE == 0) {                            // zero PTE
            
            releasePTELock(currPTE);
            PRINT_ERROR("[decommitVA] already decommitted\n");
            return TRUE;

        }

        // decrement count of committed pages
        if (totalCommittedPages > 0)  {

            InterlockedDecrement(&totalCommittedPages);

        } else {

            releasePTELock(currPTE);
            PRINT_ERROR("[decommitVA] bookkeeping error - no committed pages\n");
            return FALSE;

        }

        // zero and write PTE out
        memset(&tempPTE, 0, sizeof(PTE));
        // tempPTE.u1.dzPTE.decommitBit = 1;

        * (volatile PTE *) currPTE = tempPTE;
                
    }

    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    return TRUE;

}


BOOLEAN
commitPages (ULONG_PTR numPages)
{

    LONG oldVal;
    LONG tempVal;

    oldVal = totalCommittedPages;

    while (TRUE) {

        ASSERT( oldVal <= totalMemoryPageLimit);

        if (oldVal == totalMemoryPageLimit) {

            // no remaining pages
            PRINT("[commitVA] All commit charge used (no remaining pages) - unable to commit PTE\n");
            return FALSE;

        }

        tempVal = InterlockedCompareExchange(&totalCommittedPages, oldVal + numPages, oldVal);

        //
        // Compare exchange successful
        //

        if (tempVal == oldVal) {

            return TRUE;

        } else {

            oldVal = tempVal;

        }
    }
}