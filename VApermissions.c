#include "userMode-AWE-pageFile.h"
#include "VApermissions.h"
#include "pageFault.h"
#include "pageFile.h"
#include "PTEpermissions.h"
#include "enqueue-dequeue.h"
#include "jLock.h"
#include "VADNodes.h"


faultStatus 
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{

    // initialize PFstatus to success - only changes on pagefault return val
    faultStatus PFstatus;
    PFstatus = SUCCESS;

    // todo - cannot get page_state_change once fixed in pf handler

    while (PFstatus == SUCCESS || PFstatus == PAGE_STATE_CHANGE ) {
        
        #ifdef AV_TEMP_TESTING

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

    // todo - cannot get page_state_change once fixed in pf handler

    while (PFstatus == SUCCESS || PFstatus == PAGE_STATE_CHANGE ) {

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
    PPTE VADStartPTE;
    BOOLEAN lockHeld;
    PVADNode currVAD;
    ULONG_PTR numPages;
    BOOLEAN bRes;
    BOOLEAN returnCommitPages;
    PVOID currVA;
    BOOLEAN reprocessPTE;

    reprocessPTE = FALSE;

    //
    // Calculate endVA from startVA and commit size, with -1 for inclusivity
    // (use <= as condition). This is also consistent with calculation
    // in decommitVA and VAD create/delete
    //

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
    // Calculate number of pages spanned by commit
    // via PTE difference + 1 (since the same PTE would be one page)
    //

    numPages = endPTE - startPTE + 1;

    //
    // Get a vad from starting address and check whether both
    // start address and end address fit within the singular VAD
    //
    
    EnterCriticalSection(&VADListHead.lock);
 
    currVAD = getVAD(startVA);

    if (currVAD == NULL) {

        LeaveCriticalSection(&VADListHead.lock);
        PRINT("[commitVA] Requested commit startVA does not fall within a VAD\n");
        return FALSE;

    }


    //
    // Verify requested commit falls within a single VAD.
    // requested number of pages must be greater than the number of the pages in teh vad
    //

    VADStartPTE = getPTE(currVAD->startVA);

    ULONG_PTR commitNum;

    commitNum = currVAD->numPages - (startPTE - VADStartPTE);

    if (commitNum < numPages) {

        LeaveCriticalSection(&VADListHead.lock);
        PRINT("[commitVA] Requested commit does not fall within a single VAD\n");
        return FALSE;

    }


    if (currVAD->commitBit == 0) {

        //
        // If the VAD is reserve, try and commit all pages up front
        //  - if initial charge is successful, return commit charge during walk
        //  - if initial charge is unsuccessful, check how many are already 
        //    committed and try to commit only the uncommitted PTEs instead
        //

        ULONG_PTR currDecommitted;

        bRes = commitPages(numPages);

        //
        // If pages are not committed successfully upfront, walk PTEs
        // to see how many are already committed
        //

        if (bRes == FALSE) {

            currDecommitted = checkDecommitted(FALSE, startPTE, endPTE);

            bRes = commitPages(currDecommitted);

            if (bRes == FALSE) {

                LeaveCriticalSection(&VADListHead.lock);
                PRINT("[commitVA] Insufficient commit charge\n");
                return FALSE;

            }

            returnCommitPages = FALSE;

        } else {

            returnCommitPages = TRUE;

        }

    }
    else {

        //
        // If the VAD is commit, find exact charge of decommitted pages
        // and only commit those
        //

        ULONG_PTR currDecommitted;

        currDecommitted = checkDecommitted(TRUE, startPTE, endPTE);

        bRes = commitPages(currDecommitted);

        if (bRes == FALSE) {

            LeaveCriticalSection(&VADListHead.lock);

            PRINT("[commitVA] Insufficient commit charge\n");
            
            return FALSE;
            
        }

        returnCommitPages = FALSE;

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

        if (currPTE != startPTE && reprocessPTE == FALSE) {

            lockHeld = acquireOrHoldSubsequentPTELock(currPTE, currPTE - 1);

        } else {

            reprocessPTE = FALSE;
            
        }
 
        ASSERT(lockHeld == TRUE);
        
        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;

        //
        // Check whether PTE has previously been committed
        // (i.e. whether valid/transition/demandzero bit is already set 
        // OR if the PTE is zero but the VAD is committed
        // (avoids double charging commit)
        //

        if (tempPTE.u1.hPTE.validBit == 1 
            || tempPTE.u1.tPTE.transitionBit == 1 
            || tempPTE.u1.pfPTE.permissions != NO_ACCESS
            || (tempPTE.u1.ulongPTE == 0 && currVAD->commitBit)
            ) {

            //
            // Overwrite old PTE permissions
            //

            if (tempPTE.u1.hPTE.validBit == 1) {

                transferPTEpermissions(&tempPTE, RWEpermissions);

            } 
            else if (tempPTE.u1.tPTE.transitionBit == 1) {

                PPFNdata transPFN;
                transPFN = PFNarray + tempPTE.u1.tPTE.PFN;

                //
                // For a transition PTE, page lock  must be acquired in
                // order to verify PTE has not changed state. If PTE is transition,
                // writing of PTE occurss within this if-statement, rather
                // than falling through in order to release locks
                //

                acquireJLock(&transPFN->lockBits);

                // Verify the PTE is still in transition format and pointed to by PTE index
                if ( tempPTE.u1.ulongPTE != currPTE->u1.ulongPTE
                || (transPFN->statusBits != STANDBY && transPFN->statusBits != MODIFIED)
                || transPFN->PTEindex != (ULONG64) (currPTE - PTEarray) ) {
                    
                    releaseJLock(&transPFN->lockBits);

                    PRINT("[commitVA] currPFN has changed from transition state\n");

                    //
                    // Reprocess same PTE since it has since been changed
                    //

                    reprocessPTE = TRUE;

                    currPTE--;

                    continue;

                }


                tempPTE.u1.tPTE.permissions = RWEpermissions;

                writePTE(currPTE, tempPTE);

                releaseJLock(&transPFN->lockBits);

                //
                // PTE has previously been committed, VAD is reserve,
                // AND returnCommittedPages is TRUE, return charge
                // (Must occur within if statement since trans PTE
                // does not "fall through").
                //

                if (currVAD->commitBit == 0 && returnCommitPages) {
                    
                    //
                    // "Return" (decrement) count of committed pages
                    //

                    ASSERT(totalCommittedPages > 0);

                    InterlockedDecrement64(&totalCommittedPages);

                }

                continue;


            } 
            else if (tempPTE.u1.pfPTE.permissions != NO_ACCESS) {             // covers both pagefile and demand zero cases

                tempPTE.u1.pfPTE.permissions = RWEpermissions;

            } 
            else if (tempPTE.u1.ulongPTE == 0 && currVAD->commitBit) {

                tempPTE.u1.dzPTE.pageFileIndex = INVALID_BITARRAY_INDEX;
                tempPTE.u1.dzPTE.permissions = RWEpermissions;

            } else {

                PRINT_ERROR("unrecognized state\n");

            }

            //
            // PTE has previously been committed, VAD is reserve,
            // AND returnCommittedPages is TRUE, return charge
            //

            if (currVAD->commitBit == 0 && returnCommitPages) {
                
                //
                // "Return" (decrement) count of committed pages
                //

                ASSERT(totalCommittedPages > 0);

                InterlockedDecrement64(&totalCommittedPages);

            }

            writePTE(currPTE, tempPTE);
            
            PRINT("[commitVA] PTE is already valid, transition, or demand zero\n");
            continue;

        }

        //
        // If VAD is commit but PTE has previously been decommitted, 
        // OR if VAD is reserve, commit PTE (commit charge has already been bumpeds)
        //
        if (currVAD->commitBit) {

            ASSERT( tempPTE.u1.dzPTE.decommitBit == 1 );

        }

        //
        // Commit PTE/page with priviliges param, clearing decommit bit and setting pfIndex to invalid value
        //

        tempPTE.u1.dzPTE.permissions = RWEpermissions;

        tempPTE.u1.dzPTE.decommitBit = 0;

        tempPTE.u1.dzPTE.pageFileIndex = INVALID_BITARRAY_INDEX;

        currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );                       // equiv to PTEindex*page_size
        PRINT("[commitVA] Committed PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, RWEpermissions);
        

        //
        // Write out newPTE contents indivisibly
        //

        writePTE(currPTE, tempPTE);        
                            
    }

    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    LeaveCriticalSection(&VADListHead.lock);

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

    //
    // Acquire PTE lock - although it may also be acquired in trimming function in 
    // main, the recursive nature of underlying CRITICAL_SECTION locking functionality
    // permits the re-acquisition of a critical section by the owning thread
    //

    acquirePTELock(PTEaddress);

    oldPTE = *PTEaddress;

    //
    // Check if PTE's valid bit is set - if not, can't be trimmed and return failure
    //
    
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
    PTE newPTE;
    newPTE.u1.ulongPTE = 0;


    currVA = (PVOID) ( (ULONG_PTR) leafVABlock + (PTEaddress - PTEarray) *PAGE_SIZE );
    
    // unmap page from VA (invalidates hardwarePTE)
    MapUserPhysicalPages(currVA, 1, NULL);


    // acquire page lock (prior to viewing/editing PFN fields)
    acquireJLock(&PFNtoTrim->lockBits);

    //
    // If write in progress bit is set, set the page status bits to 
    // signify the modified writer to re-enqueue page. If dirty bit is 
    // clear, PFN can be re-enqueued to standby. If it is set, PFN has been
    // remodified and thus remodified bit must be set in addition to 
    // setting status bits to modified.
    //

    if (PFNtoTrim->writeInProgressBit == 1 || PFNtoTrim->readInProgressBit == 1) {       // TODO - must be updated to include read in progress/refcount info

        if (oldPTE.u1.hPTE.dirtyBit == 0) {

            PFNtoTrim->statusBits = STANDBY;

        }
        else {
            
            ASSERT(oldPTE.u1.hPTE.dirtyBit == 1);

            // notify modified writer that page has been re-modified since initial write began
            PFNtoTrim->remodifiedBit = 1;

            PFNtoTrim->statusBits = MODIFIED;

        }

    }
    else {

        // check dirtyBit to see if page has been modified
        if (oldPTE.u1.hPTE.dirtyBit == 0 && PFNtoTrim->remodifiedBit == 0) {

            // add given VA's page to standby list
            enqueuePage(&standbyListHead, PFNtoTrim);

        } 
        else {

            if (PFNtoTrim->pageFileOffset != INVALID_BITARRAY_INDEX) {

                ASSERT(PFNtoTrim->remodifiedBit == 1);

                clearPFBitIndex(PFNtoTrim->pageFileOffset);

                PFNtoTrim->pageFileOffset = INVALID_BITARRAY_INDEX;
                
            }

            //
            // Since PTE dirty bit is set, we can also clear PFN remodified bit and 
            // enqueue to modified list
            //

            PFNtoTrim->remodifiedBit = 0;

            // add given VA's page to modified list;
            wakeModifiedWriter = enqueuePage(&modifiedListHead, PFNtoTrim);

        }
    }

    //
    // Set PTE transitionBit to 1, assign PFN and permissions,
    // and write out
    //

    newPTE.u1.tPTE.transitionBit = 1;  

    newPTE.u1.tPTE.PFN = pageNum;

    newPTE.u1.tPTE.permissions = getPTEpermissions(oldPTE);

    writePTE(PTEaddress, newPTE);

    //
    // Release PFN and PTE lock in order of acquisition
    //

    releaseJLock(&PFNtoTrim->lockBits);

    releasePTELock(PTEaddress);

    if (wakeModifiedWriter == TRUE) {

        BOOL bRes;
        bRes = SetEvent(wakeModifiedWriterHandle);

        if (bRes != TRUE) {
            PRINT_ERROR("[trimPTE] failed to set event\n");
        }

        ResetEvent(wakeModifiedWriterHandle);

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
protectVA(PVOID startVA, PTEpermissions newRWEpermissions, ULONG_PTR commitSize) 
{

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

        writePTE(currPTE, tempPTE);
        
                
    }
    
    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    return TRUE;

}


BOOLEAN
decommitVA (PVOID startVA, ULONG_PTR commitSize) 
{

    PPTE startPTE;
    PVOID endVA;
    PPTE endPTE;
    PPTE currPTE;
    PVOID currVA;
    BOOLEAN lockHeld;
    BOOLEAN reprocessPTE;
    ULONG_PTR numPages;
    PVADNode currVAD;
    PPTE VADStartPTE;
    
    startPTE = getPTE(startVA);

    reprocessPTE = FALSE;

    if (startPTE == NULL) {

        PRINT("[commitVA] Starting address does not correspond to a valid PTE\n");
        return FALSE;

    }

    //
    // Calculate endVA inclusively, where endVA converts to the final PTE that is 
    // decommitted (Thus, use <= as condition). This is also consistent with calculation
    // in commitVA and VAD create/delete
    //

    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);


    endPTE = getPTE(endVA);

    if (endPTE == NULL) {

        PRINT("[commitVA] Ending address does not correspond to a valid PTE\n");
        return FALSE;
        
    }

    //
    // Calculate number of pages spanned by decommit call
    // via PTE difference + 1 (since the same PTE would be one page)
    //

    numPages = endPTE - startPTE + 1;

    //
    // Get a vad from starting address and check whether both
    // start address and end address fit within the singular VAD
    //
    
    EnterCriticalSection(&VADListHead.lock);
 
    currVAD = getVAD(startVA);

    if (currVAD == NULL) {

        LeaveCriticalSection(&VADListHead.lock);
        PRINT("[commitVA] Requested decommit startVA does not fall within a VAD\n");
        return FALSE;

    }

    VADStartPTE = getPTE(currVAD->startVA);

    ULONG_PTR decommitNum;

    decommitNum = currVAD->numPages - (startPTE - VADStartPTE);

    if (decommitNum < numPages) {

        LeaveCriticalSection(&VADListHead.lock);
        PRINT_ALWAYS("[commitVA] Requested commit does not fall within a single VAD\n");
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

        } else {

            reprocessPTE = FALSE;

        }

        ASSERT(lockHeld == TRUE);

        //
        // Make a shallow copy/"snapshot" of the PTE to edit and check
        //

        PTE tempPTE;
        tempPTE = *currPTE;

        //
        // Check if valid/transition/demandzero bit  is already set (avoids double charging if transition)
        //

        if (tempPTE.u1.hPTE.validBit == 1) {                            // valid/hardware format
        

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


            // unmap VA from page
            BOOL bResult;
            bResult = MapUserPhysicalPages(currVA, 1, NULL);


            // TODO - can this happen when 2x VAs are mapped?
            if (bResult != TRUE) {

                PRINT_ERROR("[decommitVA] unable to decommit VA %llx - MapUserPhysical call failed\n", (ULONG_PTR) currVA);

            }


            if (currPFN->writeInProgressBit == 1) {

                currPFN->statusBits = AWAITING_FREE;
                
            }
            else {

                //
                // If the PFN contents are also stored in pageFile
                //

                if (currPFN->pageFileOffset != INVALID_BITARRAY_INDEX ) {

                    clearPFBitIndex(currPFN->pageFileOffset);

                    currPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

                }

                //
                // Clear remodified bit (if set) prior to enqueue
                //

                currPFN->remodifiedBit = 0;

                // enqueue Page to free list
                enqueuePage(&freeListHead, currPFN);

            }

            releaseJLock(&currPFN->lockBits);

            tempPTE.u1.ulongPTE = 0;

        } 
        else if (tempPTE.u1.tPTE.transitionBit == 1) {                  // transition format

            // get PFN
            PPFNdata currPFN;
            currPFN = PFNarray + tempPTE.u1.tPTE.PFN;

            //
            // Must acquire page lock to verify PTE has not changed state
            //

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

                //
                // Dequeue page from standby/modified list
                //

                dequeueSpecificPage(currPFN);

                //
                // If the PFN contents are also stored in pageFile, clear PF space
                // and pagefile offset field in PFN
                //

                if (currPFN->pageFileOffset != INVALID_BITARRAY_INDEX ) {

                    clearPFBitIndex(currPFN->pageFileOffset);

                    currPFN->pageFileOffset = INVALID_BITARRAY_INDEX;
                    
                }

                //
                // Enqueue page to free list (setting status bits in process)
                //

                enqueuePage(&freeListHead, currPFN);

            }      


            tempPTE.u1.ulongPTE = 0;

            //
            // If VAD is commit, set decommitBit in PTE
            //
            
            if (currVAD->commitBit) {

                tempPTE.u1.dzPTE.decommitBit = 1;

            }

            //
            // PTE must be written prior to release of page lock
            //

            writePTE(currPTE, tempPTE);

            releaseJLock(&currPFN->lockBits);

            
            //
            // Decrement committed pages regardless of whether VAD is 
            // commit or reserve
            //

            ASSERT(totalCommittedPages > 0);

            InterlockedDecrement64(&totalCommittedPages);

            continue;


        }  
        else if (tempPTE.u1.pfPTE.pageFileIndex != INVALID_BITARRAY_INDEX
                && tempPTE.u1.pfPTE.permissions != NO_ACCESS) {         // pagefile format

            clearPFBitIndex(tempPTE.u1.pfPTE.pageFileIndex);
            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.dzPTE.permissions != NO_ACCESS) {           // demand zero format

            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.dzPTE.decommitBit == 1) {

            PRINT("[decommitVA] PTE has already been decommitted\n");

            continue;

        }
        else if (tempPTE.u1.ulongPTE == 0 && currVAD->commitBit == 0) { // zero PTE
            
            PRINT("[decommitVA] PTE has already been decommitted\n");

            continue;

        }


        //
        // Decrement committed pages regardless of whether VAD is 
        // commit or reserve
        //

        ASSERT(totalCommittedPages > 0);

        InterlockedDecrement64(&totalCommittedPages);

        //
        // If VAD is commit, set decommitBit in PTE
        //
        
        if (currVAD->commitBit) {

            tempPTE.u1.dzPTE.decommitBit = 1;

        }


        //
        // Write out PTE 
        //

        writePTE(currPTE, tempPTE);
   
    }

    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    LeaveCriticalSection(&VADListHead.lock);

    return TRUE;

}


