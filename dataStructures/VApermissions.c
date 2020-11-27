#include "../usermodeMemoryManager.h"
#include "../coreFunctions/pageFault.h"
#include "../coreFunctions/pageFile.h"
#include "../infrastructure/enqueue-dequeue.h"
#include "../infrastructure/jLock.h"
#include "VApermissions.h"
#include "PTEpermissions.h"
#include "VADNodes.h"


faultStatus 
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{

    faultStatus PFstatus;

    //
    // Initialize PFstatus to success - only changes on pagefault return val
    //

    PFstatus = SUCCESS;

    while (PFstatus == SUCCESS ) {

        #ifdef AV_TEMP_TESTING

            //
            // Workaround for special pool/application verifier
            //

            PPTE currPTE;
            PTE snapPTE;

            currPTE = getPTE(virtualAddress);

            snapPTE = *currPTE;

            if (snapPTE.u1.hPTE.validBit == 1) {

                PPFNdata currPFN;
                ULONG_PTR currPFNindex;

                currPFNindex = snapPTE.u1.hPTE.PFN;

                currPFN = PFNarray + currPFNindex;

                acquireJLock(&currPFN->lockBits);

                if (snapPTE.u1.ulongPTE == currPTE->u1.ulongPTE) {

                    PTEpermissions tempRWEpermissions;
                    
                    tempRWEpermissions = getPTEpermissions(snapPTE);

                    if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

                        releaseJLock(&currPFN->lockBits);

                        PFstatus = pageFault(virtualAddress, RWEpermissions);

                        PRINT("Invalid permissions\n");

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

    while (PFstatus == SUCCESS ) {

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
    ULONG_PTR commitPagesToReturn;

    //
    // Initialize count of pages to return count to zero
    // and reprocess PTE flag to false
    //

    commitPagesToReturn = 0;

    reprocessPTE = FALSE;

    //
    // If commitsize is zero, return immediately - nothing to be committed
    //

    if (commitSize == 0) {

        PRINT("[commitVA] commit size of zero\n");
        return FALSE;

    }

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

    if (currVAD == NULL || currVAD->deleteBit == 1) {

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

    #ifdef VAD_COMMIT_CHECK
        
        //
        // VAD read lock must remain held at this point so that 
        // currVAD can be read accurately by callee commit
        // checks
        //
    
        checkVADCommit(currVAD);

    #endif

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

            //
            // VAD "read" lock is held here so currVAD can be passed and read 
            // accurately as a parameter to checkDecommitted
            //

            currDecommitted = checkDecommitted(currVAD, startPTE, endPTE);

            bRes = commitPages(currDecommitted);

            if (bRes == FALSE) {

                LeaveCriticalSection(&VADListHead.lock);

                PRINT("[commitVA] Insufficient commit charge\n");

                return FALSE;

            }

            EnterCriticalSection(&VADWriteLock);

            currVAD->commitCount += currDecommitted;
            
            //
            // Assert that there are not more pages committed in the VAD
            // than actual VM pages (would be a bookkeeping error)
            //

            ASSERT(currVAD->commitCount <= currVAD->numPages);

            LeaveCriticalSection(&VADWriteLock);

            //
            // Set return COmmit Pages flag to false (since only enough pages
            // to commit currently decommitted pages have been committed)
            //

            returnCommitPages = FALSE;

        } 
        else {

            //
            // If "upfront" commit is successfully allocated (and 
            // totalcommittedpages incremented as such), acquire
            // VAD lock and and update VAD commit count accordingly.
            // The returnCommitPages flag must also be set to true in
            // order to return commit for previously committed pages
            //

            EnterCriticalSection(&VADWriteLock);

            currVAD->commitCount += numPages;

            //
            // Cannot assert that there are not more pages committed in the VAD
            // than actual VM pages, since there could be more committed due to
            // the forthcoming return of committed pages
            //

            LeaveCriticalSection(&VADWriteLock);

            returnCommitPages = TRUE;

        }

    }
    else {

        //
        // If the VAD is commit, find exact charge of decommitted pages
        // and only commit those
        //

        ULONG_PTR currDecommitted;

        //
        // VAD "read" lock remains held here. CurrVAD can be passed and read 
        // accurately as a parameter to checkDecommitted
        //

        currDecommitted = checkDecommitted(currVAD, startPTE, endPTE);

        bRes = commitPages(currDecommitted);

        if (bRes == FALSE) {

            LeaveCriticalSection(&VADListHead.lock);

            PRINT("[commitVA] Insufficient commit charge\n");
            
            return FALSE;
            
        }

        //
        // Acquire VAD lock in order to increment commit count
        //

        EnterCriticalSection(&VADWriteLock);

        currVAD->commitCount += currDecommitted;

        //
        // Assert that there are not more pages committed in the VAD
        // than actual VM pages (would be a bookkeeping error)
        //

        ASSERT(currVAD->commitCount <= currVAD->numPages);

        LeaveCriticalSection(&VADWriteLock);

        //
        // Since VAD is mem commit, set return commit pages flag to false
        //

        returnCommitPages = FALSE;

    }

    //
    // Acquire starting PTE lock and set lockHeld boolean flag to true
    //

    acquirePTELock(startPTE);

    lockHeld = TRUE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        PTE tempPTE;

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
        // make a shallow copy/"snapshot" of the PTE to edit and check
        //

        tempPTE = *currPTE;

        //
        // Check whether PTE has previously been committed
        // (i.e. whether valid/transition/demandzero bit is already set 
        // OR if the PTE is zero but the VAD is committed
        // (avoids double charging commit). Delete bit cannot be set in
        // VAD since it was validated earlier in the function
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

                //
                // Verify the PTE is still in transition format and pointed to by PTE index
                //

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
                // does not "fall through" like other PTE statements).
                //

                if (returnCommitPages) {
                    
                    ASSERT(currVAD->commitBit == 0);

                    commitPagesToReturn++;

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

            writePTE(currPTE, tempPTE);

            
            //
            // PTE has previously been committed, VAD is reserve,
            // AND returnCommittedPages is TRUE, return charge
            //

            if (returnCommitPages) {
                
                ASSERT(currVAD->commitBit == 0);

                commitPagesToReturn++;

            }
            
            PRINT("[commitVA] PTE is already valid, transition, or demand zero\n");

            continue;

        }

        //
        // If VAD is commit, verify the decommit bit is set in the PTE
        // since it must have been set to hit this path
        //

        if (currVAD->commitBit && currVAD->deleteBit == 0) {

            ASSERT( tempPTE.u1.dzPTE.decommitBit == 1 );

        }

        //
        // Commit PTE/page with priviliges param, clearing decommit bit and setting pfIndex to invalid value
        //

        tempPTE.u1.dzPTE.permissions = RWEpermissions;

        tempPTE.u1.dzPTE.decommitBit = 0;

        tempPTE.u1.dzPTE.pageFileIndex = INVALID_BITARRAY_INDEX;

        //
        // Calculate current VA
        //

        currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );

        PRINT("[commitVA] Committed PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, RWEpermissions);

        //
        // Write out newPTE contents indivisibly
        //

        writePTE(currPTE, tempPTE);        
                            
    }

    //
    // Check returnCommitPages flag to see whether accumulated commit must be
    // returned
    //

    if (returnCommitPages) {

        ASSERT(currVAD->commitBit == 0);

        decrementMultipleCommit(currVAD, commitPagesToReturn);

    }

    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    LeaveCriticalSection(&VADListHead.lock);

    return TRUE;

}


BOOLEAN
trimVA(void* virtualAddress)
{

    PPTE PTEaddress;

    PRINT("[trimVA] trimming page with VA %llu\n", (ULONG_PTR) virtualAddress);

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

    //
    // Calculate an inclusive endVA (use <= as condition)
    //

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

        PRINT("[decommitVA] Requested decommit does not fall within a single VAD\n");

        return FALSE;

    }

    //
    // Acquire starting PTE lock and set lockHeld boolean flag to true
    //

    acquirePTELock(startPTE);

    lockHeld = TRUE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        PTE tempPTE;

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

        tempPTE = *currPTE;

        //
        // Check if valid/transition/demandzero bit us set (meaning PTE is committed)
        //

        if (tempPTE.u1.hPTE.validBit == 1) {                            // valid/hardware format
        
            PPFNdata currPFN;
            BOOL bResult;

            //
            // Get PFN metadata from PTE 
            //

            currPFN = PFNarray + tempPTE.u1.hPTE.PFN;

            //
            // acquire PFN lock
            //
            
            acquireJLock(&currPFN->lockBits);

            #ifdef TESTING_VERIFY_ADDRESSES

                //
                // Single volatile snapshot of current value at currVA
                //

                volatile ULONG_PTR compareVal;

                compareVal = * (ULONG_PTR*) currVA;

                if ((ULONG_PTR) currVA != compareVal) {

                    if (compareVal == (ULONG_PTR)0) {

                        //
                        // This may occur if a thread attempts to decommit an address WHILE
                        // another thread is ACTIVELY WRITING IT (between pagefault and actual 
                        // writing out in writeVA)
                        //

                        PRINT("Another thread is currently between pagefault and write in writeVA function\n");

                    } else {

                        PRINT_ERROR("decommitting (VA = 0x%llx) with contents 0x%llx\n", (ULONG_PTR) currVA, compareVal);

                    }    
                    
                }
                
            #endif

            //
            // Unmap VA from page
            //

            bResult = MapUserPhysicalPages(currVA, 1, NULL);


            if (bResult != TRUE) {

                PRINT_ERROR("[decommitVA] unable to decommit VA %llx - MapUserPhysical call failed\n", (ULONG_PTR) currVA);

            }

            if (currPFN->writeInProgressBit == 1 || currPFN->refCount != 0) {

                currPFN->statusBits = AWAITING_FREE;
                
            }
            else {

                ASSERT(currPFN->readInProgressBit == 0);

                //
                // If the PFN contents are also stored in pageFile
                //

                if (currPFN->pageFileOffset != INVALID_BITARRAY_INDEX) {

                    clearPFBitIndex(currPFN->pageFileOffset);

                    currPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

                }

                //
                // Clear remodified bit (if set) prior to enqueue
                //

                currPFN->remodifiedBit = 0;

                //
                // Enqueue page to free list
                //

                enqueuePage(&freeListHead, currPFN);

            }

            releaseJLock(&currPFN->lockBits);

            tempPTE.u1.ulongPTE = 0;

        } 
        else if (tempPTE.u1.tPTE.transitionBit == 1) {                  // transition format

            PPFNdata currPFN;

            //
            // Get PFN metadata
            //

            currPFN = PFNarray + tempPTE.u1.tPTE.PFN;

            //
            // Must acquire page lock to verify PTE has not changed state
            //

            acquireJLock(&currPFN->lockBits);

            //
            // Verify PTE is still in transition format and pointed to by PTE index
            //
            
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
            // Dequeue PFN only if both write in progress bit is clear AND refCount is 
            // zero (as a proxy for read in progress)
            //

            if (currPFN->writeInProgressBit == 1 || currPFN->refCount != 0) {

                currPFN->statusBits = AWAITING_FREE;

            }
            else {

                ASSERT(currPFN->readInProgressBit == 0);

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
            // If VAD is commit, set decommitBit in PTE and decrement the
            // VAD's currently committed count
            //
            
            if (currVAD->commitBit) {

                //
                // If VAD is not being deleted, set decommit bit and invalid 
                // bitarray index
                //

                if (currVAD->deleteBit == 0) {

                    tempPTE.u1.dzPTE.decommitBit = 1;

                    tempPTE.u1.dzPTE.pageFileIndex = INVALID_BITARRAY_INDEX;

                }

                //
                // If VAD deleteBit is set (for a commit VAD), leave decommitbit and 
                // pagefile index to zero (zeroing whole PTE)
                // 

            } 

            //
            // PTE must be written prior to release of page lock
            //

            writePTE(currPTE, tempPTE);

            releaseJLock(&currPFN->lockBits);

            decrementCommit(currVAD);

            continue;


        }  
        else if (tempPTE.u1.pfPTE.pageFileIndex != INVALID_BITARRAY_INDEX
                && tempPTE.u1.pfPTE.permissions != NO_ACCESS) {

            //
            // PTE is in pagefile format - clear pagefile space and zero PTE
            //

            clearPFBitIndex(tempPTE.u1.pfPTE.pageFileIndex);

            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.dzPTE.permissions != NO_ACCESS) {           
            
            //
            //  PTE is in dz format - simply zero PTE
            //

            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.dzPTE.decommitBit == 1) {

            //
            // Decommit bit should only ever be set in a commit VAD
            // (reserve VAD should only clear/zero PTE)
            //

            ASSERT(currVAD->commitBit);

            if (currVAD->deleteBit == 1) {

                //
                // If a commit VAD is currently being deleted, clear the whole 
                // PTE for next use (even though a clear PTE would typically
                // be "committed" in a commit VAD)
                //

                tempPTE.u1.dzPTE.decommitBit = 0;

                tempPTE.u1.dzPTE.pageFileIndex = 0;

                ASSERT(tempPTE.u1.ulongPTE == 0);

                writePTE(currPTE, tempPTE);

            }

            PRINT("[decommitVA] PTE has already been decommitted\n");

            continue;

        }
        else if (tempPTE.u1.ulongPTE == 0 
                && (currVAD->commitBit == 0 
                || (currVAD->commitBit && currVAD->deleteBit) 
                ) ) {                               

            //
            // If PTE is zero AND the VAD is mem reserve OR
            // the VAD is mem commit AND the deleteBit is set
            //
            
            PRINT("[decommitVA] PTE has already been decommitted\n");

            continue;

        } 
        else if (tempPTE.u1.ulongPTE != 0) {
            
            PRINT_ERROR("[decommitVA] unrecognized state\n");

            continue;

        } else if (!(tempPTE.u1.ulongPTE == 0 && currVAD->commitBit) ) {

            //
            // only remainingcase w validity is zero PTE with a commit VAD is committed
            //

            PRINT_ERROR("[decommitVA] unrecognized state\n");
        }

        //
        // If VAD is commit, either set the decommit bit (if VAD deletebit is clear)
        // OR clear the whole PTE
        //
        
        if (currVAD->commitBit) {

            //
            // If the VAD has not yet been deleted, also set the decommitBit
            // and invalid bitarray index in PTE
            //

            if (currVAD->deleteBit == 0) {

                tempPTE.u1.dzPTE.decommitBit = 1;

                tempPTE.u1.dzPTE.pageFileIndex = INVALID_BITARRAY_INDEX;

            } else {

                tempPTE.u1.dzPTE.decommitBit = 0;

                tempPTE.u1.dzPTE.pageFileIndex = 0;

                ASSERT(tempPTE.u1.ulongPTE == 0);

            }

        }

        //
        // Write out PTE 
        //

        writePTE(currPTE, tempPTE);

        decrementCommit(currVAD);

    }

    //
    // All PTE's have been updated - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    //
    // VAD "read" lock can now also be released
    //

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


BOOLEAN
decommitPages (ULONG_PTR numPages)
{

    ULONG64 oldVal;
    ULONG64 tempVal;

    oldVal = totalCommittedPages;

    while (TRUE) {

        ASSERT( oldVal <= totalMemoryPageLimit);

        
        //
        // Check for oldVAL + numpages wrapping
        //

        if (oldVal - numPages > oldVal) {

            PRINT_ERROR("WRAPPING\n");
            return FALSE;

        }

                
        //
        // Assert that page counts have been kept correctly
        //

        ASSERT (oldVal - numPages >= 0);

        tempVal = InterlockedCompareExchange64(&totalCommittedPages, oldVal - numPages, oldVal);
        
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
checkDecommitted(PVADNode currVAD, PPTE startPTE, PPTE endPTE)
{

    PPTE currPTE;
    ULONG_PTR numDecommitted;
    BOOLEAN lockHeld;

    numDecommitted = 0;

    lockHeld = TRUE;

    acquirePTELock(startPTE);

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        PTE tempPTE;

        //
        // Only acquire lock if the currentPTE is the first PTE OR
        // if current PTE's lock differs from the previous PTE's lock 
        //

        if (currPTE != startPTE) {

            lockHeld = acquireOrHoldSubsequentPTELock(currPTE, currPTE - 1);

        }
 
        if (lockHeld == FALSE) {

            PRINT_ERROR("[commitVA] unable to acquire lock - fatal error\n");
            return 0;

        }
        
        //
        // make a shallow copy/"snapshot" of the PTE to edit and check
        //

        tempPTE = *currPTE;

        //
        // For a commit VAD, check if PTE is explicitly decommitted
        // via the decommitBit (and assert that other fields remain
        // as they should)
        //

        if (currVAD->commitBit && 
            (tempPTE.u1.hPTE.validBit == 0 
                && tempPTE.u1.tPTE.transitionBit == 0 
                && (tempPTE.u1.dzPTE.decommitBit 
                    || ( currVAD->deleteBit && tempPTE.u1.ulongPTE == 0 )
            ) ) ) {

            numDecommitted++;

        }

        //
        // For a reserve VAD, check if PTE is decommitted
        // (i.e. if valid/transition/demandzero bit is already clear
        // and that decommit bit is not set
        //

        else if (currVAD->commitBit == 0
            && tempPTE.u1.hPTE.validBit == 0 
            && tempPTE.u1.tPTE.transitionBit == 0
            && tempPTE.u1.pfPTE.permissions == NO_ACCESS
            ) {
            
            ASSERT(tempPTE.u1.dzPTE.decommitBit == 0);
        
            numDecommitted++;

        }
                            
    }

    //
    // All PTE's have been checked - final PTE lock can be safely released
    //

    releasePTELock(endPTE);

    return numDecommitted;


}