BOOLEAN
commitPages (ULONG_PTR numPages)
{

    ULONG64 oldVal;
    ULONG64 tempVal;

    oldVal = totalCommittedPages;

    while (TRUE) {

        ASSERT( oldVal <= totalMemoryPageLimit);

        
        //
        // Check for oldVAL + numpages wrapping
        //

        if (oldVal + numPages < oldVal) {

            PRINT_ERROR("WRAPPING\n");
            return FALSE;

        }

                
        //
        // Checks whether proposed page commit is greater than the limit
        //

        if (oldVal + numPages > totalMemoryPageLimit) {

            //
            // no remaining pages
            //

            PRINT("[commitVA] All commit charge used (no remaining pages) - unable to commit PTE\n");
            return FALSE;

        }

        tempVal = InterlockedCompareExchange64(&totalCommittedPages, oldVal + numPages, oldVal);
        
        //
        // Compare exchange successful
        //

        if (tempVal == oldVal) {

            ASSERT(totalCommittedPages <= totalMemoryPageLimit);

            return TRUE;

        } else {

            oldVal = tempVal;

        }
    }
}


ULONG_PTR
checkDecommitted(BOOLEAN isVADCommit, PPTE startPTE, PPTE endPTE)
{

    PPTE currPTE;
    ULONG_PTR numDecommitted;
    BOOLEAN lockHeld;

    numDecommitted = 0;

    lockHeld = TRUE;

    acquirePTELock(startPTE);

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
        // For a commit VAD, check if PTE is explicitly decommitted
        //

        if (isVADCommit && 
            (tempPTE.u1.hPTE.validBit == 0 
            && tempPTE.u1.tPTE.transitionBit == 0 
            && tempPTE.u1.dzPTE.decommitBit) ) {

            numDecommitted++;

        }

        //
        // For a reserve VAD, check if PTE is decommitted
        // (i.e. if valid/transition/demandzero bit is already clear)
        // todo - does this change if the vad permissions around PTE change?
        //

        else if (isVADCommit == FALSE
            && tempPTE.u1.hPTE.validBit == 0 
            && tempPTE.u1.tPTE.transitionBit == 0
            && tempPTE.u1.pfPTE.permissions == NO_ACCESS
            ) {
            
            ASSERT(tempPTE.u1.dzPTE.decommitBit == 0);
        
            numDecommitted++;

        }
                            
    }

    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    return numDecommitted;


}